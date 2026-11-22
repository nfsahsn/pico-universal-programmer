/*
 * Concurrent Target UART Bridge implementation (lock-free SPSC 2 KiB rings).
 */

#include "firmware/uart_bridge.h"

#include <string.h>

#include "config/board_pinout.h"

#if !defined(PU_HOST_TEST) && (defined(__linux__) || defined(_WIN32) || defined(__APPLE__))
#define PU_HOST_TEST 1
#endif

#if !defined(PU_HOST_TEST)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#endif
#include "firmware/tusb_config.h"
#include "firmware/usb_descriptors.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "tusb.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

#define PU_UART_RING_MASK (PU_UART_RING_SIZE - 1u)

#if defined(__cplusplus)
static_assert((PU_UART_RING_SIZE & PU_UART_RING_MASK) == 0u, "UART ring size must be power of two");
#else
_Static_assert((PU_UART_RING_SIZE & PU_UART_RING_MASK) == 0u, "UART ring size must be power of two");
#endif

typedef struct {
    uint8_t buf[PU_UART_RING_SIZE];
    volatile size_t head;
    volatile size_t tail;
} pu_uart_ring_t;

static pu_uart_ring_t s_rx_ring;
static pu_uart_ring_t s_tx_ring;
static uint32_t s_rx_drops = 0u;
static pu_uart_line_coding_t s_coding = {
    .baud = PU_UART_DEFAULT_BAUD,
    .data_bits = 8u,
    .stop_bits = 1u,
    .parity = PU_UART_PARITY_NONE
};
static pu_uart_hw_hal_t s_hw_hal;

static size_t ring_used(const pu_uart_ring_t *r) {
    size_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    size_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    return (h - t) & PU_UART_RING_MASK;
}

static size_t ring_free(const pu_uart_ring_t *r) {
    return PU_UART_RING_USABLE_CAP - ring_used(r);
}

