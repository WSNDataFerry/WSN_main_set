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
} schedule_msg_t;

#define ESP_NOW_MAGIC_SCHEDULE 0x53434845
#define ESP_NOW_MAGIC_COMPRESSED 0xCF

/**
 * @brief Initialize ESP-NOW and Wi-Fi
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t esp_now_manager_init(void);

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
 * @brief Get the latest received Time Slicing schedule
 */
schedule_msg_t esp_now_get_current_schedule(void);

#endif // ESP_NOW_MANAGER_H
