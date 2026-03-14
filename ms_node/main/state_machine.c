#include "state_machine.h"
#include "ble_manager.h"
#include "config.h"
#include "election.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_now_manager.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "led_manager.h"
#include "metrics.h"
#include "neighbor_manager.h"
#include "rf_receiver.h"
#include "storage_manager.h"
#include "compression.h"
#include "uav_client.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h> // For qsort
#include <string.h>

static int compare_priority(const void *a, const void *b) {
  const neighbor_entry_t *na = (const neighbor_entry_t *)a;
  const neighbor_entry_t *nb = (const neighbor_entry_t *)b;
  
  float score_a =
      (na->link_quality * 100.0f) + (100.0f - (na->battery * 100.0f));
  float score_b =
      (nb->link_quality * 100.0f) + (100.0f - (nb->battery * 100.0f));

  if (score_b > score_a)
    return 1;
  if (score_b < score_a)
    return -1;
  return 0;
}

uint32_t state_machine_get_sleep_time_ms(void) {
  // Only smart sleep in MEMBER state with a valid schedule
  if (g_current_state == STATE_MEMBER) {
    schedule_msg_t sched = esp_now_get_current_schedule();
    int64_t now_us = esp_timer_get_time();

    if (sched.magic == ESP_NOW_MAGIC_SCHEDULE && sched.epoch_us > (now_us - (SLOT_DURATION_SEC * 10000000LL))) {
      int64_t my_slot_start = sched.epoch_us + (sched.slot_index * sched.slot_duration_sec * 1000000LL);
      int64_t time_to_slot = my_slot_start - now_us;

      if (time_to_slot > 0) {
        // We are early. Sleep until slot.
        return (uint32_t)(time_to_slot / 1000);
      } else {
        // We are IN the slot or LATE.
        // If we are strictly in the slot, we shouldn't sleep long.
        // If we missed it, sleep default.
        int64_t time_in_slot = -time_to_slot;
        int64_t slot_len_us = sched.slot_duration_sec * 1000000LL;

        if (time_in_slot < slot_len_us) {
          // Currently IN slot. Don't sleep! Run loop immediately.
          return 100;
        }
      }
    }
  }
  return 5000; // Default sleep
}

static const char *TAG = "STATE";

node_state_t g_current_state = STATE_INIT;
bool g_is_ch = false;
uint32_t g_node_id = 0;
uint64_t g_mac_addr = 0;

// Forward declarations
extern uint8_t g_cluster_key[CLUSTER_KEY_SIZE];

static uint64_t state_entry_time = 0;

// File-scope flag: tracks whether BLE advertising was started in MEMBER state.
// Must be file-scope (not static local) so it resets correctly on every
// MEMBER re-entry via transition_to_state().
static bool member_ble_started = false;

// ---------------------------------------------------------------------------
// STELLAR / TDMA Superframe Phase Control
// ---------------------------------------------------------------------------

typedef enum {
  PHASE_STELLAR = 0,
  PHASE_DATA = 1,
} phase_t;

static phase_t s_current_phase = PHASE_STELLAR;
static uint64_t s_phase_start_ms = 0;
static uint64_t s_last_schedule_send_ms = 0;   // Timestamp of last TDMA schedule send
static int      s_schedule_sends_this_phase = 0; // Count of schedule sends this DATA phase
// s_last_ch_store_ms and s_last_mbr_store_ms removed — storage moved to main loop (ms_node.c)
static bool ch_assertion_verified = false;
static uint64_t ch_assertion_start = 0;
const uint32_t CH_ASSERTION_GRACE_MS = 3000;
static bool s_pre_guard_active = false;
// Bug 6 fix: Moved to file scope and reset in transition_to_state()
static int ch_miss_count = 0;
static phase_t last_logged_phase = PHASE_STELLAR;


static void state_machine_update_phase(uint64_t now_ms) {
  // Initialize phase start on first run
  if (s_phase_start_ms == 0) {
    s_phase_start_ms = now_ms;
  }

  uint64_t elapsed = now_ms - s_phase_start_ms;
  const uint64_t frame_len = STELLAR_PHASE_MS + DATA_PHASE_MS;

  // Start a new superframe if we ran past the end
  if (elapsed >= frame_len) {
    s_phase_start_ms = now_ms;
    elapsed = 0;
  }

  if (elapsed < STELLAR_PHASE_MS) {
    s_current_phase = PHASE_STELLAR;
  } else {
    s_current_phase = PHASE_DATA;
  }
}

/**
 * state_machine_sync_phase_from_epoch()
 *
 * Called when a TDMA schedule packet arrives from the CH.
 *
 * The CH sets epoch_us = esp_timer_get_time() + PHASE_GUARD_MS*1000 at DATA
 * phase entry. epoch_us is boot-relative to the CH — it CANNOT be subtracted
 * from the member's timer (different boot times).
 *
 * Instead use the semantic fact: the CH just entered DATA phase, so the
 * member should also be at the DATA phase boundary right now. That means:
 *   elapsed_in_superframe = STELLAR_PHASE_MS
 *   s_phase_start_ms = member_now_ms - STELLAR_PHASE_MS
 *
 * This makes state_machine_update_phase() immediately transition to PHASE_DATA
 * on the next call, perfectly synchronised with the CH.
 */
void state_machine_sync_phase_from_epoch(int64_t epoch_us) {
  (void)epoch_us; // Value is CH-boot-relative; we only use the receipt event.

  uint64_t member_now_ms = (uint64_t)(esp_timer_get_time() / 1000LL);
  // Set elapsed to exactly STELLAR_PHASE_MS so state_machine_update_phase()
  // evaluates to PHASE_DATA immediately.
  uint64_t new_phase_start_ms = member_now_ms - (uint64_t)STELLAR_PHASE_MS;

  int64_t diff_ms = (int64_t)new_phase_start_ms - (int64_t)s_phase_start_ms;
  // Apply only when the adjustment is significant (>500ms) to avoid repeated
  // tiny re-syncs that could disrupt an in-progress send.
  if (diff_ms < -500 || diff_ms > 500) {
    ESP_LOGI("SM_SYNC",
             "Phase sync: aligning to CH DATA entry "
             "(elapsed=%u ms -> PHASE_DATA, diff=%lld ms)",
             (unsigned)STELLAR_PHASE_MS, (long long)diff_ms);
    s_phase_start_ms = new_phase_start_ms;
  }
}

const char *state_machine_get_state_name(void) {
  switch (g_current_state) {
  case STATE_INIT:
    return "INIT";
  case STATE_DISCOVER:
    return "DISCOVER";
  case STATE_CANDIDATE:
    return "CANDIDATE";
  case STATE_CH:
    return "CH";
  case STATE_MEMBER:
    return "MEMBER";
  case STATE_UAV_ONBOARDING:
    return "UAV_ONBOARDING";
  case STATE_SLEEP:
    return "SLEEP";
  default:
    return "UNKNOWN";
  }
}

