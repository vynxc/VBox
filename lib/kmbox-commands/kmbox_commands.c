/*
 * KMBox Commands Library Implementation
 * Handles serial command parsing and custom HID report generation
 */

#include "kmbox_commands.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>





#define RELEASE_MIN_TIME_MS 125
#define RELEASE_MAX_TIME_MS 175
#define CLICK_PRESS_MIN_TIME_MS 75
#define CLICK_PRESS_MAX_TIME_MS 125


static const char* button_names[KMBOX_BUTTON_COUNT] = {
    "left",
    "right", 
    "middle",
    "side1",
    "side2"
};





static const char* lock_button_names[KMBOX_BUTTON_COUNT] = {
    "ml",    // Left button
    "mr",    // Right button
    "mm",    // Middle button
    "ms1",   // Side button 1
    "ms2"    // Side button 2
};





static kmbox_state_t g_kmbox_state; // zero-initialized by default (static storage)
static kmbox_parser_t g_parser;     // zero-initialized by default (static storage)







typedef struct {
    int16_t dx;
    int16_t dy;
    uint32_t t_ms;
} movement_event_t;



#define KMBOX_MOV_HISTORY_SIZE 256
static movement_event_t g_mov_history[KMBOX_MOV_HISTORY_SIZE];
static uint16_t g_mov_head = 0;   // Next write position
static uint16_t g_mov_count = 0;  // Number of valid entries

