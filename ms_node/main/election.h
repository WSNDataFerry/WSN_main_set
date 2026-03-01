#ifndef ELECTION_H
#define ELECTION_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize election system
 */
void election_init(void);

/**
 * @brief Run distributed CH election
 * @return Node ID of elected CH (may be self)
 */
uint32_t election_run(void);

/**
 * @brief Check if re-election is needed
 * @return true if re-election should be triggered
 */
bool election_check_reelection_needed(void);

/**
 * @brief Get election window start time
 * @return Timestamp in milliseconds
 */
uint64_t election_get_window_start(void);

/**
 * @brief Reset election window
 */
void election_reset_window(void);

#endif // ELECTION_H

