/*
 * Hurricane PIOKMBox Firmware
*/


#include "led_control.h"
#include "usb_hid.h"
#include "defines.h"
#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "ws2812.pio.h"
#include "tusb.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

//--------------------------------------------------------------------+
// CONSTANTS AND CONFIGURATION
//--------------------------------------------------------------------+

// All timing, brightness, and configuration constants are now in defines.h

//--------------------------------------------------------------------+
// TYPE DEFINITIONS
//--------------------------------------------------------------------+

/**
 * @brief LED controller state structure
 */
typedef struct {
    // Hardware state
    bool initialized;
    PIO pio_instance;
    uint state_machine;
    
    // Status management
    system_status_t current_status;
    system_status_t status_override;
    bool status_override_active;
    uint32_t boot_start_time;
    
    // Activity tracking
    bool activity_flash_active;
    uint32_t activity_flash_start_time;
    uint32_t activity_flash_color;
    
    bool caps_lock_flash_active;
    uint32_t caps_lock_flash_start_time;
    
    // Breathing effect
    bool breathing_enabled;
    uint32_t breathing_start_time;
    float current_brightness;
    
    // LED blinking
    uint32_t blink_interval_ms;
    uint32_t last_blink_time;
    bool led_state;
    
    // Rainbow effect
    bool rainbow_effect_active;
    uint32_t rainbow_start_time;
    uint32_t rainbow_hue;  // Current hue in the rainbow cycle (0-360)
} led_controller_t;

/**
 * @brief Status configuration structure
 */
typedef struct {
    uint32_t color;
    bool breathing_effect;
    const char* name;
} status_config_t;

//--------------------------------------------------------------------+
// PRIVATE VARIABLES
//--------------------------------------------------------------------+

static led_controller_t g_led_controller = {
    .initialized = false,
    .pio_instance = pio1,
    .state_machine = 0,
    .current_status = STATUS_BOOTING,
    .status_override = STATUS_BOOTING,
    .status_override_active = false,
    .boot_start_time = 0,
    .activity_flash_active = false,
    .caps_lock_flash_active = false,
    .breathing_enabled = false,
    .current_brightness = MAX_BRIGHTNESS,
    .blink_interval_ms = DEFAULT_BLINK_INTERVAL_MS,
    .last_blink_time = 0,
    .led_state = false,
    .rainbow_effect_active = false,
    .rainbow_start_time = 0,
    .rainbow_hue = 0
};

// Status configuration lookup table
static const status_config_t g_status_configs[] = {
    [STATUS_BOOTING]              = {COLOR_BOOTING,              true,  "BOOTING"},
    [STATUS_USB_DEVICE_ONLY]      = {COLOR_USB_DEVICE_ONLY,      false, "USB_DEVICE_ONLY"},
    [STATUS_USB_HOST_ONLY]        = {COLOR_USB_HOST_ONLY,        false, "USB_HOST_ONLY"},
    [STATUS_BOTH_ACTIVE]          = {COLOR_BOTH_ACTIVE,          false, "BOTH_ACTIVE"},
    [STATUS_MOUSE_CONNECTED]      = {COLOR_MOUSE_CONNECTED,      false, "MOUSE_CONNECTED"},
    [STATUS_KEYBOARD_CONNECTED]   = {COLOR_KEYBOARD_CONNECTED,   false, "KEYBOARD_CONNECTED"},
    [STATUS_BOTH_HID_CONNECTED]   = {COLOR_BOTH_HID_CONNECTED,   false, "BOTH_HID_CONNECTED"},
    [STATUS_ERROR]                = {COLOR_ERROR,                true,  "ERROR"},
    [STATUS_SUSPENDED]            = {COLOR_SUSPENDED,            true,  "SUSPENDED"},
    [STATUS_USB_RESET_PENDING]    = {COLOR_USB_RESET_PENDING,    true,  "USB_RESET_PENDING"},
    [STATUS_USB_RESET_SUCCESS]    = {COLOR_USB_RESET_SUCCESS,    false, "USB_RESET_SUCCESS"},
    [STATUS_USB_RESET_FAILED]     = {COLOR_USB_RESET_FAILED,     true,  "USB_RESET_FAILED"}
};

