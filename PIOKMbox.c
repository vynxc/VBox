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





typedef struct {
    uint32_t watchdog_status_timer;
    uint32_t last_button_press_time;
    bool button_pressed_last;
    bool usb_reset_cooldown;
    uint32_t usb_reset_cooldown_start;
} main_loop_state_t;











static const uint32_t WATCHDOG_STATUS_INTERVAL_MS = WATCHDOG_STATUS_REPORT_INTERVAL_MS;





#if PIO_USB_AVAILABLE
static void core1_main(void);
static void core1_task_loop(void);
#endif

static bool initialize_system(void);
static bool initialize_usb_device(void);
static void main_application_loop(void);


static void process_button_input(system_state_t* state, uint32_t current_time);


static void report_watchdog_status(uint32_t current_time, uint32_t* watchdog_status_timer);


static inline bool is_time_elapsed(uint32_t current_time, uint32_t last_time, uint32_t interval);





#if PIO_USB_AVAILABLE


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

    sleep_ms(10);
    

    pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
    pio_cfg.pin_dp = PIN_USB_HOST_DP;
    pio_cfg.pinout = PIO_USB_PINOUT_DPDM;
    

    tuh_configure(USB_HOST_PORT, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
    

    tuh_init(USB_HOST_PORT);
    

    usb_host_mark_initialized();
    

    core1_task_loop();
}

static void core1_task_loop(void) {

    uint32_t heartbeat_counter = 0;
    uint32_t last_heartbeat_ms = 0;
    

    const uint32_t heartbeat_check_threshold = CORE1_HEARTBEAT_CHECK_LOOPS * 4; // 4x less frequent
    
    while (true) {
        tuh_task();
        

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




static bool initialize_system(void) {

    stdio_init_all();
    

    sleep_ms(200);
    
    if (!set_sys_clock_khz(CPU_FREQ, true)) {
        return false;
    }
    

    sleep_ms(100);  // Allow clock to stabilize
    stdio_init_all();
    sleep_ms(100);  // Allow UART to stabilize
    


    uart_init(uart0, STDIO_UART_BAUDRATE);
    uart_set_format(uart0, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(uart0, true);  // Enable FIFO for better performance
    

    kmbox_serial_init();
    

    neopixel_init();


    usb_hid_init();


    watchdog_init();

    return true;
}

static bool initialize_usb_device(void) {
    
    const bool device_init_success = tud_init(USB_DEVICE_PORT);
    
    if (device_init_success) {
        usb_device_mark_initialized();
    }
    
    return device_init_success;
}







static void process_button_input(system_state_t* state, uint32_t current_time) {

    const bool button_currently_pressed = !gpio_get(PIN_BUTTON); // Button is active low


    if (state->usb_reset_cooldown) {
        if (is_time_elapsed(current_time, state->usb_reset_cooldown_start, USB_RESET_COOLDOWN_MS)) {
            state->usb_reset_cooldown = false;
        }
        state->button_pressed_last = button_currently_pressed;
        return; // Skip button processing during cooldown
    }


    if (button_currently_pressed) {
        if (!state->button_pressed_last) {

            state->last_button_press_time = current_time;
        } else {

            if (is_time_elapsed(current_time, state->last_button_press_time, BUTTON_HOLD_TRIGGER_MS)) {
                usb_stacks_reset();
                state->usb_reset_cooldown = true;
                state->usb_reset_cooldown_start = current_time;
            }
        }
    } else if (state->button_pressed_last) {


    }

    state->button_pressed_last = button_currently_pressed;
}





static void report_watchdog_status(uint32_t current_time, uint32_t* watchdog_status_timer) {
    if (!is_time_elapsed(current_time, *watchdog_status_timer, WATCHDOG_STATUS_INTERVAL_MS)) {
        return;
    }

    *watchdog_status_timer = current_time;
}





static inline bool is_time_elapsed(uint32_t current_time, uint32_t last_time, uint32_t interval) {
    return (current_time - last_time) >= interval;
}






static void main_application_loop(void) {
    system_state_t* state = get_system_state();
    system_state_init(state);
    

    const uint32_t watchdog_interval = WATCHDOG_TASK_INTERVAL_MS;
    const uint32_t visual_interval = VISUAL_TASK_INTERVAL_MS;
    const uint32_t error_interval = ERROR_CHECK_INTERVAL_MS;
    const uint32_t button_interval = BUTTON_DEBOUNCE_MS;
    const uint32_t status_report_interval = WATCHDOG_STATUS_REPORT_INTERVAL_MS;


    uint32_t current_time = to_ms_since_boot(get_absolute_time());
    uint16_t loop_counter = 0;
    

    uint8_t task_flags = 0;
    #define WATCHDOG_FLAG   (1 << 0)
    #define VISUAL_FLAG     (1 << 1)
    #define BUTTON_FLAG     (1 << 2)
    #define STATUS_FLAG     (1 << 3)

    while (true) {

        tud_task();
        hid_device_task();
        

        kmbox_serial_task();
        

        if (++loop_counter >= 32) {  // Sample every 32 loops
            current_time = to_ms_since_boot(get_absolute_time());
            loop_counter = 0;
            

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
            

            if (task_flags && (current_time - state->last_error_check_time) >= error_interval) {
                state->last_error_check_time = current_time;
            }
        }
        

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





int main(void) {
    

    set_sys_clock_khz(CPU_FREQ, true);
    

    sleep_ms(10);
    

    #ifdef PIN_USB_5V
    gpio_init(PIN_USB_5V);
    gpio_set_dir(PIN_USB_5V, GPIO_OUT);
    gpio_put(PIN_USB_5V, 0);  // Keep USB power OFF initially
    #endif
    
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 1);  // Turn on LED
    
    

    if (!initialize_system()) {
        return -1;
    }
    
    usb_host_enable_power();
    sleep_ms(100);
    
    multicore_reset_core1();
    multicore_launch_core1(core1_main);
    
    if (!initialize_usb_device()) {
        return -1;
    }
    
    watchdog_init();
   
    neopixel_enable_power();    
    
    main_application_loop();
    
    return 0;
}
