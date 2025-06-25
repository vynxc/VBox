/*
 * Hurricane PIOKMbox Firmware
 */

#include "usb_hid_init.h"
#include "defines.h"
#include "led_control.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/gpio.h"
#include <stdio.h>
#include <string.h>

// Function declarations
static bool generate_serial_string(void);
static bool init_gpio_pins(void);

// External declarations for variables defined in other modules
extern device_connection_state_t connection_state;
extern char serial_string[SERIAL_STRING_BUFFER_SIZE];

// USB stack initialization tracking
static bool usb_device_initialized = false;
static bool usb_host_initialized = false;

bool usb_hid_init(void)
{
    // Generate unique serial string from chip ID
    if (!generate_serial_string()) {
        LOG_ERROR("Failed to generate serial string");
        return false;
    }
    
    // Initialize GPIO pins
    if (!init_gpio_pins()) {
        LOG_ERROR("Failed to initialize GPIO pins");
        return false;
    }
    
    // Initialize connection state
    memset(&connection_state, 0, sizeof(connection_state));
    
    // Explicitly initialize VID/PID fields to zero
    connection_state.mouse_vid = 0;
    connection_state.mouse_pid = 0;
    connection_state.keyboard_vid = 0;
    connection_state.keyboard_pid = 0;
    connection_state.descriptors_updated = false;
    connection_state.strings_updated = false;
    
    // Initialize string descriptor buffers with default values
    strncpy(connection_state.manufacturer, MANUFACTURER_STRING, USB_STRING_BUFFER_SIZE - 1);
    strncpy(connection_state.product, PRODUCT_STRING, USB_STRING_BUFFER_SIZE - 1);
    strncpy(connection_state.serial, serial_string, USB_STRING_BUFFER_SIZE - 1);
    
    printf("USB HID module initialized successfully\n");
    return true;
}

static bool generate_serial_string(void)
{
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    
    // Board ID is always valid since it's an array, not a pointer
    // No validation needed for board_id.id
    
    // Convert 8 bytes of unique ID to 16 character hex string
    for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++) {
        int result = snprintf(&serial_string[i * SERIAL_HEX_CHARS_PER_BYTE], 
                             SERIAL_SNPRINTF_BUFFER_SIZE, 
                             "%02X", 
                             board_id.id[i]);
        if (result < 0 || result >= SERIAL_SNPRINTF_BUFFER_SIZE) {
            return false;
        }
    }
    
    serial_string[SERIAL_STRING_LENGTH] = '\0'; // Ensure null termination
    return true;
}

static bool init_gpio_pins(void)
{
    // Initialize button pin
    gpio_init(PIN_BUTTON);
    
    gpio_set_dir(PIN_BUTTON, GPIO_IN);
    gpio_pull_up(PIN_BUTTON);
    
    // USB 5V power pin initialization but keep it OFF during early boot
    gpio_init(PIN_USB_5V);
    gpio_set_dir(PIN_USB_5V, GPIO_OUT);
    gpio_put(PIN_USB_5V, 0); // Keep USB power OFF initially for cold boot stability
    
    return true;
}

bool usb_host_enable_power(void)
{
    LOG_INIT("Enabling USB host power...");
    gpio_put(PIN_USB_5V, 1); // Enable USB power
    sleep_ms(100); // Allow power to stabilize
    LOG_INIT("USB host power enabled on pin %d", PIN_USB_5V);
    return true;
}

void usb_device_mark_initialized(void)
{
    usb_device_initialized = true;
}

void usb_host_mark_initialized(void)
{
    usb_host_initialized = true;
}

// USB stack reset functions - these would be implemented based on the original code
bool usb_device_stack_reset(void)
{
    // Implementation would go here
    return true;
}

bool usb_host_stack_reset(void)
{
    // Implementation would go here
    return true;
}

bool usb_stacks_reset(void)
{
    // Implementation would go here
    return true;
}

void usb_stack_error_check(void)
{
    // Implementation would go here
}