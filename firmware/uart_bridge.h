/*
 * Concurrent Target UART Bridge (UART0 GP0/GP1 <-> USB CDC IF 3..4).
 *
 * Operates completely independently of engine_owner so target serial traffic
 * continues uninterrupted while SWD, JTAG, or CH32V003 SWIO sessions run.
 */

#ifndef PU_FIRMWARE_UART_BRIDGE_H
#define PU_FIRMWARE_UART_BRIDGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PU_UART_RING_SIZE       2048u
#define PU_UART_RING_USABLE_CAP (PU_UART_RING_SIZE - 1u)
#define PU_UART_MIN_BAUD        300u
#define PU_UART_MAX_BAUD        3000000u
#define PU_UART_DEFAULT_BAUD    115200u

typedef enum {
    PU_UART_PARITY_NONE = 0,
    PU_UART_PARITY_ODD  = 1,
    PU_UART_PARITY_EVEN = 2
} pu_uart_parity_t;

typedef struct {
    uint32_t baud;
    uint8_t data_bits;
    uint8_t stop_bits;
    pu_uart_parity_t parity;
} pu_uart_line_coding_t;

typedef struct {
    void (*configure_hw)(const pu_uart_line_coding_t *coding);
    void (*tx_kick_hw)(void);
} pu_uart_hw_hal_t;

void pu_uart_bridge_init(const pu_uart_hw_hal_t *hal);
bool pu_uart_bridge_apply_line_coding(uint32_t baud,
                                      uint8_t data_bits,
                                      uint8_t stop_bits,
                                      pu_uart_parity_t parity);
pu_uart_line_coding_t pu_uart_bridge_current_coding(void);

size_t pu_uart_bridge_rx_push(const uint8_t *data, size_t len);
size_t pu_uart_bridge_rx_pop(uint8_t *out, size_t max_len);
size_t pu_uart_bridge_rx_available(void);
uint32_t pu_uart_bridge_rx_drop_count(void);

size_t pu_uart_bridge_tx_push(const uint8_t *data, size_t len);
size_t pu_uart_bridge_tx_pop(uint8_t *out, size_t max_len);
size_t pu_uart_bridge_tx_available(void);

#if !defined(PU_HOST_TEST)
void pu_uart_bridge_rp2040_init(void);
void pu_uart_bridge_task(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PU_FIRMWARE_UART_BRIDGE_H */
