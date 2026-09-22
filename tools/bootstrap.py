#!/usr/bin/env python3
"""Install pinned Linux x86_64 build dependencies into ignored .tools/."""
import hashlib
import argparse
import json
import os
from pathlib import Path
import platform
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
LOCK = json.loads((ROOT / 'tools/dependencies.json').read_text())
TOOLS = ROOT / '.tools'


def check_sdk(path):
    actual = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != LOCK['pico_sdk']['commit']:
        raise RuntimeError(f'{path} is not the pinned SDK commit; keep custom checkouts elsewhere')
    dirty = subprocess.check_output(['git', '-C', str(path), 'status', '--porcelain',
                                     '--untracked-files=no'], text=True)
    if dirty.strip():
        raise RuntimeError(f'{path} has modified tracked files; refusing an unrepeatable build')


def check_checkout(path, spec):
    actual = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != spec['commit']:
        raise RuntimeError(f'{path}: source revision does not match dependencies.json')
    dirty = subprocess.check_output(['git', '-C', str(path), 'status', '--porcelain',
                                     '--untracked-files=no'], text=True)
    if dirty.strip():
        raise RuntimeError(f'{path}: modified source checkout; preserve custom changes elsewhere')


def ensure_probe(name, spec):
    destination = TOOLS / name
    if not destination.exists():
        with tempfile.TemporaryDirectory(dir=TOOLS, prefix=name + '-') as temp:
            checkout = Path(temp) / name
            subprocess.run(['git', 'init', str(checkout)], check=True)
            subprocess.run(['git', '-C', str(checkout), 'fetch', '--depth', '1',
                            spec['url'], spec['commit']], check=True)
            subprocess.run(['git', '-C', str(checkout), 'checkout', '--detach', 'FETCH_HEAD'], check=True)
            check_checkout(checkout, spec)
            os.replace(checkout, destination)
    check_checkout(destination, spec)
    if spec['submodules']:
        subprocess.run(['git', '-C', str(destination), 'submodule', 'update', '--init',
                        '--depth', '1', *spec['submodules']], check=True)
        check_checkout(destination, spec)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probes', action='store_true', help='also fetch all pinned probe sources')
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'AMD64'):
        raise SystemExit('Automatic bootstrap currently supports Linux x86_64. See docs/BUILDING.md.')
    TOOLS.mkdir(exist_ok=True)
    sdk = TOOLS / 'pico-sdk'
    if not sdk.exists():
        with tempfile.TemporaryDirectory(dir=TOOLS, prefix='sdk-') as temp:
            checkout = Path(temp) / 'pico-sdk'
            subprocess.run(['git', 'clone', '--depth', '1', '--branch', LOCK['pico_sdk']['tag'],
                            LOCK['pico_sdk']['url'], str(checkout)], check=True)
            check_sdk(checkout)
            os.replace(checkout, sdk)
    check_sdk(sdk)
    gcc = LOCK['arm_gcc_linux_x86_64']
    destination = TOOLS / gcc['directory']
    # Always check the archive against the committed checksum. An extracted
    # directory alone is not evidence of a verified download.
    archive = TOOLS / Path(gcc['url']).name
    if not archive.exists():
        with tempfile.NamedTemporaryFile(dir=TOOLS, suffix='.download', delete=False) as stream:
            temporary = Path(stream.name)
        try:
            urllib.request.urlretrieve(gcc['url'], temporary)
            if hashlib.sha256(temporary.read_bytes()).hexdigest() != gcc['sha256']:
                raise RuntimeError('ARM toolchain download checksum mismatch')
            os.replace(temporary, archive)
        finally:
            temporary.unlink(missing_ok=True)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != gcc['sha256']:
        raise RuntimeError(f'{archive}: checksum mismatch; do not use this toolchain')
    if not destination.exists():
        with tempfile.TemporaryDirectory(dir=TOOLS, prefix='gcc-') as temp:
            with tarfile.open(archive) as source:
                source.extractall(temp, filter='data')
            os.replace(Path(temp) / gcc['directory'], destination)
    print(f'SDK: {sdk} ({LOCK["pico_sdk"]["commit"]})')
    print(f'ARM GCC: {destination / "bin"}')
    if args.probes:
        subprocess.run(['git', '-C', str(sdk), 'submodule', 'update', '--init', '--depth', '1',
                        'lib/tinyusb'], check=True)
        for name, spec in LOCK['probes'].items():
            ensure_probe(name, spec)


if __name__ == '__main__':
    main()
