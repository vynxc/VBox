/*
 * KMBox Interface - UART Interface
 * 
 * Provides a clean interface for KMBox communication over UART.
 */

#ifndef KMBOX_INTERFACE_H
#define KMBOX_INTERFACE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>


typedef enum {
    KMBOX_TRANSPORT_NONE = 0,
    KMBOX_TRANSPORT_UART
} kmbox_transport_type_t;


typedef struct {
    uint32_t baudrate;
    unsigned int tx_pin;
    unsigned int rx_pin;
    bool use_dma;
} kmbox_uart_config_t;


typedef struct {
    kmbox_transport_type_t transport_type;
    union {
        kmbox_uart_config_t uart;
    } config;
    

    void (*on_command_received)(const uint8_t* data, size_t len);
} kmbox_interface_config_t;


typedef struct {
    uint32_t bytes_received;
    uint32_t bytes_sent;
    uint32_t packets_received;
    uint32_t packets_sent;
    uint32_t errors;
    uint32_t commands_processed;
} kmbox_interface_stats_t;


bool kmbox_interface_init(const kmbox_interface_config_t* config);


void kmbox_interface_process(void);


bool kmbox_interface_send(const uint8_t* data, size_t len);


bool kmbox_interface_is_ready(void);


void kmbox_interface_get_stats(kmbox_interface_stats_t* stats);


void kmbox_interface_deinit(void);


kmbox_transport_type_t kmbox_interface_get_transport_type(void);


extern const kmbox_uart_config_t KMBOX_UART_DEFAULT_CONFIG;

#endif // KMBOX_INTERFACE_H