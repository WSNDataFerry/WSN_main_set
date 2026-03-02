# ms_node System Guide — A to Z (Concepts + Code)

This document teaches how the **ms_node** WSN firmware works from concepts to implementation, with direct references to the codebase. It is written for a student or new developer who needs to understand every major function and how the system fits together.

---

## Part 1: Concepts You Need First

### 1.1 Wireless Sensor Network (WSN)

A **WSN** is a set of nodes that:
- Sense the environment (temperature, humidity, gas, etc.).
- Communicate over wireless links.
- Often run on batteries, so energy and role (who does more work) matter.

**In our system:** Each ESP32-S3 node runs the same firmware. Nodes form one **cluster**. One node is elected **Cluster Head (CH)**; the rest are **members**. Members send sensor data to the CH; the CH can aggregate and/or forward data (e.g. to a UAV or gateway).

### 1.2 Cluster and Cluster Head (CH)

- **Cluster:** A group of nodes that share the same **cluster key** (for authentication) and cooperate under one **CH**.
- **CH (Cluster Head):** The node that:
  - Announces itself over BLE so others can find it.
  - Builds a **TDMA schedule** (who sends when) for the DATA phase.
  - Receives sensor data from members over **ESP-NOW**.
  - Can run UAV onboarding and store/forward data.

**Concept:** Only one CH per cluster at a time. Who becomes CH is decided by an **election** based on battery, trust, link quality, and (in STELLAR) Pareto/Nash scoring.

### 1.3 STELLAR Algorithm (High Level)

**STELLAR** is the election algorithm used when `USE_STELLAR_ALGORITHM` is 1 (`config.h`):

1. Each node has **metrics:** battery, uptime, trust, link quality.
2. These are turned into **utility values** (non-linear functions).
3. A **Pareto frontier** is computed: candidates that are not “dominated” by anyone else (no one is better in every dimension).
4. **Nash Bargaining** picks one candidate from the Pareto set (fair compromise).
5. Tie-break is **lowest node_id** so all nodes pick the same CH.

**Code:** `main/election.c` — `election_run_stellar()`, `compute_pareto_frontier()`, `nash_bargaining_selection()`.

### 1.4 Superframe: STELLAR Phase vs DATA Phase

Time is divided into a repeating **superframe** of two phases (`config.h`):

- **STELLAR phase** (`STELLAR_PHASE_MS` = 20 s): Nodes exchange BLE beacons (metrics, CH flag), run elections if needed, and discover neighbors. BLE is active.
- **DATA phase** (`DATA_PHASE_MS` = 20 s): CH sends a **TDMA schedule** over ESP-NOW. Members send sensor data in their **time slots**. BLE can be off to avoid interfering with ESP-NOW.

**Concept:** Separate “who is CH” and “when do I send” (STELLAR) from “actually sending data” (DATA) so the radio is used efficiently and collisions are reduced.

**Code:** `main/state_machine.c` — `state_machine_update_phase()`, `s_current_phase` (PHASE_STELLAR / PHASE_DATA); `config.h` — `STELLAR_PHASE_MS`, `DATA_PHASE_MS`, `PHASE_GUARD_MS`.

### 1.5 TDMA (Time Division Multiple Access)

**Concept:** Each member gets a **slot** (e.g. 10 s). Only in that slot does it send sensor data to the CH. This avoids multiple members transmitting at once.

**In code:** CH builds `schedule_msg_t` with `epoch_us`, `slot_index`, `slot_duration_sec`, and sends it via `esp_now_manager_send_schedule_burst()`. Members receive it in `esp_now_manager.c` `process_packet()`, then `state_machine_sync_phase_from_epoch()` aligns their phase. In MEMBER state, the node checks “am I in my slot?” and only then calls `esp_now_manager_send_data()`.

**Config:** `SLOT_DURATION_SEC` (10), `MAX_CLUSTER_SIZE` (5).

---

## Part 2: System Overview and Entry Point

### 2.1 Where Everything Starts

