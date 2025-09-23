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
#include "kmbox_serial_handler.h"

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
    uint32_t watchdog_status_timer;
    uint32_t last_button_press_time;
    bool button_pressed_last;
    bool usb_reset_cooldown;
    uint32_t usb_reset_cooldown_start;
} main_loop_state_t;

// Remove unused structures
//typedef struct {
//    bool button_pressed;
//    uint32_t current_time;
//    uint32_t hold_duration;
//} button_state_t;

//--------------------------------------------------------------------+
// Constants and Configuration
//--------------------------------------------------------------------+
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
static void process_button_input(system_state_t* state, uint32_t current_time);

// Reporting functions
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
    // Optimize heartbeat checking - use larger counter intervals
    uint32_t heartbeat_counter = 0;
    uint32_t last_heartbeat_ms = 0;
    
    // Performance optimization: reduce heartbeat frequency checks
    const uint32_t heartbeat_check_threshold = CORE1_HEARTBEAT_CHECK_LOOPS * 4; // 4x less frequent
    
    while (true) {
        tuh_task();
        
        // Heartbeat check optimization - much less frequent timing calls
        if (++heartbeat_counter >= heartbeat_check_threshold) {
            const uint32_t current_time = to_ms_since_boot(get_absolute_time());
            if ((current_time - last_heartbeat_ms) >= WATCHDOG_HEARTBEAT_INTERVAL_MS) {
                watchdog_core1_heartbeat();
                last_heartbeat_ms = current_time;
            }
            heartbeat_counter = 0;
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
    printf("Setting system clock to %d kHz...\n", CPU_FREQ);
    if (!set_sys_clock_khz(CPU_FREQ, true)) {
        printf("CRITICAL: Failed to set system clock to %d kHz\n", CPU_FREQ);
        return false;
    }
    
    // Re-initialize stdio after clock change with proper delay
    sleep_ms(100);  // Allow clock to stabilize
    stdio_init_all();
    sleep_ms(100);  // Allow UART to stabilize
    printf("System clock set successfully to %d kHz\n", CPU_FREQ);
    
    // Configure UART0 for debug output with non-blocking operation
    // Set a high baud rate to reduce printf backpressure and enable FIFO
    uart_init(uart0, STDIO_UART_BAUDRATE);
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(uart0, true);  // Enable FIFO for better performance
    
    // Initialize KMBox serial handler on UART1
    kmbox_serial_init();
    
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

static void process_button_input(system_state_t* state, uint32_t current_time) {
    // Performance optimization: single GPIO read per call
    const bool button_currently_pressed = !gpio_get(PIN_BUTTON); // Button is active low

    // Handle cooldown after USB reset - early exit for performance
    if (state->usb_reset_cooldown) {
        if (is_time_elapsed(current_time, state->usb_reset_cooldown_start, USB_RESET_COOLDOWN_MS)) {
            state->usb_reset_cooldown = false;
        }
        state->button_pressed_last = button_currently_pressed;
        return; // Skip button processing during cooldown
    }

    // Optimized state machine - avoid redundant checks
    if (button_currently_pressed) {
        if (!state->button_pressed_last) {
            // Button just pressed
            state->last_button_press_time = current_time;
        } else {
            // Button being held - check for reset trigger
            if (is_time_elapsed(current_time, state->last_button_press_time, BUTTON_HOLD_TRIGGER_MS)) {
                printf("Button held - triggering USB reset\n");
                usb_stacks_reset();
                state->usb_reset_cooldown = true;
                state->usb_reset_cooldown_start = current_time;
            }
        }
    } else if (state->button_pressed_last) {
        // Button just released - could add short press handling here if needed
        // For now, no action on short press
    }

    state->button_pressed_last = button_currently_pressed;
}

//--------------------------------------------------------------------+
// Reporting Functions
//--------------------------------------------------------------------+

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
    
    // Cache frequently used intervals
    const uint32_t watchdog_interval = WATCHDOG_TASK_INTERVAL_MS;
    const uint32_t visual_interval = VISUAL_TASK_INTERVAL_MS;
    const uint32_t error_interval = ERROR_CHECK_INTERVAL_MS;
    const uint32_t button_interval = BUTTON_DEBOUNCE_MS;
    const uint32_t status_report_interval = WATCHDOG_STATUS_REPORT_INTERVAL_MS;

    // Performance optimization: reduce time sampling frequency
    uint32_t current_time = to_ms_since_boot(get_absolute_time());
    uint16_t loop_counter = 0;
    
    // Batch time checks with bit flags for efficiency
    uint8_t task_flags = 0;
    #define WATCHDOG_FLAG   (1 << 0)
    #define VISUAL_FLAG     (1 << 1)
    #define BUTTON_FLAG     (1 << 2)
    #define STATUS_FLAG     (1 << 3)

    while (true) {
        // TinyUSB device task - highest priority
        tud_task();
        hid_device_task();
        
        // KMBox serial task - high priority for responsiveness
        kmbox_serial_task();
        
        // Sample time less frequently to reduce overhead
        if (++loop_counter >= 32) {  // Sample every 32 loops
            current_time = to_ms_since_boot(get_absolute_time());
            loop_counter = 0;
            
            // Batch all time checks into flags for efficiency
            task_flags = 0;
            if ((current_time - state->last_watchdog_time) >= watchdog_interval) {
                task_flags |= WATCHDOG_FLAG;
            }
            if ((current_time - state->last_visual_time) >= visual_interval) {
                task_flags |= VISUAL_FLAG;
            }
            if ((current_time - state->last_button_time) >= button_interval) {
                task_flags |= BUTTON_FLAG;
            }
            if ((current_time - state->watchdog_status_timer) >= status_report_interval) {
                task_flags |= STATUS_FLAG;
            }
            
            // Error check optimization - only when other tasks run
            if (task_flags && (current_time - state->last_error_check_time) >= error_interval) {
                state->last_error_check_time = current_time;
            }
        }
        
        // Execute tasks based on flags (avoids repeated time checks)
        if (task_flags & WATCHDOG_FLAG) {
            watchdog_task();
            watchdog_core0_heartbeat();
            state->last_watchdog_time = current_time;
        }
        
        if (task_flags & VISUAL_FLAG) {
            led_blinking_task();
            neopixel_status_task();
            state->last_visual_time = current_time;
        }
        
        if (task_flags & BUTTON_FLAG) {
            process_button_input(state, current_time);
            state->last_button_time = current_time;
        }
        
        if (task_flags & STATUS_FLAG) {
            report_watchdog_status(current_time, &state->watchdog_status_timer);
        }
    }
}

//--------------------------------------------------------------------+
// Main Function
//--------------------------------------------------------------------+

int main(void) {
    
    // Set system clock to 120MHz or 240MHz (required for PIO USB, speed depends on device type)
    set_sys_clock_khz(CPU_FREQ, true);
    
    // Small delay for clock stabilization
    sleep_ms(10);
    
    // Initialize basic GPIO
    #ifdef PIN_USB_5V
    gpio_init(PIN_USB_5V);
    gpio_set_dir(PIN_USB_5V, GPIO_OUT);
    gpio_put(PIN_USB_5V, 0);  // Keep USB power OFF initially
    #endif
    
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
    
    multicore_reset_core1();
    multicore_launch_core1(core1_main);
    
    if (!initialize_usb_device()) {
        printf("CRITICAL: USB Device initialization failed\n");
        return -1;
    }
    
    watchdog_init();
    // NOTE: For debugging startup hangs we skip the extended, blocking
    // watchdog_start() sequence which performs long sleeps/prints and
    // enables the hardware watchdog. If you need the full watchdog
    // behavior re-enable the call below.
    // watchdog_start();
    neopixel_enable_power();    
    printf("=== PIOKMBox Ready ===\n");
    
    // Enter main application loop
    main_application_loop();
    
    return 0;
}
