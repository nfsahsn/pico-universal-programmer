#!/usr/bin/env python3
"""Install an explicit CMSIS-DAP ELF programmer profile in an Arduino core."""
import argparse
from pathlib import Path
import os
import re
import shutil
import sys
import tempfile
import fcntl

START = '# BEGIN PICO UNIVERSAL LINUX'
END = '# END PICO UNIVERSAL LINUX'
ROOT = Path(__file__).resolve().parents[1]


def merge(existing, body, profile=None):
    global_start, global_end = START, END
    start = global_start if profile is None else global_start + " " + profile
    end = global_end if profile is None else global_end + " " + profile
    return merge_block(existing, body, start, end)


def merge_block(existing, body, START, END):
    starts = list(re.finditer(r'^' + re.escape(START) + r'$', existing, re.MULTILINE))
    ends = list(re.finditer(r'^' + re.escape(END) + r'$', existing, re.MULTILINE))
    if starts or ends:
        if len(starts) != 1 or len(ends) != 1 or starts[0].start() >= ends[0].start():
            raise ValueError('Malformed or ambiguous existing Pico Universal block')
        return existing[:starts[0].start()] + START + '\n' + body + '\n' + END + existing[ends[0].end():]
    return existing + ('\n' if existing and not existing.endswith('\n') else '') + START + '\n' + body + '\n' + END + '\n'


def remove(existing, profile):
    start, end = START + ' ' + profile, END + ' ' + profile
    if not re.search(r'^(?:' + re.escape(start) + '|' + re.escape(end) + r')$', existing, re.MULTILINE):
        return existing
    checked = merge(existing, '', profile)  # Validate both markers before removal.
    return re.sub(r'^' + re.escape(start) + r'\n\n' + re.escape(end) + r'(?:\n|$)',
                  '', checked, count=1, flags=re.MULTILINE)


