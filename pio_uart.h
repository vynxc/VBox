#ifndef PIO_UART_H
#define PIO_UART_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

bool pio_uart_init(uint32_t baud);
void pio_uart_deinit(void);
bool pio_uart_tx_blocking(const uint8_t *buf, size_t len);
bool pio_uart_is_initialized(void);
// DMA-backed TX and RX
// Blocking DMA TX helper
bool pio_uart_tx_dma(const uint8_t *buf, size_t len);

// Register a callback that receives incoming bytes from RX DMA blocks.
// The callback is invoked from IRQ context; it should copy data out quickly.
typedef void (*pio_uart_rx_cb_t)(const uint8_t *data, size_t len);
void pio_uart_set_rx_callback(pio_uart_rx_cb_t cb);

// Attach an existing ring buffer so RX DMA writes directly into it.
// buffer: pointer to the byte array (power-of-two size required)
// head_ptr/tail_ptr: pointers to the volatile indices managed by the caller
// buf_size: size of buffer in bytes (must be power-of-two, >= 64)
// Returns true on success, false on invalid parameters or if PIO UART not initialized.
bool pio_uart_attach_rx_ringbuffer(volatile uint8_t *buffer, volatile uint16_t *head_ptr, volatile uint16_t *tail_ptr, size_t buf_size);

#endif // PIO_UART_H
