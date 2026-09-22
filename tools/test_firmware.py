#!/usr/bin/env python3
"""Build and run firmware logic against host GPIO/time/flash fakes (Linux)."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import struct
import argparse
import random

try:
    from . import merge_firmware
    from .bootrom_model import simulate_write
except ImportError:
    import merge_firmware
    from bootrom_model import simulate_write

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, help='also validate an actual combined UF2 with the C validator')
    parser.add_argument('--sanitize', action='store_true', help='enable undefined-behavior checks')
    args = parser.parse_args()
    compiler = shlex.split(os.environ.get('CC', 'cc'))
    if args.sanitize:
        compiler += ['-fsanitize=undefined', '-fno-sanitize-recover=all']
    sources = ['tests/test_firmware.c', 'src/button.c', 'src/led_indicator.c',
               'src/image_validator.c', 'src/bootloader.c', 'src/storage.c', 'src/crc32.c',
               'src/probe_control_request.c', 'src/probe_controls.c']
    with tempfile.TemporaryDirectory(prefix='programmer-tests-') as directory:
        executable = Path(directory) / 'firmware_tests'
        inputs = {}
        for name, base in merge_firmware.SLOTS.items():
            data = bytearray(768)
            struct.pack_into('<2I', data, 256, 0x20042000, base + 0x201)
            path = Path(directory) / (name + '.bin')
            path.write_bytes(data)
            inputs[name] = path
        package = Path(directory) / 'fixture.uf2'
        if args.package:
            package = args.package.resolve()
        else:
            merge_firmware.merge_and_export(inputs, package)
        stream = package.read_bytes()
        initial = bytes(range(256)) * (0x200000 // 256)
        expected = bytearray(initial)
        for address, data in merge_firmware.uf2_to_bin(stream).items():
            offset = address - merge_firmware.FLASH_BASE
            expected[offset:offset + len(data)] = data
        count = len(stream) // 512
        shuffled = list(range(count))
        random.Random(2040).shuffle(shuffled)
        for label, order in [('forward', range(count)), ('reverse', range(count - 1, -1, -1)),
                             ('shuffled/repeated', shuffled + shuffled[:16])]:
            flash = simulate_write(stream, initial, order)
            if flash != expected:
                mismatch = next(i for i, pair in enumerate(zip(flash, expected)) if pair[0] != pair[1])
                raise SystemExit(f'BOOTSEL write mismatch ({label}) at '
                                 f'0x{merge_firmware.FLASH_BASE + mismatch:08x}; unsafe UF2 sector layout')
        print('BOOTSEL write model passed: forward, reverse, shuffled/repeated blocks; '
              'existing flash overwritten correctly; untouched sectors preserved', flush=True)
        fixture = Path(directory) / 'fixture.bin'
        fixture.write_bytes(flash)
        subprocess.run([*compiler, '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-O2', '-UNDEBUG', '-Itests/fakes', '-I.', *sources,
                        '-o', str(executable)], cwd=ROOT, check=True)
        subprocess.run([str(executable), str(fixture)], check=True, timeout=30)


if __name__ == '__main__':
    main()
