# MS Node Data Storage Format

## Overview
This document describes the data storage format used by MS (Main Set) sensor nodes for environmental data collection in a delay-tolerant network.

## Storage Architecture
- **File System**: SPIFFS on ESP32-S3 internal flash
- **Partition Size**: 2MB (0x200000 bytes) at offset 0x310000
- **File Path**: `/spiffs/samples.lz`
- **Format**: Binary chunks with optional compression

## Data Structure

### Chunk Header (32 bytes)
Each data chunk begins with a header containing metadata:

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;     // 0x4D534C47 ('MSLG' in ASCII)
    uint16_t version;   // Format version (current: 2)
    uint8_t  algo;      // Compression: 0=raw, 1=miniz(deflate)
    uint8_t  level;     // Compression level (1-9 for miniz)
    uint32_t raw_len;   // Original data size before compression
    uint32_t data_len;  // Actual stored data size after header
    uint32_t crc32;     // CRC32 checksum of payload data
    uint64_t node_id;   // Unique node identifier (from MAC address)
    uint32_t timestamp; // Seconds since boot (can be synced)
    uint32_t reserved;  // Reserved for future use
} log_chunk_hdr_t;
```

**Field Details**:
- `magic`: Always 0x4D534C47. On flash (little-endian), appears as `47 4C 53 4D`
- `version`: Format version number. Version 2 includes CRC32 and node_id
- `algo`: 0 for uncompressed, 1 for miniz/deflate compression
- `level`: Deflate compression level (typically 3 for balance)
- `raw_len`: Size of data before compression
- `data_len`: Size of compressed data (same as raw_len if algo=0)
- `crc32`: CRC32 checksum for data integrity verification
- `node_id`: 48-bit MAC address packed into 64-bit field
- `timestamp`: Unix-like timestamp (currently seconds since boot)
- `reserved`: Padding for future extensions

### Payload Data
Following the 32-byte header, the payload contains sensor readings in JSON format:

```json
{
  "ts_ms": 123456,
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

**JSON Field Descriptions**:
- `ts_ms`: Timestamp in milliseconds (from ESP timer)
- `env.bme_t`: BME280 temperature (°C)
- `env.bme_h`: BME280 humidity (%)
- `env.bme_p`: BME280 pressure (hPa)
- `env.aht_t`: AHT21 temperature (°C)
- `env.aht_h`: AHT21 humidity (%)
- `gas.aqi`: ENS160 Air Quality Index (0-5 scale)
- `gas.tvoc`: Total Volatile Organic Compounds (ppb)
- `gas.eco2`: Equivalent CO2 (ppm)
- `mag.x/y/z`: Magnetic field strength (μT)
- `power.bus_v`: INA219 bus voltage (V)
- `power.shunt_mv`: INA219 shunt voltage (mV)
- `power.i_ma`: INA219 current (mA)

## Storage Management

### Capacity Planning
- **Total SPIFFS**: ~1.92 MB usable space
- **Target Storage**: 1 week of data at varied sampling rates
- **Sample Rates**:
  - Normal mode: ~1 reading every 2 seconds
  - Power Save mode: ~1 reading every 60 seconds
  - Critical mode: Deep sleep (no continuous logging)

### Circular Buffer Behavior
- Storage is monitored continuously
- **Warning threshold**: 90% full
- **Critical threshold**: 95% full
- When critical threshold is reached, old data is automatically cleared
- This implements a FIFO (First-In-First-Out) circular buffer

### Data Compression
- Compression is applied when buffered data exceeds 1024 bytes
- Miniz (deflate) level 3 is used for balance of speed/size
- Data is only stored compressed if it saves >5% space
- Typical compression ratios:
  - JSON sensor data: 40-50% size reduction
  - Small/repetitive data: May be stored raw

### Data Integrity
- CRC32 checksum computed for each chunk's payload
- Checksums verified during data retrieval
- Header format ensures atomic writes (power-loss safe)
- SPIFFS wear leveling handles flash lifetime

## Node Identification
Each node has a unique 64-bit ID derived from its MAC address:
- Format: `XX:XX:XX:XX:XX:XX` (6 bytes)
- Packed into 64-bit field with upper 16 bits zero
- Example: MAC `10:20:BA:4D:F0:3C` → ID `0x00001020BA4DF03C`

## Time Synchronization
- **Current**: Timestamps use ESP32 timer (seconds since boot)
- **Future**: Support for RTC sync via BLE from data mule
- Target accuracy: ±1 minute per day
- Sync can correct for drift during data collection

## Data Retrieval

### Chunk Iteration
To read the data file:
1. Open `/spiffs/samples.lz` in binary mode
2. Read 32-byte header
3. Verify magic number (`0x4D534C47`)
4. Read `data_len` bytes of payload
5. If `algo == 1`, decompress using miniz/deflate
6. Verify CRC32 checksum
7. Parse JSON payload
8. Repeat until EOF

### Example Reader (Python)
```python
import struct
import zlib
import json

def read_chunks(filepath):
    with open(filepath, 'rb') as f:
        while True:
            hdr_data = f.read(32)
            if len(hdr_data) < 32:
                break
            
            magic, ver, algo, level, raw_len, data_len, crc32, node_id, ts, _ = \
                struct.unpack('<IHBBIIIQII', hdr_data)
            
            if magic != 0x4D534C47:
                break
            
            payload = f.read(data_len)
            
            # Verify CRC
            if zlib.crc32(payload) != crc32:
                print(f"CRC mismatch at offset {f.tell() - data_len}")
                continue
            
            # Decompress if needed
            if algo == 1:
                payload = zlib.decompress(payload)
            
            # Parse JSON
            lines = payload.decode('utf-8').strip().split('\n')
            for line in lines:
                record = json.loads(line)
                yield {
                    'node_id': node_id,
                    'timestamp': ts,
                    'data': record
                }
```

## Export Formats

### CSV Export
For analysis in spreadsheet software:
```csv
node_id,timestamp,ts_ms,bme_t,bme_h,bme_p,aht_t,aht_h,aqi,tvoc,eco2,mag_x,mag_y,mag_z,bus_v,i_ma
0x1020BA4DF03C,1234,123456,23.5,65.2,1013.25,23.3,64.8,1,125,450,12.34,-5.67,45.12,3.700,22.5
```

### JSON Lines Export
For programmatic processing:
```jsonl
{"node_id":"10:20:BA:4D:F0:3C","timestamp":1234,"ts_ms":123456,"env":{"bme_t":23.5,...}}
{"node_id":"10:20:BA:4D:F0:3C","timestamp":1234,"ts_ms":125456,"env":{"bme_t":23.6,...}}
```

## Wear Leveling & Flash Lifetime
- SPIFFS implements automatic wear leveling
- Flash endurance: ~10,000-100,000 erase cycles
- With circular buffer overwrites, typical lifetime:
  - Writing 100KB/day → ~5-50 years of operation
  - Critical threshold prevents excessive wear

## Security Considerations
- **Current**: No encryption (environmental data is non-sensitive)
- **Future options**:
  - AES encryption for data at rest
  - BLE pairing with encryption for transmission
  - Authentication to prevent rogue data collection

## Version History
- **Version 1**: Basic MSLG format with compression
- **Version 2**: Added CRC32, node_id, timestamp, and reserved fields

## Contact
For questions about data format or integration, refer to project documentation.
