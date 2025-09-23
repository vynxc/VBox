/*
 * KMBox Commands Library Implementation
 * Handles serial command parsing and custom HID report generation
 */

#include "kmbox_commands.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

//--------------------------------------------------------------------+
// Constants
//--------------------------------------------------------------------+

#define RELEASE_MIN_TIME_MS 125
#define RELEASE_MAX_TIME_MS 175
#define CLICK_PRESS_MIN_TIME_MS 75
#define CLICK_PRESS_MAX_TIME_MS 125

// Button name strings
static const char* button_names[KMBOX_BUTTON_COUNT] = {
    "left",
    "right", 
    "middle",
    "side1",
    "side2"
};

// Button bit masks for HID report - now handled inline for performance
// 0x01, 0x02, 0x04, 0x08, 0x10 for LEFT, RIGHT, MIDDLE, SIDE1, SIDE2

// Lock button name strings (short form for commands)
static const char* lock_button_names[KMBOX_BUTTON_COUNT] = {
    "ml",    // Left button
    "mr",    // Right button
    "mm",    // Middle button
    "ms1",   // Side button 1
    "ms2"    // Side button 2
};

//--------------------------------------------------------------------+
// Static Variables
//--------------------------------------------------------------------+

static kmbox_state_t g_kmbox_state; // zero-initialized by default (static storage)
static kmbox_parser_t g_parser;     // zero-initialized by default (static storage)

//--------------------------------------------------------------------+
// Movement History for catch_xy
//--------------------------------------------------------------------+

// Keep a short ring buffer of recent movement deltas with timestamps (ms)
// to support km.catch_xy(duration) queries up to 1000ms.
typedef struct {
    int16_t dx;
    int16_t dy;
    uint32_t t_ms;
} movement_event_t;

// Choose a reasonable size; even at high USB poll rates, 256 entries
// comfortably covers >1000ms of movement samples.
#define KMBOX_MOV_HISTORY_SIZE 256
static movement_event_t g_mov_history[KMBOX_MOV_HISTORY_SIZE];
static uint16_t g_mov_head = 0;   // Next write position
static uint16_t g_mov_count = 0;  // Number of valid entries

static void record_movement_event(int16_t dx, int16_t dy, uint32_t now_ms)
{
    // Ignore zero deltas to reduce noise
    if (dx == 0 && dy == 0) return;

    g_mov_history[g_mov_head].dx = dx;
    g_mov_history[g_mov_head].dy = dy;
    g_mov_history[g_mov_head].t_ms = now_ms;
    g_mov_head = (uint16_t)((g_mov_head + 1) % KMBOX_MOV_HISTORY_SIZE);
    if (g_mov_count < KMBOX_MOV_HISTORY_SIZE) {
        g_mov_count++;
    }
}

static void sum_movement_since(uint32_t since_ms, uint32_t now_ms, int32_t *out_x, int32_t *out_y)
{
    (void)now_ms; // currently unused; kept for future precision improvements
    int32_t sx = 0, sy = 0;
    if (g_mov_count == 0) {
        *out_x = 0; *out_y = 0; return;
    }
    // Iterate newest->oldest until entries are older than window
    int idx = (int)g_mov_head - 1;
    if (idx < 0) idx = KMBOX_MOV_HISTORY_SIZE - 1;
    uint16_t remaining = g_mov_count;
    while (remaining--) {
        const movement_event_t *ev = &g_mov_history[idx];
        if (ev->t_ms < since_ms) break; // older than window
        sx += ev->dx;
        sy += ev->dy;
        idx--;
        if (idx < 0) idx = KMBOX_MOV_HISTORY_SIZE - 1;
    }
    *out_x = sx;
    *out_y = sy;
}

//--------------------------------------------------------------------+
// Random Number Generation
//--------------------------------------------------------------------+

// Simple linear congruential generator for random release times
static uint32_t g_rand_seed = 0x12345678;

static uint32_t get_random_release_time(void)
{
    // Update seed
    g_rand_seed = (g_rand_seed * 1103515245 + 12345) & 0x7FFFFFFF;
    
    // Generate random value between RELEASE_MIN_TIME_MS and RELEASE_MAX_TIME_MS
    uint32_t range = RELEASE_MAX_TIME_MS - RELEASE_MIN_TIME_MS + 1;
    uint32_t random_offset = (g_rand_seed >> 16) % range;
    
    return RELEASE_MIN_TIME_MS + random_offset;
}

static uint32_t get_random_click_press_time(void)
{
    // Update seed
    g_rand_seed = (g_rand_seed * 1103515245 + 12345) & 0x7FFFFFFF;
    
    // Generate random value between CLICK_PRESS_MIN_TIME_MS and CLICK_PRESS_MAX_TIME_MS
    uint32_t range = CLICK_PRESS_MAX_TIME_MS - CLICK_PRESS_MIN_TIME_MS + 1;
    uint32_t random_offset = (g_rand_seed >> 16) % range;
    
    return CLICK_PRESS_MIN_TIME_MS + random_offset;
}

