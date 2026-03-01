# MS Node - Environmental Sensor Node for Delay-Tolerant Networks

## Overview

The MS Node (Main Set Node) is an ESP32-S3 based environmental monitoring system designed for remote wildlife monitoring in delay-tolerant network scenarios. The node continuously senses environmental parameters, stores data locally, and operates on battery power with intelligent power management.

## System Architecture

### Hardware Components

- **Processing Module**: ESP32-S3-N16R8 (16MB Flash, 8MB PSRAM)
- **Environmental Sensors**:
  - **BME280**: Temperature, humidity, barometric pressure (I²C, 0x76)
  - **ENS160**: Air quality - VOC and equivalent CO₂ (I²C, 0x53)
  - **GY-271/HMC5883L**: 3-axis magnetometer (I²C, 0x1E)
  - **INMP441**: Digital MEMS microphone for bio-acoustic monitoring (I²S)
- **Power Monitoring**: INA219 current/voltage sensor (I²C, 0x40)
- **Auxiliary Sensors**: AHT21 temperature/humidity (I²C, 0x38)
- **Storage**: SPIFFS filesystem for local data logging
- **Connectivity**: BLE for data retrieval and configuration

### Pin Configuration

```
I²C Bus:
  SDA: GPIO 8
  SCL: GPIO 9
  Frequency: 100 kHz
  Pull-ups: Enabled

I²S (INMP441 Microphone):
  WS (Word Select): GPIO 5
  SCK (Serial Clock): GPIO 6
  SD (Serial Data): GPIO 7
  Sample Rate: 16 kHz
  Bit Depth: 16-bit PCM
  Mode: Mono (left channel)

Battery Monitoring:
  ADC Channel: GPIO 4 (ADC1_CH3)
  Voltage Divider: 220kΩ / 100kΩ
  Attenuation: 2.5dB
```

## Key Features

### 1. Continuous Environmental Monitoring

Each sensor reading is timestamped and stored in JSON format:

```json
{
  "ts_ms": 1234567890,
  "env": {"bme_t": 25.3, "bme_h": 65.2, "bme_p": 1013.2, "aht_t": 25.1, "aht_h": 64.8},
  "gas": {"aqi": 1, "tvoc": 120, "eco2": 450},
  "mag": {"x": 12.5, "y": -8.3, "z": 45.2},
  "power": {"bus_v": 3.756, "shunt_mv": 0.124, "i_ma": 12.4},
  "audio": {"samples": 512, "rms": 0.0168, "peak": 0.0604}
}
```

### 2. Trust Filtering

All sensor readings pass through validation to detect:
- Out-of-range values (temperature: -40°C to 85°C, humidity: 0-100%, pressure: 300-1100 hPa)
- VOC/CO₂ anomalies (TVOC: 0-60000 ppb, eCO₂: 100-65000 ppm)
- Audio signal integrity (DC offset, clipping detection)

Invalid readings are logged with warnings and excluded from data storage.

### 3. Power Management Engine (PME)

Three operational modes based on battery level:

| Mode | Battery | Sample Rate | Sensors Active | Deep Sleep |
|------|---------|-------------|----------------|------------|
| **Normal** | ≥60% | 2 seconds | All sensors | No |
| **PowerSave** | 10-59% | 60 seconds | Light sensors, mic, power monitor | No |
| **Critical** | <10% | N/A | None | 2-hour intervals |

Power consumption:
- Normal mode: ~80-100mA (all sensors + WiFi off)
- PowerSave mode: ~20-30mA (reduced sensor load)
- Deep sleep: ~10µA (ESP32-S3 ultra-low-power mode)

### 4. Local Data Storage

- **Filesystem**: SPIFFS on 16MB Flash
- **Format**: MSLG (Multi-Sensor Log) with CRC32 integrity
- **Node ID**: MAC-based unique identifier (AA:BB:CC:DD:EE:FF)
- **Storage Management**: Auto-cleanup when >95% full (removes oldest data)

### 5. BLE GATT Service

Authenticated access (passkey: 123456) via custom service UUID `12340000-1234-1234-1234-123456789abc`:

#### Characteristics:

1. **Time Sync** (Write, UUID: `12340001-...`):
   - Write 4-byte Unix timestamp for clock synchronization
   
2. **Data Request** (Read, UUID: `12340002-...`):
   - Returns file size and data summary
   