static void transition_to_state(node_state_t new_state) {
  if (g_current_state == new_state) {
    return;
  }

  ESP_LOGI(TAG, "State transition: %s -> %s", state_machine_get_state_name(),
           (new_state == STATE_INIT             ? "INIT"
            : new_state == STATE_DISCOVER       ? "DISCOVER"
            : new_state == STATE_CANDIDATE      ? "CANDIDATE"
            : new_state == STATE_CH             ? "CH"
            : new_state == STATE_MEMBER         ? "MEMBER"
            : new_state == STATE_UAV_ONBOARDING ? "UAV_ONBOARDING"
                                                : "SLEEP"));

  // Ensure global flag matches state
  if (new_state == STATE_CH) {
    g_is_ch = true;
    ch_assertion_verified = false; // Reset CH assertion verification on every CH entry
    ch_assertion_start = esp_timer_get_time() / 1000; // Start grace period timer for CH assertion verification
  } else {
    g_is_ch = false;
  }

  // Bug 2 fix: Reset MEMBER-specific state ONLY when entering MEMBER
  if (new_state == STATE_MEMBER) {
    member_ble_started = false;
    s_pre_guard_active = false; // Reset pre-guard on MEMBER entry
    ch_miss_count = 0;           // Bug 6 fix: Reset miss counter on MEMBER entry
  }

  g_current_state = new_state;
  led_manager_set_state(new_state);
  state_entry_time = esp_timer_get_time() / 1000;
}

void state_machine_init(void) {
  // Get MAC address for node ID
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  g_mac_addr = ((uint64_t)mac[0] << 40) | ((uint64_t)mac[1] << 32) |
               ((uint64_t)mac[2] << 24) | ((uint64_t)mac[3] << 16) |
               ((uint64_t)mac[4] << 8) | mac[5];

  // Use lower 32 bits as node ID
  g_node_id = (uint32_t)(g_mac_addr & 0xFFFFFFFF);

  ESP_LOGI(TAG, "State machine initialized: node_id=%lu, MAC=%llx", g_node_id,
           g_mac_addr);

  transition_to_state(STATE_INIT);
}


