#include "state_machine.h"
#include "ble_manager.h"
#include "config.h"
#include "election.h"
#include "esp_log.h"
#include "esp_mac.h"
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
#include <freertos/queue.h>
#include <freertos/task.h>
#include <stdlib.h> // For qsort
#include <string.h>

#define UAV_ONBOARDING_TIMEOUT_MS 300000  // 5 min - only used when UAV detected (RF or TRIGGER_UAV)

static int compare_priority(const void *a, const void *b) {
  const neighbor_entry_t *na = (const neighbor_entry_t *)a;
  const neighbor_entry_t *nb = (const neighbor_entry_t *)b;

  // Githmi's Formula: P = LinkQuality + (100 - Battery)
  // Higher P goes first.
  // Note: metrics are 0.0-1.0, so we scale by 100.
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

#define SLEEP_MIN_MS 100
#define SLEEP_MAX_MS 60000

uint32_t state_machine_get_sleep_time_ms(void) {
  // Only smart sleep in MEMBER state with a valid schedule
  if (g_current_state == STATE_MEMBER) {
    schedule_msg_t sched = esp_now_get_current_schedule();
    int64_t now_us = esp_timer_get_time();

    if (sched.magic == ESP_NOW_MAGIC_SCHEDULE &&
        sched.epoch_us > (now_us - (SLOT_DURATION_SEC *
                                    10000000LL))) { /* Valid recent schedule */

      int64_t my_slot_start =
          sched.epoch_us +
          (sched.slot_index * sched.slot_duration_sec * 1000000LL);
      int64_t time_to_slot = my_slot_start - now_us;

      if (time_to_slot > 0) {
        // We are early. Sleep until slot. Cap to avoid overflow/long sleep.
        uint32_t sleep_ms = (uint32_t)(time_to_slot / 1000);
        if (sleep_ms > SLEEP_MAX_MS)
          sleep_ms = SLEEP_MAX_MS;
        return sleep_ms;
      } else {
        // We are IN the slot or LATE.
        int64_t time_in_slot = -time_to_slot;
        int64_t slot_len_us = sched.slot_duration_sec * 1000000LL;

        if (time_in_slot < slot_len_us) {
          return SLEEP_MIN_MS;  // In slot: run soon
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
static bool s_schedule_sent_this_data_phase = false;
static uint64_t s_last_ch_store_ms = 0; // Moved to file scope for debugging

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
  } else {
    g_is_ch = false;
  }

  // Reset MEMBER BLE flag on every state transition so it is always
  // re-initialised when entering MEMBER state (Bug 1 fix).
  if (new_state != STATE_MEMBER) {
    member_ble_started = false;
  }

  g_current_state = new_state;
  led_manager_set_state(new_state);
  state_entry_time = esp_timer_get_time() / 1000;
}

static QueueHandle_t s_onboarding_result_queue = NULL;

static void onboarding_task(void *pv) {
  esp_err_t ret = uav_client_run_onboarding();
  if (s_onboarding_result_queue != NULL) {
    xQueueOverwrite(s_onboarding_result_queue, &ret);
  }
  vTaskDelete(NULL);
}

void state_machine_init(void) {
  if (s_onboarding_result_queue == NULL) {
    s_onboarding_result_queue = xQueueCreate(1, sizeof(esp_err_t));
  }
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

void state_machine_force_uav_test(void) {
  ESP_LOGI(TAG, "Forcing UAV Test Mode (Manual Trigger)");
  transition_to_state(STATE_UAV_ONBOARDING);
}

void state_machine_run(void) {
  uint64_t now_ms = esp_timer_get_time() / 1000;

  // Update superframe phase (STELLAR vs DATA) and reset TDMA schedule flag
  phase_t prev_phase = s_current_phase;
  state_machine_update_phase(now_ms);
  if (s_current_phase != prev_phase) {
    ESP_LOGI(TAG, "Phase transition: %s -> %s",
             (prev_phase == PHASE_STELLAR) ? "STELLAR" : "DATA",
             (s_current_phase == PHASE_STELLAR) ? "STELLAR" : "DATA");

    if (s_current_phase == PHASE_DATA) {
      // New DATA phase: Stop BLE to clear radio for ESP-NOW
      // CRITICAL FIX: Only established nodes (Member/CH) respect the quiet
      // phase. Candidates and Discovery nodes MUST keep scanning/advertising to
      // find the cluster.

      if (g_current_state == STATE_MEMBER || g_current_state == STATE_CH) {
        ble_manager_stop_scanning();
      }

      // Only Members stop advertising.
      // CH must remain visible for new nodes.
      // Candidates must advertise to win elections.
      if (g_current_state == STATE_MEMBER) {
        ble_manager_stop_advertising();
      }

      // CRITICAL FIX: Ensure radio is on ESP-NOW channel (1) after BLE hopping
      esp_wifi_set_channel(ESP_NOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

      // Allow one TDMA schedule broadcast
      s_schedule_sent_this_data_phase = false;
    } else {
      // New STELLAR phase: Restart BLE
      ble_manager_start_advertising();
      ble_manager_start_scanning();
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
      // Update metrics
      // metrics_update(); // Handled by metrics_task

      // Keep scanning so we can detect other CH beacons and resolve conflicts
      // (CH state can be reached from DISCOVER where scanning may have been
      // stopped).
      ble_manager_start_scanning();

      // Update CH announcement
      ble_manager_update_advertisement();

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

      // Check cluster size
      neighbor_entry_t neighbors[MAX_NEIGHBORS];
      size_t count = neighbor_manager_get_all(neighbors, MAX_NEIGHBORS);
      if (count > MAX_CLUSTER_SIZE) {
        ESP_LOGW(TAG, "Cluster size exceeded (%zu), triggering split", count);
        // In production, implement cluster split logic
      }

      // UAV Trigger Check
      if (rf_receiver_check_trigger()) {
        ESP_LOGI(TAG, "UAV Trigger detected! Transitioning to UAV ONBOARDING");
        transition_to_state(STATE_UAV_ONBOARDING);
      }

      // ---------------------------------------------------------
      // TIME SLICING SCHEDULER (Novelty) - runs once per DATA phase
      // ---------------------------------------------------------
      if (s_current_phase == PHASE_DATA && !s_schedule_sent_this_data_phase) {
        neighbor_entry_t neighbors[MAX_NEIGHBORS];
        size_t count = neighbor_manager_get_all(neighbors, MAX_NEIGHBORS);

        if (count > 0) {
          // Sort by Priority (Githmi-style: P = Link + (100-Bat))
          qsort(neighbors, count, sizeof(neighbor_entry_t), compare_priority);

          // TDMA epoch: start after a guard inside DATA phase
          int64_t epoch_us =
              esp_timer_get_time() + ((int64_t)PHASE_GUARD_MS * 1000LL);

          // DYNAMIC SLOT CALCULATION (Phase 24)
          uint32_t available_ms = DATA_PHASE_MS - PHASE_GUARD_MS;
          uint32_t slot_ms = available_ms / count;
          if (slot_ms < 2000) {
            slot_ms = 2000;
            ESP_LOGW(TAG, "Slot time clamped to minimum 2s");
          }
          uint8_t slot_sec = slot_ms / 1000;

          schedule_msg_t scheds[MAX_NEIGHBORS];
          uint8_t macs[MAX_NEIGHBORS][6];
          for (size_t i = 0; i < count; i++) {
            scheds[i].epoch_us = epoch_us;
            scheds[i].slot_index = (uint8_t)i;
            scheds[i].slot_duration_sec = slot_sec;
            scheds[i].magic = ESP_NOW_MAGIC_SCHEDULE;
            memcpy(macs[i], neighbors[i].mac_addr, 6);
          }

          esp_err_t burst_ret =
              esp_now_manager_send_schedule_burst(macs, scheds, count);

          if (burst_ret != ESP_OK) {
            ESP_LOGW(TAG, "Schedule burst failed, falling back to per-peer send");
            for (size_t i = 0; i < count; i++) {
              esp_now_manager_send_data(neighbors[i].mac_addr,
                                        (uint8_t *)&scheds[i], sizeof(scheds[i]));
            }
          }
          for (size_t i = 0; i < count; i++) {
            ESP_LOGI(TAG, "SCHED: Assigned Slot %d to Node %lu (Score %.2f)",
                     (int)i, neighbors[i].node_id, neighbors[i].score);
          }

          s_schedule_sent_this_data_phase = true;
        }
      }

      // ---------------------------------------------------------
      // CH SELF-DATA STORAGE (New Requirement)
      // ---------------------------------------------------------
      // Log data at same rate as members (every 5s or as configured)
      if (s_current_phase == PHASE_DATA &&
          (now_ms - s_last_ch_store_ms) >= 5000) {
        sensor_payload_t payload;
        metrics_get_sensor_data(&payload);

        // Ensure nonzero timestamp
        if (payload.timestamp_ms == 0) {
          payload.timestamp_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
          payload.node_id = g_node_id;
        }

        // Format as JSON (Full sensor data)
        char json_payload[384];
        snprintf(
            json_payload, sizeof(json_payload),
            "{\"id\":%lu,\"seq\":%lu,\"mac\":\"%02x%02x%02x%02x%02x%02x\","
            "\"ts\":%llu,"
            "\"t\":%.1f,\"h\":%.1f,\"p\":%lu,\"q\":%d,\"eco2\":%d,\"tvoc\":%d,"
            "\"mx\":%.2f,\"my\":%.2f,\"mz\":%.2f,\"a\":%.3f}",
            payload.node_id, payload.seq_num, payload.mac_addr[0],
            payload.mac_addr[1], payload.mac_addr[2], payload.mac_addr[3],
            payload.mac_addr[4], payload.mac_addr[5], payload.timestamp_ms,
            payload.temp_c, payload.hum_pct,
            (unsigned long)payload.pressure_hpa, payload.aqi, payload.eco2_ppm,
            payload.tvoc_ppb, payload.mag_x, payload.mag_y, payload.mag_z,
            payload.audio_rms);

        // Write to storage (MSLG format, compression when payload >= 1KB)
        esp_err_t ret = storage_manager_write_compressed(json_payload, true);
        if (ret == ESP_OK) {
          ESP_LOGI(TAG, "CH Stored own sensor data: seq=%lu", payload.seq_num);
          s_last_ch_store_ms = now_ms;
        } else {
          ESP_LOGE(TAG, "CH Failed to store own data: %s",
                   esp_err_to_name(ret));
        }
      }
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
      if (s_current_phase == PHASE_STELLAR) {
        ble_manager_start_scanning();
      }

      // Ensure BLE advertising is active (for broadcasting our metrics)
      // Note: member_ble_started is file-scope (see top of file) so it resets
      // correctly on every MEMBER re-entry via transition_to_state().
      if (!member_ble_started) {
        if (ble_manager_is_ready()) {
          ble_manager_start_advertising();
          member_ble_started = true;
          ESP_LOGI(TAG, "MEMBER: BLE advertising started");
        } else {
          ESP_LOGW(TAG, "MEMBER: Waiting for BLE to be ready");
          break; // Wait for BLE to be ready
        }
      }

      // Check if CH is still valid (with grace period to tolerate transient
      // loss). CH_MISS_THRESHOLD is defined in config.h and matches
      // CH_BEACON_TIMEOUT_MS (5 misses × ~1s/cycle ≈ 5s).
      static int ch_miss_count = 0;

      uint32_t current_ch = neighbor_manager_get_current_ch();
      if (current_ch == 0) {
        ch_miss_count++;
        if (ch_miss_count >= CH_MISS_THRESHOLD) {
          ESP_LOGW(
              TAG,
              "CH lost (confirmed after %d misses), returning to candidate",
              ch_miss_count);
          ch_miss_count = 0;
          member_ble_started = false; // Reset flag
          transition_to_state(STATE_CANDIDATE);
          election_reset_window();
          break;
        } else {
          ESP_LOGW(TAG, "CH beacon missed (%d/%d), waiting...", ch_miss_count,
                   CH_MISS_THRESHOLD);
        }
      } else {
        ch_miss_count = 0; // Reset on valid beacon
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
          // In our TDMA slot — enforce >2s gap so we send exactly once per slot
          if ((now_us - (last_data_send * 1000)) > 2000000LL) {
            can_send = true;
            ESP_LOGI(TAG, "TIME SLICING: In Slot %d (window match), sending...",
                     sched.slot_index);
          }
        }
      } else if (s_current_phase == PHASE_DATA) {
        // ── No-Schedule Fallback
        // ────────────────────────────────────────────── CH may not have sent a
        // schedule yet (e.g. neighbors table was empty at phase boundary). Send
        // once per DATA-phase entry so we still deliver data to the CH.
        bool just_entered_data = (s_last_seen_phase != PHASE_DATA);
        if (just_entered_data || (now_ms - last_data_send) >= 5000) {
          can_send = true;
          ESP_LOGI(TAG,
                   "NO-SCHED FALLBACK: Sending sensor data (phase=%s, "
                   "last_send=%llu ms ago)",
                   just_entered_data ? "NEW_DATA" : "PERIODIC",
                   (unsigned long long)(now_ms - last_data_send));
        }
      }
      s_last_seen_phase = s_current_phase;

      // Only send during DATA phase
      if (s_current_phase != PHASE_DATA) {
        can_send = false;
      }

      // Cache CH MAC in STELLAR phase so we can still send in DATA phase
      // even after BLE stops and the CH beacon timeout fires.
      if (s_current_phase == PHASE_STELLAR && current_ch != 0) {
        uint8_t tmp_mac[6];
        if (neighbor_manager_get_ch_mac(tmp_mac)) {
          memcpy(s_cached_ch_mac, tmp_mac, 6);
          s_cached_ch_id = current_ch;
        }
      }

      // Use live CH lookup when available; fall back to cache in DATA phase.
      bool ch_available = false;
      uint8_t ch_mac_to_use[6] = {0};
      if (current_ch != 0) {
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

      if (can_send) {
        if (ch_available) {
          sensor_payload_t payload;
          metrics_get_sensor_data(&payload);

          // N2-FIX: Remove timestamp_ms==0 silent drop.
          // Mock data is valid even before sensor task has run once;
          // we still want to send it so CH can see members are alive.
          if (payload.timestamp_ms == 0) {
            ESP_LOGW(TAG,
                     "Sensor data has timestamp=0 (sensor task not yet run), "
                     "sending with current timestamp anyway");
            payload.timestamp_ms = (uint64_t)(esp_timer_get_time() / 1000ULL);
            payload.node_id = g_node_id;
          }

          // FIX: Prevent duplicate storage writes - track last stored sequence
          // number
          static uint64_t last_stored_seq = 0;
          bool should_store = (payload.seq_num != last_stored_seq);

          if (should_store) {
            // STORE-FIRST: Format as JSON and write to storage (Full sensor
            // data)
            char json_payload[384];
            snprintf(
                json_payload, sizeof(json_payload),
                "{\"id\":%lu,\"seq\":%lu,\"mac\":\"%02x%02x%02x%02x%02x%02x\","
                "\"ts\":%llu,"
                "\"t\":%.1f,\"h\":%.1f,\"p\":%lu,\"q\":%d,\"eco2\":%d,\"tvoc\":"
                "%d,"
                "\"mx\":%.2f,\"my\":%.2f,\"mz\":%.2f,\"a\":%.3f}",
                payload.node_id, payload.seq_num, payload.mac_addr[0],
                payload.mac_addr[1], payload.mac_addr[2], payload.mac_addr[3],
                payload.mac_addr[4], payload.mac_addr[5], payload.timestamp_ms,
                payload.temp_c, payload.hum_pct,
                (unsigned long)payload.pressure_hpa, payload.aqi,
                payload.eco2_ppm, payload.tvoc_ppb, payload.mag_x,
                payload.mag_y, payload.mag_z, payload.audio_rms);

            esp_err_t ret =
                storage_manager_write_compressed(json_payload, true);
            if (ret == ESP_OK) {
              last_stored_seq = payload.seq_num;
              ESP_LOGI(TAG, "Stored sensor data (Store-First): seq=%lu",
                       payload.seq_num);
            } else {
              ESP_LOGE(TAG, "Failed to store sensor data: %s",
                       esp_err_to_name(ret));
            }
          } else {
            // Sequence number already stored, skip storage but still send
            ESP_LOGD(TAG,
                     "Skipping duplicate storage for seq=%lu (already stored)",
                     payload.seq_num);
          }
          // CRITICAL FIX: Actually send sensor payload via ESP-NOW to CH
          esp_err_t send_ret = esp_now_manager_send_data(
              ch_mac_to_use, (uint8_t *)&payload, sizeof(sensor_payload_t));
          if (send_ret == ESP_OK) {
            last_data_send = now_ms;
            ESP_LOGI(TAG,
                     "Sent sensor data via ESP-NOW to CH: seq=%lu, node_id=%lu",
                     payload.seq_num, payload.node_id);
          } else {
            ESP_LOGW(TAG, "ESP-NOW send failed: %s (seq=%lu)",
                     esp_err_to_name(send_ret), payload.seq_num);
          }
        } else {
          ESP_LOGW(TAG,
                   "CH MAC unknown (BLE off + cache empty), cannot send. "
                   "current_ch=%lu cached_ch=%lu",
                   current_ch, s_cached_ch_id);
        }
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
      if (s_current_phase == PHASE_DATA && in_slot && current_ch != 0) {
        uint8_t ch_mac[6];
        if (neighbor_manager_get_ch_mac(ch_mac)) {
          // Prepare the queue (Rename data.txt -> queue.txt)
          // Returns OK if renamed OR if queue already exists
          if (storage_manager_prepare_upload() == ESP_OK) {

            FILE *q = storage_manager_open_queue();
            if (q) {
              char history_line[256];
              int packets_sent = 0;
              int64_t cutoff_us = slot_end_us - 500000LL; // 500ms margin
              const int MAX_QUEUE_LINES_PER_CYCLE = 10;   // Cap per iteration

              // Drain Loop: Read line-by-line (capped per cycle to avoid WDT)
              while (packets_sent < MAX_QUEUE_LINES_PER_CYCLE &&
                     fgets(history_line, sizeof(history_line), q)) {
                // Check time budget
                if (esp_timer_get_time() >= cutoff_us) {
                  ESP_LOGW(TAG,
                           "OFFLOAD: Time budget exceeded, pausing queue.");
                  break;
                }

                // Strip newline
                size_t len = strlen(history_line);
                if (len > 0 && history_line[len - 1] == '\n') {
                  history_line[len - 1] = 0;
                  len--;
                }
                if (len == 0)
                  continue;

                // Send as Historical Data (Raw JSON string)
                esp_err_t ret = esp_now_manager_send_data(
                    ch_mac, (uint8_t *)history_line, len);

                if (ret == ESP_OK) {
                  packets_sent++;
                  // Fast throttle (20ms) to allow mesh processing
                  vTaskDelay(pdMS_TO_TICKS(20));
                } else {
                  ESP_LOGW(TAG, "OFFLOAD: Send failed, pausing queue.");
                  // Ideally we should seek back or retry, but for simplicity
                  // we prioritize not blocking. Data stays in queue.
                  break;
                }
              }

              bool uploading_done = feof(q);
              fclose(q);

              if (uploading_done) {
                // Queue fully processed, delete it
                storage_manager_remove_queue();
                ESP_LOGI(TAG, "OFFLOAD: Queue drained (%d items). Deleted.",
                         packets_sent);
              } else {
                ESP_LOGI(TAG,
                         "OFFLOAD: Partial drain (%d items). Queue retained.",
                         packets_sent);
              }

              // After plain-text queue drain, also attempt to drain MSLG
              // compressed chunks (DATA_FILE_COMPRESSED). These were written
              // by storage_manager_write_compressed(). We pop the oldest
              // chunk, decompress if needed, and attempt to send.
              {
                int comp_sent = 0;
                const int MAX_MSLG_BURST = 8; // limit per slot to avoid blocking
                while (esp_timer_get_time() < cutoff_us &&
                       comp_sent < MAX_MSLG_BURST) {
                  uint8_t *chunk_payload = NULL;
                  size_t chunk_len = 0;
                  uint32_t raw_len = 0;
                  uint8_t algo = 0;
                  uint32_t ts = 0;

                  if (storage_manager_pop_mslg_chunk(&chunk_payload, &chunk_len,
                                                     &raw_len, &algo, &ts) !=
                      ESP_OK) {
                    break; // no more compressed chunks
                  }

                  uint8_t *send_buf = chunk_payload;
                  size_t send_len = chunk_len;
                  uint8_t *decomp_buf = NULL;

                  if (algo == 1) {
                    // Decompress into RAM before sending (we send raw/uncompressed)
                    decomp_buf = heap_caps_malloc(raw_len, MALLOC_CAP_8BIT);
                    if (!decomp_buf) {
                      ESP_LOGW(TAG, "OFFLOAD: No memory to decompress chunk, requeueing");
                      // Re-append by re-storing raw JSON (best-effort)
                      // Attempt to decompress into small buffer first
                      heap_caps_free(chunk_payload);
                      break;
                    }

                    size_t out_len = raw_len;
                    comp_stats_t stats = {0};
                    esp_err_t derr = lz_decompress_miniz(chunk_payload, chunk_len,
                                                         decomp_buf, raw_len, &out_len,
                                                         &stats);
                    if (derr != ESP_OK || out_len != raw_len) {
                      ESP_LOGW(TAG, "OFFLOAD: Decompression failed, requeueing");
                      heap_caps_free(decomp_buf);
                      // Requeue: write decompressed as raw would be ideal but
                      // decompression failed so we drop this iteration and reappend
                      // the compressed blob by writing it back as-is (best-effort
                      // using write_compressed is not possible without raw JSON).
                      // To avoid data loss we append original compressed payload
                      // by calling storage_manager_write_compressed on a
                      // best-effort fallback: write raw chunk as a hex string
                      // (not ideal). For now we re-append by writing raw payload
                      // as a fallback.
                      storage_manager_write_compressed((const char *)chunk_payload, false);
                      heap_caps_free(chunk_payload);
                      break;
                    }

                    // Use decompressed buffer for sending
                    send_buf = decomp_buf;
                    send_len = out_len;
                    // free compressed payload (we hold decompressed)
                    heap_caps_free(chunk_payload);
                  }

                  esp_err_t send_ret = esp_now_manager_send_data(ch_mac, send_buf, send_len);
                  if (send_ret == ESP_OK) {
                    comp_sent++;
                    // small throttle to yield to other tasks
                    vTaskDelay(pdMS_TO_TICKS(20));
                    if (decomp_buf) {
                      heap_caps_free(decomp_buf);
                    } else {
                      heap_caps_free(chunk_payload);
                    }
                  } else {
                    ESP_LOGW(TAG, "OFFLOAD: Compressed chunk send failed, requeueing");
                    // Re-append the payload to storage for retry later.
                    if (send_buf && send_len > 0) {
                      // Write back (will compress again if enabled)
                      storage_manager_write_compressed((const char *)send_buf, true);
                    }
                    if (decomp_buf) heap_caps_free(decomp_buf);
                    break; // stop draining this slot
                  }
                } // while draining compressed
                if (comp_sent > 0) {
                  ESP_LOGI(TAG, "OFFLOAD: Compressed drain sent %d packets", comp_sent);
                }
              }

            } // if(q)
          } // if(prepare)
        }
      }

    }
    break;

  case STATE_UAV_ONBOARDING:
    // UAV Interaction Phase (run in separate task with timeout so state machine does not block forever)
    {
      ESP_LOGI(TAG, "Starting UAV Onboarding Sequence (timeout %d s)...",
               (int)(UAV_ONBOARDING_TIMEOUT_MS / 1000));

      ble_manager_stop_scanning();

      if (s_onboarding_result_queue != NULL) {
        xQueueReset(s_onboarding_result_queue);
        if (xTaskCreate(onboarding_task, "uav_onboard", 8192, NULL,
                        tskIDLE_PRIORITY + 2, NULL) == pdPASS) {
          esp_err_t ret = ESP_ERR_TIMEOUT;
          if (xQueueReceive(s_onboarding_result_queue, &ret,
                            pdMS_TO_TICKS(UAV_ONBOARDING_TIMEOUT_MS)) == pdTRUE) {
            if (ret == ESP_OK) {
              ESP_LOGI(TAG, "UAV Onboarding SUCCESS");
            } else {
              ESP_LOGE(TAG, "UAV Onboarding FAILED: %s", esp_err_to_name(ret));
            }
          } else {
            ESP_LOGE(TAG, "UAV Onboarding TIMEOUT after %d s",
                     (int)(UAV_ONBOARDING_TIMEOUT_MS / 1000));
          }
        } else {
          ESP_LOGE(TAG, "UAV Onboarding task create failed");
        }
      } else {
        ESP_LOGE(TAG, "UAV Onboarding queue not available");
      }

      ESP_LOGI(TAG, "Returning to CH state");
      transition_to_state(STATE_CH);
      ble_manager_start_advertising();
    }
    break;

  case STATE_SLEEP:
    // Sleep state (for future implementation)
    break;
  }
}
