/*
 * Hurricane vbox Firmware
 */

#include "usb_hid.h"
#include "defines.h"
#include "led_control.h"
#include "lib/kmbox-commands/kmbox_commands.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "kmbox_serial_handler.h" // Include the header for serial handling
#include "state_management.h"     // Include the header for state management
#include "watchdog.h"             // Include the header for watchdog management
#include <string.h>               // For strcpy, strlen, memset

uint16_t attached_vid = 0;
uint16_t attached_pid = 0;
bool attached_has_serial = false;


static char attached_manufacturer[64] = "";
static char attached_product[64] = "";
static char attached_serial[32] = "";
static bool string_descriptors_fetched = false;

#define LANGUAGE_ID 0x0409 // English (US)



static void utf16_to_utf8(uint16_t *utf16_buf, size_t utf16_buf_bytes, char *utf8_buf, size_t utf8_len)
{
    if (!utf16_buf || !utf8_buf || utf8_len == 0)
        return;



    const uint8_t *raw = (const uint8_t *)utf16_buf;
    if (!raw)
        return;


    uint8_t bLength = raw[0];
    if (bLength > utf16_buf_bytes)
    {
        bLength = (uint8_t)utf16_buf_bytes;
    }


    size_t code_units = 0;
    if (bLength >= 2)
    {
        code_units = (size_t)(bLength - 2) / 2;
    }

    size_t utf8_pos = 0;
    for (size_t i = 0; i < code_units && utf8_pos < utf8_len - 1; i++)
    {

        uint16_t u = utf16_buf[1 + i];


        if (u == 0)
            break;

        if (u <= 0x7F)
        {
            utf8_buf[utf8_pos++] = (char)u;
        }
        else
        {

            utf8_buf[utf8_pos++] = '?';
        }
    }

    utf8_buf[utf8_pos] = '\0';
}


void set_attached_device_vid_pid(uint16_t vid, uint16_t pid)
{

    if (attached_vid != vid || attached_pid != pid)
    {
        attached_vid = vid;
        attached_pid = pid;
        attached_has_serial = false; // Default to no serial number unless device has one


        force_usb_reenumeration();
    }
    else
    {
    }
}

void force_usb_reenumeration()
{


    tud_disconnect();


    sleep_ms(500);


    tud_connect();


    sleep_ms(250);

}


static void fetch_device_string_descriptors(uint8_t dev_addr)
{

    memset(attached_manufacturer, 0, sizeof(attached_manufacturer));
    memset(attached_product, 0, sizeof(attached_product));
    memset(attached_serial, 0, sizeof(attached_serial));
    string_descriptors_fetched = false;


    uint16_t temp_manufacturer[32];
    uint16_t temp_product[48];
    uint16_t temp_serial[16];


    memset(temp_manufacturer, 0, sizeof(temp_manufacturer));
    memset(temp_product, 0, sizeof(temp_product));
    memset(temp_serial, 0, sizeof(temp_serial));


    if (tuh_descriptor_get_manufacturer_string_sync(dev_addr, LANGUAGE_ID, temp_manufacturer, sizeof(temp_manufacturer)) == XFER_RESULT_SUCCESS)
    {
        utf16_to_utf8(temp_manufacturer, sizeof(temp_manufacturer), attached_manufacturer, sizeof(attached_manufacturer));
    }
    else
    {
        strcpy(attached_manufacturer, MANUFACTURER_STRING); // Fallback
    }


    if (tuh_descriptor_get_product_string_sync(dev_addr, LANGUAGE_ID, temp_product, sizeof(temp_product)) == XFER_RESULT_SUCCESS)
    {
        utf16_to_utf8(temp_product, sizeof(temp_product), attached_product, sizeof(attached_product));
    }
    else
    {
        strcpy(attached_product, PRODUCT_STRING); // Fallback
    }


    if (tuh_descriptor_get_serial_string_sync(dev_addr, LANGUAGE_ID, temp_serial, sizeof(temp_serial)) == XFER_RESULT_SUCCESS)
    {
        utf16_to_utf8(temp_serial, sizeof(temp_serial), attached_serial, sizeof(attached_serial));
        attached_has_serial = (strlen(attached_serial) > 0);
    }
    else
    {
        attached_has_serial = false;
    }

    string_descriptors_fetched = true;
}


