/*
 * KMBox Interface Implementation
 * 
 * Consolidated UART and SPI interface implementation
 */

#include "kmbox_interface.h"
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/gpio.h"
#include <string.h>

// Default configurations
const kmbox_uart_config_t KMBOX_UART_DEFAULT_CONFIG = {
    .baudrate = 250000,
    .tx_pin = 4,
    .rx_pin = 5,
    .use_dma = true
};

const kmbox_spi_config_t KMBOX_SPI_DEFAULT_CONFIG = {
    .baudrate = 1000000,
    .sck_pin = 18,
    .mosi_pin = 19,
    .miso_pin = 16,
    .cs_pin = 17,
    .use_dma = true,
    .is_slave = true
};

// Buffer sizes (must be power of 2)
#define RX_BUFFER_SIZE 512
#define TX_BUFFER_SIZE 256
#define RX_BUFFER_MASK (RX_BUFFER_SIZE - 1)
#define TX_BUFFER_MASK (TX_BUFFER_SIZE - 1)

// Static assertions for buffer sizes
_Static_assert((RX_BUFFER_SIZE & (RX_BUFFER_SIZE - 1)) == 0,
               "RX_BUFFER_SIZE must be a power of two");
_Static_assert((TX_BUFFER_SIZE & (TX_BUFFER_SIZE - 1)) == 0,
               "TX_BUFFER_SIZE must be a power of two");

// Interface state
typedef struct {
    // Configuration
    kmbox_interface_config_t config;
    
    // Transport-specific handles
    union {
        uart_inst_t* uart;
        spi_inst_t* spi;
    } instance;
    
    // Ring buffers
    uint8_t __attribute__((aligned(RX_BUFFER_SIZE))) rx_buffer[RX_BUFFER_SIZE];
    uint8_t __attribute__((aligned(TX_BUFFER_SIZE))) tx_buffer[TX_BUFFER_SIZE];
    
    // Ring buffer indices
    volatile uint16_t rx_head;
    volatile uint16_t rx_tail;
    volatile uint16_t tx_head;
    volatile uint16_t tx_tail;
    
    // DMA channels
    int dma_rx_chan;
    int dma_tx_chan;
    
    // Statistics
    kmbox_interface_stats_t stats;
    
    // State flags
    bool initialized;
    bool tx_in_progress;
    
    // SPI-specific state
    bool cs_active;
    uint32_t cs_timestamp;
} kmbox_interface_state_t;

// Global interface state
static kmbox_interface_state_t g_interface = {
    .dma_rx_chan = -1,
    .dma_tx_chan = -1,
    .initialized = false
};

// Forward declarations
static bool init_uart(const kmbox_uart_config_t* config);
static bool init_spi(const kmbox_spi_config_t* config);
static void process_uart(void);
static void process_spi(void);
static void uart_dma_rx_setup(void);
static void spi_cs_callback(uint gpio, uint32_t events);
static void dma_rx_irq_handler(void);

// Initialize the interface
bool kmbox_interface_init(const kmbox_interface_config_t* config)
{
    if (!config || g_interface.initialized) {
        return false;
    }
    
    // Clear state
    memset(&g_interface, 0, sizeof(g_interface));
    g_interface.dma_rx_chan = -1;
    g_interface.dma_tx_chan = -1;
    
    // Copy configuration
    g_interface.config = *config;
    
    // Initialize based on transport type
    bool success = false;
    switch (config->transport_type) {
        case KMBOX_TRANSPORT_UART:
            success = init_uart(&config->config.uart);
            break;
            
        case KMBOX_TRANSPORT_SPI:
            success = init_spi(&config->config.spi);
            break;
            
        default:
            return false;
    }
    
    if (success) {
        g_interface.initialized = true;
    }
    
    return success;
}

