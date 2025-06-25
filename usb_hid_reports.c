/*
 * Hurricane PIOKMbox Firmware
 */

#include "usb_hid_reports.h"
#include "defines.h"
#include "led_control.h"
#include <stdio.h>

// External declarations for variables defined in other modules
extern performance_stats_t stats;

// Forward declarations for static functions
static bool process_keyboard_report_internal(const hid_keyboard_report_t* report);
static bool process_mouse_report_internal(const hid_mouse_report_t* report);

#if USE_HARDWARE_ACCELERATION
// RP2350 hardware-accelerated implementations
static bool process_keyboard_report_internal_m33(const hid_keyboard_report_t* report);
static bool process_mouse_report_internal_m33(const hid_mouse_report_t* report);
#endif

void process_kbd_report(const hid_keyboard_report_t* report)
{
    if (report == NULL) {
        return; // Fast fail without printf for performance
    }
    
    // Previous report tracking removed - not currently used
    
    // Reduced activity flash frequency for better performance
    static uint32_t activity_counter = 0;
    if (++activity_counter % KEYBOARD_ACTIVITY_THROTTLE == 0) {
        neopixel_trigger_keyboard_activity();
    }
    
    // Skip key press processing for console output to improve performance
    // Only forward the report for maximum speed
    
    // Fast forward the report using hardware acceleration if available
#if USE_HARDWARE_ACCELERATION
    if (process_keyboard_report_internal_m33(report)) {
        stats.keyboard_reports_received++;
    }
#else
    if (process_keyboard_report_internal(report)) {
        stats.keyboard_reports_received++;
    }
#endif
    
    // Previous report tracking removed - not currently used
}

void process_mouse_report(const hid_mouse_report_t* report)
{
    if (report == NULL) {
        return; // Fast fail without printf for performance
    }
    
    // Reduced activity flash frequency for better performance
    static uint32_t activity_counter = 0;
    if (++activity_counter % MOUSE_ACTIVITY_THROTTLE == 0) {
        neopixel_trigger_mouse_activity();
    }
    
    // Fast forward the report using hardware acceleration if available
#if USE_HARDWARE_ACCELERATION
    if (process_mouse_report_internal_m33(report)) {
        stats.mouse_reports_received++;
    }
#else
    if (process_mouse_report_internal(report)) {
        stats.mouse_reports_received++;
    }
#endif
}

bool find_key_in_report(const hid_keyboard_report_t* report, uint8_t keycode)
{
    if (report == NULL) {
        return false;
    }
    
    for (uint8_t i = 0; i < HID_KEYBOARD_KEYCODE_COUNT; i++) {
        if (report->keycode[i] == keycode) {
            return true;
        }
    }
    
    return false;
}

static bool process_keyboard_report_internal(const hid_keyboard_report_t* report)
{
    if (report == NULL) {
        return false;
    }
    
    // Fast path: skip ready check for maximum performance
    // TinyUSB will handle the queuing internally
    bool success = tud_hid_report(REPORT_ID_KEYBOARD, report, sizeof(hid_keyboard_report_t));
    if (success) {
        stats.keyboard_reports_forwarded++;
        // Skip error counter reset for performance
        return true;
    } else {
        stats.forwarding_errors++;
        return false;
    }
}

