/*
 * Hardened CMSIS-DAP v2 Engine for Pico Universal:
 *   - Pre-execution packet bounds, JTAG chain, & batch response-size guard
 *   - Private request snapshotting (immune to USB reset / aliased buffer reuse)
 *   - Strict mutual exclusion & lease renewal via engine_owner FSM
 *   - 64-bit clock divider math with 1 kHz floor & 24 MHz ceiling
 *   - Deterministic PIO0 SWD / JTAG execution & PIO-to-SIO pin handoff
 */

#ifndef PU_FIRMWARE_DAP_ENGINE_H
#define PU_FIRMWARE_DAP_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__has_include)
#if __has_include("config/board_pinout.h")
#include "config/board_pinout.h"
#else
#include "../config/board_pinout.h"
#endif
#else
#include "config/board_pinout.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol & timing bounds */
#define PU_DAP_PACKET_SIZE          64u
#define PU_DAP_PACKET_COUNT         8u
#define PU_DAP_MAX_JTAG_DEVS        8u
#define PU_DAP_DEFAULT_CLOCK_HZ     1000000u
#define PU_DAP_MIN_CLOCK_HZ         1000u
#define PU_DAP_MAX_CLOCK_HZ         24000000u
#define PU_DAP_SESSION_TIMEOUT_US   5000000u
#define PU_DAP_MAX_PIN_WAIT_US      3000000u

/* CMSIS-DAP v2 Command IDs */
#define ID_DAP_Info                 0x00u
#define ID_DAP_HostStatus           0x01u
#define ID_DAP_Connect              0x02u
#define ID_DAP_Disconnect           0x03u
#define ID_DAP_TransferConfigure    0x04u
#define ID_DAP_Transfer             0x05u
#define ID_DAP_TransferBlock        0x06u
#define ID_DAP_TransferAbort        0x07u
#define ID_DAP_WriteABORT           0x08u
#define ID_DAP_Delay                0x09u
#define ID_DAP_ResetTarget          0x0Au
#define ID_DAP_SWJ_Pins             0x10u
#define ID_DAP_SWJ_Clock            0x11u
#define ID_DAP_SWJ_Sequence         0x12u
#define ID_DAP_SWD_Configure        0x13u
#define ID_DAP_JTAG_Sequence        0x14u
#define ID_DAP_JTAG_Configure       0x15u
#define ID_DAP_JTAG_IDCODE          0x16u
#define ID_DAP_SWD_Sequence         0x1Du
#define ID_DAP_QueueCommands        0x7Eu
#define ID_DAP_ExecuteCommands      0x7Fu
#define ID_DAP_Invalid              0xFFu

/* CMSIS-DAP Info IDs */
#define DAP_ID_VENDOR               0x01u
#define DAP_ID_PRODUCT              0x02u
#define DAP_ID_SER_NUM              0x03u
#define DAP_ID_CMSIS_DAP_VER        0x04u
#define DAP_ID_DEVICE_VENDOR        0x05u
#define DAP_ID_DEVICE_NAME          0x06u
#define DAP_ID_BOARD_VENDOR         0x07u
#define DAP_ID_BOARD_NAME           0x08u
#define DAP_ID_PRODUCT_FW_VER       0x09u
#define DAP_ID_CAPABILITIES         0xF0u
#define DAP_ID_TIMESTAMP_CLOCK      0xF1u
#define DAP_ID_PACKET_COUNT         0xFEu
#define DAP_ID_PACKET_SIZE          0xFFu

/* CMSIS-DAP Status & Port codes */
#define DAP_OK                      0x00u
#define DAP_ERROR                   0xFFu

#define DAP_PORT_AUTODETECT         0x00u
#define DAP_PORT_DISABLED           0x00u
#define DAP_PORT_SWD                0x01u
#define DAP_PORT_JTAG               0x02u

/* CMSIS-DAP Transfer Request & Response flags */
#define DAP_TRANSFER_APnDP          (1u << 0)
#define DAP_TRANSFER_RnW            (1u << 1)
#define DAP_TRANSFER_A2             (1u << 2)
#define DAP_TRANSFER_A3             (1u << 3)
#define DAP_TRANSFER_MATCH_VALUE    (1u << 4)
#define DAP_TRANSFER_MATCH_MASK     (1u << 5)
#define DAP_TRANSFER_TIMESTAMP      (1u << 7)

#define DAP_TRANSFER_OK             (1u << 0)
#define DAP_TRANSFER_WAIT           (1u << 1)
#define DAP_TRANSFER_FAULT          (1u << 2)
#define DAP_TRANSFER_ERROR          (1u << 3)
#define DAP_TRANSFER_MISMATCH       (1u << 4)

/* CMSIS-DAP SWJ_Pins bitmasks */
#define PU_DAP_PIN_SWCLK_TCK        (1u << 0)
#define PU_DAP_PIN_SWDIO_TMS        (1u << 1)
#define PU_DAP_PIN_TDI              (1u << 2)
#define PU_DAP_PIN_TDO              (1u << 3)
#define PU_DAP_PIN_nTRST            (1u << 5)
#define PU_DAP_PIN_nRESET           (1u << 7)

/* Physical wire HAL (binds to RP2040 PIO0 by default when wire_hal == NULL) */
typedef struct {
    uint32_t sys_clock_hz;
    void (*port_setup)(uint8_t port);
    void (*port_off)(void);
    void (*set_clock)(uint32_t clkdiv_q16_8, uint32_t effective_khz);
    void (*swj_sequence)(uint32_t bit_count, const uint8_t *data);
    void (*swd_sequence)(uint32_t bit_count,
                         bool capture,
                         const uint8_t *tx_data,
                         uint8_t *rx_data);
    uint8_t (*swd_transfer)(uint8_t req_hdr,
                            uint32_t *data,
                            uint8_t turnaround_cycles,
                            bool force_data_phase);
    void (*jtag_sequence)(uint32_t bit_count,
                          bool tms,
                          const uint8_t *tdi_data,
                          uint8_t *tdo_data);
    void (*pio_drain)(void);
    void (*sio_override_begin)(void);
    uint8_t (*swj_pins_io)(uint8_t pin_output, uint8_t pin_select, uint32_t wait_us);
    void (*sio_override_end)(void);
    void (*delay_us)(uint32_t delay_us);
    uint32_t (*get_timestamp_us)(void);
} pu_dap_wire_hal_t;

void pu_dap_init(const pu_dap_wire_hal_t *wire_hal);
#ifdef PU_HOST_TEST
void pu_dap_init_for_test(void);
#endif
size_t pu_dap_process_packet(const uint8_t *req,
                             size_t req_len,
                             uint8_t *resp,
                             size_t resp_cap,
                             uint64_t now_us);
void pu_dap_on_usb_disconnect(void);

/* Validation & 64-bit clock math helpers */
bool pu_dap_packet_valid(const uint8_t *req, size_t req_len, size_t resp_cap);
uint32_t pu_dap_compute_pio_clkdiv_q16_8(uint32_t sys_hz,
                                         uint32_t target_hz,
                                         uint32_t *out_effective_khz);
uint32_t pu_dap_delay_to_khz(uint32_t sys_hz, uint32_t delay);

#ifdef __cplusplus
}
#endif

#endif /* PU_FIRMWARE_DAP_ENGINE_H */