**Entry:** `main/ms_node.c` — `app_main()` (around line 374).

Execution order:

1. **NVS** — `nvs_flash_init()` (and erase/reinit if needed). NVS stores cluster key, sensor config, uptime, trust.
2. **Auth** — `auth_init()` loads cluster key from NVS (or uses default). Must be before state machine so BLE beacons can be verified.
3. **Node identity** — `esp_read_mac(..., ESP_MAC_BT)`; `g_node_id` = lower 32 bits of MAC. Used everywhere (election, logs, payloads).
4. **Metrics** — `metrics_init()` initializes battery/trust/link/uptime and (if STELLAR) Lyapunov weights.
5. **Neighbor table** — `neighbor_manager_init()` allocates mutex and clears the table.
6. **Election** — `election_init()` creates `s_election_mutex` and sets election window to 0.
7. **Persistence** — `persistence_init()` mounts SPIFFS; `persistence_load_reputations()` loads saved trust for known nodes.
8. **BLE** — `ble_manager_init()` starts NimBLE stack and host task.
9. **LED** — `led_manager_init()` (non-fatal if RMT/LED strip fails).
10. **Logger / Storage** — `logger_init()`, `storage_manager_init()` for SPIFFS logs and MSLG.
11. **Network** — `esp_netif_init()`, `esp_event_loop_create_default()`, then `esp_now_manager_init()` (Wi‑Fi STA, ESP-NOW, send/recv callbacks).
12. **RF receiver** — `rf_receiver_init()` (optional; for UAV trigger).
13. **State machine** — `state_machine_init()` sets initial state to `STATE_INIT` and records entry time.
14. **Sensor config** — `sensor_config_load(&s_sensor_config)` from NVS.
15. **Battery / PME** — `battery_init(&bcfg)` (ADC for voltage divider on GPIO4), `pme_init(&cfg)` (energy management thresholds).
16. **Tasks** — `xTaskCreate(state_machine_task, ...)`, `xTaskCreate(metrics_task, ...)`, `xTaskCreate(console_config_task, ...)`.
17. **I2C and sensors** — `ms_i2c_init()`, then BME280, AHT21, ENS160, GY-271, INA219, INMP441 init with retries. Failures are non-fatal; main loop uses dummy values for missing sensors.

**Why this order:** Auth and node ID before BLE/state machine so beacons are verifiable. Battery/PME before metrics task so battery % is valid. State machine last so all subsystems it depends on are ready.

### 2.2 Main Loop vs Tasks

- **`state_machine_task`** (priority 5): Calls `state_machine_run()` every 100 ms. Handles DISCOVER, CANDIDATE, CH, MEMBER, UAV_ONBOARDING, phase, schedule send, data send, history offload.
- **`metrics_task`** (priority 4): Calls `metrics_update()` every 1 s. Updates battery, trust, link quality, STELLAR score.
- **`console_config_task`** (priority 1): Reads serial for CONFIG/CLUSTER/TRIGGER_UAV and applies config or forces UAV test.
- **Main loop** (after `app_main`): Reads battery (and mock path), updates PME, runs sensor reads, builds `sensor_payload_t`, calls `metrics_set_sensor_data()`, does logger/storage work, then **sleeps** for `state_machine_get_sleep_time_ms()` with WDT feed every 1 s.

**Code:** `ms_node.c` — `state_machine_task` (348), `metrics_task` (358), main loop (battery block ~621, sensor block ~748, sleep ~634).

---

## Part 3: Configuration Constants

**File:** `main/config.h`

Key constants and their role:

