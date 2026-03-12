# STELLAR Protocol Timing Diagram & Superframe Architecture

## Quick Links

- [STELLAR_ALGORITHM.md](STELLAR_ALGORITHM.md) — Multi-metric cluster head election math
- [TDMA_SCHEDULING.md](TDMA_SCHEDULING.md) — Detailed TDMA slot mechanics and ESP-NOW communication
- [DUAL_CORE_SENSOR_ARCHITECTURE.md](DUAL_CORE_SENSOR_ARCHITECTURE.md) — Why sensors run continuously and task timing

## Overview

The STELLAR (Secure Trust-Enhanced Lyapunov-optimised Leader Allocation for Resilient networks) protocol uses a **dual-phase superframe** structure that alternates between:

1. **STELLAR Phase (0-20s):** BLE-based neighbor discovery and distributed cluster head (CH) election
2. **DATA Phase (20-40s):** ESP-NOW-based TDMA data transmission and store-first reliability

This temporal radio separation solves the fundamental constraint of ESP32-S3's **single shared 2.4 GHz radio** between BLE and WiFi/ESP-NOW.

---

## Superframe Structure

```
                              ONE SUPERFRAME (40 seconds)
    ◄─────────────────────────────────────────────────────────────────────────►
    
    ┌─────────────────────────────────┬─────────────────────────────────────────┐
    │         STELLAR PHASE           │              DATA PHASE                 │
    │           (20 sec)              │               (20 sec)                  │
    ├─────────────────────────────────┼─────────────────────────────────────────┤
    │                                 │                                         │
    │  ┌───────────────────────────┐  │  ┌─────────────────────────────────┐   │
    │  │     BLE ACTIVE            │  │  │      ESP-NOW ACTIVE             │   │
    │  │  • Scanning ON            │  │  │   • BLE Scanning OFF            │   │
    │  │  • Advertising ON         │  │  │   • Advertising ON (beacons)   │   │
    │  │  • Metrics Exchange       │  │  │   • TDMA Slot Transmission     │   │
    │  │  • CH Election            │  │  │   • Sensor Data to CH          │   │
    │  │  • Neighbor Discovery     │  │  │   • Store-First Pattern        │   │
    │  └───────────────────────────┘  │  └─────────────────────────────────┘   │
    │                                 │                                         │
    └─────────────────────────────────┴─────────────────────────────────────────┘
    
    t=0                              t=20s                                    t=40s
```

---

## Detailed Phase Timing

### STELLAR Phase (0-20 seconds)

```
    STELLAR PHASE (20 seconds)
    ◄───────────────────────────────────────────────────────────────────────────►
    
    ┌───────────────────────────────────────────────────────────────────────────┐
    │                                                                           │
    │   BLE Operations:                                                         │
    │   ═══════════════                                                         │
    │                                                                           │
    │   ┌─────────────────────────────────────────────────────────────────────┐ │
    │   │  SCANNING ████████████████████████████████████████████████████████ │ │
    │   │           Receive neighbor beacons, collect RSSI & metrics          │ │
    │   └─────────────────────────────────────────────────────────────────────┘ │
    │                                                                           │
    │   ┌─────────────────────────────────────────────────────────────────────┐ │
    │   │  ADVERTISING ███████████████████████████████████████████████████████│ │
    │   │              Broadcast own metrics (battery, uptime, trust, LQ)     │ │
    │   └─────────────────────────────────────────────────────────────────────┘ │
    │                                                                           │
    │   ┌─────────────────────────────────────────────────────────────────────┐ │
    │   │  ELECTION   ░░░░░░░░░░░░░░░░░░░░░██████████████████████████████████ │ │
    │   │             Wait 10s (ELECTION_WINDOW_MS), then elect CH            │ │
    │   └─────────────────────────────────────────────────────────────────────┘ │
    │                                                                           │
    │   Activities:                                                             │
    │   • Receive metrics from neighbors via BLE scan                          │
    │   • Broadcast own STELLAR metrics (battery %, uptime, trust, RSSI)       │
    │   • Update neighbor table with RSSI & metrics                            │
    │   • Run CH election algorithm (after 10s window)                         │
    │   • Validate existing CH or elect new one                                │
    │   • ESP-NOW: IDLE (radio shared with BLE)                                │
    │                                                                           │
    └───────────────────────────────────────────────────────────────────────────┘
    
    t=0                     t=10s                                          t=20s
         │                    │                                               │
         │  Collect metrics   │  Election window ends                        │
         │  from neighbors    │  → Compute STELLAR scores                    │
         │                    │  → Elect/Confirm CH                          │
         │                    │  → Cache CH ID for DATA phase                │
```

