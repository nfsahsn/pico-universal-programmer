#!/usr/bin/env python3
"""Install checksum-pinned Arduino CLI locally; optionally install test cores."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import platform
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cores', action='store_true', help='install pinned STM32 and ESP32 test cores locally')
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() != 'x86_64':
        parser.error('Automated tool bootstrap currently supports Linux x86_64')
    lock = json.loads((ROOT / 'tools/dependencies.json').read_text())
    spec = lock['arduino_cli_linux_x86_64']
    directory = ROOT / '.tools/arduino-cli'
    directory.mkdir(parents=True, exist_ok=True)
    archive = directory / 'download.tar.gz'
    if not archive.exists() or hashlib.sha256(archive.read_bytes()).hexdigest() != spec['sha256']:
        data = urllib.request.urlopen(spec['url'], timeout=60).read()
        if hashlib.sha256(data).hexdigest() != spec['sha256']:
            raise SystemExit('Arduino CLI archive checksum mismatch')
        archive.write_bytes(data)
    # Extract only the requested regular file; never trust archive paths or links.
    with tarfile.open(fileobj=io.BytesIO(archive.read_bytes()), mode='r:gz') as source:
        member = source.getmember('arduino-cli')
        if not member.isfile():
            raise SystemExit('Arduino CLI is not a regular archive member')
        executable = directory / 'arduino-cli'
        pending = directory / '.arduino-cli.new'
        pending.write_bytes(source.extractfile(member).read())
        pending.chmod(0o755)
        pending.replace(executable)
    data_dir = ROOT / '.tools/arduino'
    data_dir.mkdir(exist_ok=True)
    subprocess.run([str(executable), '--config-dir', str(data_dir), 'core', 'update-index'], check=True)
    if args.cores:
        for spec in lock['arduino_test_cores'].values():
            subprocess.run([str(executable), '--config-dir', str(data_dir), 'core', 'install',
                            spec['package'] + '@' + spec['version'], '--additional-urls', spec['index_url']], check=True)
    print('Arduino CLI: ' + str(executable))
    print('Isolated data directory: ' + str(data_dir))


if __name__ == '__main__':
    main()