| Constant | Typical value | Purpose |
|----------|----------------|---------|
| `CLUSTER_KEY_SIZE` | 32 | Bytes for HMAC key (NVS + BLE). |
| `MAX_NEIGHBORS` | 10 | Max entries in neighbor table. |
| `ELECTION_WINDOW_MS` | 10000 | Seconds in CANDIDATE before running election. |
| `STELLAR_PHASE_MS` | 20000 | Length of STELLAR phase (ms). |
| `DATA_PHASE_MS` | 20000 | Length of DATA phase (ms). |
| `CH_BEACON_TIMEOUT_MS` | 5000 | CH beacon older than this → not valid CH. |
| `CH_MISS_THRESHOLD` | 5 | Consecutive misses before MEMBER declares CH lost. |
| `TRUST_FLOOR` | 0.2f | Min trust for valid CH and election candidate. |
| `BATTERY_LOW_THRESHOLD` | 0.2f | Below this, CH may yield if healthier node exists. |
| `LINK_QUALITY_FLOOR` | 0.2f | Below this → re-election. |
| `CLUSTER_RADIUS_RSSI_THRESHOLD` | -85.0f | Neighbor with RSSI better than this is “in cluster”. |
| `ENABLE_MOCK_SENSORS` | 1 | Allow dummy sensor values when HW fails. |
| `ENABLE_MOCK_BATTERY` | 0 | 1 = use 75% mock when ADC invalid; 0 = USB 100% fallback. |
| `USE_STELLAR_ALGORITHM` | 1 | 1 = STELLAR election; 0 = legacy score sort. |

---

## Part 4: Authentication and Security

**File:** `main/auth.c`

### 4.1 Cluster Key

- **Concept:** All nodes in the same cluster must share a **secret key** so BLE beacons can be authenticated (HMAC) and replays rejected.
- **Code:** `g_cluster_key[CLUSTER_KEY_SIZE]` is loaded in `auth_init()` from NVS namespace `"auth"`, key `"cluster_key"`. If missing or invalid, `s_default_cluster_key` is used (dev only). `auth_provision_cluster_key()` writes a new key to NVS and updates `g_cluster_key`.

### 4.2 HMAC

- **Concept:** Integrity of BLE advertisement: the sender computes HMAC-SHA256 over the payload (with cluster key), truncates to 1 byte for BLE space; the receiver recomputes and compares.
- **Code:** `auth_generate_hmac(message, msg_len, key, hmac_out)` — mbedtls HMAC-SHA256. `auth_verify_hmac(...)` — recompute and constant-time compare. BLE uses 1-byte truncation in `ble_manager.c` (build and verify with `g_cluster_key`).

### 4.3 Replay Protection

- **Concept:** Same packet (same node_id + timestamp) must not be accepted twice.
- **Code:** `auth_check_replay(timestamp, node_id)` — mutex-protected table of `(node_id, last_timestamp)`. If timestamp is within ±REPLAY_WINDOW_MS and not already seen for that node, accept and update; else reject. Table evicts oldest when full (FIFO).

---

## Part 5: Node Identity and Metrics

### 5.1 Node ID and MAC

- **Concept:** Each node has a unique identity. We use the BLE MAC (6 bytes) and take the lower 32 bits as `g_node_id` so it fits in logs and payloads.
- **Code:** `ms_node.c` — in `app_main()`, `esp_read_mac(mac_init, ESP_MAC_BT)` then `g_node_id = (uint32_t)(g_mac_addr & 0xFFFFFFFF)`. Declared in `state_machine.c` (and used in election, neighbor_manager, payloads).

### 5.2 Metrics (Battery, Trust, Link Quality, Uptime)

**File:** `main/metrics.c`

- **Concept:** The election and CH logic need a single “score” per node. That score is built from battery, uptime, trust, and link quality. STELLAR uses utilities and Pareto/Nash; legacy uses a weighted sum.
- **Battery:** `metrics_read_battery()` returns `pme_get_batt_pct()/100.0f`. If 0% (USB/mock), returns 1.0f so we don’t trigger re-election. Real battery comes from `pme_set_batt_pct(batt_pct)` fed by `ms_node.c` after `battery_read()`.
- **Uptime:** Persisted in NVS; incremented periodically in `metrics_update()`.
- **Trust:** From NVS or default; updated by `metrics_record_hmac_success()` and BLE/ESP-NOW success/failure; persisted periodically.
- **Link quality:** Derived from RSSI EWMA and PER (packet error rate) in `metrics_update_link_quality()`.
- **STELLAR score:** In `metrics_update()`, raw STELLAR score is computed then **smoothed** with EWMA (`STELLAR_SCORE_EWMA_ALPHA`) so brief dips don’t trigger re-election. Stored in `current_metrics.stellar_score` and `composite_score`.

