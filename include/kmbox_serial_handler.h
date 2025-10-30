/*
 * KMBox Serial Command Handler Header
 */

#ifndef KMBOX_SERIAL_HANDLER_H
#define KMBOX_SERIAL_HANDLER_H

#include <stdbool.h>
#include <stdint.h>
#include "defines.h"

#ifdef __cplusplus
extern "C" {
#endif


void kmbox_serial_init(void);


void kmbox_serial_task(void);


bool kmbox_send_mouse_report(void);

#ifdef __cplusplus
}
#endif

#endif // KMBOX_SERIAL_HANDLER_H