//--------------------------------------------------------------------+
// PRIVATE FUNCTION DECLARATIONS
//--------------------------------------------------------------------+

static bool validate_brightness(float brightness);
static bool validate_color(uint32_t color);
static bool validate_status(system_status_t status);
static uint32_t get_current_time_ms(void);
static bool is_time_elapsed(uint32_t start_time, uint32_t duration_ms);
static void update_breathing_brightness(void);
static system_status_t determine_system_status(void);
static void apply_status_change(system_status_t new_status);
static void handle_activity_flash(void);
static void handle_caps_lock_flash(void);
static void handle_breathing_effect(void);
static void handle_rainbow_effect(void);
static void log_status_change(system_status_t status, uint32_t color, bool breathing);
static uint32_t hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value);

//--------------------------------------------------------------------+
// UTILITY FUNCTIONS
//--------------------------------------------------------------------+

/**
 * @brief Validate brightness value
 */
static bool validate_brightness(float brightness)
{
    return (brightness >= MIN_BRIGHTNESS && brightness <= MAX_BRIGHTNESS);
}

/**
 * @brief Validate color value (basic sanity check)
 */
static bool validate_color(uint32_t color)
{
    return (color <= 0xFFFFFF); // 24-bit RGB
}

/**
 * @brief Validate system status
 */
static bool validate_status(system_status_t status)
{
    return (status < (sizeof(g_status_configs) / sizeof(g_status_configs[0])));
}

/**
 * @brief Get current time in milliseconds
 */
static uint32_t get_current_time_ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}

/**
 * @brief Check if specified time has elapsed
 */
static bool is_time_elapsed(uint32_t start_time, uint32_t duration_ms)
{
    return (get_current_time_ms() - start_time) >= duration_ms;
}

//--------------------------------------------------------------------+
// LED BLINKING FUNCTIONS
//--------------------------------------------------------------------+

void led_blinking_task(void)
{
    // Skip if blinking is disabled
    if (g_led_controller.blink_interval_ms == 0) {
        return;
    }

    uint32_t current_time = get_current_time_ms();
    
    // Check if it's time to toggle
    if (!is_time_elapsed(g_led_controller.last_blink_time, g_led_controller.blink_interval_ms)) {
        return;
    }

    // Update timing and toggle LED
    g_led_controller.last_blink_time = current_time;
    g_led_controller.led_state = !g_led_controller.led_state;
    gpio_put(PIN_LED, g_led_controller.led_state);
}

void led_set_blink_interval(uint32_t interval_ms)
{
    g_led_controller.blink_interval_ms = interval_ms;
    
    // Reset timing when interval changes
    if (interval_ms > 0) {
        g_led_controller.last_blink_time = get_current_time_ms();
    }
}

//--------------------------------------------------------------------+
// NEOPIXEL CORE FUNCTIONS
//--------------------------------------------------------------------+

void neopixel_init(void)
{
    // Prevent double initialization
    if (g_led_controller.initialized) {
        printf("Warning: Neopixel already initialized\n");
        return;
    }

    // Initialize LED pin
    gpio_init(PIN_LED);
    gpio_set_dir(PIN_LED, GPIO_OUT);
    gpio_put(PIN_LED, 0);

    // Initialize neopixel power pin but keep it OFF during early boot
    gpio_init(NEOPIXEL_POWER);
    gpio_set_dir(NEOPIXEL_POWER, GPIO_OUT);
    gpio_put(NEOPIXEL_POWER, 0);  // Keep power OFF initially

    printf("Neopixel pins initialized (power OFF for cold boot stability)\n");
}

