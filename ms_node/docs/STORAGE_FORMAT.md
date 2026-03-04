# MSLG Storage Format & Data Flow Documentation

## Overview

This document describes the sensor data storage format (MSLG), compression mechanism, and data flow architecture used in the WSN (Wireless Sensor Network) system.

---

## Table of Contents

1. [Sensor Data Rate](#sensor-data-rate)
2. [Data Flow Architecture](#data-flow-architecture)
3. [Storage Format (MSLG)](#storage-format-mslg)
4. [Compression](#compression)
5. [Store-First Pattern](#store-first-pattern)
6. [Data Offload & Removal](#data-offload--removal)

---

## Sensor Data Rate

### Default Sensor Intervals (from `sensor_config.c`)

| Sensor Type | Interval | Rate |
|-------------|----------|------|
| Environment (BME280/AHT21) | 60,000 ms | 1 per minute |
| Gas (ENS160) | 120,000 ms | 1 per 2 minutes |
| Magnetometer (GY-271) | 60,000 ms | 1 per minute |
| Power (INA219) | 10,000 ms | 1 per 10 seconds |
| Audio (INMP441) | 300,000 ms | 1 per 5 minutes (disabled by default) |

**Average storage write rate: ~1 sensor record per 60 seconds** (based on `env_sensor_interval_ms`)

---

## Data Flow Architecture

### Member Node Flow

```
┌─────────────────────────────────────────────────────────────────────┐
│                    MEMBER NODE DATA FLOW                            │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  1️⃣ SENSOR TASK (ms_node.c) - Every 60s (env_interval)              │
│     ┌─────────────────────────────────────────────────────────────┐ │
│     │ Read sensors (real or simulated/mock)                       │ │
│     │ Create sensor_payload_t struct in RAM                       │ │
│     │ Call: metrics_set_sensor_data(&payload)  ← Store in RAM     │ │
│     └─────────────────────────────────────────────────────────────┘ │
│                           ↓                                         │
│  2️⃣ STATE MACHINE (state_machine.c) - During DATA phase            │
│     ┌─────────────────────────────────────────────────────────────┐ │
│     │ metrics_get_sensor_data(&payload)  ← Read from RAM          │ │
│     └─────────────────────────────────────────────────────────────┘ │
│                           ↓                                         │
│  3️⃣ STORE FIRST - Write to SPIFFS                                  │
│     ┌─────────────────────────────────────────────────────────────┐ │
│     │ Convert sensor_payload_t → JSON string (~250 bytes)         │ │
│     │ storage_manager_write_compressed() → /spiffs/data.lz        │ │
│     └─────────────────────────────────────────────────────────────┘ │
│                           ↓                                         │
│  4️⃣ THEN SEND via ESP-NOW                                          │
│     ┌─────────────────────────────────────────────────────────────┐ │
│     │ esp_now_manager_send_data(ch_mac, &payload, sizeof(...))    │ │
│     │ Sends BINARY sensor_payload_t (~52 bytes) to CH             │ │
│     └─────────────────────────────────────────────────────────────┘ │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### CH (Cluster Head) Node Flow

```
┌─────────────────────────────────────────────────────────────────────┐
│                       CH NODE DATA FLOW                             │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  Own Sensor Read (every 60s)                                        │
│       ↓                                                             │
│  Convert to JSON string                                             │
│       ↓                                                             │
│  storage_manager_write_compressed() → SPIFFS (data.lz)             │
│       ↓                                                             │
│  Receive from Members via ESP-NOW → Also store to SPIFFS           │
│       ↓                                                             │
│  When UAV arrives: Read from storage → Upload all data             │
│       ↓                                                             │
│  Upload via HTTP POST /data (4 KB chunked write loop)              │
│  esp_http_client_open() → write(4KB) × N → fetch_headers()        │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Key Point: Two Different Formats

| Destination | Format | Size |
|-------------|--------|------|
| SPIFFS Storage | JSON string in MSLG chunk | ~250-300 bytes |
| ESP-NOW to CH | Binary `sensor_payload_t` struct | ~52 bytes |

---

## Storage Format (MSLG)

### MSLG = Per-Chunk Format (NOT Per-File!)

**Important:** MSLG is NOT a single file header. Each record/chunk has its OWN 32-byte header.

### File Structure: `/spiffs/data.lz`

```
┌─────────────────────────────────────────────────────────────────────┐
│                    FILE: /spiffs/data.lz                            │
├─────────────────────────────────────────────────────────────────────┤
│ ┌─────────────────────────────────┐                                 │
│ │ MSLG Header #1 (32 bytes)       │  ← For THIS chunk only          │
│ │   magic: 0x4D534C47 ("MSLG")    │                                 │
│ │   version: 2                    │                                 │
│ │   algo: 0 (raw)                 │                                 │
│ │   level: 0                      │                                 │
│ │   raw_len: 250                  │  ← Size of JSON payload         │
│ │   data_len: 250                 │  ← Same (no compression)        │
│ │   crc32: 0xABCD1234             │  ← CRC of THIS chunk's payload  │
│ │   node_id: 0x3125565838         │  ← THIS node's MAC              │
│ │   timestamp: 3600               │  ← Seconds since boot           │
│ │   reserved: 0                   │                                 │
│ ├─────────────────────────────────┤                                 │
│ │ JSON Payload #1 (250 bytes)     │                                 │
│ │ {"id":3125565838,"seq":1,...}   │                                 │
│ └─────────────────────────────────┘                                 │
│ ┌─────────────────────────────────┐                                 │
│ │ MSLG Header #2 (32 bytes)       │  ← For NEXT chunk               │
│ │   ...                           │                                 │
│ ├─────────────────────────────────┤                                 │
│ │ JSON Payload #2 (248 bytes)     │                                 │
│ │ {"id":3125565838,"seq":2,...}   │                                 │
│ └─────────────────────────────────┘                                 │
│ ┌─────────────────────────────────┐                                 │
│ │ MSLG Header #3 ...              │                                 │
│ │ ...more chunks...               │                                 │
│ └─────────────────────────────────┘                                 │
└─────────────────────────────────────────────────────────────────────┘
```

### MSLG Header Structure (32 bytes)

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;     // 0x4D534C47 ('MSLG') - identifies valid chunk
    uint16_t version;   // Format version (currently 2)
    uint8_t  algo;      // 0=raw (no compression), 1=miniz/deflate
    uint8_t  level;     // Compression level (1-9) if compressed, 0 if raw
    uint32_t raw_len;   // Original JSON size BEFORE compression
    uint32_t data_len;  // Actual stored payload size (= raw_len if not compressed)
    uint32_t crc32;     // CRC32 checksum of THIS chunk's payload only
    uint64_t node_id;   // MAC address of node that generated this data
    uint32_t timestamp; // Seconds since boot when THIS chunk was written
    uint32_t reserved;  // Reserved for future use
} mslg_chunk_hdr_t;
```

### Header Fields Explained

| Field | Size | Description |
|-------|------|-------------|
| `magic` | 4 bytes | `0x4D534C47` ("MSLG") - identifies valid chunk start |
| `version` | 2 bytes | Format version (currently 2) |
| `algo` | 1 byte | **0 = raw (no compression), 1 = miniz/deflate** |
| `level` | 1 byte | Compression level (1-9) if algo=1, else 0 |
| `raw_len` | 4 bytes | **Original JSON size BEFORE compression** |
| `data_len` | 4 bytes | **Actual stored payload size after header** |
| `crc32` | 4 bytes | **Checksum of THIS chunk's payload only** |
| `node_id` | 8 bytes | MAC address (48 bits) of the originating node |
| `timestamp` | 4 bytes | **Seconds since boot when THIS chunk was written** |
| `reserved` | 4 bytes | Reserved for future use |

### JSON Payload Format

Each chunk's payload is a JSON string:

```json
{
  "id": 3125565838,
  "seq": 1,
  "mac": "aabbccddeeff",
  "ts": 12345678,
  "t": 25.5,
  "h": 60.0,
  "p": 1013,
  "q": 50,
  "eco2": 400,
  "tvoc": 100,
  "mx": 30.0,
  "my": 30.0,
  "mz": 40.0,
  "a": 0.05
}
```

| Field | Description |
|-------|-------------|
| `id` | Node ID (from MAC) |
| `seq` | Sequence number (monotonic) |
| `mac` | MAC address hex string |
| `ts` | Timestamp (ms since boot) |
| `t` | Temperature (°C) |
| `h` | Humidity (%) |
| `p` | Pressure (hPa) |
| `q` | Air Quality Index |
| `eco2` | eCO2 (ppm) |
| `tvoc` | TVOC (ppb) |
| `mx/my/mz` | Magnetometer (µT) |
| `a` | Audio RMS |

---

## Compression

### Compression Configuration

```c
#define COMPRESSION_MIN_BYTES 1024      // Only compress if data >= 1KB
#define COMPRESSION_MIN_SAVINGS_DIV 20  // Require >5% savings
#define COMPRESSION_LEVEL 3             // Balanced compression level
```

### Why Compression Usually Doesn't Apply

For typical sensor data (~200-300 bytes per JSON payload):

```
JSON payload size: ~250 bytes
Compression threshold: 1024 bytes (1KB)

250 bytes << 1024 bytes threshold
Result: algo=0 (raw), raw_len == data_len
```

**Each sensor reading creates:**
- 32-byte MSLG header
- ~200-300 byte raw JSON payload
- **Total: ~260 bytes per record**

### When Compression IS Used

Compression activates when:
1. Data size ≥ 1KB (batched data, large payloads)
2. Compression saves >5% space
3. Stack has ≥1KB free

### Compressed vs Raw Chunks

**Raw (algo=0):**
```
┌─────────────────────────────────────────────────────────────────────┐
│ Header (32 bytes):                                                  │
│   algo      = 0  ← RAW                                              │
│   raw_len   = 250                                                   │
│   data_len  = 250  ← Same as raw_len                                │
├─────────────────────────────────────────────────────────────────────┤
│ Payload (250 bytes):                                                │
│   {"id":3125565838,"seq":1,"mac":"aabb..."...}  ← Plain JSON        │
└─────────────────────────────────────────────────────────────────────┘
```

**Compressed (algo=1):**
```
┌─────────────────────────────────────────────────────────────────────┐
│ Header (32 bytes):                                                  │
│   algo      = 1  ← MINIZ/DEFLATE                                    │
│   level     = 3  ← Compression level                                │
│   raw_len   = 2000  ← Original size before compression              │
│   data_len  = 1200  ← Compressed size (smaller!)                    │
├─────────────────────────────────────────────────────────────────────┤
│ Payload (1200 bytes):                                               │
│   [compressed binary data - NOT readable JSON]                      │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Store-First Pattern

### Why Store First?

The system uses a **Store-First** architecture:
1. **Write to SPIFFS first** (persistent storage)
2. **Then send via ESP-NOW** (may fail)

**Benefit:** If ESP-NOW send fails (CH unreachable, radio busy), data is already safely stored and can be retried later.

### Code Flow (state_machine.c)

```c
// Step 1: Get sensor data from RAM
metrics_get_sensor_data(&payload);

// Step 2: Store to SPIFFS FIRST (as JSON in MSLG format)
storage_manager_write_compressed(json_payload, true);

// Step 3: THEN send via ESP-NOW (as binary struct)
esp_now_manager_send_data(ch_mac, &payload, sizeof(sensor_payload_t));
```

---

## Data Offload & Removal

### Pop Operation = Read + Remove

The `storage_manager_pop_mslg_chunk()` function:
1. Reads the first chunk from `data.lz`
2. Copies remaining chunks to `temp.mslg`
3. Deletes original file
4. Renames temp → `data.lz`
5. Returns the popped chunk (removed from storage)

### Time Slot Burst Offload Flow

```
┌─────────────────────────────────────────────────────────────────────┐
│               MEMBER → CH DATA OFFLOAD (Time Slot Burst)            │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  During TIME SLOT (burst mode, up to 8 chunks per slot):            │
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │ 1. storage_manager_pop_mslg_chunk()                           │  │
│  │    - Read first chunk from data.lz                            │  │
│  │    - Copy remaining chunks to temp.mslg                       │  │
│  │    - DELETE original, RENAME temp → data.lz                   │  │
│  │    - Return chunk payload (REMOVED from storage)              │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                           ↓                                         │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │ 2. If algo=1: Decompress chunk (miniz)                        │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                           ↓                                         │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │ 3. esp_now_manager_send_data(ch_mac, chunk, len)              │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                           ↓                                         │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │ 4a. SUCCESS → free(chunk), continue to next chunk             │  │
│  │                                                               │  │
│  │ 4b. FAILURE → storage_manager_write_compressed(chunk)         │  │
│  │               RE-APPEND to storage for retry later!           │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  Repeat up to MAX_MSLG_BURST (8) chunks per slot                   │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Data Safety Guarantees

| Event | Action |
|-------|--------|
| Send SUCCESS | Chunk deleted from storage, freed from RAM ✅ |
| Send FAILURE | Chunk RE-APPENDED to storage for retry ♻️ |
| Decompression FAILURE | Original chunk re-written to storage |

**Data is only permanently deleted after successful ESP-NOW transmission.**

---

## Storage Capacity Estimation

### Per-Record Size
- MSLG Header: 32 bytes
- JSON Payload: ~200-300 bytes
- **Total per record: ~250-330 bytes**

### Example Capacity (1MB SPIFFS)
```
SPIFFS size: 1,048,576 bytes (1 MB)
Reserved/overhead: ~10%
Usable: ~940,000 bytes

Records per MB: 940,000 / 280 ≈ 3,357 records
At 1 record/minute: ~56 hours of data
At 1 record/10 seconds: ~9.3 hours of data
```

---

## Files Reference

| File | Purpose |
|------|---------|
| `/spiffs/data.lz` | MSLG format storage (compressed/raw chunks) |
| `/spiffs/data.txt` | Legacy plain-text format (backward compatibility) |
| `/spiffs/queue.txt` | Upload queue (renamed from data.txt for upload) |
| `/spiffs/temp.mslg` | Temporary file during pop operations |

---

## Related Source Files

| File | Description |
|------|-------------|
| `components/storage_manager/storage_manager.c` | MSLG format, compression, pop/write operations |
| `components/sensors/src/sensor_config.c` | Sensor interval configuration |
| `main/state_machine.c` | Store-first logic, time slot burst offload |
| `main/ms_node.c` | Sensor task, payload creation |
| `main/metrics.c` | RAM storage for current sensor data |
| `main/metrics.h` | `sensor_payload_t` struct definition |

*Last Updated: March 2026*
*Author: Chandupa Chiranjeewa Bandaranayake*
