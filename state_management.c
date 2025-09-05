/*
 * Hurricane PIOKMBox Firmware
*/

#include "state_management.h"
#include "defines.h"
#include <string.h>

static system_state_t g_system_state;

void system_state_init(system_state_t* state) {
    memset(state, 0, sizeof(system_state_t));
    // Set any non-zero initial values here
}

system_state_t* get_system_state(void) {
    return &g_system_state;
}

// Performance-optimized inline time check function
inline bool system_state_should_run_task(const system_state_t* state, uint32_t current_time,
                                  uint32_t last_run_time, uint32_t interval_ms) {
    (void)state; // Suppress unused parameter warning
    return (current_time - last_run_time) >= interval_ms;
}

// Batch update function for performance - updates multiple timers at once
void system_state_batch_update_timers(system_state_t* state, uint32_t current_time,
                                     uint8_t update_flags) {
    if (update_flags & 0x01) state->last_watchdog_time = current_time;
    if (update_flags & 0x02) state->last_visual_time = current_time;
    if (update_flags & 0x04) state->last_button_time = current_time;
    if (update_flags & 0x08) state->watchdog_status_timer = current_time;
}
