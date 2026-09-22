"""Apply narrowly scoped board/JTAG integration to pinned Debug Probe sources.

Generated files retain upstream notices. Every substitution must match exactly;
source upgrades require explicit review rather than silently applying a patch.
"""
from pathlib import Path
import re
import sys


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'upstream integration point changed: {old[:100]!r}')
    return text.replace(old, new, 1)


def function(text, name, body):
    pattern = r'(__STATIC_(?:FORCE)?INLINE\s+(?:void|uint32_t)\s+' + name + r'\s*\([^)]*\)\s*\{)[^{}]*(\})'
    result, count = re.subn(pattern, lambda m: m[1] + '\n' + body + '\n' + m[2], text)
    if count != 1:
        raise ValueError(f'upstream function changed: {name}')
    return result


def prepare(source, output):
    output.mkdir(parents=True, exist_ok=True)
    board = (source / 'include/board_pico_config.h').read_text()
    for old, new in [('#define PROBE_PIN_RESET 1', '#define PROBE_PIN_RESET 6'),
                     ('#define PROBE_UART_TX 4', '#define PROBE_UART_TX 0'),
                     ('#define PROBE_UART_RX 5', '#define PROBE_UART_RX 1'),
                     ('#define PROBE_UART_INTERFACE uart1', '#define PROBE_UART_INTERFACE uart0'),
                     ('#define PROBE_USB_CONNECTED_LED 25', '/* GP25 belongs to shared mode controls. */'),
                     ('#define PROBE_PRODUCT_STRING "Debugprobe on Pico (CMSIS-DAP)"',
                      '#define PROBE_PRODUCT_STRING "Pico Universal CMSIS-DAP"')]:
        board = replace_once(board, old, new)
    board += '\n#define PROBE_PIN_TDI 4\n#define PROBE_PIN_TDO 5\n#define PROBE_PIN_TRST 7\n'
    (output / 'board_pico_config.h').write_text(board)

    dap = (source / 'include/DAP_config.h').read_text()
    dap = '#include "pin_control.h"\n' + dap
    dap = replace_once(dap, '#define DAP_JTAG                0', '#define DAP_JTAG                1')
    dap = function(dap, 'PORT_JTAG_SETUP', '''  probe_deinit();
  const uint pins[] = {PROBE_PIN_SWCLK, PROBE_PIN_SWDIO, PROBE_PIN_TDI};
  for (uint i = 0; i < 3; ++i) {
    gpio_init(pins[i]);
    gpio_put(pins[i], 1);
    gpio_set_dir(pins[i], GPIO_OUT);
  }
  gpio_init(PROBE_PIN_TDO);
  gpio_set_dir(PROBE_PIN_TDO, GPIO_IN);
  gpio_init(PROBE_PIN_RESET);
  gpio_put(PROBE_PIN_RESET, 0);
  gpio_set_dir(PROBE_PIN_RESET, GPIO_IN);
  gpio_pull_up(PROBE_PIN_RESET);
  gpio_init(PROBE_PIN_TRST);
  gpio_put(PROBE_PIN_TRST, 0);
  gpio_set_dir(PROBE_PIN_TRST, GPIO_IN);
  gpio_pull_up(PROBE_PIN_TRST);''')
    dap = function(dap, 'PORT_SWD_SETUP', '''  gpio_init(PROBE_PIN_TDI);
  gpio_set_dir(PROBE_PIN_TDI, GPIO_IN);
  gpio_init(PROBE_PIN_TDO);
  gpio_set_dir(PROBE_PIN_TDO, GPIO_IN);
  gpio_init(PROBE_PIN_TRST);
  gpio_set_dir(PROBE_PIN_TRST, GPIO_IN);
  probe_init();
  cached_delay = UINT32_MAX;''')
    dap = function(dap, 'PORT_OFF', '''  probe_deinit();
  const uint pins[] = {PROBE_PIN_SWCLK, PROBE_PIN_SWDIO, PROBE_PIN_TDI,
                      PROBE_PIN_TDO, PROBE_PIN_RESET, PROBE_PIN_TRST};
  for (uint i = 0; i < 6; ++i) {
    gpio_init(pins[i]);
    gpio_set_dir(pins[i], GPIO_IN);
  }''')
    functions = {
        'PIN_SWCLK_TCK_IN': '  return gpio_get(PROBE_PIN_SWCLK);',
        'PIN_SWCLK_TCK_SET': '  probe_pin_write(PROBE_PIN_SWCLK, 1);',
        'PIN_SWCLK_TCK_CLR': '  probe_pin_write(PROBE_PIN_SWCLK, 0);',
        'PIN_SWDIO_TMS_IN': '  return gpio_get(PROBE_PIN_SWDIO);',
        'PIN_SWDIO_TMS_SET': '  probe_pin_write(PROBE_PIN_SWDIO, 1);',
        'PIN_SWDIO_TMS_CLR': '  probe_pin_write(PROBE_PIN_SWDIO, 0);',
        'PIN_TDI_IN': '  return gpio_get(PROBE_PIN_TDI);',
        'PIN_TDI_OUT': '  gpio_put(PROBE_PIN_TDI, bit & 1u);',
        'PIN_TDO_IN': '  return gpio_get(PROBE_PIN_TDO);',
        'PIN_nTRST_IN': '  return gpio_get(PROBE_PIN_TRST);',
        'PIN_nTRST_OUT': '  gpio_set_dir(PROBE_PIN_TRST, (bit & 1u) ? GPIO_IN : GPIO_OUT);',
    }
    for name, body in functions.items():
        dap = function(dap, name, body)
    (output / 'DAP_config.h').write_text(dap)
    main = (source / 'src/main.c').read_text()
    main = '#include "include/probe_controls.h"\n#include "dap_guard.h"\n' + main
    main = replace_once(main, '    stdio_uart_init();',
                        '    if (!probe_controls_init(MODE_CMSIS_DAP)) panic("Mode timer unavailable");')
    main = replace_once(main, '            tud_vendor_read(RxDataBuffer, sizeof(RxDataBuffer));\n            resp_len = DAP_ProcessCommand(RxDataBuffer, TxDataBuffer);',
                        '            uint32_t received = tud_vendor_read(RxDataBuffer, sizeof(RxDataBuffer));\n            resp_len = dap_execute_checked(RxDataBuffer, received, TxDataBuffer);')
    main = replace_once(main, '  DAP_ProcessCommand(RxDataBuffer, TxDataBuffer);',
                        '  response_size = dap_execute_checked(RxDataBuffer, bufsize, TxDataBuffer);')
    (output / 'main.c').write_text(main)
    endpoint = (source / 'src/tusb_edpt_handler.c').read_text()
    endpoint = '#include "dap_guard.h"\n' + endpoint
    endpoint = replace_once(endpoint, '\t\t\t// Only queue the next buffer in the out callback', '''\t\t\tUSBRequestBuffer.data_len[WR_IDX(USBRequestBuffer)] = xferred_bytes;
            if (!dap_packet_valid(WR_SLOT_PTR(USBRequestBuffer), xferred_bytes)) {
                (WR_SLOT_PTR(USBRequestBuffer))[0] = ID_DAP_Invalid;
                USBRequestBuffer.data_len[WR_IDX(USBRequestBuffer)] = 1;
            }
\t\t\t// Only queue the next buffer in the out callback''')
    endpoint = replace_once(endpoint, '\t\twhile(USBRequestBuffer.rptr != USBRequestBuffer.wptr)\n\t\t{', '''\t\twhile(USBRequestBuffer.rptr != USBRequestBuffer.wptr)
\t\t{
            while (buffer_full(&USBResponseBuffer))
                xTaskNotifyWait(0, 0xFFFFFFFFu, &cmd, 1);
            if (buffer_empty(&USBRequestBuffer)) break;''')
    endpoint = replace_once(endpoint,
        'DAP_ExecuteCommand(RD_SLOT_PTR(USBRequestBuffer), WR_SLOT_PTR(USBResponseBuffer)) & 0xffff',
        'dap_execute_checked(RD_SLOT_PTR(USBRequestBuffer), USBRequestBuffer.data_len[RD_IDX(USBRequestBuffer)], WR_SLOT_PTR(USBResponseBuffer))')
    (output / 'tusb_edpt_handler.c').write_text(endpoint)
    pio = (source / 'src/sw_dp_pio.c').read_text()
    pio = '#include "clock_math.h"\n' + pio
    pio = replace_once(pio, '#define MAKE_KHZ(x) (CPU_CLOCK / (2000 * ((x) + 1)))',
                       '#define MAKE_KHZ(x) probe_delay_to_khz(CPU_CLOCK, (x))')
    (output / 'sw_dp_pio.c').write_text(pio)
    probe = (source / 'src/probe.c').read_text()
    probe = '#include "pin_control.h"\n' + probe
    probe = replace_once(probe, 'static void probe_wait_idle()', 'void probe_wait_idle(void)')
    for signature in ['void probe_write_bits(uint bit_count, uint32_t data_byte)',
                      'void probe_hiz_clocks(uint bit_count)', 'uint32_t probe_read_bits(uint bit_count)',
                      'void probe_read_mode(void)', 'void probe_write_mode(void)']:
        probe = replace_once(probe, signature + ' {', signature + ' {\n    probe_pin_release();')
    probe += '\nbool probe_swd_is_active(void) { return probe.initted != 0; }\n'
    (output / 'probe.c').write_text(probe)
    version = output / 'probe/version.h'
    version.parent.mkdir(exist_ok=True)
    version.write_text('#define PROBE_VERSION "debugprobe-v2.3.1-universal-dev"\n')


if __name__ == '__main__':
    prepare(Path(sys.argv[1]), Path(sys.argv[2]))
