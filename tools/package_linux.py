#!/usr/bin/env python3
"""Build and verify a Linux hardware-test candidate with source provenance."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE_DIRS = ['src', 'include', 'config', 'cmake', 'probes', 'tools', 'tests', 'examples', 'linux', 'docs']

if not __debug__:
    raise SystemExit('Verification requires Python assertions enabled')

def source_fingerprint():
    files = [ROOT / n for n in ['README.md', 'CMakeLists.txt', 'pico_sdk_import.cmake']]
    for directory in SOURCE_DIRS:
        files.extend(p for p in (ROOT / directory).rglob('*') if p.is_file() and '__pycache__' not in p.parts and p.suffix != '.pyc')
    return {str(p.relative_to(ROOT)): digest(p) for p in sorted(files)}


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cli', default='arduino-cli')
    parser.add_argument('--arduino-data', type=Path, default=Path.home() / '.arduino15')
    parser.add_argument('--output', type=Path, default=ROOT / 'build/linux-candidate.tar.gz')
    parser.add_argument('--esp32-data', type=Path, default=ROOT / '.tools/arduino')
    args = parser.parse_args()
    args.output = args.output.resolve()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    lock = json.loads((ROOT / 'tools/dependencies.json').read_text())
    gdb = args.esp32_data.resolve() / 'packages/esp32/tools/riscv32-esp-elf-gdb' / lock['arduino_test_cores']['esp32']['gdb_version'] / 'bin/riscv32-esp-elf-gdb'
    if not gdb.is_file():
        parser.error('Pinned RISC-V GDB missing; bootstrap Arduino cores or select --esp32-data')
    before = source_fingerprint()
    with tempfile.TemporaryDirectory(prefix='linux-candidate-', dir=ROOT / 'build') as temporary:
        stage = Path(temporary) / 'pico-universal-linux'
        stage.mkdir()
        logs = stage / 'verification'
        logs.mkdir()
        commands = [
            ('layout', [sys.executable, 'tools/flash_layout.py', '--check']),
            ('python-tests', [sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-v']),
            ('firmware-tests', [sys.executable, 'tools/test_firmware.py']),
            ('firmware-sanitizers', [sys.executable, 'tools/test_firmware.py', '--sanitize']),
            ('arduino-recipes', [sys.executable, 'tools/test_arduino_integration.py', '--cli', args.cli,
                                 '--data', str(args.arduino_data.resolve())]),
            ('arduino-board-tests', [sys.executable, 'tools/test_arduino_boards.py', '--cli', args.cli,
                                     '--stm32-data', str(args.arduino_data.resolve()),
                                     '--ch32v-data', str(args.arduino_data.resolve()),
                                     '--esp32-data', str(args.esp32_data.resolve())]),
            ('probe-tests', [sys.executable, 'tools/test_probe_integration.py', '--gdb', str(gdb), '--test-image', 'build/arduino/ch32v/UploadSmoke.ino.elf']),
            ('probe-sanitizers', [sys.executable, 'tools/test_probe_integration.py', '--sanitize']),
            ('release-build', [sys.executable, 'tools/build.py', '--all']),
            ('package-validation', [sys.executable, 'tools/test_firmware.py', '--package', 'build/pico_universal_development.uf2']),
            ('debug-build', [sys.executable, 'tools/build.py', '--config', 'Debug', '--build-dir', 'build/debug']),
        ]
        results = []
        for name, command in commands:
            print('Checking ' + name, flush=True)
            log = logs / (name + '.log')
            with log.open('w') as stream:
                result = subprocess.run(command, cwd=ROOT, stdout=stream, stderr=subprocess.STDOUT)
            if result.returncode:
                print(log.read_text()[-12000:], file=sys.stderr)
                raise SystemExit(f'{name} failed; no candidate published')
            results.append({'check': name, 'passed': True, 'log_sha256': digest(log)})
        shutil.copy2(ROOT / 'build/arduino-evidence.json', stage / 'verification/arduino-evidence.json')
        # Include source and integration tools, excluding legacy generated binaries/manuals.
        for directory in ['src', 'include', 'config', 'cmake', 'probes', 'tools', 'tests', 'examples', 'linux']:
            shutil.copytree(ROOT / directory, stage / directory,
                            ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
        (stage / 'docs').mkdir()
        for name in ['ARDUINO_LINUX.md', 'BUILDING.md', 'HARDWARE_WIRING.md', 'ARCHITECTURE.md',
                     'PRODUCTION_READINESS.md', 'LINUX_HARDWARE_TEST.md', 'PRE_HARDWARE_REVIEW.md']:
            shutil.copy2(ROOT / 'docs' / name, stage / 'docs' / name)
        for name in ['README.md', 'CMakeLists.txt', 'pico_sdk_import.cmake']:
            shutil.copy2(ROOT / name, stage / name)
        (stage / 'firmware').mkdir()
        shutil.copy2(ROOT / 'build/pico_universal_development.uf2', stage / 'firmware/pico_universal_development.uf2')
        # Full pinned source trees (including notices and initialized submodules), not toolchains.
        dependencies = ['pico-sdk', 'debugprobe', 'picorvd', 'miolink']
        provenance = {}
        source_archive = stage / 'third-party-source.tar.gz'
        def source_filter(info):
            return None if '.git' in Path(info.name).parts else info
        with tarfile.open(source_archive, 'w:gz') as archive:
            for name in dependencies:
                path = ROOT / '.tools' / name
                provenance[name] = {
                    'commit': subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip(),
                    'submodules': subprocess.check_output(['git', '-C', str(path), 'submodule', 'status', '--recursive'], text=True).splitlines()}
                archive.add(path, arcname='third-party-source/' + name, filter=source_filter)
        if source_fingerprint() != before:
            raise SystemExit('Source changed during verification; rerun before publishing a candidate')
        manifest = {'source_fingerprint': before, 'schema': 1, 'status': 'hardware-test-candidate', 'production_qualified': False,
                    'hardware_tests': 'not performed', 'created_utc': datetime.now(timezone.utc).isoformat(),
                    'git_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
                    'working_tree_dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT)),
                    'arduino_cli_version': subprocess.check_output([args.cli, 'version'], text=True).strip(),
                    'python_version': sys.version,
                    'dependencies': provenance, 'checks': results,
                    'files': {str(p.relative_to(stage)): digest(p) for p in sorted(stage.rglob('*')) if p.is_file()}}
        (stage / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        archive_path = Path(temporary) / 'candidate.tar.gz'
        with tarfile.open(archive_path, 'w:gz') as archive:
            archive.add(stage, arcname=stage.name)
        archive_path.replace(args.output)
    checksum = digest(args.output)
    args.output.with_name(args.output.name + '.sha256').write_text(checksum + '  ' + args.output.name + '\n')
    print(f'Hardware-test candidate: {args.output}\nSHA-256: {checksum}')


if __name__ == '__main__':
    main()
