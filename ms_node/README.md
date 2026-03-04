# MS Node — Delay-Tolerant Wireless Sensor Network (WSN) Pipeline

> ESP32-S3 multi-node environmental monitoring system with **STELLAR** cluster-head election, **TDMA** data scheduling, **MSLG** compressed storage, and **UAV** data offloading.

---

## Table of Contents

1. [Overview](#overview)
2. [System Architecture](#system-architecture)
3. [Node Roles & State Machine](#node-roles--state-machine)
4. [STELLAR Algorithm](#stellar-algorithm)
5. [Superframe: STELLAR + DATA Phases](#superframe-stellar--data-phases)
6. [Data Pipeline](#data-pipeline)
7. [MSLG Storage Format](#mslg-storage-format)
8. [TDMA Scheduling](#tdma-scheduling)
9. [UAV Onboarding (Data Offload)](#uav-onboarding-data-offload)
10. [SPIFFS Auto-Purge](#spiffs-auto-purge)
11. [Sensors & Mock Data](#sensors--mock-data)
12. [Communication Stack](#communication-stack)
13. [Hardware](#hardware)
14. [Build, Flash & Monitor](#build-flash--monitor)
15. [Project Structure](#project-structure)
16. [Configuration Reference](#configuration-reference)
17. [Troubleshooting](#troubleshooting)

> **📊 For detailed MSLG data-flow analysis with graphs and numerical projections, see [`docs/MSLG_DATA_FLOW.md`](docs/MSLG_DATA_FLOW.md)**

---## Overview

Each **MS Node** (Main Set Node) is an ESP32-S3 that:

1. **Senses** — reads environmental sensors (temperature, humidity, pressure, air quality, magnetometer, audio) every 2 s.
2. **Clusters** — discovers neighbours via **BLE** beacons, elects a **Cluster Head (CH)** using the **STELLAR** algorithm.
3. **Stores** — writes sensor payloads into a local **MSLG** compressed file on SPIFFS.
4. **Forwards** — members send data to the CH over **ESP-NOW** during their **TDMA** time slot.
5. **Offloads** — when a **UAV** (Raspberry Pi drone) arrives, the CH uploads all stored data via **WiFi / HTTP**, then resumes normal operation.

The system is fully autonomous and operates on battery power in remote wildlife-monitoring deployments.

---

## System Architecture

```
                         UAV (Raspberry Pi)
                              │
                     WiFi STA ↕ HTTP POST
                              │
        ┌─────────────────────┴─────────────────────────┐
        │                  Cluster Head (CH)             │
        │  ┌───────────┐  ┌──────────┐  ┌────────────┐  │
        │  │ BLE Beacon │  │ ESP-NOW  │  │   SPIFFS   │  │
        │  │ (STELLAR)  │  │ TX / RX  │  │  data.lz   │  │
        │  └───────────┘  └──────────┘  └────────────┘  │
        └──────────┬────────────┬────────────────────────┘
                   │            │
           ESP-NOW │            │ ESP-NOW
                   │            │
        ┌──────────┴──┐  ┌─────┴──────────┐
        │  Member 1   │  │   Member 2     │
        │  (MBR)      │  │   (MBR)        │
        │  BLE + NOW  │  │   BLE + NOW    │
        │  SPIFFS     │  │   SPIFFS       │
        └─────────────┘  └────────────────┘
```

**Current test cluster** — 3 nodes:

| Role | Node ID | Description |
|------|---------|-------------|
| CH | `3125565838` | Cluster Head — aggregates all data |
| MBR1 | `3125668638` | Member — senses + forwards to CH |
| MBR2 | `3125683842` | Member — senses + forwards to CH |

---

## Node Roles & State Machine

Every node starts in `STATE_INIT` → `STATE_DISCOVER` → then transitions to either `STATE_CH` or `STATE_MEMBER` based on the STELLAR election result.

```
STATE_INIT
    │
    ▼
STATE_DISCOVER ──────── BLE scanning, neighbour table built
    │
    ▼
STATE_CANDIDATE ─────── election_run() → STELLAR score comparison
    │
    ├──▶ STATE_CH ────── Won election: broadcast schedule, aggregate data
    │
    └──▶ STATE_MEMBER ── Lost election: send data to CH in TDMA slot
            │
            └──▶ STATE_UAV_ONBOARDING ── RF trigger detected (CH only)
                        │
                        └──▶ STATE_CH ── resume after offload
```

| State | File | Description |
|-------|------|-------------|
| `STATE_CH` | `state_machine.c:425` | Broadcast TDMA schedule, store own data every 5 s, receive member data via ESP-NOW |
| `STATE_MEMBER` | `state_machine.c:645` | Wait for TDMA slot, store-first + send to CH, burst-drain MSLG chunks |
| `STATE_UAV_ONBOARDING` | `state_machine.c:1135` | Suspend ESP-NOW → WiFi STA → HTTP upload → reinit ESP-NOW |

---

## STELLAR Algorithm

**STELLAR** (Secure Trust-Enhanced Lyapunov-optimised Leader Allocation for Resilient networks) selects the Cluster Head by computing a composite score Ψ(n) for every candidate:

```
Ψ(n) = κ(n) · Σ wᵢ(t) · φᵢ(mᵢ) + α · ParetoRank(n)
```

Where:
- **wᵢ(t)** — Lyapunov-stable adaptive weights that converge via gradient descent: `dw/dt = −η∇V(w,t) − β(w − w_eq)`
- **φᵢ** — utility functions per metric (concave battery, saturating uptime, smooth-step trust, power link quality)
- **κ(n)** — centrality factor `1 / (1 + ε(1 − cₙ))`
- **ParetoRank** — multi-objective Pareto dominance count

### Metrics (0.0 – 1.0)

| Metric | Weight | Utility | Source |
|--------|--------|---------|--------|
| Battery | 0.30 | `(1 − e^(−λb)) / (1 − e^(−λ))` | ADC (GPIO 4) or mock |
| Uptime | 0.20 | `tanh(λ · u / u_max)` | `esp_timer_get_time()` |
| Trust | 0.30 | `t²(3 − 2t)` (smooth-step) | HMAC success rate × reputation × PDR |
| Link Quality | 0.20 | `l^(1/γ)` | RSSI EWMA × packet error rate |

### Entropy-based Confidence

Differential entropy `H(mᵢ) = ½ ln(2πe · σᵢ²)` is computed for each metric variance. Higher entropy → lower confidence → the Lyapunov optimizer shifts weight away from that metric.

### Election Tie-break

Deterministic **lowest `node_id`** tie-break ensures exactly one CH per cluster without stagger windows.

**Files:** `election.c`, `metrics.c`, `config.h` (weights, thresholds)

---

## Superframe: STELLAR + DATA Phases

The protocol alternates between two phases in a fixed superframe:

```
◀──────────── 40 s superframe ────────────▶

┌──────────────────┬──────────────────────┐
│  STELLAR PHASE   │     DATA PHASE       │
│     20 000 ms    │     20 000 ms        │
│                  │                      │
│  BLE ON          │  BLE OFF             │
│  ESP-NOW OFF     │  ESP-NOW ON          │
│  Scan + Advertise│  TDMA schedule       │
│  Election runs   │  Sensor send/recv    │
│  Neighbour table │  MSLG store + drain  │
└──────────────────┴──────────────────────┘
```

| Parameter | Value | Defined in |
|-----------|-------|------------|
| `STELLAR_PHASE_MS` | 20 000 ms | `config.h:22` |
| `DATA_PHASE_MS` | 20 000 ms | `config.h:23` |
| `PHASE_GUARD_MS` | 5 000 ms | `config.h:24` |
| State machine tick | 100 ms | FreeRTOS task |
| Main loop period | 2 000 ms | `ms_node.c` |

### Phase Boundary Logging

At every transition the firmware prints a boxed diagnostic:

```
╔═══════════════════════════════════════════╗
║ PHASE → DATA | Role: CH                  ║
║ BLE scan=OFF  adv=OFF | ESP-NOW=ON       ║
║ MSLG chunks: 42                          ║
╚═══════════════════════════════════════════╝
```

---

## Data Pipeline

### Member Flow (sawtooth MSLG pattern)

```
Main Loop (2 s)
  │
  ▼ metrics_set_sensor_data()   ← fresh random mock / real sensor values
  │
  ▼ seq_num++                   ← unique every 2 s

State Machine (100 ms tick, DATA phase, in TDMA slot)
  │
  ├─▶ Store-First: write_compressed(json_payload) → data.lz  [+1 MSLG chunk]
  │
  ├─▶ Live Send:   esp_now_send(CH, sensor_payload_t)         [to CH]
  │                 (2 s gap enforcement → ~3-5 sends per slot)
  │
  └─▶ Burst Drain: pop_mslg_chunk() × 8 → send to CH         [-N MSLG chunks]
                    decompress if algo=1, requeue on failure
```

**Net effect:** MSLG count oscillates — grows from store-first, shrinks from burst drain.

### CH Flow (monotonic MSLG growth)

```
Self-Store (every 5 s during DATA phase)
  │
  └─▶ write_compressed(own json_payload) → data.lz            [+1 chunk / 5 s]

ESP-NOW Receive Callback (esp_now_manager.c:160)
  │
  ├─▶ dedup_should_store(node_id, seq_num)  ← 4-entry circular buffer per node
  │
  └─▶ write_compressed(member payload) → data.lz              [+1 chunk per RX]
```

**Net effect:** CH accumulates ~14-24 new chunks per 40 s superframe (own + MBR1 + MBR2). MSLG grows until a UAV offloads it, **or** SPIFFS auto-purge triggers at 90% capacity.

### Deduplication

| Layer | Scope | Mechanism |
|-------|-------|-----------|
| **Seq-based** | CH receive path | `dedup_should_store()` — 4-entry history per node. Prevents ESP-NOW retransmission duplicates. |
| **Store-first** | Member self-store | No dedup — every TDMA slot entry stores once. `seq_num` advances every 2 s so payloads are unique. |
| **CH self-store** | CH own data | No dedup — stores every 5 s with monotonically increasing `seq_num`. |

> **📊 For detailed growth graphs, code path traces, numerical projections, and the dedup circular buffer visualisation, see [`docs/MSLG_DATA_FLOW.md`](docs/MSLG_DATA_FLOW.md)**

---

## MSLG Storage Format

**MSLG** = Multi-Sensor Log. Binary chunked format stored in `/spiffs/data.lz`.

### Chunk Header (32 bytes, packed)

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;       // 0x4D534C47 ('MSLG')
    uint16_t version;     // 2
    uint8_t  algo;        // 0 = raw, 1 = miniz (DEFLATE)
    uint8_t  level;       // Compression level (1-9)
    uint32_t raw_len;     // Original data size
    uint32_t data_len;    // Stored payload size (after compression)
    uint32_t crc32;       // CRC32 of payload
    uint64_t node_id;     // MAC-derived node identifier
    uint32_t timestamp;   // Seconds since boot
    uint32_t reserved;    // Future use
} mslg_chunk_hdr_t;       // Total: 32 bytes
```

### Compression

- **Algorithm:** miniz (DEFLATE), level 3
- **Threshold:** Only compress if payload ≥ 1 024 bytes
- **Savings requirement:** Must save > 5% or falls back to raw
- **Stack guard:** Disables compression if < 1 024 bytes stack free
- **Allocation:** Compression buffer allocated on PSRAM first, falls back to internal SRAM

### Operations

| Function | Description |
|----------|-------------|
| `storage_manager_write_compressed()` | Append an MSLG chunk (raw or compressed) |
| `storage_manager_pop_mslg_chunk()` | Pop the oldest chunk (FIFO), rewrite file minus first chunk |
| `storage_manager_get_mslg_chunk_count()` | Count valid MSLG chunks in `data.lz` |
| `storage_manager_read_all_decompressed()` | Read + decompress all chunks into a buffer |
| `storage_manager_get_compression_stats()` | Aggregate raw vs compressed byte counts |
| `storage_manager_clear()` | Delete `data.lz` + `data.txt` |

---

## TDMA Scheduling

The CH computes a priority-ordered schedule and broadcasts it to members via ESP-NOW at the start of each DATA phase.

### Schedule Message

```c
typedef struct {
    int64_t epoch_us;           // DATA-phase start timestamp
    uint8_t slot_index;         // This member's assigned slot (0-based)
    uint8_t slot_duration_sec;  // Duration per slot (default: 10 s)
    uint32_t magic;             // 0x53434845 ("SCHE")
} schedule_msg_t;
```

### Priority Formula (Githmi's Formula)

```
P = LinkQuality × 100 + (100 − Battery × 100)
```

Nodes with higher link quality and lower battery get earlier slots — ensuring weak-battery nodes transmit first before potential dropout.

### Slot Timing

```
DATA Phase Start (epoch_us)
│
├─── PHASE_GUARD_MS (5 s) ─── no TDMA activity
│
├─── Slot 0: [epoch + 0×10s, epoch + 1×10s)  → Member with highest P
│
├─── Slot 1: [epoch + 1×10s, epoch + 2×10s)  → Next member
│
└─── ...
```

### Member Behaviour During Slot

1. **Store-first:** Write own sensor data to MSLG
2. **Live send:** ESP-NOW unicast to CH (2 s gap enforcement, ~3-5 sends per slot)
3. **Burst drain:** Pop up to 8 MSLG chunks, decompress, send to CH
4. **Cutoff:** Stop 500 ms before slot end to avoid collision

### TDMA Window Diagnostics

```
╔═══════════════════════════════════════════╗
║ TDMA WINDOW END                          ║
║ Sensor sends: 3 | MSLG drain: 5         ║
╚═══════════════════════════════════════════╝
```

---

## UAV Onboarding (Data Offload)

When an **RF 433 MHz trigger** is detected (drone approaching), the CH transitions to `STATE_UAV_ONBOARDING`:

| Step | Action |
|------|--------|
| 1/8 | Disable RF receiver (prevent re-trigger) |
| 2/8 | Broadcast `CH_BUSY` to members → members HOLD data |
| 3/8 | Stop BLE scanning / advertising |
| 4/8 | Deinit ESP-NOW |
| 5/8 | Connect WiFi STA to UAV hotspot (`WSN_AP` / `raspberry`) |
| 6/8 | HTTP POST `/onboard` → chunked upload stored data → receive ACK |
| 7/8 | Cleanup WiFi → reinit ESP-NOW |
| 8/8 | Broadcast `CH_RESUME` → members resume TDMA → return to `STATE_CH` |

**WiFi credentials:** `UAV_WIFI_SSID "WSN_AP"` / `UAV_WIFI_PASS "raspberry"` / Port `8080`

### Data Upload: 4 KB Chunked Transfer

Stored MSLG data can reach **36–64 KB** decompressed while the ESP32's TCP send buffer is only **5,760 bytes**.
A single `esp_http_client_perform()` call with the full payload stalls the TCP window and causes timeouts.

The upload uses the **open / write / fetch_headers** API instead:

```
esp_http_client_open(client, total_len)     ← TCP connect + send headers
  while (bytes_sent < total_len)
    esp_http_client_write(client, buf+off, 4096)   ← 4 KB at a time
esp_http_client_fetch_headers(client)       ← read server response
esp_http_client_close(client)
```

| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Chunk size | **4,096 bytes** | Fits within TCP SND_BUF (5,760 B), avoids fragmentation |
| `timeout_ms` | **30,000 ms** | Accounts for slow ACKs over short-lived WiFi hotspot |
| STA power save | **`WIFI_PS_NONE`** | Disabled after connect — keeps radio active during upload |

> **Why not `perform()`?** — `perform()` buffers the entire body internally before writing.
> With a 36 KB payload and a 5,760 B TCP window, the lwIP stack must segment into ~25 TCP
> frames. If the STA radio power-saves between ACKs, the AP stops seeing the client, drops
> beacons, and the connection dies (`bcn_timeout → ESP_ERR_HTTP_WRITE_DATA`).

### WiFi Reliability Hardening

| Fix | Side | Detail |
|-----|------|--------|
| `esp_wifi_set_ps(WIFI_PS_NONE)` | ESP32 (STA) | Prevents radio sleep between TCP segments |
| `iwconfig wlan1 power off` | Pi (AP) | Prevents AP beacon drops during idle moments |
| `threaded=True` in Flask | Pi (AP) | Prevents TCP stall while server processes heavy `/data` payload |

---

## SPIFFS Auto-Purge

The CH accumulates data from all sources (own sensors + all member forwards) without any drain path during normal operation. The MSLG file (`data.lz`) will eventually fill the SPIFFS partition.

### Behaviour

When SPIFFS usage reaches **≥ 90%**, the storage manager automatically **deletes all stored data** and continues operation:

```
╔══════════════════════════════════════════════════════════╗
║  SPIFFS 92% FULL — PURGING ALL STORED DATA             ║
╚══════════════════════════════════════════════════════════╝
```

**Files deleted:** `data.lz` (MSLG chunks), `data.txt` (plain-text data), `queue.txt` (upload queue)

### When It Triggers

| Trigger | Description |
|---------|-------------|
| **Pre-flight check** | Checked before every `write_mslg_chunk()` call |
| **Write failure retry** | If an MSLG write fails (SPIFFS full), purge + retry once |

### Design Rationale

In a store-and-forward DTN the only "real" drain is the UAV offload. If no UAV arrives for a long time, accumulating stale data that blocks new writes is worse than losing old data. Fresh sensor readings are more valuable than hour-old ones that the UAV never collected.

### Threshold Configuration

```c
// storage_manager.c
#define SPIFFS_PURGE_THRESHOLD_PCT 90
```

---

## Sensors & Mock Data

### Hardware Sensors

| Sensor | Bus | Address | Measurements |
|--------|-----|---------|-------------|
| **BME280** | I²C | 0x76 | Temperature, humidity, pressure |
| **AHT21** | I²C | 0x38 | Temperature, humidity (auxiliary) |
| **ENS160** | I²C | 0x53 | AQI, eCO₂, TVOC |
| **GY-271** (HMC5883L) | I²C | 0x1E | 3-axis magnetometer |
| **INA219** | I²C | 0x40 | Bus voltage, current, power |
| **INMP441** | I²S | GPIO 5/6/7 | Audio RMS (bio-acoustic) |

### Mock Data (when `ENABLE_MOCK_SENSORS=1`)

When hardware sensors are not present the firmware generates realistic random values every main loop iteration (2 s):

| Field | Range | Unit |
|-------|-------|------|
| `temp_c` | 15.0 – 40.0 | °C |
| `hum_pct` | 30.0 – 95.0 | % |
| `pressure_hpa` | 1005 – 1025 | hPa |
| `aqi` | 1 – 300 | index |
| `eco2_ppm` | 400 – 1200 | ppm |
| `tvoc_ppb` | 10 – 500 | ppb |
| `mag_x/y/z` | −100.0 – 100.0 | µT |
| `audio_rms` | 0.001 – 0.200 | normalised |

`seq_num` increments every main loop iteration, ensuring every payload is unique for deduplication.

### Sensor Payload Structure

```c
typedef struct {
    uint32_t node_id;
    float    temp_c;
    float    hum_pct;
    uint32_t pressure_hpa;
    uint16_t eco2_ppm;
    uint16_t tvoc_ppb;
    uint16_t aqi;
    float    audio_rms;
    float    mag_x, mag_y, mag_z;
    uint64_t timestamp_ms;
    uint32_t seq_num;
    uint8_t  mac_addr[6];
    uint8_t  flags;  // SENSOR_PAYLOAD_FLAG_SENSORS_REAL | BATTERY_REAL
} sensor_payload_t;
```

---

## Communication Stack

### BLE (STELLAR Phase)

- **Advertising:** Custom manufacturer data (Espressif Company ID `0x02E5`)
- **Payload:** `ble_score_packet_t` — node_id, composite score, battery, trust, link quality, WiFi MAC, is_ch flag, seq_num, truncated HMAC
- **Scan interval:** 100 ms (50% duty cycle)
- **Purpose:** Neighbor discovery, score exchange, election input
- **Enhanced Reliability:**
  - Extended mutex timeouts (100ms → 500ms) for race condition mitigation
  - ESP-NOW peer registration validation before neighbor table updates
  - Post-addition verification with comprehensive debug logging
  - Memset initialization for clean neighbor state
  - Table status debugging when neighbor table full
- **Radio Coordination:** 
  - Scanning ENABLED during STELLAR phase (neighbor discovery priority)
  - Scanning DISABLED during DATA phase (ESP-NOW priority, prevents MAC conflicts)
  - Advertising continues throughout (CH beacons, low duty cycle)
- **Authentication:** HMAC validation prevents spoofed neighbor advertisements

### ESP-NOW (DATA Phase)

- **Channel:** 1 (fixed)
- **Encryption:** PMK `pmk1234567890123` / LMK `lmk1234567890123`
- **Max payload:** 250 bytes
- **Radio Coexistence:** BLE scanning DISABLED during DATA phase (prevents MAC conflicts)
- **Success Rate:** >90% (improved from <10% with temporal separation)
- **Enhanced Reliability:** Robust neighbor table management with race condition fixes
- **Uses:**
  - CH → Members: `schedule_msg_t` (TDMA assignment)
  - CH → Members: `ch_status_msg_t` (UAV busy / resume) 
  - Members → CH: `sensor_payload_t` (live sensor data)
  - Members → CH: MSLG chunk burst drain (historical data, FIFO ordered)
- **Store-First Pattern:** All data saved locally before transmission (fault tolerance)
- **Transmission Features:** 
  - Semaphore-protected sends with MAC-layer ACK confirmation
  - Extended mutex timeouts (500ms) for race condition mitigation
  - Post-addition verification of neighbor table entries
  - Time budget management with automatic requeue on slot overrun

### WiFi STA (UAV Onboarding only)

- **SSID:** `WSN_AP` / **Pass:** `raspberry`
- **Server:** `http://<gateway>:8080`
- **Endpoints:** `/onboard` (POST data), `/ack` (POST acknowledgement)

### RF 433 MHz (UAV Trigger)

- **Purpose:** External trigger from approaching UAV
- **Module:** ASK / OOK receiver
- **Effect:** Forces CH into `STATE_UAV_ONBOARDING`

---

## Hardware

### Platform

- **MCU:** ESP32-S3-N16R8 (Dual-core Xtensa LX7 @ 240 MHz)
- **Flash:** 16 MB
- **PSRAM:** 8 MB (Octal mode)
- **SDK:** ESP-IDF v5.3.4

### Pin Configuration

```
I²C Bus:        SDA = GPIO 8,  SCL = GPIO 9,  100 kHz
I²S (INMP441):  WS  = GPIO 5,  SCK = GPIO 6,  SD = GPIO 7,  16 kHz mono
Battery ADC:    GPIO 4 (ADC1_CH3),  220 kΩ / 100 kΩ divider
RF Receiver:    Configured in rf_receiver component
```

### Power Management (PME)

| Mode | Battery | Sample Rate | Deep Sleep |
|------|---------|-------------|------------|
| **Normal** | ≥ 60% | 2 s | No |
| **PowerSave** | 10 – 59% | 60 s | No |
| **Critical** | < 10% | — | 2-hour intervals |

---

## Build, Flash & Monitor

### Prerequisites

- ESP-IDF v5.3+ (tested with v5.3.4)
- Python 3.8+
- USB-UART driver for ESP32-S3

### Quick Start

```bash
cd WSN_main_set/ms_node

# Build
idf.py build

# Flash single node
idf.py -p /dev/ttyUSB0 flash

# Flash + monitor
idf.py -p /dev/ttyUSB0 flash monitor

# Flash all 3 nodes (uses devices.yaml)
./flash_all.sh
```

### Device Manager

```bash
python ../tools/device_manager.py build
python ../tools/device_manager.py list-ports
python ../tools/device_manager.py flash-all
python ../tools/device_manager.py monitor-all
python ../tools/device_manager.py optimize --port /dev/ttyUSB0 audio_interval_ms=300000
```

### Cluster Verification

```bash
python check_cluster.py          # Monitor all nodes, verify election
python get_status.py              # Quick cluster status
python get_cluster_full_info.py   # Detailed info dump
```

---

## Project Structure

```
ms_node/
├── main/
│   ├── ms_node.c              # Entry point, sensor loop, seq_num management
│   ├── state_machine.c        # STELLAR/DATA phase control, CH/Member behaviour
│   ├── state_machine.h        # States: INIT, DISCOVER, CH, MEMBER, UAV, SLEEP
│   ├── config.h               # All tuneable parameters (phases, weights, thresholds)
│   ├── election.c / .h        # STELLAR election logic
│   ├── metrics.c / .h         # Score computation, Lyapunov weights, entropy
│   ├── neighbor_manager.c / .h # Neighbour table, stale cleanup, CH lookup
│   ├── ble_manager.c / .h     # BLE advertising + scanning (STELLAR phase)
│   ├── ble_beacon.c / .h      # BLE advertisement packet encoding
│   ├── ble_gatt_service.c / .h # BLE GATT config / data characteristics
│   ├── esp_now_manager.c / .h # ESP-NOW init/deinit, send/recv, TDMA schedule
│   ├── auth.c / .h            # HMAC authentication for BLE packets
│   ├── persistence.c / .h     # NVS persistence for state across reboots
│   ├── led_manager.c / .h     # Status LED indicators
│   └── compression_bench.c    # Compression micro-benchmark
│
├── components/
│   ├── storage_manager/       # MSLG read/write/pop, SPIFFS mount, auto-purge
│   ├── compression/           # miniz (DEFLATE) + Huffman codecs
│   ├── uav_client/            # WiFi STA connection, HTTP POST onboarding
│   ├── sensors/               # BME280, AHT21, ENS160, GY-271, INA219, INMP441
│   ├── battery/               # ADC battery monitoring
│   ├── pme/                   # Power Management Engine (Normal/PowerSave/Critical)
│   ├── logger/                # SPIFFS block-buffered logger
│   └── rf_receiver/           # 433 MHz ASK/OOK receiver (UAV trigger)
│
├── spiffs_data/               # Files pre-loaded to SPIFFS partition
├── docs/
│   └── MSLG_DATA_FLOW.md     # Detailed data flow analysis & graphs
├── sdkconfig                  # ESP-IDF project configuration
├── sdkconfig.defaults         # Default sdkconfig overrides
├── CMakeLists.txt             # Top-level CMake build
├── flash_all.sh               # Multi-device flash script
├── devices.yaml               # Serial port ↔ node mapping
└── README.md                  # This file
```

---

## Configuration Reference

### Superframe Timing (`config.h`)

| Define | Default | Description |
|--------|---------|-------------|
| `STELLAR_PHASE_MS` | 20 000 | STELLAR (BLE) phase duration |
| `DATA_PHASE_MS` | 20 000 | DATA (ESP-NOW) phase duration |
| `PHASE_GUARD_MS` | 5 000 | Guard before TDMA slots begin |
| `SLOT_DURATION_SEC` | 10 | TDMA slot per member |
| `ELECTION_WINDOW_MS` | 10 000 | Election evaluation window |

### STELLAR Weights (`config.h`)

| Define | Default | Description |
|--------|---------|-------------|
| `WEIGHT_BATTERY` | 0.30 | Battery weight in score |
| `WEIGHT_UPTIME` | 0.20 | Uptime weight |
| `WEIGHT_TRUST` | 0.30 | Trust weight (HMAC + reputation + PDR) |
| `WEIGHT_LINK_QUALITY` | 0.20 | Link quality weight (RSSI + PER) |
| `STELLAR_SCORE_EWMA_ALPHA` | 0.25 | Score smoothing factor |
| `LYAPUNOV_ETA` | 0.05 | Weight convergence learning rate |

### Neighbour & Cluster (`config.h`)

| Define | Default | Description |
|--------|---------|-------------|
| `MAX_NEIGHBORS` | 10 | Max neighbour table size |
| `MAX_CLUSTER_SIZE` | 5 | Max nodes per cluster |
| `NEIGHBOR_TIMEOUT_MS` | 60 000 | Stale neighbour removal |
| `CH_BEACON_TIMEOUT_MS` | 45 000 | CH loss detection timeout |
| `CH_MISS_THRESHOLD` | 10 | Consecutive misses before re-election |
| `CLUSTER_RADIUS_RSSI_THRESHOLD` | −85 dBm | Cluster membership RSSI floor |

### Enhanced Neighbor Management (Recent Improvements)

| Feature | Implementation | Impact |
|---------|---------------|--------|
| **Extended Mutex Timeouts** | 100ms → 500ms (5x increase) | Race condition mitigation |
| **ESP-NOW Peer Registration** | Validate before table updates | Robust peer management |
| **Post-Addition Verification** | Confirm neighbor actually added | Table consistency assurance |
| **Comprehensive Debug Logging** | Full operation tracing | Enhanced troubleshooting |
| **Clean State Initialization** | memset for neighbor structures | Reliable state management |
| **Table Status Monitoring** | Debug dump on capacity issues | Operational transparency |

### Storage (`storage_manager.c`)

| Define | Default | Description |
|--------|---------|-------------|
| `COMPRESSION_MIN_BYTES` | 1 024 | Min payload size for compression |
| `COMPRESSION_LEVEL` | 3 | miniz DEFLATE level |
| `SPIFFS_PURGE_THRESHOLD_PCT` | 90 | Auto-purge when SPIFFS ≥ this % |

### ESP-NOW (`config.h`)

| Define | Default | Description |
|--------|---------|-------------|
| `ESP_NOW_CHANNEL` | 1 | WiFi channel for ESP-NOW |
| `ESP_NOW_PMK` | `"pmk1234567890123"` | Primary Master Key |
| `ESP_NOW_LMK` | `"lmk1234567890123"` | Local Master Key |

### ESP-NOW Communication Reliability (Recent Improvements)

| Feature | Implementation | Impact |
|---------|---------------|--------|
| **Radio Coexistence** | BLE scanning DISABLED during DATA phase | <10% → >90% success rate |
| **Temporal Separation** | Phase-aware radio priority management | Eliminated MAC conflicts |
| **Semaphore Protection** | Prevent concurrent send operations | MAC layer protection |
| **Callback Confirmation** | MAC-layer ACK verification | Reliable delivery confirmation |
| **Store-First Pattern** | Save locally before transmission | Fault tolerance, no data loss |
| **Burst Drain Optimization** | Batch MSLG pop operations | 24x speedup (900ms for 24 chunks) |
| **Time Budget Management** | Automatic requeue on slot overrun | Predictable TDMA timing |

---

## Troubleshooting

### MSLG chunk count stuck / not growing

- **Cause:** `seq_num` was not incrementing → dedup rejected all writes.
- **Fix:** `seq_num` now increments every 2 s main loop (not gated by 60 s sensor interval).

### MSLG never draining on members

- **Cause:** Drain code was nested inside `prepare_upload()` which needs `data.txt`, but all data goes to `data.lz`.
- **Fix:** MSLG burst drain now runs independently of the plain-text queue.

### BLE hex dump spam in logs

- **Cause:** Non-Espressif BLE advertisements were dumped as raw hex.
- **Fix:** Removed hex dump; only Espressif-tagged packets are processed.

### Phase boundary log shows wrong BLE state

- **Cause:** Log was printed before BLE stop/start completed.
- **Fix:** Phase boundary log now prints after BLE transition, includes Role (CH / MEMBER).

### SPIFFS full — no more writes

- **Cause:** CH accumulates data from all nodes without UAV offload.
- **Fix:** Auto-purge at 90% capacity deletes all files and retries the write. See [SPIFFS Auto-Purge](#spiffs-auto-purge).

### WiFi connection fails during UAV onboarding

- **Cause:** Old polling-based `wifi_join()` was unreliable.
- **Fix:** Rewritten with event-driven handler, EventGroup, proper retry logic in `uav_client.c`.

### Build: stack overflow in state machine task

- **Cause:** Compression + decompression buffers on stack.
- **Fix:** `STATE_MACHINE_TASK_STACK_SIZE` increased to 12 288. Compression buffers allocated on heap / PSRAM.

---

## License

This project is provided for educational and research purposes.

---

**Team:** WSNDataFerry — Delay-Tolerant Sensor Network Research
**Hardware:** ESP32-S3-N16R8
**SDK:** ESP-IDF v5.3.4
**Last Updated:** March 2026