void neopixel_enable_power(void)
{
    if (g_led_controller.initialized) {
        printf("Warning: Neopixel already fully initialized\n");
        return;
    }

    printf("Enabling neopixel power...\n");
    
    // Enable neopixel power
    gpio_put(NEOPIXEL_POWER, 1);

    // Allow power to stabilize
    sleep_ms(POWER_STABILIZATION_DELAY_MS);

    // Load WS2812 program into PIO
    uint offset = pio_add_program(g_led_controller.pio_instance, &ws2812_program);
    if (offset == (uint)-1) {
        printf("Error: Failed to load WS2812 program into PIO\n");
        return;
    }

    // Initialize state machine
    ws2812_program_init(g_led_controller.pio_instance,
                       g_led_controller.state_machine,
                       offset,
                       PIN_NEOPIXEL,
                       WS2812_FREQUENCY_HZ,
                       false);

    // Mark as initialized and set initial state
    g_led_controller.initialized = true;
    g_led_controller.boot_start_time = get_current_time_ms();
    
    // Set initial color
    neopixel_set_color(COLOR_BOOTING);

    printf("Neopixel fully initialized and powered on pin %d\n", PIN_NEOPIXEL);
}

uint32_t neopixel_rgb_to_grb(uint32_t rgb)
{
    if (!validate_color(rgb)) {
        return 0;
    }

    const uint8_t r = (rgb >> 16) & 0xFF;
    const uint8_t g = (rgb >> 8) & 0xFF;
    const uint8_t b = rgb & 0xFF;

    return (g << 16) | (r << 8) | b;
}

uint32_t neopixel_apply_brightness(uint32_t color, float brightness)
{
    if (!validate_color(color) || !validate_brightness(brightness)) {
        return 0;
    }

    const uint8_t r = (uint8_t)(((color >> 16) & 0xFF) * brightness);
    const uint8_t g = (uint8_t)(((color >> 8) & 0xFF) * brightness);
    const uint8_t b = (uint8_t)((color & 0xFF) * brightness);

    return (r << 16) | (g << 8) | b;
}

void neopixel_set_color(uint32_t color)
{
    neopixel_set_color_with_brightness(color, MAX_BRIGHTNESS);
}

void neopixel_set_color_with_brightness(uint32_t color, float brightness)
{
    if (!g_led_controller.initialized) {
        return;
    }

    if (!validate_color(color) || !validate_brightness(brightness)) {
        printf("Warning: Invalid color (0x%06lX) or brightness (%.2f)\n", 
               (unsigned long)color, brightness);
        return;
    }

    // Apply brightness and convert to GRB format
    const uint32_t dimmed_color = neopixel_apply_brightness(color, brightness);
    const uint32_t grb_color = neopixel_rgb_to_grb(dimmed_color);
    
    // Send to PIO state machine
    pio_sm_put_blocking(g_led_controller.pio_instance, 
                       g_led_controller.state_machine, 
                       grb_color << WS2812_RGB_SHIFT);
}

//--------------------------------------------------------------------+
// BREATHING EFFECT
//--------------------------------------------------------------------+

void neopixel_breathing_effect(void)
{
    update_breathing_brightness();
}

static void update_breathing_brightness(void)
{
    const uint32_t current_time = get_current_time_ms();
    
    // Initialize breathing start time if needed
    if (g_led_controller.breathing_start_time == 0) {
        g_led_controller.breathing_start_time = current_time;
    }

    // Calculate cycle position
    uint32_t cycle_time = current_time - g_led_controller.breathing_start_time;
    
    // Reset cycle if complete
    if (cycle_time >= BREATHING_CYCLE_MS) {
        g_led_controller.breathing_start_time = current_time;
        cycle_time = 0;
    }

    // Calculate brightness using sine wave for smooth transition
    float progress;
    if (cycle_time < BREATHING_HALF_CYCLE_MS) {
        // First half: getting brighter
        progress = (float)cycle_time / BREATHING_HALF_CYCLE_MS;
    } else {
        // Second half: getting dimmer
        progress = 1.0f - ((float)(cycle_time - BREATHING_HALF_CYCLE_MS) / BREATHING_HALF_CYCLE_MS);
    }

    // Apply sine wave for smoother breathing effect
    g_led_controller.current_brightness = BREATHING_MIN_BRIGHTNESS + 
        (BREATHING_MAX_BRIGHTNESS - BREATHING_MIN_BRIGHTNESS) * 
        sinf(progress * (float)M_PI / 2.0f);
}

