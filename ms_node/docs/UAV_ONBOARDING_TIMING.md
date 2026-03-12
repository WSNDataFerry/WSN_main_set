# UAV Onboarding Timing Analysis & CH Beacon Timeout Considerations

## Quick Links

- [STELLAR_ALGORITHM.md](STELLAR_ALGORITHM.md) — How cluster head election works and beacon timeout triggers re-election
- [UAV_ONBOARDING_CRASH_FIX.md](UAV_ONBOARDING_CRASH_FIX.md) — WiFi startup crash and solution
- [TDMA_SCHEDULING.md](TDMA_SCHEDULING.md) — How members detect CH beacon loss and handle CH_STATUS messages
- [MSLG_DATA_FLOW.md](MSLG_DATA_FLOW.md) — Store-first reliability pattern ensures no data loss during onboarding

## 📋 Overview

This document analyzes the timing implications of UAV data onboarding on the STELLAR protocol's Cluster Head (CH) beacon mechanism. When a CH node enters UAV onboarding mode, it switches from BLE beacons to WiFi STA mode, potentially causing member nodes to lose their CH beacon reference.

---

## ⏱️ Current Timeout Configuration

| Parameter | Value | Purpose |
|-----------|-------|---------|
| `CH_BEACON_TIMEOUT_MS` | **45 seconds** | Max time without CH beacon before member considers CH lost |
| `CH_MEMBER_HYSTERESIS_MS` | 8 seconds | Hysteresis to prevent oscillation after beacon timeout |
| `CH_MISS_THRESHOLD` | 10 consecutive | Missed beacons in STELLAR phase before re-election |
| `NEIGHBOR_TIMEOUT_MS` | 60 seconds | General neighbor entry expiration |
| `timeout_ms` (data upload) | **30 seconds** | HTTP POST timeout for `/data` endpoint (was 10 s) |

---

## 🛫 UAV Onboarding Timing Breakdown

### Worst-Case Scenario Timeline

```
Time 0s:   RF trigger received (code 22 from UAV)
           ├── Broadcast CH_BUSY to members (200ms delay)
           ├── Stop BLE scanning/advertising
           └── Deinit ESP-NOW (channel conflict)

Time 0.5s: WiFi STA connection attempt begins
           ├── esp_wifi_set_ps(WIFI_PS_NONE) — disable STA power save
           └── Retry loop: 5 retries × exponential backoff = MAX 30 seconds
           
Time 10.5s: WiFi connected (worst case)
           ├── POST /onboard (timeout: 5 seconds)
           └── Wait for session_id response

Time 15.5s: Session established (worst case)
           ├── POST /data — Upload ALL stored data (4 KB chunked)
           │   └── Timeout: 30 seconds
           │   └── Data size: Up to ~64KB decompressed
           │   └── Chunk size: 4,096 bytes per esp_http_client_write()
           │   └── Transfer: 16 chunks × ~200ms/chunk ≈ 3-5 seconds typical
           
Time 20.5s: Data upload complete (worst case)
           ├── POST /ack (fast, <1 second)
           
Time 21.5s: Onboarding complete
           ├── Reinitialize ESP-NOW
           ├── Broadcast CH_RESUME to members
           └── Return to STATE_CH, resume BLE advertising
```

### Summary: Maximum CH Silent Period

| Phase | Duration (Typical) | Duration (Worst Case) |
|-------|-------------------|----------------------|
| Pre-WiFi setup | 0.5s | 0.5s |
| WiFi connection | 2-3s | **30s** |
| POST /onboard | 0.5s | 5s |
| POST /data (4 KB chunked upload) | 2-5s | **30s** |
| POST /ack | 0.2s | 1s |
| Post-cleanup | 0.3s | 0.5s |
| **TOTAL** | **5-9 seconds** | **~67 seconds** |

---

## 🔴 The Problem

Current `CH_BEACON_TIMEOUT_MS = 45 seconds` is designed to survive:
- DATA phase (20s) + STELLAR phase (20s) + 5s buffer

