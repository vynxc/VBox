/*
 * KMBox Serial Command Handler Header
 */

#ifndef KMBOX_SERIAL_HANDLER_H
#define KMBOX_SERIAL_HANDLER_H

#include <stdbool.h>
#include <stdint.h>
#include "defines.h"

// Initialize the serial handler
void kmbox_serial_init(void);

// Process any available serial input (call this in main loop)
void kmbox_serial_task(void);

// Send mouse report with kmbox button states
bool kmbox_send_mouse_report(void);

#endif // KMBOX_SERIAL_HANDLER_H