**Code:** `metrics_init()`, `metrics_update()`, `metrics_get_current()`, `metrics_set_sensor_data()` / `metrics_get_sensor_data()` (for the payload the state machine sends to CH).

### 5.3 Battery Read and Mock

**File:** `main/ms_node.c` (battery block ~621), `components/battery/battery.c`

- **Concept:** We need a battery voltage (or a safe default) to feed PME and thus metrics. If no battery is connected (e.g. USB only), we use a fixed value so the node can still be CH.
- **Flow:**  
  1. If `ENABLE_MOCK_BATTERY` → force mock path.  
  2. Else call `battery_read(&vadc_mv, &vbat_mv, &batt_pct)`.  
  3. If `vadc_mv > 0 && vbat_mv > 2000 && vbat_mv <= 5000` → real battery: `pme_set_batt_pct(batt_pct)`, `s_battery_real = true`.  
  4. Else: if `ENABLE_MOCK_BATTERY` → `batt_pct = 75`, `vbat_mv = 3975`, log `[MOCK] Battery: 75%`; else USB fallback 100%, 5000 mV. Then `pme_set_batt_pct(batt_pct)`.

**Battery driver:** `battery_init()` configures ADC (e.g. ADC1_CH3, 220k/100k divider); `battery_read()` samples N times, converts to mV (with calibration if available), then VBAT = Vadc×(R1+R2)/R2, and `pct_from_vbat_mv()` maps 3300–4200 mV to 0–100%.

---

## Part 6: Neighbor Discovery (BLE and Neighbor Table)

### 6.1 BLE Role

**Concept:** Nodes discover each other and exchange metrics (score, battery, trust, CH flag) via **BLE advertising and scanning**. No connection; just manufacturer data in ad packets. Same channel as WiFi/ESP-NOW is avoided during ESP-NOW send (BLE quiet window).

**File:** `main/ble_manager.c`

- **Init:** `ble_manager_init()` — NimBLE init, host task, GAP callback. Sets `ble_ready` when stack is up.
- **Advertising:** `ble_manager_start_advertising()`, `ble_manager_stop_advertising()`. Advertisement carries a custom **manufacturer data** block.
- **Scanning:** `ble_manager_start_scanning()`, `ble_manager_stop_scanning()`.
- **Update ad:** `ble_manager_update_advertisement()` builds `ble_score_packet_t`: node_id, score, battery, trust, link_quality, uptime, is_ch, seq_num, and 1-byte HMAC over the payload using `g_cluster_key`. This packet is what other nodes see when they scan.

**Receiving:** In the scan callback, we parse manufacturer data. If it matches our company ID and length, we copy to `ble_score_packet_t`, verify HMAC (1 byte), check replay, then call `neighbor_manager_update(...)` with the received fields. We also call `metrics_record_ble_reception()` and `metrics_record_hmac_success()` for link/trust.

### 6.2 Neighbor Table

**File:** `main/neighbor_manager.c`, `main/neighbor_manager.h`