//--------------------------------------------------------------------+
// Button Management
//--------------------------------------------------------------------+

static kmbox_button_t parse_button_name(const char* name)
{
    for (int i = 0; i < KMBOX_BUTTON_COUNT; i++) {
        if (strcmp(name, button_names[i]) == 0) {
            return (kmbox_button_t)i;
        }
    }
    return KMBOX_BUTTON_COUNT; // Invalid button
}

static kmbox_button_t parse_lock_button_name(const char* name)
{
    for (int i = 0; i < KMBOX_BUTTON_COUNT; i++) {
        if (strcmp(name, lock_button_names[i]) == 0) {
            return (kmbox_button_t)i;
        }
    }
    return KMBOX_BUTTON_COUNT; // Invalid button
}

static void set_button_state(kmbox_button_t button, bool pressed, uint32_t current_time_ms)
{
    if (button >= KMBOX_BUTTON_COUNT) {
        return;
    }
    
    button_state_t* btn_state = &g_kmbox_state.buttons[button];
    
    if (pressed) {
        // Force button press
        btn_state->is_pressed = true;
        btn_state->is_forced = true;
        btn_state->release_time = 0; // Indefinite press
        btn_state->is_clicking = false; // Cancel any ongoing click
    } else {
        // Force button release for random duration
        if (btn_state->is_forced && btn_state->is_pressed) {
            btn_state->is_pressed = false;
            btn_state->release_time = current_time_ms + get_random_release_time();
            btn_state->is_clicking = false; // Cancel any ongoing click
        }
    }
}

static void start_button_click(kmbox_button_t button, uint32_t current_time_ms)
{
    if (button >= KMBOX_BUTTON_COUNT) {
        return;
    }
    
    button_state_t* btn_state = &g_kmbox_state.buttons[button];
    
    // Start click sequence
    btn_state->is_clicking = true;
    btn_state->is_pressed = true;
    btn_state->is_forced = true;
    
    // Calculate click timing
    uint32_t press_duration = get_random_click_press_time();
    uint32_t release_duration = get_random_release_time();
    
    btn_state->click_release_start = current_time_ms + press_duration;
    btn_state->click_end_time = btn_state->click_release_start + release_duration;
    btn_state->release_time = 0; // Not used during click
}

static void set_button_lock(kmbox_button_t button, bool locked)
{
    if (button >= KMBOX_BUTTON_COUNT) {
        return;
    }
    
    g_kmbox_state.buttons[button].is_locked = locked;
}

static bool get_button_lock(kmbox_button_t button)
{
    if (button >= KMBOX_BUTTON_COUNT) {
        return false;
    }
    
    return g_kmbox_state.buttons[button].is_locked;
}

//--------------------------------------------------------------------+
// Button State Callback
//--------------------------------------------------------------------+

static void send_button_state_callback(uint8_t button_state)
{
    // Send the callback in the format: km.[button_state]\r\n>>> 
    // where button_state is the raw character representing the bitmap
    printf("km.%c\r\n>>> ", button_state);
}

//--------------------------------------------------------------------+
// Command Parsing
//--------------------------------------------------------------------+