3. **Node Info** (Read, UUID: `12340003-...`):
   - Returns node ID and storage usage
   - Format: `"ID:AA:BB:CC:DD:EE:FF,Used:12345,Total:16000000"`

4. **Config Update** (Write, UUID: `12340004-...`):
   - Runtime configuration updates
   - Format: `key=value`
   - Examples:
     ```
     audio_interval_ms=300000    # Set mic sampling to 5 minutes
     inmp441_enabled=0           # Disable microphone
     env_sensor_interval_ms=120000  # Set env sensors to 2 minutes
     ```

## User Configuration

### Method 1: NVS Configuration (Persistent)

Edit configuration via BLE or modify defaults in [sensor_config.c](components/sensors/src/sensor_config.c):

```c
sensor_config_t config = {
    .inmp441_enabled = true,
    .audio_interval_ms = 300000,  // 5 minutes
    .audio_sample_rate = 16000,
    .audio_duration_ms = 1000,    // 1 second clips
    
    .env_sensor_interval_ms = 60000,   // 1 minute
    .gas_sensor_interval_ms = 60000,
    .mag_sensor_interval_ms = 60000,
    .power_sensor_interval_ms = 60000,
    
    // Trust filtering thresholds
    .temp_min_c = -40.0f,
    .temp_max_c = 85.0f,
    .humidity_min_pct = 0.0f,
    .humidity_max_pct = 100.0f,
    .pressure_min_hpa = 300.0f,
    .pressure_max_hpa = 1100.0f,
};

sensor_config_save(&config);
```

### Method 2: Runtime BLE Updates (from Data Mule/Drone)

Use a BLE client to write to the Config Update characteristic:

**Python Example** (using `bleak`):
```python
import asyncio
from bleak import BleakClient

CONFIG_CHAR_UUID = "12340004-1234-1234-1234-123456789abc"

async def update_config():
    async with BleakClient("AA:BB:CC:DD:EE:FF") as client:
        # Disable microphone to save power
        await client.write_gatt_char(CONFIG_CHAR_UUID, b"inmp441_enabled=0")
        
        # Change sampling intervals
        await client.write_gatt_char(CONFIG_CHAR_UUID, b"audio_interval_ms=600000")
        await client.write_gatt_char(CONFIG_CHAR_UUID, b"env_sensor_interval_ms=120000")

asyncio.run(update_config())
```

**nRF Connect Example**:
1. Connect to MS Node
2. Enter passkey: `123456`
3. Navigate to Config Update characteristic (`12340004-...`)
4. Write ASCII string: `audio_interval_ms=300000`
5. Configuration updates immediately and persists to NVS

### Configurable Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `bme280_enabled` | true | Enable BME280 sensor |
| `aht21_enabled` | true | Enable AHT21 sensor |
| `ens160_enabled` | true | Enable ENS160 gas sensor |
| `gy271_enabled` | true | Enable magnetometer |
| `ina219_enabled` | true | Enable power monitor |
| `inmp441_enabled` | false | Enable microphone (1.4mA @ 16kHz) |
| `env_sensor_interval_ms` | 60000 | Env sensor sampling interval |
| `gas_sensor_interval_ms` | 60000 | Gas sensor sampling interval |
| `mag_sensor_interval_ms` | 60000 | Magnetometer sampling interval |
| `power_sensor_interval_ms` | 60000 | Power monitor sampling interval |
| `audio_interval_ms` | 300000 | Microphone sampling interval |
| `audio_sample_rate` | 16000 | Microphone sample rate (Hz) |
| `audio_duration_ms` | 1000 | Audio capture duration |

## Modularity and Extensibility

### Adding a New Sensor

The system is designed for easy sensor integration. Follow this example to add a soil moisture sensor:

#### 1. Create Sensor Component

**`components/sensors/include/soil_moisture_sensor.h`**:
```c
#pragma once
#include "esp_err.h"
#include <stdint.h>

typedef struct {
    float moisture_pct;
    uint16_t raw_value;
    bool valid;
} soil_moisture_reading_t;

esp_err_t soil_moisture_init(void);
esp_err_t soil_moisture_read(soil_moisture_reading_t *reading);
```

