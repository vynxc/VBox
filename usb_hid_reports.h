/*
 * Hurricane PIOKMbox Firmware
 */

#ifndef USB_HID_REPORTS_H
#define USB_HID_REPORTS_H

#include <stdbool.h>
#include "usb_hid_types.h"
#include "class/hid/hid.h"

// Report processing functions
void process_kbd_report(const hid_keyboard_report_t* report);
void process_mouse_report(const hid_mouse_report_t* report);

// Utility functions
bool find_key_in_report(const hid_keyboard_report_t* report, uint8_t keycode);

#endif // USB_HID_REPORTS_H