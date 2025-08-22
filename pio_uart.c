#include "pio_uart.h"
#include "defines.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
// Generated PIO header
#include "pio_uart.pio.h"

// Minimal PIO UART implementation wrapper.
// - Uses pio1 SM1 for RX and SM2 for TX by default
// - Provides init which returns true on success, false on failure
// - Provides a blocking tx helper (uses pio_sm_put_blocking)

// We include a tiny PIO UART program using the SDK helper macro
// The project already generates ws2812.pio.h; for simplicity we build a tiny assembler here.

// PIO assembly for a basic UART RX/TX (8N1) is non-trivial to inline safely here.
// For now provide only the integration scaffolding and a simple loopback test helper.

static PIO pio = pio1;
static const uint RX_SM = 1;
static const uint TX_SM = 2;
static bool pio_inited = false;
// DMA channels (avoid 0 which PIO USB may use)
static int dma_ch_tx = -1;
static int dma_ch_rx = -1;
// RX callback
static pio_uart_rx_cb_t rx_cb = NULL;
// RX buffer for DMA transfers
// double-buffer for RX DMA so we can process half/full blocks
static uint8_t rx_dma_buffer0[256];
static uint8_t rx_dma_buffer1[256];
static const size_t RX_DMA_BLOCK_SIZE = sizeof(rx_dma_buffer0);
// track which buffer is currently armed for DMA
static volatile int rx_active_buf = 0;

// Track last configured transfer size so IRQ handler can advance the ring head
static size_t last_configured_rx_xfer = 0;
// Track the last write address configured for the RX DMA so IRQ can compute
// how many bytes were written by inspecting the DMA HW write_addr register.
static uint32_t last_configured_write_addr = 0;

// Optional direct ringbuffer attachment (to avoid intermediate copy)
static volatile uint8_t *attached_ring_buf = NULL;
static volatile uint16_t *attached_ring_head = NULL;
static volatile uint16_t *attached_ring_tail = NULL;
static size_t attached_ring_size = 0;
static uint16_t attached_ring_mask = 0;

// forward declaration of IRQ handler
static void dma_rx_irq_handler(void);

bool pio_uart_init(uint32_t baud)
{
    // Compute clkdiv for the desired baud using system clock and 8x oversampling
    const float system_clock_hz = (float)clock_get_hz(clk_sys);
    const float oversample = 8.0f;
    float clkdiv = system_clock_hz / (baud * oversample);

    // Claim SMs
    pio_sm_claim(pio, RX_SM);
    pio_sm_claim(pio, TX_SM);

    // Add programs and get offsets
    uint tx_offset = pio_add_program(pio, &uart_tx_program);
    uint rx_offset = pio_add_program(pio, &uart_rx_program);

    // Configure TX SM
    pio_sm_config tx_cfg = uart_tx_program_get_default_config(tx_offset);
    sm_config_set_out_pins(&tx_cfg, KMBOX_UART_TX_PIN, 1);
    // set pin directions for TX SM
    pio_sm_set_consecutive_pindirs(pio, TX_SM, KMBOX_UART_TX_PIN, 1, true);
    sm_config_set_clkdiv(&tx_cfg, clkdiv);
    pio_sm_init(pio, TX_SM, tx_offset, &tx_cfg);
    pio_sm_set_enabled(pio, TX_SM, true);

    // Configure RX SM
    pio_sm_config rx_cfg = uart_rx_program_get_default_config(rx_offset);
    sm_config_set_in_pins(&rx_cfg, KMBOX_UART_RX_PIN);
    // set pin directions for RX SM
    pio_sm_set_consecutive_pindirs(pio, RX_SM, KMBOX_UART_RX_PIN, 1, false);
    sm_config_set_clkdiv(&rx_cfg, clkdiv);
    pio_sm_init(pio, RX_SM, rx_offset, &rx_cfg);
    pio_sm_set_enabled(pio, RX_SM, true);

    // Setup DMA channels (avoid channel 0 used by PIO USB)
    dma_ch_tx = dma_claim_unused_channel(true);
    dma_ch_rx = dma_claim_unused_channel(true);

    if (dma_ch_rx >= 0) {
        dma_channel_config c = dma_channel_get_default_config(dma_ch_rx);
        channel_config_set_read_increment(&c, false); // read from PIO RX FIFO
        channel_config_set_write_increment(&c, true); // write to RAM
        channel_config_set_dreq(&c, pio_get_dreq(pio, RX_SM, false));

    // configure for one-shot block transfer into buffer0 initially
    dma_channel_configure(dma_ch_rx, &c, rx_dma_buffer0, &pio->rxf[RX_SM], RX_DMA_BLOCK_SIZE, false);
    last_configured_rx_xfer = RX_DMA_BLOCK_SIZE;
    last_configured_write_addr = (uint32_t)rx_dma_buffer0;
        // enable IRQ for this channel
        dma_channel_set_irq0_enabled(dma_ch_rx, true);
        irq_set_exclusive_handler(DMA_IRQ_0, dma_rx_irq_handler);
        irq_set_enabled(DMA_IRQ_0, true);
        // start the first transfer
        dma_start_channel_mask(1u << dma_ch_rx);
    } else {
        // no RX DMA available, leave RX to PIO polling (not implemented)
    }

    pio_inited = true;
    return true;
}

void pio_uart_deinit(void)
{
    if (!pio_inited) return;
    // Unclaim SMs
    pio_sm_unclaim(pio, RX_SM);
    pio_sm_unclaim(pio, TX_SM);
    // release DMA channels if claimed
    if (dma_ch_tx >= 0) dma_channel_unclaim(dma_ch_tx);
    if (dma_ch_rx >= 0) dma_channel_unclaim(dma_ch_rx);
    pio_inited = false;
}