**`components/sensors/src/soil_moisture_sensor.c`**:
```c
#include "soil_moisture_sensor.h"
#include "i2c_bus.h"
#include "esp_log.h"

#define SOIL_MOISTURE_ADDR 0x20  // Example I2C address

esp_err_t soil_moisture_init(void) {
    // Initialize sensor via I2C
    return ms_i2c_init();
}

esp_err_t soil_moisture_read(soil_moisture_reading_t *reading) {
    uint8_t data[2];
    esp_err_t ret = ms_i2c_read(SOIL_MOISTURE_ADDR, 0x00, data, 2);
    
    if (ret == ESP_OK) {
        reading->raw_value = (data[0] << 8) | data[1];
        reading->moisture_pct = (reading->raw_value / 65535.0f) * 100.0f;
        reading->valid = true;
    } else {
        reading->valid = false;
    }
    
    return ret;
}
```

#### 2. Update Configuration

Add to `sensor_config.h`:
```c
typedef struct {
    // ... existing fields ...
    bool soil_moisture_enabled;
    uint32_t soil_moisture_interval_ms;
} sensor_config_t;
```

Add to `sensor_config.c` defaults:
```c
.soil_moisture_enabled = true,
.soil_moisture_interval_ms = 60000,
```

#### 3. Integrate into Main Loop

Update `ms_node.c`:
```c
#include "soil_moisture_sensor.h"

// In app_main():
ret = soil_moisture_init();
if (ret != ESP_OK) ESP_LOGW(TAG, "Soil moisture init skipped: %s", esp_err_to_name(ret));

// In main loop:
soil_moisture_reading_t soil = {0};
bool ok_soil = (soil_moisture_read(&soil) == ESP_OK && soil.valid);

if (ok_soil) {
    ESP_LOGI(TAG, "Soil Moisture: %.2f%%", soil.moisture_pct);
}

// Add to JSON log:
"soil":{"moisture_pct":%.2f,"raw":%u}
```

#### 4. Update BLE Config Handler

Add to `ble_gatt_service.c` config parser:
```c
else if (strcmp(key, "soil_moisture_enabled") == 0) {
    cfg.soil_moisture_enabled = (atoi(value) != 0);
}
```

### Sensor Interface Guidelines

- **I²C Sensors**: Use `ms_i2c_read()` / `ms_i2c_write()` helpers from `i2c_bus.h`
- **I²S Sensors**: Follow INMP441 pattern in `inmp441_sensor.c`
- **SPI Sensors**: Add SPI bus initialization similar to I²C in `components/sensors/`
- **Analog Sensors**: Use ADC like battery monitoring in `battery.c`

### Component Organization

```
ms_node/
├── main/
│   ├── ms_node.c           # Main application loop
│   ├── ble_gatt_service.c  # BLE GATT characteristics
│   ├── ble_beacon.c        # BLE advertising
│   └── compression_bench.c # Performance benchmarks
├── components/
│   ├── sensors/            # Sensor drivers
│   │   ├── include/        # Public headers
│   │   │   ├── bme280_sensor.h
│   │   │   ├── ens160_sensor.h
│   │   │   ├── inmp441_sensor.h
│   │   │   └── sensor_config.h
│   │   └── src/            # Implementations
│   │       ├── bme280_sensor.c
│   │       ├── sensor_config.c
│   │       └── i2c_bus.c   # Shared I²C interface
│   ├── logger/             # SPIFFS data storage
│   ├── pme/                # Power management
│   ├── battery/            # Battery monitoring
│   └── compression/        # Data compression (future)
└── spiffs_data/            # Files pre-loaded to SPIFFS
```

## Building and Flashing

### Prerequisites

- ESP-IDF v5.5.1 or later
- Python 3.8+
- USB-to-UART driver for ESP32-S3

### Build Commands

```bash
# Configure project (first time only)
idf.py menuconfig

# Build firmware
idf.py build

# Flash all partitions (bootloader, app, partition table)
idf.py -p COM3 flash

# Flash only application (faster updates)
idf.py -p COM3 app-flash

# Monitor serial output
idf.py -p COM3 monitor

# Flash and monitor
idf.py -p COM3 flash monitor
```

### Device management (optimize, monitor, reflash)

Use the **device manager** to build, flash, monitor, and push config to one or all nodes from a single place. Edit `devices.yaml` with your serial ports (copy from `devices.example.yaml` if needed).

