/*
 * KMBox Interface Implementation
 * 
 * UART-only interface implementation
 */

#include "kmbox_interface.h"
#include "pico/stdlib.h"
#include "hardware/uart.h"

#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include <string.h>


const kmbox_uart_config_t KMBOX_UART_DEFAULT_CONFIG = {
    .baudrate = 250000,
    .tx_pin = 4,
    .rx_pin = 5,
    .use_dma = true
};



#define RX_BUFFER_SIZE 2048
#define TX_BUFFER_SIZE 1024
#define RX_BUFFER_MASK (RX_BUFFER_SIZE - 1)
#define TX_BUFFER_MASK (TX_BUFFER_SIZE - 1)


_Static_assert((RX_BUFFER_SIZE & (RX_BUFFER_SIZE - 1)) == 0,
               "RX_BUFFER_SIZE must be a power of two");
_Static_assert((TX_BUFFER_SIZE & (TX_BUFFER_SIZE - 1)) == 0,
               "TX_BUFFER_SIZE must be a power of two");


typedef struct {

    kmbox_interface_config_t config;
    

    uart_inst_t* uart;
    

    uint8_t __attribute__((aligned(RX_BUFFER_SIZE))) rx_buffer[RX_BUFFER_SIZE];
    uint8_t __attribute__((aligned(TX_BUFFER_SIZE))) tx_buffer[TX_BUFFER_SIZE];
    

    volatile uint16_t rx_head;
    volatile uint16_t rx_tail;
    volatile uint16_t tx_head;
    volatile uint16_t tx_tail;
    

    int dma_rx_chan;
    int dma_tx_chan;
    

    kmbox_interface_stats_t stats;
    

    bool initialized;
    bool tx_in_progress;
    

} kmbox_interface_state_t;


static kmbox_interface_state_t g_interface = {
    .dma_rx_chan = -1,
    .dma_tx_chan = -1,
    .initialized = false
};


static bool init_uart(const kmbox_uart_config_t* config);
static void process_uart(void);
static void uart_dma_rx_setup(void);
static void dma_rx_irq_handler(void);


bool kmbox_interface_init(const kmbox_interface_config_t* config)
{
    if (!config || g_interface.initialized) {
        return false;
    }
    

    memset(&g_interface, 0, sizeof(g_interface));
    g_interface.dma_rx_chan = -1;
    g_interface.dma_tx_chan = -1;
    

    g_interface.config = *config;
    

    bool success = false;
    if (config->transport_type != KMBOX_TRANSPORT_UART) {
        return false;
    }
    success = init_uart(&config->config.uart);
    
    if (success) {
        g_interface.initialized = true;
    }
    
    return success;
}