static void record_movement_event(int16_t dx, int16_t dy, uint32_t now_ms)
{

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






static uint32_t g_rand_seed = 0x12345678;

static uint32_t get_random_release_time(void)
{

    g_rand_seed = (g_rand_seed * 1103515245 + 12345) & 0x7FFFFFFF;
    

    uint32_t range = RELEASE_MAX_TIME_MS - RELEASE_MIN_TIME_MS + 1;
    uint32_t random_offset = (g_rand_seed >> 16) % range;
    
    return RELEASE_MIN_TIME_MS + random_offset;
}

static uint32_t get_random_click_press_time(void)
{

    g_rand_seed = (g_rand_seed * 1103515245 + 12345) & 0x7FFFFFFF;
    

    uint32_t range = CLICK_PRESS_MAX_TIME_MS - CLICK_PRESS_MIN_TIME_MS + 1;
    uint32_t random_offset = (g_rand_seed >> 16) % range;
    
    return CLICK_PRESS_MIN_TIME_MS + random_offset;
}





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

        btn_state->is_pressed = true;
        btn_state->is_forced = true;
        btn_state->release_time = 0; // Indefinite press
        btn_state->is_clicking = false; // Cancel any ongoing click
    } else {

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
    

    btn_state->is_clicking = true;
    btn_state->is_pressed = true;
    btn_state->is_forced = true;
    

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





static void send_button_state_callback(uint8_t button_state)
{


    printf("km.%c\r\n>>> ", button_state);
}





static void parse_command(const char* cmd, uint32_t current_time_ms)
{
















    bool is_km = (strncmp(cmd, "km.", 3) == 0);
    bool is_alias_move = (cmd[0] == 'm' && cmd[1] == '(');
    if (!is_km && !is_alias_move) {
        return;
    }


    if (strncmp(cmd + 3, "catch_xy(", 9) == 0) {
        const char* num_start = cmd + 12; // Skip "km.catch_xy("
        char* num_end;
        long duration = strtol(num_start, &num_end, 10);

        if (*num_end != ')') {
            return;
        }
        if (duration < 0) duration = 0;
        if (duration > 1000) duration = 1000;

        uint32_t now = current_time_ms;
        uint32_t since = now - (uint32_t)duration;
        int32_t sx = 0, sy = 0;
        sum_movement_since(since, now, &sx, &sy);


        printf("(%ld, %ld)\r\n>>> ", (long)sx, (long)sy);
        return;
    }


    printf("%s%.*s", cmd, g_parser.terminator_len, g_parser.command_terminator);
    

    if ((is_km && strncmp(cmd + 3, "move(", 5) == 0) || is_alias_move) {

        const char* args_start = is_alias_move ? (cmd + 2) : (cmd + 8); // Skip "m(" or "km.move("
        const char* comma_pos = strchr(args_start, ',');
        if (!comma_pos) {
            return;
        }
        

        char x_str[16];
        size_t x_len = comma_pos - args_start;
        if (x_len >= sizeof(x_str)) {
            return;
        }
        strncpy(x_str, args_start, x_len);
        x_str[x_len] = '\0';


        {
            const char *p = x_str;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '\0') {
                return; // x missing; require explicit 0 per spec
            }
        }
        

        const char* y_start = comma_pos + 1;
        while (*y_start && isspace((unsigned char)*y_start)) {
            y_start++;
        }
        

        const char* paren_end = strchr(y_start, ')');
        if (!paren_end) {
            return;
        }
        

        char y_str[16];
        size_t y_len = paren_end - y_start;
        if (y_len >= sizeof(y_str)) {
            return;
        }
        strncpy(y_str, y_start, y_len);
        y_str[y_len] = '\0';


        {
            const char *p = y_str;
            while (*p && isspace((unsigned char)*p)) p++;
            if (*p == '\0') {
                return; // y missing; require explicit 0 per spec
            }
        }
        

        int x_amount = atoi(x_str);
        int y_amount = atoi(y_str);
        

        kmbox_add_mouse_movement(x_amount, y_amount);
        

        printf(">>> ");
        return;
    }
    

    if (strncmp(cmd + 3, "wheel(", 6) == 0) {

        const char* num_start = cmd + 9; // Skip "km.wheel("
        char* num_end;
        long wheel_amount = strtol(num_start, &num_end, 10);
        

        if (*num_end != ')') {
            return;
        }
        

        kmbox_add_wheel_movement((int8_t)wheel_amount);
        

        printf(">>> ");
        return;
    }
    

    if (strncmp(cmd + 3, "lock_mx(", 8) == 0) {

        const char* arg_start = cmd + 11; // Skip "km.lock_mx("
        const char* paren_end = strchr(arg_start, ')');
        
        if (!paren_end) {
            return;
        }
        

        size_t arg_len = paren_end - arg_start;
        if (arg_len == 0) {

            printf("%d\r\n>>> ", g_kmbox_state.lock_mx ? 1 : 0);
            return;
        }
        

        char state_str[8];
        if (arg_len >= sizeof(state_str)) {
            return;
        }
        
        strncpy(state_str, arg_start, arg_len);
        state_str[arg_len] = '\0';
        

        int state = atoi(state_str);
        if (state != 0 && state != 1) {
            return; // Invalid state
        }
        

        g_kmbox_state.lock_mx = (state == 1);
        

        printf(">>> ");
        return;
    }
    

    if (strncmp(cmd + 3, "lock_my(", 8) == 0) {

        const char* arg_start = cmd + 11; // Skip "km.lock_my("
        const char* paren_end = strchr(arg_start, ')');
        
        if (!paren_end) {
            return;
        }
        

        size_t arg_len = paren_end - arg_start;
        if (arg_len == 0) {

            printf("%d\r\n>>> ", g_kmbox_state.lock_my ? 1 : 0);
            return;
        }
        

        char state_str[8];
        if (arg_len >= sizeof(state_str)) {
            return;
        }
        
        strncpy(state_str, arg_start, arg_len);
        state_str[arg_len] = '\0';
        

        int state = atoi(state_str);
        if (state != 0 && state != 1) {
            return; // Invalid state
        }
        

        g_kmbox_state.lock_my = (state == 1);
        

        printf(">>> ");
        return;
    }
    

    if (strncmp(cmd + 3, "click(", 6) == 0) {

        const char* num_start = cmd + 9; // Skip "km.click("
        char* num_end;
        long button_num = strtol(num_start, &num_end, 10);
        

        if (*num_end != ')' || button_num < 0 || button_num >= KMBOX_BUTTON_COUNT) {
            return;
        }
        

        start_button_click((kmbox_button_t)button_num, current_time_ms);
        

        printf(">>> ");
        return;
    }
    

    if (strncmp(cmd + 3, "buttons(", 8) == 0) {

        const char* arg_start = cmd + 11; // Skip "km.buttons("
        const char* paren_end = strchr(arg_start, ')');
        
        if (!paren_end) {
            return;
        }
        

        size_t arg_len = paren_end - arg_start;
        if (arg_len == 0) {

            printf("%d\r\n>>> ", g_kmbox_state.button_callback_enabled ? 1 : 0);
            return;
        }
        

        char state_str[8];
        if (arg_len >= sizeof(state_str)) {
            return;
        }
        
        strncpy(state_str, arg_start, arg_len);
        state_str[arg_len] = '\0';
        

        int state = atoi(state_str);
        if (state != 0 && state != 1) {
            return; // Invalid state
        }
        

        g_kmbox_state.button_callback_enabled = (state == 1);
        

        printf(">>> ");
        return;
    }
    

    if (strncmp(cmd + 3, "lock_", 5) == 0) {

        const char* lock_cmd_start = cmd + 8; // Skip "km.lock_"
        

        const char* paren_start = strchr(lock_cmd_start, '(');
        if (!paren_start) {
            return;
        }
        

        const char* paren_end = strchr(paren_start, ')');
        if (!paren_end) {
            return;
        }
        

        size_t button_name_len = paren_start - lock_cmd_start;
        if (button_name_len >= 16) {
            return;
        }
        
        char button_name[16];
        strncpy(button_name, lock_cmd_start, button_name_len);
        button_name[button_name_len] = '\0';
        

        kmbox_button_t button = parse_lock_button_name(button_name);
        if (button == KMBOX_BUTTON_COUNT) {
            return; // Invalid button name
        }
        

        size_t arg_len = paren_end - paren_start - 1;
        if (arg_len == 0) {

            bool is_locked = get_button_lock(button);
            printf("%d\r\n>>> ", is_locked ? 1 : 0);
            return;
        }
        

        char state_str[8];
        if (arg_len >= sizeof(state_str)) {
            return;
        }
        
        strncpy(state_str, paren_start + 1, arg_len);
        state_str[arg_len] = '\0';
        

        int state = atoi(state_str);
        if (state != 0 && state != 1) {
            return; // Invalid state
        }
        

        set_button_lock(button, state == 1);
        

        printf(">>> ");
        return;
    }
    


    const char* paren_start = strchr(cmd + 3, '(');
    if (!paren_start) {
        return;
    }
    

    const char* paren_end = strchr(paren_start, ')');
    if (!paren_end) {
        return;
    }
    

    size_t button_name_len = paren_start - (cmd + 3);
    if (button_name_len >= 16) { // Reasonable limit for button name
        return;
    }
    
    char button_name[16];
    strncpy(button_name, cmd + 3, button_name_len);
    button_name[button_name_len] = '\0';
    

    char state_str[8];
    size_t state_len = paren_end - paren_start - 1;
    if (state_len >= sizeof(state_str)) {
        return;
    }
    
    strncpy(state_str, paren_start + 1, state_len);
    state_str[state_len] = '\0';
    

    kmbox_button_t button = parse_button_name(button_name);
    if (button == KMBOX_BUTTON_COUNT) {
        return; // Invalid button name
    }


    {
        const char *p = state_str;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0') {

            int pressed = g_kmbox_state.buttons[button].is_pressed ? 1 : 0;
            printf("%d\r\n>>> ", pressed);
            return;
        }
    }


    int state = atoi(state_str);
    if (state != 0 && state != 1) {
        return; // Invalid state
    }
    

    set_button_state(button, state == 1, current_time_ms);


    printf(">>> ");
}