// Initialize UART transport
static bool init_uart(const kmbox_uart_config_t* config)
{
    // Determine UART instance based on pins
    if (config->tx_pin == 0 && config->rx_pin == 1) {
        g_interface.instance.uart = uart0;
    } else if (config->tx_pin == 4 && config->rx_pin == 5) {
        g_interface.instance.uart = uart1;
    } else {
        return false;
    }
    
    // Initialize UART
    uart_init(g_interface.instance.uart, config->baudrate);

    // Configure pins
    gpio_set_function(config->tx_pin, GPIO_FUNC_UART);
    gpio_set_function(config->rx_pin, GPIO_FUNC_UART);
    
    // Set UART format
    uart_set_format(g_interface.instance.uart, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(g_interface.instance.uart, true);
    
    // Setup DMA if enabled
    if (config->use_dma) {
        uart_dma_rx_setup();
    }
    
    return true;
}

// Initialize SPI transport
static bool init_spi(const kmbox_spi_config_t* config)
{
    // Determine SPI instance based on pins
    if (config->sck_pin == 2 || config->sck_pin == 6) {
        g_interface.instance.spi = spi0;
    } else if (config->sck_pin == 10 || config->sck_pin == 14) {
        g_interface.instance.spi = spi1;
    } else {
        return false;
    }
    
    // Initialize SPI
    spi_init(g_interface.instance.spi, config->baudrate);
    spi_set_slave(g_interface.instance.spi, config->is_slave);
    
    // Configure pins
    gpio_set_function(config->sck_pin, GPIO_FUNC_SPI);
    gpio_set_function(config->mosi_pin, GPIO_FUNC_SPI);
    gpio_set_function(config->miso_pin, GPIO_FUNC_SPI);
    
    // Configure CS pin
    if (config->is_slave) {
        gpio_init(config->cs_pin);
        gpio_set_dir(config->cs_pin, GPIO_IN);
        gpio_pull_up(config->cs_pin);
        
        // Setup CS interrupt
        gpio_set_irq_enabled_with_callback(config->cs_pin,
                                           GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                           true,
                                           &spi_cs_callback);
    } else {
        gpio_init(config->cs_pin);
        gpio_set_dir(config->cs_pin, GPIO_OUT);
        gpio_put(config->cs_pin, 1);
    }
    
    return true;
}

// Setup UART DMA
static void uart_dma_rx_setup(void)
{
    g_interface.dma_rx_chan = dma_claim_unused_channel(true);
    
    dma_channel_config c = dma_channel_get_default_config(g_interface.dma_rx_chan);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, uart_get_dreq(g_interface.instance.uart, false));
    channel_config_set_ring(&c, true, __builtin_ctz(RX_BUFFER_SIZE));
    
    dma_channel_configure(
        g_interface.dma_rx_chan,
        &c,
        g_interface.rx_buffer,
        &uart_get_hw(g_interface.instance.uart)->dr,
        0xFFFF,
        true
    );
    
    dma_channel_set_irq1_enabled(g_interface.dma_rx_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_1, dma_rx_irq_handler);
    irq_set_enabled(DMA_IRQ_1, true);
}

// DMA RX IRQ handler
static void dma_rx_irq_handler(void)
{
    if (g_interface.dma_rx_chan >= 0) {
        dma_hw->ints1 = 1u << g_interface.dma_rx_chan;
        dma_channel_set_trans_count(g_interface.dma_rx_chan, 0xFFFF, true);
    }
}

// SPI CS callback
static void spi_cs_callback(uint gpio, uint32_t events)
{
    if (gpio != g_interface.config.config.spi.cs_pin) {
        return;
    }
    
    if (events & GPIO_IRQ_EDGE_FALL) {
        g_interface.cs_active = true;
        g_interface.cs_timestamp = to_ms_since_boot(get_absolute_time());
    } else if (events & GPIO_IRQ_EDGE_RISE) {
        g_interface.cs_active = false;
    }
}

// Process interface tasks
void kmbox_interface_process(void)
{
    if (!g_interface.initialized) {
        return;
    }
    
    switch (g_interface.config.transport_type) {
        case KMBOX_TRANSPORT_UART:
            process_uart();
            break;
            
        case KMBOX_TRANSPORT_SPI:
            process_spi();
            break;
            
        default:
            break;
    }
}

