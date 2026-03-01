#ifndef PERSISTENCE_H
#define PERSISTENCE_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize persistence system (SPIFFS, RTC)
 */
void persistence_init(void);

/**
 * @brief Save reputation table to SPIFFS
 */
void persistence_save_reputations(void);

/**
 * @brief Load reputation table from SPIFFS
 */
void persistence_load_reputations(void);

#endif // PERSISTENCE_H