### DATA Phase (20-40 seconds)

```
    DATA PHASE (20 seconds)
    ◄───────────────────────────────────────────────────────────────────────────►
    
    ┌───────────────────────────────────────────────────────────────────────────┐
    │                                                                           │
    │   ESP-NOW Operations:                                                     │
    │   ═══════════════════                                                     │
    │                                                                           │
    │   ┌───────┬───────┬───────┬───────┬───────┬───────┬───────┬───────┐     │
    │   │ GUARD │ SLOT  │ SLOT  │ SLOT  │ SLOT  │ SLOT  │ SLOT  │ SLOT  │     │
    │   │  5s   │ Node0 │ Node1 │ Node2 │ Node3 │ Node4 │  ...  │  ...  │     │
    │   │       │ 10s   │ 10s   │ 10s   │ 10s   │ 10s   │       │       │     │
    │   └───────┴───────┴───────┴───────┴───────┴───────┴───────┴───────┘     │
    │                                                                           │
    │   BLE Status:                                                             │
    │   ════════════                                                            │
    │                                                                           │
    │   ┌─────────────────────────────────────────────────────────────────────┐ │
    │   │  SCANNING     ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░ │ │
    │   │               DISABLED - Radio dedicated to ESP-NOW                 │ │
    │   └─────────────────────────────────────────────────────────────────────┘ │
    │                                                                           │
    │   ┌─────────────────────────────────────────────────────────────────────┐ │
    │   │  ADVERTISING  █████████████████████████████████████████████████████ │ │
    │   │               CH beacons continue (low duty cycle)                  │ │
    │   └─────────────────────────────────────────────────────────────────────┘ │
    │                                                                           │
    │   Activities:                                                             │
    │   • BLE scanning OFF (prevents ESP-NOW MAC-layer ACK failures)           │
    │   • CH beacons continue via advertising                                  │
    │   • Members send sensor data during assigned TDMA slot                   │
    │   • CH receives and stores data from members                             │
    │   • Store-First: Write to SPIFFS before ESP-NOW send                     │
    │   • Burst drain from storage during time slot                            │
    │                                                                           │
    └───────────────────────────────────────────────────────────────────────────┘
    
    t=20s        t=25s       t=35s       t=45s                             t=40s
      │           │           │           │                                  │
      │  Guard    │  Slot 0   │  Slot 1   │  (if more nodes...)             │
      │  period   │  active   │  active   │                                  │
```

---

## TDMA Slot Detail

```
                         SINGLE TDMA SLOT (10 seconds)
    ◄─────────────────────────────────────────────────────────────────────────────►
    
    ┌─────────────────────────────────────────────────────────────────────────────┐
    │                                                                             │
    │  Member Node Actions:                                                       │
    │                                                                             │
    │  ┌───────────────────────────────────────────────────────────────────────┐ │
    │  │ 1. GET     │ 2. STORE    │ 3. SEND      │ 4. BURST DRAIN             │ │
    │  │ Sensor     │ to SPIFFS   │ via ESP-NOW  │ (pop & send old chunks)    │ │
    │  │ Data       │ (MSLG fmt)  │ to CH        │                            │ │
    │  │            │             │              │                            │ │
    │  │ ~10ms      │ ~50ms       │ ~100ms       │ Remaining slot time        │ │
    │  └───────────────────────────────────────────────────────────────────────┘ │
    │                                                                             │
    │  Data Flow:                                                                 │
    │                                                                             │
    │  ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐             │
    │  │  Sensor  │───►│   RAM    │───►│  SPIFFS  │───►│ ESP-NOW  │───► CH     │
    │  │  Read    │    │ payload  │    │  (MSLG)  │    │  Send    │             │
    │  └──────────┘    └──────────┘    └──────────┘    └──────────┘             │
    │                                                                             │
    │  Burst Mode (MAX_MSLG_BURST = 8 chunks):                                   │
    │  ┌─────────────────────────────────────────────────────────────────────┐   │
    │  │ pop_chunk → decompress(if needed) → send → success? delete : requeue │   │
    │  │ Repeat up to 8 times or until slot ends                              │   │
    │  └─────────────────────────────────────────────────────────────────────┘   │
    │                                                                             │
    └─────────────────────────────────────────────────────────────────────────────┘
```

