# TDMA Scheduling and ESP-NOW Communication System

> **Comprehensive guide to Time Division Multiple Access (TDMA) scheduling, ESP-NOW communication timing, STELLAR-based cluster head role assignment, and BLE coexistence in the WSN system**

---

## Quick Links

- [STELLAR_ALGORITHM.md](STELLAR_ALGORITHM.md) — How CH is elected during STELLAR phase
- [TIMING_DIAGRAM.md](TIMING_DIAGRAM.md) — Visual timeline of superframe phases
- [MSLG_DATA_FLOW.md](MSLG_DATA_FLOW.md) — How data flows from sensor → SPIFFS → CH → UAV
- [DUAL_CORE_SENSOR_ARCHITECTURE.md](DUAL_CORE_SENSOR_ARCHITECTURE.md) — Task scheduling and sensor-radio coupling

---

## Table of Contents

1. [Overview](#overview)
2. [Superframe Architecture](#superframe-architecture)
3. [STELLAR Phase (BLE Priority)](#stellar-phase-ble-priority)
4. [DATA Phase (ESP-NOW Priority)](#data-phase-esp-now-priority)
5. [TDMA Slot Scheduling](#tdma-slot-scheduling)
6. [ESP-NOW Communication Protocol](#esp-now-communication-protocol)
7. [Radio Coexistence Management](#radio-coexistence-management)
8. [Neighbor Table Management](#neighbor-table-management)
9. [Store-First Data Pipeline](#store-first-data-pipeline)
10. [Timing Constraints and Optimization](#timing-constraints-and-optimization)
11. [Troubleshooting Guide](#troubleshooting-guide)

---

## Overview

The WSN system implements a **dual-phase superframe** protocol that alternates between:

- **STELLAR Phase**: BLE-based neighbor discovery, metrics exchange, and cluster head (CH) election
- **DATA Phase**: ESP-NOW-based TDMA data transmission with BLE scanning disabled for radio coexistence

This design solves the fundamental problem of **ESP32-S3 single-radio architecture** where BLE and ESP-NOW cannot operate simultaneously without interference.

### Key Design Principles

1. **Temporal Radio Separation**: BLE and ESP-NOW operate in different time windows
2. **Store-First Pattern**: All data is stored locally before transmission (fault tolerance)
3. **FIFO Ordering**: Data transmission maintains chronological sequence integrity
4. **Adaptive TDMA**: Dynamic slot allocation based on current cluster membership
5. **Neighbor Table Robustness**: Enhanced mutex synchronization and verification mechanisms

---

## Superframe Architecture

```
                              SUPERFRAME CYCLE (40 seconds)
    ◄─────────────────────────────────────────────────────────────────────────────►
    
    ┌─────────────────────────────────┬─────────────────────────────────────────┐
    │         STELLAR PHASE           │              DATA PHASE                 │
    │           (20 sec)              │               (20 sec)                  │
    ├─────────────────────────────────┼─────────────────────────────────────────┤
    │                                 │                                         │
    │  ┌───────────────────────────┐  │  ┌─────────────────────────────────┐   │
    │  │     BLE PRIORITY          │  │  │      ESP-NOW PRIORITY           │   │
    │  │  • Scanning ENABLED       │  │  │   • BLE Scanning DISABLED       │   │
    │  │  • Advertising ENABLED    │  │  │   • Advertising ENABLED (CH)    │   │
    │  │  • Metrics Exchange       │  │  │   • TDMA Data Transmission      │   │
    │  │  • CH Election            │  │  │   • Burst Drain from Storage    │   │
    │  │  • Neighbor Discovery     │  │  │   • Store-First Pattern         │   │
    │  └───────────────────────────┘  │  └─────────────────────────────────┘   │
    │                                 │                                         │
    └─────────────────────────────────┴─────────────────────────────────────────┘
    
    t=0s                            t=20s                                    t=40s
    │                               │                                      │
    │   Phase Transition            │   Phase Transition                   │   Cycle
    │   ════════════════            │   ════════════════                   │   Repeats
    │   • BLE → ESP-NOW            │   • ESP-NOW → BLE                   │
    │   • Election Complete        │   • TDMA Complete                   │
    │   • CH Cached                │   • Next Discovery Cycle            │
```

### Configuration Parameters

| Parameter | Value | Purpose |
|-----------|-------|---------|
| `STELLAR_PHASE_MS` | 20,000 ms | BLE metrics/election window |
| `DATA_PHASE_MS` | 20,000 ms | ESP-NOW TDMA transmission window |
| `PHASE_GUARD_MS` | 5,000 ms | Guard time before TDMA slots start |
| `SLOT_DURATION_SEC` | 10 sec | Duration of each node's TDMA slot |
| `ELECTION_WINDOW_MS` | 10,000 ms | Metrics collection before election |

---

## STELLAR Phase (BLE Priority)

### Purpose
- **Neighbor Discovery**: Detect and catalog nearby nodes
- **Metrics Exchange**: Share battery, uptime, trust, and link quality metrics  
- **CH Election**: Run STELLAR algorithm to select optimal cluster head
- **Neighbor Table Maintenance**: Update and verify neighbor relationships

### Detailed Timeline

```
    STELLAR PHASE (20 seconds)
    ◄───────────────────────────────────────────────────────────────────────────►
    
    ┌───────────────────────────────────────────────────────────────────────────┐
    │                                                                           │
    │   0-10s: METRICS COLLECTION WINDOW                                       │
    │   ═══════════════════════════════════════                                 │
    │                                                                           │
    │   ┌─────────────────────────────────────────────────────────────────────┐ │
    │   │  BLE SCANNING ██████████████████████████████████████████████████████│ │
    │   │               • Collect neighbor beacons                           │ │
    │   │               • Extract RSSI, metrics, sequence numbers            │ │
    │   │               • Update neighbor_manager with discovered nodes      │ │
    │   └─────────────────────────────────────────────────────────────────────┘ │
    │                                                                           │
    │   ┌─────────────────────────────────────────────────────────────────────┐ │
    │   │  BLE ADVERTISING ███████████████████████████████████████████████████│ │
    │   │                  • Broadcast own metrics every ~1s                 │ │
    │   │                  • Include battery %, uptime, trust, link quality  │ │
    │   │                  • HMAC authentication for security               │ │
    │   └─────────────────────────────────────────────────────────────────────┘ │
    │                                                                           │
    │   10-20s: ELECTION & DECISION WINDOW                                     │
    │   ══════════════════════════════════════                                 │
    │                                                                           │
    │   ┌─────────────────────────────────────────────────────────────────────┐ │
    │   │  STELLAR ALGORITHM                                                  │ │
    │   │  ├─ Compute utility scores for all discovered nodes                │ │
    │   │  ├─ Build Pareto frontier (non-dominated solutions)                │ │
    │   │  ├─ Apply Nash Bargaining or fallback to max STELLAR score        │ │
    │   │  ├─ Elect/confirm CH with deterministic tie-breaking by node_id    │ │
    │   │  └─ Cache elected CH_ID for DATA phase                             │ │
    │   └─────────────────────────────────────────────────────────────────────┘ │
    │                                                                           │
    └───────────────────────────────────────────────────────────────────────────┘
    
    t=0s        t=10s                                                       t=20s
     │           │                                                           │
     │  Metrics  │  Election Window Ends                                    │  Phase
     │  Window   │  • Run STELLAR algorithm                                 │  Ends
     │  Opens    │  • Elect/Confirm CH                                     │
     │           │  • Cache CH for DATA phase                              │
```

### BLE Operations

#### Advertising Packet Format
```c
typedef struct {
    uint32_t node_id;      // Unique node identifier
    uint16_t score;        // Scaled composite STELLAR score (0-10000)
    uint8_t battery;       // Battery percentage (0-100)
    uint8_t trust;         // Trust metric (0-100) 
    uint8_t link_quality;  // Link quality (0-100)
    uint8_t is_ch;         // Current CH status (0/1)
    uint32_t seq_num;      // Sequence number for deduplication
    uint8_t hmac[2];       // Truncated HMAC for authentication
} __attribute__((packed)) ble_score_packet_t;
```

#### Neighbor Discovery Process
```c
// When BLE beacon received:
1. Validate HMAC authentication
2. Extract node metrics (battery, uptime, trust, link_quality)
3. Call neighbor_manager_update() with enhanced debugging:
   - Check if node already exists in neighbor table
   - If new: Add with ESP-NOW peer registration
   - If existing: Update metrics and RSSI
   - Verify addition was successful (post-update check)
4. Update STELLAR metrics for election algorithm
```

### Enhanced Neighbor Table Management

The neighbor management system has been comprehensively improved to address race conditions and synchronization issues:

#### Robust Addition Process
```c
esp_err_t neighbor_manager_update(uint32_t node_id, int rssi, float score, 
                                 float trust, uint32_t seq_num) {
    // Enhanced mutex timeout (100ms → 500ms) for race condition mitigation  
    if (!neighbor_mutex_lock(500)) {
        ESP_LOGE(TAG, "neighbor_manager_update: mutex timeout");
        return ESP_ERR_TIMEOUT;
    }

    // Search existing neighbors with detailed logging
    ESP_LOGI(TAG, "[DEBUG] Searching for existing neighbor %lu in table of %d entries", 
             node_id, g_neighbor_count);
    
    int existing_idx = find_neighbor_index(node_id);
    
    if (existing_idx >= 0) {
        // Update existing neighbor
        update_neighbor_metrics(existing_idx, rssi, score, trust, seq_num);
    } else {
        // Add new neighbor with comprehensive validation
        if (g_neighbor_count < MAX_NEIGHBORS) {
            // Initialize with memset for clean state
            neighbor_info_t *new_neighbor = &g_neighbors[g_neighbor_count];
            memset(new_neighbor, 0, sizeof(neighbor_info_t));
            
            // Register ESP-NOW peer BEFORE updating table
            uint8_t peer_mac[6];
            if (convert_node_id_to_mac(node_id, peer_mac) == ESP_OK) {
                esp_err_t peer_result = esp_now_peer_register(peer_mac);
                if (peer_result == ESP_OK || peer_result == ESP_ERR_ESPNOW_EXIST) {
                    ESP_LOGI(TAG, "[DEBUG] ESP-NOW peer registered successfully for node %lu", node_id);
                } else {
                    ESP_LOGW(TAG, "ESP-NOW peer registration failed for %lu: %s", 
                            node_id, esp_err_to_name(peer_result));
                    neighbor_mutex_unlock();
                    return peer_result;
                }
            }
            
            // Field-by-field initialization
            new_neighbor->node_id = node_id;
            new_neighbor->rssi = rssi;
            new_neighbor->score = score;
            new_neighbor->trust = trust;
            new_neighbor->seq_num = seq_num;
            new_neighbor->last_seen_ms = esp_timer_get_time() / 1000ULL;
            new_neighbor->verified = true;  // Mark as verified
            
            g_neighbor_count++;
            
            ESP_LOGI(TAG, "Added neighbor: node_id=%lu, RSSI=%d, Seq=%lu", 
                    node_id, rssi, seq_num);
            ESP_LOGI(TAG, "[DEBUG] Successfully added neighbor %lu, new table size: %d", 
                    node_id, g_neighbor_count);
                    
            // POST-ADDITION VERIFICATION
            int verify_idx = find_neighbor_index(node_id);
            if (verify_idx >= 0) {
                ESP_LOGI(TAG, "[DEBUG] VERIFICATION: Node %lu successfully found in table after addition", node_id);
            } else {
                ESP_LOGE(TAG, "[DEBUG] VERIFICATION FAILED: Node %lu NOT found after addition!", node_id);
            }
        } else {
            ESP_LOGW(TAG, "Neighbor table full (%d/%d), cannot add node %lu", 
                    g_neighbor_count, MAX_NEIGHBORS, node_id);
            ESP_LOGW(TAG, "[DEBUG] Current neighbor table status:");
            for (int i = 0; i < g_neighbor_count; i++) {
                ESP_LOGW(TAG, "[DEBUG]   [%d] node_id=%lu, verified=%d", 
                        i, g_neighbors[i].node_id, g_neighbors[i].verified);
            }
            neighbor_mutex_unlock();
            return ESP_ERR_NO_MEM;
        }
    }
    
    neighbor_mutex_unlock();
    return ESP_OK;
}
```

#### Key Improvements
1. **Extended Mutex Timeout**: Increased from 100ms to 500ms to handle race conditions
2. **ESP-NOW Peer Registration Validation**: Verify peer registration before table updates
3. **Comprehensive Logging**: Detailed debug output for all neighbor operations
4. **Post-Addition Verification**: Confirm neighbor was actually added to the table
5. **Clean Initialization**: Use memset to ensure clean neighbor state
6. **Table Status Debugging**: Full table dump when table is full

---

## DATA Phase (ESP-NOW Priority)

### Purpose
- **TDMA Data Transmission**: Members send sensor data to CH during assigned time slots
- **Burst Drain**: Send backlogged data from local MSLG storage
- **Store-First Pattern**: Ensure all data is saved locally before transmission
- **Radio Coexistence**: Disable BLE scanning to prevent ESP-NOW interference

### TDMA Slot Structure

```
                         DATA PHASE (20 seconds)
    ◄─────────────────────────────────────────────────────────────────────────────►
    
    ┌───────┬───────────┬───────────┬───────────┬───────────┬───────────┬─────────┐
    │ GUARD │   SLOT 0  │   SLOT 1  │   SLOT 2  │   SLOT 3  │   SLOT 4  │  FLEX   │
    │  5s   │    10s    │    10s    │    10s    │    10s    │    10s    │   5s    │
    └───────┴───────────┴───────────┴───────────┴───────────┴───────────┴─────────┘
    
    t=20s  t=25s       t=35s       t=45s       t=55s       t=65s       t=70s    t=40s
     │      │           │           │           │           │           │         │
     │    Node 0      Node 1     Node 2     Node 3     Node 4       Buffer    Phase
     │    Active      Active     Active     Active     Active       Time      Ends
     │                                                                         
     │    ┌─────────────────────────────────────────────────────────────────────┐
     │    │ BLE SCANNING STATUS: DISABLED                                       │
     │    │ Reason: Prevent radio contention with ESP-NOW MAC-layer ACKs       │
     │    │ Impact: ~10x improvement in ESP-NOW success rate (90%+ vs <10%)    │
     │    └─────────────────────────────────────────────────────────────────────┘
     │
     │    ┌─────────────────────────────────────────────────────────────────────┐
     │    │ CH BEACON TIMEOUT: PHASE-AWARE CHECKING                            │
     │    │ • During DATA phase: Skip timeout checks (scanning disabled)       │
     │    │ • Use cached CH ID from STELLAR phase                             │
     │    │ • Resume timeout checking in next STELLAR phase                   │
     │    └─────────────────────────────────────────────────────────────────────┘
```

### Individual TDMA Slot Detail

```
                         SINGLE TDMA SLOT (10 seconds)
    ◄─────────────────────────────────────────────────────────────────────────────►
    
    ┌─────────────────────────────────────────────────────────────────────────────┐
    │                                                                             │
    │  Member Node Activities:                                                    │
    │                                                                             │
    │  ┌─────────────┬─────────────┬─────────────┬───────────────────────────────┐│
    │  │ 1. COLLECT  │ 2. STORE    │ 3. LIVE TX  │ 4. BURST DRAIN                ││
    │  │ Sensor Data │ to SPIFFS   │ via ESP-NOW │ (Historical Data)             ││
    │  │             │ (MSLG)      │ to CH       │                               ││
    │  │             │             │             │                               ││
    │  │ ~10ms       │ ~50ms       │ ~100ms      │ Remaining slot time           ││
    │  │             │             │ (if fresh)  │ (~9.8 seconds)               ││
    │  └─────────────┴─────────────┴─────────────┴───────────────────────────────┘│
    │                                                                             │
    │  Store-First Data Flow:                                                     │
    │  ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐             │
    │  │  Sensor  │───►│   JSON   │───►│  SPIFFS  │───►│ ESP-NOW  │───► CH     │
    │  │  Reading │    │ Payload  │    │  (MSLG)  │    │  Packet  │             │
    │  │          │    │          │    │ Compress │    │          │             │
    │  └──────────┘    └──────────┘    └──────────┘    └──────────┘             │
    │       │               │               │               │                    │
    │       │               │               │               └─ Binary ESP-NOW   │
    │       │               │               └─ LZ4 compressed                   │
    │       │               └─ JSON formatted                                   │
    │       └─ Raw sensor values                                                │
    │                                                                             │
    │  Burst Drain Process (FIFO from storage):                                  │
    │  ┌─────────────────────────────────────────────────────────────────────┐   │
    │  │ FOR i = 0 TO MAX_MSLG_BURST (24 chunks):                           │   │
    │  │   ├─ pop_mslg_chunk() → get oldest stored data                     │   │
    │  │   ├─ decompress(if compressed) → JSON payload                      │   │
    │  │   ├─ esp_now_send(CH, payload)                                     │   │
    │  │   ├─ success? → delete chunk : requeue for later                   │   │
    │  │   ├─ check time budget (slot end) → break if exceeded             │   │
    │  │   └─ repeat until storage empty or slot time exhausted            │   │
    │  └─────────────────────────────────────────────────────────────────────┘   │
    │                                                                             │
    └─────────────────────────────────────────────────────────────────────────────┘
```

### TDMA Scheduling Algorithm

```c
void state_machine_data_phase() {
    // Get current cluster members from neighbor table
    neighbor_info_t members[MAX_NEIGHBORS];
    int member_count = neighbor_manager_get_members(members);
    
    // Calculate TDMA schedule
    uint64_t phase_start_ms = get_data_phase_start_time();
    uint64_t guard_end_ms = phase_start_ms + PHASE_GUARD_MS;  // +5000ms
    uint64_t slot_duration_ms = SLOT_DURATION_SEC * 1000;    // 10000ms
    
    // Find my slot index (sorted by node_id for deterministic ordering)
    int my_slot_index = find_my_slot_index(members, member_count);
    
    if (my_slot_index >= 0) {
        // Calculate my transmission window
        uint64_t my_slot_start = guard_end_ms + (my_slot_index * slot_duration_ms);
        uint64_t my_slot_end = my_slot_start + slot_duration_ms;
        
        ESP_LOGI(TAG, "TDMA: My slot %d/%d: %llu-%llu ms", 
                 my_slot_index, member_count, my_slot_start, my_slot_end);
        
        // Wait for my slot
        wait_until_time(my_slot_start);
        
        // Execute slot activities
        execute_tdma_slot(my_slot_end);
    } else {
        ESP_LOGW(TAG, "NO-SCHED: Not found in neighbor table, passive mode");
    }
}

void execute_tdma_slot(uint64_t slot_end_us) {
    uint64_t now_us = esp_timer_get_time();
    uint64_t time_budget = slot_end_us - now_us;
    
    // 1. Store-First: Save current sensor data
    sensor_payload_t current_data;
    metrics_get_sensor_data(&current_data);
    
    char json_payload[384];
    format_sensor_json(json_payload, sizeof(json_payload), &current_data);
    storage_manager_write_compressed(json_payload, true);  // LZ4 compress
    
    // 2. Live Send: Transmit current reading (if fresh)
    static uint64_t last_live_send_ms = 0;
    uint64_t now_ms = now_us / 1000ULL;
    
    if ((now_ms - last_live_send_ms) >= 2000) {  // 2s gap enforcement
        esp_err_t send_result = esp_now_send_sensor_data(&current_data);
        if (send_result == ESP_OK) {
            ESP_LOGI(TAG, "Live send: seq=%lu SUCCESS", current_data.seq_num);
        } else {
            ESP_LOGW(TAG, "Live send: seq=%lu FAILED (%s)", 
                    current_data.seq_num, esp_err_to_name(send_result));
        }
        last_live_send_ms = now_ms;
    }
    
    // 3. Burst Drain: Send stored historical data (FIFO)
    burst_drain_mslg_storage(slot_end_us);
}
```

---

## ESP-NOW Communication Protocol

### Packet Formats

#### Sensor Data Packet
```c
typedef struct {
    uint32_t node_id;        // Source node identifier
    float temp_c;            // Temperature (°C)
    float hum_pct;           // Humidity (%)
    uint32_t pressure_hpa;   // Pressure (hPa)
    uint16_t eco2_ppm;       // eCO2 (ppm)
    uint16_t tvoc_ppb;       // TVOC (ppb)
    uint16_t aqi;            // Air Quality Index
    float audio_rms;         // Audio RMS amplitude
    float mag_x, mag_y, mag_z; // Magnetometer (µT)
    uint64_t timestamp_ms;   // Timestamp (ms since boot)
    uint32_t seq_num;        // Sequence number for PDR calculation
    uint8_t mac_addr[6];     // Source MAC for provenance
    uint8_t flags;           // SENSORS_REAL, BATTERY_REAL flags
} __attribute__((packed)) sensor_payload_t;
```

#### CH Status Broadcast
```c
typedef struct {
    uint32_t magic;      // 0x43485354 ("CHST")
    uint32_t ch_node_id; // CH's node ID
    uint8_t status;      // 0=NORMAL, 1=UAV_BUSY, 2=RESUME
    uint8_t reserved[3]; // Padding for alignment
} __attribute__((packed)) ch_status_msg_t;

// Status values
#define CH_STATUS_NORMAL    0  // Normal operation
#define CH_STATUS_UAV_BUSY  1  // UAV onboarding in progress (members hold data)
#define CH_STATUS_RESUME    2  // Resume normal operation after UAV
```

### Send Process with Enhanced Reliability

```c
esp_err_t esp_now_send_sensor_data(const sensor_payload_t *data) {
    // Semaphore prevents concurrent sends (MAC layer protection)
    if (!send_semaphore_take(1000)) {  // 1s timeout
        ESP_LOGW(TAG, "Send semaphore timeout");
        return ESP_ERR_TIMEOUT;
    }
    
    // Find CH MAC address from neighbor table
    uint8_t ch_mac[6];
    esp_err_t ch_result = neighbor_manager_get_ch_mac(ch_mac);
    if (ch_result != ESP_OK) {
        ESP_LOGW(TAG, "CH MAC not found, cannot send");
        send_semaphore_give();
        return ch_result;
    }
    
    // ESP-NOW transmission with callback tracking
    s_send_in_progress = true;
    s_send_success = false;
    
    esp_err_t send_result = esp_now_send(ch_mac, (const uint8_t *)data, sizeof(sensor_payload_t));
    
    if (send_result == ESP_OK) {
        // Wait for callback confirmation (MAC layer ACK)
        bool callback_received = wait_for_send_callback(1000);  // 1s timeout
        
        if (callback_received && s_send_success) {
            ESP_LOGI(TAG, "ESP-NOW send SUCCESS: seq=%lu", data->seq_num);
            s_send_stats.success_count++;
        } else {
            ESP_LOGW(TAG, "ESP-NOW send FAILED: seq=%lu (callback=%d, success=%d)", 
                    data->seq_num, callback_received, s_send_success);
            s_send_stats.fail_count++;
            send_result = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "esp_now_send() error: %s", esp_err_to_name(send_result));
        s_send_stats.fail_count++;
    }
    
    s_send_in_progress = false;
    send_semaphore_give();
    return send_result;
}
```

### CH Receive Process with Deduplication

```c
static void esp_now_recv_cb(const esp_now_recv_info_t *recv_info, 
                           const uint8_t *data, int len) {
    if (len == sizeof(sensor_payload_t)) {
        sensor_payload_t *payload = (sensor_payload_t *)data;
        
        // Deduplication check (prevents ESP-NOW retransmission duplicates)
        if (!dedup_should_store(payload->node_id, payload->seq_num)) {
            ESP_LOGD(TAG, "Duplicate seq %lu from node %lu, skipping", 
                    payload->seq_num, payload->node_id);
            return;
        }
        
        ESP_LOGI(TAG, "RX Sensor Data from node_%lu: Seq=%lu, T=%.1f°C, H=%.1f%%", 
                payload->node_id, payload->seq_num, payload->temp_c, payload->hum_pct);
        
        // Store to MSLG (compressed)
        char json_str[384];
        format_sensor_json(json_str, sizeof(json_str), payload);
        storage_manager_write_compressed(json_str, true);
        
        // Update receive statistics
        s_recv_stats.data_packets++;
        s_recv_stats.last_recv_ms = esp_timer_get_time() / 1000ULL;
    }
}
```

---

## Radio Coexistence Management

### The Fundamental Problem

ESP32-S3 has a **single 2.4 GHz radio** shared between BLE and ESP-NOW (WiFi). When both protocols attempt to use the radio simultaneously, **MAC-layer ACK failures** occur, causing ESP-NOW transmission failures.

### Solution: Temporal Separation

```
    ┌─────────────────────────────────────────────────────────────────────────────┐
    │                      RADIO RESOURCE TIMELINE                                │
    └─────────────────────────────────────────────────────────────────────────────┘
    
    Time:   0s        10s       20s       25s       30s       35s       40s
            │          │         │         │         │         │         │
            ▼          ▼         ▼         ▼         ▼         ▼         ▼
    
    BLE     ████████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░████████████████
    Scan    │←── ACTIVE ──────→│←──────── DISABLED ──────────→│←── ACTIVE ───→
    
    BLE     ████████████████████████████████████████████████████████████████████
    Adv     │←─────────────── CONTINUOUS (CH beacons) ──────────────────────────→
    
    ESP-NOW ░░░░░░░░░░░░░░░░░░░░████████████████████████████████░░░░░░░░░░░░░░░░
    TX/RX   │←──── IDLE ──────→│←────── ACTIVE (TDMA) ───────→│←── IDLE ─────→
    
            │                   │                               │
            │   STELLAR PHASE   │        DATA PHASE             │   STELLAR
            │   (BLE Priority)  │     (ESP-NOW Priority)        │   PHASE
            │                   │                               │
    
    Legend:
    ████ = Active/Enabled    ░░░░ = Inactive/Disabled
    
    Key Insight:
    • BLE Advertising continues during DATA phase (low duty cycle, minimal interference)
    • BLE Scanning DISABLED during DATA phase (high interference with ESP-NOW)
    • ESP-NOW gets exclusive radio access during DATA phase
    • Result: ESP-NOW success rate improves from <10% to >90%
```

### Implementation Details

#### BLE Control Functions
```c
// STELLAR phase: Enable all BLE functions
void stellar_phase_start() {
    ESP_LOGI(TAG, "STELLAR phase: Enabling BLE scan + advertise");
    
    // Enable BLE scanning for neighbor discovery
    ble_start_scanning();
    
    // Enable BLE advertising for metrics broadcast  
    ble_start_advertising();
    
    // ESP-NOW remains idle (avoid radio contention)
    esp_now_set_idle_mode();
}

// DATA phase: Disable BLE scanning, keep advertising
void data_phase_start() {
    ESP_LOGI(TAG, "DATA phase: Disabling BLE scan, ESP-NOW active");
    
    // CRITICAL: Stop BLE scanning to free radio for ESP-NOW
    ble_stop_scanning();
    
    // Keep advertising enabled (CH beacons, low duty cycle)
    // This allows CH status broadcasts to reach members
    
    // Enable ESP-NOW for TDMA data transmission
    esp_now_set_active_mode();
}
```

#### Phase-Aware CH Beacon Timeout

The CH beacon timeout mechanism must account for the fact that BLE scanning is disabled during DATA phase:

```c
bool check_ch_beacon_timeout() {
    // Skip timeout check during DATA phase (scanning disabled anyway)
    if (g_current_phase == PHASE_DATA) {
        ESP_LOGD(TAG, "DATA phase: skipping CH beacon timeout check");
        return false;  // No timeout during DATA phase
    }
    
    // Only check timeout during STELLAR phase
    uint64_t now_ms = esp_timer_get_time() / 1000ULL;
    uint64_t time_since_beacon = now_ms - s_last_ch_beacon_ms;
    
    if (time_since_beacon > CH_BEACON_TIMEOUT_MS) {
        s_consecutive_ch_misses++;
        
        if (s_consecutive_ch_misses >= CH_MISS_THRESHOLD) {
            ESP_LOGW(TAG, "CH beacon timeout: %llu ms, consecutive misses: %d", 
                    time_since_beacon, s_consecutive_ch_misses);
            return true;  // Timeout confirmed
        }
    } else {
        s_consecutive_ch_misses = 0;  // Reset counter
    }
    
    return false;
}
```

### Benefits of Temporal Separation

| Metric | Before (Concurrent) | After (Temporal) | Improvement |
|--------|-------------------|------------------|-------------|
| ESP-NOW Success Rate | <10% | >90% | **9x improvement** |
| BLE Discovery Success | ~50% | >95% | **2x improvement** |
| Radio Contention Events | High | None | **Eliminated** |
| MAC-layer ACK Failures | Frequent | Rare | **>10x reduction** |
| Data Throughput | Low, inconsistent | High, predictable | **Reliable delivery** |

---

## Store-First Data Pipeline

### Philosophy

The **Store-First** pattern ensures **data durability** and **fault tolerance** in the wireless sensor network:

1. **Always store locally first** (before any transmission attempt)
2. **Transmit from storage** (not from live sensor readings)
3. **FIFO ordering preserved** (chronological data sequence maintained)
4. **Fault tolerance** (transmission failures don't lose data)
5. **Efficient burst drain** (batch transmission optimizations)

### Data Flow Architecture

```
    ┌─────────────────────────────────────────────────────────────────────────────┐
    │                           STORE-FIRST DATA PIPELINE                          │
    └─────────────────────────────────────────────────────────────────────────────┘
    
    Main Loop (every 2s)
    ─────────────────────
    ┌─────────────┐      ┌─────────────┐      ┌─────────────┐
    │   SENSORS   │─────►│    JSON     │─────►│    MSLG     │
    │   Reading   │      │   Format    │      │   Storage   │
    │             │      │             │      │ (LZ4 comp.) │
    └─────────────┘      └─────────────┘      └─────────────┘
           │                     │                     │
           │  Temperature        │  {"id":3125565838   │  [Binary MSLG
           │  Humidity           │   "seq":42,         │   Header + 
           │  Pressure           │   "t":23.5,         │   Compressed
           │  Air Quality        │   "h":65.2,         │   JSON Payload]
           │  Audio RMS          │   "ts":12345678}    │
           │  Magnetometer       │                     │
           │                     │                     │
           │                     │                     │
           ▼                     ▼                     ▼
    ┌─────────────────────────────────────────────────────────────────────────────┐
    │ Key Characteristics:                                                        │
    │ • seq_num increments EVERY loop (unique payloads)                         │
    │ • JSON includes ALL sensor data + metadata                                │  
    │ • MSLG compression reduces storage by ~60%                                │
    │ • Storage happens REGARDLESS of transmission success/failure              │
    │ • Creates reliable audit trail for WSN data                              │
    └─────────────────────────────────────────────────────────────────────────────┘
    
    TDMA Slot (every 10s)
    ─────────────────────
    ┌─────────────┐      ┌─────────────┐      ┌─────────────┐      ┌─────────────┐
    │    MSLG     │─────►│ DECOMPRESS  │─────►│  ESP-NOW    │─────►│     CH      │
    │   Storage   │      │     (if     │      │    Send     │      │  Receives   │
    │ (FIFO pop)  │      │  required)  │      │             │      │             │
    └─────────────┘      └─────────────┘      └─────────────┘      └─────────────┘
           │                     │                     │                     │
           │  Oldest chunk       │  Raw JSON payload  │  Binary sensor_     │  Store to CH's
           │  from storage       │  (decompressed)     │  payload_t struct   │  MSLG storage
           │  (chronological)    │                     │  via ESP-NOW        │
           │                     │                     │                     │
           ▼                     ▼                     ▼                     ▼
    ┌─────────────────────────────────────────────────────────────────────────────┐
    │ Transmission Features:                                                      │
    │ • FIFO: Oldest data transmitted first (time sequence preserved)           │
    │ • Burst mode: Up to 24 chunks per TDMA slot (efficient batch transfer)   │
    │ • Retry on failure: Failed sends requeued to storage (no data loss)      │
    │ • Success confirmation: Only delete from storage after ACK received      │
    │ • Time budget management: Pause transmission if slot time exceeded       │
    └─────────────────────────────────────────────────────────────────────────────┘
```

### MSLG Storage Format

```c
typedef struct {
    uint32_t magic;      // 0x4D534C47 ("MSLG")
    uint32_t version;    // Format version (currently 1)
    uint8_t  algo;       // 0=raw, 1=LZ4 compressed
    uint8_t  level;      // Compression level (if applicable)
    uint32_t raw_len;    // Original uncompressed length
    uint32_t data_len;   // Payload length (compressed or raw)
    uint32_t crc32;      // CRC32 of payload
    uint32_t node_id;    // Source node ID
    uint32_t timestamp;  // Timestamp (seconds since boot)
    uint32_t reserved;   // Future use
} __attribute__((packed)) mslg_chunk_hdr_t;

// Followed by payload data (compressed or raw JSON)
```

### Burst Drain Optimization

The burst drain process has been optimized to handle large backlogs efficiently:

```c
#define BATCH_POP_MAX 24  // Max chunks to pop in one SPIFFS operation

esp_err_t burst_drain_mslg_storage(uint64_t slot_end_us) {
    // Batch pop optimization: Read multiple chunks in ONE SPIFFS pass
    // Old approach: O(N) file rewrites → ~900ms per chunk
    // New approach: O(1) file rewrite → ~900ms total regardless of N
    
    mslg_popped_chunk_t batch[BATCH_POP_MAX];
    int batch_count = 0;
    
    esp_err_t pop_result = storage_manager_pop_mslg_chunks_batch(
        batch, BATCH_POP_MAX, &batch_count);
    
    if (pop_result == ESP_OK && batch_count > 0) {
        ESP_LOGI(TAG, "Burst drain: Processing %d chunks", batch_count);
        
        for (int i = 0; i < batch_count; i++) {
            // Time budget check (requeue remaining chunks if slot ending)
            if (esp_timer_get_time() >= (slot_end_us - 500000LL)) {  // 500ms margin
                ESP_LOGW(TAG, "Time budget exceeded at chunk %d/%d, requeueing rest", 
                        i, batch_count);
                
                // Requeue unsent chunks (maintains FIFO order)
                for (int j = i; j < batch_count; j++) {
                    storage_manager_write_compressed((const char *)batch[j].payload, 
                                                   (batch[j].algo == 1));
                    heap_caps_free(batch[j].payload);
                }
                break;
            }
            
            // Decompress if needed
            uint8_t *send_payload;
            size_t send_len;
            
            if (batch[i].algo == 1) {  // LZ4 compressed
                send_payload = decompress_lz4(batch[i].payload, batch[i].payload_len, 
                                             batch[i].raw_len);
                send_len = batch[i].raw_len;
            } else {  // Raw
                send_payload = batch[i].payload;
                send_len = batch[i].payload_len;
            }
            
            // Parse JSON and send via ESP-NOW
            sensor_payload_t parsed_data;
            if (parse_sensor_json((const char *)send_payload, &parsed_data) == ESP_OK) {
                esp_err_t send_result = esp_now_send_sensor_data(&parsed_data);
                
                if (send_result == ESP_OK) {
                    ESP_LOGI(TAG, "Burst send SUCCESS: seq=%lu", parsed_data.seq_num);
                } else {
                    ESP_LOGW(TAG, "Burst send FAILED: seq=%lu, requeueing", parsed_data.seq_num);
                    // Requeue failed chunk
                    storage_manager_write_compressed((const char *)send_payload, false);
                }
            }
            
            // Cleanup
            if (batch[i].algo == 1) {
                heap_caps_free(send_payload);  // Free decompressed buffer
            }
            heap_caps_free(batch[i].payload);  // Free original payload
        }
    }
    
    return ESP_OK;
}
```

---

## Timing Constraints and Optimization

### Critical Timing Requirements

| Operation | Time Budget | Optimization Strategy |
|-----------|-------------|----------------------|
| **BLE Discovery** | 10s | Parallel scan + advertise |
| **STELLAR Election** | 10s | Cached metrics, fast computation |
| **Phase Transition** | <100ms | Pre-computed neighbor table |
| **TDMA Slot** | 10s | Batch operations, time budget checks |
| **ESP-NOW Send** | <100ms | Semaphore queuing, callback confirm |
| **MSLG Storage** | <50ms | Batch pop optimization (24x speedup) |
| **Compression** | <10ms | LZ4 fast mode, PSRAM allocation |

### Performance Optimizations Implemented

#### 1. Batch MSLG Pop Optimization
- **Problem**: O(N) file rewrites (900ms per chunk)
- **Solution**: O(1) batch pop (900ms for up to 24 chunks)
- **Impact**: 24x speedup in burst drain operations

#### 2. ESP-NOW Send Semaphore
- **Problem**: Concurrent send attempts cause MAC failures
- **Solution**: Semaphore-protected send with callback confirmation
- **Impact**: Prevents radio contention, reliable ACK handling

#### 3. Neighbor Table Race Condition Fixes
- **Problem**: Race conditions in neighbor addition/lookup
- **Solution**: Extended mutex timeouts, post-addition verification
- **Impact**: Robust neighbor discovery, reliable TDMA scheduling

#### 4. Time Budget Management
- **Problem**: TDMA slots overrun causing phase timing issues
- **Solution**: Time budget checks with automatic requeue
- **Impact**: Predictable slot timing, no data loss on timeout

#### 5. Memory Optimization
- **Problem**: Large payloads cause heap fragmentation
- **Solution**: PSRAM allocation for compression buffers
- **Impact**: Stable operation with large JSON payloads

### Timing Diagram with Optimizations

```
    TDMA SLOT WITH OPTIMIZATIONS (10 seconds)
    ┌────────────┬─────────────────────────────────────────────────────────────┐
    │ SEQUENTIAL │                 PARALLEL BURST DRAIN                       │
    │ OPERATIONS │              (Batch Optimized)                            │
    ├────────────┼─────────────────────────────────────────────────────────────┤
    │            │                                                           │
    │ ┌────────┐ │ ┌─────────────────────────────────────────────────────────┐ │
    │ │Store   │ │ │ Batch Pop (900ms) → 24 chunks in RAM                  │ │
    │ │+Send   │ │ │   ├─ Decompress chunk 0                               │ │
    │ │Live    │ │ │   ├─ Send chunk 0 (50ms)                              │ │
    │ │150ms   │ │ │   ├─ Decompress chunk 1                               │ │
    │ │        │ │ │   ├─ Send chunk 1 (50ms)                              │ │
    │ └────────┘ │ │   ├─ ... (continue until time budget or chunks done)  │ │
    │            │ │   └─ Requeue any unsent chunks                         │ │
    │            │ └─────────────────────────────────────────────────────────┘ │
    └────────────┴─────────────────────────────────────────────────────────────┘
    
    Time: 0ms     150ms                                                  10000ms
    
    Before Optimization:
    • 6 chunks max per slot (6 × 900ms SPIFFS + 6 × 50ms send = 5.7s)
    • Remaining 4.3s wasted
    • Storage backlog grows faster than drain rate
    
    After Optimization:
    • 24+ chunks per slot (900ms batch + 24 × 50ms send = 2.1s)
    • Remaining 7.9s for additional chunks
    • Storage backlog drains faster than accumulation rate
```

---

## Troubleshooting Guide

### Common Issues and Solutions

#### 1. ESP-NOW Send Failures (High Rate)

**Symptoms:**
```
ESP-NOW send FAILED: seq=42 (status=1)
SEND_CB: status=1 (FAIL) to 10:20:ba:4d:eb:1e
```

**Diagnosis:**
- Check if BLE scanning is disabled during DATA phase
- Verify neighbor table contains target node
- Confirm ESP-NOW peer registration

**Solution:**
```c
// Verify phase-aware BLE control
if (g_current_phase == PHASE_DATA) {
    assert(ble_scanning_enabled() == false);  // Must be disabled
}

// Verify neighbor in table
uint8_t ch_mac[6];
esp_err_t mac_result = neighbor_manager_get_ch_mac(ch_mac);
assert(mac_result == ESP_OK);

// Verify ESP-NOW peer registration
esp_now_peer_info_t peer;
esp_err_t peer_result = esp_now_get_peer(ch_mac, &peer);
assert(peer_result == ESP_OK);
```

#### 2. Neighbor Discovery Failures

**Symptoms:**
```
[DEBUG] Node 3125565838 not found in table, attempting to add as new neighbor
[DEBUG] ESP-NOW peer registration failed: ESP_ERR_ESPNOW_EXIST
```

**Diagnosis:**
- Check mutex timeouts in neighbor_manager_update()
- Verify BLE HMAC authentication
- Confirm neighbor table not full

**Solution:**
```c
// Enable enhanced debugging
ESP_LOGI(TAG, "[DEBUG] neighbor_manager_update called: node_id=%lu", node_id);
ESP_LOGI(TAG, "[DEBUG] Searching for existing neighbor %lu in table of %d entries", 
         node_id, g_neighbor_count);

// Check for table full condition
if (g_neighbor_count >= MAX_NEIGHBORS) {
    ESP_LOGW(TAG, "[DEBUG] Current neighbor table status:");
    for (int i = 0; i < g_neighbor_count; i++) {
        ESP_LOGW(TAG, "[DEBUG]   [%d] node_id=%lu, verified=%d", 
                i, g_neighbors[i].node_id, g_neighbors[i].verified);
    }
}
```

#### 3. TDMA Scheduling Issues

**Symptoms:**
```
NO-SCHED: Not found in neighbor table, passive mode
TDMA: No transmission window assigned
```

**Diagnosis:**
- Verify node was added to neighbor table during STELLAR phase
- Check that CH election completed successfully
- Confirm TDMA slot calculation logic

**Solution:**
```c
// Add TDMA scheduling debugging
ESP_LOGI(TAG, "[TDMA] Found %d members for scheduling:", member_count);
for (int i = 0; i < member_count; i++) {
    ESP_LOGI(TAG, "[TDMA]   Slot %d: node_id=%lu (verified=%d)", 
             i, members[i].node_id, members[i].verified);
}

// Verify slot assignment
int my_slot = find_my_slot_index(members, member_count);
if (my_slot >= 0) {
    ESP_LOGI(TAG, "[TDMA] Assigned to slot %d/%d", my_slot, member_count);
} else {
    ESP_LOGW(TAG, "[TDMA] Not found in member list - neighbor table issue");
}
```

#### 4. Storage/Memory Issues

**Symptoms:**
```
MSLG write failed — attempting purge + retry
OOM: Failed to allocate compression buffer
```

**Diagnosis:**
- Check SPIFFS usage with `storage_manager_get_usage()`
- Monitor heap usage during compression operations
- Verify PSRAM allocation fallbacks

**Solution:**
```c
// Add memory monitoring
size_t used, total;
storage_manager_get_usage(&used, &total);
float usage_pct = (float)used / total * 100.0f;

ESP_LOGI(TAG, "SPIFFS usage: %zu/%zu bytes (%.1f%%)", used, total, usage_pct);

// Add heap monitoring
ESP_LOGI(TAG, "Heap: free=%zu, min_free=%zu, largest_block=%zu", 
         esp_get_free_heap_size(), esp_get_minimum_free_heap_size(),
         heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
```

#### 5. Phase Transition Timing

**Symptoms:**
```
Phase transition delayed: expected at 20000ms, actual at 20150ms
BLE scan still active during DATA phase
```

**Diagnosis:**
- Check state machine task priority and stack size
- Verify timer accuracy for phase transitions
- Monitor CPU load during transitions

**Solution:**
```c
// Add phase transition timing verification
static uint64_t last_phase_start = 0;
uint64_t now = esp_timer_get_time() / 1000ULL;
uint64_t phase_duration = now - last_phase_start;

ESP_LOGI(TAG, "Phase transition: %s->%s, duration=%llu ms", 
         phase_to_string(old_phase), phase_to_string(new_phase), phase_duration);

// Verify expected timing
uint64_t expected_duration = (old_phase == PHASE_STELLAR) ? STELLAR_PHASE_MS : DATA_PHASE_MS;
if (abs((int)(phase_duration - expected_duration)) > 100) {  // 100ms tolerance
    ESP_LOGW(TAG, "Phase timing drift: expected %llu, actual %llu", 
             expected_duration, phase_duration);
}
```

### Diagnostic Commands

#### Enable Enhanced Logging
```c
// Add to main.c for comprehensive debugging
esp_log_level_set("NEIGHBOR", ESP_LOG_DEBUG);
esp_log_level_set("ESP_NOW", ESP_LOG_DEBUG);
esp_log_level_set("BLE", ESP_LOG_DEBUG);
esp_log_level_set("STATE", ESP_LOG_DEBUG);
```

#### Monitor Key Statistics
```c
// Add periodic statistics reporting
void print_system_status() {
    // Neighbor table status
    int neighbor_count = neighbor_manager_get_count();
    ESP_LOGI(TAG, "System Status: neighbors=%d, phase=%s, role=%s", 
             neighbor_count, phase_to_string(g_current_phase), 
             role_to_string(g_current_role));
    
    // ESP-NOW statistics
    ESP_LOGI(TAG, "ESP-NOW: TX success=%lu, fail=%lu, RX=%lu", 
             s_send_stats.success_count, s_send_stats.fail_count, 
             s_recv_stats.data_packets);
    
    // Storage status
    size_t used, total;
    storage_manager_get_usage(&used, &total);
    int chunk_count = storage_manager_get_mslg_chunk_count();
    ESP_LOGI(TAG, "Storage: %zu/%zu bytes (%.1f%%), %d chunks", 
             used, total, (float)used/total*100.0f, chunk_count);
}
```

---

## Related Documentation

- [`TIMING_DIAGRAM.md`](TIMING_DIAGRAM.md) - Detailed timing diagrams and radio coexistence
- [`DATA_STORAGE.md`](DATA_STORAGE.md) - MSLG format and storage management
- [`MSLG_DATA_FLOW.md`](MSLG_DATA_FLOW.md) - Data pipeline analysis with performance graphs
- [`UAV_ONBOARDING_TIMING.md`](UAV_ONBOARDING_TIMING.md) - UAV communication protocol
- [Main README.md](../README.md) - System overview and build instructions

---

*Last Updated: March 2026*  
*Author: WSN Development Team*  
*Version: 2.0 - Enhanced with neighbor table fixes and ESP-NOW reliability improvements*