static void add_rx_drops(uint32_t delta) {
    uint32_t cur = __atomic_load_n(&s_rx_drops, __ATOMIC_RELAXED);
    for (;;) {
        uint32_t next = (cur > UINT32_MAX - delta) ? UINT32_MAX : (cur + delta);
        if (__atomic_compare_exchange_n(&s_rx_drops, &cur, next, false,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
            break;
        }
    }
}

static size_t ring_push(pu_uart_ring_t *r, const uint8_t *data, size_t len) {
    if (data == NULL || len == 0u) {
        return 0u;
    }
    size_t avail = ring_free(r);
    size_t to_write = (len < avail) ? len : avail;
    size_t h = r->head;
    for (size_t i = 0u; i < to_write; ++i) {
        r->buf[h] = data[i];
        h = (h + 1u) & PU_UART_RING_MASK;
    }
    __atomic_store_n(&r->head, h, __ATOMIC_RELEASE);
    return to_write;
}

static size_t ring_pop(pu_uart_ring_t *r, uint8_t *out, size_t max_len) {
    if (out == NULL || max_len == 0u) {
        return 0u;
    }
    size_t avail = ring_used(r);
    size_t to_read = (max_len < avail) ? max_len : avail;
    size_t t = r->tail;
    for (size_t i = 0u; i < to_read; ++i) {
        out[i] = r->buf[t];
        t = (t + 1u) & PU_UART_RING_MASK;
    }
    __atomic_store_n(&r->tail, t, __ATOMIC_RELEASE);
    return to_read;
}

void pu_uart_bridge_init(const pu_uart_hw_hal_t *hal) {
    memset(&s_rx_ring, 0, sizeof(s_rx_ring));
    memset(&s_tx_ring, 0, sizeof(s_tx_ring));
    s_rx_drops = 0u;
    s_coding.baud = PU_UART_DEFAULT_BAUD;
    s_coding.data_bits = 8u;
    s_coding.stop_bits = 1u;
    s_coding.parity = PU_UART_PARITY_NONE;
    if (hal != NULL) {
        s_hw_hal = *hal;
    } else {
        memset(&s_hw_hal, 0, sizeof(s_hw_hal));
    }
}

bool pu_uart_bridge_apply_line_coding(uint32_t baud,
                                      uint8_t data_bits,
                                      uint8_t stop_bits,
                                      pu_uart_parity_t parity) {
    if (baud < PU_UART_MIN_BAUD || baud > PU_UART_MAX_BAUD) {
        return false;
    }
    if (data_bits < 5u || data_bits > 8u) {
        return false;
    }
    if (stop_bits != 1u && stop_bits != 2u) {
        return false;
    }
    if (parity != PU_UART_PARITY_NONE &&
        parity != PU_UART_PARITY_ODD &&
        parity != PU_UART_PARITY_EVEN) {
        return false;
    }

    s_coding.baud = baud;
    s_coding.data_bits = data_bits;
    s_coding.stop_bits = stop_bits;
    s_coding.parity = parity;

    if (s_hw_hal.configure_hw != NULL) {
        s_hw_hal.configure_hw(&s_coding);
    }
    return true;
}

pu_uart_line_coding_t pu_uart_bridge_current_coding(void) {
    return s_coding;
}

size_t pu_uart_bridge_rx_push(const uint8_t *data, size_t len) {
    size_t written = ring_push(&s_rx_ring, data, len);
    if (written < len) {
        add_rx_drops((uint32_t)(len - written));
    }
    return written;
}

size_t pu_uart_bridge_rx_pop(uint8_t *out, size_t max_len) {
    return ring_pop(&s_rx_ring, out, max_len);
}

size_t pu_uart_bridge_rx_available(void) {
    return ring_used(&s_rx_ring);
}

uint32_t pu_uart_bridge_rx_drop_count(void) {
    return __atomic_load_n(&s_rx_drops, __ATOMIC_ACQUIRE);
}

size_t pu_uart_bridge_tx_push(const uint8_t *data, size_t len) {
    size_t written = ring_push(&s_tx_ring, data, len);
    if (written > 0u && s_hw_hal.tx_kick_hw != NULL) {
        s_hw_hal.tx_kick_hw();
    }
    return written;
}

size_t pu_uart_bridge_tx_pop(uint8_t *out, size_t max_len) {
    return ring_pop(&s_tx_ring, out, max_len);
}

size_t pu_uart_bridge_tx_available(void) {
    return ring_used(&s_tx_ring);
}

#if !defined(PU_HOST_TEST)
/* Naturally aligned 2 KiB circular DMA ring buffer (2^11 = 2048 bytes) for 3 Mbaud RX */
#define PU_UART_DMA_RING_BITS 11u
static uint8_t s_dma_rx_buf[PU_UART_RING_SIZE] __attribute__((aligned(PU_UART_RING_SIZE)));
static int s_dma_rx_chan = -1;
static size_t s_dma_rx_tail = 0u;

static void rp2040_uart0_dma_rx_arm(void) {
    if (s_dma_rx_chan < 0) {
        return;
    }
    dma_channel_config cfg = dma_channel_get_default_config((uint)s_dma_rx_chan);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_ring(&cfg, true /* write ring */, PU_UART_DMA_RING_BITS);
    channel_config_set_dreq(&cfg, DREQ_UART0_RX);
    dma_channel_configure((uint)s_dma_rx_chan, &cfg,
                          s_dma_rx_buf,
                          &uart_get_hw(uart0)->dr,
                          0xFFFFFFFFu,
                          true);
}

static void rp2040_uart0_configure_hw(const pu_uart_line_coding_t *coding) {
    if (coding == NULL) {
        return;
    }
    /* Drain pending TX cleanly before reconfiguring PL011 baud/frame registers */
    uart_tx_wait_blocking(uart0);
    uart_set_baudrate(uart0, coding->baud);
    uart_parity_t p = UART_PARITY_NONE;
    if (coding->parity == PU_UART_PARITY_ODD) {
        p = UART_PARITY_ODD;
    } else if (coding->parity == PU_UART_PARITY_EVEN) {
        p = UART_PARITY_EVEN;
    }
    uart_set_format(uart0, coding->data_bits, coding->stop_bits, p);
}

static void rp2040_uart0_drain_tx(void) {
    while (uart_is_writable(uart0) && pu_uart_bridge_tx_available() > 0u) {
        uint8_t ch = 0u;
        if (pu_uart_bridge_tx_pop(&ch, 1u) == 1u) {
            uart_putc_raw(uart0, (char)ch);
        }
    }
}

static void rp2040_uart0_poll_dma_rx(void) {
    /* Track hardware PL011 RX FIFO overruns explicitly */
    if ((uart_get_hw(uart0)->rsr & UART_UARTRSR_OE_BITS) != 0u) {
        uart_get_hw(uart0)->rsr = UART_UARTRSR_OE_BITS;
        add_rx_drops(1u);
    }
    if (s_dma_rx_chan < 0) {
        return;
    }
    uintptr_t wr_addr = dma_hw->ch[(uint)s_dma_rx_chan].write_addr;
    uintptr_t base_addr = (uintptr_t)s_dma_rx_buf;
    size_t head = (size_t)((wr_addr - base_addr) & PU_UART_RING_MASK);
    if (head != s_dma_rx_tail) {
        if (head > s_dma_rx_tail) {
            (void)pu_uart_bridge_rx_push(&s_dma_rx_buf[s_dma_rx_tail], head - s_dma_rx_tail);
        } else {
            (void)pu_uart_bridge_rx_push(&s_dma_rx_buf[s_dma_rx_tail], PU_UART_RING_SIZE - s_dma_rx_tail);
            if (head > 0u) {
                (void)pu_uart_bridge_rx_push(s_dma_rx_buf, head);
            }
        }
        s_dma_rx_tail = head;
    }
    if (!dma_channel_is_busy((uint)s_dma_rx_chan)) {
        s_dma_rx_tail = 0u;
        rp2040_uart0_dma_rx_arm();
    }
}

static const pu_uart_hw_hal_t kRp2040UartHal = {
    .configure_hw = rp2040_uart0_configure_hw,
    .tx_kick_hw = rp2040_uart0_drain_tx
};

void pu_uart_bridge_rp2040_init(void) {
    pu_uart_bridge_init(&kRp2040UartHal);

    uart_init(uart0, PU_UART_DEFAULT_BAUD);
    gpio_set_function(PU_PIN_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(PU_PIN_UART_RX, GPIO_FUNC_UART);
    gpio_pull_up(PU_PIN_UART_RX);
    uart_set_hw_flow(uart0, false, false);
    uart_set_format(uart0, 8u, 1u, UART_PARITY_NONE);
    uart_set_fifo_enabled(uart0, true);

    s_dma_rx_tail = 0u;
    s_dma_rx_chan = dma_claim_unused_channel(true);
    rp2040_uart0_dma_rx_arm();
}

void pu_uart_bridge_task(void) {
    rp2040_uart0_poll_dma_rx();

    /* Host CDC -> Target UART0 TX */
    if (tud_cdc_n_available(PU_TUD_CDC_IDX_UART) > 0u) {
        uint8_t buf[64];
        size_t free_space = PU_UART_RING_USABLE_CAP - pu_uart_bridge_tx_available();
        if (free_space > 0u) {
            size_t want = (free_space < sizeof(buf)) ? free_space : sizeof(buf);
            uint32_t got = tud_cdc_n_read(PU_TUD_CDC_IDX_UART, buf, (uint32_t)want);
            if (got > 0u) {
                (void)pu_uart_bridge_tx_push(buf, (size_t)got);
            }
        }
    }
    rp2040_uart0_drain_tx();

    /* Target UART0 RX -> Host CDC (gate on tud_mounted, not DTR, for Linux termios compatibility) */
    if (tud_mounted()) {
        uint32_t wr_avail = tud_cdc_n_write_available(PU_TUD_CDC_IDX_UART);
        if (wr_avail > 0u && pu_uart_bridge_rx_available() > 0u) {
            uint8_t buf[64];
            size_t want = (wr_avail < sizeof(buf)) ? (size_t)wr_avail : sizeof(buf);
            size_t popped = pu_uart_bridge_rx_pop(buf, want);
            if (popped > 0u) {
                (void)tud_cdc_n_write(PU_TUD_CDC_IDX_UART, buf, (uint32_t)popped);
                (void)tud_cdc_n_write_flush(PU_TUD_CDC_IDX_UART);
            }
        }
    }
}
#endif