static void reset_device_string_descriptors(void)
{
    memset(attached_manufacturer, 0, sizeof(attached_manufacturer));
    memset(attached_product, 0, sizeof(attached_product));
    memset(attached_serial, 0, sizeof(attached_serial));
    string_descriptors_fetched = false;
    attached_has_serial = false;
}


uint16_t get_attached_vid(void)
{
    return attached_vid;
}


uint16_t get_attached_pid(void)
{
    return attached_pid;
}


const char *get_dynamic_serial_string()
{
    static char dynamic_serial[64];
    if (attached_vid && attached_pid)
    {
        snprintf(dynamic_serial, sizeof(dynamic_serial), "vbox_%04X_%04X", attached_vid, attached_pid);
        return dynamic_serial;
    }
    return "vbox_v1.0";
}


typedef struct
{
    uint32_t device_errors;
    uint32_t host_errors;
    uint32_t consecutive_device_errors;
    uint32_t consecutive_host_errors;
    uint32_t last_error_check_time;
    bool device_error_state;
    bool host_error_state;
} usb_error_tracker_t;


typedef struct
{
    bool mouse_connected;
    uint8_t mouse_dev_addr;
} device_connection_state_t;


static bool caps_lock_state = false;

static device_connection_state_t connection_state = {0};


static char serial_string[SERIAL_STRING_BUFFER_SIZE] = {0};


static usb_error_tracker_t usb_error_tracker = {0};


static bool usb_device_initialized = false;
static bool usb_host_initialized = false;


static bool generate_serial_string(void);


static void handle_device_disconnection(uint8_t dev_addr);
static void handle_hid_device_connection(uint8_t dev_addr, uint8_t itf_protocol);


static bool process_mouse_report_internal(const hid_mouse_report_t *report);


static void print_device_info(uint8_t dev_addr, const tusb_desc_device_t *desc);


#define HID_DESC_BUF_SIZE 256

static const uint8_t desc_hid_mouse_default[] = {
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE))};

static const uint8_t desc_hid_consumer[] = {
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(REPORT_ID_CONSUMER_CONTROL))};


const uint8_t desc_hid_report[] = {
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(REPORT_ID_CONSUMER_CONTROL))};

static uint8_t desc_hid_report_runtime[HID_DESC_BUF_SIZE];
static size_t desc_hid_runtime_len = 0;
static bool desc_hid_runtime_valid = false;

static uint8_t host_mouse_desc[HID_DESC_BUF_SIZE];
static size_t host_mouse_desc_len = 0;
static bool host_mouse_has_report_id = false;
static uint8_t host_mouse_report_id = 0;

static void build_runtime_hid_report_with_mouse(const uint8_t *mouse_desc, size_t mouse_len)
{

    size_t pos = 0;


    if (mouse_desc != NULL && mouse_len > 0)
    {
        if (pos + mouse_len >= HID_DESC_BUF_SIZE)
            return;
        memcpy(&desc_hid_report_runtime[pos], mouse_desc, mouse_len);
        pos += mouse_len;
    }
    else
    {
        size_t dlen = sizeof(desc_hid_mouse_default);
        if (pos + dlen >= HID_DESC_BUF_SIZE)
            return;
        memcpy(&desc_hid_report_runtime[pos], desc_hid_mouse_default, dlen);
        pos += dlen;
    }


    size_t clen = sizeof(desc_hid_consumer);
    if (pos + clen >= HID_DESC_BUF_SIZE)
        return;
    memcpy(&desc_hid_report_runtime[pos], desc_hid_consumer, clen);
    pos += clen;


    for (; pos < HID_DESC_BUF_SIZE; ++pos)
    {
        desc_hid_report_runtime[pos] = 0xC0;
    }

    desc_hid_runtime_len = HID_DESC_BUF_SIZE;
    desc_hid_runtime_valid = true;
}