- **Concept:** Each node keeps a table of **neighbors** (nodes it has heard via BLE or inferred from ESP-NOW). Each entry has: node_id, MAC, score, battery, uptime, trust, link_quality, rssi_ewma, last_seen, is_ch, ch_announce_timestamp, verified (HMAC ok), last_seq_num.
- **Update:** `neighbor_manager_update(node_id, mac, rssi, score, battery, uptime, trust, link_quality, is_ch, seq_num)` — insert or update entry, refresh RSSI EWMA and last_seen. If the node is new, `persistence_get_initial_trust()` can set initial trust from SPIFFS.
- **Current CH:** `neighbor_manager_get_current_ch()` returns the node_id of the neighbor that is CH, **verified**, **trust ≥ TRUST_FLOOR**, and whose CH beacon is **younger than CH_BEACON_TIMEOUT_MS**. If several qualify, the one with **highest score** is returned.
- **CH MAC:** `neighbor_manager_get_ch_mac(mac_out)` returns the MAC of that same CH (for ESP-NOW send).
- **In cluster:** `neighbor_manager_is_in_cluster(neighbor)` is true if `neighbor->rssi_ewma >= CLUSTER_RADIUS_RSSI_THRESHOLD` (-85 dBm).
- **Cleanup:** `neighbor_manager_cleanup_stale()` removes entries older than `NEIGHBOR_TIMEOUT_MS`.

---

## Part 7: Election (Who Becomes CH)

**File:** `main/election.c`

### 7.1 When Election Runs

- **Concept:** Election runs only in **STATE_CANDIDATE**, after the node has been a candidate for **ELECTION_WINDOW_MS** (10 s). So nodes collect BLE beacons for 10 s, then everyone runs the same deterministic algorithm and picks one winner.
- **Code:** `state_machine.c` — in CANDIDATE, `election_get_window_start()`, then `if (now_ms - window_start >= ELECTION_WINDOW_MS)` → `election_run()`. Window is reset when entering CANDIDATE via `election_reset_window()`.

### 7.2 election_run()

- **Mutex:** `election_run()` takes `s_election_mutex` so two tasks don’t run it at once.
- **Algorithm:** If `USE_STELLAR_ALGORITHM` → `election_run_stellar()`, else `election_run_legacy()`.
- **STELLAR path (conceptual):**  
  1. Build candidate list: self + neighbors that are in cluster, verified, trust ≥ TRUST_FLOOR.  
  2. Compute utility (battery, uptime, trust, link_quality) per candidate.  
  3. Compute Pareto frontier (non-dominated set).  
  4. Nash Bargaining on Pareto set → winner.  
  5. Fallbacks: max STELLAR score on Pareto, then max over all.  
  6. Tie-break: **lowest node_id**.
- **Return:** Winner’s node_id. 0 if no valid candidate.
- **Code:** `election_run_stellar()` (candidates, Pareto, Nash), `election_run_legacy()` (sort by score then link_quality, battery, trust, node_id; winner = first).

### 7.3 election_check_reelection_needed()

- **Concept:** Even after someone is CH, we may need to **re-elect** (e.g. CH battery low, or two nodes both claiming CH). This function returns true if the current situation is “invalid” so the state machine can transition CH→MEMBER or CH→CANDIDATE, or MEMBER→CANDIDATE.
- **When I am CH:** Return true if: my battery &lt; BATTERY_LOW and a healthier verified neighbor exists; or my trust &lt; TRUST_FLOOR; or my link_quality &lt; LINK_QUALITY_FLOOR; or another verified node is CH and has higher score (or same score but lower node_id) — **yield**.
- **When I am MEMBER:** Return true if: no valid CH (`get_current_ch()==0`); or CH entry missing; or CH battery low and a healthier node exists; or CH trust/link_quality below floor.
- **Code:** `election.c` — `election_check_reelection_needed()` (uses `metrics_get_current()`, `neighbor_manager_get_all()`, `neighbor_manager_get_current_ch()`, `neighbor_manager_get()`).

---

## Part 8: State Machine (States and Transitions)

**File:** `main/state_machine.c`, `main/state_machine.h`

### 8.1 States (node_state_t)