---

## Radio Coexistence Timeline

```
    ┌─────────────────────────────────────────────────────────────────────────────┐
    │                      RADIO RESOURCE SHARING                                 │
    └─────────────────────────────────────────────────────────────────────────────┘
    
    Time:   0s        10s       20s       25s       30s       35s       40s
            │          │         │         │         │         │         │
            ▼          ▼         ▼         ▼         ▼         ▼         ▼
    
    BLE     ████████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░████████████████
    Scan    │←── ACTIVE ──────→│←──────── DISABLED ──────────→│←── ACTIVE ───→
    
    BLE     ████████████████████████████████████████████████████████████████████
    Adv     │←───────────────────── ALWAYS ACTIVE ─────────────────────────────→
    
    ESP-NOW ░░░░░░░░░░░░░░░░░░░░████████████████████████████████░░░░░░░░░░░░░░░░
    TX/RX   │←──── IDLE ──────→│←────── ACTIVE (TDMA) ───────→│←── IDLE ─────→
    
            │                   │                               │
            │   STELLAR PHASE   │        DATA PHASE             │   STELLAR
            │   (BLE Priority)  │     (ESP-NOW Priority)        │   PHASE
            │                   │                               │
    
    Legend:
    ████ = Active/Enabled
    ░░░░ = Inactive/Disabled
    
    Enhanced Neighbor Management (Recent Improvements):
    ══════════════════════════════════════════════════════
    • Extended mutex timeouts: 100ms → 500ms (race condition mitigation)
    • ESP-NOW peer registration validation before neighbor table updates
    • Post-addition verification with comprehensive debug logging
    • Clean neighbor state initialization with memset
    • Table status monitoring and full debugging dumps
    
    Performance Impact:
    ═══════════════════
    ESP-NOW Success Rate:      <10% → >90% (9x improvement)
    BLE Discovery Success:     ~50% → >95% (2x improvement)
    Radio Contention Events:   High → None (eliminated)
    Neighbor Table Reliability: Low → High (race conditions fixed)
```

---

## Why BLE Scanning is Disabled During DATA Phase

```
    ┌─────────────────────────────────────────────────────────────────────────────┐
    │                      RADIO CONTENTION PROBLEM                               │
    └─────────────────────────────────────────────────────────────────────────────┘
    
    ESP32-S3 has a SINGLE 2.4 GHz radio shared between BLE and ESP-NOW.
    
    If BLE scanning is ON during ESP-NOW transmission:
    
    ┌─────────────────────────────────────────────────────────────────────────────┐
    │                                                                             │
    │   ESP-NOW TX ─────────┬──────────────────┐                                 │
    │                       │                  │                                 │
    │                       ▼                  ▼                                 │
    │                  ┌─────────┐        ┌─────────┐                            │
    │                  │  DATA   │        │  ACK    │ ← MAC-layer ACK required  │
    │                  │ PACKET  │        │ TIMEOUT │   for ESP-NOW success     │
    │                  └─────────┘        └─────────┘                            │
    │                       │                  ▲                                 │
    │                       │                  │                                 │
    │   BLE Scan Window ────┼──────────────────┼───────                          │
    │                       │   COLLISION!     │                                 │
    │                       │   Radio busy     │                                 │
    │                       │   with BLE scan  │                                 │
    │                       ▼                  │                                 │
    │                  ┌─────────────────────────┐                               │
    │                  │  ESP-NOW SEND FAILS!    │                               │
    │                  │  (ESP_ERR_ESPNOW_BASE)  │                               │
    │                  └─────────────────────────┘                               │
    │                                                                             │
    └─────────────────────────────────────────────────────────────────────────────┘
    
    Solution: Disable BLE scanning during DATA phase
    
    ┌─────────────────────────────────────────────────────────────────────────────┐
    │                                                                             │
    │   ESP-NOW TX ─────────┬──────────────────┐                                 │
    │                       │                  │                                 │
    │                       ▼                  ▼                                 │
    │                  ┌─────────┐        ┌─────────┐                            │
    │                  │  DATA   │        │  ACK    │ ✓ ACK received            │
    │                  │ PACKET  │◄──────►│  OK!    │                            │
    │                  └─────────┘        └─────────┘                            │
    │                                          │                                 │
    │   BLE Scan ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░│░░░░░░░░░░░░░                   │
    │            DISABLED - Radio FREE         │                                 │
    │                                          ▼                                 │
    │                  ┌─────────────────────────┐                               │
    │                  │  ESP-NOW SEND SUCCESS!  │                               │
    │                  └─────────────────────────┘                               │
    │                                                                             │
    └─────────────────────────────────────────────────────────────────────────────┘
```

