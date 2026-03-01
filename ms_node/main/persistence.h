#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Initialize persistence system (SPIFFS, RTC)
 */
void persistence_init(void);

/**
 * @brief Save reputation (node_id, trust) pairs to SPIFFS
 * @param node_ids Array of node IDs
 * @param trusts Array of trust values (0.0f..1.0f)
 * @param count Number of entries
 */
void persistence_save_reputations(const uint32_t *node_ids, const float *trusts,
                                  size_t count);

/**
 * @brief Load reputation table from SPIFFS into internal cache.
 * Use persistence_get_initial_trust() when adding neighbors to apply cached values.
 */
void persistence_load_reputations(void);

/**
 * @brief Get cached initial trust for a node (e.g. when adding a new neighbor).
 * @param node_id Node ID
 * @param trust Output trust value if found
 * @return true if cache had an entry for node_id
 */
bool persistence_get_initial_trust(uint32_t node_id, float *trust);

#endif // PERSISTENCE_H

