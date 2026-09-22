#!/usr/bin/env python3
"""Exercise generated CMSIS-DAP GPIO bindings and SWJ routing on the host."""
import argparse
import os
import shutil
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gdb', help='optional real RISC-V GDB for remote protocol integration')
    parser.add_argument("--test-image", type=Path, help="optional real CH32V003 ELF for simulated upload")
    parser.add_argument('--sanitize', action='store_true', help='enable undefined-behavior checks in host tests')
    args = parser.parse_args()
    source = ROOT / '.tools/debugprobe'
    if not (source / 'include/DAP_config.h').is_file():
        raise SystemExit('Run tools/bootstrap.py --probes first')
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    sanitizers = ['-fsanitize=undefined', '-fno-sanitize-recover=all'] if args.sanitize else []
    compiler += sanitizers
    with tempfile.TemporaryDirectory(prefix='probe-tests-') as directory:
        output = Path(directory)
        subprocess.run([sys.executable, str(ROOT / 'probes/debugprobe/prepare.py'),
                        str(source), str(output)], check=True)
        executable = output / 'jtag_tests'
        subprocess.run([*compiler, '-std=c11', '-O2', '-UNDEBUG', '-Wall', '-Wextra',
                        '-Werror', '-Wno-unused-parameter', '-Itests/probe_fakes',
                        '-Itests/fakes', '-Iprobes/debugprobe', '-I' + str(output), 'tests/test_jtag_integration.c',
                        'probes/debugprobe/swj_sequence.c', 'probes/debugprobe/pin_control.c',
                        '-o', str(executable)], cwd=ROOT, check=True)
        subprocess.run([str(executable)], check=True, timeout=10)
        guard_test = output / 'dap_guard_tests'
        subprocess.run([*compiler, '-std=c11', '-O2', '-UNDEBUG', '-Wall', '-Wextra',
                        '-Wno-unused-parameter', '-D__ASM=__asm__', '-I' + str(source / 'CMSIS_DAP/CMSIS/DAP/Firmware/Include'),
                        '-I' + str(output), '-Iprobes/debugprobe', '-Itests/probe_fakes', '-Itests/fakes',
                        'tests/test_dap_guard.c', 'probes/debugprobe/dap_guard.c', '-o', str(guard_test)],
                       cwd=ROOT, check=True)
        subprocess.run([str(guard_test)], check=True, timeout=10)
        picorvd = output / 'picorvd'
        subprocess.run([sys.executable, str(ROOT / 'probes/picorvd/prepare.py'),
                        str(ROOT / '.tools/picorvd'), str(picorvd)], check=True)
        cxx = shlex.split(os.environ.get('CXX', 'c++')) + sanitizers
        packet_test = output / 'packet_tests'
        subprocess.run([*cxx, '-std=c++20', '-O2', '-UNDEBUG', '-I' + str(picorvd),
                        str(ROOT / 'tests/test_picorvd_packet.cpp'), '-o', str(packet_test)], check=True)
        subprocess.run([str(packet_test)], check=True, timeout=10)
        print('PicoRVD packet tests passed: overflow, truncated blobs, hex parsing, cursor bounds')
        harness = output / 'server-harness'
        shutil.copytree(ROOT / 'tests/picorvd_fakes', harness)
        for name in ['GDBServer.cpp', 'GDBServer.h', 'Packet.h', 'utils.h']:
            shutil.copy2(picorvd / name, harness / name)
        shutil.copy2(ROOT / 'probes/picorvd/usb_bridge.h', harness / 'usb_bridge.h')
        server_test = output / 'server_tests'
        subprocess.run([*cxx, '-std=c++20', '-O2', '-UNDEBUG', '-I' + str(harness),
                        str(ROOT / 'tests/test_picorvd_server.cpp'), str(harness / 'GDBServer.cpp'),
                        '-o', str(server_test)], check=True)
        subprocess.run([str(server_test)], check=True, timeout=10)
        for name in ['SoftBreak.cpp', 'SoftBreak.h']:
            shutil.copy2(picorvd / name, harness / name)
        breakpoint_test = output / 'breakpoint_tests'
        subprocess.run([*cxx, '-std=c++20', '-O2', '-UNDEBUG', '-DTEST_REAL_SOFTBREAK',
                        '-I' + str(harness), str(ROOT / 'tests/test_picorvd_breakpoints.cpp'),
                        str(harness / 'SoftBreak.cpp'), '-o', str(breakpoint_test)], check=True)
        subprocess.run([str(breakpoint_test)], check=True, timeout=10)
        bmp = output / 'blackmagic'
        subprocess.run([sys.executable, str(ROOT / 'probes/blackmagic/prepare.py'),
                        str(ROOT / '.tools/miolink/firmware'), str(bmp)], check=True)
        fake = bmp / 'host'
        fake.mkdir()
        (fake / 'general.h').write_text('''#pragma once
#include <stdbool.h>
#include <stdint.h>
#define MIN(a,b) ((a)<(b)?(a):(b))
#define pdMS_TO_TICKS(x) (x)
#define pdFALSE 0
#define USB_CDC_GDB 0
#define USB_CDC_NOTIF_USB_RX_AVAILABLE 1
typedef struct { unsigned end; } platform_timeout_s;
unsigned usb_get_config(void);
unsigned tud_cdc_n_get_line_state(unsigned);
unsigned tud_cdc_n_write_available(unsigned);
unsigned tud_cdc_n_write(unsigned, const void *, unsigned);
void tud_cdc_n_write_flush(unsigned);
unsigned tud_cdc_n_available(unsigned);
unsigned tud_cdc_n_read(unsigned, void *, unsigned);
void vTaskDelay(unsigned);
int xTaskNotifyWait(unsigned, unsigned, uint32_t *, unsigned);
void platform_timeout_set(platform_timeout_s *, unsigned);
bool platform_timeout_is_expired(platform_timeout_s *);
unsigned platform_timeout_time_left(platform_timeout_s *);
void gdb_if_putchar(char, bool);
char gdb_if_getchar(void);
char gdb_if_getchar_to(uint32_t);
''')
        for name in ['platform.h', 'platform_timing.h', 'FreeRTOS.h', 'task.h', 'tusb.h',
                     'usb_cdc.h', 'usb.h', 'gdb_if.h']:
            (fake / name).write_text('#include "general.h"\n')
        shutil.copy2(bmp / 'gdb_if.c', fake / 'gdb_if.c')
        bmp_test = output / 'bmp_transport_tests'
        subprocess.run([*compiler, '-std=c11', '-O2', '-UNDEBUG', '-I' + str(fake),
                        str(ROOT / 'tests/test_blackmagic_transport.c'), str(fake / 'gdb_if.c'),
                        '-o', str(bmp_test)], check=True)
        subprocess.run([str(bmp_test)], check=True, timeout=10)
        if args.gdb:
            subprocess.run([sys.executable, str(ROOT / "tools/test_gdb_remote.py"), args.gdb, str(server_test)] + ([str(args.test_image)] if args.test_image else []), check=True, timeout=210)




if __name__ == '__main__':
    main()
