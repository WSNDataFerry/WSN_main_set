# ms_node — Senior Embedded Systems Code & Architecture Analysis

**Document type:** Line-by-line code and system architecture review  
**Role:** Senior Embedded Systems Engineer  
**Scope:** `WSN_main_set/ms_node` (ESP32-S3 WSN firmware with STELLAR clustering)  
**Date:** 2026-03-01  
**Fixes applied:** 2026-03-01 (see changelog below)  

---

## 1. Executive Summary

The ms_node firmware implements a battery-aware Wireless Sensor Network (WSN) with STELLAR clustering, BLE-based discovery, ESP-NOW data transfer, and TDMA time-slicing. The design is **architecturally sound** and shows awareness of radio coexistence, watchdog safety, and concurrency. Several **critical and high-priority issues** (security, build, config validation, concurrency, and resource use) should be addressed before production or long-term deployment.

| Category            | Verdict | Notes |
|---------------------|--------|--------|
| Architecture        | Good   | Clear separation; state machine + tasks; BLE/ESP-NOW coexistence handled |
| Concurrency         | Fair   | Mutexes used but recursion and init-order risks; replay table unprotected |
| Security            | Weak   | Hardcoded keys; 1-byte HMAC; no ESP-NOW encryption in use |
| Robustness / Safety | Fair   | Config not validated; duplicate CMake entry; long blocking paths |
| Maintainability     | Good   | Logging and comments generally helpful; some magic numbers |
| Resource / RT       | Fair   | Large stacks; heap in ISR path; chunked sleep and WDT present |

---

## 2. System Architecture Assessment

### 2.1 High-level view

- **Entry:** `app_main()` in `ms_node.c` — NVS, auth, metrics, neighbors, election, persistence, BLE, logger, storage, netif, ESP-NOW, RF receiver, then state machine. Sensor and PME/battery init follow. Order is mostly correct (e.g. auth before state machine, battery before metrics task).
- **Tasks:**
  - **state_machine_task** (100 ms) — state machine and phase/superframe logic.
  - **metrics_task** (1 s) — metrics update and status log.
  - **console_config_task** (low priority) — serial CONFIG/CLUSTER/TRIGGER_UAV.
  - **Main loop** — sensors, battery, PME, logging, `state_machine_get_sleep_time_ms()`-driven chunked sleep with WDT feed.
- **Data flow:** BLE beacons (scores, HMAC) → neighbor_manager + metrics; election (STELLAR or legacy) → CH/MEMBER; ESP-NOW for schedule + sensor/history payloads; SPIFFS/NVS for logs and persistence.

**Strengths:** Phase-based superframe (STELLAR / DATA), BLE quiet window before ESP-NOW send, TDMA slot alignment and no-schedule fallback, store-first then send for members.

**Gaps:** No explicit document of task priorities and stack sizes in one place; dependency order is in code/comments only.

### 2.2 Build and project structure

- **Critical:** `main/CMakeLists.txt` lists `persistence.c` **twice** (lines 13–14). This can cause duplicate symbols and undefined behavior. Remove the duplicate.
- **Optional:** `compression_bench.c` is still in SRCS; if unused, remove or guard with a config option.

---

## 3. Module-by-Module Code Review

### 3.1 `ms_node.c` (main application)

**Initialization order (lines 306–354)**  
- NVS → auth → MAC/node_id → metrics → neighbor_manager → election → persistence → BLE → LED → logger → storage_manager → netif/event → ESP-NOW → rf_receiver → state_machine. Sensible; state_machine last is correct.

**`apply_config_key_value()` (127–174)**  
- Uses fixed `buf[128]`, `strlen`/`strchr`/`strcmp`; no overflow if `len < sizeof(buf)`.
- **Issue:** No validation of values. `atoi(value)` for intervals can be negative or zero; stored as `uint32_t` (wraps). Example: `env_sensor_interval_ms=0` or `audio_interval_ms=-1` can cause div-by-zero or bizarre timing. **Recommendation:** Validate ranges (e.g. 100–86400000 ms for intervals) and reject invalid keys/values.

**`cluster_report_print()` (176–221)**  
- Uses `neighbor_manager_get_all(neighbors, MAX_NEIGHBORS)`; stack allocation `neighbor_entry_t neighbors[MAX_NEIGHBORS]` is acceptable (struct size × 10). No obvious overflows.