- **STATE_INIT:** Boot. After 2 s → DISCOVER.
- **STATE_DISCOVER:** BLE advertise + scan for 5 s. If `neighbor_manager_get_current_ch() != 0` (after ≥2 s or at end) → MEMBER. Else → CANDIDATE, `election_reset_window()`.
- **STATE_CANDIDATE:** Advertise + scan; refresh ad every 1 s. After ELECTION_WINDOW_MS → `election_run()`. If winner == self → CH; if winner != 0 → MEMBER; if winner == 0 → DISCOVER.
- **STATE_CH:** Advertise CH, scan (to detect conflicts). Each run: `election_check_reelection_needed()`. If true and (phase STELLAR or other_ch exists): if other_ch → yield to MEMBER; else → CANDIDATE and reset window. Else: build TDMA schedule, `esp_now_manager_send_schedule_burst()`, store own sensor data every 5 s. UAV trigger → UAV_ONBOARDING.
- **STATE_MEMBER:** Start BLE if not already. Check CH: if `get_current_ch()==0` for CH_MISS_THRESHOLD consecutive runs → CANDIDATE. In STELLAR phase, if `election_check_reelection_needed()` → CANDIDATE. Cache CH MAC in STELLAR for DATA phase. In DATA phase, if in TDMA slot or no-schedule fallback: get payload from `metrics_get_sensor_data()`, store locally (store-first), then `esp_now_manager_send_data(ch_mac, &payload, sizeof(sensor_payload_t))`. Also drain history queue (offload to CH) with time and packet limits.
- **STATE_UAV_ONBOARDING:** Run `uav_client_run_onboarding()` in a task; state machine waits on a queue with timeout (e.g. 5 min). On completion or timeout → back to CH.
- **STATE_SLEEP:** Placeholder for future deep-sleep logic.

### 8.2 Phase (STELLAR vs DATA)

- **state_machine_update_phase(now_ms):** Elapsed time since `s_phase_start_ms` drives phase. If elapsed &lt; STELLAR_PHASE_MS → PHASE_STELLAR; else if elapsed &lt; STELLAR_PHASE_MS + DATA_PHASE_MS → PHASE_DATA; else wrap and reset `s_phase_start_ms`.
- **state_machine_sync_phase_from_epoch(epoch_us):** Called when we receive a schedule from CH. Aligns our `s_phase_start_ms` so we enter DATA phase in sync with the CH (so TDMA slots line up).

### 8.3 transition_to_state()

- **Code:** `transition_to_state(new_state)` — if state unchanged, return. Log transition. Set `state_entry_time = now_ms`, `g_current_state = new_state`. If new_state is CH → `g_is_ch = true`; if not MEMBER → `g_is_ch = false`. Call `led_manager_set_state(new_state)`.

---

## Part 9: Data Path (Sensors → Storage → CH)

### 9.1 Building the Payload (Main Loop)

**File:** `main/ms_node.c` (sensor and payload block ~748–1046)

- **Concept:** The main loop reads sensors (or uses dummy values for missing ones), builds a **sensor_payload_t** (node_id, timestamp, seq, mac, temp, humidity, pressure, AQI, TVOC, eCO2, mag, audio_rms, flags), and passes it to metrics so the state machine can send it to the CH.
- **Flow:** For each sensor type (BME280, AHT21, ENS160, GY-271, INA219, INMP441), if enabled and timing allows: read; on failure fill dummy and set `ok_xxx = true` anyway. Set `real_xxx = ok_xxx` (real means read succeeded). Then `s_sensors_real = real_bme || real_aht || ...`. Build `sensor_payload_t`: set node_id, timestamp_ms, seq_num, mac_addr, flags (SENSOR_PAYLOAD_FLAG_SENSORS_REAL if s_sensors_real). Fill fields from sensor structs where ok_xxx; for missing use **MOCK DATA FALLBACK** (e.g. random temp/humidity if !ok_bme&&!ok_aht). Call `metrics_set_sensor_data(&payload)`.
- **Storage:** Same loop also writes a JSON log line to logger (change detection) and optionally stores to SPIFFS. Member state later does “store-first” then send.

### 9.2 Sending to CH (MEMBER State)

**File:** `main/state_machine.c` (MEMBER block)