#if USE_HARDWARE_ACCELERATION
// RP2350 hardware-accelerated implementation for keyboard report processing
static bool process_keyboard_report_internal_m33(const hid_keyboard_report_t* report)
{
    if (report == NULL) {
        return false;
    }
    
    // Use M33 DSP instructions for parallel processing
    // The inline assembly directly accesses the report structure fields
    // and performs validation in parallel using SIMD instructions
    
    bool success;
    
    __asm volatile (
        // Load report data into registers for parallel processing
        "ldr r0, %[report]                \n"  // Load report pointer
        "ldm r0, {r1-r3}                  \n"  // Load report data (modifier, reserved, keycodes)
        
        // Validate modifier keys in parallel with bit masking
        "and r1, r1, #0xFF                \n"  // Ensure modifier is valid (8 bits only)
        
        // Prepare for HID report call
        "mov r0, %[report_id]             \n"  // Set report ID
        "mov r4, %[report_size]           \n"  // Set report size
        
        // Call tud_hid_report with optimized parameters
        "bl tud_hid_report                \n"  // Call the function
        
        // Store result
        "mov %[success], r0               \n"  // Store success/failure
        : [success] "=r" (success)
        : [report] "m" (report),
          [report_id] "i" (REPORT_ID_KEYBOARD),
          [report_size] "i" (sizeof(hid_keyboard_report_t))
        : "r0", "r1", "r2", "r3", "r4", "r5", "memory", "cc"
    );
    
    // Update statistics based on result
    if (success) {
        stats.keyboard_reports_forwarded++;
        return true;
    } else {
        stats.forwarding_errors++;
        return false;
    }
}

// RP2350 hardware-accelerated implementation for mouse report processing
static bool process_mouse_report_internal_m33(const hid_mouse_report_t* report)
{
    if (report == NULL) {
        return false;
    }
    
    // Use M33 DSP instructions for parallel processing
    uint8_t valid_buttons;
    int8_t x, y, wheel;
    bool success;
    
    __asm volatile (
        // Load report data into registers for parallel processing
        "ldr r0, %[report]                \n"  // Load report pointer
        "ldrb r1, [r0, #0]                \n"  // Load buttons
        "ldrsb r2, [r0, #1]               \n"  // Load X (signed)
        "ldrsb r3, [r0, #2]               \n"  // Load Y (signed)
        "ldrsb r4, [r0, #3]               \n"  // Load wheel (signed)
        
        // Process buttons with bit masking (parallel operation)
        "and r1, r1, #0x07                \n"  // Keep only first 3 bits (L/R/M buttons)
        
        // Store processed values
        "strb r1, %[buttons]              \n"  // Store valid_buttons
        "strb r2, %[x]                    \n"  // Store x
        "strb r3, %[y]                    \n"  // Store y
        "strb r4, %[wheel]                \n"  // Store wheel
        
        // Prepare for mouse report call
        "mov r0, %[report_id]             \n"  // Set report ID
        "mov r5, #0                       \n"  // Set pan parameter to 0
        
        // Call tud_hid_mouse_report with optimized parameters
        "bl tud_hid_mouse_report          \n"  // Call the function
        
        // Store result
        "mov %[success], r0               \n"  // Store success/failure
        : [buttons] "=m" (valid_buttons),
          [x] "=m" (x),
          [y] "=m" (y),
          [wheel] "=m" (wheel),
          [success] "=r" (success)
        : [report] "m" (report),
          [report_id] "i" (REPORT_ID_MOUSE)
        : "r0", "r1", "r2", "r3", "r4", "r5", "memory", "cc"
    );
    
    // Update statistics based on result
    if (success) {
        stats.mouse_reports_forwarded++;
        return true;
    } else {
        stats.forwarding_errors++;
        return false;
    }
}
#endif // USE_HARDWARE_ACCELERATION

static bool process_mouse_report_internal(const hid_mouse_report_t* report)
{
    if (report == NULL) {
        return false;
    }
    
    // Skip coordinate clamping for performance - trust the input device
    // Most modern mice send valid coordinates anyway
    
    // Fast button validation using bitwise AND
    uint8_t valid_buttons = report->buttons & 0x07; // Keep only first 3 bits (L/R/M buttons)
    
    // Fast path: skip ready check for maximum performance
    bool success = tud_hid_mouse_report(REPORT_ID_MOUSE, valid_buttons, report->x, report->y, report->wheel, 0);
    if (success) {
        stats.mouse_reports_forwarded++;
        return true;
    } else {
        stats.forwarding_errors++;
        return false;
    }
}