static void parse_command(const char* cmd, uint32_t current_time_ms)
{
    // Expected formats:
    // button_name(state) - Example: left(1) or side2(0)
    // click(button_num) - Example: click(0) for left button
    // buttons() - Get callback state
    // buttons(state) - Enable (1) or disable (0) callback
    // move(x, y) - Move mouse by x,y pixels
    // wheel(amount) - Scroll wheel up (+) or down (-)
    // lock_mx() - Get X axis lock state
    // lock_mx(state) - Set X axis lock (1=locked, 0=unlocked)
    // lock_my() - Get Y axis lock state
    // lock_my(state) - Set Y axis lock (1=locked, 0=unlocked)
    // catch_xy(duration_ms) - Sum x/y input over last duration (0..1000ms)
    // Aliases supported:
    // m(...) -> km.move(...)

    // Determine if this is a standard km.* command or a supported alias
    bool is_km = (strncmp(cmd, "km.", 3) == 0);
    bool is_alias_move = (cmd[0] == 'm' && cmd[1] == '(');
    if (!is_km && !is_alias_move) {
        return;
    }

    // Check if this is a catch_xy command
    if (strncmp(cmd + 3, "catch_xy(", 9) == 0) {
        const char* num_start = cmd + 12; // Skip "km.catch_xy("
        char* num_end;
        long duration = strtol(num_start, &num_end, 10);
        // Validate closing parenthesis (no trailing spaces per other commands)
        if (*num_end != ')') {
            return;
        }
        if (duration < 0) duration = 0;
        if (duration > 1000) duration = 1000;

        uint32_t now = current_time_ms;
        uint32_t since = now - (uint32_t)duration;
        int32_t sx = 0, sy = 0;
        sum_movement_since(since, now, &sx, &sy);

        // Output format: (x, y) then prompt
        printf("(%ld, %ld)\r\n>>> ", (long)sx, (long)sy);
        return;
    }

    // Echo the command back with the original line terminator
    printf("%s%.*s", cmd, g_parser.terminator_len, g_parser.command_terminator);
    
    // Check if this is a move command
    if ((is_km && strncmp(cmd + 3, "move(", 5) == 0) || is_alias_move) {
        // Parse move command
        const char* args_start = is_alias_move ? (cmd + 2) : (cmd + 8); // Skip "m(" or "km.move("
        const char* comma_pos = strchr(args_start, ',');
        if (!comma_pos) {
            return;
        }
        
        // Parse X value
        char x_str[16];
        size_t x_len = comma_pos - args_start;
        if (x_len >= sizeof(x_str)) {
            return;
        }
        strncpy(x_str, args_start, x_len);
        x_str[x_len] = '\0';

        // Ensure X is provided (not blank/whitespace-only)
        {
            const char *p = x_str;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '\0') {
                return; // x missing; require explicit 0 per spec
            }
        }
        
        // Skip whitespace after comma
        const char* y_start = comma_pos + 1;
        while (*y_start && isspace((unsigned char)*y_start)) {
            y_start++;
        }
        
        // Find closing parenthesis
        const char* paren_end = strchr(y_start, ')');
        if (!paren_end) {
            return;
        }
        
        // Parse Y value
        char y_str[16];
        size_t y_len = paren_end - y_start;
        if (y_len >= sizeof(y_str)) {
            return;
        }
        strncpy(y_str, y_start, y_len);
        y_str[y_len] = '\0';

        // Ensure Y is provided (not blank/whitespace-only)
        {
            const char *p = y_str;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '\0') {
                return; // y missing; require explicit 0 per spec
            }
        }
        
        // Convert to integers
        int x_amount = atoi(x_str);
        int y_amount = atoi(y_str);
        
        // Add movement
        kmbox_add_mouse_movement(x_amount, y_amount);
        
        // Send the prompt
        printf(">>> ");
        return;
    }
    
    // Check if this is a wheel command
    if (strncmp(cmd + 3, "wheel(", 6) == 0) {
        // Parse wheel command
        const char* num_start = cmd + 9; // Skip "km.wheel("
        char* num_end;
        long wheel_amount = strtol(num_start, &num_end, 10);
        
        // Validate closing parenthesis
        if (*num_end != ')') {
            return;
        }
        
        // Add wheel movement
        kmbox_add_wheel_movement((int8_t)wheel_amount);
        
        // Send the prompt
        printf(">>> ");
        return;
    }
    
    // Check if this is a lock_mx command
    if (strncmp(cmd + 3, "lock_mx(", 8) == 0) {
        // Parse lock_mx command
        const char* arg_start = cmd + 11; // Skip "km.lock_mx("
        const char* paren_end = strchr(arg_start, ')');
        
        if (!paren_end) {
            return;
        }
        
        // Check if there's an argument
        size_t arg_len = paren_end - arg_start;
        if (arg_len == 0) {
            // No argument - return lock state with result
            printf("%d\r\n>>> ", g_kmbox_state.lock_mx ? 1 : 0);
            return;
        }
        
        // Extract state value
        char state_str[8];
        if (arg_len >= sizeof(state_str)) {
            return;
        }
        
        strncpy(state_str, arg_start, arg_len);
        state_str[arg_len] = '\0';
        
        // Parse state
        int state = atoi(state_str);
        if (state != 0 && state != 1) {
            return; // Invalid state
        }
        
        // Set lock state
        g_kmbox_state.lock_mx = (state == 1);
        
        // Send the prompt
        printf(">>> ");
        return;
    }
    
    // Check if this is a lock_my command
    if (strncmp(cmd + 3, "lock_my(", 8) == 0) {
        // Parse lock_my command
        const char* arg_start = cmd + 11; // Skip "km.lock_my("
        const char* paren_end = strchr(arg_start, ')');
        
        if (!paren_end) {
            return;
        }
        
        // Check if there's an argument
        size_t arg_len = paren_end - arg_start;
        if (arg_len == 0) {
            // No argument - return lock state with result
            printf("%d\r\n>>> ", g_kmbox_state.lock_my ? 1 : 0);
            return;
        }
        
        // Extract state value
        char state_str[8];
        if (arg_len >= sizeof(state_str)) {
            return;
        }
        
        strncpy(state_str, arg_start, arg_len);
        state_str[arg_len] = '\0';
        
        // Parse state
        int state = atoi(state_str);
        if (state != 0 && state != 1) {
            return; // Invalid state
        }
        
        // Set lock state
        g_kmbox_state.lock_my = (state == 1);
        
        // Send the prompt
        printf(">>> ");
        return;
    }
    
    // Check if this is a click command
    if (strncmp(cmd + 3, "click(", 6) == 0) {
        // Parse click command
        const char* num_start = cmd + 9; // Skip "km.click("
        char* num_end;
        long button_num = strtol(num_start, &num_end, 10);
        
        // Validate button number and closing parenthesis
        if (*num_end != ')' || button_num < 0 || button_num >= KMBOX_BUTTON_COUNT) {
            return;
        }
        
        // Start the click sequence
        start_button_click((kmbox_button_t)button_num, current_time_ms);
        
        // Send the prompt
        printf(">>> ");
        return;
    }
    
    // Check if this is a buttons callback command
    if (strncmp(cmd + 3, "buttons(", 8) == 0) {
        // Parse buttons command
        const char* arg_start = cmd + 11; // Skip "km.buttons("
        const char* paren_end = strchr(arg_start, ')');
        
        if (!paren_end) {
            return;
        }
        
        // Check if there's an argument
        size_t arg_len = paren_end - arg_start;
        if (arg_len == 0) {
            // No argument - return callback state with result
            printf("%d\r\n>>> ", g_kmbox_state.button_callback_enabled ? 1 : 0);
            return;
        }
        
        // Extract state value
        char state_str[8];
        if (arg_len >= sizeof(state_str)) {
            return;
        }
        
        strncpy(state_str, arg_start, arg_len);
        state_str[arg_len] = '\0';
        
        // Parse state
        int state = atoi(state_str);
        if (state != 0 && state != 1) {
            return; // Invalid state
        }
        
        // Set callback state
        g_kmbox_state.button_callback_enabled = (state == 1);
        
        // Send the prompt
        printf(">>> ");
        return;
    }
    
    // Check if this is a lock command
    if (strncmp(cmd + 3, "lock_", 5) == 0) {
        // Parse lock command
        const char* lock_cmd_start = cmd + 8; // Skip "km.lock_"
        
        // Find the opening parenthesis
        const char* paren_start = strchr(lock_cmd_start, '(');
        if (!paren_start) {
            return;
        }
        
        // Find the closing parenthesis
        const char* paren_end = strchr(paren_start, ')');
        if (!paren_end) {
            return;
        }
        
        // Extract button name
        size_t button_name_len = paren_start - lock_cmd_start;
        if (button_name_len >= 16) {
            return;
        }
        
        char button_name[16];
        strncpy(button_name, lock_cmd_start, button_name_len);
        button_name[button_name_len] = '\0';
        
        // Parse button
        kmbox_button_t button = parse_lock_button_name(button_name);
        if (button == KMBOX_BUTTON_COUNT) {
            return; // Invalid button name
        }
        
        // Check if there's an argument
        size_t arg_len = paren_end - paren_start - 1;
        if (arg_len == 0) {
            // No argument - return lock state with result
            bool is_locked = get_button_lock(button);
            printf("%d\r\n>>> ", is_locked ? 1 : 0);
            return;
        }
        
        // Extract state value
        char state_str[8];
        if (arg_len >= sizeof(state_str)) {
            return;
        }
        
        strncpy(state_str, paren_start + 1, arg_len);
        state_str[arg_len] = '\0';
        
        // Parse state
        int state = atoi(state_str);
        if (state != 0 && state != 1) {
            return; // Invalid state
        }
        
        // Apply the lock
        set_button_lock(button, state == 1);
        
        // Send the prompt
        printf(">>> ");
        return;
    }
    
    // Parse regular button command
    // Find the opening parenthesis
    const char* paren_start = strchr(cmd + 3, '(');
    if (!paren_start) {
        return;
    }
    
    // Find the closing parenthesis
    const char* paren_end = strchr(paren_start, ')');
    if (!paren_end) {
        return;
    }
    
    // Extract button name
    size_t button_name_len = paren_start - (cmd + 3);
    if (button_name_len >= 16) { // Reasonable limit for button name
        return;
    }
    
    char button_name[16];
    strncpy(button_name, cmd + 3, button_name_len);
    button_name[button_name_len] = '\0';
    
    // Extract state value (may be empty for getter form km.[button]())
    char state_str[8];
    size_t state_len = paren_end - paren_start - 1;
    if (state_len >= sizeof(state_str)) {
        return;
    }
    
    strncpy(state_str, paren_start + 1, state_len);
    state_str[state_len] = '\0';
    
    // Parse button name first
    kmbox_button_t button = parse_button_name(button_name);
    if (button == KMBOX_BUTTON_COUNT) {
        return; // Invalid button name
    }

    // Determine if this is a getter: empty or whitespace-only inside parentheses
    {
        const char *p = state_str;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0') {
            // Getter: print current state (1 pressed, 0 released)
            int pressed = g_kmbox_state.buttons[button].is_pressed ? 1 : 0;
            printf("%d\r\n>>> ", pressed);
            return;
        }
    }

    // Setter path: parse explicit 0/1
    int state = atoi(state_str);
    if (state != 0 && state != 1) {
        return; // Invalid state
    }
    
    // Apply the command
    set_button_state(button, state == 1, current_time_ms);

    // Per spec, echo the command then print the prompt only
    printf(">>> ");
}

