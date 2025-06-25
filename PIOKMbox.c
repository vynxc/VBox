/*
 * Hurricane PIOKMBox Firmware
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/uart.h"
#include "pico/unique_id.h"

#include "tusb.h"
#include "defines.h"
#include "config.h"
#include "timing_config.h"
#include "led_control.h"
#include "usb_hid.h"
#include "watchdog.h"
#include "init_state_machine.h"
#include "state_management.h"

#if PIO_USB_AVAILABLE
#include "pio_usb.h"
#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "tusb.h"
#endif

//--------------------------------------------------------------------+
// Type Definitions and Structures
//--------------------------------------------------------------------+

typedef struct {
    uint32_t stats_timer;
    uint32_t watchdog_status_timer;
    uint32_t last_button_press_time;
    bool button_pressed_last;
    bool usb_reset_cooldown;
    uint32_t usb_reset_cooldown_start;
} main_loop_state_t;

typedef struct {
    bool button_pressed;
    uint32_t current_time;
    uint32_t hold_duration;
} button_state_t;

//--------------------------------------------------------------------+
// Constants and Configuration
//--------------------------------------------------------------------+
static const uint32_t STATS_INTERVAL_MS = STATS_REPORT_INTERVAL_MS;
static const uint32_t WATCHDOG_STATUS_INTERVAL_MS = WATCHDOG_STATUS_REPORT_INTERVAL_MS;

//--------------------------------------------------------------------+
// Function Prototypes
//--------------------------------------------------------------------+

#if PIO_USB_AVAILABLE
static void core1_main(void);
static void core1_task_loop(void);
#endif

static bool initialize_system(void);
static bool initialize_usb_device(void);
static void main_application_loop(void);

// Button handling functions
static button_state_t get_button_state(uint32_t current_time);
static void handle_button_press_start(system_state_t* state, uint32_t current_time);
static void handle_button_held(system_state_t* state, uint32_t current_time);
static void handle_button_release(const system_state_t* state, uint32_t hold_duration);
static void process_button_input(system_state_t* state, uint32_t current_time);

// Reporting functions
static void report_hid_statistics(uint32_t current_time, uint32_t* stats_timer);
static void report_watchdog_status(uint32_t current_time, uint32_t* watchdog_status_timer);

// Utility functions
static inline bool is_time_elapsed(uint32_t current_time, uint32_t last_time, uint32_t interval);

//--------------------------------------------------------------------+
// Core1 Main (USB Host Task)
//--------------------------------------------------------------------+

#if PIO_USB_AVAILABLE
// Separate initialization concerns into focused functions

typedef enum {
    INIT_SUCCESS,
    INIT_FAILURE,
    INIT_RETRY_NEEDED
} init_result_t;

typedef struct {
    int attempt;
    int max_attempts;
    uint32_t base_delay_ms;
    uint32_t last_heartbeat_time;
} init_context_t;

typedef struct {
    uint32_t last_heartbeat_ms;
    uint32_t heartbeat_counter;
} core1_state_t;


static void core1_main(void) {
    // Small delay to let core0 stabilize
    sleep_ms(10);
    
    // CRITICAL: Configure PIO USB BEFORE tuh_init() - this is the key!
    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = PIN_USB_HOST_DP;
    pio_cfg.pinout = PIO_USB_PINOUT_DPDM;
    
    // Configure host stack with PIO USB configuration
    tuh_configure(USB_HOST_PORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    
    // Initialize host stack on core1
    tuh_init(USB_HOST_PORT);
    
    // Mark host as initialized
    usb_host_mark_initialized();
    
    // Start the main host task loop
    core1_task_loop();
}

static void core1_task_loop(void) {
    core1_state_t state = {0};  // Local state instead of static

    while (true) {
        tuh_task();
        
        // Check heartbeat timing less frequently
        if (++state.heartbeat_counter >= CORE1_HEARTBEAT_CHECK_LOOPS) {
            const uint32_t current_time = to_ms_since_boot(get_absolute_time());
            if (system_state_should_run_task(NULL, current_time,
                                            state.last_heartbeat_ms,
                                            WATCHDOG_HEARTBEAT_INTERVAL_MS)) {
                watchdog_core1_heartbeat();
                state.last_heartbeat_ms = current_time;
            }
            state.heartbeat_counter = 0;
        }
    }
}

#endif // PIO_USB_AVAILABLE
//--------------------------------------------------------------------+
// System Initialization Functions
//--------------------------------------------------------------------+

static bool initialize_system(void) {
    // Initialize stdio first for early debug output
    stdio_init_all();
    
    // Add extended startup delay for cold boot stability
    sleep_ms(200);
    
    printf("PICO PIO KMBox - Starting initialization...\n");
    printf("Neopixel pins initialized (power OFF for cold boot stability)\n");
    
#if PIO_USB_AVAILABLE
    // Set system clock to 120MHz (required for PIO USB - must be multiple of 12MHz)
    printf("Setting system clock to %d kHz...\n", PIO_USB_SYSTEM_CLOCK_KHZ);
    if (!set_sys_clock_khz(PIO_USB_SYSTEM_CLOCK_KHZ, true)) {
        printf("CRITICAL: Failed to set system clock to %d kHz\n", PIO_USB_SYSTEM_CLOCK_KHZ);
        return false;
    }
    
    // Re-initialize stdio after clock change with proper delay
    sleep_ms(100);  // Allow clock to stabilize
    stdio_init_all();
    sleep_ms(100);  // Allow UART to stabilize
    printf("System clock set successfully to %d kHz\n", PIO_USB_SYSTEM_CLOCK_KHZ);
#endif
    
    // Configure UART for non-blocking operation
    uart_set_fifo_enabled(uart0, true);  // Enable FIFO for better performance
    
    // Initialize LED control module (neopixel power OFF for now)
    neopixel_init();

    // Initialize USB HID module (USB host power OFF for now)
    usb_hid_init();

    // Initialize watchdog system (but don't start it yet)
    watchdog_init();

    printf("System initialization complete\n");
    return true;
}

static bool initialize_usb_device(void) {
    // Simple device initialization - let the working example pattern guide us
    printf("USB Device: Initializing on controller %d (native USB)...\n", USB_DEVICE_PORT);
    
    const bool device_init_success = tud_init(USB_DEVICE_PORT);
    printf("USB Device init: %s\n", device_init_success ? "SUCCESS" : "FAILED");
    
    if (device_init_success) {
        usb_device_mark_initialized();
        printf("USB Device: Initialization complete\n");
    }
    
    return device_init_success;
}



//--------------------------------------------------------------------+
// Button Handling Functions
//--------------------------------------------------------------------+

static button_state_t get_button_state(uint32_t current_time) {
    button_state_t state = {
        .button_pressed = !gpio_get(PIN_BUTTON), // Button is active low
        .current_time = current_time,
        .hold_duration = 0
    };
    return state;
}

static void handle_button_press_start(system_state_t* state, uint32_t current_time) {
    state->last_button_press_time = current_time;
    // Button pressed (reduced logging)
}

static void handle_button_held(system_state_t* state, uint32_t current_time) {
    if (is_time_elapsed(current_time, state->last_button_press_time, BUTTON_HOLD_TRIGGER_MS)) {
        printf("Button held - triggering USB reset\n");
        usb_stacks_reset();
        state->usb_reset_cooldown = true;
        state->usb_reset_cooldown_start = current_time;
    }
}

static void handle_button_release(const system_state_t* state, uint32_t hold_duration) {
    (void)state; // Suppress unused parameter warning
    if (hold_duration < BUTTON_HOLD_TRIGGER_MS) {
        // Button released (reduced logging)
    }
}

static void process_button_input(system_state_t* state, uint32_t current_time) {
    const button_state_t button = get_button_state(current_time);

    // Handle cooldown after USB reset
    if (state->usb_reset_cooldown) {
        if (is_time_elapsed(current_time, state->usb_reset_cooldown_start, USB_RESET_COOLDOWN_MS)) {
            state->usb_reset_cooldown = false;
        }
        state->button_pressed_last = button.button_pressed;
        return; // Skip button processing during cooldown
    }

    if (button.button_pressed && !state->button_pressed_last) {
        // Button just pressed
        handle_button_press_start(state, current_time);
    } else if (button.button_pressed && state->button_pressed_last) {
        // Button being held
        handle_button_held(state, current_time);
    } else if (!button.button_pressed && state->button_pressed_last) {
        // Button just released
        const uint32_t hold_duration = current_time - state->last_button_press_time;
        handle_button_release(state, hold_duration);
    }

    state->button_pressed_last = button.button_pressed;
}

//--------------------------------------------------------------------+
// Reporting Functions
//--------------------------------------------------------------------+

static void report_hid_statistics(uint32_t current_time, uint32_t* stats_timer) {
    if (!is_time_elapsed(current_time, *stats_timer, STATS_INTERVAL_MS)) {
        return;
    }

    *stats_timer = current_time;
    
    hid_stats_t stats;
    get_hid_stats(&stats);
    
    printf("=== HID Statistics ===\n");
    printf("Mouse: RX=%lu, TX=%lu\n", stats.mouse_reports_received, stats.mouse_reports_forwarded);
    printf("Keyboard: RX=%lu, TX=%lu\n", stats.keyboard_reports_received, stats.keyboard_reports_forwarded);
    printf("Errors: %lu\n", stats.forwarding_errors);
    printf("Mouse connected: %s\n", is_mouse_connected() ? "YES" : "NO");
    printf("Keyboard connected: %s\n", is_keyboard_connected() ? "YES" : "NO");
    printf("=====================\n");
}

static void report_watchdog_status(uint32_t current_time, uint32_t* watchdog_status_timer) {
    if (!is_time_elapsed(current_time, *watchdog_status_timer, WATCHDOG_STATUS_INTERVAL_MS)) {
        return;
    }

    *watchdog_status_timer = current_time;
    
    const watchdog_status_t watchdog_status = watchdog_get_status();
    
    printf("=== Watchdog Status ===\n");
    printf("System healthy: %s\n", watchdog_status.system_healthy ? "YES" : "NO");
    printf("Core 0: %s (heartbeats: %lu)\n",
           watchdog_status.core0_responsive ? "RESPONSIVE" : "UNRESPONSIVE",
           watchdog_status.core0_heartbeat_count);
    printf("Core 1: %s (heartbeats: %lu)\n",
           watchdog_status.core1_responsive ? "RESPONSIVE" : "UNRESPONSIVE",
           watchdog_status.core1_heartbeat_count);
    printf("Hardware updates: %lu\n", watchdog_status.hardware_updates);
    printf("Timeout warnings: %lu\n", watchdog_status.timeout_warnings);
    printf("=======================\n");
}

//--------------------------------------------------------------------+
// Utility Functions
//--------------------------------------------------------------------+

static inline bool is_time_elapsed(uint32_t current_time, uint32_t last_time, uint32_t interval) {
    return (current_time - last_time) >= interval;
}

//--------------------------------------------------------------------+
// Main Application Loop
//--------------------------------------------------------------------+


static void main_application_loop(void) {
    system_state_t* state = get_system_state();
    system_state_init(state);

    while (true) {
        // TinyUSB device task - highest priority
        tud_task();
        hid_device_task();
        
        const uint32_t current_time = to_ms_since_boot(get_absolute_time());
        
        // Watchdog tasks - controlled frequency
        if (system_state_should_run_task(state, current_time, 
                                        state->last_watchdog_time, 
                                        WATCHDOG_TASK_INTERVAL_MS)) {
            watchdog_task();
            watchdog_core0_heartbeat();
            state->last_watchdog_time = current_time;
        }
        
        // LED and visual tasks
        if (system_state_should_run_task(state, current_time,
                                        state->last_visual_time,
                                        VISUAL_TASK_INTERVAL_MS)) {
            led_blinking_task();
            neopixel_status_task();
            state->last_visual_time = current_time;
        }
        
        // USB stack error monitoring - DISABLED to prevent endpoint conflicts
        // The error checking was causing automatic resets that conflict with dual-mode operation
        if (system_state_should_run_task(state, current_time,
                                        state->last_error_check_time,
                                        ERROR_CHECK_INTERVAL_MS)) {
            // usb_stack_error_check(); // Disabled to prevent endpoint conflicts
            state->last_error_check_time = current_time;
        }
        
        // Button input processing
        if (system_state_should_run_task(state, current_time,
                                        state->last_button_time,
                                        BUTTON_DEBOUNCE_MS)) {
            process_button_input(state, current_time);
            state->last_button_time = current_time;
        }
        
        // Periodic reporting
        report_hid_statistics(current_time, &state->stats_timer);
        report_watchdog_status(current_time, &state->watchdog_status_timer);
    }
}

//--------------------------------------------------------------------+
// Main Function
//--------------------------------------------------------------------+

int main(void) {
    // Follow the working example pattern closely
    
    // Set system clock to 120MHz (required for PIO USB)
    set_sys_clock_khz(120000, true);
    
    // Small delay for clock stabilization
    sleep_ms(10);
    
    // Initialize basic GPIO
    gpio_init(PIN_USB_5V);
    gpio_set_dir(PIN_USB_5V, GPIO_OUT);
    gpio_put(PIN_USB_5V, 0);  // Keep USB power OFF initially
    
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 1);  // Turn on LED
    
    printf("=== PIOKMBox Starting ===\n");
    
    // Initialize system components
    if (!initialize_system()) {
        printf("CRITICAL: System initialization failed\n");
        return -1;
    }
    
    // Enable USB host power
    usb_host_enable_power();
    sleep_ms(100);  // Brief power stabilization
    
    // Launch core1 first (like the working example)
    printf("Launching core1 for USB host...\n");
    multicore_reset_core1();
    multicore_launch_core1(core1_main);
    
    // Initialize device stack on core0 (like the working example)
    printf("Initializing USB device stack on core0...\n");
    if (!initialize_usb_device()) {
        printf("CRITICAL: USB Device initialization failed\n");
        return -1;
    }
    
    // Initialize remaining systems
    watchdog_init();
    watchdog_start();
    neopixel_enable_power();
    
    printf("=== PIOKMBox Ready ===\n");
    
    // Enter main application loop
    main_application_loop();
    
    return 0;
}
