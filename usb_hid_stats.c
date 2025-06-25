/*
 * Hurricane PIOKMbox Firmware
 */

#include "usb_hid_stats.h"
#include "defines.h"
#include <stdio.h>
#include <string.h>

// External declarations for variables defined in other modules
extern device_connection_state_t connection_state;
extern performance_stats_t stats;
extern tusb_desc_device_t desc_device;

// Performance optimization: reduce debug output
static uint32_t debug_counter = 0;

// Print HID device connection status with VID/PID information
void print_hid_connection_status(void)
{
    if (connection_state.mouse_connected) {
        printf("Mouse: Connected (VID:0x%04X PID:0x%04X)\n",
               connection_state.mouse_vid, connection_state.mouse_pid);
    } else {
        printf("Mouse: Not connected\n");
    }
    
    if (connection_state.keyboard_connected) {
        printf("Keyboard: Connected (VID:0x%04X PID:0x%04X)\n",
               connection_state.keyboard_vid, connection_state.keyboard_pid);
    } else {
        printf("Keyboard: Not connected\n");
    }
    
    // Show current device descriptor VID/PID
    printf("Current Device Descriptor: VID:0x%04X PID:0x%04X\n",
           desc_device.idVendor, desc_device.idProduct);
    
    // Show current string descriptors
    printf("Current String Descriptors:\n");
    printf("  Manufacturer: %s\n", connection_state.manufacturer);
    printf("  Product: %s\n", connection_state.product);
    printf("  Serial: %s\n", connection_state.serial);
}

void get_hid_stats(hid_stats_t* stats_out)
{
    if (stats_out == NULL) {
        return;
    }
    
    // Thread-safe copy of statistics
    stats_out->mouse_reports_received = stats.mouse_reports_received;
    stats_out->mouse_reports_forwarded = stats.mouse_reports_forwarded;
    stats_out->keyboard_reports_received = stats.keyboard_reports_received;
    stats_out->keyboard_reports_forwarded = stats.keyboard_reports_forwarded;
    stats_out->forwarding_errors = stats.forwarding_errors;
    
    // Print connection status with VID/PID information
    print_hid_connection_status();
}

void reset_hid_stats(void)
{
    memset(&stats, 0, sizeof(stats));
    debug_counter = 0;
}