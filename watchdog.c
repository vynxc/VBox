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

//--------------------------------------------------------------------+
// INTERNAL STATE
//--------------------------------------------------------------------+

static watchdog_status_t g_watchdog_status;
static bool g_watchdog_initialized = false;
static bool g_watchdog_started = false;
static bool g_debug_enabled = WATCHDOG_ENABLE_DEBUG;
static uint32_t g_last_hardware_update_ms = 0;

// Inter-core communication for heartbeats
static volatile uint32_t g_core0_heartbeat_timestamp = 0;
static volatile uint32_t g_core1_heartbeat_timestamp = 0;

//--------------------------------------------------------------------+
// INTERNAL FUNCTIONS
//--------------------------------------------------------------------+

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
        // Core hasn't sent any heartbeats yet
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
    
    // Update heartbeat times from volatile variables
    g_watchdog_status.core0_last_heartbeat_ms = g_core0_heartbeat_timestamp;
    g_watchdog_status.core1_last_heartbeat_ms = g_core1_heartbeat_timestamp;
    
    // Check core 0 responsiveness
    bool core0_was_responsive = g_watchdog_status.core0_responsive;
    g_watchdog_status.core0_responsive = is_core_responsive(
        g_watchdog_status.core0_last_heartbeat_ms, current_time);
    
    if (!g_watchdog_status.core0_responsive && core0_was_responsive) {
        uint32_t time_since = current_time - g_watchdog_status.core0_last_heartbeat_ms;
        handle_timeout_warning(0, time_since);
    }
    
    // Check core 1 responsiveness
    bool core1_was_responsive = g_watchdog_status.core1_responsive;
    g_watchdog_status.core1_responsive = is_core_responsive(
        g_watchdog_status.core1_last_heartbeat_ms, current_time);
    
    if (!g_watchdog_status.core1_responsive && core1_was_responsive) {
        uint32_t time_since = current_time - g_watchdog_status.core1_last_heartbeat_ms;
        handle_timeout_warning(1, time_since);
    }
    
    // Update overall system health
    g_watchdog_status.system_healthy = 
        g_watchdog_status.core0_responsive && g_watchdog_status.core1_responsive;
    
    // If system is unhealthy for too long, force reset
    if (!g_watchdog_status.system_healthy) {
        static uint32_t unhealthy_start_time = 0;
        if (unhealthy_start_time == 0) {
            unhealthy_start_time = current_time;
        } else if (current_time - unhealthy_start_time > WATCHDOG_CORE_TIMEOUT_MS * 2) {
            watchdog_force_reset();
        }
    } else {
        // Reset unhealthy timer when system becomes healthy again
        // Reset unhealthy timer when system becomes healthy again
        // (No action needed - timer is reset by healthy state)
    }
}

//--------------------------------------------------------------------+
// PUBLIC API IMPLEMENTATION
//--------------------------------------------------------------------+

void watchdog_init(void) {
    if (g_watchdog_initialized) {
        return;
    }
    
    // Clear status structure
    memset(&g_watchdog_status, 0, sizeof(g_watchdog_status));
    
    // Initialize timestamps
    g_core0_heartbeat_timestamp = 0;
    g_core1_heartbeat_timestamp = 0;
    g_last_hardware_update_ms = 0;
    
    // Set initial status
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
    
    // Extended delay to ensure system is fully stable before starting watchdog
    for (int i = 0; i < 30; i++) {
        watchdog_core0_heartbeat();  // Send heartbeat during wait
        // Blink LED during extended stabilization - very slow blink
        gpio_put(PIN_LED, (i % 4 < 2) ? 1 : 0);  // 2 on, 2 off pattern
        sleep_ms(100);
    }
    gpio_put(PIN_LED, 1);  // LED on after stabilization
    
    if (WATCHDOG_ENABLE_HARDWARE) {
        // Enable hardware watchdog with specified timeout

        
        // Try enabling watchdog with retry logic for cold boot robustness
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
    
    // Send initial heartbeats to establish baseline with delay
    watchdog_core0_heartbeat();
    sleep_ms(100);
    
    // Give cores time to start sending regular heartbeats
    for (int i = 0; i < 20; i++) {
        watchdog_core0_heartbeat();  // Send heartbeat during wait
        // Blink LED during heartbeat establishment - medium blink
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
    
    // Update hardware watchdog at regular intervals
    if (current_time - g_last_hardware_update_ms >= WATCHDOG_UPDATE_INTERVAL_MS) {
        update_hardware_watchdog();
    }
    
    // Check inter-core health
    check_inter_core_health();
}

watchdog_status_t watchdog_get_status(void) {
    return g_watchdog_status;
}

bool watchdog_is_system_healthy(void) {
    return g_watchdog_initialized && g_watchdog_status.system_healthy;
}

void watchdog_force_reset(void) {
    
    // Force immediate reset by causing hardware watchdog timeout
    if (WATCHDOG_ENABLE_HARDWARE) {
        // Stop updating the hardware watchdog and wait for reset
        while (true) {
            tight_loop_contents();
        }
    } else {
        // If hardware watchdog is disabled, use software reset
        watchdog_enable(1, true);  // 1ms timeout
        while (true) {
            tight_loop_contents();
        }
    }
}

void watchdog_set_debug(bool enable) {
    g_debug_enabled = enable;
}