bool usb_hid_init(void)
{

    if (!generate_serial_string())
    {
        return false;
    }

    gpio_init(PIN_BUTTON);

    gpio_set_dir(PIN_BUTTON, GPIO_IN);
    gpio_pull_up(PIN_BUTTON);


    memset(&connection_state, 0, sizeof(connection_state));


    build_runtime_hid_report_with_mouse(NULL, 0);

    (void)0; // suppressed init log
    return true;
}

static bool generate_serial_string(void)
{
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);





    for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES; i++)
    {
        int result = snprintf(&serial_string[i * SERIAL_HEX_CHARS_PER_BYTE],
                              SERIAL_SNPRINTF_BUFFER_SIZE,
                              "%02X",
                              board_id.id[i]);
        if (result < 0 || result >= SERIAL_SNPRINTF_BUFFER_SIZE)
        {
            return false;
        }
    }

    serial_string[SERIAL_STRING_LENGTH] = '\0'; // Ensure null termination
    return true;
}

bool usb_host_enable_power(void)
{
#ifdef PIN_USB_5V
    gpio_put(PIN_USB_5V, 1); // Enable USB power
#endif
    sleep_ms(100); // Allow power to stabilize
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





bool get_caps_lock_state(void)
{
    return caps_lock_state;
}

bool is_mouse_connected(void)
{
    return connection_state.mouse_connected;
}

static void handle_device_disconnection(uint8_t dev_addr)
{

    if (dev_addr == connection_state.mouse_dev_addr)
    {
        connection_state.mouse_connected = false;
        connection_state.mouse_dev_addr = 0;
    }
}

static void handle_hid_device_connection(uint8_t dev_addr, uint8_t itf_protocol)
{

    if (dev_addr == 0)
    {

        return;
    }



    switch (itf_protocol)
    {
    case HID_ITF_PROTOCOL_MOUSE:
        connection_state.mouse_connected = true;
        connection_state.mouse_dev_addr = dev_addr;
        neopixel_trigger_mouse_activity(); // Flash magenta for mouse connection
        break;

    default:

        break;
    }


    neopixel_update_status();
}

static bool process_mouse_report_internal(const hid_mouse_report_t *report)
{
    if (!report || !tud_mounted() || !tud_ready() || !tud_hid_ready())
        return false;


    uint8_t valid_buttons = report->buttons & 0x1F; 

    kmbox_update_physical_buttons(valid_buttons);

    if (report->x != 0 || report->y != 0)
        kmbox_add_mouse_movement(report->x, report->y);

    if (report->wheel != 0)
        kmbox_add_wheel_movement(report->wheel);

    uint8_t buttons_to_send;
    int8_t x, y, wheel, pan;
    kmbox_get_mouse_report(&buttons_to_send, &x, &y, &wheel, &pan);

    int8_t final_x = x;
    int8_t final_y = y;
    int8_t final_wheel = wheel;

    if (!tud_hid_ready())
        return false;

    return tud_hid_mouse_report(REPORT_ID_MOUSE, buttons_to_send, final_x, final_y, final_wheel, pan);
}

static void print_device_info(uint8_t dev_addr, const tusb_desc_device_t *desc)
{
    (void)dev_addr; // Suppress unused parameter warning
    if (desc == NULL)
    {
        return;
        return;
    }

    (void)desc; // suppressed detailed device info logging
}

void process_mouse_report(const hid_mouse_report_t *report)
{
    if (report == NULL)
    {
        return; // Fast fail without printf for performance
    }

    static uint32_t activity_counter = 0;
    if (++activity_counter % MOUSE_ACTIVITY_THROTTLE == 0)
    {
        neopixel_trigger_mouse_activity();
    }


    if (process_mouse_report_internal(report))
    {

    }


    if (report->x != 0 || report->y != 0)
    {
        neopixel_rainbow_on_movement(report->x, report->y);
    }
}

void hid_device_task(void)
{

    static uint32_t start_ms = 0;
    uint32_t current_ms = to_ms_since_boot(get_absolute_time());

    if (current_ms - start_ms < HID_DEVICE_TASK_INTERVAL_MS)
    {
        return; // Not enough time elapsed
    }
    start_ms = current_ms;


    if (tud_suspended() && !gpio_get(PIN_BUTTON))
    {

        tud_remote_wakeup();
        return;
    }


    if (!tud_mounted() || !tud_ready())
    {
        return; // Don't send reports if device is not properly mounted
    }


    if (!connection_state.mouse_connected)
    {
        send_hid_report(REPORT_ID_MOUSE);
    }
    else
    {

        send_hid_report(REPORT_ID_CONSUMER_CONTROL);
    }
}

void send_hid_report(uint8_t report_id)
{

    if (!tud_mounted() || !tud_ready())
    {
        return; // Prevent endpoint conflicts by not sending reports when device isn't ready
    }


    static uint32_t last_mount_check = 0;
    uint32_t current_time = to_ms_since_boot(get_absolute_time());
    if (current_time - last_mount_check > 1000)
    { // Check every second
        if (!tud_mounted())
        {
            return; // Device was unmounted, don't send reports
        }
        last_mount_check = current_time;
    }

    switch (report_id)
    {

    case REPORT_ID_MOUSE:

        if (!connection_state.mouse_connected)
        {

            if (tud_hid_ready())
            {
                static bool prev_button_state = true; // true = not pressed (active low)
                bool current_button_state = gpio_get(PIN_BUTTON);

                if (!current_button_state)
                { // button pressed (active low)

                    tud_hid_mouse_report(REPORT_ID_MOUSE, MOUSE_BUTTON_NONE,
                                         MOUSE_NO_MOVEMENT, MOUSE_BUTTON_MOVEMENT_DELTA,
                                         MOUSE_NO_MOVEMENT, MOUSE_NO_MOVEMENT);
                }
                else if (prev_button_state != current_button_state)
                {

                    tud_hid_mouse_report(REPORT_ID_MOUSE, MOUSE_BUTTON_NONE,
                                         MOUSE_NO_MOVEMENT, MOUSE_NO_MOVEMENT,
                                         MOUSE_NO_MOVEMENT, MOUSE_NO_MOVEMENT);
                }

                prev_button_state = current_button_state;
            }
        }
        break;

    case REPORT_ID_CONSUMER_CONTROL:
    {

        if (tud_hid_ready())
        {
            static const uint16_t empty_key = 0;
            tud_hid_report(REPORT_ID_CONSUMER_CONTROL, &empty_key, HID_CONSUMER_CONTROL_SIZE);
        }
        break;
    }

    default:
        break;
    }
}

void hid_host_task(void)
{


}


void tud_mount_cb(void)
{
    led_set_blink_interval(LED_BLINK_MOUNTED_MS);
    neopixel_update_status();
}

void tud_umount_cb(void)
{
    led_set_blink_interval(LED_BLINK_UNMOUNTED_MS);


    usb_error_tracker.consecutive_device_errors++;

    neopixel_update_status();
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    led_set_blink_interval(LED_BLINK_SUSPENDED_MS);
    neopixel_update_status();
}

void tud_resume_cb(void)
{
    led_set_blink_interval(LED_BLINK_RESUMED_MS);
    neopixel_update_status();
}


void tuh_mount_cb(uint8_t dev_addr)
{

    neopixel_trigger_usb_connection_flash();
    neopixel_update_status();
}

void tuh_umount_cb(uint8_t dev_addr)
{


    handle_device_disconnection(dev_addr);


    static uint32_t last_unmount_time = 0;
    uint32_t current_time = to_ms_since_boot(get_absolute_time());

    if (current_time - last_unmount_time < 5000)
    { // Less than 5 seconds since last unmount
        usb_error_tracker.consecutive_host_errors++;

    }
    else
    {

        usb_error_tracker.consecutive_host_errors = 0;
    }
    last_unmount_time = current_time;


    neopixel_trigger_usb_disconnection_flash();
    neopixel_update_status();
}


void tuh_hid_mount_cb(uint8_t dev_addr, uint8_t instance, const uint8_t *desc_report, uint16_t desc_len)
{
    uint16_t vid, pid;
    tuh_vid_pid_get(dev_addr, &vid, &pid);


    fetch_device_string_descriptors(dev_addr);

    set_attached_device_vid_pid(vid, pid);


    host_mouse_has_report_id = false;
    host_mouse_report_id = 0;
    if (desc_report != NULL && desc_len > 0)
    {
        size_t copy_len = desc_len;
        if (copy_len > sizeof(host_mouse_desc))
            copy_len = sizeof(host_mouse_desc);
        memcpy(host_mouse_desc, desc_report, copy_len);
        host_mouse_desc_len = copy_len;


        for (size_t i = 0; i + 1 < host_mouse_desc_len; ++i)
        {
            if (host_mouse_desc[i] == 0x85)
            {
                host_mouse_has_report_id = true;
                host_mouse_report_id = host_mouse_desc[i + 1];
                break;
            }
        }


        build_runtime_hid_report_with_mouse(host_mouse_desc, host_mouse_desc_len);
    }
    else
    {
        host_mouse_desc_len = 0;
    }

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);




    handle_hid_device_connection(dev_addr, itf_protocol);


    if (!tuh_hid_receive_report(dev_addr, instance))
    {

        neopixel_trigger_usb_disconnection_flash();
    }
    else
    {

        neopixel_update_status();
    }
    neopixel_update_status();
}