However, if UAV onboarding occurs during a specific timing window:
1. Member node receives last CH beacon
2. CH enters UAV onboarding (BLE stops)
3. Member is in STELLAR phase (beacon timeout active)
4. UAV onboarding takes 67 seconds (absolute worst case with retries)
5. Member detects "CH lost" at 45s → **triggers re-election**

### Risk Matrix

| UAV Onboarding Duration | Beacon Timeout | Risk |
|------------------------|----------------|------|
| < 20s (typical 5-9s) | 45s | ✅ Safe (DATA phase buffer) |
| 20-40s | 45s | ⚠️ Marginal (depends on phase) |
| > 40s (extreme WiFi issues) | 45s | ❌ Re-election likely |

---

## ✅ Current Mitigations

### 1. CH_BUSY Broadcast (ESP-NOW)
Before entering WiFi mode, CH broadcasts `CH_STATUS_UAV_BUSY` message to all members:

```c
// state_machine.c - STATE_UAV_ONBOARDING
esp_now_manager_broadcast_ch_status(CH_STATUS_UAV_BUSY);
vTaskDelay(pdMS_TO_TICKS(200)); // Give time for broadcasts
```

Member nodes check this status before considering CH lost:
```c
// state_machine.c - STATE_MEMBER
if (esp_now_manager_is_ch_busy()) {
    can_send = false;  // Hold data, don't re-elect
}
```

### 2. Auto-Clear Stale Status
If CH fails during onboarding (crash, power loss), members auto-clear busy status after 60s:

```c
// esp_now_manager.c
if (s_ch_busy && (now_ms - s_ch_status_time_ms) > 60000) {
    ESP_LOGW(TAG, "CH_BUSY status stale (>60s), auto-clearing");
    s_ch_busy = false;
}
```

### 3. Phase-Aware Timeout Check
DATA phase disables beacon timeout checking (BLE already off):

```c
// state_machine.c - STATE_MEMBER
// Skip beacon timeout check during DATA phase to prevent false positives.
```

---

## 🔧 WiFi Reliability & Chunked Upload (March 2026 Fix)

### Problem: Large Payload Stalls TCP Connection

Field testing revealed that the `/data` POST with 36+ KB payloads **never arrived at the Flask server**. The ESP32 would hang for the full timeout, then report `ESP_ERR_HTTP_WRITE_DATA`.

**Root Cause Chain:**

```
1. esp_http_client_perform() tries to write 36KB in one call
2. lwIP TCP send buffer is only 5,760 bytes (CONFIG_LWIP_TCP_SND_BUF_DEFAULT)
3. Payload requires ~25 TCP segments to transmit
4. ESP32 STA WiFi power management (wifi:pm start, type: 0) sleeps radio between ACKs
5. Pi AP also power-manages → stops sending beacons
6. ESP32 detects bcn_timeout → WiFi link degrades
7. TCP write stalls → timeout → ESP_ERR_HTTP_WRITE_DATA
8. Flask never receives the POST (connection died at TCP level)
```

### Solution: 4 KB Chunked Write Loop

Replaced `esp_http_client_perform()` with the explicit **open / write / fetch_headers** API:

```c
// Open connection and send HTTP headers (Content-Length known upfront)
esp_http_client_open(client, upload_len);

// Write body in 4KB chunks — each chunk waits for TCP ACK
const int CHUNK_SIZE = 4096;
size_t bytes_sent = 0;
while (bytes_sent < upload_len) {
    int to_send = MIN(CHUNK_SIZE, upload_len - bytes_sent);
    int written = esp_http_client_write(client, upload_data + bytes_sent, to_send);
    if (written < 0) break;  // write error
    bytes_sent += written;
}

// Read server response
esp_http_client_fetch_headers(client);
int status = esp_http_client_get_status_code(client);
esp_http_client_close(client);
```

**Why 4 KB?**
- TCP SND_BUF = 5,760 bytes → 4,096 fits within one TCP window
- Avoids fragmentation at the lwIP layer
- Each `write()` call blocks until TCP ACK → natural flow control
- No internal buffering overflow

### WiFi Power Management Fixes