```bash
# From repo: esp_project/tools/ or esp_project/ms_node/
python ../tools/device_manager.py build
python ../tools/device_manager.py list-ports          # List USB serial ports
python ../tools/device_manager.py list-devices        # Show devices.yaml
python ../tools/device_manager.py flash --name Node1  # Flash one device
python ../tools/device_manager.py flash-all           # Flash all devices
python ../tools/device_manager.py monitor --name Node1
python ../tools/device_manager.py monitor-all         # Monitor all (parallel)
# Push config over serial (same keys as BLE Config Update)
python ../tools/device_manager.py optimize --port /dev/tty.usbmodem123 audio_interval_ms=300000 inmp441_enabled=0
```

**Serial CONFIG:** When a device is connected via USB, you can also send config from the serial monitor: type `CONFIG key=value` (e.g. `CONFIG audio_interval_ms=300000`). The device applies and saves to NVS; the main loop reloads config periodically.

**Cluster check:** Run `python check_cluster.py` to monitor all nodes and verify discovery/election (uses `devices.yaml`).

### Configuration

Key `sdkconfig` settings:
- `CONFIG_BT_BLE_SMP_ENABLE=y` - BLE security/pairing
- `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` - 16MB flash
- `CONFIG_SPIRAM_MODE_OCT=y` - Octal PSRAM mode
- `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y` - 240MHz clock

## Testing and Validation

### Sensor Accuracy Tests

**BME280**: Verify against reference thermometer and weather station
```
Expected: ±1°C temperature, ±3% humidity, ±1 hPa pressure
```

**ENS160**: Expose to known VOC source (e.g., alcohol swab)
```
Expected: TVOC rises from ~0-50 ppb to >200 ppb
Expected: eCO₂ rises from ~400 ppm baseline
```

**GY-271**: Move magnet near sensor
```
Expected: Magnetic field changes >50 µT
```

**INMP441**: Clap or play 1kHz tone
```
Expected: RMS amplitude increases from ~0.01 to >0.05
Expected: No clipping (peak < 0.9)
```

### Storage Validation

Check SPIFFS contents:
```bash
idf.py -p COM3 monitor

# In monitor, trigger flush and check logs
# Look for: "MSLG" block, CRC32 validation
```

### BLE Testing

**nRF Connect App**:
1. Scan for "MS_Node_AABBCC"
2. Connect and authenticate (passkey: 123456)
3. Read Node Info characteristic
4. Write config update: `inmp441_enabled=1`
5. Read logs to verify change

### Power Profiling

Measure current with multimeter on BAT+ line:
- Normal mode: 80-100mA (all sensors active)
- PowerSave mode: 20-30mA (reduced sensors)
- Deep sleep: <20µA (timer wakeup only)

## Safety and Environmental Considerations

- **Wildlife Safety**: Microphone is passive (no ultrasonic emissions)
- **Waterproofing**: Recommend IP67 enclosure for outdoor deployment
- **Temperature Range**: Operational -20°C to +60°C (BME280 limit)
- **Battery Chemistry**: Use 18650 Li-ion with protection circuit
- **Mounting**: Secure magnetometer away from metal objects

## Troubleshooting

### Issue: I²C sensors timeout

**Cause**: Missing pull-up resistors or incorrect wiring
**Solution**: Add 4.7kΩ pull-ups on SDA/SCL to 3.3V, check connections

### Issue: Microphone returns all zeros

**Cause**: Not connected or incorrect I²S pins
**Solution**: Verify GPIO5/6/7 wiring, check L/R pin grounded, add delay after init

### Issue: Storage full warning

**Cause**: >90% SPIFFS usage
**Solution**: BLE retrieve and delete data, or increase `LOGGER_MAX_SIZE` in config

### Issue: Battery percentage incorrect

**Cause**: Wrong voltage divider or ADC calibration
**Solution**: Measure BAT_SENSE voltage, adjust R1/R2 values in `battery_cfg_t`

### Issue: BLE connection fails

**Cause**: Passkey mismatch or SMP disabled
**Solution**: Verify `CONFIG_BT_BLE_SMP_ENABLE=y` in sdkconfig, use passkey 123456

## Data Retrieval Workflow

### Using Python Log Parser

1. **Download log file from ESP32:**
   - Connect via BLE GATT (future: use chunked file transfer)
   - OR access SPIFFS directly via USB serial

2. **Parse binary log with CRC32 verification:**
   ```bash
   python tools/log_parser.py /path/to/msn.log
   ```

