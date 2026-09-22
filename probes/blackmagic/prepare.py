"""Prepare reviewed integration files; preserve upstream copyright notices."""
from pathlib import Path
import sys


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'upstream integration point changed: {old[:100]!r}')
    return text.replace(old, new, 1)


def prepare(source, output):
    output.mkdir(parents=True, exist_ok=True)
    header = (source / 'include/bmp/platform.h').read_text()
    for old, new in [('#define USB_SERIAL_UART_MAIN    (uart1)', '#define USB_SERIAL_UART_MAIN    (uart0)'),
                     ('#define USB_SERIAL_UART_TDI_TDO (uart0)', '#define USB_SERIAL_UART_TDI_TDO (uart1)')]:
        header = replace_once(header, old, new)
    (output / 'platform.h').write_text(header)
    rtos = (source / 'include/bmp/FreeRTOSConfig.h').read_text()
    rtos = replace_once(rtos, '(250000000UL)', '(125000000UL)')
    (output / 'FreeRTOSConfig.h').write_text(rtos)
    main = (source / 'source/main.c').read_text()
    main = '#include "include/probe_controls.h"\n' + main
    main = replace_once(main, '\tplatform_init();',
                        '\tplatform_init();\n\tif (!probe_controls_init(MODE_BLACKMAGIC)) panic("Mode timer unavailable");')
    (output / 'main.c').write_text(main)
    platform = (source / 'source/bmp/platform.c').read_text()
    platform = replace_once(platform, '#include "pico/cyw43_arch.h"',
                            '/* Fixed original Pico board: no CYW43 device is present. */')
    platform = replace_once(platform, 'cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, state);',
                            'panic("Unexpected Pico W board type");')
    platform = replace_once(platform, 'cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, idle_state);',
                            'panic("Unexpected Pico W board type");')
    # Original Pico has no reset buffer: drive low or release, never drive high.
    platform = replace_once(platform, 'gpio_set_dir(target_pins->reset, GPIO_OUT);\n\t\tgpio_put(target_pins->reset, !target_pins->reset_state);',
                            'gpio_put(target_pins->reset, 0);\n\t\tgpio_set_dir(target_pins->reset, GPIO_IN);\n\t\tgpio_pull_up(target_pins->reset);')
    platform = replace_once(platform, 'gpio_put(target_pins->reset, target_pins->reset_state);',
                            'gpio_set_dir(target_pins->reset, GPIO_OUT);')
    platform = replace_once(platform, 'gpio_put(target_pins->reset, !target_pins->reset_state);',
                            'gpio_set_dir(target_pins->reset, GPIO_IN);')
    (output / 'platform.c').write_text(platform)
    transport = (source / 'source/bmp/gdb_if.c').read_text()
    transport = replace_once(transport, '\t\twhile (gdb_to_usb_count > 0) {', '''\t\twhile (gdb_to_usb_count > 0) {
            if (usb_get_config() != 1 || !gdb_serial_get_dtr()) {
                gdb_to_usb_count = 0;
                return;
            }''')
    # Drop buffered bytes from the old session rather than replaying them after reconnect.
    old = "if (!gdb_serial_get_dtr()) {\n"
    if transport.count(old) != 2:
        raise ValueError('Pinned GDB disconnect checks changed')
    transport = transport.replace(old, old + '\t\tusb_to_gdb_buf_pos = usb_to_gdb_count = 0;\n')
    transport = replace_once(transport, '\t\tif (usb_get_config() != 1) {\n\t\t\tcontinue;',
                             '\t\tif (usb_get_config() != 1) {\n\t\t\tusb_to_gdb_buf_pos = usb_to_gdb_count = 0;\n\t\t\tvTaskDelay(pdMS_TO_TICKS(1));\n\t\t\tcontinue;')
    transport = replace_once(transport, 'pdMS_TO_TICKS(portMAX_DELAY)', 'pdMS_TO_TICKS(10)')
    transport = replace_once(transport, '\tif (usb_get_config() != 1) {\n\t\treturn -1;',
                            '\tif (usb_get_config() != 1) {\n\t\tusb_to_gdb_buf_pos = usb_to_gdb_count = 0;\n\t\treturn -1;')
    transport = replace_once(transport, '\twhile (!platform_timeout_is_expired(&receive_timeout)) {', '''\twhile (!platform_timeout_is_expired(&receive_timeout)) {
        if (!gdb_serial_get_dtr() || usb_get_config() != 1) {
            usb_to_gdb_buf_pos = usb_to_gdb_count = 0;
            return '\\x04';
        }''')
    transport = replace_once(transport, 'pdMS_TO_TICKS(timeout_left)', 'pdMS_TO_TICKS(MIN(timeout_left, 10))')
    (output / 'gdb_if.c').write_text(transport)
    (output / 'git_version.h').write_text(
        '#define GIT_BMP_VERSION "ad143564b00ada0a919c11117ee1ac134c5d8768"\n'
        '#define GIT_FREERTOS_VERSION "fed39c5ea7483dde7bf7c92950d49e6b75539f5a"\n'
        '#define GIT_MIOLINK_VERSION "358438abc3601673212aa1cbc1790c4ed2e6d343-universal-dev"\n')


if __name__ == '__main__':
    prepare(Path(sys.argv[1]), Path(sys.argv[2]))