//--------------------------------------------------------------------+
// STATUS MANAGEMENT
//--------------------------------------------------------------------+

static system_status_t determine_system_status(void)
{
    // Check for suspended state first
    if (tud_suspended()) {
        return STATUS_SUSPENDED;
    }

    // Check boot timeout first - if we've been running long enough, we should exit boot status
    if (g_led_controller.boot_start_time == 0) {
        g_led_controller.boot_start_time = get_current_time_ms();
    }

    // If still in boot timeout, stay in booting status
    if (!is_time_elapsed(g_led_controller.boot_start_time, BOOT_TIMEOUT_MS)) {
        return STATUS_BOOTING;
    }

    // After boot timeout, determine actual status based on USB connections
    const bool device_mounted = tud_mounted();
    
#if PIO_USB_AVAILABLE
    const bool host_mounted = tuh_mounted(1);
    const bool mouse_connected = is_mouse_connected();
    const bool keyboard_connected = is_keyboard_connected();

    // Both USB device and host are active
    if (device_mounted && host_mounted) {
        if (mouse_connected && keyboard_connected) {
            return STATUS_BOTH_HID_CONNECTED;
        } else if (mouse_connected) {
            return STATUS_MOUSE_CONNECTED;
        } else if (keyboard_connected) {
            return STATUS_KEYBOARD_CONNECTED;
        } else {
            return STATUS_BOTH_ACTIVE;
        }
    }
    // Only USB device is mounted
    else if (device_mounted) {
        return STATUS_USB_DEVICE_ONLY;
    }
    // Only USB host has devices
    else if (host_mounted) {
        if (mouse_connected && keyboard_connected) {
            return STATUS_BOTH_HID_CONNECTED;
        } else if (mouse_connected) {
            return STATUS_MOUSE_CONNECTED;
        } else if (keyboard_connected) {
            return STATUS_KEYBOARD_CONNECTED;
        } else {
            return STATUS_USB_HOST_ONLY;
        }
    }
    // Neither USB device nor host have connections - show host only since it's initialized
    else {
        return STATUS_USB_HOST_ONLY;
    }
#else
    // PIO USB not available, only check device
    if (device_mounted) {
        return STATUS_USB_DEVICE_ONLY;
    } else {
        return STATUS_USB_DEVICE_ONLY;  // Still show device status even if not mounted
    }
#endif
}

static void apply_status_change(system_status_t new_status)
{
    if (!validate_status(new_status)) {
        printf("Error: Invalid status %d\n", new_status);
        return;
    }

    const status_config_t* config = &g_status_configs[new_status];
    
    g_led_controller.current_status = new_status;
    g_led_controller.breathing_enabled = config->breathing_effect;
    
    // Reset breathing timing when status changes
    if (g_led_controller.breathing_enabled) {
        g_led_controller.breathing_start_time = 0;
    } else {
        neopixel_set_color(config->color);
    }

    log_status_change(new_status, config->color, config->breathing_effect);
}

void neopixel_update_status(void)
{
    const system_status_t new_status = determine_system_status();
    
    if (new_status != g_led_controller.current_status) {
        apply_status_change(new_status);
    }
}

static void log_status_change(system_status_t status, uint32_t color, bool breathing)
{
    const status_config_t* config = &g_status_configs[status];
    
    printf("Neopixel status: %s (Color: 0x%06lX, Breathing: %s)\n",
           config->name, (unsigned long)color, breathing ? "Yes" : "No");
    printf("USB Device mounted: %s, suspended: %s\n",
           tud_mounted() ? "Yes" : "No", tud_suspended() ? "Yes" : "No");
    printf("HID devices - Mouse: %s, Keyboard: %s\n",
           is_mouse_connected() ? "Yes" : "No", is_keyboard_connected() ? "Yes" : "No");
}

//--------------------------------------------------------------------+
// TASK HANDLERS
//--------------------------------------------------------------------+

static void handle_activity_flash(void)
{
    if (!g_led_controller.activity_flash_active) {
        return;
    }

    if (is_time_elapsed(g_led_controller.activity_flash_start_time, ACTIVITY_FLASH_DURATION_MS)) {
        g_led_controller.activity_flash_active = false;
        // Return to normal status display will happen in main task
    } else {
        neopixel_set_color(g_led_controller.activity_flash_color);
    }
}

