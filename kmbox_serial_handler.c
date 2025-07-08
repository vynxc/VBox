/*
 * KMBox Serial Command Handler
 * Integrates kmbox-commands library with the PIOKMBox firmware
 * Uses dedicated UART1 for KMBox serial input (separate from debug UART0)
 */

#include "kmbox_serial_handler.h"
#include "lib/kmbox-commands/kmbox_commands.h"
#include "usb_hid.h"
#include "led_control.h"
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include <stdio.h>

// Ring buffer for non-blocking UART reception
// Use power-of-2 size for efficient modulo operation
#define UART_RX_BUFFER_SIZE 256
#define UART_RX_BUFFER_MASK (UART_RX_BUFFER_SIZE - 1)
static volatile uint8_t uart_rx_buffer[UART_RX_BUFFER_SIZE];
static volatile uint16_t uart_rx_head = 0;
static volatile uint16_t uart_rx_tail = 0;

// UART RX interrupt handler for high-performance non-blocking reception
static void on_uart_rx(void) {
    while (uart_is_readable(KMBOX_UART)) {
        uint8_t ch = uart_getc(KMBOX_UART);
        
        // Calculate next head position using bitwise AND (faster than modulo)
        uint16_t next_head = (uart_rx_head + 1) & UART_RX_BUFFER_MASK;
        
        // Store character if buffer not full
        if (next_head != uart_rx_tail) {
            uart_rx_buffer[uart_rx_head] = ch;
            uart_rx_head = next_head;
        }
        // If buffer full, oldest data is discarded
    }
}

// Get character from ring buffer (non-blocking)
static int uart_rx_getchar(void) {
    if (uart_rx_head == uart_rx_tail) {
        return -1; // Buffer empty
    }
    
    uint8_t ch = uart_rx_buffer[uart_rx_tail];
    uart_rx_tail = (uart_rx_tail + 1) & UART_RX_BUFFER_MASK;
    return ch;
}

// Initialize the serial handler
void kmbox_serial_init(void)
{
    // Initialize UART1 for KMBox serial input
    uart_init(KMBOX_UART, KMBOX_UART_BAUDRATE);
    
    // Set up GPIO pins for UART1
    gpio_set_function(KMBOX_UART_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(KMBOX_UART_RX_PIN, GPIO_FUNC_UART);
    
    // Enable UART FIFOs for better performance
    uart_set_fifo_enabled(KMBOX_UART, true);
    
    // Set up UART RX interrupt for non-blocking reception
    int uart_irq = (KMBOX_UART == uart0) ? UART0_IRQ : UART1_IRQ;
    irq_set_exclusive_handler(uart_irq, on_uart_rx);
    irq_set_enabled(uart_irq, true);
    
    // Enable UART RX interrupt
    uart_set_irq_enables(KMBOX_UART, true, false);
    
    // Initialize the kmbox commands module
    kmbox_commands_init();
    
    printf("KMBox serial handler initialized on UART1 (TX: GPIO%d, RX: GPIO%d) @ %d baud\n",
           KMBOX_UART_TX_PIN, KMBOX_UART_RX_PIN, KMBOX_UART_BAUDRATE);
}

// Process any available serial input
void kmbox_serial_task(void)
{
    // Get current time
    uint32_t current_time_ms = to_ms_since_boot(get_absolute_time());
    
    // Process all available characters from the ring buffer
    int c;
    while ((c = uart_rx_getchar()) != -1) {
        // Process the character
        kmbox_process_serial_char((char)c, current_time_ms);
    }
    
    // Update button states (handles timing for releases)
    kmbox_update_states(current_time_ms);
}

// Send mouse report with kmbox button states
bool kmbox_send_mouse_report(void)
{
    // Check if USB is ready
    if (!tud_hid_ready()) {
        return false;
    }
    
    // Get the report data from kmbox commands
    uint8_t buttons;
    int8_t x, y, wheel, pan;
    kmbox_get_mouse_report(&buttons, &x, &y, &wheel, &pan);
    
    // Send the report using TinyUSB
    bool success = tud_hid_mouse_report(REPORT_ID_MOUSE, buttons, x, y, wheel, pan);
    
    if (success) {
        // Trigger rainbow effect periodically when KMBox commands are processed
        static uint32_t rainbow_counter = 0;
        if (++rainbow_counter % 50 == 0) {
            neopixel_trigger_rainbow_effect();
        }
    }
    
    return success;
}