static bool init_uart(const kmbox_uart_config_t* config)
{

    if (config->tx_pin == 0 && config->rx_pin == 1) {
        g_interface.uart = uart0;
    } else if (config->tx_pin == 4 && config->rx_pin == 5) {
        g_interface.uart = uart1;
    } else {
        return false;
    }
    

    uart_init(g_interface.uart, config->baudrate);


    gpio_set_function(config->tx_pin, GPIO_FUNC_UART);
    gpio_set_function(config->rx_pin, GPIO_FUNC_UART);
    

    uart_set_format(g_interface.uart, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(g_interface.uart, true);
    

    if (config->use_dma) {
        uart_dma_rx_setup();
    }
    
    return true;
}




static void uart_dma_rx_setup(void)
{
    g_interface.dma_rx_chan = dma_claim_unused_channel(true);
    
    dma_channel_config c = dma_channel_get_default_config(g_interface.dma_rx_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, uart_get_dreq(g_interface.uart, false));
    channel_config_set_ring(&c, true, __builtin_ctz(RX_BUFFER_SIZE));
    
    dma_channel_configure(
        g_interface.dma_rx_chan,
        &c,
    g_interface.rx_buffer,
    &uart_get_hw(g_interface.uart)->dr,
        0xFFFF,
        true
    );
    
    dma_channel_set_irq1_enabled(g_interface.dma_rx_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_1, dma_rx_irq_handler);
    irq_set_enabled(DMA_IRQ_1, true);
}



static void __not_in_flash_func(dma_rx_irq_handler)(void)
{
    if (g_interface.dma_rx_chan >= 0) {
        dma_hw->ints1 = 1u << g_interface.dma_rx_chan;
        dma_channel_set_trans_count(g_interface.dma_rx_chan, 0xFFFF, true);
    }
}




void kmbox_interface_process(void)
{
    if (!g_interface.initialized) {
        return;
    }
    
    if (g_interface.config.transport_type == KMBOX_TRANSPORT_UART) {
        process_uart();
    }
}


static void process_uart(void)
{
    uint16_t head = g_interface.rx_head;
    uint16_t tail = g_interface.rx_tail;
    

    if (g_interface.config.config.uart.use_dma && g_interface.dma_rx_chan >= 0) {
        uint32_t write_addr = dma_channel_hw_addr(g_interface.dma_rx_chan)->write_addr;
        uint32_t buffer_start = (uint32_t)g_interface.rx_buffer;
        head = (write_addr - buffer_start) & RX_BUFFER_MASK;
    } else {

    while (uart_is_readable(g_interface.uart)) {
            uint16_t next_head = (head + 1) & RX_BUFFER_MASK;
            if (next_head != tail) {
                g_interface.rx_buffer[head] = uart_getc(g_interface.uart);
                head = next_head;
            } else {
                uart_getc(g_interface.uart); // Discard
                g_interface.stats.errors++;
            }
        }
        g_interface.rx_head = head;
    }
    

    while (tail != head) {
        uint16_t chunk_size;
        if (head > tail) {
            chunk_size = head - tail;
        } else {
            chunk_size = RX_BUFFER_SIZE - tail;
        }
        
        if (g_interface.config.on_command_received && chunk_size > 0) {
            g_interface.config.on_command_received(&g_interface.rx_buffer[tail], chunk_size);
            g_interface.stats.bytes_received += chunk_size;
        }
        
        tail = (tail + chunk_size) & RX_BUFFER_MASK;
    }
    
    g_interface.rx_tail = tail;
}




bool kmbox_interface_send(const uint8_t* data, size_t len)
{
    if (!g_interface.initialized || !data || len == 0) {
        return false;
    }
    

    uint16_t head = g_interface.tx_head;
    uint16_t tail = g_interface.tx_tail;
    uint16_t available = (tail - head - 1) & TX_BUFFER_MASK;
    
    if (available < len) {
        g_interface.stats.errors++;
        return false;
    }
    

    size_t first = TX_BUFFER_SIZE - (head & TX_BUFFER_MASK);
    if (first > len) first = len;
    memcpy(&g_interface.tx_buffer[head], data, first);
    if (len > first) {
        memcpy(&g_interface.tx_buffer[0], data + first, len - first);
    }
    head = (head + (uint16_t)len) & TX_BUFFER_MASK;
    
    g_interface.tx_head = head;
    g_interface.stats.bytes_sent += len;
    

    if (!g_interface.tx_in_progress) {

        g_interface.tx_in_progress = true;
    }
    
    return true;
}


bool kmbox_interface_is_ready(void)
{
    if (!g_interface.initialized) {
        return false;
    }
    
    uint16_t head = g_interface.tx_head;
    uint16_t tail = g_interface.tx_tail;
    uint16_t available = (tail - head - 1) & TX_BUFFER_MASK;
    
    return available > 0;
}


void kmbox_interface_get_stats(kmbox_interface_stats_t* stats)
{
    if (stats) {
        *stats = g_interface.stats;
    }
}


void kmbox_interface_deinit(void)
{
    if (!g_interface.initialized) {
        return;
    }
    

    if (g_interface.dma_rx_chan >= 0) {
        dma_channel_abort(g_interface.dma_rx_chan);
        dma_channel_unclaim(g_interface.dma_rx_chan);
    }
    
    if (g_interface.dma_tx_chan >= 0) {
        dma_channel_abort(g_interface.dma_tx_chan);
        dma_channel_unclaim(g_interface.dma_tx_chan);
    }
    

    if (g_interface.config.transport_type == KMBOX_TRANSPORT_UART) {
        uart_deinit(g_interface.uart);
    }
    
    g_interface.initialized = false;
}


kmbox_transport_type_t kmbox_interface_get_transport_type(void)
{
    return g_interface.initialized ? g_interface.config.transport_type : KMBOX_TRANSPORT_NONE;
}