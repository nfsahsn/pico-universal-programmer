#!/usr/bin/env python3
"""Configure/build the supervisor with the locally bootstrapped dependencies."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

try:
    from .bootstrap import check_sdk, check_checkout
except ImportError:
    from bootstrap import check_sdk, check_checkout

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build')
    parser.add_argument('--config', choices=['Release', 'Debug'], default='Release')
    parser.add_argument('--all', action='store_true', help='build probes and a complete development package')
    args = parser.parse_args()
    lock = json.loads((ROOT / 'tools/dependencies.json').read_text())
    sdk = ROOT / '.tools/pico-sdk'
    check_sdk(sdk)
    gcc = ROOT / '.tools' / lock['arm_gcc_linux_x86_64']['directory'] / 'bin'
    if not (gcc / 'arm-none-eabi-gcc').exists():
        parser.error('Run tools/bootstrap.py first')
    env = dict(os.environ, PICO_SDK_PATH=str(sdk))
    env['PATH'] = os.pathsep.join([str(Path(sys.executable).parent), str(gcc), env.get('PATH', '')])
    cmake = shutil.which('cmake', path=env['PATH'])
    if not cmake:
        parser.error('Install tools/requirements-build.txt into the active Python environment')
    subprocess.run([cmake, '-S', str(ROOT), '-B', str(args.build_dir), '-G', 'Ninja',
                    f'-DCMAKE_BUILD_TYPE={args.config}', '-DPICO_NO_PICOTOOL=1',
                    f'-DPython3_EXECUTABLE={sys.executable}'], env=env, check=True)
    subprocess.run([cmake, '--build', str(args.build_dir), '--parallel', '4'], env=env, check=True)
    if args.all:
        for name, spec in lock['probes'].items():
            check_checkout(ROOT / '.tools' / name, spec)
        for name in ['picorvd', 'debugprobe', 'blackmagic']:
            build = args.build_dir.resolve() / 'probes' / name
            subprocess.run([cmake, '-S', str(ROOT / 'probes' / name), '-B', str(build), '-G', 'Ninja',
                            f'-DCMAKE_BUILD_TYPE={args.config}', '-DPICO_BOARD=pico', '-DPICO_NO_PICOTOOL=1',
                            f'-DPython3_EXECUTABLE={sys.executable}'], env=env, check=True)
            subprocess.run([cmake, '--build', str(build), '--parallel', '4'], env=env, check=True)
        subprocess.run([sys.executable, str(ROOT / 'tools/merge_firmware.py'),
                        '--bootloader', str(args.build_dir / 'pico_universal_programmer.bin'),
                        '--cmsis', str(args.build_dir / 'probes/debugprobe/debugprobe.bin'),
                        '--bmp', str(args.build_dir / 'probes/blackmagic/blackmagic.bin'),
                        '--picorvd', str(args.build_dir / 'probes/picorvd/picorvd.bin'),
                        '-o', str(args.build_dir / 'pico_universal_development.uf2')], check=True)


if __name__ == '__main__':
    main()
