/*
 * Hurricane PIOKMbox Firmware
 */

#ifndef USB_HID_STATS_H
#define USB_HID_STATS_H

#include "usb_hid_types.h"

// Performance monitoring functions
void print_hid_connection_status(void);
void get_hid_stats(hid_stats_t* stats_out);
void reset_hid_stats(void);

#endif // USB_HID_STATS_H