def safe_property(value):
    value = str(value)
    if any(c in value for c in '\r\n"{}'):
        raise ValueError('Recipe values cannot contain quotes, braces or newlines')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core', required=True, type=Path, help='directory containing platform.txt')
    parser.add_argument('--profile', default='default', help='unique programmer profile ID')
    parser.add_argument('--engine', choices=['openocd', 'esp32', 'blackmagic', 'picorvd'], default='openocd')
    parser.add_argument('--gdb', type=Path)
    parser.add_argument('--remove', action='store_true', help='remove only this managed profile')
    parser.add_argument('--speed', type=int, default=100)
    parser.add_argument('--target', help='OpenOCD target config for this board/profile')
    parser.add_argument('--transport', default='swd', choices=['swd', 'jtag'])
    parser.add_argument('--openocd', type=Path)
    parser.add_argument('--scripts', type=Path)
    parser.add_argument('--apply', action='store_true', help='write files (otherwise preview only)')
    args = parser.parse_args()
    try:
        if not sys.platform.startswith('linux'):
            raise ValueError('This installer supports Linux only')
        core = args.core.resolve(strict=True)
        if not (core / 'platform.txt').is_file() or not (core / 'boards.txt').is_file():
            raise ValueError('Core must contain platform.txt and boards.txt')
        if not re.fullmatch(r'[a-z][a-z0-9_]{0,39}', args.profile):
            raise ValueError('Profile must start with a letter and contain only lowercase letters, digits or underscores')
        key = 'pico_universal_' + args.profile
        python = safe_property(sys.executable)
        if args.remove:
            programmer = recipe = ''
        else:
            if args.engine in ('openocd', 'esp32'):
                if args.openocd is None or not args.openocd.is_file() or not os.access(args.openocd, os.X_OK):
                    raise ValueError('OpenOCD must be an executable file')
                if args.scripts is None or not args.scripts.is_dir():
                    raise ValueError('OpenOCD scripts directory does not exist')
                if not args.target or not re.fullmatch(r'[A-Za-z0-9_./-]+\.cfg', args.target) or args.target.startswith('-'):
                    raise ValueError('Invalid target configuration name')
                if not (args.scripts / args.target).is_file():
                    raise ValueError('Target configuration not found in scripts directory')
                if not (args.scripts / 'interface/cmsis-dap.cfg').is_file():
                    raise ValueError('CMSIS-DAP configuration not found')
                if args.speed < 1:
                    raise ValueError('Adapter speed must be positive')
                executable, scripts, target = map(safe_property, [args.openocd.resolve(), args.scripts.resolve(), args.target])
                uploader = safe_property(ROOT / 'tools/upload_openocd.py')
                options = f'--openocd "{executable}" --scripts "{scripts}" --target "{target}" --transport {args.transport} --speed {args.speed}'
                if args.engine == 'esp32':
                    if args.transport != 'jtag':
                        raise ValueError('ESP32 CMSIS-DAP requires JTAG')
                    # Offsets come from the selected Espressif core board properties.
                    options += ' --esp --segment "{build.bootloader_addr}" "{build.path}/{build.project_name}.bootloader.bin" --segment 0x8000 "{build.path}/{build.project_name}.partitions.bin" --segment 0xe000 "{runtime.platform.path}/tools/partitions/boot_app0.bin" --segment 0x10000 "{build.path}/{build.project_name}.bin"'
                else:
                    options += ' --image "{build.path}/{build.project_name}.elf"'
                communication = 'usb'
                label = f'CMSIS-DAP {args.transport}: {target}'
            else:
                if args.gdb is None or not args.gdb.is_file() or not os.access(args.gdb, os.X_OK):
                    raise ValueError('GDB must be an executable file with Python support')
                executable = safe_property(args.gdb.resolve())
                uploader = safe_property(ROOT / 'tools/upload_gdb.py')
                options = f'--engine {args.engine} --gdb "{executable}" --transport {args.transport} --port "{{serial.port}}" --image "{{build.path}}/{{build.project_name}}.elf"'
                communication = 'serial'
                label = args.engine
            programmer = '\n'.join([
                f'{key}.name=Pico Universal [{args.profile}] {label}',
                f'{key}.communication={communication}',
                f'{key}.program.tool={key}', f'{key}.program.tool.default={key}'])
            recipe = '\n'.join([
                f'tools.{key}.program.params.verbose=', f'tools.{key}.program.params.quiet=',
                f'tools.{key}.program.pattern="{python}" "{uploader}" {options}'])
        lock = (core / '.pico-universal.lock').open('a') if args.apply else None
        if lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
        updates = []
        for filename, body in [('programmers.txt', programmer), ('platform.local.txt', recipe)]:
            path = core / filename
            if path.is_symlink():
                raise ValueError('Refusing to replace symlink: ' + str(path))
            old = path.read_text() if path.exists() else ''
            if args.remove:
                content = remove(old, args.profile)
                if content == old:
                    continue
            else:
                # Refuse to shadow a user's unmanaged entry with the same identifier.
                outside = re.sub(r'^' + re.escape(START + ' ' + args.profile) + r'$.*?^' + re.escape(END + ' ' + args.profile) + r'$', '', old, flags=re.MULTILINE | re.DOTALL)
                prefix = key + '.' if filename == 'programmers.txt' else 'tools.' + key + '.'
                if re.search(r'^' + re.escape(prefix), outside, re.MULTILINE):
                    raise ValueError('Profile identifier already exists outside managed block: ' + key)
                content = merge(old, body, args.profile)
            updates.append((path, content))
        # Validate every destination backup before mutating either core file.
        for path, _ in updates:
            backup = path.with_name(path.name + '.pico-backup')
            if args.apply and backup.is_symlink():
                raise ValueError('Refusing symlink backup: ' + str(backup))
        for path, content in updates:
            if not args.apply:
                print(f'--- {path}\n{content}')
                continue
            if path.exists():
                backup = path.with_name(path.name + '.pico-backup')
                if backup.is_symlink():
                    raise ValueError('Refusing symlink backup: ' + str(backup))
                if not backup.exists():
                    shutil.copy2(path, backup)
            descriptor, temporary = tempfile.mkstemp(dir=core, prefix='.pico-')
            try:
                with os.fdopen(descriptor, 'w') as stream:
                    stream.write(content)
                    stream.flush()
                    os.fsync(stream.fileno())
                os.chmod(temporary, path.stat().st_mode & 0o777 if path.exists() else 0o644)
                os.replace(temporary, path)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)
            print('Updated ' + str(path))
    except (OSError, ValueError) as error:
        parser.exit(2, f'Installation error: {error}\n')


if __name__ == '__main__':
    main()
