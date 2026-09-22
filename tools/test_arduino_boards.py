#!/usr/bin/env python3
"""Compile pinned representative cores and exercise uploads with recording tools."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', default=str(ROOT / '.tools/arduino-cli/arduino-cli'))
    parser.add_argument('--stm32-data', type=Path, default=ROOT / '.tools/arduino')
    parser.add_argument('--esp32-data', type=Path, default=ROOT / '.tools/arduino')
    parser.add_argument('--ch32v-data', type=Path, default=ROOT / '.tools/arduino')
    args = parser.parse_args()
    lock = json.loads((ROOT / 'tools/dependencies.json').read_text())
    evidence = []
    for name, spec in lock['arduino_test_cores'].items():
        data = getattr(args, name + '_data').resolve()
        vendor, architecture = spec['package'].split(':')
        core = data / 'packages' / vendor / 'hardware' / architecture / spec['version']
        if not core.is_dir():
            raise SystemExit(f'Missing pinned core {spec["package"]}@{spec["version"]}; bootstrap --cores first')
        build = ROOT / 'build/arduino' / name
        subprocess.run([args.cli, '--config-dir', str(data), 'compile', '--fqbn', spec['fqbn'],
                        '--jobs', '2', '--build-path', str(build), str(ROOT / 'examples/UploadSmoke')], check=True)
        engines = {'stm32': 'openocd,blackmagic', 'esp32': 'esp32', 'ch32v': 'picorvd'}[name]
        subprocess.run([sys.executable, str(ROOT / 'tools/test_arduino_integration.py'), '--cli', args.cli,
                        '--data', str(data), '--platform', str(core), '--fqbn', spec['fqbn'],
                        '--images', str(build), '--engines', engines], check=True)
        evidence.append({'core': spec['package'], 'version': spec['version'], 'fqbn': spec['fqbn'],
                         'physical_upload': False,
                         'artifacts': {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                       for path in sorted(build.glob('UploadSmoke.ino.*')) if path.is_file()}})
    (ROOT / 'build/arduino-evidence.json').write_text(json.dumps(evidence, indent=2) + '\n')


if __name__ == '__main__':
    main()