**Console task (223–257)**  
- `fgetc(stdin)` in a task: on ESP-IDF this is UART; blocking read is expected. Line buffer 128 bytes; `pos` guarded. Commands CONFIG/CLUSTER/TRIGGER_UAV are clear.

**Main loop**  
- Battery: mock path (`ENABLE_MOCK_SENSORS`) uses static `sim_batt`/`calls`; fine. USB fallback (100%, 5000 mV) when no battery is documented.
- PME mode drives sensor intervals (lines 458–471); intervals are fixed per mode (e.g. 60 s env in NORMAL). **Note:** `s_sensor_config.*_interval_ms` from NVS are loaded but the loop uses the **mode-based** intervals (458–471), not the config intervals for env/gas/mag/power/audio. So BLE/serial CONFIG changes to those intervals do **not** affect the main loop sampling; only `beacon_interval_ms`/`beacon_offset_ms` and sensor enables are used elsewhere. This is a **design/consistency bug**: either use config intervals in the loop or document that they are “display only”.
- Sensor reads: retries and dummy data when a sensor fails; `metrics_set_sensor_data()` and JSON log with change-detection are clear.
- Deep sleep (618–630): `logger_flush()` before `esp_deep_sleep_start()` is correct.
- Sleep (634–648): `state_machine_get_sleep_time_ms()` then chunked `vTaskDelay` with `esp_task_wdt_reset()` every 1 s is correct for WDT.

**Minor:**  
- Duplicate comment block at 176–177 (“Print cluster report…”).  
- `sample_period_ms_for_mode()` returns 2000 for `PME_MODE_CRITICAL` but critical mode uses deep sleep; the comment “Won’t be used” is correct but the branch could return 0 or a sentinel for clarity.

### 3.2 `state_machine.c` (state machine and superframe)

**Phase and sync**  
- `state_machine_update_phase()`: `s_phase_start_ms` and STELLAR/DATA lengths are consistent with `config.h`.  
- `state_machine_sync_phase_from_epoch()`: Correctly ignores CH-relative epoch and aligns member phase to DATA boundary (member_now_ms - STELLAR_PHASE_MS). Hysteresis (diff > 500 ms) avoids jitter.

**`state_machine_get_sleep_time_ms()` (40–72)**  
- Schedule validity uses `epoch_us > (now_us - (SLOT_DURATION_SEC * 10000000LL))`. `SLOT_DURATION_SEC` is 10; 10*10000000 = 100e6 µs = 100 s. So “recent” means within 100 s; if slot_duration_sec is smaller (e.g. 2 s from dynamic slot calculation), the schedule can still be considered valid for 100 s. Consider basing “recent” on actual slot_duration_sec or DATA_PHASE_MS.  
- Cast `(uint32_t)(time_to_slot / 1000)` can overflow if time_to_slot is very large positive; cap to a max sleep (e.g. 60000 ms) to avoid unexpectedly long sleeps.

**DISCOVER (291–349)**  
- `last_update` and `last_candidate_update` are **function-level static**; they persist across invocations and can drift across state re-entries. Prefer resetting on state entry (e.g. in `transition_to_state`) or using state_entry_time for consistency.

**CH block (417–511)**  
- Schedule build: `slot_ms = available_ms / count`; clamp to minimum 2000 ms is good.  
- **Bug:** `esp_now_manager_send_data(neighbors[i].mac_addr, ...)` is called in a loop; each call does BLE stop → wait 120 ms → send. So for N members, N×120 ms plus send time: with 5 members this can exceed the DATA phase guard. Consider sending schedule in one burst with a single BLE quiet window, or shortening BLE_QUIET_MS where safe.  
- CH self-data storage (518–552): 5 s throttle; `storage_manager_write_compressed` return checked. Good.

**MEMBER block (506–898)**  
- CH cache (`s_cached_ch_mac`, `s_cached_ch_id`) for DATA phase when BLE is off is correct.  
- `last_data_send` and TDMA slot check: 2 s gap for “send once per slot” is reasonable.  
- **Risk:** `storage_manager_open_queue()` and `fgets` loop (794–878) plus compressed chunk drain (804–886): In one state_machine_run() iteration this can run for a large fraction of the slot. Comment mentions “MAX_BURST_PACKETS” and time budget (`cutoff_us`); the line-by-line drain has no explicit packet limit, only time. If the queue is large, the state machine task can block for a long time (e.g. hundreds of ms). Recommend a per-iteration packet cap (e.g. 5–10) in addition to time budget.  
- **Heap in callback context:** `storage_manager_pop_mslg_chunk` + `heap_caps_malloc` + `lz_decompress_miniz` run inside the state machine task (not ISR), so no ISR heap use; the risk is long run time and stack.  
- **Fallback (894–897):** `last_history_sync` is updated every run; the “Lazy single line sync” block is empty (comment only). Dead code or placeholder; remove or implement.