// Process UART data
static void process_uart(void)
{
    uint16_t head = g_interface.rx_head;
    uint16_t tail = g_interface.rx_tail;
    
    // Update head from DMA if using DMA
    if (g_interface.config.config.uart.use_dma && g_interface.dma_rx_chan >= 0) {
        uint32_t write_addr = dma_channel_hw_addr(g_interface.dma_rx_chan)->write_addr;
        uint32_t buffer_start = (uint32_t)g_interface.rx_buffer;
        head = (write_addr - buffer_start) & RX_BUFFER_MASK;
    } else {
        // Non-DMA: read from UART FIFO
        while (uart_is_readable(g_interface.instance.uart)) {
            uint16_t next_head = (head + 1) & RX_BUFFER_MASK;
            if (next_head != tail) {
                g_interface.rx_buffer[head] = uart_getc(g_interface.instance.uart);
                head = next_head;
            } else {
                uart_getc(g_interface.instance.uart); // Discard
                g_interface.stats.errors++;
            }
        }
        g_interface.rx_head = head;
    }
    
    // Process received data
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

// Process SPI data
static void process_spi(void)
{
    // Process any available SPI data
    while (spi_is_readable(g_interface.instance.spi)) {
        uint8_t byte;
        spi_read_blocking(g_interface.instance.spi, 0xFF, &byte, 1);
        
        uint16_t next_head = (g_interface.rx_head + 1) & RX_BUFFER_MASK;
        if (next_head != g_interface.rx_tail) {
            g_interface.rx_buffer[g_interface.rx_head] = byte;
            g_interface.rx_head = next_head;
            g_interface.stats.bytes_received++;
        } else {
            g_interface.stats.errors++;
        }
    }
    
    // Process buffered data
    uint16_t tail = g_interface.rx_tail;
    uint16_t head = g_interface.rx_head;
    
    if (tail != head && g_interface.config.on_command_received) {
        uint16_t chunk_size;
        if (head > tail) {
            chunk_size = head - tail;
        } else {
            chunk_size = RX_BUFFER_SIZE - tail;
        }
        
        g_interface.config.on_command_received(&g_interface.rx_buffer[tail], chunk_size);
        g_interface.rx_tail = (tail + chunk_size) & RX_BUFFER_MASK;
    }
}

// Send data through the interface
bool kmbox_interface_send(const uint8_t* data, size_t len)
{
    if (!g_interface.initialized || !data || len == 0) {
        return false;
    }
    
    // Check available space
    uint16_t head = g_interface.tx_head;
    uint16_t tail = g_interface.tx_tail;
    uint16_t available = (tail - head - 1) & TX_BUFFER_MASK;
    
    if (available < len) {
        g_interface.stats.errors++;
        return false;
    }
    
    // Copy to TX buffer
    for (size_t i = 0; i < len; i++) {
        g_interface.tx_buffer[head] = data[i];
        head = (head + 1) & TX_BUFFER_MASK;
    }
    
    g_interface.tx_head = head;
    g_interface.stats.bytes_sent += len;
    
    // Start transmission if not in progress
    if (!g_interface.tx_in_progress) {
        // TODO: Implement TX transmission
        g_interface.tx_in_progress = true;
    }
    
    return true;
}

// Check if interface is ready to send
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

// Get interface statistics
void kmbox_interface_get_stats(kmbox_interface_stats_t* stats)
{
    if (stats) {
        *stats = g_interface.stats;
    }
}

// Deinitialize the interface
void kmbox_interface_deinit(void)
{
    if (!g_interface.initialized) {
        return;
    }
    
    // Stop DMA
    if (g_interface.dma_rx_chan >= 0) {
        dma_channel_abort(g_interface.dma_rx_chan);
        dma_channel_unclaim(g_interface.dma_rx_chan);
    }
    
    if (g_interface.dma_tx_chan >= 0) {
        dma_channel_abort(g_interface.dma_tx_chan);
        dma_channel_unclaim(g_interface.dma_tx_chan);
    }
    
    // Deinitialize transport
    switch (g_interface.config.transport_type) {
        case KMBOX_TRANSPORT_UART:
            uart_deinit(g_interface.instance.uart);
            break;
            
        case KMBOX_TRANSPORT_SPI:
            if (g_interface.config.config.spi.is_slave) {
                gpio_set_irq_enabled(g_interface.config.config.spi.cs_pin,
                                     GPIO_IRQ_EDGE_FALL | GPIO_IRQ_EDGE_RISE,
                                     false);
            }
            spi_deinit(g_interface.instance.spi);
            break;
            
        default:
            break;
    }
    
    g_interface.initialized = false;
}

// Get current transport type
kmbox_transport_type_t kmbox_interface_get_transport_type(void)
{
    return g_interface.initialized ? g_interface.config.transport_type : KMBOX_TRANSPORT_NONE;
}