//--------------------------------------------------------------------+
// Public API Implementation
//--------------------------------------------------------------------+

void kmbox_commands_init(void)
{
    // Initialize state
    memset(&g_kmbox_state, 0, sizeof(g_kmbox_state));
    memset(&g_parser, 0, sizeof(g_parser));
    
    // Initialize random seed with a better value if available
    // For now, using a fixed seed for reproducibility
    g_rand_seed = 0x12345678;
    
    // Initialize button callback state
    g_kmbox_state.button_callback_enabled = false;
    g_kmbox_state.last_button_state = 0;
    
    // Debug: Log initial axis lock states
    printf("KMBox initialized - lock_mx=%d, lock_my=%d\n", 
           g_kmbox_state.lock_mx ? 1 : 0, g_kmbox_state.lock_my ? 1 : 0);
}

void kmbox_process_serial_char(char c, uint32_t current_time_ms)
{
    // Handle line termination characters
    if (c == '\n' || c == '\r') {
        // Check if we have a command to process
        if (g_parser.buffer_pos > 0 && !g_parser.skip_next_terminator) {
            // Store the terminator for this command
            if (c == '\r') {
                g_parser.command_terminator[0] = '\r';
                g_parser.terminator_len = 1;
                // Check if next char will be \n (for \r\n)
                g_parser.skip_next_terminator = true;
                g_parser.last_terminator = '\r';
            } else {
                g_parser.command_terminator[0] = '\n';
                g_parser.terminator_len = 1;
            }
            
            // Null terminate and process command
            g_parser.buffer[g_parser.buffer_pos] = '\0';
            parse_command(g_parser.buffer, current_time_ms);
            
            // Reset parser
            g_parser.buffer_pos = 0;
            g_parser.in_command = false;
        } else if (g_parser.skip_next_terminator) {
            // Check if this is part of a \r\n sequence
            if (g_parser.last_terminator == '\r' && c == '\n') {
                // This is the \n following a \r, update terminator to \r\n
                g_parser.command_terminator[1] = '\n';
                g_parser.terminator_len = 2;
                g_parser.skip_next_terminator = false;
            } else {
                // This is a new line terminator, not part of \r\n
                g_parser.skip_next_terminator = false;
                // If buffer has content, process it
                if (g_parser.buffer_pos > 0) {
                    // Store the new terminator
                    if (c == '\r') {
                        g_parser.command_terminator[0] = '\r';
                        g_parser.terminator_len = 1;
                        g_parser.skip_next_terminator = true;
                        g_parser.last_terminator = '\r';
                    } else {
                        g_parser.command_terminator[0] = '\n';
                        g_parser.terminator_len = 1;
                    }
                    
                    g_parser.buffer[g_parser.buffer_pos] = '\0';
                    parse_command(g_parser.buffer, current_time_ms);
                    g_parser.buffer_pos = 0;
                    g_parser.in_command = false;
                }
            }
        }
        return;
    }
    
    // Reset skip flag for non-terminator characters
    g_parser.skip_next_terminator = false;
    
    // Add character to buffer if there's space
    if (g_parser.buffer_pos < KMBOX_CMD_BUFFER_SIZE - 1) {
        g_parser.buffer[g_parser.buffer_pos++] = c;
        
        // Check if we're starting a command
        if (!g_parser.in_command && g_parser.buffer_pos >= 3) {
            if (strncmp(g_parser.buffer, "km.", 3) == 0) {
                g_parser.in_command = true;
            }
        }
    } else {
        // Buffer overflow - reset
        g_parser.buffer_pos = 0;
        g_parser.in_command = false;
    }
}

