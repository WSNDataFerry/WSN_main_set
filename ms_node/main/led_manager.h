#ifndef LED_MANAGER_H
#define LED_MANAGER_H

#include "state_machine.h"

// Initialize LED manager (GPIO setup)
void led_manager_init(void);

// Update LED state based on node state
void led_manager_set_state(node_state_t state);

#endif // LED_MANAGER_H