bool pio_uart_tx_blocking(const uint8_t *buf, size_t len)
{
    if (!pio_inited) return false;
    // If DMA TX available, use it
    if (dma_ch_tx >= 0) {
        // Configure DMA transfer from buffer to PIO TX FIFO
        dma_channel_config c = dma_channel_get_default_config(dma_ch_tx);
        channel_config_set_read_increment(&c, true);
        channel_config_set_write_increment(&c, false);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
        channel_config_set_dreq(&c, pio_get_dreq(pio, TX_SM, true));
        dma_channel_configure(dma_ch_tx, &c, &pio->txf[TX_SM], buf, len, true);
        // busy-wait until transfer complete
        dma_channel_wait_for_finish_blocking(dma_ch_tx);
        return true;
    }

    // Fallback: push into PIO TX FIFO using blocking API (fast enough for moderate bursts)
    for (size_t i = 0; i < len; ++i) {
        // pio_sm_put_blocking writes 32-bit words to OSR; our program uses pull/out 8
        // so place the byte into TX FIFO as a 32-bit value
        pio_sm_put_blocking(pio, TX_SM, (uint32_t)buf[i]);
    }
    return true;
}

bool pio_uart_is_initialized(void)
{
    return pio_inited;
}

bool pio_uart_tx_dma(const uint8_t *buf, size_t len)
{
    return pio_uart_tx_blocking(buf, len);
}

void pio_uart_set_rx_callback(pio_uart_rx_cb_t cb)
{
    rx_cb = cb;
}

bool pio_uart_attach_rx_ringbuffer(volatile uint8_t *buffer, volatile uint16_t *head_ptr, volatile uint16_t *tail_ptr, size_t buf_size)
{
    if (!pio_inited || dma_ch_rx < 0) return false;
    if (buf_size < 64) return false;
    if ((buf_size & (buf_size - 1)) != 0) return false; // must be power of two

    attached_ring_buf = buffer;
    attached_ring_head = head_ptr;
    attached_ring_tail = tail_ptr;
    attached_ring_size = buf_size;
    attached_ring_mask = (uint16_t)(buf_size - 1);

    // Configure DMA in write-address ring mode so it continuously writes into
    // the provided ring buffer without per-chunk reconfiguration. Use the
    // SDK ring helper to wrap writes at power-of-two buffer size.
    dma_channel_abort(dma_ch_rx);
    dma_channel_config c = dma_channel_get_default_config(dma_ch_rx);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(pio, RX_SM, false));
    // enable ring on write_addr with shift = log2(buf_size)
    channel_config_set_ring(&c, true, __builtin_ctz(attached_ring_size));

    // Start a long-running transfer; the DMA will wrap write_addr automatically.
    const uint32_t long_count = 0xFFFF;
    dma_channel_configure(dma_ch_rx, &c, (void *)attached_ring_buf, &pio->rxf[RX_SM], long_count, false);
    last_configured_rx_xfer = long_count;
    last_configured_write_addr = (uint32_t)attached_ring_buf;
    dma_channel_set_irq0_enabled(dma_ch_rx, true);
    dma_start_channel_mask(1u << dma_ch_rx);

    return true;
}

// DMA IRQ handler for RX
static void dma_rx_irq_handler(void)
{
    // clear IRQ
    dma_hw->ints0 = 1u << dma_ch_rx;

    if (attached_ring_buf && attached_ring_head && attached_ring_tail) {
        // Read current DMA write pointer and compute delta since last check.
        uint32_t cur_write = dma_channel_hw_addr(dma_ch_rx)->write_addr;
        uint32_t written = cur_write - last_configured_write_addr;
        if (written == 0) return;

        // Cap to buffer size (safety)
        if (written > attached_ring_size) written = attached_ring_size;

        // Advance the ring head by written bytes
        uint16_t advance = (uint16_t)written;
        uint16_t head = *attached_ring_head;
        head = (head + advance) & attached_ring_mask;
        *attached_ring_head = head;

        // Check for overflow (consumer too slow). Compute unread count.
        uint16_t cont = head & attached_ring_mask;
        uint16_t tail = *attached_ring_tail & attached_ring_mask;
        size_t unread = (cont >= tail) ? (cont - tail) : (attached_ring_size - (tail - cont));
        if (unread >= attached_ring_size - 1) {
            // Buffer full; abort DMA to avoid overwriting. Consumer must drain
            // and re-attach the DMA (or we could restart it here).
            dma_channel_abort(dma_ch_rx);
            return;
        }

        // Update last write pointer for next IRQ
        last_configured_write_addr = cur_write;
        return;
    }

    // Legacy double-buffer fallback
    uint8_t *filled = (rx_active_buf == 0) ? rx_dma_buffer0 : rx_dma_buffer1;
    if (rx_cb) {
        rx_cb(filled, RX_DMA_BLOCK_SIZE);
    }

    // swap active buffer and re-arm DMA to write into the other buffer
    rx_active_buf = (rx_active_buf == 0) ? 1 : 0;
    uint8_t *next = (rx_active_buf == 0) ? rx_dma_buffer0 : rx_dma_buffer1;
    // reconfigure the DMA channel to write into 'next' and restart
    dma_channel_set_read_addr(dma_ch_rx, &pio->rxf[RX_SM], false);
    dma_channel_set_write_addr(dma_ch_rx, next, false);
    dma_channel_set_trans_count(dma_ch_rx, RX_DMA_BLOCK_SIZE, true);
}