- **When:** Only in **PHASE_DATA**. Either we have a valid TDMA schedule and we’re in our slot (with 2 s gap since last send), or no schedule (no-schedule fallback) and we send periodically (e.g. every 5 s) or once on DATA entry.
- **How:** `metrics_get_sensor_data(&payload)`. If timestamp 0, fix with current time. Optionally store to local storage (store-first, dedup by seq). Then `esp_now_manager_send_data(ch_mac_to_use, (uint8_t*)&payload, sizeof(sensor_payload_t))`. CH MAC comes from `neighbor_manager_get_ch_mac()` in STELLAR, or from cached `s_cached_ch_mac` in DATA when BLE is off.

### 9.3 Receiving on CH (ESP-NOW)

**File:** `main/esp_now_manager.c`

- **Recv callback:** `esp_now_recv_cb()` receives raw buffer. If first byte is compression magic, decompress and call `process_packet()` on decompressed data; else call `process_packet()` directly.
- **process_packet():**
  - If `len == sizeof(schedule_msg_t)` and magic matches → update `g_current_schedule` (under mutex), call `state_machine_sync_phase_from_epoch(sched->epoch_us)`.
  - If `len == sizeof(sensor_payload_t)` → log “RX Sensor Data from node_X”, write JSON line to storage via `storage_manager_write_compressed()`, and `neighbor_manager_update_trust(node_id, true)`.
  - If data starts with `'{'` → historical JSON: copy to static buffer, store, update trust.
- **Send:** `esp_now_manager_send_data(peer_addr, data, len)` — take send semaphore, stop BLE (quiet window 120 ms), set WiFi channel, wait for previous send done, call `esp_now_send()`, wait for send callback, restore BLE, give semaphore.

---

## Part 10: Persistence and Storage

### 10.1 Persistence (Reputation / Trust)

**File:** `main/persistence.c`

- **Concept:** Trust values for neighbors can be saved so after reboot we don’t start from zero for known nodes.
- **persistence_init():** Mount SPIFFS (format if needed).
- **persistence_save_reputations(node_ids, trusts, count):** Write to `SPIFFS_BASE_PATH "/reputation.txt"` as “node_id trust” lines. Called from main loop periodically with current neighbors.
- **persistence_load_reputations():** Read file into in-memory cache (e.g. up to 32 entries).
- **persistence_get_initial_trust(node_id, &trust):** Return trust from cache when adding a new neighbor so `neighbor_manager_set_trust_value()` can set initial trust.

### 10.2 Storage Manager (SPIFFS, MSLG, Compression)

**File:** `main/storage_manager.c` (and logger)

- **Concept:** Logs and sensor JSON are written to SPIFFS. “Store-first” means member writes locally then sends same data to CH. CH writes received JSON and its own sensor data. Compression (e.g. LZ) can be used for larger blocks. MSLG is a log format; offload sends chunks to CH via ESP-NOW.

---

## Part 11: UAV Onboarding, LED, Console

### 11.1 UAV Onboarding

- **Concept:** When the CH detects an RF trigger or receives TRIGGER_UAV (serial), it goes to **STATE_UAV_ONBOARDING** and runs provisioning (e.g. Wi‑Fi to UAV). It must not block forever.
- **Code:** `state_machine.c` — in CH, check `rf_receiver_check_trigger()` or console TRIGGER_UAV; transition to UAV_ONBOARDING. An **onboarding task** runs `uav_client_run_onboarding()`. The state machine waits on a queue with **UAV_ONBOARDING_TIMEOUT_MS** (e.g. 5 min). On success or timeout, transition back to CH.

### 11.2 LED Manager

**File:** `main/led_manager.c`

- **Concept:** One LED indicates state: INIT/DISCOVER/CANDIDATE = blinking white; CH = solid blue; MEMBER = blinking green. If LED strip init fails (e.g. no RMT), `led_strip` is NULL and the task skips strip operations.
- **Code:** `led_manager_set_state(state)` sets pending state; LED task applies it after debounce and drives the strip (or no-op if NULL).