| Layer | Fix | Code |
|-------|-----|------|
| **ESP32 STA** | Disable power save after connect | `esp_wifi_set_ps(WIFI_PS_NONE)` in `wifi_join()` |
| **Pi AP** | Disable radio power management | `sudo iwconfig wlan1 power off` in `data_onboarding()` |
| **Flask** | Enable threaded request handling | `threaded=True` in `app.run()` kwargs |

### Before vs After

| Metric | Before | After |
|--------|--------|-------|
| Upload API | `esp_http_client_perform()` (buffered) | `open/write/fetch_headers` (streamed) |
| Chunk size | Full payload at once (36 KB+) | 4,096 bytes per write |
| `timeout_ms` | 10,000 ms | 30,000 ms |
| STA power save | ON (default) | OFF (`WIFI_PS_NONE`) |
| AP power save | ON (default) | OFF (`iwconfig power off`) |
| Flask threading | Single-threaded | `threaded=True` |
| Observed result | `bcn_timeout` → `ESP_ERR_HTTP_WRITE_DATA` | ✅ Reliable transfer |

---

## 🤔 Discussion Points

### Q1: Is 45s CH_BEACON_TIMEOUT_MS Sufficient?

**Current Answer: YES, with CH_BUSY mechanism**

The CH_BUSY broadcast allows members to extend their tolerance indefinitely while CH is known to be doing UAV work. The 45s timeout only applies when:
- CH disappears without sending CH_BUSY (power failure, crash)
- Member didn't receive the CH_BUSY broadcast (rare, triple broadcast used)

### Q2: What if Member Misses CH_BUSY Broadcast?

**Risk: Low but possible**

Triple broadcast with 50ms gaps provides reliability:
```c
for (int i = 0; i < 3; i++) {
    ret = esp_now_send(BROADCAST_ADDR, (uint8_t *)&msg, sizeof(msg));
    vTaskDelay(pdMS_TO_TICKS(50));
}
```

If member misses all 3 (in deep sleep, BLE busy), it will:
1. Timeout at 45s (if in STELLAR phase)
2. Trigger re-election → **cluster disruption**

### Q3: Should We Increase CH_BEACON_TIMEOUT_MS?

**Trade-off Analysis:**

| Longer Timeout | Pros | Cons |
|---------------|------|------|
| 60s | Survives worst-case UAV onboarding | Slower failure detection |
| 90s | Very safe margin | Members orphaned for 90s if CH fails |
| 120s | Extreme safety | Unacceptable failure recovery time |

**Recommendation:** Keep at 45s, rely on CH_BUSY mechanism.

### Q4: Alternative Approach - Extend Timeout on CH_BUSY?

**Proposed Enhancement:**

When member receives CH_BUSY, temporarily extend beacon timeout:

```c
// PROPOSED: esp_now_manager.c or state_machine.c
#define CH_BUSY_EXTENDED_TIMEOUT_MS 120000  // 2 minutes

// In member state, when CH_BUSY received:
if (status_msg->status == CH_STATUS_UAV_BUSY) {
    s_extended_timeout_until = now_ms + CH_BUSY_EXTENDED_TIMEOUT_MS;
}

// In beacon timeout check:
bool timeout_extended = (now_ms < s_extended_timeout_until);
if (!timeout_extended && (now_ms - last_beacon > CH_BEACON_TIMEOUT_MS)) {
    // CH lost
}
```

**Benefit:** Members that receive CH_BUSY get 2-minute tolerance.
**Fallback:** Members that miss broadcast still have 45s standard timeout.

---

## 📊 Timing Diagram: UAV Onboarding vs Beacon Timeout