// Accept a complete command line (without trailing terminator characters).
// This helper allows callers to hand over full lines from DMA/ring-buffer
// with minimal per-byte overhead. The function will copy at most
// KMBOX_CMD_BUFFER_SIZE-1 bytes into the parser buffer and call parse_command().
void kmbox_process_serial_line(const char *line, size_t len, const char *terminator, uint8_t term_len, uint32_t current_time_ms)
{
    if (len == 0 || !line) return;

    // Truncate if necessary
    size_t copy_len = (len >= KMBOX_CMD_BUFFER_SIZE) ? (KMBOX_CMD_BUFFER_SIZE - 1) : len;

    // Copy into parser buffer and null-terminate
    memcpy(g_parser.buffer, line, copy_len);
    g_parser.buffer[copy_len] = '\0';
    g_parser.buffer_pos = (uint8_t)copy_len;

    // Store terminator info
    if (terminator && term_len > 0) {
        size_t tl = (term_len > 2) ? 2 : term_len;
        memcpy(g_parser.command_terminator, terminator, tl);
        g_parser.terminator_len = (uint8_t)tl;
    } else {
        g_parser.terminator_len = 0;
    }

    // Process the command
    parse_command(g_parser.buffer, current_time_ms);

    // Reset parser state
    g_parser.buffer_pos = 0;
    g_parser.in_command = false;
    g_parser.skip_next_terminator = false;
}

