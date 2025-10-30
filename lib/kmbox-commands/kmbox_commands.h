/*
 * KMBox Commands Library
 * Handles serial command parsing and custom HID report generation
 */

#ifndef KMBOX_COMMANDS_H
#define KMBOX_COMMANDS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>





typedef enum {
    KMBOX_BUTTON_LEFT = 0,
    KMBOX_BUTTON_RIGHT,
    KMBOX_BUTTON_MIDDLE,
    KMBOX_BUTTON_SIDE1,
    KMBOX_BUTTON_SIDE2,
    KMBOX_BUTTON_COUNT
} kmbox_button_t;





typedef struct {
    bool is_pressed;
    bool is_forced;  // True if button state is forced by command
    uint32_t release_time;  // Time when forced release should end (0 if not active)
    bool is_clicking;  // True if button is in a click sequence
    uint32_t click_release_start;  // Time when click press ends and release starts
    uint32_t click_end_time;  // Time when entire click sequence ends
    bool is_locked;  // True if button is locked (physical input masked from output)
} button_state_t;

typedef struct {
    button_state_t buttons[KMBOX_BUTTON_COUNT];
    uint8_t physical_buttons;  // Actual physical button states
    uint32_t last_update_time;
    bool button_callback_enabled;  // True if button state change callback is enabled
    uint8_t last_button_state;     // Last reported button state for callback
    

    int16_t mouse_x_accumulator;  // Accumulated X movement
    int16_t mouse_y_accumulator;  // Accumulated Y movement
    int8_t wheel_accumulator;      // Accumulated wheel movement
    

    bool lock_mx;  // Lock X axis (left/right movement)
    bool lock_my;  // Lock Y axis (up/down movement)
} kmbox_state_t;





#define KMBOX_CMD_BUFFER_SIZE 64

typedef struct {
    char buffer[KMBOX_CMD_BUFFER_SIZE];
    uint8_t buffer_pos;
    bool in_command;
    bool skip_next_terminator;  // Skip next terminator if it's part of \r\n
    char last_terminator;       // Track last terminator seen ('\r' or '\n')
    char command_terminator[3]; // Store the line terminator(s) used for current command
    uint8_t terminator_len;     // Length of the terminator (1 for \n or \r, 2 for \r\n)
} kmbox_parser_t;






void kmbox_commands_init(void);


void kmbox_process_serial_char(char c, uint32_t current_time_ms);





void kmbox_process_serial_line(const char *line, size_t len, const char *terminator, uint8_t term_len, uint32_t current_time_ms);


void kmbox_update_states(uint32_t current_time_ms);


void kmbox_get_mouse_report(uint8_t* buttons, int8_t* x, int8_t* y, int8_t* wheel, int8_t* pan);


void kmbox_add_mouse_movement(int16_t x, int16_t y);


void kmbox_add_wheel_movement(int8_t wheel);


void kmbox_set_axis_lock(bool lock_x, bool lock_y);


bool kmbox_get_lock_mx(void);
bool kmbox_get_lock_my(void);


bool kmbox_has_forced_buttons(void);


const char* kmbox_get_button_name(kmbox_button_t button);


void kmbox_update_physical_buttons(uint8_t physical_buttons);

#endif // KMBOX_COMMANDS_H