**UAV_ONBOARDING (901–923)**  
- `uav_client_run_onboarding()` is blocking; BLE stopped during onboarding. If onboarding hangs, the state machine task is stuck. Add a timeout or run onboarding in a separate task with a watchdog.

**STATE_SLEEP (925–927)**  
- Empty; documented as future. No issue.

### 3.3 `election.c` (STELLAR and legacy election)

**STELLAR path**  
- Pareto dominance, Nash bargaining (log-sum), centrality (RSSI variance), tie-break by node_id: implemented consistently.  
- `election_run_stellar()` builds candidates from self + neighbors; filters by `neighbor_manager_is_in_cluster` and `verified` and `trust >= TRUST_FLOOR`.  
- Fallbacks: Nash → max STELLAR on Pareto → max STELLAR overall. Good.  
- **Candidate array:** `candidates[MAX_NEIGHBORS + 1]` on stack; size is acceptable.

**`election_check_reelection_needed()`**  
- CH conflict resolution (434–469): compares score and node_id; logic is clear.  
- Multiple `neighbor_manager_get_all(neighbors, MAX_NEIGHBORS)` in one function: each take/give mutex; no deadlock.

**`election_run()` (393–408)**  
- `election_in_progress` flag prevents re-entrancy but is **not** mutex-protected. If two tasks ever called `election_run()` (e.g. state_machine and a hypothetical other), race is possible. Recommend a mutex or single-threaded guarantee (e.g. only state_machine_task calls it).

### 3.4 `metrics.c` (metrics and STELLAR weights)

**Mutex usage**  
- `metrics_mutex` is created in `metrics_init()`. All accessors check `if (metrics_mutex)` before take/give; if creation fails, code still runs without lock (data races). Recommendation: treat mutex creation failure as fatal or disable metrics updates until mutex exists.

**metrics_update()**  
- Calls `metrics_get_uptime()` and `metrics_get_stellar_weights()` (via variance/entropy/weight update). `metrics_get_uptime()` uses NVS (same partition as other metrics); no mutex there — acceptable if only one writer.  
- `metrics_update_stellar_weights()` and related take the same recursive mutex; no deadlock.

**metrics_read_battery()**  
- Uses `pme_get_batt_pct()`; 0% mapped to 1.0f to avoid re-election on USB. Documented.

**NVS usage**  
- Uptime and trust blobs; open/commit/close per call. No obvious leak; many small NVS writes can wear flash; 60 s period is reasonable.

### 3.5 `esp_now_manager.c` (ESP-NOW and schedule)

**Init**  
- Wi-Fi STA, no power save, channel set, ESP-NOW init, PMK set. Send semaphore (binary) and send-done semaphore created; sem given once so first send can proceed. Good.

**`process_packet()` (31–128)**  
- **Schedule:** Normalizes epoch to local time (`now_us - time_into_cycle`) so slot calculation is consistent. Then calls `state_machine_sync_phase_from_epoch(sched->epoch_us)` — epoch value is unused in sync (only “received” event matters); correct.  
- **Sensor payload:** Log and store; `neighbor_manager_update_trust` on success.  
- **Historical JSON (99–116):** `malloc(len + 1)` in receive path. Receive callback runs in Wi-Fi task context; **heap allocation in that context** can increase latency and risk fragmentation. Prefer a pool or static buffer for max payload (250 bytes), or copy to a queue and process in a lower-priority task.

**`esp_now_manager_send_data()` (239–368)**  
- BLE quiet window (stop scan/adv, 120 ms, channel set), semaphore wait, send, wait for send_cb, restore BLE. Retries and backoff. Well structured.  
- **Send callback (157–179):** Runs in Wi-Fi driver/ISR context. Only `xSemaphoreGive` and integer assign; no heap, no long work. Good.

**`esp_now_get_current_schedule()`**  
- Returns `g_current_schedule` by value; no lock. Schedule is written in recv_cb and read by state machine and main loop. Tearing possible on 32-bit (structure has int64_t and more). Recommend a single lock or atomic double-buffer for the schedule.