void kmbox_update_states(uint32_t current_time_ms)
{
    g_kmbox_state.last_update_time = current_time_ms;
    
    // Update each button state - UNROLLED for performance
    // Eliminates loop overhead in this frequently called function
    
    // Button 0 (LEFT)
    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_LEFT];
        
        // Handle click sequence
        if (btn->is_clicking) {
            if (current_time_ms >= btn->click_end_time) {
                // Click sequence complete, return to hardware state
                btn->is_clicking = false;
                btn->is_forced = false;
                btn->click_release_start = 0;
                btn->click_end_time = 0;
                
                // Restore physical button state
                bool physical_pressed = (g_kmbox_state.physical_buttons & 0x01) != 0;
                btn->is_pressed = physical_pressed;
            } else if (current_time_ms >= btn->click_release_start) {
                // In release phase of click
                btn->is_pressed = false;
            }
            // If still in press phase, is_pressed remains true
        }
        // Check if a forced release has expired (for non-click commands)
        else if (btn->is_forced && !btn->is_pressed && btn->release_time > 0) {
            if (current_time_ms >= btn->release_time) {
                // Release time expired, button returns to physical state
                btn->is_forced = false;
                btn->release_time = 0;
                
                // Restore physical button state (unless locked)
                if (!btn->is_locked) {
                    bool physical_pressed = (g_kmbox_state.physical_buttons & 0x01) != 0;
                    btn->is_pressed = physical_pressed;
                }
            }
        }
        // Update non-forced button states from physical state (unless locked)
        else if (!btn->is_forced && !btn->is_locked) {
            bool physical_pressed = (g_kmbox_state.physical_buttons & 0x01) != 0;
            btn->is_pressed = physical_pressed;
        }
    }
    
    // Button 1 (RIGHT)
    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_RIGHT];
        
        // Handle click sequence
        if (btn->is_clicking) {
            if (current_time_ms >= btn->click_end_time) {
                // Click sequence complete, return to hardware state
                btn->is_clicking = false;
                btn->is_forced = false;
                btn->click_release_start = 0;
                btn->click_end_time = 0;
                
                // Restore physical button state
                bool physical_pressed = (g_kmbox_state.physical_buttons & 0x02) != 0;
                btn->is_pressed = physical_pressed;
            } else if (current_time_ms >= btn->click_release_start) {
                // In release phase of click
                btn->is_pressed = false;
            }
            // If still in press phase, is_pressed remains true
        }
        // Check if a forced release has expired (for non-click commands)
        else if (btn->is_forced && !btn->is_pressed && btn->release_time > 0) {
            if (current_time_ms >= btn->release_time) {
                // Release time expired, button returns to physical state
                btn->is_forced = false;
                btn->release_time = 0;
                
                // Restore physical button state (unless locked)
                if (!btn->is_locked) {
                    bool physical_pressed = (g_kmbox_state.physical_buttons & 0x02) != 0;
                    btn->is_pressed = physical_pressed;
                }
            }
        }
        // Update non-forced button states from physical state (unless locked)
        else if (!btn->is_forced && !btn->is_locked) {
            bool physical_pressed = (g_kmbox_state.physical_buttons & 0x02) != 0;
            btn->is_pressed = physical_pressed;
        }
    }
    
    // Button 2 (MIDDLE)
    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_MIDDLE];
        
        // Handle click sequence
        if (btn->is_clicking) {
            if (current_time_ms >= btn->click_end_time) {
                // Click sequence complete, return to hardware state
                btn->is_clicking = false;
                btn->is_forced = false;
                btn->click_release_start = 0;
                btn->click_end_time = 0;
                
                // Restore physical button state
                bool physical_pressed = (g_kmbox_state.physical_buttons & 0x04) != 0;
                btn->is_pressed = physical_pressed;
            } else if (current_time_ms >= btn->click_release_start) {
                // In release phase of click
                btn->is_pressed = false;
            }
            // If still in press phase, is_pressed remains true
        }
        // Check if a forced release has expired (for non-click commands)
        else if (btn->is_forced && !btn->is_pressed && btn->release_time > 0) {
            if (current_time_ms >= btn->release_time) {
                // Release time expired, button returns to physical state
                btn->is_forced = false;
                btn->release_time = 0;
                
                // Restore physical button state (unless locked)
                if (!btn->is_locked) {
                    bool physical_pressed = (g_kmbox_state.physical_buttons & 0x04) != 0;
                    btn->is_pressed = physical_pressed;
                }
            }
        }
        // Update non-forced button states from physical state (unless locked)
        else if (!btn->is_forced && !btn->is_locked) {
            bool physical_pressed = (g_kmbox_state.physical_buttons & 0x04) != 0;
            btn->is_pressed = physical_pressed;
        }
    }
    
    // Button 3 (SIDE1)
    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_SIDE1];
        
        // Handle click sequence
        if (btn->is_clicking) {
            if (current_time_ms >= btn->click_end_time) {
                // Click sequence complete, return to hardware state
                btn->is_clicking = false;
                btn->is_forced = false;
                btn->click_release_start = 0;
                btn->click_end_time = 0;
                
                // Restore physical button state
                bool physical_pressed = (g_kmbox_state.physical_buttons & 0x08) != 0;
                btn->is_pressed = physical_pressed;
            } else if (current_time_ms >= btn->click_release_start) {
                // In release phase of click
                btn->is_pressed = false;
            }
            // If still in press phase, is_pressed remains true
        }
        // Check if a forced release has expired (for non-click commands)
        else if (btn->is_forced && !btn->is_pressed && btn->release_time > 0) {
            if (current_time_ms >= btn->release_time) {
                // Release time expired, button returns to physical state
                btn->is_forced = false;
                btn->release_time = 0;
                
                // Restore physical button state (unless locked)
                if (!btn->is_locked) {
                    bool physical_pressed = (g_kmbox_state.physical_buttons & 0x08) != 0;
                    btn->is_pressed = physical_pressed;
                }
            }
        }
        // Update non-forced button states from physical state (unless locked)
        else if (!btn->is_forced && !btn->is_locked) {
            bool physical_pressed = (g_kmbox_state.physical_buttons & 0x08) != 0;
            btn->is_pressed = physical_pressed;
        }
    }
    
    // Button 4 (SIDE2)
    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_SIDE2];
        
        // Handle click sequence
        if (btn->is_clicking) {
            if (current_time_ms >= btn->click_end_time) {
                // Click sequence complete, return to hardware state
                btn->is_clicking = false;
                btn->is_forced = false;
                btn->click_release_start = 0;
                btn->click_end_time = 0;
                
                // Restore physical button state
                bool physical_pressed = (g_kmbox_state.physical_buttons & 0x10) != 0;
                btn->is_pressed = physical_pressed;
            } else if (current_time_ms >= btn->click_release_start) {
                // In release phase of click
                btn->is_pressed = false;
            }
            // If still in press phase, is_pressed remains true
        }
        // Check if a forced release has expired (for non-click commands)
        else if (btn->is_forced && !btn->is_pressed && btn->release_time > 0) {
            if (current_time_ms >= btn->release_time) {
                // Release time expired, button returns to physical state
                btn->is_forced = false;
                btn->release_time = 0;
                
                // Restore physical button state (unless locked)
                if (!btn->is_locked) {
                    bool physical_pressed = (g_kmbox_state.physical_buttons & 0x10) != 0;
                    btn->is_pressed = physical_pressed;
                }
            }
        }
        // Update non-forced button states from physical state (unless locked)
        else if (!btn->is_forced && !btn->is_locked) {
            bool physical_pressed = (g_kmbox_state.physical_buttons & 0x10) != 0;
            btn->is_pressed = physical_pressed;
        }
    }
    
    // Check if button state has changed and callback is enabled
    if (g_kmbox_state.button_callback_enabled) {
        // Build current button state bitmap - UNROLLED for performance
        uint8_t current_button_state = 
            (g_kmbox_state.buttons[KMBOX_BUTTON_LEFT].is_pressed   ? 0x01 : 0) |
            (g_kmbox_state.buttons[KMBOX_BUTTON_RIGHT].is_pressed  ? 0x02 : 0) |
            (g_kmbox_state.buttons[KMBOX_BUTTON_MIDDLE].is_pressed ? 0x04 : 0) |
            (g_kmbox_state.buttons[KMBOX_BUTTON_SIDE1].is_pressed  ? 0x08 : 0) |
            (g_kmbox_state.buttons[KMBOX_BUTTON_SIDE2].is_pressed  ? 0x10 : 0);
        
        // Send callback if state changed
        if (current_button_state != g_kmbox_state.last_button_state) {
            send_button_state_callback(current_button_state);
            g_kmbox_state.last_button_state = current_button_state;
        }
    }
}

