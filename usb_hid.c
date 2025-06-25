/*
 * Hurricane PIOKMbox Firmware
 */

#include "usb_hid.h"
#include "defines.h"
#include "led_control.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

//--------------------------------------------------------------------+
// GLOBAL VARIABLES
//--------------------------------------------------------------------+

// Device connection state
device_connection_state_t connection_state = {0};

// Serial string buffer (16 hex chars + null terminator)
char serial_string[SERIAL_STRING_BUFFER_SIZE] = {0};

// Statistics and error tracking
performance_stats_t stats = {0};
usb_error_tracker_t usb_error_tracker = {0};

// Main initialization function that calls all component initializations
bool usb_hid_init_all(void)
{
    // Initialize the USB HID module
    if (!usb_hid_init()) {
        LOG_ERROR("Failed to initialize USB HID module");
        return false;
    }
    
    // Additional initialization can be added here if needed
    
    return true;
}