static void handle_caps_lock_flash(void)
{
    if (!g_led_controller.caps_lock_flash_active) {
        return;
    }

    if (is_time_elapsed(g_led_controller.caps_lock_flash_start_time, ACTIVITY_FLASH_DURATION_MS)) {
        g_led_controller.caps_lock_flash_active = false;
        // Return to normal status display will happen in main task
    }
}

static void handle_breathing_effect(void)
{
    if (!g_led_controller.breathing_enabled) {
        return;
    }

    neopixel_breathing_effect();
    
    const status_config_t* config = &g_status_configs[g_led_controller.current_status];
    neopixel_set_color_with_brightness(config->color, g_led_controller.current_brightness);
}

void neopixel_status_task(void)
{
    static uint32_t last_update_time = 0;
    
    // Throttle updates to reduce CPU usage
    if (!is_time_elapsed(last_update_time, STATUS_UPDATE_INTERVAL_MS)) {
        return;
    }
    last_update_time = get_current_time_ms();

    // Use override status if active, otherwise update normally
    if (g_led_controller.status_override_active) {
        if (g_led_controller.current_status != g_led_controller.status_override) {
            apply_status_change(g_led_controller.status_override);
        }
    } else {
        neopixel_update_status();
    }

    // Handle special effects (order matters for priority)
    handle_activity_flash();
    handle_caps_lock_flash();
    
    // Handle rainbow effect (can overlay with other effects)
    if (g_led_controller.rainbow_effect_active) {
        handle_rainbow_effect();
        // Rainbow takes priority over other effects
        return;
    }
    
    // Handle other effects if rainbow is not active
    if (!g_led_controller.activity_flash_active && !g_led_controller.caps_lock_flash_active) {
        handle_breathing_effect();
    }
}

//--------------------------------------------------------------------+
// ACTIVITY TRIGGER FUNCTIONS
//--------------------------------------------------------------------+

static void trigger_activity_flash_internal(uint32_t color)
{
    if (!g_led_controller.initialized || !validate_color(color)) {
        return;
    }

    g_led_controller.activity_flash_active = true;
    g_led_controller.activity_flash_start_time = get_current_time_ms();
    g_led_controller.activity_flash_color = color;
}

void neopixel_trigger_activity_flash(void)
{
    trigger_activity_flash_internal(COLOR_ACTIVITY_FLASH);
}

void neopixel_trigger_mouse_activity(void)
{
    trigger_activity_flash_internal(COLOR_MOUSE_ACTIVITY);
}

void neopixel_trigger_keyboard_activity(void)
{
    trigger_activity_flash_internal(COLOR_KEYBOARD_ACTIVITY);
}

void neopixel_trigger_usb_connection_flash(void)
{
    trigger_activity_flash_internal(COLOR_USB_CONNECTION);
}

void neopixel_trigger_usb_disconnection_flash(void)
{
    trigger_activity_flash_internal(COLOR_USB_DISCONNECTION);
}

void neopixel_trigger_caps_lock_flash(void)
{
    if (!g_led_controller.initialized) {
        return;
    }

    g_led_controller.caps_lock_flash_active = true;
    g_led_controller.caps_lock_flash_start_time = get_current_time_ms();
}

//--------------------------------------------------------------------+
// USB RESET FUNCTIONS
//--------------------------------------------------------------------+

void neopixel_trigger_usb_reset_pending(void)
{
    if (!g_led_controller.initialized) {
        return;
    }

    neopixel_set_status_override(STATUS_USB_RESET_PENDING);
    printf("Neopixel: USB reset pending status activated\n");
}

void neopixel_trigger_usb_reset_success(void)
{
    if (!g_led_controller.initialized) {
        return;
    }

    // Clear any status override first
    neopixel_clear_status_override();
    
    // Trigger success flash
    trigger_activity_flash_internal(COLOR_USB_RESET_SUCCESS);
    
    printf("Neopixel: USB reset success flash triggered\n");
}

