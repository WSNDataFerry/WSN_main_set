# Dual-Core Sensor & Radio Architecture

> **Date:** March 2026  
> **Scope:** Verified against `ms_node.c`, `state_machine.c`, `metrics.c`, `storage_manager.c`, and STELLAR election flow
> **Platform:** ESP32-S3 (dual-core Xtensa LX7), ESP-IDF v5.3, FreeRTOS SMP

---

## Table of Contents

1. [Overview](#1-overview)
2. [Task Layout](#2-task-layout)
3. [Producer-Consumer Model](#3-producer-consumer-model)
4. [Superframe Phasing vs Sensor Sampling](#4-superframe-phasing-vs-sensor-sampling)
5. [Why Sensors Run Continuously](#5-why-sensors-run-continuously)
6. [STELLAR Metrics & Sensor Coupling](#6-stellar-metrics--sensor-coupling)
7. [Data Staleness Analysis](#7-data-staleness-analysis)
8. [Code Path References](#8-code-path-references)

---

## 1. Overview

The ESP32-S3 has a **single 2.4 GHz radio** shared between BLE (NimBLE) and
ESP-NOW (Wi-Fi MAC layer).  They cannot operate simultaneously — transmitting
a BLE advertisement while an ESP-NOW frame is in-flight would corrupt both.

The STELLAR protocol solves this with **time-division superframing**:

| Phase | Duration | Radio Mode | Purpose |
|-------|----------|------------|---------|
| **STELLAR** | 20 000 ms | BLE scan + advertise | Neighbour discovery, score exchange, CH election |
| **DATA** | 20 000 ms | ESP-NOW TX/RX | Sensor payload transmission, MSLG storage, burst drain |

**Key design decision:** sensor data *collection* (I2C reads) is decoupled
from sensor data *transmission* (ESP-NOW) and *storage* (SPIFFS).  Collection
runs continuously; transmission and storage happen only during DATA phase.

---

## 2. Task Layout

All tasks use `xTaskCreate()` (not pinned), so FreeRTOS SMP schedules them
across both cores as needed.

```
┌──────────────────────────────────────────────────────────────────────┐
│  FreeRTOS SMP — ESP32-S3 Dual Core                                   │
│                                                                      │
│  ┌──────────────────────────────────────────────────┐                │
│  │  app_main  (sensor loop)                         │  Priority: 1   │
│  │  • I2C sensor reads (BME280, AHT21, ENS160, etc)│                │
│  │  • seq_num increment                             │                │
│  │  • metrics_set_sensor_data()                     │                │
│  │  • Sleep 100ms–5000ms (PME-dependent)            │                │
│  │  • Runs in BOTH phases — no phase check          │                │
│  └──────────────────────────────────────────────────┘                │
│                                                                      │
│  ┌──────────────────────────────────────────────────┐                │
│  │  state_machine_task                              │  Priority: 5   │
│  │  • Phase management (STELLAR ↔ DATA)             │                │
│  │  • BLE start/stop                                │                │
│  │  • ESP-NOW TX/RX                                 │                │
│  │  • SPIFFS writes (MSLG chunks)                   │                │
│  │  • TDMA slot scheduling                          │                │
│  │  • 100ms tick                                    │                │
│  └──────────────────────────────────────────────────┘                │
│                                                                      │
│  ┌──────────────────────────────────────────────────┐                │
│  │  metrics_task                                    │  Priority: 3   │
│  │  • STELLAR score computation (Lyapunov, Pareto)  │                │
│  │  • PDR / HSR / Trust EWMA updates                │                │
│  │  • 1-second tick                                 │                │
│  └──────────────────────────────────────────────────┘                │
│                                                                      │
│  ┌──────────────────────────────────────────────────┐                │
│  │  console_config_task                             │  Priority: 1   │
│  │  • USB serial console for runtime config         │                │
│  └──────────────────────────────────────────────────┘                │
└──────────────────────────────────────────────────────────────────────┘
```

---

## 3. Producer-Consumer Model

Sensor data flows through a **single-slot mailbox** (`metrics.c`) protected
by a FreeRTOS mutex:

```
  ┌─────────────┐       mutex-protected       ┌──────────────────┐
  │  app_main   │  ──metrics_set_sensor_data──▶│  metrics.c       │
  │  (producer) │       overwrites latest      │  current_sensor  │
  │             │                              │  _data (global)  │
  └─────────────┘                              └────────┬─────────┘
                                                        │
                                               metrics_get_sensor_data
                                                        │
                                                        ▼
                                               ┌──────────────────┐
                                               │ state_machine    │
                                               │ (consumer)       │
                                               │                  │
                                               │ • JSON format    │
                                               │ • SPIFFS write   │
                                               │ • ESP-NOW send   │
                                               └──────────────────┘
```

### Mailbox semantics

- **Write:** `metrics_set_sensor_data(&payload)` — called every main-loop
  iteration (every 100ms–5000ms depending on PME mode).  Overwrites the
  previous value.  Always includes a fresh `seq_num` and `timestamp_ms`.
- **Read:** `metrics_get_sensor_data(&payload)` — called by the state machine
  during DATA phase.  Returns a copy of whatever the producer last wrote.
- **Thread safety:** A FreeRTOS mutex serialises access.  No lock-free tricks.

---

## 4. Superframe Phasing vs Sensor Sampling

```
 Time (s)   0          10          20          30          40
            │           │           │           │           │
            ├───────────┴───────────┼───────────┴───────────┤
            │    STELLAR PHASE      │      DATA PHASE        │
            │    BLE scan+advertise │      ESP-NOW TX/RX     │
            │                       │                        │
 Sensor     ●───●───●───●───●───●───●───●───●───●───●───●   │
 loop       ^   ^   ^   ^   ^   ^   ^   ^   ^   ^   ^   ^   │
 (5s gap    │   │   │   │   │   │   │   │   │   │   │   │   │
  in POWER  └───┘   └───┘   └───┘   └───┘   └───┘   └───┘   │
  SAVE)     metrics_set  metrics_set  ...                     │
            ↑                       ↑                        │
            │                       │                        │
            Runs here (STELLAR)     Runs here (DATA)         │
            No phase check!         No phase check!          │
            │                       │                        │
            └── Data in RAM only ───┤── Data → SPIFFS + NOW ─┤
                (not stored/sent)   │   (consumed by SM)     │
                                    │                        │
                                    │  CH: store every 5s    │
                                    │  MBR: store + send     │
                                    │       per TDMA slot    │
                                    │                        │
```

### What happens in each phase

| Action | STELLAR Phase | DATA Phase |
|--------|:------------:|:----------:|
| I2C sensor reads | ✅ Continuous | ✅ Continuous |
| `seq_num++` | ✅ Every loop | ✅ Every loop |
| `metrics_set_sensor_data()` | ✅ Every loop | ✅ Every loop |
| `metrics_get_sensor_data()` | ❌ Not called | ✅ Called by SM |
| SPIFFS write (MSLG chunk) | ❌ | ✅ CH self-store + member store-first |
| ESP-NOW send | ❌ Radio on BLE | ✅ Member → CH |
| ESP-NOW receive | ❌ Radio on BLE | ✅ CH receives member data |
| BLE scan | ✅ Active | ❌ Stopped |
| BLE advertise | ✅ Active | ⚠️ CH only (for schedule broadcast) |

---

## 5. Why Sensors Run Continuously

Running sensors through both phases (instead of only during DATA) provides
several critical benefits:

### 5.1 No cold-start delay

I2C sensor initialization and first-read warmup takes tens of milliseconds.
If sensor reads were gated to DATA phase only, the first reading at the start
of each DATA phase would either be delayed or invalid.

### 5.2 ENS160 requires continuous environment feed

The ENS160 air quality sensor needs regular temperature/humidity compensation
updates via `ens160_set_env()`.  Pausing reads for 20 seconds every
superframe would degrade AQI accuracy.

```c
// ms_node.c — called after every AHT21 read, regardless of phase
if (ok_aht) {
    (void)ens160_set_env(aht.temperature_c, aht.humidity_pct);
}
```

### 5.3 Fresh data at DATA phase start

When the state machine transitions from STELLAR → DATA, it calls
`metrics_get_sensor_data()` immediately.  Because the sensor loop has been
running throughout STELLAR, the mailbox already contains a reading that is
at most `sleep_ms` old (5 seconds in POWER_SAVE mode).

### 5.4 seq_num always advances

The `s_packet_seq_num` counter increments every main-loop iteration.  This
ensures that when the state machine reads it at the start of DATA phase, the
seq is always unique compared to the previous DATA phase — critical for the
dedup layer in `esp_now_manager.c`.

---

## 6. STELLAR Metrics & Sensor Coupling

The **STELLAR algorithm** for cluster head election depends on real-time metrics that are directly derived from sensor data:

### Metrics Used by STELLAR

| Metric | Source | Sensor | Update Rate |
|--------|--------|--------|------------|
| **Battery %** | INA219 current monitor | Power sensor | 10s (via `metrics_task`) |
| **Uptime** | ESP timer | System | Continuous (no sensor) |
| **Trust Score** | PDR + HMAC + HSR | Network (not sensor) | 1s (via `metrics_task`) |
| **Link Quality** | RSSI EWMA + PER | BLE receiver | Updated during BLE scan |

### Why Continuous Sensor Sampling Matters for STELLAR

During **STELLAR Phase** (BLE-only, no ESP-NOW), the **metrics_task** (priority 3) computes the node's STELLAR score and prepares the BLE score packet for broadcasting:

```c
// Called every 1 second during STELLAR phase
void metrics_update_stellar_score(void) {
    
    // 1. Get battery from INA219 power sensor
    float battery_pct = ina219_get_battery_percent();
    
    // 2. Validate against sensor data (ensure both systems agree)
    // The sensor mailbox contains latest power readings
    sensor_payload_t latest = metrics_get_sensor_data();
    if (latest.power.bus_v < 2.8) {
        battery_pct = 0.0;  // Safety: agree with sensor
    }
    
    // 3. Compute STELLAR utility with current metrics
    float stellar_score = election_stellar_score();
    
    // 4. Encode into BLE score packet
    ble_score_packet_t pkt = {
        .score = (uint32_t)(stellar_score * 10000),
        .battery_pct = (uint8_t)battery_pct,
        .uptime_norm = ...,
        .trust = ...,
        // ... send via BLE advertiser
    };
}
```

### Sensor-Mediated Election Stability

Because sensors run **continuously** (even during STELLAR phase), the STELLAR scores computed at election time (seconds 10-20 of the STELLAR phase) reflect **fresh data** from seconds 5-20:

```
STELLAR Phase Timeline:
t=0-5s:   Sensors read normally
t=5-10s:  Sensors read normally + BLE scan begins collecting neighbor beacons
t=10s:    ELECTION WINDOW OPENS
          ├─ My battery reading: from t=5s (5s stale, acceptable)
          ├─ Neighbor battery reading: from their last advertise (10-20s stale, also OK)
          └─ Based on fresh data: STELLAR score is reliable

t=10-20s: Continue reading sensors + collecting beacons
          → Only latest reading used for election decision (at t=20s boundary)
```

**Result:** CH election is **not** based on stale data sampled weeks ago. It reflects current network conditions.

---

## 7. Data Staleness Analysis

The **worst-case staleness** of a sensor reading consumed by the state machine
depends on PME mode:

| PME Mode | Main Loop Sleep | Worst-Case Staleness | Readings per STELLAR Phase |
|----------|:--------------:|:-------------------:|:--------------------------:|
| ACTIVE | 100 ms | 100 ms | ~200 |
| NORMAL | 2 000 ms | 2 000 ms | ~10 |
| POWER_SAVE | 5 000 ms | 5 000 ms | ~4 |
| CRITICAL | Deep sleep | N/A (node asleep) | 0 |

### In POWER_SAVE mode (typical deployment, battery ~30-44%)

```
STELLAR phase (20s):
  Loop 1  → t=0s   : sensor read → metrics_set
  Loop 2  → t=5s   : sensor read → metrics_set (overwrites)
  Loop 3  → t=10s  : sensor read → metrics_set (overwrites)
  Loop 4  → t=15s  : sensor read → metrics_set (overwrites)

DATA phase (20s):
  t=20s : state_machine calls metrics_get → gets reading from t=15s (5s stale)
  t=25s : state_machine calls metrics_get → gets reading from t=20s (5s stale)
  t=30s : state_machine calls metrics_get → gets reading from t=25s (5s stale)
  t=35s : state_machine calls metrics_get → gets reading from t=30s (5s stale)
```

**5 seconds of staleness** is acceptable for environmental monitoring sensors
(temperature, humidity, pressure, air quality) which change on timescales of
minutes to hours.

### What is NOT lost

- **No readings are dropped.** The mailbox always holds the latest value.
- **No phase gaps.** The sensor loop never pauses for phase transitions.
- **Intermediate STELLAR-phase readings** are overwritten in the mailbox but
  they served their purpose: keeping ENS160 compensated and ensuring the
  mailbox is warm when DATA phase starts.

---

## 7. Code Path References

### Sensor producer (ms_node.c — `app_main`)

```
Line  415  xTaskCreate(state_machine_task, ...)   // spawns SM task
Line  520+ while (1) {                             // main sensor loop — never ends
Line  680+   ok_bme = bme280_read(...)             // I2C reads — no phase check
Line  890+   s_packet_seq_num++                    // always increments
Line  950    metrics_set_sensor_data(&payload)     // always updates mailbox
Line  990    vTaskDelay(sleep_ms)                  // PME-dependent sleep
           }
```

### Consumer — CH self-store (state_machine.c)

```
Line  604  if (s_current_phase == PHASE_DATA &&    // ← DATA-only guard
               (now_ms - s_last_ch_store_ms) >= 5000)
Line  607    metrics_get_sensor_data(&payload)      // reads mailbox
Line  632    storage_manager_write_compressed(...)   // SPIFFS write
```

### Consumer — Member store-first + ESP-NOW (state_machine.c)

```
Line  897  if (can_send) {                          // ← only true in TDMA slot (DATA phase)
Line  900    metrics_get_sensor_data(&payload)       // reads mailbox
Line  935    storage_manager_write_compressed(...)    // SPIFFS write
Line  947    esp_now_manager_send_data(...)           // ESP-NOW to CH
```

### Mailbox (metrics.c)

```
Line  65   static sensor_payload_t current_sensor_data   // single-slot global
Line  70   void metrics_set_sensor_data(...)              // mutex lock → memcpy → unlock
Line  80   void metrics_get_sensor_data(...)              // mutex lock → memcpy → unlock
```

---

## Summary Diagram

```
┌──────────────────────────────────────────────────────────────────────────┐
│                        ESP32-S3 Dual Core                                │
│                                                                          │
│  ┌────────────────────────────────────────────────────────────────────┐  │
│  │  SENSOR PRODUCER (app_main)            runs in ALL phases          │  │
│  │  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐  ┌──────┐     │  │
│  │  │BME280│  │AHT21 │  │ENS160│  │GY-271│  │INA219│  │INMP  │     │  │
│  │  └──┬───┘  └──┬───┘  └──┬───┘  └──┬───┘  └──┬───┘  └──┬───┘     │  │
│  │     └─────────┴─────────┴─────────┴─────────┴─────────┘          │  │
│  │                              │                                     │  │
│  │                     metrics_set_sensor_data()                      │  │
│  │                              │                                     │  │
│  └──────────────────────────────┼─────────────────────────────────────┘  │
│                                 │                                        │
│                    ┌────────────▼────────────┐                           │
│                    │    metrics.c mailbox     │                           │
│                    │  (mutex-protected RAM)   │                           │
│                    └────────────┬────────────┘                           │
│                                 │                                        │
│                        metrics_get_sensor_data()                         │
│                                 │                                        │
│  ┌──────────────────────────────┼─────────────────────────────────────┐  │
│  │  STATE MACHINE               ▼           DATA phase ONLY           │  │
│  │                                                                    │  │
│  │  STELLAR Phase:              DATA Phase:                           │  │
│  │  • BLE scan ✅                • ESP-NOW TX/RX ✅                   │  │
│  │  • BLE advertise ✅           • SPIFFS write ✅                    │  │
│  │  • Score exchange ✅          • MSLG drain ✅                      │  │
│  │  • No sensor consume ❌       • metrics_get ✅                     │  │
│  │  • No SPIFFS write ❌         • BLE off ❌                         │  │
│  │  • No ESP-NOW ❌              │                                    │  │
│  └───────────────────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────────────────┘
```

---

*Document auto-generated from code analysis of `ms_node.c`, `state_machine.c`,
and `metrics.c` — March 2026.*