void tuh_hid_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    (void)instance; // Suppress unused parameter warning


    reset_device_string_descriptors();


    handle_device_disconnection(dev_addr);


    neopixel_trigger_usb_disconnection_flash();
    neopixel_update_status();
}

void tuh_hid_report_received_cb(uint8_t dev_addr, uint8_t instance, const uint8_t *report, uint16_t len)
{

    if (report == NULL || len == 0)
    {
        tuh_hid_receive_report(dev_addr, instance);
        return;
    }

    uint8_t const itf_protocol = tuh_hid_interface_protocol(dev_addr, instance);


    switch (itf_protocol)
    {

    case HID_ITF_PROTOCOL_MOUSE:

        if (len > 0)
        {

            hid_mouse_report_t mouse_report_local = {0};

            if (len == 8)
            {


                mouse_report_local.buttons = report[0];


                int16_t x16 = (int16_t)(report[4] | (report[5] << 8));
                int16_t y16 = (int16_t)(report[6] | (report[7] << 8));









                mouse_report_local.x = (x16 > 127) ? 127 : ((x16 < -128) ? -128 : (int8_t)x16);
                mouse_report_local.y = (y16 > 127) ? 127 : ((y16 < -128) ? -128 : (int8_t)y16);



                int8_t wheel = 0;
                if (report[1] != 0)
                    wheel = (int8_t)report[1];
                else if (report[2] != 0)
                    wheel = (int8_t)report[2];
                else if (report[3] != 0)
                    wheel = (int8_t)report[3];

                mouse_report_local.wheel = wheel;
                mouse_report_local.pan = 0;
            }
            else
            {

                size_t copy_sz = (len < sizeof(mouse_report_local)) ? len : sizeof(mouse_report_local);
                memcpy(&mouse_report_local, report, copy_sz);
            }



            process_mouse_report(&mouse_report_local);
        }
        break;

    default:

        break;
    }


    tuh_hid_receive_report(dev_addr, instance);
}


uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, const uint8_t *buffer, uint16_t bufsize)
{
    (void)instance;
}

void tud_hid_report_complete_cb(uint8_t instance, const uint8_t *report, uint16_t len)
{
    (void)instance;
    (void)len;
    (void)report;
}

bool usb_device_stack_reset(void)
{
    neopixel_trigger_usb_reset_pending();

    const uint32_t start_time = to_ms_since_boot(get_absolute_time());


    if (!usb_device_initialized)
    {
        return true;
    }





    usb_error_tracker.device_errors = 0;
    usb_error_tracker.consecutive_device_errors = 0;
    usb_error_tracker.device_error_state = false;

    const uint32_t elapsed_time = to_ms_since_boot(get_absolute_time()) - start_time;
    (void)elapsed_time; // suppressed timing log

    return true; // Return success to prevent repeated attempts
}

bool usb_host_stack_reset(void)
{
#if PIO_USB_AVAILABLE
    neopixel_trigger_usb_reset_pending();

    const uint32_t start_time = to_ms_since_boot(get_absolute_time());


    if (!usb_host_initialized)
    {
        return true;
    }





    memset(&connection_state, 0, sizeof(connection_state));


    usb_error_tracker.host_errors = 0;
    usb_error_tracker.consecutive_host_errors = 0;
    usb_error_tracker.host_error_state = false;

    const uint32_t elapsed_time = to_ms_since_boot(get_absolute_time()) - start_time;
    (void)elapsed_time; // suppressed timing log

    return true; // Return success to prevent repeated attempts
#else
    (void)0; // suppressed log
    return false;
#endif
}

