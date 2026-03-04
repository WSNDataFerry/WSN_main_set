#ifndef ESP_NOW_MANAGER_H
#define ESP_NOW_MANAGER_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Data structure for cluster messages
typedef struct {
  uint32_t sender_id;
  uint32_t type;
  uint8_t payload[240]; // Max ESP-NOW payload is 250 bytes
} cluster_message_t;

// Time Slicing Schedule Message
typedef struct {
  int64_t epoch_us;
  uint8_t slot_index;
  uint8_t slot_duration_sec;
  uint32_t magic; // 0x53434845 "SCHE"
  uint32_t target_node_id; // Node this slot is for (used by broadcast schedule)
} schedule_msg_t;

// CH Status Broadcast Message (for UAV onboarding notification)
typedef struct {
  uint32_t magic;         // 0x43485354 "CHST" (CH Status)
  uint32_t ch_node_id;    // CH node ID
  uint8_t status;         // 0 = NORMAL, 1 = UAV_BUSY, 2 = RESUME
  uint8_t reserved[3];    // Padding for alignment
} ch_status_msg_t;

#define ESP_NOW_MAGIC_SCHEDULE 0x53434845
#define ESP_NOW_MAGIC_COMPRESSED 0xCF
#define ESP_NOW_MAGIC_CH_STATUS 0x43485354

// CH Status codes
#define CH_STATUS_NORMAL    0   // Normal operation
#define CH_STATUS_UAV_BUSY  1   // CH busy with UAV onboarding - members hold data
#define CH_STATUS_RESUME    2   // CH ready, members can resume TDMA

/**
 * @brief Initialize ESP-NOW and Wi-Fi
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t esp_now_manager_init(void);

/**
 * @brief Deinitialize ESP-NOW (for UAV onboarding WiFi STA mode)
 * @note Must be called before switching WiFi to connect to UAV hotspot
 * @return esp_err_t ESP_OK on success
 */
esp_err_t esp_now_manager_deinit(void);

/**
 * @brief Reinitialize ESP-NOW after UAV onboarding
 * @note Must be called after UAV onboarding completes to restore ESP-NOW
 * @return esp_err_t ESP_OK on success
 */
esp_err_t esp_now_manager_reinit(void);

/**
 * @brief Register a peer for ESP-NOW communication
 *
 * @param peer_addr MAC address of the peer
 * @param encrypt Whether to use encryption
 * @return esp_err_t ESP_OK on success
 */
esp_err_t esp_now_manager_register_peer(const uint8_t *peer_addr, bool encrypt);

/**
 * @brief Send data to a specific peer
 *
 * @param peer_addr MAC address of the peer (NULL for broadcast)
 * @param data Pointer to data buffer
 * @param len Length of data
 * @return esp_err_t ESP_OK on success
 */
esp_err_t esp_now_manager_send_data(const uint8_t *peer_addr,
                                    const uint8_t *data, size_t len);

/**
 * @brief Send data via ESP-NOW with minimal overhead (DATA phase burst drain).
 *
 * Skips the BLE stop/start + 120 ms quiet window that the normal send path
 * uses.  ONLY safe to call when the caller guarantees BLE scanning and
 * advertising are already stopped (i.e. during DATA phase for MEMBER nodes).
 *
 * @param peer_addr MAC address of target peer
 * @param data Pointer to data buffer
 * @param len Length of data
 * @return esp_err_t ESP_OK on success
 */
esp_err_t esp_now_manager_send_data_fast(const uint8_t *peer_addr,
                                          const uint8_t *data, size_t len);

/**
 * @brief Get the latest received Time Slicing schedule
 */
schedule_msg_t esp_now_get_current_schedule(void);

/**
 * @brief Broadcast CH status to all members
 * @param status CH_STATUS_NORMAL, CH_STATUS_UAV_BUSY, or CH_STATUS_RESUME
 * @return esp_err_t ESP_OK on success
 */
esp_err_t esp_now_manager_broadcast_ch_status(uint8_t status);

/**
 * @brief Check if CH is currently busy (UAV onboarding)
 * @return true if CH is busy, false if normal operation
 */
bool esp_now_manager_is_ch_busy(void);

/**
 * @brief Get timestamp of last CH status update
 * @return timestamp in ms, 0 if no status received
 */
uint64_t esp_now_manager_get_ch_status_time(void);

#endif // ESP_NOW_MANAGER_H
