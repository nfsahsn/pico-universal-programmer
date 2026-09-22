#!/usr/bin/env python3
"""Batch upload and readback verification through Black Magic or PicoRVD."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def script(engine, port, transport, target):
    if not re.fullmatch(r'/dev/[A-Za-z0-9_./:-]+', port):
        raise ValueError('Select a Linux serial device, preferably /dev/serial/by-id/...')
    if target < 1:
        raise ValueError('Target index must be positive')
    lines = ['set confirm off', 'set pagination off', 'set remotetimeout 10',
             'set trust-readonly-sections off', 'set remote memory-read-packet-size 256',
             'set remote memory-write-packet-size 256',
             # Require Python before connecting or modifying target flash.
             'python import gdb, re', 'target extended-remote ' + port]
    if engine == 'blackmagic':
        lines += ['monitor ' + ('swd_scan' if transport == 'swd' else 'jtag_scan'), f'attach {target}']
    else:
        lines += ['monitor reset']
    lines += ['load', 'python',
              'report = gdb.execute("compare-sections", to_string=True)',
              'gdb.write(report)',
              'if "MIS-MATCHED" in report or "warning" in report.lower() or "error" in report.lower() or not re.search(r"Section .* matched\\.", report):',
              '    raise gdb.GdbError("Readback verification failed or produced no section matches")',
              'end']
    if engine == 'picorvd':
        lines += ['monitor reset', 'detach']
    else:
        # Black Magic reset monitor command disconnects the target itself.
        lines += ['detach', 'monitor reset 10']
    lines += ['quit']
    return '\n'.join(lines) + '\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--engine', required=True, choices=['blackmagic', 'picorvd'])
    parser.add_argument('--gdb', required=True)
    parser.add_argument('--port', required=True)
    parser.add_argument('--transport', choices=['swd', 'jtag'], default='swd')
    parser.add_argument('--target-index', type=int, default=1)
    parser.add_argument('--image', required=True, type=Path)
    parser.add_argument('--timeout', type=int, default=180)
    parser.add_argument('--dry-run', action='store_true')
    args = parser.parse_args(argv)
    try:
        executable = shutil.which(args.gdb)
        if executable is None:
            raise ValueError('GDB executable not found')
        image = args.image.resolve(strict=True)
        if not image.is_file() or image.read_bytes()[:4] != b'\x7fELF':
            raise ValueError('Upload requires an ELF image')
        if args.timeout < 1:
            raise ValueError('Timeout must be positive')
        commands = script(args.engine, args.port, args.transport, args.target_index)
        if args.dry_run:
            print(json.dumps({'gdb': executable, 'image': str(image), 'commands': commands}))
            return 0
        if not Path(args.port).exists() or not os.access(args.port, os.R_OK | os.W_OK):
            raise ValueError('Serial device is missing or inaccessible: ' + args.port)
        with tempfile.TemporaryDirectory(prefix='pico-upload-') as directory:
            script_file = Path(directory) / 'upload.gdb'
            script_file.write_text(commands)
            # Snapshot prevents a concurrent build replacing the image mid-upload.
            snapshot = Path(directory) / 'firmware.elf'
            shutil.copyfile(image, snapshot)
            return subprocess.run([executable, '-nx', '-nh', '--batch',
                                   '-iex', 'set auto-load off', '--se=' + str(snapshot),
                                   '-x', str(script_file)], timeout=args.timeout, check=False,
                                   env=dict(os.environ, LC_ALL='C')).returncode
    except subprocess.TimeoutExpired:
        print('Upload timed out; target contents may be incomplete', file=sys.stderr)
        return 124
    except (OSError, ValueError) as error:
        print('Upload error: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