void kmbox_get_mouse_report(uint8_t* buttons, int8_t* x, int8_t* y, int8_t* wheel, int8_t* pan)
{
    if (!buttons || !x || !y || !wheel || !pan) {
        return;
    }
    
    // Build button byte from current states - UNROLLED for performance
    // Direct bit manipulation eliminates loop overhead and enables better optimization
    uint8_t button_byte = 
        (g_kmbox_state.buttons[KMBOX_BUTTON_LEFT].is_pressed   ? 0x01 : 0) |
        (g_kmbox_state.buttons[KMBOX_BUTTON_RIGHT].is_pressed  ? 0x02 : 0) |
        (g_kmbox_state.buttons[KMBOX_BUTTON_MIDDLE].is_pressed ? 0x04 : 0) |
        (g_kmbox_state.buttons[KMBOX_BUTTON_SIDE1].is_pressed  ? 0x08 : 0) |
        (g_kmbox_state.buttons[KMBOX_BUTTON_SIDE2].is_pressed  ? 0x10 : 0);
    
    // Set output values
    *buttons = button_byte;
    
    // Get movement values from accumulators
    // Clamp to int8_t range (-128 to 127)
    if (g_kmbox_state.mouse_x_accumulator > 127) {
        *x = 127;
        g_kmbox_state.mouse_x_accumulator -= 127;
    } else if (g_kmbox_state.mouse_x_accumulator < -128) {
        *x = -128;
        g_kmbox_state.mouse_x_accumulator -= -128;
    } else {
        *x = (int8_t)g_kmbox_state.mouse_x_accumulator;
        g_kmbox_state.mouse_x_accumulator = 0;
    }
    
    if (g_kmbox_state.mouse_y_accumulator > 127) {
        *y = 127;
        g_kmbox_state.mouse_y_accumulator -= 127;
    } else if (g_kmbox_state.mouse_y_accumulator < -128) {
        *y = -128;
        g_kmbox_state.mouse_y_accumulator -= -128;
    } else {
        *y = (int8_t)g_kmbox_state.mouse_y_accumulator;
        g_kmbox_state.mouse_y_accumulator = 0;
    }
    
    // Get wheel value
    *wheel = g_kmbox_state.wheel_accumulator;
    g_kmbox_state.wheel_accumulator = 0;
    
    *pan = 0;  // No pan movement from commands
}