bool usb_stacks_reset(void)
{
    neopixel_trigger_usb_reset_pending();

    const bool device_success = usb_device_stack_reset();
    const bool host_success = usb_host_stack_reset();
    const bool overall_success = device_success && host_success;

    if (overall_success)
    {
        neopixel_trigger_usb_reset_success();
    }
    else
    {
        neopixel_trigger_usb_reset_failed();
    }

    return overall_success;
}

void usb_stack_error_check(void)
{
    const uint32_t current_time = to_ms_since_boot(get_absolute_time());


    if (current_time - usb_error_tracker.last_error_check_time < USB_ERROR_CHECK_INTERVAL_MS)
    {
        return;
    }
    usb_error_tracker.last_error_check_time = current_time;



    const bool device_healthy = tud_ready(); // Remove mount requirement as device can be functional without being mounted
    if (!device_healthy)
    {
        usb_error_tracker.consecutive_device_errors++;
        if (!usb_error_tracker.device_error_state)
        {

        }
    }
    else
    {
        if (usb_error_tracker.consecutive_device_errors > 0)
        {

        }
        usb_error_tracker.consecutive_device_errors = 0;
        usb_error_tracker.device_error_state = false;
    }


    if (usb_error_tracker.consecutive_device_errors >= USB_STACK_ERROR_THRESHOLD)
    {
        if (!usb_error_tracker.device_error_state)
        {

            usb_error_tracker.device_error_state = true;
        }
    }

#if PIO_USB_AVAILABLE
    if (usb_error_tracker.consecutive_host_errors >= USB_STACK_ERROR_THRESHOLD)
    {
        if (!usb_error_tracker.host_error_state)
        {

            usb_error_tracker.host_error_state = true;
        }
    }
#endif
}




uint8_t const *tud_descriptor_device_cb(void)
{
    static tusb_desc_device_t desc_device;

    desc_device = (tusb_desc_device_t){
        .bLength = sizeof(tusb_desc_device_t),
        .bDescriptorType = TUSB_DESC_DEVICE,
        .bcdUSB = 0x0200,
        .bDeviceClass = 0x00,
        .bDeviceSubClass = 0x00,
        .bDeviceProtocol = 0x00,
        .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,

        .idVendor = (get_attached_vid() != 0) ? get_attached_vid() : USB_VENDOR_ID,
        .idProduct = (get_attached_pid() != 0) ? get_attached_pid() : USB_PRODUCT_ID,
        .bcdDevice = 0x0100,

        .iManufacturer = 0x01,
        .iProduct = 0x02,
        .iSerialNumber = attached_has_serial ? 0x03 : 0x00,

        .bNumConfigurations = 0x01};

    return (uint8_t const *)&desc_device;
}


uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    if (desc_hid_runtime_valid)
    {
        return desc_hid_report_runtime;
    }

    return desc_hid_report_runtime; // still points to buffer (may contain defaults)
}


enum
{
    ITF_NUM_HID,
    ITF_NUM_TOTAL
};

uint8_t const desc_configuration[] =
    {

        TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, USB_CONFIG_POWER_MA),


        TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE, sizeof(desc_hid_report), EPNUM_HID, CFG_TUD_HID_EP_BUFSIZE, HID_POLLING_INTERVAL_MS)};


uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index; // for multiple configurations
    return desc_configuration;
}


char const *string_desc_arr[] =
    {
        (const char[]){USB_LANGUAGE_ENGLISH_US_BYTE1, USB_LANGUAGE_ENGLISH_US_BYTE2}, // 0: is supported language is English (0x0409)
        MANUFACTURER_STRING,                                                          // 1: Manufacturer
        PRODUCT_STRING,                                                               // 2: Product
        serial_string                                                                 // 3: Serial number from unique chip ID
};

static uint16_t _desc_str[MAX_STRING_DESCRIPTOR_CHARS + 1];


static uint8_t convert_string_to_utf16(const char *str, uint16_t *desc_str)
{
    if (str == NULL || desc_str == NULL)
    {
        return 0;
    }


    uint8_t chr_count = strlen(str);
    if (chr_count > MAX_STRING_DESCRIPTOR_CHARS)
    {
        chr_count = MAX_STRING_DESCRIPTOR_CHARS;
    }


    for (uint8_t i = 0; i < chr_count; i++)
    {
        desc_str[STRING_DESC_FIRST_CHAR_OFFSET + i] = str[i];
    }

    return chr_count;
}


uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;

    uint8_t chr_count;

    if (index == BUFFER_FIRST_ELEMENT_INDEX)
    {
        memcpy(&_desc_str[STRING_DESC_FIRST_CHAR_OFFSET],
               string_desc_arr[BUFFER_FIRST_ELEMENT_INDEX],
               STRING_DESC_HEADER_SIZE);
        chr_count = STRING_DESC_CHAR_COUNT_INIT;
    }
    else
    {



        if (!(index < sizeof(string_desc_arr) / sizeof(string_desc_arr[BUFFER_FIRST_ELEMENT_INDEX])))
        {
            return NULL;
        }

        const char *str = string_desc_arr[index];
        if (str == NULL)
        {
            return NULL;
        }


        if (string_descriptors_fetched)
        {
            switch (index)
            {
            case STRING_DESC_MANUFACTURER_IDX:
                str = attached_manufacturer;
                break;
            case STRING_DESC_PRODUCT_IDX:
                str = attached_product;
                break;
            case STRING_DESC_SERIAL_IDX:
                if (attached_has_serial && strlen(attached_serial) > 0)
                {
                    str = attached_serial;
                }
                else
                {
                    str = get_dynamic_serial_string();
                }
                break;
            default:

                break;
            }
        }
        else
        {

            if (index == STRING_DESC_SERIAL_IDX)
            {
                str = get_dynamic_serial_string();
            }
        }


        chr_count = convert_string_to_utf16(str, _desc_str);
    }


    _desc_str[BUFFER_FIRST_ELEMENT_INDEX] = (TUSB_DESC_STRING << STRING_DESC_TYPE_SHIFT) |
                                            (STRING_DESC_LENGTH_MULTIPLIER * chr_count + STRING_DESC_HEADER_SIZE);

    return _desc_str;
}