3. **View sensor data as JSON:**
   ```bash
   python tools/log_parser.py msn.log --json
   ```

4. **Output:**
   ```
   Chunk 1: MINIZ-3 | 2048 bytes | CRC32=0x1A2B3C4D ✓ PASS | Node: 10:20:BA:4D:F0:3C | Time: 2026-01-04T12:34:56
           Compressed: 512 bytes (25.0%)
   Chunk 2: RAW | 1024 bytes | CRC32=0xABCDEF12 ✓ PASS | Node: 10:20:BA:4D:F0:3C | Time: 2026-01-04T12:35:00
   
   === Summary ===
   Total chunks: 2
   Total raw data: 3072 bytes
   Total compressed: 512 bytes (16.7%)
   CRC32 validation: 2/2 PASS
   ```

**Data Integrity:**
- ✅ **PASS**: CRC32 matches, data is uncorrupted
- ❌ **CORRUPTED**: CRC32 mismatch, flash memory error or transmission corruption

### BLE Data Download (Current Status)

**Implemented:**
- BLE beacon for proximity detection
- GATT authentication and configuration
- File size reporting

**Not Yet Implemented:**
- Chunked file transfer over BLE
- Large dataset streaming

**Workaround:** Access SPIFFS via USB serial monitor or ESP-IDF partition tools

## Adding a New Sensor - Step-by-Step Guide

### Method 1: Compile-Time Configuration (Permanent Addition)

#### Step 1: Create Sensor Driver

Create `components/sensors/src/newsensor.c`:

```c
#include "newsensor.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

static const char *TAG = "newsensor";
static i2c_master_dev_handle_t s_newsensor_handle = NULL;

esp_err_t newsensor_init(void) {
    // Initialize I2C device
    extern i2c_master_bus_handle_t i2c_get_bus_handle(void);
    i2c_master_bus_handle_t bus = i2c_get_bus_handle();
    
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x48,  // Your sensor's I2C address
        .scl_speed_hz = 100000,
    };
    
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &s_newsensor_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device: %s", esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "New sensor initialized");
    return ESP_OK;
}

esp_err_t newsensor_read(newsensor_reading_t *reading) {
    if (!s_newsensor_handle) return ESP_ERR_INVALID_STATE;
    
    // Implement sensor reading logic here
    uint8_t data[4];
    esp_err_t ret = i2c_master_receive(s_newsensor_handle, data, sizeof(data), 1000);
    if (ret != ESP_OK) return ret;
    
    // Parse data into reading structure
    reading->value = (data[0] << 8) | data[1];
    reading->valid = true;
    
    return ESP_OK;
}
```

Create header `components/sensors/include/newsensor.h`:

```c
#pragma once
#include "esp_err.h"
#include <stdbool.h>

typedef struct {
    float value;
    bool valid;
} newsensor_reading_t;

esp_err_t newsensor_init(void);
esp_err_t newsensor_read(newsensor_reading_t *reading);
```

#### Step 2: Add to Configuration

Edit `components/sensors/include/sensor_config.h`:

```c
typedef struct {
    // ... existing fields ...
    bool newsensor_enabled;  // ADD THIS
    uint32_t newsensor_interval_ms;  // ADD THIS
    
    // ... rest of struct ...
} sensor_config_t;
```

Edit `components/sensors/src/sensor_config.c`:

```c
sensor_config_t sensor_config_get_default(void) {
    sensor_config_t cfg = {
        // ... existing defaults ...
        .newsensor_enabled = true,          // ADD THIS
        .newsensor_interval_ms = 60000,     // ADD THIS (60 seconds)
    };
    return cfg;
}

esp_err_t sensor_config_load(sensor_config_t *config) {
    // ... existing NVS reads ...
    LOAD_BOOL("newsensor_en", newsensor_enabled);      // ADD THIS
    LOAD_U32("newsensor_int", newsensor_interval_ms);  // ADD THIS
    
    return ESP_OK;
}

esp_err_t sensor_config_save(const sensor_config_t *config) {
    // ... existing NVS writes ...
    SAVE_BOOL("newsensor_en", newsensor_enabled);      // ADD THIS
    SAVE_U32("newsensor_int", newsensor_interval_ms);  // ADD THIS
    
    return ESP_OK;
}
```

#### Step 3: Add to Main Loop

Edit `main/ms_node.c`:

