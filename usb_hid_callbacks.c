/*
 * Hurricane PIOKMbox Firmware
 */

#include "usb_hid_callbacks.h"
#include "usb_hid_types.h"
#include "usb_hid_reports.h"
#include "usb_hid_descriptors.h"
#include "usb_hid_strings.h"
#include "usb_hid_host.h"
#include "defines.h"
#include "led_control.h"
#include <stdio.h>

// External declarations for variables defined in other modules
extern device_connection_state_t connection_state;
extern usb_error_tracker_t usb_error_tracker;

// Device callbacks with improved error handling
void tud_mount_cb(void)
{
    led_set_blink_interval(LED_BLINK_MOUNTED_MS);
    // USB Device mounted (reduced logging)
    neopixel_update_status();
}

void tud_umount_cb(void)
{
    led_set_blink_interval(LED_BLINK_UNMOUNTED_MS);
    // USB Device unmounted (reduced logging)
    
    // Track device unmount as potential error condition
    usb_error_tracker.consecutive_device_errors++;
    // Device unmount error tracking (reduced logging)
    
    neopixel_update_status();
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void) remote_wakeup_en;
    led_set_blink_interval(LED_BLINK_SUSPENDED_MS);
    // USB Device suspended (reduced logging)
    neopixel_update_status();
}

void tud_resume_cb(void)
{
    led_set_blink_interval(LED_BLINK_RESUMED_MS);
    // USB Device resumed (reduced logging)
    neopixel_update_status();
}

// HID device callbacks
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t* buffer, uint16_t reqlen)
{
    // Not used for this application
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) buffer;
    (void) reqlen;
    
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const* buffer, uint16_t bufsize)
{
    (void) instance;
    (void) report_id;
    (void) report_type;
    (void) bufsize;
    
    // This is where we would handle incoming LED state reports (caps lock, etc.)
    if (report_type == HID_REPORT_TYPE_OUTPUT && bufsize == 1) {
        // The LED state is in the first byte
        uint8_t led_state = buffer[0];
        
        // Check if caps lock is on
        bool caps_lock = (led_state & KEYBOARD_LED_CAPSLOCK) != 0;
        
        // Update the caps lock state
        // This would update a global variable that's defined elsewhere
        // caps_lock_state = caps_lock;
        
        // Visual feedback for caps lock state
        if (caps_lock) {
            neopixel_trigger_caps_lock_flash();
        } else {
            // No specific function for caps lock off, just update status
            neopixel_update_status();
        }
    }
}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const* report, uint16_t len)
{
    (void) instance;
    (void) report;
    (void) len;
    
    // Not used for this application
}

// Host callbacks with improved error handling
void tuh_mount_cb(uint8_t dev_addr)
{
    printf("USB Host: Device mounted at address %d\n", dev_addr);
    
    // Get VID/PID for basic device identification
    uint16_t vid, pid;
    if (tuh_vid_pid_get(dev_addr, &vid, &pid)) {
        printf("USB Host: Device %d - VID:0x%04X PID:0x%04X\n", dev_addr, vid, pid);
        
        // Update device descriptors with this VID/PID
        update_device_descriptors(vid, pid);
    } else {
        printf("USB Host: Device %d - Basic HID device (VID/PID not available)\n", dev_addr);
    }
    
    neopixel_trigger_usb_connection_flash();
    neopixel_update_status();
}

void tuh_umount_cb(uint8_t dev_addr)
{
    printf("USB Host: Device unmounted at address %d\n", dev_addr);

    // Handle device disconnection - this will reset VID/PID if needed
    handle_device_disconnection(dev_addr);
    
    // Track host unmount with improved logic
    static uint32_t last_unmount_time = 0;
    uint32_t current_time = to_ms_since_boot(get_absolute_time());
    
    if (current_time - last_unmount_time < 5000) { // Less than 5 seconds since last unmount
        usb_error_tracker.consecutive_host_errors++;
    } else {
        usb_error_tracker.consecutive_host_errors = 0;
    }
    last_unmount_time = current_time;
    
    // Trigger visual feedback
    neopixel_trigger_usb_disconnection_flash();
    neopixel_update_status();
}

// HID host callbacks with improved validation
void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, const uint8_t* desc_report, uint16_t desc_len)
{
    (void)desc_report; // Suppress unused parameter warning
    (void)desc_len;    // Suppress unused parameter warning
    
    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);
    
    printf("USB Host: HID device mounted - addr=%d, instance=%d, protocol=%d\n",
           dev_addr, instance, itf_protocol);
    
    if (itf_protocol == HID_ITF_PROTOCOL_MOUSE) {
        printf("USB Host: Mouse detected!\n");
    } else if (itf_protocol == HID_ITF_PROTOCOL_KEYBOARD) {
        printf("USB Host: Keyboard detected!\n");
    } else {
        printf("USB Host: Unknown HID protocol: %d\n", itf_protocol);
    }

    // Handle HID device connection
    handle_hid_device_connection(dev_addr, itf_protocol);

    // Start receiving reports
    if (!tuh_hid_receive_report(dev_addr, instance)) {
        printf("USB Host: Failed to start receiving reports for device %d\n", dev_addr);
    } else {
        printf("USB Host: Started receiving reports for device %d\n", dev_addr);
    }
    neopixel_update_status();
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    (void)instance; // Suppress unused parameter warning
    
    // HID device unmounted (reduced logging)
    
    // Handle device disconnection
    handle_device_disconnection(dev_addr);
    
    // Trigger visual feedback
    neopixel_trigger_usb_disconnection_flash();
    neopixel_update_status();
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, const uint8_t* report, uint16_t len)
{
    // Fast path: minimal validation for performance
    if (report == NULL || len == 0) {
        tuh_hid_receive_report(dev_addr, instance);
        return;
    }

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);

    // Direct processing without extra copying for better performance
    switch (itf_protocol) {
        case HID_ITF_PROTOCOL_KEYBOARD:
            if (len >= 1) {
                // Direct cast for performance - avoid memcpy overhead
                const hid_keyboard_report_t* kbd_report = (const hid_keyboard_report_t*)report;
                process_kbd_report(kbd_report);
            }
            break;

        case HID_ITF_PROTOCOL_MOUSE:
            if (len >= 3) { // Minimum mouse report: buttons + x + y
                // Direct cast for performance - avoid memcpy overhead
                const hid_mouse_report_t* mouse_report = (const hid_mouse_report_t*)report;
                process_mouse_report(mouse_report);
            }
            break;

        default:
            // Skip debug logging for unknown protocols to reduce overhead
            break;
    }

    // Continue to request reports
    tuh_hid_receive_report(dev_addr, instance);
}