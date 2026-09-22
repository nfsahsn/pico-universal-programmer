#!/usr/bin/env python3
"""Run the actual PicoRVD packet engine against a real GDB using fake target RAM/flash."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import tty


if not __debug__:
    raise SystemExit('Verification requires Python assertions enabled')

def fixture_elf():
    data = bytearray(0x1c0)
    ident = b'\x7fELF\x01\x01\x01' + bytes(9)
    struct.pack_into('<16sHHIIIIIHHHHHH', data, 0, ident, 2, 243, 1, 0, 52, 0x140, 8, 52, 32, 1, 40, 3, 2)
    struct.pack_into('<IIIIIIII', data, 52, 1, 0x100, 0, 0, 4, 4, 5, 4)
    data[0x100:0x104] = b'\x13\x00\x00\x00'  # NOP, never executed by fake CPU
    strings = b'\0.text\0.shstrtab\0'
    data[0x110:0x110 + len(strings)] = strings
    struct.pack_into('<IIIIIIIIII', data, 0x168, 1, 1, 6, 0, 0x100, 4, 0, 0, 4, 0)
    struct.pack_into('<IIIIIIIIII', data, 0x190, 7, 3, 0, 0, 0x110, len(strings), 0, 0, 1, 0)
    return data


def run(gdb, server, corrupt=False, source_image=None):
    with tempfile.TemporaryDirectory(prefix='gdb-remote-') as temporary:
        image = Path(temporary) / 'smoke.elf'
        image.write_bytes(Path(source_image).read_bytes() if source_image else fixture_elf())
        master, slave = os.openpty()
        tty.setraw(slave)
        try:
            process = subprocess.Popen([str(server), '--serve'] + (['--corrupt'] if corrupt else []), stdin=master, stdout=master, stderr=subprocess.PIPE)
            result = None
            try:
                uploader = Path(__file__).resolve().parent / 'upload_gdb.py'
                result = subprocess.run([sys.executable, str(uploader), '--engine', 'picorvd', '--gdb', str(gdb),
                                         '--port', os.ttyname(slave), '--image', str(image), '--timeout', '90'],
                                        capture_output=True, text=True, timeout=100)
                print(result.stdout, end='')
                if corrupt:
                    if result.returncode == 0 or "Readback verification failed" not in result.stderr:
                        raise RuntimeError("Corruption was not reported correctly: " + result.stderr)
                elif result.returncode:
                    raise RuntimeError(result.stderr)
            finally:
                process.terminate()
                process.wait(timeout=5)
                if result is not None and result.returncode and not corrupt:
                    print(process.stderr.read().decode(), file=sys.stderr)
        finally:
            os.close(master)
            os.close(slave)
    print('Real GDB / PicoRVD engine: ' + ('corruption correctly rejected' if corrupt else 'upload and readback passed against simulated target'))


if __name__ == '__main__':
    source = sys.argv[3] if len(sys.argv) > 3 else None
    run(sys.argv[1], sys.argv[2], source_image=source)
    run(sys.argv[1], sys.argv[2], corrupt=True, source_image=source)
