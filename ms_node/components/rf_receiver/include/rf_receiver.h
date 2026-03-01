#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Configuration
#define RF_RECEIVER_GPIO 21
#define RF_EXPECTED_CODE 22

/**
 * @brief Initialize the RF receiver (RMT)
 */
esp_err_t rf_receiver_init(void);

/**
 * @brief Check if the specific UAV trigger code has been received
 * @return true if trigger received, false otherwise
 */
bool rf_receiver_check_trigger(void);

/**
 * @brief Manually force a trigger event (for testing without RF remote)
 */
void rf_receiver_force_trigger(void);
