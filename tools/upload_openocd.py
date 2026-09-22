#!/usr/bin/env python3
"""Verified ELF upload through CMSIS-DAP, with explicit OpenOCD target selection."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def tcl_word(value):
    """Quote one Tcl word without allowing substitution, including unusual paths."""
    return '"' + ''.join('\\' + c if c in '\\"$[]{}' else
                          '\\n' if c == '\n' else '\\r' if c == '\r' else c
                          for c in str(value)) + '"'


def command(args):
    segments = getattr(args, "segment", [])
    if segments:
        if args.image is not None:
            raise ValueError("Use either ELF or binary segments, not both")
        if len(segments) > 1 and not getattr(args, 'esp', False):
            raise ValueError("Use one ELF for multi-region targets; separate generic program calls can erase earlier segments")
        images = []
        for offset, filename in segments:
            address = int(offset, 0)
            path = Path(filename).resolve(strict=True)
            size = path.stat().st_size
            if not path.is_file() or size == 0 or address < 0 or address + size > 2**32:
                raise ValueError("Invalid binary segment")
            images.append((address, path, size))
        images.sort()
        for previous, current in zip(images, images[1:]):
            if previous[0] + previous[2] > current[0]:
                raise ValueError("Binary segments overlap")
            if getattr(args, 'esp', False) and (previous[0] + previous[2] - 1) // 4096 >= current[0] // 4096:
                raise ValueError("Espressif binary segments share an erase sector")
        image = None
    elif args.image is not None:
        image = args.image.resolve(strict=True)
    else:
        raise ValueError("Provide an ELF image or binary segments")
    if image is not None:
        if not image.is_file() or image.suffix.lower() != '.elf':
            raise ValueError('This upload requires a linked ELF image')
        with image.open('rb') as stream:
            if stream.read(4) != b'\x7fELF':
                raise ValueError('Image is not an ELF file')
        if getattr(args, 'esp', False):
            raise ValueError('Espressif uploads require binary segments, including bootloader and partitions')
    if args.speed < 1:
        raise ValueError('Adapter speed must be positive (kHz)')
    if not args.target or args.target.startswith('-') or any(c in args.target for c in '\r\n\x00'):
        raise ValueError('Provide an explicit OpenOCD target configuration')
    executable = shutil.which(args.openocd)
    if executable is None:
        raise ValueError('OpenOCD executable not found: ' + args.openocd)
    result = [executable]
    for directory in args.scripts:
        if not directory.is_dir():
            raise ValueError('OpenOCD scripts directory not found: ' + str(directory))
        result += ['-s', str(directory.resolve())]
    result += ['-f', 'interface/cmsis-dap.cfg', '-c', 'transport select ' + args.transport]
    if args.serial:
        result += ['-c', 'adapter serial ' + tcl_word(args.serial)]
    result += ['-f', args.target, '-c', f'adapter speed {args.speed}']
    if image is not None:
        result += ['-c', 'program ' + tcl_word(image) + ' verify reset exit']
    else:
        helper = 'program_esp' if getattr(args, 'esp', False) else 'program'
        for address, path, _ in images:
            operation = f'{helper} {tcl_word(path)} 0x{address:x} verify'
            # Any failed segment must prevent subsequent writes and success reporting.
            result += ['-c', 'if {[catch {' + operation + '} message]} {echo $message; shutdown error}']
        result += ['-c', 'reset run', '-c', 'shutdown']
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--openocd', default='openocd')
    parser.add_argument('--scripts', type=Path, action='append', default=[])
    parser.add_argument('--target', required=True, help='OpenOCD target .cfg file, e.g. target/stm32f4x.cfg')
    parser.add_argument('--transport', choices=['swd', 'jtag'], required=True)
    parser.add_argument('--speed', type=int, default=100, help='adapter clock in kHz (default: 100)')
    parser.add_argument('--serial', help='select a particular probe')
    parser.add_argument('--image', type=Path)
    parser.add_argument('--segment', nargs=2, action='append', default=[], metavar=('OFFSET', 'FILE'))
    parser.add_argument('--esp', action='store_true', help='use Espressif OpenOCD binary programming helper')
    parser.add_argument('--timeout', type=int, default=180, help='maximum upload duration in seconds')
    parser.add_argument('--dry-run', action='store_true', help='print argument array without connecting to hardware')
    args = parser.parse_args(argv)
    try:
        if args.timeout < 1:
            raise ValueError("Timeout must be positive")
        invocation = command(args)
        if args.dry_run:
            print(json.dumps(invocation))
            return 0
        with tempfile.TemporaryDirectory(prefix='pico-upload-') as directory:
            # Build tools may replace their outputs during an upload. Program
            # and verify the same private snapshot for every segment.
            if args.image is not None:
                snapshot = Path(directory) / 'firmware.elf'
                shutil.copyfile(args.image, snapshot)
                args.image = snapshot
            else:
                snapshots = []
                for index, (offset, filename) in enumerate(args.segment):
                    snapshot = Path(directory) / f'segment-{index}.bin'
                    shutil.copyfile(filename, snapshot)
                    snapshots.append((offset, str(snapshot)))
                args.segment = snapshots
            return subprocess.run(command(args), check=False, timeout=args.timeout).returncode
    except subprocess.TimeoutExpired:
        print("Upload timed out; target contents may be incomplete", file=sys.stderr)
        return 124
    except (OSError, ValueError) as error:
        print(f'Upload error: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