```
CH Node Timeline:
═══════════════════════════════════════════════════════════════════════════════
│ STELLAR │ DATA │ STELLAR │ DATA │ STELLAR │                               │
│ (BLE)   │(ESP) │ (BLE)   │(ESP) │ (BLE)   │                               │
═══════════════════════════════════════════════════════════════════════════════
                                    │
                                    ▼ RF Trigger (Code 22)
                                    ╔═══════════════════════════════╗
                                    ║      UAV ONBOARDING           ║
                                    ║  (WiFi STA - NO BLE Beacons)  ║
                                    ║  Duration: 5-27 seconds       ║
                                    ╚═══════════════════════════════╝
                                                                    │
                                                                    ▼ CH_RESUME
                                            ═══════════════════════════════════════════════════════════════════════════════
                                            │                        │ STELLAR │ DATA │ STELLAR │ DATA │ ...            │
                                            │        UAV BUSY        │ (BLE)   │(ESP) │ (BLE)   │(ESP) │                │
                                            ═══════════════════════════════════════════════════════════════════════════════

Member Node Timeline:
═══════════════════════════════════════════════════════════════════════════════
│ STELLAR │ DATA │ STELLAR │ DATA │ STELLAR │                               │
│ (listen)│(send)│ (listen)│(send)│ (listen)│                               │
═══════════════════════════════════════════════════════════════════════════════
          │       │         │       │
          │       │         │       ▼ Receives CH_BUSY via ESP-NOW
          │       │         │       ╔═══════════════════════════════╗
          │       │         │       ║    HOLD DATA - CH is BUSY     ║
          │       │         │       ║  (No beacon timeout trigger)  ║
          │       │         │       ║  Wait for CH_RESUME           ║
          │       │         │       ╚═══════════════════════════════╝
          │       │         │                                       │
          │       │         │                                       ▼ CH_RESUME
                        ═══════════════════════════════════════════════════════════════════════════════
                        │                        │    HOLD DATA     │ STELLAR │ DATA │ ...          │
                        │        Normal Ops      │   (buffered)     │ (resume)│(send)│              │
                        ═══════════════════════════════════════════════════════════════════════════════

Timeline (seconds):
0────10────20────30────40────50────60────70────80────90────100
                    │
                    └─► CH_BEACON_TIMEOUT_MS = 45s window
                        (Only applies if CH_BUSY NOT received)
```

---

## 🔧 Recommendations

### Immediate (Current Implementation - Sufficient)

1. ✅ **CH_BUSY/CH_RESUME broadcasts** - Already implemented
2. ✅ **60s auto-clear** - Prevents permanent lockup
3. ✅ **Phase-aware timeout** - DATA phase doesn't trigger false alarms
4. ✅ **Triple broadcast** - Improves reliability

### Future Enhancements (Optional)

1. **Extended timeout on CH_BUSY** - Add `s_extended_timeout_until` variable
2. **Heartbeat during onboarding** - Brief ESP-NOW "I'm alive" every 15s during long uploads
3. **Pre-announce onboarding duration** - Include estimated duration in CH_BUSY message
4. **Retry CH_BUSY after WiFi connect** - Re-broadcast over ESP-NOW if initial broadcasts may have been missed (requires brief ESP-NOW reinit)

---

## 📝 Conclusion

**The current 45-second CH_BEACON_TIMEOUT_MS is acceptable** because:

1. **CH_BUSY mechanism** notifies members before BLE stops
2. **Typical UAV onboarding takes 5-9 seconds** (well under 45s) with chunked upload
3. **Worst-case ~67 seconds** requires CH_BUSY to prevent re-election (covers WiFi retry + upload retry)
4. **Auto-clear at 60s** prevents deadlock if CH crashes
5. **4 KB chunked upload** eliminates TCP stalls that previously caused 100% failure at ≥36 KB

**Risk Level: LOW** - Re-election during UAV onboarding is unlikely unless:
- Member misses all 3 CH_BUSY broadcasts (very rare)
- UAV onboarding takes >40 seconds **and** CH_BUSY missed (extremely rare)
- Both conditions occur simultaneously

---

## 📚 Related Files

| File | Purpose |
|------|---------|
| `main/config.h` | Timeout definitions |
| `main/state_machine.c` | STATE_UAV_ONBOARDING, beacon timeout logic |
| `main/esp_now_manager.c` | CH_BUSY broadcast, is_ch_busy() check |
| `components/uav_client/uav_client.c` | WiFi join, HTTP uploads |
| `TIMING_DIAGRAM.md` | Overall STELLAR timing |

---

*Last Updated: March 2026*
*Author: Chandupa Chiranjeewa Bandaranayake*
