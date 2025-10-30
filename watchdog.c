/*
 * Hurricane PIOKMBox Firmware
 */


#include "watchdog.h"
#include "defines.h"
#include "pico/stdlib.h"
#include "hardware/watchdog.h"
#include "hardware/gpio.h"
#include "pico/multicore.h"
#include "pico/time.h"
#include <stdio.h>
#include <string.h>





static watchdog_status_t g_watchdog_status;
static bool g_watchdog_initialized = false;
static bool g_watchdog_started = false;
static bool g_debug_enabled = WATCHDOG_ENABLE_DEBUG;
static uint32_t g_last_hardware_update_ms = 0;


static volatile uint32_t g_core0_heartbeat_timestamp = 0;
static volatile uint32_t g_core1_heartbeat_timestamp = 0;





/**
 * Get current time in milliseconds since boot
 */
static uint32_t get_time_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

/**
 * Update hardware watchdog timer
 */
static void update_hardware_watchdog(void) {
    if (WATCHDOG_ENABLE_HARDWARE) {
        watchdog_update();
        g_watchdog_status.hardware_updates++;
        g_last_hardware_update_ms = get_time_ms();
    }
}

/**
 * Check if a core is responsive based on its last heartbeat
 */
static bool is_core_responsive(uint32_t last_heartbeat_ms, uint32_t current_time_ms) {
    if (last_heartbeat_ms == 0) {

        return false;
    }
    
    uint32_t time_since_heartbeat = current_time_ms - last_heartbeat_ms;
    return time_since_heartbeat <= WATCHDOG_CORE_TIMEOUT_MS;
}

/**
 * Handle timeout warning
 */
static void handle_timeout_warning(int core_num, uint32_t time_since_heartbeat) {
    g_watchdog_status.timeout_warnings++;
    
}

/**
 * Check inter-core health and update status
 */
static void check_inter_core_health(void) {
    if (!WATCHDOG_ENABLE_INTER_CORE) {
        return;
    }
    
    uint32_t current_time = get_time_ms();
    

    g_watchdog_status.core0_last_heartbeat_ms = g_core0_heartbeat_timestamp;
    g_watchdog_status.core1_last_heartbeat_ms = g_core1_heartbeat_timestamp;
    

    bool core0_was_responsive = g_watchdog_status.core0_responsive;
    g_watchdog_status.core0_responsive = is_core_responsive(
        g_watchdog_status.core0_last_heartbeat_ms, current_time);
    
    if (!g_watchdog_status.core0_responsive && core0_was_responsive) {
        uint32_t time_since = current_time - g_watchdog_status.core0_last_heartbeat_ms;
        handle_timeout_warning(0, time_since);
    }
    

    bool core1_was_responsive = g_watchdog_status.core1_responsive;
    g_watchdog_status.core1_responsive = is_core_responsive(
        g_watchdog_status.core1_last_heartbeat_ms, current_time);
    
    if (!g_watchdog_status.core1_responsive && core1_was_responsive) {
        uint32_t time_since = current_time - g_watchdog_status.core1_last_heartbeat_ms;
        handle_timeout_warning(1, time_since);
    }
    

    g_watchdog_status.system_healthy = 
        g_watchdog_status.core0_responsive && g_watchdog_status.core1_responsive;
    

    if (!g_watchdog_status.system_healthy) {
        static uint32_t unhealthy_start_time = 0;
        if (unhealthy_start_time == 0) {
            unhealthy_start_time = current_time;
        } else if (current_time - unhealthy_start_time > WATCHDOG_CORE_TIMEOUT_MS * 2) {
            watchdog_force_reset();
        }
    } else {



    }
}





void watchdog_init(void) {
    if (g_watchdog_initialized) {
        return;
    }
    

    memset(&g_watchdog_status, 0, sizeof(g_watchdog_status));
    

    g_core0_heartbeat_timestamp = 0;
    g_core1_heartbeat_timestamp = 0;
    g_last_hardware_update_ms = 0;
    

    g_watchdog_status.system_healthy = false;
    g_watchdog_status.core0_responsive = false;
    g_watchdog_status.core1_responsive = false;
    
    g_watchdog_initialized = true;
    
   
}

void watchdog_start(void) {
    if (!g_watchdog_initialized) {
        return;
    }
    
    if (g_watchdog_started) {
        return;
    }
    

    for (int i = 0; i < 30; i++) {
        watchdog_core0_heartbeat();  // Send heartbeat during wait

        gpio_put(PIN_LED, (i % 4 < 2) ? 1 : 0);  // 2 on, 2 off pattern
        sleep_ms(100);
    }
    gpio_put(PIN_LED, 1);  // LED on after stabilization
    
    if (WATCHDOG_ENABLE_HARDWARE) {


        

        bool watchdog_enabled = false;
        for (int attempt = 0; attempt < 3; attempt++) {
            watchdog_enable(WATCHDOG_HARDWARE_TIMEOUT_MS, true);
            sleep_ms(100);  // Small delay to let it settle
            watchdog_enabled = true;  // Assume success since watchdog_enable doesn't return status
            break;
        }
        
        if (!watchdog_enabled) {
        }
    }
    
    g_watchdog_started = true;
    g_last_hardware_update_ms = get_time_ms();
    

    watchdog_core0_heartbeat();
    sleep_ms(100);
    

    for (int i = 0; i < 20; i++) {
        watchdog_core0_heartbeat();  // Send heartbeat during wait

        gpio_put(PIN_LED, (i % 3 < 1) ? 1 : 0);  // 1 on, 2 off pattern
        sleep_ms(100);
    }
    gpio_put(PIN_LED, 1);  // LED on after heartbeat establishment
    
}

void watchdog_stop(void) {
    if (!g_watchdog_started) {
        return;
    }
    
   
    
    g_watchdog_started = false;
    
    
}

void watchdog_core0_heartbeat(void) {
    if (!g_watchdog_initialized) {
        return;
    }
    
    uint32_t current_time = get_time_ms();
    g_core0_heartbeat_timestamp = current_time;
    g_watchdog_status.core0_heartbeat_count++;
    
}

void watchdog_core1_heartbeat(void) {
    if (!g_watchdog_initialized) {
        return;
    }
    
    uint32_t current_time = get_time_ms();
    g_core1_heartbeat_timestamp = current_time;
    g_watchdog_status.core1_heartbeat_count++;
    
}

void watchdog_task(void) {
    if (!g_watchdog_initialized || !g_watchdog_started) {
        return;
    }
    
    uint32_t current_time = get_time_ms();
    

    if (current_time - g_last_hardware_update_ms >= WATCHDOG_UPDATE_INTERVAL_MS) {
        update_hardware_watchdog();
    }
    

    check_inter_core_health();
}

watchdog_status_t watchdog_get_status(void) {
    return g_watchdog_status;
}

bool watchdog_is_system_healthy(void) {
    return g_watchdog_initialized && g_watchdog_status.system_healthy;
}

void watchdog_force_reset(void) {
    

    if (WATCHDOG_ENABLE_HARDWARE) {

        while (true) {
            tight_loop_contents();
        }
    } else {

        watchdog_enable(1, true);  // 1ms timeout
        while (true) {
            tight_loop_contents();
        }
    }
}

void watchdog_set_debug(bool enable) {
    g_debug_enabled = enable;
}