### 11.3 Console (Serial Commands)

**File:** `main/ms_node.c` — `console_config_task()`

- **Concept:** Serial input can change config or trigger UAV test. Commands like CONFIG key=value, CLUSTER (print report), TRIGGER_UAV.
- **Code:** Reads line (e.g. 128 bytes), parses, calls `apply_config_key_value()` or `cluster_report_print()` or `state_machine_force_uav_test()`.

---

## Part 12: End-to-End Flow Summary

1. **Boot** → INIT (2 s) → DISCOVER (5 s). BLE ad/scan; if existing CH found → MEMBER; else → CANDIDATE.
2. **CANDIDATE** (10 s) → election_run() → CH or MEMBER or DISCOVER.
3. **CH:** Announce CH on BLE, build schedule, send schedule at DATA start, receive sensor data over ESP-NOW, store. Re-election check: yield or step down if needed.
4. **MEMBER:** Hear CH on BLE, cache CH MAC. In DATA phase, in slot (or fallback): get payload from metrics, store locally, send to CH via ESP-NOW. Re-election check: if CH bad or lost → CANDIDATE.
5. **Superframe:** STELLAR phase (20 s) — beacons, elections. DATA phase (20 s) — TDMA send to CH. Phase sync via schedule from CH.
6. **Sensors:** Main loop reads (or mock), builds sensor_payload_t, metrics_set_sensor_data(). Battery: battery_read() or mock 75% / USB 100% → PME → metrics.
7. **Auth:** Cluster key from NVS; BLE ad verified with 1-byte HMAC; replay table protected by mutex.

---

## Quick Reference: Key Files and Functions

| Concept | File | Key functions |
|--------|------|----------------|
| Entry, tasks, sensors, battery | `main/ms_node.c` | `app_main()`, `state_machine_task()`, `metrics_task()`, battery block, sensor + `metrics_set_sensor_data()` |
| States, phase, CH/MEMBER duties | `main/state_machine.c` | `state_machine_run()`, `transition_to_state()`, `state_machine_update_phase()`, `state_machine_sync_phase_from_epoch()`, `state_machine_get_sleep_time_ms()` |
| Election (STELLAR / legacy) | `main/election.c` | `election_run()`, `election_run_stellar()`, `election_check_reelection_needed()`, `election_reset_window()` |
| BLE beacons | `main/ble_manager.c` | `ble_manager_init()`, `ble_manager_start_advertising()`, `ble_manager_update_advertisement()`, scan callback → `neighbor_manager_update()` |
| Neighbor table, current CH | `main/neighbor_manager.c` | `neighbor_manager_update()`, `neighbor_manager_get_current_ch()`, `neighbor_manager_get_ch_mac()`, `neighbor_manager_is_in_cluster()` |
| ESP-NOW send/recv | `main/esp_now_manager.c` | `esp_now_manager_init()`, `esp_now_manager_send_data()`, `esp_now_manager_send_schedule_burst()`, `esp_now_recv_cb()` → `process_packet()` |
| Metrics, score, battery | `main/metrics.c` | `metrics_init()`, `metrics_update()`, `metrics_get_current()`, `metrics_read_battery()`, `metrics_set_sensor_data()`, `metrics_get_sensor_data()` |
| Auth, HMAC, replay | `main/auth.c` | `auth_init()`, `auth_generate_hmac()`, `auth_verify_hmac()`, `auth_check_replay()`, `auth_provision_cluster_key()` |
| Config constants | `main/config.h` | All `#define`s for phases, thresholds, algorithm toggles |
| Battery ADC | `components/battery/battery.c` | `battery_init()`, `battery_read()`, `pct_from_vbat_mv()` |
| Persistence (trust) | `main/persistence.c` | `persistence_init()`, `persistence_save_reputations()`, `persistence_load_reputations()`, `persistence_get_initial_trust()` |

---

This document and the referenced source files together give an A–Z picture of how each concept is implemented and how the functions interact.