void state_machine_run(void) {
  uint64_t now_ms = esp_timer_get_time() / 1000;

  // Update superframe phase (STELLAR vs DATA) and reset TDMA schedule flag
  phase_t prev_phase = s_current_phase;
  state_machine_update_phase(now_ms);
  if (s_current_phase != prev_phase) {

    if (s_current_phase == PHASE_DATA) {
      // New DATA phase: Stop BLE to clear radio for ESP-NOW
      // CRITICAL FIX: Only established nodes (Member/CH) respect the quiet
      // phase. Candidates and Discovery nodes MUST keep scanning/advertising to
      // find the cluster.

      // Stop advertising for ALL roles during DATA phase.
      // CH advertising was previously kept on for "new node visibility" but
      // it occupies the 2.4 GHz radio, interfering with ESP-NOW schedule
      // delivery (status=1 failures).  New nodes discover the CH during
      // STELLAR phase; DATA phase is exclusively for ESP-NOW.
      if (g_current_state == STATE_MEMBER || g_current_state == STATE_CH) {
        ble_manager_stop_advertising();
      }

      // CRITICAL FIX: Ensure radio is on ESP-NOW channel (1) after BLE hopping
      esp_wifi_set_channel(ESP_NOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

      // Allow TDMA schedule broadcasts in this DATA phase
      s_schedule_sends_this_phase = 0;
      s_last_schedule_send_ms = 0;
    } else {
      // New STELLAR phase: Restart BLE
      ble_manager_start_advertising();
      ble_manager_start_scanning();

      // Bug 4 fix: Remove broadcast peer at end of DATA phase
      // Prevents peer table from filling up over time
      static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
      if (esp_now_is_peer_exist(BCAST)) {
        esp_now_del_peer(BCAST);
        ESP_LOGD(TAG, "BLE PHASEEntry: Removed broadcast peer from ESP-NOW table");
      }
    }

    // ── Diagnostic: Phase boundary with radio state (AFTER transition) ──
    bool ble_scan   = ble_manager_is_scanning();
    bool ble_adv    = ble_manager_is_advertising();
    int  mslg_count = storage_manager_get_mslg_chunk_count();
    const char *role = (g_current_state == STATE_CH) ? "CH" :
                       (g_current_state == STATE_MEMBER) ? "MEMBER" : "OTHER";

    // ESP-NOW is always initialised but only usable when the radio is NOT
    // in BLE mode.  During STELLAR the radio is on BLE → ESP-NOW is idle.
    const bool espnow_active = (s_current_phase == PHASE_DATA);

    if (s_current_phase == PHASE_DATA) {
      ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════╗");
      ESP_LOGI(TAG, "║  STELLAR phase END  →  DATA phase START          ║");
      ESP_LOGI(TAG, "╠═══════════════════════════════════════════════════╣");
      ESP_LOGI(TAG, "║  Role: %-6s                                    ║", role);
      ESP_LOGI(TAG, "║  BLE  RX (scan): %-8s  TX (adv): %-8s    ║",
               ble_scan ? "ACTIVE" : "INACTIVE",
               ble_adv  ? "ACTIVE" : "INACTIVE");
      ESP_LOGI(TAG, "║  ESP-NOW RX: %-8s       TX: %-8s          ║",
               espnow_active ? "ACTIVE" : "INACTIVE",
               espnow_active ? "ACTIVE" : "INACTIVE");
      ESP_LOGI(TAG, "║  MSLG chunks stored: %-5d                       ║", mslg_count);
      ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════╝");
    } else {
      ESP_LOGI(TAG, "╔═══════════════════════════════════════════════════╗");
      ESP_LOGI(TAG, "║  DATA phase END  →  STELLAR phase START          ║");
      ESP_LOGI(TAG, "╠═══════════════════════════════════════════════════╣");
      ESP_LOGI(TAG, "║  Role: %-6s                                    ║", role);
      ESP_LOGI(TAG, "║  BLE  RX (scan): %-8s  TX (adv): %-8s    ║",
               ble_scan ? "ACTIVE" : "INACTIVE",
               ble_adv  ? "ACTIVE" : "INACTIVE");
      ESP_LOGI(TAG, "║  ESP-NOW RX: %-8s       TX: %-8s          ║",
               espnow_active ? "ACTIVE" : "INACTIVE",
               espnow_active ? "ACTIVE" : "INACTIVE");
      ESP_LOGI(TAG, "║  MSLG chunks stored: %-5d                       ║", mslg_count);
      ESP_LOGI(TAG, "╚═══════════════════════════════════════════════════╝");
    }
  }

  switch (g_current_state) {
  case STATE_INIT:
    // Boot & self-init
    ESP_LOGI(TAG, "Boot & self-init");

    // Transition to DISCOVER after a short delay (bypass BLE ready check)
    if (now_ms - state_entry_time > 2000) { // Wait 2 seconds for initialization
      transition_to_state(STATE_DISCOVER);
    }
    break;

  case STATE_DISCOVER:
    // Neighbor discovery phase
    if (now_ms - state_entry_time < 5000) { // 5 second discovery
      if (!ble_manager_is_ready()) {
        break;
      }

      // Start both advertising and scanning so nodes can discover each other
      ble_manager_start_advertising();
      ble_manager_start_scanning();

      // Update metrics and advertisement periodically
      static uint64_t last_update = 0;
      if (now_ms - last_update >= 1000) { // Update every second
        // metrics_update(); // Handled by metrics_task
        ble_manager_update_advertisement();
        last_update = now_ms;

        // Check if there's already an existing CH (check continuously, not just
        // at end)
        uint32_t existing_ch = neighbor_manager_get_current_ch();
        if (existing_ch != 0 && (now_ms - state_entry_time) >= 2000) {
          // Found CH after at least 2 seconds of discovery (give time to find
          // neighbors)
          ESP_LOGI(TAG,
                   "DISCOVER: Found existing CH node_%lu after %llu ms, "
                   "joining as MEMBER",
                   existing_ch,
                   (unsigned long long)(now_ms - state_entry_time));
          // Keep BLE running for beaconing as MEMBER (don't stop)
          // Just stop scanning to save power
          ble_manager_stop_scanning();
          g_is_ch = false;
          transition_to_state(STATE_MEMBER);
          break; // Exit DISCOVER immediately
        }
      }
    } else {
      // Discovery complete (5 seconds elapsed) - check if there's already a CH
      uint32_t existing_ch = neighbor_manager_get_current_ch();
      if (existing_ch != 0) {
        ESP_LOGI(TAG,
                 "DISCOVER: Found existing CH node_%lu at end of window, "
                 "joining as MEMBER",
                 existing_ch);
        // Keep BLE running for beaconing as MEMBER (don't stop)
        // Just stop scanning to save power
        ble_manager_stop_scanning();
        g_is_ch = false;
        transition_to_state(STATE_MEMBER);
      } else {
        // No CH found, move to candidate for election
        // Keep both advertising and scanning for election
        transition_to_state(STATE_CANDIDATE);
        election_reset_window();
      }
    }
    break;

  case STATE_CANDIDATE:
    // Candidate phase - collect scores and run election
    {
      // Start advertising our score
      ble_manager_start_advertising();
      ble_manager_start_scanning();

      // Update metrics and advertisement periodically
      static uint64_t last_candidate_update = 0;
      if (now_ms - last_candidate_update >= 1000) {
        // metrics_update(); // Handled by metrics_task
        ble_manager_update_advertisement();
        last_candidate_update = now_ms;
      }

      // Cleanup stale neighbors
      neighbor_manager_cleanup_stale();

      // Check if election window has expired
      uint64_t window_start = election_get_window_start();
      if (window_start == 0) {
        election_reset_window();
        window_start = election_get_window_start();
      }

      if (now_ms - window_start >= ELECTION_WINDOW_MS) {
        // Run election
        uint32_t winner = election_run();

        if (winner == g_node_id) {
          // We won!
          g_is_ch = true;
          transition_to_state(STATE_CH);
        } else if (winner != 0) {
          // Someone else won
          g_is_ch = false;
          transition_to_state(STATE_MEMBER);
        } else {
          // No valid winner, restart discovery
          ESP_LOGW(TAG, "No valid election winner, restarting discovery");
          transition_to_state(STATE_DISCOVER);
        }
      }
    }
    break;

  case STATE_CH:
    // Cluster Head duties
    {
      // ─────────────────────────────────────────────────────────────────────
      // CH ASSERTION GRACE PERIOD (Dual-CH Prevention)
      // ─────────────────────────────────────────────────────────────────────
      // When first becoming CH, wait a brief period while scanning to detect
      // any conflicting CHs before fully asserting. This prevents dual-CH
      // scenarios where two nodes both think they won the election due to
      // incomplete neighbor information.
      // ─────────────────────────────────────────────────────────────────────
      // Bug 1 fix: Removed redundant reset block here — transition_to_state()
      // now correctly initializes these on CH entry.
      // ─────────────────────────────────────────────────────────────────────
      // During grace period, aggressively scan for conflicting CHs
      // BUT only during STELLAR phase — DATA phase radio belongs to ESP-NOW
      if (s_current_phase == PHASE_STELLAR) {
        ble_manager_start_scanning();
        ble_manager_update_advertisement();
      }

      if (!ch_assertion_verified) {
        if (now_ms - ch_assertion_start < CH_ASSERTION_GRACE_MS) {
          // Still in grace period - check for conflicts
          if (election_check_reelection_needed()) {
            // Another better CH exists - yield immediately
            ESP_LOGW(TAG, "CH conflict detected during assertion grace - yielding");
            ch_assertion_verified = false;
            ch_assertion_start = 0;
            uint32_t other_ch = neighbor_manager_get_current_ch();
            if (other_ch != 0) {
              ESP_LOGI(TAG, "Yielding to existing CH %lu during assertion",
                       other_ch);
              g_is_ch = false;
              transition_to_state(STATE_MEMBER);
            } else {
              g_is_ch = false;
              transition_to_state(STATE_CANDIDATE);
              election_reset_window();
            }
            break;
          }
          // Still checking - don't proceed with CH duties yet
          break;
        } else {
          // Grace period ended with no conflicts
          ch_assertion_verified = true;
          ESP_LOGI(TAG, "CH assertion verified - no conflicts detected");
        }
      }

      // Update metrics
      // metrics_update(); // Handled by metrics_task

      // Keep scanning so we can detect other CH beacons and resolve conflicts
      // (CH state can be reached from DISCOVER where scanning may have been
      // stopped).
      // CRITICAL: Only during STELLAR phase! During DATA phase the radio must
      // stay on the WiFi channel for ESP-NOW.  This was the root cause of
      // ble_scan=1 on ALL CH schedule sends → members couldn't ACK data.
      if (s_current_phase == PHASE_STELLAR) {
        ble_manager_start_scanning();
        ble_manager_update_advertisement();
      }

      // Check if re-election / yielding is needed.
      // During DATA phase we keep the CH fixed *unless* there is a CH conflict
      // (another CH is present) which must be resolved immediately to avoid
      // multiple CHs operating simultaneously.
      if (election_check_reelection_needed()) {
        // Log score delta so we can see WHY re-election was triggered
        node_metrics_t my_m = metrics_get_current();
        // pareto_rank=0, centrality=0.5 (neutral) for diagnostic log only
        float my_psi = metrics_compute_stellar_score(&my_m, 0, 0.5f);
        ESP_LOGI(TAG,
                 "RE-ELECTION CHECK: my_Ψ≈%.4f — a better candidate exists "
                 "(see election logs for winner score)",
                 my_psi);

        // Check if there's already a valid CH we're yielding to
        uint32_t other_ch = neighbor_manager_get_current_ch();
        bool allow_yield =
            (s_current_phase == PHASE_STELLAR) || (other_ch != 0);

        if (!allow_yield) {
          break;
        }

        if (other_ch != 0) {
          ESP_LOGI(TAG, "Yielding to existing CH %lu, becoming MEMBER",
                   other_ch);
          g_is_ch = false;
          transition_to_state(STATE_MEMBER);
        } else {
          ESP_LOGI(TAG, "Re-election triggered, returning to candidate");
          g_is_ch = false;
          transition_to_state(STATE_CANDIDATE);
          election_reset_window();
        }
        break;
      }

      // CH duties: maintain member list, etc.
      neighbor_manager_cleanup_stale();

      // BUG 3 FIX: Query neighbor list ONCE and reuse for both cluster check and scheduler.
      // Previously queried twice; between calls a neighbor could join/leave, causing
      // split-logic and scheduler to operate on different snapshots.
      neighbor_entry_t neighbors[MAX_NEIGHBORS];
      size_t neighbor_count = neighbor_manager_get_all(neighbors, MAX_NEIGHBORS);

      // Check cluster size
      if (neighbor_count > MAX_CLUSTER_SIZE) {
        ESP_LOGW(TAG, "Cluster size exceeded (%zu), triggering split", neighbor_count);
        // In production, implement cluster split logic
      }

      // UAV Trigger Check
      if (rf_receiver_check_trigger()) {
        ESP_LOGI(TAG, "UAV Trigger detected! Transitioning to UAV ONBOARDING");
        transition_to_state(STATE_UAV_ONBOARDING);
        break;
      }

      // ---------------------------------------------------------
      // TIME SLICING SCHEDULER (Novelty) - resends every 5s during DATA
      // ---------------------------------------------------------
      // Phase desync fix: Members may still be in STELLAR (BLE scanning)
      // when the CH enters DATA and sends the first schedule.  By re-sending
      // every 5 s we dramatically increase the probability that at least one
      // schedule arrives while the member's radio is on the WiFi channel.
      // The epoch is recalculated each time so members always get a fresh,
      // correct slot window relative to "now".
      // ---------------------------------------------------------
      {
        uint64_t sched_now_ms = esp_timer_get_time() / 1000;
        bool should_send_sched = (s_current_phase == PHASE_DATA) && (s_schedule_sends_this_phase == 0 || (sched_now_ms - s_last_schedule_send_ms) >= 5000);

        if (should_send_sched && neighbor_count > 0) {
          ESP_LOGI(TAG, "[DEBUG] TDMA Schedule: Found %zu neighbors for scheduling", neighbor_count);
          for (size_t i = 0; i < neighbor_count; i++) {
            ESP_LOGI(TAG, "[DEBUG] TDMA Neighbor[%zu]: node_id=%lu, verified=%d, in_cluster=%d, is_ch=%d",
                     i, neighbors[i].node_id, neighbors[i].verified,
                     neighbor_manager_is_in_cluster(&neighbors[i]), neighbors[i].is_ch);
          }
          // Sort by Priority (Githmi-style: P = Link + (100-Bat))
          qsort(neighbors, neighbor_count, sizeof(neighbor_entry_t), compare_priority);
          // TDMA epoch: start after a guard from NOW
          int64_t epoch_us = esp_timer_get_time() + ((int64_t)PHASE_GUARD_MS * 1000LL);

          // DYNAMIC SLOT CALCULATION (Phase 24)
          // BUG 6 FIX: Check that clamped slots don't exceed available time.
          // If each slot is clamped to 2s minimum and we have 10 members,
          // total becomes 20s, exceeding the 15s DATA phase. This would cause
          // slots to spill into the next STELLAR phase.
          uint32_t available_ms = DATA_PHASE_MS - PHASE_GUARD_MS;
          uint32_t slot_ms = available_ms / neighbor_count;

          // If minimum slot would exceed available time, use smaller slot to fit
          // Bug 3 fix: Properly handle slot overflow by limiting scheduled members
          int scheduled_count = neighbor_count;
          if (slot_ms < 2000) {
            // Check if clamped slots would overflow
            uint32_t clamped_total_ms = 2000 * neighbor_count;
            if (clamped_total_ms > available_ms) {
              // Cap the number of scheduled members instead of using sub-2s slots
              scheduled_count = available_ms / 2000;
              if (scheduled_count < 1) scheduled_count = 1; // Always schedule at least one member
              slot_ms = available_ms / scheduled_count;
              ESP_LOGW(TAG,
                  "Cluster too large: scheduling %d/%lu members with slot %lu ms (was %lu)",
                  scheduled_count, (unsigned long)neighbor_count, (unsigned long)slot_ms,
                  (unsigned long)available_ms / neighbor_count);
            } else {
              slot_ms = 2000;
              ESP_LOGW(TAG, "Slot time clamped to minimum 2s");
            }
          }
          uint8_t slot_sec = slot_ms / 1000;
          if (slot_sec < 1) slot_sec = 1;  // add this

          for (size_t i = 0; i < (size_t)scheduled_count && i < neighbor_count; i++) {
            schedule_msg_t sched;
            sched.epoch_us = epoch_us;
            sched.slot_index = i;
            sched.slot_duration_sec = slot_sec;
            sched.magic = ESP_NOW_MAGIC_SCHEDULE;
            sched.target_node_id = neighbors[i].node_id;

            // ── Unicast (needs MAC ACK) ──
            esp_err_t uc_ret;
            if (s_schedule_sends_this_phase == 0) {
              uc_ret = esp_now_manager_send_data(neighbors[i].mac_addr,
                                                  (uint8_t *)&sched, sizeof(sched));
            } else {
              uc_ret = esp_now_manager_send_data_fast(neighbors[i].mac_addr,
                                                      (uint8_t *)&sched, sizeof(sched));
            }

            // ── Broadcast backup (no ACK needed — survives radio contention) ──
            // If unicast failed (member radio busy) the broadcast has a
            // second chance of being received.  target_node_id lets each
            // member filter to its own slot.
            if (uc_ret != ESP_OK) {
              static const uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
              // BUG 7 FIX: Check if broadcast peer exists before adding to avoid
              // filling the peer table with duplicates.
              if (!esp_now_is_peer_exist(BCAST)) {
                esp_now_peer_info_t bp = {0};
                memcpy(bp.peer_addr, BCAST, 6);
                bp.channel = ESP_NOW_CHANNEL;
                bp.ifidx   = WIFI_IF_STA;
                bp.encrypt = false;
                esp_now_add_peer(&bp);
              }
              esp_now_send(BCAST, (uint8_t *)&sched, sizeof(sched));
              ESP_LOGW(TAG, "SCHED: Unicast FAIL for node_%lu — sent broadcast backup",
                        neighbors[i].node_id);
            }

            ESP_LOGI(TAG, "SCHED: Assigned Slot %d to Node %lu (Score %.2f) [tx#%d]",
                      (int)i, neighbors[i].node_id, neighbors[i].score,
                      s_schedule_sends_this_phase + 1);
          }

          s_schedule_sends_this_phase++;
          s_last_schedule_send_ms = sched_now_ms;
        }
      }

      // ---------------------------------------------------------
      // CH SELF-DATA STORAGE — MOVED TO MAIN LOOP (ms_node.c)
      // ---------------------------------------------------------
      // Previously stored here every 5s, but during heavy MBR burst
      // receives the SPIFFS mutex contention could delay this 100ms
      // task by 10-15s, causing seq gaps (e.g. seq 43-45 missing).
      //
      // Now: the main loop (ms_node.c) stores directly to MSLG in
      // the SAME iteration that increments seq_num.  This guarantees
      // zero seq gaps for CH data regardless of ESP-NOW receive load.
      // ---------------------------------------------------------
    }
    break;

  case STATE_MEMBER:
    // Member duties
    {
      // Update metrics
      // metrics_update(); // Handled by metrics_task

      // Only scan during STELLAR phase. During DATA phase the radio must be
      // quiet for ESP-NOW transmission; unconditionally re-starting scanning
      // here was the primary cause of ESP-NOW send failures (BLE scan window
      // occupied the radio during esp_now_send, preventing MAC-layer ACK).
      //
      // BLE PRE-GUARD (3 s): Stop BLE just before STELLAR ends so the radio
      // is on the WiFi channel when the DATA phase boundary fires.  This is
      // NOT meant to cover the full phase-desync window — the CH's schedule
      // resend every 5 s + broadcast backup handles that.  The 3 s guard
      // ensures the very first schedule of each DATA phase is receivable.
      // STELLAR still gets 17 s for election/neighbor discovery.
      if (s_current_phase == PHASE_STELLAR) {
        uint64_t elapsed_in_frame = now_ms - s_phase_start_ms;

        if (elapsed_in_frame >= (STELLAR_PHASE_MS - BLE_PRE_GUARD_MS)) {
          // Last 3 s of STELLAR: stop BLE to free radio for ESP-NOW
          if (!s_pre_guard_active) {
            ble_manager_stop_scanning();
            ble_manager_stop_advertising();
            // Pin radio to ESP-NOW channel
            esp_wifi_set_channel(ESP_NOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
            ESP_LOGI(TAG, "BLE PRE-GUARD: Stopped BLE %.1fs before DATA phase",
                     (STELLAR_PHASE_MS - elapsed_in_frame) / 1000.0);
            s_pre_guard_active = true;
          }
        } else {
          // Normal STELLAR: scan for neighbors
          s_pre_guard_active = false;
          ble_manager_start_scanning();
        }
      }

      // Ensure BLE advertising is active (for broadcasting our metrics)
      // Note: member_ble_started is file-scope (see top of file) so it resets
      // correctly on every MEMBER re-entry via transition_to_state().
      // CRITICAL: Only start advertising during STELLAR phase.  Starting it
      // during DATA would re-enable BLE and interfere with ESP-NOW.
      if (!member_ble_started && s_current_phase == PHASE_STELLAR) {
        if (ble_manager_is_ready()) {
          ble_manager_start_advertising();
          member_ble_started = true;
          ESP_LOGI(TAG, "MEMBER: BLE advertising started");
        } else {
          ESP_LOGW(TAG, "MEMBER: Waiting for BLE to be ready");
          break; // Wait for BLE to be ready
        }
      }

      // ─────────────────────────────────────────────────────────────────────
      // PHASE-AWARE CH BEACON CHECK (Stability Fix)
      // ─────────────────────────────────────────────────────────────────────
      // During DATA phase, BLE scanning is OFF so we CANNOT receive CH beacons.
      // Skip beacon timeout check during DATA phase to prevent false positives.
      // We only check CH validity during STELLAR phase when BLE is active.
      // The CH is cached (s_cached_ch_id) at STELLAR→DATA transition.
      // ─────────────────────────────────────────────────────────────────────
      // Bug 6 fix: ch_miss_count and last_logged_phase are now file-scope
      // and reset in transition_to_state() on MEMBER entry.
      // ─────────────────────────────────────────────────────────────────────

      if (s_current_phase == PHASE_STELLAR) {
        if (last_logged_phase != PHASE_STELLAR) {
          ESP_LOGD(TAG, "STELLAR phase: CH beacon check enabled (BLE on)");
        }
        last_logged_phase = PHASE_STELLAR;
        
        // STELLAR phase: BLE is on, we can receive beacons - check CH validity
        uint32_t current_ch = neighbor_manager_get_current_ch();
        if (current_ch == 0) {
          ch_miss_count++;
          if (ch_miss_count >= CH_MISS_THRESHOLD) {
            ESP_LOGW(
                TAG,
                "CH lost (confirmed after %d misses during STELLAR), "
                "returning to candidate",
                ch_miss_count);
            ch_miss_count = 0;
            member_ble_started = false; // Reset flag
            transition_to_state(STATE_CANDIDATE);
            election_reset_window();
            break;
          } else {
            ESP_LOGW(TAG, "CH beacon missed (%d/%d) during STELLAR, waiting...",
                     ch_miss_count, CH_MISS_THRESHOLD);
          }
        } else {
          // Valid CH beacon received - reset miss counter
          if (ch_miss_count > 0) {
            ESP_LOGI(TAG, "CH beacon received (node_%lu), miss counter reset",
                     current_ch);
          }
          ch_miss_count = 0;
        }
      } else {
        // DATA phase: BLE is off, skip beacon check (rely on cached CH)
        // Only log once per DATA phase entry to avoid spam
        if (last_logged_phase != PHASE_DATA) {
          ESP_LOGD(TAG, "DATA phase: CH beacon check suspended (BLE off)");
          last_logged_phase = PHASE_DATA;
        }
      }

      // Update advertisement (as member) - ONLY IN STELLAR PHASE
      if (s_current_phase == PHASE_STELLAR) {
        static uint64_t last_adv_update = 0;
        uint64_t now_ms_adv = esp_timer_get_time() / 1000;
        if (now_ms_adv - last_adv_update >= 1000) {
          if (ble_manager_is_ready()) {
            ble_manager_update_advertisement();
            last_adv_update = now_ms_adv;
          }
        }
      }

      // Member duties: duty-cycle radio, send keep-alive, etc.
      neighbor_manager_cleanup_stale();

      // Check if re-election is needed
      if (s_current_phase == PHASE_STELLAR &&
          election_check_reelection_needed()) {
        ESP_LOGI(TAG, "Re-election needed, returning to candidate");
        member_ble_started = false; // Reset flag
        transition_to_state(STATE_CANDIDATE);
        election_reset_window();
      }

      // Send data (Time Sliced or Fallback)
      static uint64_t last_data_send = 0;
      // Track when the DATA phase last started so we can send once on entry
      // even without a TDMA schedule (no-schedule fallback).
      static phase_t s_last_seen_phase = PHASE_STELLAR;
      // Cache the CH MAC from STELLAR phase so we can still send during DATA
      // phase even after BLE stops and the CH beacon timeout fires.
      static uint8_t s_cached_ch_mac[6] = {0};
      static uint32_t s_cached_ch_id = 0;

      // ── Diagnostic: TDMA window tracking ──
      static bool s_window_active = false;
      static int  s_window_mslg_sent = 0;
      static int  s_window_sensor_sent = 0;
      static int  s_window_mslg_total_at_start = 0;

      schedule_msg_t sched = esp_now_get_current_schedule();
      int64_t now_us = esp_timer_get_time();
      bool can_send = false;
      bool has_valid_schedule = (sched.magic == ESP_NOW_MAGIC_SCHEDULE &&
                                 sched.epoch_us > (now_us - 60000000LL));

      if (has_valid_schedule) {
        // ── TDMA Slot Mode
        // ────────────────────────────────────────────────────
        int64_t my_start =
            sched.epoch_us +
            (sched.slot_index * sched.slot_duration_sec * 1000000LL);
        int64_t my_end = my_start + (sched.slot_duration_sec * 1000000LL);

        if (now_us >= my_start && now_us < my_end) {
          // ── Diagnostic: Window started
          if (!s_window_active) {
            s_window_active = true;
            s_window_mslg_sent = 0;
            s_window_sensor_sent = 0;
            s_window_mslg_total_at_start = storage_manager_get_mslg_chunk_count();
            ESP_LOGI(TAG, "┌─ TDMA WINDOW STARTED ─────────────────────────┐");
            ESP_LOGI(TAG, "│ Slot: %d | Duration: %ds                      │",
                     sched.slot_index, sched.slot_duration_sec);
            ESP_LOGI(TAG, "│ MSLG chunks in storage at start: %d           │",
                     s_window_mslg_total_at_start);
            ESP_LOGI(TAG, "└───────────────────────────────────────────────┘");
          }

          // In our TDMA slot — enforce >2s gap so we send exactly once per slot
          if ((now_us - (last_data_send * 1000)) > 2000000LL) {
            can_send = true;
            ESP_LOGI(TAG, "TIME SLICING: In Slot %d (window match), sending...",
                     sched.slot_index);
          }
        } else if (s_window_active) {
          // ── Diagnostic: Window ended — print summary
          int mslg_remaining = storage_manager_get_mslg_chunk_count();
          int total_payloads = s_window_sensor_sent + s_window_mslg_sent;
          float pct_sensor = (total_payloads > 0)
              ? (100.0f * s_window_sensor_sent / total_payloads) : 0.0f;
          float pct_mslg = (total_payloads > 0)
              ? (100.0f * s_window_mslg_sent / total_payloads) : 0.0f;
          ESP_LOGI(TAG, "┌─ TDMA WINDOW ENDED ───────────────────────────────────┐");
          ESP_LOGI(TAG, "│ Sensor payloads sent : %-4d  (%.1f%% of total)        │",
                   s_window_sensor_sent, pct_sensor);
          ESP_LOGI(TAG, "│ MSLG chunks sent     : %-4d  (%.1f%% of total)        │",
                   s_window_mslg_sent, pct_mslg);
          ESP_LOGI(TAG, "│ Total packets sent   : %-4d                           │",
                   total_payloads);
          ESP_LOGI(TAG, "│ MSLG remaining       : %-4d (was %d at start)         │",
                   mslg_remaining, s_window_mslg_total_at_start);
          ESP_LOGI(TAG, "└───────────────────────────────────────────────────────┘");
          s_window_active = false;
        }
      } else if (s_current_phase == PHASE_DATA) {
        // ── No schedule received yet — DO NOT SEND.
        // Without a TDMA window all members would transmit simultaneously,
        // causing collisions (same firmware on every node).  Data stays in
        // local MSLG storage and will be drained once a schedule arrives.
        // The CH re-sends schedules every 5 s; BLE pre-guard on the member
        // ensures the radio is listening when schedules arrive.
        static bool s_nosched_warned = false;
        bool just_entered_data = (s_last_seen_phase != PHASE_DATA);
        if (just_entered_data) {
          s_nosched_warned = false;
        }
        if (!s_nosched_warned) {
          ESP_LOGW(TAG,
                   "NO-SCHED: Waiting for TDMA schedule from CH "
                   "(data buffered in MSLG, chunks=%d)",
                   storage_manager_get_mslg_chunk_count());
          s_nosched_warned = true;
        }
      }
      s_last_seen_phase = s_current_phase;

      // Only send during DATA phase
      if (s_current_phase != PHASE_DATA) {
        can_send = false;
      }

      // ─────────────────────────────────────────────────────────────────────
      // CH_BUSY CHECK: If CH is busy with UAV onboarding, HOLD data
      // ─────────────────────────────────────────────────────────────────────
      // Members should NOT send data when CH is offloading to UAV because:
      // 1. ESP-NOW is deinitialized on CH (no one to receive)
      // 2. WiFi channel changed (ESP-NOW would fail anyway)
      // 3. Data would be lost - better to buffer locally
      // ─────────────────────────────────────────────────────────────────────
      static bool s_ch_busy_logged = false;
      if (esp_now_manager_is_ch_busy()) {
        if (can_send && !s_ch_busy_logged) {
          ESP_LOGW(TAG, "CH is BUSY with UAV onboarding - HOLDING DATA (will resume when CH sends RESUME)");
          s_ch_busy_logged = true;
        }
        can_send = false;  // Block all sends while CH is busy
      } else {
        if (s_ch_busy_logged) {
          ESP_LOGI(TAG, "CH RESUME received - resuming TDMA data transmission");
          s_ch_busy_logged = false;
        }
      }

      // Cache CH MAC in STELLAR phase so we can still send in DATA phase
      // even after BLE stops and the CH beacon timeout fires.
      uint32_t current_ch_for_cache = neighbor_manager_get_current_ch();
      if (s_current_phase == PHASE_STELLAR && current_ch_for_cache != 0) {
        uint8_t tmp_mac[6];
        if (neighbor_manager_get_ch_mac(tmp_mac)) {
          memcpy(s_cached_ch_mac, tmp_mac, 6);
          s_cached_ch_id = current_ch_for_cache;
        }
      }

      // Use live CH lookup when available; fall back to cache in DATA phase.
      bool ch_available = false;
      uint8_t ch_mac_to_use[6] = {0};
      if (current_ch_for_cache != 0) {
        uint8_t tmp_mac[6];
        if (neighbor_manager_get_ch_mac(tmp_mac)) {
          memcpy(ch_mac_to_use, tmp_mac, 6);
          ch_available = true;
        }
      }
      if (!ch_available && s_current_phase == PHASE_DATA &&
          s_cached_ch_id != 0) {
        // BLE beacon unavailable in DATA phase — use cached CH MAC
        memcpy(ch_mac_to_use, s_cached_ch_mac, 6);
        ch_available = true;
        ESP_LOGD(TAG,
                 "Using cached CH MAC for DATA phase send "
                 "(BLE off + beacon timeout, cached node_%lu)",
                 s_cached_ch_id);
      }

      // ---------------------------------------------------------
      // MEMBER CONTINUOUS STORAGE — MOVED TO MAIN LOOP (ms_node.c)
      // ---------------------------------------------------------
      // Previously stored here every 5s using a shared latest-payload
      // snapshot, but the 100ms task could miss seqs when the main
      // loop advanced faster than this task could store. Now: the
      // main loop stores directly to MSLG in the SAME iteration that
      // increments seq_num, guaranteeing zero seq gaps for member data.
      // ---------------------------------------------------------

      // ---------------------------------------------------------
      // MEMBER SEND — ONLY WITHIN ASSIGNED TDMA SLOT
      // ---------------------------------------------------------
      // No fallback send without a schedule: identical firmware on
      // all members would fire simultaneously → RF collisions.
      // Data stays in local MSLG until a TDMA window is received.
      // The CH re-sends schedules every 5s during DATA phase and
      // the member BLE pre-guard ensures reception.
      // ---------------------------------------------------------
      if (can_send && has_valid_schedule) {
        last_data_send = now_ms;
        ESP_LOGD(TAG, "TDMA slot active - burst drain will handle data delivery");
      }

      // -------------------------------------------------------------
      // Time-Bounded Burst (Novelty: "Sprint" during slot)
      // -------------------------------------------------------------
      // Reuse sched and now_us variables from above (updated)
      now_us = esp_timer_get_time();
      sched = esp_now_get_current_schedule();
      bool in_slot = false;
      int64_t slot_end_us = 0;

      if (sched.magic == ESP_NOW_MAGIC_SCHEDULE &&
          sched.epoch_us > (now_us - (SLOT_DURATION_SEC * 10000000LL))) {
        int64_t my_start =
            sched.epoch_us +
            (sched.slot_index * sched.slot_duration_sec * 1000000LL);
        int64_t my_end = my_start + (sched.slot_duration_sec * 1000000LL);
        slot_end_us = my_end;

        if (now_us >= my_start && now_us < my_end) {
          in_slot = true;
        }
      }

      // If we are in our slot, burst send stored data!
      // Bug 3 fix: Limit to MAX_BURST_PACKETS per cycle to prevent blocking
      // the state machine task for the entire slot duration. The remaining
      // packets will be sent in subsequent loop iterations.
      // -------------------------------------------------------------
      // 2. Failover Data Transfer & Burst Upload (Store-First Drain)
      // -------------------------------------------------------------
      if (s_current_phase == PHASE_DATA && in_slot && ch_available) {
        uint8_t ch_mac[6];
        memcpy(ch_mac, ch_mac_to_use, 6); // Use cached MAC (resolved above with fallback)
        {
          int64_t cutoff_us = slot_end_us - 500000LL; // 500ms margin
          {
            /* Batch pop up to 24 chunks (covers ~2 minutes of backlog). */
            #define BATCH_POP_MAX 24
            mslg_popped_chunk_t batch[BATCH_POP_MAX];
            int batch_count = 0;

            esp_err_t pop_err = storage_manager_pop_mslg_chunks_batch(
                batch, BATCH_POP_MAX, &batch_count);

            int comp_sent = 0;

            if (pop_err == ESP_OK && batch_count > 0) {
              for (int bi = 0; bi < batch_count; bi++) {

                /* Time budget check — requeue unsent chunks */
                if (esp_timer_get_time() >= cutoff_us) {
                  ESP_LOGW(TAG, "OFFLOAD: Time budget hit at chunk %d/%d, requeueing rest",
                           bi, batch_count);
                  for (int ri = bi; ri < batch_count; ri++) {
                    storage_manager_write_compressed(
                        (const char *)batch[ri].payload, (batch[ri].algo == 1));
                    heap_caps_free(batch[ri].payload);
                  }
                  break;
                }

                uint8_t *send_buf = batch[bi].payload;
                size_t send_len = batch[bi].payload_len;
                uint8_t *decomp_buf = NULL;

                /* Decompress if needed */
                if (batch[bi].algo == 1) {
                  decomp_buf = heap_caps_malloc(batch[bi].raw_len, MALLOC_CAP_8BIT);
                  if (!decomp_buf) {
                    ESP_LOGW(TAG, "OFFLOAD: OOM decompress chunk %d, requeueing rest", bi);
                    for (int ri = bi; ri < batch_count; ri++) {
                      storage_manager_write_compressed(
                          (const char *)batch[ri].payload, true);
                      heap_caps_free(batch[ri].payload);
                    }
                    break;
                  }

                  size_t out_len = batch[bi].raw_len;
                  comp_stats_t stats = {0};
                  esp_err_t derr = lz_decompress_miniz(
                      batch[bi].payload, batch[bi].payload_len,
                      decomp_buf, batch[bi].raw_len, &out_len, &stats);

                  if (derr != ESP_OK || out_len != batch[bi].raw_len) {
                    ESP_LOGW(TAG, "OFFLOAD: Decompress fail chunk %d, requeueing rest", bi);
                    heap_caps_free(decomp_buf);
                    for (int ri = bi; ri < batch_count; ri++) {
                      storage_manager_write_compressed(
                          (const char *)batch[ri].payload, true);
                      heap_caps_free(batch[ri].payload);
                    }
                    break;
                  }

                  send_buf = decomp_buf;
                  send_len = out_len;
                  heap_caps_free(batch[bi].payload);
                  batch[bi].payload = NULL;
                }

                /* Send via fast path (no BLE quiet window) */
                esp_err_t send_ret = esp_now_manager_send_data_fast(
                    ch_mac, send_buf, send_len);

                if (send_ret == ESP_OK) {
                  comp_sent++;
                  s_window_mslg_sent++;
                  vTaskDelay(pdMS_TO_TICKS(5));
                  if (decomp_buf) {
                    heap_caps_free(decomp_buf);
                    decomp_buf = NULL; // Bug 7 fix: Prevent double free
                  } else {
                    heap_caps_free(batch[bi].payload);
                  }
                  batch[bi].payload = NULL;
                } else {
                  ESP_LOGW(TAG, "OFFLOAD: fast send fail chunk %d, requeueing rest", bi);
                  /* Requeue this chunk and all remaining */
                  if (decomp_buf) {
                    storage_manager_write_compressed(
                        (const char *)decomp_buf, false);
                    heap_caps_free(decomp_buf);
                  } else {
                    storage_manager_write_compressed(
                        (const char *)batch[bi].payload, true);
                    heap_caps_free(batch[bi].payload);
                  }
                  batch[bi].payload = NULL;
                  for (int ri = bi + 1; ri < batch_count; ri++) {
                    storage_manager_write_compressed(
                        (const char *)batch[ri].payload, true);
                    heap_caps_free(batch[ri].payload);
                    batch[ri].payload = NULL;
                  }
                  break;
                }
              }
            }

            if (comp_sent > 0) {
              ESP_LOGI(TAG, "OFFLOAD: MSLG drain sent %d chunks | remaining: %d",
                       comp_sent, storage_manager_get_mslg_chunk_count());
            }
          }

        }
      }

      // Fallback: Lazy single line sync if no schedule
      static uint64_t last_history_sync = 0;
      // BUG 5 FIX: Only update timer when fallback ACTUALLY sends, not every tick.
      // Previously the timestamp reset happened unconditionally outside the if block,
      // so the countdown restarted every 100ms and could never reach 5000ms.
      if (!in_slot && current_ch_for_cache != 0 &&
          (esp_timer_get_time() / 1000 - last_history_sync) >= 5000) {
        // ... existing fallback code ...
        last_history_sync = esp_timer_get_time() / 1000;
      }
    }
    break;

  case STATE_UAV_ONBOARDING:
    // UAV Interaction Phase - Temporarily switch from ESP-NOW to WiFi STA
    {
      ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
      ESP_LOGI(TAG, "║       UAV ONBOARDING: Suspending STELLAR Protocol        ║");
      ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");

      // 0. DISABLE RF receiver to prevent multiple triggers during onboarding
      ESP_LOGI(TAG, "[0/8] Disabling RF receiver to prevent multiple triggers");
      extern void rf_receiver_disable(void);
      rf_receiver_disable();

      // 1. FIRST: Broadcast CH_BUSY status to ALL members via ESP-NOW
      //    Members will see this and HOLD their data (not try to send during onboarding)
      ESP_LOGI(TAG, "[1/8] Broadcasting CH_BUSY to members - HOLD DATA");
      esp_now_manager_broadcast_ch_status(CH_STATUS_UAV_BUSY);
      vTaskDelay(pdMS_TO_TICKS(200)); // Give time for broadcasts to complete

      // 2. Stop BLE scanning to avoid radio interference
      ESP_LOGI(TAG, "[2/8] Stopping BLE scanning");
      ble_manager_stop_scanning();
      ble_manager_stop_advertising();

      // 3. Deinit ESP-NOW (channel conflict with WiFi AP connection)
      //    ESP-NOW uses fixed channel, WiFi STA connects to UAV AP on its channel
      ESP_LOGI(TAG, "[3/8] Deinitializing ESP-NOW for WiFi STA mode");
      esp_err_t err = esp_now_manager_deinit();
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to deinit ESP-NOW: %s", esp_err_to_name(err));
      }

      // 4. Execute UAV onboarding (WiFi connect → HTTP upload → ACK)
      //    This uploads ALL stored sensor data to the UAV server
      ESP_LOGI(TAG, "[4/8] Executing UAV onboarding sequence...");
      ESP_LOGI(TAG, "       - Connecting to UAV WiFi AP (WSN_AP)");
      ESP_LOGI(TAG, "       - Uploading ALL stored sensor data");
      ESP_LOGI(TAG, "       - Waiting for server ACK");
      
      esp_err_t ret = uav_client_run_onboarding();

      if (ret == ESP_OK) {
        ESP_LOGI(TAG, "╔══════════════════════════════════════════════════════════╗");
        ESP_LOGI(TAG, "║         UAV Onboarding SUCCESS: Data Offloaded           ║");
        ESP_LOGI(TAG, "╚══════════════════════════════════════════════════════════╝");
      } else {
        ESP_LOGE(TAG, "╔══════════════════════════════════════════════════════════╗");
        ESP_LOGE(TAG, "║         UAV Onboarding FAILED: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "╚══════════════════════════════════════════════════════════╝");
      }

      // 5. Cleanup UAV client resources (destroy STA netif before reinit)
      ESP_LOGI(TAG, "[5/8] Reinitializing ESP-NOW on fixed channel");
      uav_client_cleanup();  // Destroy STA netif before ESP-NOW reinit
      err = esp_now_manager_reinit();
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to reinit ESP-NOW: %s", esp_err_to_name(err));
      }

      // 6. Broadcast CH_RESUME status to ALL members
      //    Members will see this and RESUME their TDMA data sending
      ESP_LOGI(TAG, "[6/8] Broadcasting CH_RESUME to members - RESUME TDMA");
      esp_now_manager_broadcast_ch_status(CH_STATUS_RESUME);
      vTaskDelay(pdMS_TO_TICKS(100)); // Give time for broadcast

      // 7. Re-enable RF receiver to listen for future triggers
      ESP_LOGI(TAG, "[7/8] Re-enabling RF receiver");
      extern void rf_receiver_enable(void);
      rf_receiver_enable();

      // 8. Return to CH state and resume STELLAR protocol
      ESP_LOGI(TAG, "[8/8] Returning to CH state, resuming STELLAR protocol");
      transition_to_state(STATE_CH);

      // Re-enable BLE advertising (beacons for members)
      ble_manager_start_advertising();
      ESP_LOGI(TAG, "BLE advertising resumed - STELLAR protocol active");
    }
    break;

  case STATE_SLEEP:
    // Sleep state (for future implementation)
    break;
  }
}
