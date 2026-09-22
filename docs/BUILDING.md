# Building and verification

These commands build the supervisor and all three integrated probe engines.
A qualified combined release still requires hardware qualification and release
provenance; see
[production readiness](PRODUCTION_READINESS.md). The legacy UF2 files at repository
root are not regenerated or endorsed by this process.

## Linux x86_64

Requirements: Git, Python 3.12 or newer, network access, and a native C compiler
for host tests. Dependencies are installed under `.venv/` and `.tools/`.

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install -r tools/requirements-build.txt
python tools/bootstrap.py --probes
python tools/flash_layout.py --check
python -m unittest discover -s tests -v
python tools/test_firmware.py
python tools/test_probe_integration.py
python tools/build.py --all
python tools/test_firmware.py --package build/pico_universal_development.uf2
```

`tools/dependencies.json` pins Pico SDK 2.2.0 by commit and xPack ARM GCC
14.2.1-1.1 by archive SHA-256. Bootstrap refuses a modified SDK checkout or a
checksum mismatch. Probe commits and their submodules are pinned in the same
file. The supervisor has a 60 KiB linker limit. Its outputs are
`build/pico_universal_programmer.{elf,bin}`. Probe BIN/ELF outputs are under
`build/probes/`. The **only UF2 produced is `build/pico_universal_development.uf2`**,
containing the supervisor and all three probes. It preserves saved settings.
Omit `--all` to build only supervisor components; that does not produce or update
the combined UF2. Obsolete root-level and stock UF2 files have been removed.

The host test harness compiles actual firmware sources against GPIO, time,
watchdog and flash fakes. On a Linux host without `cc`, an optional alternative is:

```sh
python -m pip install ziglang==0.14.1
CC="python -m ziglang cc" python tools/test_firmware.py
CC="python -m ziglang cc" python tools/test_probe_integration.py
CC="python -m ziglang cc" CXX="python -m ziglang c++" python tools/build.py --all
```

These tests check logic; they do not emulate RP2040 peripheral timing or qualify
USB/debug protocols. The ARM build compiles the real SDK and assembly handover.

## Other build hosts

Install CMake 3.31.6, Ninja 1.11.1, ARM GCC 14.2.1 and the pinned SDK checkout.
Set `PICO_SDK_PATH` and place the tools on `PATH`; use `build.bat` on Windows or
invoke CMake directly. These host paths are not yet qualified in CI.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_NO_PICOTOOL=1
cmake --build build
```

## Packaging

Every input must be linked for its actual flash partition, with boot2 at the
partition base and vectors at base + 0x100. Reserve the final 256-byte probe-slot
page for the descriptor added by the packager. Copying a stock binary to a new
address does not relocate its code. Stock Debug Probe UF2 firmware is not a valid
CMSIS-DAP slot input and is deliberately rejected.

```sh
python tools/merge_firmware.py \
  --bootloader build/pico_universal_programmer.bin \
  --cmsis path/to/slot-linked-debugprobe.bin \
  --bmp path/to/slot-linked-blackmagic.bin \
  --picorvd path/to/slot-linked-picorvd.bin \
  -o build/pico_universal_development.uf2
```

All four images are required by default. `--allow-partial` explicitly permits a
maintenance update containing fewer images. It never ignores a missing requested
file. The packager checks block structure, family, addresses, partition limits,
coverage, and initial vectors before replacing the output atomically. It adds
CRC-32 image descriptors that the supervisor verifies at boot. Settings sectors
are never included. Every touched erase sector is padded to sixteen UF2 pages
to match the RP2040 BOOTSEL erase bitmap. `tools/test_firmware.py --package ...`
models BOOTSEL writes in forward, reverse and shuffled order over existing flash,
checks untouched sectors, then feeds the result to the firmware's C validator.
These checks do not prove correct linking of every absolute
reference or working probe protocols; those release gates remain open.

The canonical layout is `config/flash_layout.json`. After intentional changes,
run `python tools/flash_layout.py` and review the generated header. CI rejects a
stale header. Existing firmware compatibility and migration must be considered
before changing partition addresses.

## Linux hardware-test archive

After firmware dependencies and host compilers are installed:

```sh
python tools/bootstrap_arduino.py --cores
python tools/package_linux.py --cli .tools/arduino-cli/arduino-cli --arduino-data .tools/arduino
```

This runs packaging, firmware and probe tests; actual Arduino CLI recipe tests;
representative board-core compilations; Release builds of all engines; Debug
supervisor compilation; undefined-behavior checks on the host firmware/probe tests;
and C validation of the resulting combined UF2. Any
failed step prevents publication of a new candidate. Output is
`build/linux-candidate.tar.gz` and its `.sha256` file. Source changes during the
verification run also invalidate the candidate. An older archive, if present,
is not deleted when verification fails; check its manifest timestamp and hash.

The archive contains `manifest.json`, logs, firmware, integration tools, source
and a separate `third-party-source.tar.gz` preserving upstream notices. Extract
the latter for inspection; the regular bootstrap still fetches the pinned Git
checkouts when rebuilding. The package is for hardware qualification, not a
claim of completed production certification or unrestricted MCU support.
