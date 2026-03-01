#ifndef AUTH_H
#define AUTH_H

#include "config.h"
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * @brief Generate HMAC-SHA256 for a message
 * @param message Message to authenticate
 * @param msg_len Message length
 * @param key Cluster key
 * @param hmac_out Output buffer (32 bytes for SHA256)
 * @return true on success
 */
bool auth_generate_hmac(const uint8_t *message, size_t msg_len,
                        const uint8_t *key, uint8_t *hmac_out);

/**
 * @brief Verify HMAC of a received message
 * @param message Received message
 * @param msg_len Message length
 * @param received_hmac HMAC from message
 * @param key Cluster key
 * @return true if HMAC is valid
 */
bool auth_verify_hmac(const uint8_t *message, size_t msg_len,
                      const uint8_t *received_hmac, const uint8_t *key);

/**
 * @brief Check for replay attacks using timestamp
 * @param timestamp Message timestamp
 * @param node_id Source node ID
 * @return true if timestamp is valid (not a replay)
 */
bool auth_check_replay(uint64_t timestamp, uint32_t node_id);

/**
 * @brief Initialize authentication system
 */
void auth_init(void);

#endif // AUTH_H
