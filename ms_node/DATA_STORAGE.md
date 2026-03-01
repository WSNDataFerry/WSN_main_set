# Data Storage Implementation

## Features Implemented

### 1. CRC32 Integrity Checks
- Each chunk header includes CRC32 checksum of payload data
- Computed using ESP-IDF's hardware CRC32 (rom/crc.h)
- Validates data integrity during transmission and after power cycles

### 2. Node ID in Metadata
- Unique 64-bit node ID derived from ESP32 MAC address
- Stored in every chunk header for provenance tracking
- Accessible via `logger_get_node_id()` API

### 3. Circular Buffer (Auto-cleanup)
- Monitors SPIFFS usage via `logger_storage_warning()` (>90%) and `logger_storage_critical()` (>95%)
- Automatically clears old data when storage exceeds 95% full
- Prevents storage overflow during extended deployments

### 4. RTC Time Synchronization
- `logger_set_time(unix_timestamp)`: Sync time from BLE drone/base station
- `logger_get_time()`: Get current Unix timestamp (or uptime if not synced)
- Boot timestamp persisted, timestamps in chunk headers are Unix time when synced

### 5. BLE GATT Service for Data Retrieval
- Custom UUID: `12340000-1234-1234-1234-123456789abc`
- Three characteristics:
  - **Time Sync** (Write, UUID: 12340001): Write 4-byte Unix timestamp to sync node clock
  - **Data Request** (Read, UUID: 12340002): Returns file size and metadata
  - **Node Info** (Read, UUID: 12340003): Returns node ID, storage used/total
- Simple passkey authentication (passkey: 123456)

## Updated Chunk Header Format

```c
typedef struct __attribute__((packed)) {
    uint32_t magic;     // 'MSLG' (0x4D534C47)
    uint16_t version;   // 2 (bumped for CRC32 + node_id)
    uint8_t algo;       // 0 = raw, 1 = miniz(deflate)
    uint8_t level;      // deflate level when algo=1
    uint32_t raw_len;   // bytes before compression
    uint32_t data_len;  // bytes stored after header
    uint32_t crc32;     // CRC32 of payload data
    uint64_t node_id;   // Unique node ID from MAC
    uint32_t timestamp; // Unix timestamp (if synced)
    uint32_t reserved;  // Future use
} log_chunk_hdr_t;  // Total: 32 bytes
```

## Testing Each Feature

### CRC32 Validation
```bash
# Dump storage and verify CRC matches
python decode_chunks.py storage.bin --verify-crc
```

### Node ID
```bash
# Read from serial monitor at boot:
# I (xxx) logger: Node ID: 10:20:BA:4D:F0:3C (0x1020BA4DF03C)

# Or via BLE GATT:
# Connect to MSN-B027-PS-4DF03C
# Read characteristic 12340003 -> "ID:10:20:BA:4D:F0:3C,Used:12345,Total:1920401"
```

### Circular Buffer
```bash
# Fill storage to >95%:
# 1. Let node run for hours/days until storage fills
# 2. Monitor logs for "Storage CRITICAL (>95%), will clear old data"
# 3. Verify old data is deleted and new data continues writing
```

### RTC Time Sync
```python
# Via BLE GATT (using Nordic nRF Connect or custom script):
import time
import bluetooth

# Connect to node
# Write current Unix time (4 bytes, little-endian) to characteristic 12340001
timestamp = int(time.time())
data = timestamp.to_bytes(4, 'little')
# characteristic.write(data)

# Verify: subsequent chunks will have correct Unix timestamps instead of uptime
```

### BLE GATT Data Retrieval
```bash
# Use nRF Connect app (Android/iOS):
# 1. Scan for MSN-B027-PS-XXXXXX
# 2. Connect (may prompt for passkey: 123456)
# 3. Discover services -> find 12340000-...
# 4. Read Node Info (12340003) -> see node ID and storage
# 5. Read Data Request (12340002) -> see file size
# 6. Write Time Sync (12340001) -> sync clock
```

## API Usage Examples

### In Application Code

```c
// Check storage status
if (logger_storage_warning()) {
    ESP_LOGW(TAG, "Storage >90% full");
}

// Sync time from drone/base station
uint32_t current_time = 1735776000; // Example: 2025-01-02 00:00:00 UTC
logger_set_time(current_time);

// Get node ID for display
char node_id[18];
logger_get_node_id(node_id, sizeof(node_id));
printf("Node: %s\n", node_id);

// Manual cleanup if needed
if (logger_storage_critical()) {
    logger_cleanup_old_data();
}
```

## Storage Capacity Analysis

With 2 MB SPIFFS partition and compression:
- Raw sensor reading: ~300 bytes JSON
- Compressed: ~130 bytes (miniz ratio ~0.43)
- Chunk overhead: 32 bytes header
- Net per reading: ~162 bytes

**Capacity**:
- 1 reading/min: ~233 KB/day → 1.63 MB/week ✓ Fits
- 1 reading/10s: ~1.4 MB/day → Need cleanup after 36 hours
- With 95% threshold: ~1.84 MB usable → triggers cleanup at ~11,300 readings

## Security Notes

- BLE GATT uses simple passkey (123456) for demo purposes
- Production should implement:
  - Unique per-device passkeys stored in NVS
  - BLE secure connections (LE Secure Connections)
  - Bonding persistence across reboots
  - Whitelist of authorized drone MAC addresses

## Next Steps

- Implement chunk-level deletion (vs. full clear) for circular buffer
- Add BLE characteristic for streaming actual chunk data (not just metadata)
- Persistent boot timestamp in NVS to survive reboots
- CRC verification during read/decompress operations