void kmbox_commands_init(void)
{

    memset(&g_kmbox_state, 0, sizeof(g_kmbox_state));
    memset(&g_parser, 0, sizeof(g_parser));
    


    g_rand_seed = 0x12345678;
    

    g_kmbox_state.button_callback_enabled = false;
    g_kmbox_state.last_button_state = 0;
    

    printf("KMBox initialized - lock_mx=%d, lock_my=%d\n", 
           g_kmbox_state.lock_mx ? 1 : 0, g_kmbox_state.lock_my ? 1 : 0);
}

void kmbox_process_serial_char(char c, uint32_t current_time_ms)
{

    if (c == '\n' || c == '\r') {

        if (g_parser.buffer_pos > 0 && !g_parser.skip_next_terminator) {

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
        } else if (g_parser.skip_next_terminator) {

            if (g_parser.last_terminator == '\r' && c == '\n') {

                g_parser.command_terminator[1] = '\n';
                g_parser.terminator_len = 2;
                g_parser.skip_next_terminator = false;
            } else {

                g_parser.skip_next_terminator = false;

                if (g_parser.buffer_pos > 0) {

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
    

    g_parser.skip_next_terminator = false;
    

    if (g_parser.buffer_pos < KMBOX_CMD_BUFFER_SIZE - 1) {
        g_parser.buffer[g_parser.buffer_pos++] = c;
        

        if (!g_parser.in_command && g_parser.buffer_pos >= 3) {
            if (strncmp(g_parser.buffer, "km.", 3) == 0) {
                g_parser.in_command = true;
            }
        }
    } else {

        g_parser.buffer_pos = 0;
        g_parser.in_command = false;
    }
}





void kmbox_process_serial_line(const char *line, size_t len, const char *terminator, uint8_t term_len, uint32_t current_time_ms)
{
    if (len == 0 || !line) return;


    size_t copy_len = (len >= KMBOX_CMD_BUFFER_SIZE) ? (KMBOX_CMD_BUFFER_SIZE - 1) : len;


    memcpy(g_parser.buffer, line, copy_len);
    g_parser.buffer[copy_len] = '\0';
    g_parser.buffer_pos = (uint8_t)copy_len;


    if (terminator && term_len > 0) {
        size_t tl = (term_len > 2) ? 2 : term_len;
        memcpy(g_parser.command_terminator, terminator, tl);
        g_parser.terminator_len = (uint8_t)tl;
    } else {
        g_parser.terminator_len = 0;
    }


    parse_command(g_parser.buffer, current_time_ms);


    g_parser.buffer_pos = 0;
    g_parser.in_command = false;
    g_parser.skip_next_terminator = false;
}

void kmbox_update_states(uint32_t current_time_ms)
{
    g_kmbox_state.last_update_time = current_time_ms;
    


    

    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_LEFT];
        

        if (btn->is_clicking) {
            if (current_time_ms >= btn->click_end_time) {

                btn->is_clicking = false;
                btn->is_forced = false;
                btn->click_release_start = 0;
                btn->click_end_time = 0;
                

                bool physical_pressed = (g_kmbox_state.physical_buttons & 0x01) != 0;
                btn->is_pressed = physical_pressed;
            } else if (current_time_ms >= btn->click_release_start) {

                btn->is_pressed = false;
            }

        }

        else if (btn->is_forced && !btn->is_pressed && btn->release_time > 0) {
            if (current_time_ms >= btn->release_time) {

                btn->is_forced = false;
                btn->release_time = 0;
                

                if (!btn->is_locked) {
                    bool physical_pressed = (g_kmbox_state.physical_buttons & 0x01) != 0;
                    btn->is_pressed = physical_pressed;
                }
            }
        }

        else if (!btn->is_forced && !btn->is_locked) {
            bool physical_pressed = (g_kmbox_state.physical_buttons & 0x01) != 0;
            btn->is_pressed = physical_pressed;
        }
    }
    

    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_RIGHT];
        

        if (btn->is_clicking) {
            if (current_time_ms >= btn->click_end_time) {

                btn->is_clicking = false;
                btn->is_forced = false;
                btn->click_release_start = 0;
                btn->click_end_time = 0;
                

                bool physical_pressed = (g_kmbox_state.physical_buttons & 0x02) != 0;
                btn->is_pressed = physical_pressed;
            } else if (current_time_ms >= btn->click_release_start) {

                btn->is_pressed = false;
            }

        }

        else if (btn->is_forced && !btn->is_pressed && btn->release_time > 0) {
            if (current_time_ms >= btn->release_time) {

                btn->is_forced = false;
                btn->release_time = 0;
                

                if (!btn->is_locked) {
                    bool physical_pressed = (g_kmbox_state.physical_buttons & 0x02) != 0;
                    btn->is_pressed = physical_pressed;
                }
            }
        }

        else if (!btn->is_forced && !btn->is_locked) {
            bool physical_pressed = (g_kmbox_state.physical_buttons & 0x02) != 0;
            btn->is_pressed = physical_pressed;
        }
    }
    

    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_MIDDLE];
        

        if (btn->is_clicking) {
            if (current_time_ms >= btn->click_end_time) {

                btn->is_clicking = false;
                btn->is_forced = false;
                btn->click_release_start = 0;
                btn->click_end_time = 0;
                

                bool physical_pressed = (g_kmbox_state.physical_buttons & 0x04) != 0;
                btn->is_pressed = physical_pressed;
            } else if (current_time_ms >= btn->click_release_start) {

                btn->is_pressed = false;
            }

        }

        else if (btn->is_forced && !btn->is_pressed && btn->release_time > 0) {
            if (current_time_ms >= btn->release_time) {

                btn->is_forced = false;
                btn->release_time = 0;
                

                if (!btn->is_locked) {
                    bool physical_pressed = (g_kmbox_state.physical_buttons & 0x04) != 0;
                    btn->is_pressed = physical_pressed;
                }
            }
        }

        else if (!btn->is_forced && !btn->is_locked) {
            bool physical_pressed = (g_kmbox_state.physical_buttons & 0x04) != 0;
            btn->is_pressed = physical_pressed;
        }
    }
    

    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_SIDE1];
        

        if (btn->is_clicking) {
            if (current_time_ms >= btn->click_end_time) {

                btn->is_clicking = false;
                btn->is_forced = false;
                btn->click_release_start = 0;
                btn->click_end_time = 0;
                

                bool physical_pressed = (g_kmbox_state.physical_buttons & 0x08) != 0;
                btn->is_pressed = physical_pressed;
            } else if (current_time_ms >= btn->click_release_start) {

                btn->is_pressed = false;
            }

        }

        else if (btn->is_forced && !btn->is_pressed && btn->release_time > 0) {
            if (current_time_ms >= btn->release_time) {

                btn->is_forced = false;
                btn->release_time = 0;
                

                if (!btn->is_locked) {
                    bool physical_pressed = (g_kmbox_state.physical_buttons & 0x08) != 0;
                    btn->is_pressed = physical_pressed;
                }
            }
        }

        else if (!btn->is_forced && !btn->is_locked) {
            bool physical_pressed = (g_kmbox_state.physical_buttons & 0x08) != 0;
            btn->is_pressed = physical_pressed;
        }
    }
    

    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_SIDE2];
        

        if (btn->is_clicking) {
            if (current_time_ms >= btn->click_end_time) {

                btn->is_clicking = false;
                btn->is_forced = false;
                btn->click_release_start = 0;
                btn->click_end_time = 0;
                

                bool physical_pressed = (g_kmbox_state.physical_buttons & 0x10) != 0;
                btn->is_pressed = physical_pressed;
            } else if (current_time_ms >= btn->click_release_start) {

                btn->is_pressed = false;
            }

        }

        else if (btn->is_forced && !btn->is_pressed && btn->release_time > 0) {
            if (current_time_ms >= btn->release_time) {

                btn->is_forced = false;
                btn->release_time = 0;
                

                if (!btn->is_locked) {
                    bool physical_pressed = (g_kmbox_state.physical_buttons & 0x10) != 0;
                    btn->is_pressed = physical_pressed;
                }
            }
        }

        else if (!btn->is_forced && !btn->is_locked) {
            bool physical_pressed = (g_kmbox_state.physical_buttons & 0x10) != 0;
            btn->is_pressed = physical_pressed;
        }
    }
    

    if (g_kmbox_state.button_callback_enabled) {

        uint8_t current_button_state = 
            (g_kmbox_state.buttons[KMBOX_BUTTON_LEFT].is_pressed   ? 0x01 : 0) |
            (g_kmbox_state.buttons[KMBOX_BUTTON_RIGHT].is_pressed  ? 0x02 : 0) |
            (g_kmbox_state.buttons[KMBOX_BUTTON_MIDDLE].is_pressed ? 0x04 : 0) |
            (g_kmbox_state.buttons[KMBOX_BUTTON_SIDE1].is_pressed  ? 0x08 : 0) |
            (g_kmbox_state.buttons[KMBOX_BUTTON_SIDE2].is_pressed  ? 0x10 : 0);
        

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
    


    uint8_t button_byte = 
        (g_kmbox_state.buttons[KMBOX_BUTTON_LEFT].is_pressed   ? 0x01 : 0) |
        (g_kmbox_state.buttons[KMBOX_BUTTON_RIGHT].is_pressed  ? 0x02 : 0) |
        (g_kmbox_state.buttons[KMBOX_BUTTON_MIDDLE].is_pressed ? 0x04 : 0) |
        (g_kmbox_state.buttons[KMBOX_BUTTON_SIDE1].is_pressed  ? 0x08 : 0) |
        (g_kmbox_state.buttons[KMBOX_BUTTON_SIDE2].is_pressed  ? 0x10 : 0);
    

    *buttons = button_byte;
    


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
    

    *wheel = g_kmbox_state.wheel_accumulator;
    g_kmbox_state.wheel_accumulator = 0;
    
    *pan = 0;  // No pan movement from commands
}