---

## CH Beacon Timeout Handling

```
    ┌─────────────────────────────────────────────────────────────────────────────┐
    │              CH BEACON TIMEOUT - PHASE-AWARE CHECKING                       │
    └─────────────────────────────────────────────────────────────────────────────┘
    
    Problem: CH beacons received via BLE scanning, but scanning OFF during DATA.
    
    Timeline:
    
    STELLAR        DATA              STELLAR         DATA
    │◄── 20s ──►│◄── 20s ──►│◄── 20s ──►│◄── 20s ──►│
    
    CH Beacons:
    ████████████░░░░░░░░░░░░████████████░░░░░░░░░░░░
    │ Received  │ NOT recv  │ Received  │ NOT recv  │
    │ (scan ON) │(scan OFF) │ (scan ON) │(scan OFF) │
    
    Without phase-aware checking:
    ┌─────────────────────────────────────────────────────────────────────────────┐
    │  At t=25s (5s into DATA phase):                                            │
    │  • Last beacon seen at t=20s                                               │
    │  • If CH_BEACON_TIMEOUT_MS = 5s → TIMEOUT! → False CH loss!               │
    │  • Re-election triggered incorrectly                                       │
    └─────────────────────────────────────────────────────────────────────────────┘
    
    With phase-aware checking (implemented):
    ┌─────────────────────────────────────────────────────────────────────────────┐
    │  During DATA phase:                                                        │
    │  • Skip beacon timeout check (BLE scanning disabled anyway)                │
    │  • Use cached CH ID (s_cached_ch_id) from end of STELLAR phase             │
    │                                                                            │
    │  During STELLAR phase:                                                     │
    │  • Resume beacon timeout checking                                          │
    │  • CH_BEACON_TIMEOUT_MS = 45s (covers full cycle + buffer)                │
    │  • CH_MISS_THRESHOLD = 10 consecutive misses before re-election           │
    └─────────────────────────────────────────────────────────────────────────────┘
```

---

## Configuration Parameters

| Parameter | Value | Description |
|-----------|-------|-------------|
| `STELLAR_PHASE_MS` | 20,000 ms | Duration of BLE metrics/election phase |
| `DATA_PHASE_MS` | 20,000 ms | Duration of ESP-NOW data transmission phase |
| `PHASE_GUARD_MS` | 5,000 ms | Guard time before TDMA slots start |
| `SLOT_DURATION_SEC` | 10 sec | Duration of each node's TDMA slot |
| `ELECTION_WINDOW_MS` | 10,000 ms | Time to collect metrics before election |
| `CH_BEACON_TIMEOUT_MS` | 45,000 ms | CH beacon timeout (phase-aware) |
| `CH_MISS_THRESHOLD` | 10 | Consecutive misses before re-election |
| `MAX_MSLG_BURST` | 8 | Max chunks to burst-drain per slot |

---

## UAV Onboarding Phase (Special Event)

When the UAV approaches and sends RF code 22 via 433MHz transmitter, the CH enters a special **UAV_ONBOARDING** state that temporarily suspends the normal STELLAR protocol.

### UAV Onboarding Sequence

