#ifndef NEIGHBOR_MANAGER_H
#define NEIGHBOR_MANAGER_H

#include "metrics.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t node_id;
  uint8_t mac_addr[6]; // Added MAC address
  float score;
  float battery;
  uint64_t uptime_seconds; // Renamed for consistency with STELLAR
  float trust;
  float link_quality;
  float rssi_ewma;
  int8_t last_rssi;
  uint64_t last_seen_ms;
  bool is_ch;
  uint64_t ch_announce_timestamp;
  bool verified;        // HMAC verified
  uint8_t last_seq_num; // Last received sequence number
} neighbor_entry_t;

/**
 * @brief Initialize neighbor manager
 */
void neighbor_manager_init(void);

void neighbor_manager_update(uint32_t node_id, const uint8_t *mac_addr,
                             int8_t rssi, float score, float battery,
                             uint64_t uptime, float trust, float link_quality,
                             bool is_ch, uint8_t seq_num);
bool neighbor_manager_get(uint32_t node_id, neighbor_entry_t *out_entry);
bool neighbor_manager_get_by_mac(const uint8_t *mac_addr,
                                 neighbor_entry_t *out_entry);

/**
 * @brief Get all neighbors (for election)
 * @param neighbors Output array
 * @param max_count Maximum count
 * @return Number of neighbors returned
 */
size_t neighbor_manager_get_all(neighbor_entry_t *neighbors, size_t max_count);

/**
 * @brief Remove stale neighbors (timeout)
 */
void neighbor_manager_cleanup_stale(void);

/**
 * @brief Check if neighbor is within cluster radius
 * @param neighbor Neighbor entry
 * @return true if within radius
 */
bool neighbor_manager_is_in_cluster(const neighbor_entry_t *neighbor);

/**
 * @brief Get current CH MAC address
 * @param mac_out Output buffer (6 bytes)
 * @return true if found
 */
bool neighbor_manager_get_ch_mac(uint8_t *mac_out);

/**
 * @brief Get current CH from neighbors
 * @return CH node ID or 0 if none
 */
uint32_t neighbor_manager_get_current_ch(void);

/**
 * @brief Update neighbor trust based on interaction
 * @param node_id Node ID
 * @param success Whether interaction was successful
 */
/**
 * @brief Update neighbor trust based on interaction
 * @param node_id Node ID
 * @param success Whether interaction was successful
 */
void neighbor_manager_update_trust(uint32_t node_id, bool success);

/**
 * @brief Get total number of active neighbors
 * @return Count
 */
size_t neighbor_manager_get_count(void);

/**
 * @brief Get number of cluster members (verified, in cluster, not CH).
 *        For use by CH to know its member count.
 * @return Member count
 */
size_t neighbor_manager_get_member_count(void);

#endif // NEIGHBOR_MANAGER_H
