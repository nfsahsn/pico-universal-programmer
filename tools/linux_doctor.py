#!/usr/bin/env python3
"""Read-only host and USB diagnostics; never programs a target."""
import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--openocd', default='openocd')
    parser.add_argument('--gdb', default='gdb-multiarch')
    parser.add_argument('--cli', default='arduino-cli')
    args = parser.parse_args()
    report = {'host': platform.platform(), 'python': sys.version.split()[0], 'tools': {}, 'probes': []}
    for name, requested in [('openocd', args.openocd), ('gdb', args.gdb), ('arduino-cli', args.cli)]:
        path = shutil.which(requested)
        result = {'path': path}
        if path:
            try:
                run = subprocess.run([path, 'version' if name == 'arduino-cli' else '--version'], capture_output=True, text=True, timeout=10)
                result.update(returncode=run.returncode, version=(run.stdout + run.stderr).strip().splitlines()[:2])
            except (OSError, subprocess.TimeoutExpired) as error:
                result['error'] = str(error)
        report['tools'][name] = result
    identities = {('2e8a', '000c'): 'cmsis-dap', ('1d50', '6018'): 'blackmagic', ('cafe', '4001'): 'picorvd'}
    for path in sorted(Path('/sys/bus/usb/devices').glob('*')):
        try:
            identity = ((path / 'idVendor').read_text().strip(), (path / 'idProduct').read_text().strip())
            if identity not in identities:
                continue
            device = Path('/dev/bus/usb') / f'{int((path / "busnum").read_text()):03}' / f'{int((path / "devnum").read_text()):03}'
            report['probes'].append({'mode': identities[identity], 'usb': str(device),
                                     'read_write_access': os.access(device, os.R_OK | os.W_OK)})
        except (OSError, ValueError):
            continue
    report['serial_ports'] = [{'path': str(path), 'device': str(path.resolve()),
                               'read_write_access': os.access(path, os.R_OK | os.W_OK)}
                              for path in sorted(Path('/dev/serial/by-id').glob('*'))]
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