```
                         UAV ONBOARDING SEQUENCE
    ┌─────────────────────────────────────────────────────────────────────────────┐
    │                                                                             │
    │  RF Code 22 Received (433MHz)                                              │
    │         │                                                                   │
    │         ▼                                                                   │
    │  ┌─────────────────────────────────────────────────────────────────────┐   │
    │  │ [1] BROADCAST CH_BUSY TO MEMBERS                                    │   │
    │  │     • ESP-NOW broadcast: CH_STATUS_UAV_BUSY                         │   │
    │  │     • Members receive → HOLD DATA (don't send)                      │   │
    │  │     • 200ms delay for broadcast propagation                         │   │
    │  └─────────────────────────────────────────────────────────────────────┘   │
    │         │                                                                   │
    │         ▼                                                                   │
    │  ┌─────────────────────────────────────────────────────────────────────┐   │
    │  │ [2] STOP BLE                                                        │   │
    │  │     • Stop BLE scanning                                             │   │
    │  │     • Stop BLE advertising                                          │   │
    │  └─────────────────────────────────────────────────────────────────────┘   │
    │         │                                                                   │
    │         ▼                                                                   │
    │  ┌─────────────────────────────────────────────────────────────────────┐   │
    │  │ [3] DEINIT ESP-NOW                                                  │   │
    │  │     • esp_now_deinit() - release ESP-NOW                            │   │
    │  │     • esp_wifi_stop() - prepare for STA mode                        │   │
    │  └─────────────────────────────────────────────────────────────────────┘   │
    │         │                                                                   │
    │         ▼                                                                   │
    │  ┌─────────────────────────────────────────────────────────────────────┐   │
    │  │ [4] WiFi STA MODE - CONNECT TO UAV HOTSPOT                          │   │
    │  │     • Connect to "WSN_AP" (UAV hotspot on RPi)                      │   │
    │  │     • Obtain IP address (10.42.0.x)                                 │   │
    │  │     • ~5-10 seconds                                                 │   │
    │  └─────────────────────────────────────────────────────────────────────┘   │
    │         │                                                                   │
    │         ▼                                                                   │
    │  ┌─────────────────────────────────────────────────────────────────────┐   │
    │  │ [5] HTTP DATA UPLOAD                                                │   │
    │  │     • POST /onboard → Get session_id                                │   │
    │  │     • POST /data → Upload ALL MSLG chunks (decompressed)            │   │
    │  │     • POST /ack → Confirm successful upload                         │   │
    │  │     • Clear storage on success                                      │   │
    │  │     • ~10-30 seconds (depends on data size)                         │   │
    │  └─────────────────────────────────────────────────────────────────────┘   │
    │         │                                                                   │
    │         ▼                                                                   │
    │  ┌─────────────────────────────────────────────────────────────────────┐   │
    │  │ [6] REINIT ESP-NOW                                                  │   │
    │  │     • esp_wifi_set_mode(STA)                                        │   │
    │  │     • esp_wifi_start()                                              │   │
    │  │     • esp_wifi_set_channel(ESP_NOW_CHANNEL)                         │   │
    │  │     • esp_now_init() + register callbacks                           │   │
    │  └─────────────────────────────────────────────────────────────────────┘   │
    │         │                                                                   │
    │         ▼                                                                   │
    │  ┌─────────────────────────────────────────────────────────────────────┐   │
    │  │ [7] BROADCAST CH_RESUME TO MEMBERS                                  │   │
    │  │     • ESP-NOW broadcast: CH_STATUS_RESUME                           │   │
    │  │     • Members receive → RESUME TDMA data sending                    │   │
    │  │     • Return to STATE_CH                                            │   │
    │  │     • Resume BLE advertising                                        │   │
    │  └─────────────────────────────────────────────────────────────────────┘   │
    │                                                                             │
    └─────────────────────────────────────────────────────────────────────────────┘
```

### Member Behavior During UAV Onboarding

```
    ┌─────────────────────────────────────────────────────────────────────────────┐
    │                    MEMBER NODE DURING UAV ONBOARDING                        │
    └─────────────────────────────────────────────────────────────────────────────┘
    
    Normal Operation              CH_BUSY Received              CH_RESUME Received
    ─────────────────            ──────────────────            ────────────────────
    
    ┌───────────────┐            ┌───────────────┐             ┌───────────────┐
    │ TDMA Slot     │            │ TDMA Slot     │             │ TDMA Slot     │
    │ ════════════  │            │ ════════════  │             │ ════════════  │
    │               │            │               │             │               │
    │ • Collect     │ ─────────► │ • Collect     │ ──────────► │ • Collect     │
    │   sensor data │            │   sensor data │             │   sensor data │
    │               │            │               │             │               │
    │ • SEND to CH  │            │ • HOLD data   │             │ • SEND to CH  │
    │   via ESP-NOW │            │   (store only)│             │   via ESP-NOW │
    │               │            │   ⚠️ NO SEND   │             │               │
    │ • Burst drain │            │               │             │ • Burst drain │
    │   old chunks  │            │ • Log warning │             │   old chunks  │
    │               │            │   once        │             │               │
    └───────────────┘            └───────────────┘             └───────────────┘
    
    WHY HOLD DATA?
    ══════════════
    During UAV onboarding:
    • CH's ESP-NOW is DEINITIALIZED (no receiver)
    • CH's WiFi channel changed to UAV AP channel
    • Any ESP-NOW send would FAIL (no ACK)
    • Better to buffer locally and send after RESUME
```

