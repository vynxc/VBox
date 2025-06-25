/*
 * Hurricane PIOKMbox Firmware
 */

#include "usb_hid_stats.h"
#include "defines.h"
#include <stdio.h>
#include <string.h>
#include "pico/time.h"

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
    *stats_out = stats;  // Copy the entire structure
    
#if defined(RP2350)
    // Calculate derived statistics
    stats_out->hw_accel_success_rate = calculate_hw_accel_success_rate();
    stats_out->hw_avg_processing_time_us = calculate_avg_hw_processing_time_us();
    stats_out->sw_avg_processing_time_us = calculate_avg_sw_processing_time_us();
#endif
    
    // Print connection status with VID/PID information
    print_hid_connection_status();
}

void reset_hid_stats(void)
{
    memset(&stats, 0, sizeof(stats));
    debug_counter = 0;
}

#if defined(RP2350)
// RP2350 hardware acceleration statistics functions

void track_hw_processing_latency(uint64_t start_time)
{
    uint64_t end_time = time_us_64();
    uint64_t latency = end_time - start_time;
    
    stats.hw_processing_time_us += latency;
    stats.hw_processing_count++;
}

void track_sw_processing_latency(uint64_t start_time)
{
    uint64_t end_time = time_us_64();
    uint64_t latency = end_time - start_time;
    
    stats.sw_processing_time_us += latency;
    stats.sw_processing_count++;
}

void increment_hw_accel_reports(void)
{
    stats.hw_accel_reports_processed++;
}

void increment_sw_fallback_reports(void)
{
    stats.sw_fallback_reports_processed++;
}

void increment_hw_accel_errors(void)
{
    stats.hw_accel_errors++;
}

float calculate_hw_accel_success_rate(void)
{
    uint32_t total_reports = stats.hw_accel_reports_processed + stats.sw_fallback_reports_processed;
    if (total_reports == 0) {
        return 0.0f;
    }
    return (float)stats.hw_accel_reports_processed / (float)total_reports * 100.0f;
}

float calculate_avg_hw_processing_time_us(void)
{
    if (stats.hw_processing_count == 0) {
        return 0.0f;
    }
    return (float)stats.hw_processing_time_us / (float)stats.hw_processing_count;
}

float calculate_avg_sw_processing_time_us(void)
{
    if (stats.sw_processing_count == 0) {
        return 0.0f;
    }
    return (float)stats.sw_processing_time_us / (float)stats.sw_processing_count;
}

void print_hw_accel_stats(void)
{
    printf("RP2350 Hardware Acceleration Statistics:\n");
    printf("  HW Accelerated Reports: %lu\n", stats.hw_accel_reports_processed);
    printf("  SW Fallback Reports: %lu\n", stats.sw_fallback_reports_processed);
    printf("  HW Acceleration Errors: %lu\n", stats.hw_accel_errors);
    printf("  HW Acceleration Success Rate: %.2f%%\n", calculate_hw_accel_success_rate());
    
    float hw_avg_time = calculate_avg_hw_processing_time_us();
    float sw_avg_time = calculate_avg_sw_processing_time_us();
    
    printf("  Avg HW Processing Time: %.2f µs\n", hw_avg_time);
    printf("  Avg SW Processing Time: %.2f µs\n", sw_avg_time);
    
    if (hw_avg_time > 0 && sw_avg_time > 0) {
        float speedup = sw_avg_time / hw_avg_time;
        printf("  HW Acceleration Speedup: %.2fx\n", speedup);
    }
}
#endif