bool kmbox_has_forced_buttons(void)
{


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
    


    

    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_LEFT];
        if (!btn->is_forced && !btn->is_locked) {
            btn->is_pressed = (physical_buttons & 0x01) != 0;
        }
    }
    

    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_RIGHT];
        if (!btn->is_forced && !btn->is_locked) {
            btn->is_pressed = (physical_buttons & 0x02) != 0;
        }
    }
    

    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_MIDDLE];
        if (!btn->is_forced && !btn->is_locked) {
            btn->is_pressed = (physical_buttons & 0x04) != 0;
        }
    }
    

    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_SIDE1];
        if (!btn->is_forced && !btn->is_locked) {
            btn->is_pressed = (physical_buttons & 0x08) != 0;
        }
    }
    

    {
        button_state_t* btn = &g_kmbox_state.buttons[KMBOX_BUTTON_SIDE2];
        if (!btn->is_forced && !btn->is_locked) {
            btn->is_pressed = (physical_buttons & 0x10) != 0;
        }
    }
}

void kmbox_add_mouse_movement(int16_t x, int16_t y)
{

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




    record_movement_event(ax, ay, g_kmbox_state.last_update_time);
}

void kmbox_add_wheel_movement(int8_t wheel)
{
    g_kmbox_state.wheel_accumulator += wheel;
    

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