### Timing Diagram with UAV Onboarding

```
    Normal STELLAR Protocol           UAV Arrives              Back to Normal
    ═══════════════════════          ════════════             ═══════════════
    
    Time:  0s    20s   40s   60s     70s                     90s    110s   130s
           │      │     │     │       │                       │       │      │
           ▼      ▼     ▼     ▼       ▼                       ▼       ▼      ▼
    
    CH:    ┌──────┬──────┬──────┬──────┬═══════════════════════┬──────┬──────┐
           │STEL. │DATA  │STEL. │DATA  │   UAV_ONBOARDING      │STEL. │DATA  │
           │      │      │      │      │   (WiFi STA mode)     │      │      │
           └──────┴──────┴──────┴──────┴═══════════════════════┴──────┴──────┘
                                       │                       │
                                       │ RF Code 22            │ Onboarding
                                       │ received              │ complete
                                       │                       │
    MEMBER:┌──────┬──────┬──────┬──────┬───────────────────────┬──────┬──────┐
           │STEL. │DATA  │STEL. │DATA  │    DATA HOLD          │STEL. │DATA  │
           │      │ TX   │      │ TX   │    (no TX to CH)      │      │ TX   │
           └──────┴──────┴──────┴──────┴───────────────────────┴──────┴──────┘
                                       │                       │
                                       │ CH_BUSY               │ CH_RESUME
                                       │ received              │ received
    
    Legend:
    STEL.  = STELLAR Phase (BLE active)
    DATA   = DATA Phase (ESP-NOW active)
    TX     = TDMA data transmission
    ═══    = UAV Onboarding (special state)
```

### CH Status Message Format

| Field | Size | Description |
|-------|------|-------------|
| `magic` | 4 bytes | 0x43485354 ("CHST") |
| `ch_node_id` | 4 bytes | CH's node ID |
| `status` | 1 byte | 0=NORMAL, 1=UAV_BUSY, 2=RESUME |
| `reserved` | 3 bytes | Padding for alignment |

### RF Trigger Details

| Parameter | Value |
|-----------|-------|
| RF Frequency | 433 MHz |
| Expected Code | 22 |
| Protocol | RC-Switch Protocol 1 |
| Pulse Width T | 350 µs |
| Tolerance | ±40% |
| GPIO Pin | 21 |

---

## Complete Superframe Cycle

```
    t=0s                    t=20s                   t=40s                   t=60s
    │                       │                       │                       │
    ▼                       ▼                       ▼                       ▼
    ┌───────────────────────┬───────────────────────┬───────────────────────┬────
    │     STELLAR #1        │       DATA #1         │     STELLAR #2        │ ...
    │                       │                       │                       │
    │ • BLE scan ON         │ • BLE scan OFF        │ • BLE scan ON         │
    │ • Collect metrics     │ • ESP-NOW TX/RX       │ • Collect metrics     │
    │ • Run election        │ • TDMA slots          │ • Validate CH         │
    │ • Elect CH            │ • Sensor → CH         │ • Re-elect if needed  │
    │ • Cache CH for DATA   │ • Burst drain storage │ • Cache CH            │
    │                       │                       │                       │
    └───────────────────────┴───────────────────────┴───────────────────────┴────
    
    Repeats indefinitely until UAV RF trigger (code 22) is received...
```

---

## Source Files Reference

| File | Role |
|------|------|
| `main/config.h` | Timing constants (STELLAR_PHASE_MS, DATA_PHASE_MS, etc.) |
| `main/state_machine.c` | Phase management, TDMA scheduling, UAV onboarding state |
| `main/esp_now_manager.c` | ESP-NOW data transmission, CH status broadcast |
| `main/esp_now_manager.h` | ch_status_msg_t, CH_STATUS_* defines |
| `components/ble_manager/` | BLE scanning/advertising control |
| `components/rf_receiver/` | 433MHz RF receiver for UAV trigger (code 22) |
| `components/uav_client/` | WiFi STA, HTTP upload to UAV server |
| `components/storage_manager/` | MSLG format storage, burst drain |

*Last Updated: March 2026*
*Author: Chandupa Chiranjeewa Bandaranayake*