```c
#include "newsensor.h"  // ADD THIS at top

// In app_main() initialization section:
ret = newsensor_init();
if (ret != ESP_OK) ESP_LOGW(TAG, "New sensor init failed: %s", esp_err_to_name(ret));

// Add timestamp tracking variable after existing s_last_* variables:
static uint64_t s_last_newsensor_read_ms = 0;

// In main loop, calculate interval (inside switch statement):
uint32_t newsensor_interval_ms = 60000;  // Default or mode-dependent

// Add timing check:
bool time_for_newsensor = (now_ms - s_last_newsensor_read_ms) >= newsensor_interval_ms;

// Add reading variable:
newsensor_reading_t newsensor_data = {0};
bool ok_newsensor = false;

// Add sensor read (with mode gating if needed):
if (do_full && time_for_newsensor && s_sensor_config.newsensor_enabled) {
    ok_newsensor = (newsensor_read(&newsensor_data) == ESP_OK);
    if (ok_newsensor) s_last_newsensor_read_ms = now_ms;
}

// Add logging:
if (ok_newsensor) {
    ESP_LOGI(TAG, "NewSensor value=%.2f", newsensor_data.value);
}

// Add to logger data structure (find logger_sensor_data_t section):
// ... add newsensor fields to JSON output ...
```

#### Step 4: Add BLE GATT Configuration Support

Edit `main/ble_gatt_service.c`:

In the config update handler, add:

```c
else if (strcmp(key, "newsensor_enabled") == 0) {
    cfg.newsensor_enabled = (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
}
else if (strcmp(key, "newsensor_interval_ms") == 0) {
    cfg.newsensor_interval_ms = (uint32_t)atoi(val);
}
```

#### Step 5: Update CMakeLists.txt

Edit `components/sensors/CMakeLists.txt`:

```cmake
set(COMPONENT_SRCS
    "src/sensors.c"
    # ... existing files ...
    "src/newsensor.c"  # ADD THIS
)
```

#### Step 6: Build and Flash

```bash
cd ms_node
idf.py build
idf.py flash monitor
```

### Method 2: Runtime Configuration via BLE GATT

**Prerequisites:**
- Sensor driver already compiled into firmware
- `newsensor_enabled` field in sensor_config_t

**Steps:**

1. **Connect via BLE** (e.g., using nRF Connect app):
   - Scan for `MSN-B###-XX-AABBCC`
   - Connect and pair (passkey: 123456)

2. **Write to Config Update characteristic:**
   ```json
   {"newsensor_enabled": true, "newsensor_interval_ms": 30000}
   ```

3. **Verify** - node will log:
   ```
   I (12345) ble_gatt: Config updated: newsensor_enabled=true
   I (12345) sensor_config: Saved to NVS
   ```

### Testing Your New Sensor

1. **Check initialization:**
   ```
   I (1234) newsensor: New sensor initialized
   ```

2. **Verify readings appear:**
   ```
   I (5000) main: NewSensor value=42.50
   ```

3. **Check data logging:**
   - Monitor SPIFFS usage increasing
   - Parse log file with `log_parser.py`

4. **Test BLE configuration:**
   - Disable sensor: `{"newsensor_enabled": false}`
   - Change interval: `{"newsensor_interval_ms": 120000}`

## Data Retrieval Workflow

1. **Ferry/Drone arrives** → BLE beacon detected
2. **Connect** → Authenticate with passkey
3. **Read Node Info** → Get storage usage and node ID
4. **Read Data Request** → Retrieve file size
5. **Stream data** → Read SPIFFS file chunks (future enhancement)
6. **Update config** → Write to Config Update characteristic if needed
7. **Disconnect** → Node resumes autonomous operation

## Future Enhancements

- [ ] Chunked BLE file transfer for large datasets
- [ ] LoRa module for extended range communication
- [ ] GPS module for spatial tagging
- [ ] Camera module for visual monitoring
- [ ] ZSTD compression for log data
- [ ] Web-based configuration interface
- [ ] Over-the-air (OTA) firmware updates

## License

This project is provided for educational and research purposes.

## Support

For technical questions or contributions, refer to the component-specific documentation in `components/*/README.md` or review the inline code comments.

---

**Author**: Delay-Tolerant Sensor Network Team  
**Hardware**: ESP32-S3-N16R8  
**SDK**: ESP-IDF v5.5.1  
**Last Updated**: January 2026