### 3.6 `ble_manager.c` (NimBLE and beacons)

**Packet structure**  
- `ble_score_packet_t`: comment says 17 bytes data but struct has `hmac[1]`; total 20 bytes. BLE manufacturer data limit is 26 bytes (31 - 3 AD header); 20 is fine.

**HMAC**  
- Only 1 byte used for BLE (space constraint). Collision probability 1/256 per packet; **security is weak** (forgery easy). Acceptable only for integrity-in-the-clear / lab use; document and consider at least 4 bytes or move to full HMAC over a separate GATT characteristic if needed for production.

**Duplicate filter**  
- `should_process_device(node_id, seq_num)`: 500 ms debounce and seq_num check; LRU eviction when table full. Prevents duplicate processing.

**Advertising update**  
- `ble_manager_update_advertisement()` updates `g_adv_packet` and sets `g_reload_adv_data`; host task or sync callback applies it. Ensure no tear when reading packet for HMAC message (e.g. copy once at start of build).

### 3.7 `neighbor_manager.c` (neighbor table)

- All table access under `neighbor_mutex` with timeouts (50–1000 ms). `get_all` uses 1000 ms timeout; state machine and metrics both call it — ensure no priority inversion (state_machine higher than BLE task that may call `neighbor_manager_update`).  
- PER calculation (seq wrap 0–255, missed > 20 clamped): correct.  
- `neighbor_manager_cleanup_stale()` compacts in place; preserves order. Good.

### 3.8 `auth.c` (HMAC and replay)

- **Cluster key:** `g_cluster_key[32]` is **hardcoded** in source. Critical for production: key must come from secure provisioning (e.g. NVS with flash encryption) or derivation.  
- **Replay:** `replay_table` and `replay_count` are **not** mutex-protected. Concurrent BLE callbacks could call `auth_check_replay()` and corrupt the table or allow replays. Add a mutex (or restrict replay check to a single task and document).  
- **Replay eviction:** FIFO eviction when full; new node overwrites oldest. Fine for 20 entries.

---

## 4. Concurrency and Real-Time

- **Shared state:**  
  - `g_current_state`, `g_is_ch`, `g_node_id`, `g_mac_addr`: read by main loop, metrics_task, state_machine_task, and BLE/ESP-NOW callbacks. No lock. Updates only in state_machine_task (transition_to_state). So one writer; multiple readers without atomic. On ESP32 32-bit, reading a 32-bit or 64-bit value can tear; for node_id/state this may be acceptable for robustness but not strictly race-free. Consider volatile or atomic for at least `g_current_state`/`g_is_ch`.  
  - `g_current_schedule`: written in ESP-NOW recv_cb, read in state machine and main; see recommendation above (lock or double-buffer).  
- **Metrics/neighbor mutexes:** Recursive mutex for metrics is consistent. Neighbor mutex timeouts avoid indefinite block; ensure no task holds neighbor_mutex while calling into metrics (to avoid deadlock with metrics_mutex).  
- **Election:** `election_in_progress` is not atomic; prefer mutex if any other caller is ever added.  
- **Task priorities:** state_machine 5, metrics 4, main loop default, console 1. State machine and metrics both use neighbor_manager and metrics; no circular dependency seen, but long critical sections in state_machine (queue drain) can delay metrics and main loop.

---

## 5. Security Summary

| Item | Severity | Location | Recommendation |
|------|----------|----------|-----------------|
| Hardcoded cluster key | Critical | auth.c | Provision from secure storage or derivation |
| 1-byte HMAC in BLE | High | ble_manager / auth | Document; increase to ≥4 bytes or use full HMAC elsewhere |
| ESP-NOW encrypt flag | Medium | esp_now_manager | register_peer(..., false); enable LMK and set encrypt=true if needed |
| Replay table not locked | Medium | auth.c | Add mutex or single-thread replay check |
| CONFIG from UART | Low | ms_node.c | Optional: restrict to debug build or require auth |

---

## 6. Resource and Robustness

