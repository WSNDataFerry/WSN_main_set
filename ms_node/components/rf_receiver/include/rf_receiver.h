#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Configuration
#define RF_RECEIVER_GPIO 21
#define RF_EXPECTED_CODE 22           // UAV sends code 22 to trigger onboarding
#define RMT_CHAN_MEM_SYMBOLS 64
#define RMT_RESOLUTION_HZ 1000000     // 1MHz, 1us per tick
#define RCSWITCH_T_US 350             // Protocol 1: T approx 350us
#define RCSWITCH_TOLERANCE 0.40f      // +/- 40% tolerance
#define RMT_RX_SYMBOLS 4096           // Increased to accommodate longer/combined captures
#define RMT_MIN_PULSE_NS 3000 
#define RF_TASK_STACK_SIZE 8192


/**
 * @brief Initialize the RF receiver (RMT)
 * @return ESP_OK on success
 */
esp_err_t rf_receiver_init(void);

/**
 * @brief Check if the specific UAV trigger code (22) has been received
 * @return true if trigger received, false otherwise
 */
bool rf_receiver_check_trigger(void);

/**
 * @brief Manually force a trigger event (for testing without RF remote)
 */
void rf_receiver_force_trigger(void);

/**
 * @brief Disable RF receiver to prevent multiple triggers during UAV onboarding
 */
void rf_receiver_disable(void);

/**
 * @brief Re-enable RF receiver after UAV onboarding is complete
 */
void rf_receiver_enable(void);
