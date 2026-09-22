#!/usr/bin/env python3
"""Exercise actual Arduino CLI recipe expansion in an isolated sketchbook.

Uses recording tools, never a physical probe or a user's installed core.
"""
import argparse
import json
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


if not __debug__:
    raise SystemExit('Verification requires Python assertions enabled')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', default='arduino-cli')
    parser.add_argument('--data', type=Path, default=Path.home() / '.arduino15',
                        help='existing CLI data directory containing builtin discovery tools')
    parser.add_argument('--platform', type=Path, help='copy metadata from a real board core')
    parser.add_argument('--fqbn', default='picotest:arm:test')
    parser.add_argument('--images', type=Path, help='compiled UploadSmoke artifacts')
    parser.add_argument('--engines', default='openocd,esp32,blackmagic,picorvd')
    args = parser.parse_args()
    cli = shutil.which(args.cli)
    if not cli:
        parser.error('Install Arduino CLI first')
    with tempfile.TemporaryDirectory(prefix='pico-arduino-test-') as temporary:
        base = Path(temporary)
        user = base / 'sketchbook with spaces'
        core = user / 'hardware/picotest/arm'
        core.mkdir(parents=True)
        (core / 'programmers.txt').write_text('# Base programmer file\n')
        (core / 'platform.txt').write_text('name=Pico upload integration test\nversion=1.0.0\n')
        (core / 'boards.txt').write_text('test.name=Upload test target\ntest.build.core=test\ntest.upload.tool.default=unused\ntest.build.mcu=test\ntest.build.bootloader_addr=0x1000\n')
        (core / 'cores/test').mkdir(parents=True)
        if args.platform:
            vendor, architecture = args.fqbn.split(':')[:2]
            real_core = user / 'hardware' / vendor / architecture
            real_core.mkdir(parents=True)
            for item in args.platform.resolve().iterdir():
                destination = real_core / item.name
                if item.is_dir():
                    destination.symlink_to(item, target_is_directory=True)
                elif item.name not in ('platform.local.txt', 'programmers.local.txt'):
                    shutil.copyfile(item, destination)
            core = real_core
        scripts = base / 'scripts'
        (scripts / 'interface').mkdir(parents=True)
        (scripts / 'target').mkdir()
        (scripts / 'interface/cmsis-dap.cfg').write_text('')
        (scripts / 'target/test.cfg').write_text('')
        capture = base / 'arguments.json'
        fake = base / 'recording tool'
        fake.write_text('#!' + sys.executable + '\nimport hashlib, json, os, re, sys\nfrom pathlib import Path\n'
                        'Path(os.environ["PICO_TEST_CAPTURE"]).write_text(json.dumps(sys.argv[1:]))\n'
                        'images = [m.group(1) for arg in sys.argv[1:] for m in re.finditer(r\'program(?:_esp)? "([^"]+)"\', arg)]\n'
                        'images += [arg[5:] for arg in sys.argv[1:] if arg.startswith("--se=")]\n'
                        'Path(os.environ["PICO_TEST_CAPTURE"] + ".images").write_text(json.dumps([hashlib.sha256(Path(p).read_bytes()).hexdigest() for p in images]))\n'
                        'sys.exit(int(os.environ.get("PICO_TEST_EXIT", "0")))\n')
        fake.chmod(0o755)
        # Reuse tools, but not installed hardware platforms: some cores have a
        # stale platform.txt version and otherwise lose precedence to the live core.
        isolated_data = base / 'data'
        (isolated_data / 'packages').mkdir(parents=True)
        for index in args.data.glob('*index*'):
            if index.is_file():
                shutil.copyfile(index, isolated_data / index.name)
        for vendor in (args.data / 'packages').iterdir():
            if vendor.is_dir() and (vendor / 'tools').is_dir():
                destination = isolated_data / 'packages' / vendor.name
                destination.mkdir()
                (destination / 'tools').symlink_to((vendor / 'tools').resolve(), target_is_directory=True)
        config = base / 'arduino-cli.yaml'
        # JSON is also valid YAML.
        config.write_text(json.dumps({'directories': {'user': str(user), 'data': str(isolated_data),
                                                      'downloads': str(base / 'downloads')}}))
        sketch = user / 'Sketch'
        sketch.mkdir()
        (sketch / 'Sketch.ino').write_text('void setup() {}\nvoid loop() {}\n')
        build = base / 'build with spaces'
        build.mkdir()
        (build / 'Sketch.ino.elf').write_bytes(b'\x7fELF' + bytes(60))
        for suffix in ['bin', 'bootloader.bin', 'partitions.bin']:
            (build / ('Sketch.ino.' + suffix)).write_bytes(bytes(16))
        if not args.platform:
            (core / 'tools/partitions').mkdir(parents=True)
            (core / 'tools/partitions/boot_app0.bin').write_bytes(bytes(16))
        if args.images:
            for item in args.images.glob('UploadSmoke.ino.*'):
                if item.is_file():
                    shutil.copyfile(item, build / item.name.replace('UploadSmoke.ino.', 'Sketch.ino.'))
        env = dict(os.environ, PICO_TEST_CAPTURE=str(capture))
        master, slave = os.openpty()
        port = os.ttyname(slave)
        for engine in args.engines.split(','):
            subprocess.run([sys.executable, str(ROOT / 'tools/install_arduino_linux.py'),
                            '--core', str(core), '--profile', engine, '--engine', engine,
                            '--transport', 'jtag' if engine == 'esp32' else 'swd',
                            '--target', 'target/test.cfg', '--openocd', str(fake),
                            '--scripts', str(scripts), '--gdb', str(fake), '--apply'], check=True, capture_output=True)
            upload = [cli, '--config-file', str(config), 'upload', '--fqbn', args.fqbn,
                      '--port', port, '--programmer', 'pico_universal_' + engine, '--input-dir', str(build), str(sketch)]
            result = subprocess.run(upload, env=env, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            invocation = json.loads(capture.read_text())
            if engine in ('openocd', 'esp32'):
                assert 'target/test.cfg' in invocation, invocation
            else:
                assert '-x' in invocation and '--batch' in invocation, invocation
            assert not any('{build.' in item or '{runtime.' in item for item in invocation), invocation
            if engine == 'esp32':
                assert sum('program_esp' in item for item in invocation) == 4, invocation
                expected_images = [build / 'Sketch.ino.bootloader.bin', build / 'Sketch.ino.partitions.bin',
                                   core / 'tools/partitions/boot_app0.bin', build / 'Sketch.ino.bin']
            elif engine == 'openocd':
                assert any('firmware.elf' in item and 'verify reset exit' in item for item in invocation), invocation
                expected_images = [build / 'Sketch.ino.elf']
            else:
                expected_images = [build / 'Sketch.ino.elf']
            assert json.loads(Path(str(capture) + '.images').read_text()) == [
                hashlib.sha256(path.read_bytes()).hexdigest() for path in expected_images], 'Wrong upload snapshot content'
            failure = subprocess.run(upload, env=dict(env, PICO_TEST_EXIT='7'), capture_output=True, text=True)
            assert failure.returncode != 0, 'Arduino reported a failed upload as success'
            print(f'Arduino CLI {engine}: recipe expansion, paths with spaces, upload failure propagation passed')
        os.close(master)
        os.close(slave)


if __name__ == '__main__':
    main()