void neopixel_trigger_usb_reset_failed(void)
{
    if (!g_led_controller.initialized) {
        return;
    }

    neopixel_set_status_override(STATUS_USB_RESET_FAILED);
    printf("Neopixel: USB reset failed status activated\n");
}

//--------------------------------------------------------------------+
// STATUS OVERRIDE FUNCTIONS
//--------------------------------------------------------------------+

void neopixel_set_status_override(system_status_t status)
{
    if (!g_led_controller.initialized || !validate_status(status)) {
        printf("Error: Cannot set status override - invalid status %d\n", status);
        return;
    }

    g_led_controller.status_override = status;
    g_led_controller.status_override_active = true;

    printf("Neopixel: Status override set to %s\n", g_status_configs[status].name);
}

void neopixel_clear_status_override(void)
{
    if (!g_led_controller.initialized) {
        return;
    }

    g_led_controller.status_override_active = false;
    printf("Neopixel: Status override cleared\n");
}

//--------------------------------------------------------------------+
// RAINBOW EFFECT FUNCTIONS
//--------------------------------------------------------------------+

/**
 * @brief Convert HSV color to RGB
 * @param hue Hue value (0-360)
 * @param saturation Saturation value (0-255)
 * @param value Brightness value (0-255)
 * @return RGB color as 24-bit value
 */
static uint32_t hsv_to_rgb(uint16_t hue, uint8_t saturation, uint8_t value)
{
    // Ensure hue is in valid range
    hue = hue % 360;
    
    uint8_t region = hue / 60;
    uint8_t remainder = (hue - (region * 60)) * 255 / 60;
    
    uint8_t p = (value * (255 - saturation)) >> 8;
    uint8_t q = (value * (255 - ((saturation * remainder) >> 8))) >> 8;
    uint8_t t = (value * (255 - ((saturation * (255 - remainder)) >> 8))) >> 8;
    
    uint8_t r, g, b;
    
    switch (region) {
        case 0:
            r = value; g = t; b = p;
            break;
        case 1:
            r = q; g = value; b = p;
            break;
        case 2:
            r = p; g = value; b = t;
            break;
        case 3:
            r = p; g = q; b = value;
            break;
        case 4:
            r = t; g = p; b = value;
            break;
        default:
            r = value; g = p; b = q;
            break;
    }
    
    return (r << 16) | (g << 8) | b;
}

static void handle_rainbow_effect(void)
{
    const uint32_t current_time = get_current_time_ms();
    
    // Initialize rainbow start time if needed
    if (g_led_controller.rainbow_start_time == 0) {
        g_led_controller.rainbow_start_time = current_time;
    }
    
    // Check if rainbow effect should end (after 300ms for quick visual feedback)
    if (is_time_elapsed(g_led_controller.rainbow_start_time, 300)) {
        g_led_controller.rainbow_effect_active = false;
        g_led_controller.rainbow_start_time = 0;
        return;
    }
    
    // Update hue for smooth rainbow transition
    // Complete rainbow cycle every 300ms for fast, vibrant effect
    uint32_t elapsed = current_time - g_led_controller.rainbow_start_time;
    g_led_controller.rainbow_hue = (elapsed * 360 / 300) % 360;
    
    // Convert HSV to RGB with full saturation and brightness
    uint32_t rainbow_color = hsv_to_rgb(g_led_controller.rainbow_hue, 255, 255);
    
    // Apply brightness for visibility with slight pulse effect
    float brightness = 0.6f + 0.3f * sinf((float)elapsed * 0.02f);
    neopixel_set_color_with_brightness(rainbow_color, brightness);
}

void neopixel_trigger_rainbow_effect(void)
{
    if (!g_led_controller.initialized) {
        return;
    }
    
    // Always reset the effect for immediate visual feedback
    g_led_controller.rainbow_effect_active = true;
    g_led_controller.rainbow_start_time = get_current_time_ms();
    
    // Start with a random hue for variety
    static uint32_t start_hue = 0;
    start_hue = (start_hue + 120) % 360; // Shift by 120 degrees each time
    g_led_controller.rainbow_hue = start_hue;
}