bool kmbox_has_forced_buttons(void)
{
    // Check all buttons for forced state - UNROLLED for performance
    // Direct access eliminates loop overhead
    return g_kmbox_state.buttons[KMBOX_BUTTON_LEFT].is_forced ||
           g_kmbox_state.buttons[KMBOX_BUTTON_RIGHT].is_forced ||
           g_kmbox_state.buttons[KMBOX_BUTTON_MIDDLE].is_forced ||
           g_kmbox_state.buttons[KMBOX_BUTTON_SIDE1].is_forced ||
           g_kmbox_state.buttons[KMBOX_BUTTON_SIDE2].is_forced;
}

const char* kmbox_get_button_name(kmbox_button_t button)
{
    if (button < KMBOX_BUTTON_COUNT) {
        return button_names[button];
    }
    return "unknown";
}

void kmbox_update_physical_buttons(uint8_t physical_buttons)
{
    g_kmbox_state.physical_buttons = physical_buttons;
    
    // Update button states for non-forced, non-locked buttons - UNROLLED for performance
    // Direct bit testing eliminates loop overhead and array indexing
    
    // Button 0 (LEFT)
    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_LEFT];
        if (!btn->is_forced && !btn->is_locked) {
            btn->is_pressed = (physical_buttons & 0x01) != 0;
        }
    }
    
    // Button 1 (RIGHT)
    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_RIGHT];
        if (!btn->is_forced && !btn->is_locked) {
            btn->is_pressed = (physical_buttons & 0x02) != 0;
        }
    }
    
    // Button 2 (MIDDLE)
    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_MIDDLE];
        if (!btn->is_forced && !btn->is_locked) {
            btn->is_pressed = (physical_buttons & 0x04) != 0;
        }
    }
    
    // Button 3 (SIDE1)
    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_SIDE1];
        if (!btn->is_forced && !btn->is_locked) {
            btn->is_pressed = (physical_buttons & 0x08) != 0;
        }
    }
    
    // Button 4 (SIDE2)
    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_SIDE2];
        if (!btn->is_forced && !btn->is_locked) {
            btn->is_pressed = (physical_buttons & 0x10) != 0;
        }
    }
}

void kmbox_add_mouse_movement(int16_t x, int16_t y)
{
    // Apply axis locks
    int16_t ax = 0;
    int16_t ay = 0;
    if (!g_kmbox_state.lock_mx) {
        g_kmbox_state.mouse_x_accumulator += x;
        ax = x;
    }
    if (!g_kmbox_state.lock_my) {
        g_kmbox_state.mouse_y_accumulator += y;
        ay = y;
    }

    // Record actual movement applied (post-lock) using the latest known time
    // from update loop. This may lag slightly for command-initiated moves,
    // but remains accurate within the scheduling tick.
    record_movement_event(ax, ay, g_kmbox_state.last_update_time);
}

void kmbox_add_wheel_movement(int8_t wheel)
{
    g_kmbox_state.wheel_accumulator += wheel;
    
    // Clamp to int8_t range
    if (g_kmbox_state.wheel_accumulator > 127) {
        g_kmbox_state.wheel_accumulator = 127;
    } else if (g_kmbox_state.wheel_accumulator < -128) {
        g_kmbox_state.wheel_accumulator = -128;
    }
}

void kmbox_set_axis_lock(bool lock_x, bool lock_y)
{
    g_kmbox_state.lock_mx = lock_x;
    g_kmbox_state.lock_my = lock_y;
}

bool kmbox_get_lock_mx(void)
{
    return g_kmbox_state.lock_mx;
}

bool kmbox_get_lock_my(void)
{
    return g_kmbox_state.lock_my;
}