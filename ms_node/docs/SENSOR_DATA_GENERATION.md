# Sensor Data Generation: From Hardware to Network

## Overview

This document explains how the MS Node generates, processes, stores, and transmits sensor data across the wireless sensor network (WSN). 

**Key Timeline:**
- **Every 2 seconds:** Main loop reads sensors or generates mock data
- **Every 20 seconds (DATA phase):** Data is stored to SPIFFS and transmitted via ESP-NOW to cluster head
- **Every 40 seconds:** Full STELLAR superframe cycle completes
- **On UAV arrival:** All accumulated data uploaded via WiFi

---

## Table of Contents

1. [Data Generation Sources](#1-data-generation-sources)
2. [Main Loop Data Collection](#2-main-loop-data-collection)
3. [Sensor Data Structure](#3-sensor-data-structure)
4. [Data Flow Pipeline](#4-data-flow-pipeline)
5. [Storage & Compression](#5-storage--compression)
6. [Transmission via TDMA](#6-transmission-via-tdma)
7. [Cluster Head Data Aggregation](#7-cluster-head-data-aggregation)
8. [UAV Offloading](#8-uav-offloading)
9. [Debugging & Verification](#9-debugging--verification)

---

## 1. Data Generation Sources

### 1.1 Real Sensors (Hardware)

Six different sensors measure environmental conditions:

| Sensor | Interface | Measurements | Update Rate (Default) | File |
|--------|-----------|---------------|-----------------------|------|
| **BME280** | I2C (0x76) | Temperature, Humidity, Pressure | 60s | `bme280_sensor.c` |
| **AHT21** | I2C (0x38) | Temperature, Humidity | 60s | `aht21_sensor.c` |
| **ENS160** | I2C (0x53) | TVOC, eCO2, AQI | 120s | `ens160_sensor.c` |
| **HMC5883L** | I2C (0x0D) | 3-axis Magnetometer (μT) | 60s | `gy271_sensor.c` |
| **INA219** | I2C (0x40) | Bus voltage, shunt current | 10s | `ina219_sensor.c` |
| **INMP441** | I2S | Audio PCM 16-bit @ 16kHz | 300s (disabled default) | `inmp441_sensor.c` |

**Battery Monitoring:** INA219 provides **battery percentage** calculation:
```c
voltage = ina219_read_bus_voltage();  // 2.8V to 4.2V
battery_pct = (voltage - 2.8) / (4.2 - 2.8) * 100;
```

### 1.2 Mock Sensors (Simulation)

When running on test boards without hardware sensors, the code generates **realistic synthetic data**:

```c
// components/sensors/src/mock_sensors.c

mock_data_t generate_mock_sensor() {
    
    // Temperature: sine wave 15-30°C
    float temp = 22.5 + 7.5 * sin(current_time_s / 3600.0);
    
    // Humidity: inverse sine 40-80%
    float humidity = 60.0 + 20.0 * cos(current_time_s / 3600.0);
    
    // Pressure: slight variation around 1013 hPa
    float pressure = 1013.25 + 3.0 * sin(current_time_s / 7200.0);
    
    // Air quality: random variation
    uint16_t aqi = 1 + random(0, 5);
    uint16_t tvoc = 50 + random(-20, 50);
    
    // Magnetometer: realistic micro-tesla readings
    float mag_x = 20.0 + random(-5, 5);
    float mag_y = -15.0 + random(-5, 5);
    float mag_z = 40.0 + random(-5, 5);
    
    return {temp, humidity, pressure, aqi, tvoc, mag_x, mag_y, mag_z};
}
```

**Why Mock Sensors?**
- Development without hardware
- Reproducible test scenarios
- Deterministic data for timing analysis
- Continuous operation (real sensors may fail/timeout)

---

## 2. Main Loop Data Collection

### 2.1 Entry Point: `app_main()` in ms_node.c

The main thread continuously collects data every few seconds:

```c
void app_main(void) {
    // Initialize sensors (real or mock)
    sensors_init();
    metrics_init();
    
    // 2-second main loop
    while (1) {
        // 1. Get current metrics (battery, trust, link quality)
        metrics_update_ewma();
        
        // 2. Increment sequence number (used for dedup)
        seq_num++;
        
        // 3. Read all sensors (real or mock)
        sensor_payload_t payload = {
            .ts_ms = esp_timer_get_time() / 1000,
            .seq_num = seq_num,
            .env = {
                .bme_t = bme280_read_temp(),      // I2C read
                .bme_h = bme280_read_humidity(),  // I2C read
                .bme_p = bme280_read_pressure(),  // I2C read
                .aht_t = aht21_read_temp(),       // I2C read
                .aht_h = aht21_read_humidity()    // I2C read
            },
            .gas = {
                .aqi = ens160_read_aqi(),         // I2C read
                .tvoc = ens160_read_tvoc(),       // I2C read
                .eco2 = ens160_read_eco2()        // I2C read
            },
            .mag = {
                .x = gy271_read_x(),              // I2C read
                .y = gy271_read_y(),              // I2C read
                .z = gy271_read_z()               // I2C read
            },
            .power = {
                .bus_v = ina219_read_bus_v(),     // I2C read
                .shunt_mv = ina219_read_shunt_mv(), // I2C read
                .i_ma = ina219_read_current_ma()  // I2C read
            }
        };
        
        // 4. Store in shared mailbox (mutex-protected)
        metrics_set_sensor_data(&payload);
        
        // 5. Sleep based on power mode
        int sleep_ms = pme_get_sleep_duration();  // 100ms to 5000ms
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
    }
}
```

### 2.2 Data Types

```c
// Type of each sensor reading
typedef struct {
    float bme_t;   // °C
    float bme_h;   // %
    float bme_p;   // hPa
    float aht_t;   // °C
    float aht_h;   // %
} env_t;

typedef struct {
    uint16_t aqi;   // 0-5
    uint16_t tvoc;  // ppb
    uint16_t eco2;  // ppm
} gas_t;

typedef struct {
    float x, y, z;  // μT
} mag_t;

typedef struct {
    float bus_v;    // V
    float shunt_mv; // mV
    float i_ma;     // mA
} power_t;

typedef struct {
    uint64_t ts_ms;      // Timestamp (ms)
    uint32_t seq_num;    // Sequence for dedup
    env_t env;
    gas_t gas;
    mag_t mag;
    power_t power;
} sensor_payload_t;  // ~52 bytes binary
```

---

## 3. Sensor Data Structure

### 3.1 In-Memory Representation (metrics.c mailbox)

Data is cached in shared memory, protected by mutex:

```c
// metrics.c — global state
static sensor_payload_t g_current_sensor_data;
static StaticSemaphore_t g_data_mutex_buffer;
static SemaphoreHandle_t g_data_mutex;

// Called by app_main every 2s
void metrics_set_sensor_data(const sensor_payload_t *payload) {
    xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(5000));  // Acquire lock
    memcpy(&g_current_sensor_data, payload, sizeof(sensor_payload_t));
    xSemaphoreGive(g_data_mutex);  // Release lock
}

// Called by state_machine during DATA phase
void metrics_get_sensor_data(sensor_payload_t *payload) {
    xSemaphoreTake(g_data_mutex, pdMS_TO_TICKS(5000));
    memcpy(payload, &g_current_sensor_data, sizeof(sensor_payload_t));
    xSemaphoreGive(g_data_mutex);
}
```

**Characteristics:**
- **Always fresh:** Contains the latest sensor read (max 5 seconds stale in POWER_SAVE mode)
- **Thread-safe:** Mutex prevents simultaneous reads/writes
- **Overwrite-based:** New reading overwrites old - no queue
- **Non-blocking:** If lock contended, returns error (not used during critical sections)

### 3.2 JSON Representation (for SPIFFS storage)

When data is stored to SPIFFS, it's converted to JSON:

```json
{
  "ts_ms": 1234567890,
  "seq": 12345,
  "env": {
    "bme_t": 23.5,
    "bme_h": 65.2,
    "bme_p": 1013.25,
    "aht_t": 23.3,
    "aht_h": 64.8
  },
  "gas": {
    "aqi": 1,
    "tvoc": 125,
    "eco2": 450
  },
  "mag": {
    "x": 12.34,
    "y": -5.67,
    "z": 45.12
  },
  "power": {
    "bus_v": 3.700,
    "shunt_mv": 0.250,
    "i_ma": 22.5
  }
}
```

**Size:** ~250-300 bytes (uncompressed)

---

## 4. Data Flow Pipeline

### 4.1 Complete Lifecycle (Per Superframe)

```
TIME: 0s (STELLAR PHASE START)
  ├─ t=0s:    Sensors read every 2-5s (continuous)
  ├─ t=2s:    seq_num=1, metrics_set(payload)  → stored in RAM mailbox
  ├─ t=4s:    seq_num=2, metrics_set(payload)  → overwrites previous
  ├─ t=6s:    seq_num=3, metrics_set(payload)  → overwrites previous
  ├─ t=8s:    seq_num=4, metrics_set(payload)  → overwrites previous
  ├─ t=10s:   seq_num=5, metrics_set(payload)  → overwrites previous
  ├─ t=12s:   seq_num=6, metrics_set(payload)  → overwrites previous
  ├─ t=14s:   seq_num=7, metrics_set(payload)  → overwrites previous
  ├─ t=16s:   seq_num=8, metrics_set(payload)  → overwrites previous
  ├─ t=18s:   seq_num=9, metrics_set(payload)  → overwrites previous
  │
  └─ STELLAR PHASE: BLE scanning active, election running
                    No SPIFFS writes, no ESP-NOW sends

TIME: 20s (DATA PHASE START)
  ├─ t=20s:   state_machine transitions to DATA phase
  │           ├─ BLE scanning disabled
  │           ├─ ESP-NOW enabled
  │           └─ TDMA schedule broadcast: slot timings
  │
  ├─ t=20-25s: GUARD PERIOD
  │           └─ CH broadcasts schedule, members prepare
  │
  ├─ t=25s:   TDMA SLOT 0 (Member 0 or CH if only node)
  │           ├─ metrics_get(payload) — read latest from mailbox
  │           ├─ convert to JSON string
  │           ├─ STORE-FIRST: write_compressed() → SPIFFS /data.lz
  │           │   └─ Creates MSLG chunk (header + compressed JSON)
  │           ├─ LIVE SEND: esp_now_send() to CH
  │           │   └─ Sends binary payload (~52 bytes)
  │           └─ BURST DRAIN: pop_mslg_chunks() × up to 8
  │               └─ Sends decompressed old chunks to CH
  │
  ├─ t=27s, t=29s: Additional LIVE SENDs (2s gap enforcement)
  │
  ├─ t=30s:   Continue sensor reads (seq_num=11,12,13...)
  │
  ├─ t=35s:   TDMA SLOT 1 (Member 1, if exists)
  │           └─ Same flow as Slot 0
  │
  ├─ t=40s:   End of DATA phase, superframe resets to STELLAR

TIME: 40s+ (REPEAT)
  └─ Back to STELLAR phase, cycle repeats
```

### 4.2 Producer-Consumer Flow Diagram

```
┌─────────────────────────────────────────────────────────┐
│                    SENSOR DATA GENERATION              │
│                                                         │
│  1. I2C/I2S reads or mock data generation             │
│     (app_main, every 2-5s)                            │
│                                                         │
└────────────────┬────────────────────────────────────────┘
                 │
                 ▼
         ┌───────────────┐
         │   mailbox     │ (metrics.c)
         │  (RAM, 52B)   │ Mutex-protected
         └───────┬───────┘ Always holds latest
                 │
         ┌───────┴────────┬──────────────┐
         │                │              │
    (STELLAR phase)    (DATA phase)    (UAV mode)
    [idle - no read]   [consumed]      [uploaded]
         │                │              │
         ▼                ▼              ▼
    Not used        STATE_MACHINE    WiFi upload
                    metrics_get()    to Rasp Pi
                         │
         ┌───────────────┴───────────┐
         │                           │
         ▼                           ▼
    JSON string              Binary format
    250-300B                 ~52B
         │                           │
         ▼                           ▼
    compression              Direct send
    (miniz deflate)          ESP-NOW
    ~60% ratio               to CH
         │                           │
         ▼                           ▼
    MSLG chunk              CH receives
    (header+data)           (stored in CH's
    ~100B                    SPIFFS too)
         │
    stored in               
    /spiffs/data.lz
```

---

## 5. Storage & Compression

### 5.1 MSLG Format (SPIFFS)

When STATE_MACHINE calls `storage_manager_write_compressed()`:

```c
// state_machine.c — during DATA phase
if (state == STATE_CH || state == STATE_MEMBER) {
    if (in_tdma_slot || (state == STATE_CH && time % 5000 == 0)) {
        
        // Convert sensor payload to JSON string
        char json_str[512];
        snprintf(json_str, sizeof(json_str), 
            "{\"ts_ms\": %llu, \"seq\": %u, ..., \"power\": {...}}",
            payload.ts_ms, payload.seq_num, ...);
        
        // Write to SPIFFS with compression
        storage_manager_write_compressed(
            (uint8_t*)json_str,
            strlen(json_str)
        );
    }
}
```

### 5.2 Chunk Structure

```
┌──────────────────────────────────────────────┐
│  MSLG CHUNK (stored in /spiffs/data.lz)      │
├──────────────────────────────────────────────┤
│ HEADER (32 bytes)                            │
│  magic       = 0x4D534C47 ('MSLG')           │
│  version     = 2                             │
│  algo        = 1 (miniz deflate)             │
│  level       = 3 (compression level)         │
│  raw_len     = 287 (JSON size)               │
│  data_len    = 115 (compressed size)         │
│  crc32       = 0xABCD1234 (payload CRC)      │
│  node_id     = 0x10:20:BA:4D:F0:3C          │
│  timestamp   = 12345678 (boot seconds)       │
│  reserved    = 0                             │
├──────────────────────────────────────────────┤
│ PAYLOAD (115 bytes, compressed)              │
│  [gzip/deflate compressed JSON]              │
│  (60% smaller than raw JSON)                 │
└──────────────────────────────────────────────┘

Total chunk size: 32 + 115 = 147 bytes
```

### 5.3 Compression Example

```
Raw JSON (287 bytes):
{"ts_ms":1234567890,"env":{"bme_t":23.5,"bme_h":65.2,...},...}

Compressed (115 bytes, miniz deflate):
[Binary data with repeated patterns removed]

Ratio: 115 / 287 = 40% (60% size reduction)
```

**Compression Benefits:**
- Saves ~60% of SPIFFS space
- 1 week of data fits in 2MB partition
- Faster UAV upload (less WiFi bandwidth)

---

## 6. Transmission via TDMA

### 6.1 TDMA Slot Transmission (Member Node)

When a member's TDMA slot arrives (e.g., slot 0: t=25-35s):

```c
// state_machine.c — TDMA SLOT 0 logic
if (current_slot_index == 0 && in_data_phase) {
    
    // ===== STORE-FIRST =====
    sensor_payload_t payload;
    metrics_get_sensor_data(&payload);      // Read from mailbox
    
    char json_str[512];
    snprintf(json_str, sizeof(json_str), "{...}");
    storage_manager_write_compressed((uint8_t*)json_str, strlen(json_str));
    ESP_LOGI(TAG, "STORE-FIRST: +1 MSLG chunk");
    
    // ===== LIVE SEND =====
    for (int send_attempt = 0; send_attempt < 5; send_attempt++) {
        
        esp_now_send_data(ch_mac, &payload, sizeof(payload));
        vTaskDelay(pdMS_TO_TICKS(2000));  // 2s gap between sends
        
        ESP_LOGI(TAG, "LIVE SEND #%d: seq=%u", send_attempt + 1, payload.seq_num);
    }
    
    // ===== BURST DRAIN =====
    int chunks_drained = 0;
    while (chunks_drained < 8) {
        
        mslg_chunk_t chunk = storage_manager_pop_chunk();
        if (chunk == NULL) break;  // No more chunks
        
        // Decompress if needed
        if (chunk->header.algo == 1) {
            decompress_miniz(&chunk->payload, ...);
        }
        
        // Send decompressed chunk to CH
        esp_now_send_data(ch_mac, &chunk->payload, chunk->header.raw_len);
        
        chunks_drained++;
        ESP_LOGI(TAG, "BURST DRAIN: #%d (seq=%u)", chunks_drained, chunk->seq_num);
    }
}
```

### 6.2 CH Node: Data Reception & Aggregation

When CH receives data from members:

```c
// esp_now_manager.c — receive callback
static void on_recv_cb(const uint8_t *mac_addr, const uint8_t *data, int len) {
    
    sensor_payload_t *payload = (sensor_payload_t*)data;
    
    // ===== DEDUPLICATION CHECK =====
    if (dedup_table_contains(payload->node_id, payload->seq_num)) {
        ESP_LOGI(TAG, "DROP: Duplicate seq=%u from node=%u", 
                 payload->seq_num, payload->node_id);
        return;  // Ignore duplicate
    }
    
    // ===== STORE CH'S COPY =====
    ch_write_sensor_data(payload);  // CH appends to its own SPIFFS
    
    // ===== UPDATE DEDUP TABLE =====
    dedup_table_add(payload->node_id, payload->seq_num);
    
    ESP_LOGI(TAG, "CH RX: seq=%u from node=%u, RSSI=%d dBm",
             payload->seq_num, payload->node_id, rssi);
}
```

---

## 7. Cluster Head Data Aggregation

### 7.1 CH Self-Storage

The CH node stores its own sensor data **every 5 seconds** during DATA phase (not TDMA-slotted):

```c
// state_machine.c — CH behavior during DATA phase
if (state == STATE_CH && in_data_phase) {
    
    // Every 5 seconds
    if (time_ms % 5000 == 0) {
        
        // Read own sensor data
        sensor_payload_t my_data;
        metrics_get_sensor_data(&my_data);
        
        // Write to SPIFFS
        storage_manager_write_compressed((uint8_t*)json_str, strlen(json_str));
        
        // Also increment seq_num for consistency
        ch_seq_num++;
        
        ESP_LOGI(TAG, "CH SELF-STORE: seq=%u", ch_seq_num);
    }
}
```

### 7.2 CH Data Summary

```
CH Node /spiffs/data.lz Contents After 40 seconds:

Superframe 1:
  ├─ CH self-store (t=20s, 25s, 30s, 35s, 40s) — 5 chunks
  ├─ Member 0 live sends (t=25s, 27s, 29s) — 3 chunks
  ├─ Member 1 live sends (t=35s, 37s, 39s) — 3 chunks
  ├─ Member 0 burst drain (old chunks from SPIFFS) — up to 8 chunks
  └─ Member 1 burst drain (old chunks from SPIFFS) — up to 8 chunks

Total after 1 superframe: ~16-27 chunks depending on activity

Size estimate (147 bytes per chunk):
  20 chunks × 147 bytes = 2.94 KB per superframe
  ~2.94 KB × 2160 superframes/day = 6.4 MB/day
```

---

## 8. UAV Offloading

### 8.1 UAV Arrival Detection (RF Trigger)

When RF code 22 is received (RMT IR receiver on GPIO 21):

```c
// rf_receiver.c — IR code detection
static void on_rf_code_received(uint32_t code) {
    
    if (code == 0x16) {  // Code 22 (0x16 in hex)
        
        ESP_LOGI(TAG, "RF TRIGGER RECEIVED: Code 22 (UAV detected)");
        
        // Signal state machine to enter onboarding
        xQueueSend(g_state_queue, &event, 0);
    }
}
```

### 8.2 CH Initiates WiFi Upload

```c
// state_machine.c — UAV_ONBOARDING state
case STATE_UAV_ONBOARDING: {
    
    // 1. Broadcast CH_BUSY to members (members pause TDMA)
    esp_now_manager_broadcast_ch_status(CH_STATUS_UAV_BUSY);
    vTaskDelay(pdMS_TO_TICKS(200));
    
    // 2. Deinit ESP-NOW (stop WiFi MAC)
    esp_now_manager_deinit();
    
    // 3. WiFi STA connection
    uav_client_run_onboarding();
    
    // 4. Upload all SPIFFS data in 4KB chunks
    uint8_t buffer[4096];
    while (storage_manager_pop_multiple_chunks(buffer, 4096) > 0) {
        
        esp_http_client_write(..., buffer, chunk_size);
        // Typical: 20-30 chunks × 4KB = 80-120 KB
        // Upload time: 3-5 seconds at typical WiFi speed
    }
    
    // 5. Reinit ESP-NOW
    esp_now_manager_init();
    esp_now_manager_broadcast_ch_status(CH_STATUS_RESUME);
    
    // 6. Return to normal operation
    state = STATE_CH;
}
```

### 8.3 Typical Upload Scenario

```
CH Node Memory State (Before UAV):

  /spiffs/data.lz = 500 chunks × 147 bytes = 73.5 KB
                    (1-2 weeks of data at normal rate)

Upload to Rasp Pi:

  esp_http_client_write(handle, buffer, 4096);  // Chunk 0
  esp_http_client_write(handle, buffer, 4096);  // Chunk 1
  esp_http_client_write(handle, buffer, 4096);  // Chunk 2
  ...
  esp_http_client_write(handle, buffer, 4096);  // Chunk 128 (last)

  Total chunks: 128 × 4KB = 512 KB (uncompressed equivalent)
  Upload time: ~5 seconds at 100 Mbps WiFi
  After upload: SPIFFS cleared, ready to collect new data

Rasp Pi (Flask server) receives all data, stores in `/data/node_xyz_*.csv`
```

---

## 9. Debugging & Verification

### 9.1 Serial Monitor Output (Log Example)

```
I (2000) ms_node: ===== MS Node Boot =====
I (2010) ms_node: Board: ESP32-S3-DevKitC-1, Chip ID: 0x3125565838
I (2020) sensor: Initializing sensors (MOCK MODE)
I (2030) ble_beacon: BLE advertising enabled
I (2500) ms_node: Loop 0: seq=1, temp=22.5°C, hum=65%, press=1013.25hPa
I (4500) ms_node: Loop 1: seq=2, temp=22.6°C, hum=64.9%, press=1013.24hPa
I (6500) ms_node: Loop 2: seq=3, temp=22.7°C, hum=64.8%, press=1013.23hPa
...

I (20000) state_machine: ===== STELLAR PHASE (0-20s) =====
I (20100) ble_beacon: BLE scanning started, 50ms window, 100ms interval
I (20200) neighbor: Discovered peer (node_id=0x1020BA4D), rssi=-55dBm
I (20300) neighbor: Discovered peer (node_id=0x1020BA4E), rssi=-60dBm
I (20400) metrics: Battery=85%, Uptime=20s, Trust=0.92, LinkQ=0.88
I (20500) election: Computing STELLAR score...
I (20600) election: My score: 0.82 > neighbor 0.78 → ELECTED AS CH!

I (40000) state_machine: ===== DATA PHASE (20-40s) =====
I (40100) esp_now: ESP-NOW enabled, peer registration: 2 members
I (40200) state_machine: Broadcasting TDMA schedule (2 slots × 10s)
I (40300) battery: Battery monitoring: 85% (3.85V)

I (45000) state_machine: TDMA SLOT 0 - Member transmission
I (45050) storage: STORE-FIRST: Writing compressed MSLG chunk
I (45100) storage: Chunk size: raw=287B, compressed=115B (60% reduction)
I (45150) esp_now: LIVE SEND #0: seq=11, to CH (MAC: 10:20:BA:4D...)
I (45200) metrics: RSSI EWMA: -55 dBm, PER: 2%, Trust EWMA: 0.94

I (47000) esp_now: LIVE SEND #1: seq=12 (2s gap)
I (49000) esp_now: LIVE SEND #2: seq=13 (2s gap)

I (50000) storage: BURST DRAIN: Popping old chunks
I (50050) storage: Popping chunk #0 (seq=1), decompressing...
I (50100) esp_now: Sending burst chunk: seq=1 (287B)
I (50150) storage: Popping chunk #1 (seq=2), decompressing...

I (80000) state_machine: ===== STELLAR PHASE (40-60s) =====
I (80100) ble_beacon: BLE scanning resumed
...

I (456000) rf_detector: RF TRIGGER RECEIVED: Code 22
I (456100) state_machine: Transitioning to STATE_UAV_ONBOARDING
I (456200) esp_now: Broadcasting CH_BUSY status to all members
I (456300) esp_now: Deinitializing ESP-NOW
I (456400) uav_client: Starting WiFi STA connection to 'WSN_AP'
I (458000) uav_client: WiFi connected! IP: 192.168.4.10
I (458100) uav_client: POST /onboard: Registering session...
I (458200) uav_client: Session ID: 0x12345678
I (458300) uav_client: Starting data upload (500 chunks, 73.5 KB)
I (460000) uav_client: POST /data chunk 0/128: 4096 bytes
I (460200) uav_client: POST /data chunk 1/128: 4096 bytes
...
I (463000) uav_client: POST /data complete: 512 KB uploaded
I (463100) uav_client: POST /ack: acknowledging upload
I (463200) storage_manager: Clearing SPIFFS after successful upload
I (463300) esp_now: Reinitializing ESP-NOW
I (463400) esp_now: Broadcasting CH_RESUME status
I (463500) state_machine: Returning to STATE_CH, resuming TDMA
```

### 9.2 Checking Sensor Configuration

```bash
# Connect via serial monitor
screen /dev/ttyUSB0 115200

# Type: sensor_config
Output:
  BME280 enabled, interval=60000ms, last_read=2024-03-12T10:30:45Z
  AHT21 enabled, interval=60000ms, last_read=2024-03-12T10:30:46Z
  ENS160 enabled, interval=120000ms, last_read=2024-03-12T10:30:30Z
  HMC5883L enabled, interval=60000ms, last_read=2024-03-12T10:30:47Z
  INA219 enabled, interval=10000ms, last_read=2024-03-12T10:30:49Z (Battery: 85%)
  INMP441 disabled, interval=300000ms

# Type: storage_stats
Output:
  SPIFFS Total: 1920401 bytes
  SPIFFS Used: 156234 bytes (8.1%)
  Chunks stored: 1063
  Est. storage time: 12.8 days
  Compression ratio: 59.8%
```

### 9.3 Analyzing MSLG Chunks (Offline)

```python
# Python script to decode MSLG chunks from SPIFFS dump
import struct
import zlib
import json

def decode_mslg_chunk(data):
    # Parse 32-byte header
    magic, version, algo, level, raw_len, data_len, crc32, node_id, timestamp, reserved = struct.unpack(
        '<IHBBIIIQII', data[:32]
    )
    
    # Verify magic
    assert magic == 0x4D534C47, "Invalid magic (not MSLG)"
    
    # Extract payload
    payload = data[32:32+data_len]
    
    # Decompress if algo=1
    if algo == 1:
        payload = zlib.decompress(payload)
    
    # Parse JSON
    json_data = json.loads(payload.decode('utf-8'))
    
    return {
        'version': version,
        'compression': 'miniz' if algo == 1 else 'raw',
        'level': level,
        'raw_bytes': raw_len,
        'compressed_bytes': data_len,
        'node_id': f'{node_id:012x}',
        'timestamp': timestamp,
        'data': json_data
    }

# Example
with open('spiffs_dump.bin', 'rb') as f:
    offset = 0
    chunk_num = 0
    while offset < f.seek(0, 2):  # Read to end
        f.seek(offset)
        chunk_data = f.read(32 + 512)  # Read header + max payload
        
        info = decode_mslg_chunk(chunk_data)
        print(f"Chunk {chunk_num}: {info['data']['ts_ms']}, "
              f"temp={info['data']['env']['bme_t']}°C, "
              f"size={info['raw_bytes']}->{info['compressed_bytes']}B")
        
        offset += 32 + info['compressed_bytes']
        chunk_num += 1
```

---

## Summary Table: Data Generation to Delivery

| Stage | Component | Frequency | Output | Size | Storage |
|-------|-----------|-----------|--------|------|---------|
| **Generation** | Sensors (real/mock) | Every 2-5s | 6 measurements | 52B | RAM (mailbox) |
| **Collection** | app_main loop | Every 2-5s | sensor_payload_t | 52B | metrics.c (overwrite) |
| **STELLAR** | metrics_task | Every 1s | STELLAR score | N/A | Used in BLE beacon |
| **Storage (DATA)** | state_machine | TDMA slot | JSON → MSLG chunk | 147B | /spiffs/data.lz |
| **Transmission** | esp_now_send | TDMA slot × 3-5 | Binary payload | 52B | Over-the-air |
| **Aggregation** | CH ESP-NOW RX | Slot duration | Stored copy | 147B | CH's /spiffs |
| **Compression** | miniz deflate | Per chunk | 60% reduction | 115B | Packed storage |
| **Burst Drain** | pop_chunks loop | End of slot | Old chunks | 147B | Up to 8 per slot |
| **UAV Upload** | WiFi STA | Every 1-2 weeks | 4KB chunks | 512KB | Rasp Pi database |

---

**Document Version:** 1.0 (March 2026)  
**Last Updated:** March 12, 2026  
**Platform:** ESP32-S3, ESP-IDF v5.3, FreeRTOS SMP  
**Author:** Wireless Sensor Network Dev Team
