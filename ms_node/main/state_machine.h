#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
  STATE_INIT,
  STATE_DISCOVER,
  STATE_CANDIDATE,
  STATE_CH,
  STATE_MEMBER,
  STATE_UAV_ONBOARDING,
  STATE_SLEEP
} node_state_t;
// ... existing includes ...

void state_machine_init(void);
void state_machine_run(void);
const char *state_machine_get_state_name(void);

extern node_state_t g_current_state;
extern bool g_is_ch;
extern uint32_t g_node_id;
extern uint64_t g_mac_addr;

/**
 * @brief Initialize state machine
 */
void state_machine_init(void);

/**
 * @brief Run state machine (call periodically)
 */
void state_machine_run(void);

/**
 * @brief Get current state name
 */
const char *state_machine_get_state_name(void);

/**
 * @brief Calculate time to sleep until next slot (Smart Wake-up)
 * @return Sleep time in ms (default 5000 if no schedule)
 */
/**
 * @brief Calculate time to sleep until next slot (Smart Wake-up)
 * @return Sleep time in ms (default 5000 if no schedule)
 */
uint32_t state_machine_get_sleep_time_ms(void);

/**
 * @brief Synchronize the local superframe phase timer to the CH's schedule.
 *
 * Called by the ESP-NOW receive path when a schedule_msg_t is received. The
 * epoch_us in the schedule encodes when the current DATA phase started on the
 * CH: epoch_us = data_phase_start_us + PHASE_GUARD_MS*1000.
 * Back-calculating the CH's stellar_start_us allows the member to align its
 * own s_phase_start_ms so that both nodes enter DATA phase simultaneously.
 *
 * @param epoch_us  The epoch_us field from the received schedule_msg_t.
 */
void state_machine_sync_phase_from_epoch(int64_t epoch_us);

#endif // STATE_MACHINE_H