- **Stack:** state_machine_task 12288 bytes; metrics 4096; main loop uses local arrays (e.g. 400-byte line, 384-byte json_payload). Monitor high-water marks.  
- **Heap:** ESP-NOW recv path `malloc(len+1)`; state_machine decompression `heap_caps_malloc(raw_len)`. Consider bounds on raw_len and a pool for recv.  
- **Watchdog:** Main loop uses chunked sleep and `esp_task_wdt_reset()`; state_machine and metrics run every 100 ms / 1 s. Long runs in state_machine (queue drain) can approach or exceed WDT period; cap iteration work.  
- **Config:** `s_sensor_config` reloaded every 15 main-loop iterations; no validation. Intervals not used in main loop (see 3.1).  
- **Sensor init:** Retries and graceful skip when sensors missing; main loop continues with dummy data. Good for robustness.

---

## 7. Recommendations (Prioritized)

### P0 (Critical)

1. **Remove duplicate `persistence.c`** from `main/CMakeLists.txt`.  
2. **Stop using a hardcoded cluster key** in production: load from NVS (with flash encryption) or derive from device identity.  
3. **Validate CONFIG values** in `apply_config_key_value()`: positive ranges for intervals, reject unknown keys explicitly.

### P1 (High)

4. **Protect replay table** in auth.c with a mutex (or guarantee single-threaded use).  
5. **Protect `g_current_schedule`** (or use a double-buffer) and document single writer/readers for `g_current_state`/`g_is_ch` (or make atomic/volatile).  
6. **Either use sensor interval config in the main loop** or document that env/gas/mag/power/audio intervals are mode-only and CONFIG only affects other params.  
7. **Cap sleep time** in `state_machine_get_sleep_time_ms()` (e.g. min 100 ms, max 60000 ms) to avoid overflow and excessive sleep.  
8. **Limit CH schedule send time**: one BLE quiet window for all schedule unicasts, or reduce BLE_QUIET_MS where safe, so DATA phase is not exceeded.

### P2 (Medium)

9. **Limit queue drain per state_machine_run()** (e.g. max 10 lines + 5 compressed chunks per run) in addition to time budget.  
10. **Replace malloc in ESP-NOW recv** with a static buffer or pool.  
11. **Add timeout or non-blocking path** for `uav_client_run_onboarding()`.  
12. **Election re-entrancy:** Guard `election_run()` with a mutex if more than one caller is possible.  
13. **Ensure metrics_mutex creation failure** is handled (abort or skip updates and log).

### P3 (Low / Cleanup)

14. Remove duplicate “Print cluster report” comment in ms_node.c.  
15. Implement or remove the empty “Lazy single line sync” block in MEMBER state.  
16. Consider basing schedule “validity” window on slot_duration_sec or DATA_PHASE_MS in `state_machine_get_sleep_time_ms()`.  
17. Reset DISCOVER/CANDIDATE “last_update” style statics on state entry for clarity.

---

## 8. Conclusion

The ms_node codebase is structured for a research/prototype WSN with STELLAR clustering and shows good attention to BLE/ESP-NOW coexistence, TDMA alignment, and watchdog safety. Addressing the **critical** build and security items and the **high** concurrency and config issues will make it suitable for more reliable and secure deployment. The above recommendations can be implemented incrementally and tracked (e.g. in a P0/P1/P2 backlog).

---

## 9. Changelog (Fixes Applied 2026-03-01)

| Issue | Fix |
|-------|-----|
| P0: Duplicate persistence.c | Removed from CMakeLists.txt |
| P0: Config validation | Added range checks in apply_config_key_value, reject negative/invalid |
| P0: Hardcoded cluster key | Load from NVS (auth namespace), fallback to default; added auth_provision_cluster_key() |
| P1: Replay table race | Added s_replay_mutex in auth.c |
| P1: g_current_schedule race | Added s_schedule_mutex, protected in esp_now_get_current_schedule and process_packet |
| P1: Sensor intervals | Main loop uses s_sensor_config when in range [1s,24h], else mode default |
| P1: Sleep overflow | Capped state_machine_get_sleep_time_ms to SLEEP_MAX_MS (60s) |
| P1: CH schedule BLE quiet | esp_now_manager_send_schedule_burst() uses single BLE quiet for all peers |
| P2: Queue drain | MAX_QUEUE_LINES_PER_CYCLE=10 in MEMBER state |
| P2: malloc in ESP-NOW recv | Replaced with static s_json_recv_buf[251] |
| P2: Election re-entrancy | Added s_election_mutex |
| P2: metrics_mutex failure | Early return in metrics_init, metrics updates disabled |
| Cleanup | Removed duplicate comment, added auth_provision_cluster_key() |
