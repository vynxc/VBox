

#ifndef STATE_MANAGEMENT_H
#define STATE_MANAGEMENT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif





typedef struct {

    uint32_t last_watchdog_time;
    uint32_t last_visual_time;
    uint32_t last_error_check_time;
    uint32_t last_button_time;
    

    uint32_t watchdog_status_timer;
    

    uint32_t last_button_press_time;
    bool button_pressed_last;
    bool usb_reset_cooldown;
    uint32_t usb_reset_cooldown_start;
    

    bool device_initialized;
    bool host_initialized;
    bool watchdog_active;
    
} system_state_t;






void system_state_init(system_state_t* state);


system_state_t* get_system_state(void);


void system_state_update_timing(system_state_t* state, uint32_t current_time);
bool system_state_should_run_task(const system_state_t* state, uint32_t current_time, 
                                  uint32_t last_run_time, uint32_t interval_ms);


void system_state_batch_update_timers(system_state_t* state, uint32_t current_time,
                                     uint8_t update_flags);

#ifdef __cplusplus
}
#endif

#endif // STATE_MANAGEMENT_H
