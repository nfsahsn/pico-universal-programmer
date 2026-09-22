# Linux Arduino IDE integration

Status: hardware-test candidate, not a production-qualified release. Linux x86_64
is the automated build host. AVR is excluded. MCU support depends on the probe
engine, host target driver and electrical interface; sharing SWD/JTAG/SWIO alone
does not establish support.

## Install tools

Use Python 3.12 or later, Git, and Arduino IDE 2.x. For a reproducible local CLI
and representative board cores:

```sh
python3 tools/bootstrap_arduino.py --cores
```

This installs checksum-pinned Arduino CLI 1.5.1 under `.tools/arduino-cli` and
STM32 core 2.12.0 / ESP32 core 3.3.11 / WCH core 1.0.4 under `.tools/arduino`. These are isolated
from the cores installed by your normal Arduino IDE. For IDE use, install the
same board-core versions through Boards Manager and select their actual core
paths below. The installer requires explicit paths; it never guesses a core.

CMSIS-DAP uses a compatible OpenOCD executable and its matching scripts directory.
ESP32 requires Espressif OpenOCD. Black Magic/PicoRVD require a GDB with Python
support and the target architecture enabled. `gdb-multiarch` is one possible
Linux host tool. Check the installed executable with:

```sh
gdb-multiarch -nx -nh --batch -ex 'python print("Python support available")'
```

## CMSIS-DAP SWD/JTAG profile

Example for STM32F4; replace VERSION and tool paths with your installed paths:

```sh
python3 tools/install_arduino_linux.py \
  --core "$HOME/.arduino15/packages/STMicroelectronics/hardware/stm32/VERSION" \
  --profile stm32f4 --engine openocd --transport swd \
  --openocd /absolute/path/to/openocd \
  --scripts /absolute/path/to/openocd/scripts \
  --target target/stm32f4x.cfg
```

The command previews the changes. Add `--apply` to install. This profile programs
an ELF using its linked addresses, verifies it, then resets the target. The same
backend accepts other OpenOCD target files and `--transport jtag`. Start with the
default 100 kHz; increase `--speed` only after reliable target tests.

## ESP32 profile

```sh
python3 tools/install_arduino_linux.py \
  --core "$HOME/.arduino15/packages/esp32/hardware/esp32/3.3.11" \
  --profile esp32_jtag --engine esp32 --transport jtag \
  --openocd /absolute/path/to/espressif/openocd \
  --scripts /absolute/path/to/espressif/openocd/scripts \
  --target target/esp32.cfg --apply
```

Select the target file for the actual chip, not merely the ESP32 product name.
The classic ESP32 example is not a claim that the user's unspecified module is
that variant. The recipe follows the tested core's standard serial-upload image
layout: board-defined bootloader offset, partition table at 0x8000, boot_app0 at
0xe000 and application at 0x10000. All four are verified through `program_esp`.
Overlapping or missing inputs fail before OpenOCD starts. It does not flash the
padded merged image or erase the entire flash as a separate step.

Custom image layouts, secure boot, encrypted images and locked JTAG devices need
separate qualified profiles. This recipe does not provision keys or change eFuses.

## Black Magic and PicoRVD profiles

Use the target's installed Arduino core directory and a suitable GDB:

```sh
python3 tools/install_arduino_linux.py \
  --core /absolute/path/to/board/core \
  --profile bmp_swd --engine blackmagic --transport swd \
  --gdb /absolute/path/to/gdb-multiarch --apply

python3 tools/install_arduino_linux.py \
  --core /absolute/path/to/ch32v/core \
  --profile ch32_swio --engine picorvd \
  --gdb /absolute/path/to/riscv-gdb --apply
```

Black Magic scans SWD (or JTAG when selected), attaches target index 1, loads the
ELF, checks sections against the target, then pulses target reset. Connect NRST
for this reset workflow. For multi-device chains, use the standalone uploader's
`--target-index` option; the current IDE profile selects index 1.

PicoRVD connects directly without a Black Magic scan, resets/halt, loads, verifies,
resets, then detaches to resume. The integrated PicoRVD driver remains CH32V003
specific. Other WCH chips need compatible engine drivers, not just a renamed
Arduino profile. The CH32V003F4 smoke sketch compiles with WCH core 1.0.4. Its actual ELF also
passes upload and verification using a real RISC-V GDB and the PicoRVD protocol
engine against simulated target memory; deliberate corruption fails verification.
Physical programming remains unqualified.

A mismatch, empty verification report, tool error or timeout returns failure.
GDB auto-loading is disabled. Upload processes default to a 180-second timeout;
the standalone upload tools allow changing it. Large/slow targets may need longer.

## Arduino IDE operation and profile management

Restart the IDE after installation. Select the exact board and the profile under
**Tools > Programmer**. For GDB engines select the probe's GDB serial port under
**Tools > Port** (Black Magic has a separate UART port). Then choose
**Sketch > Upload Using Programmer**. The ordinary Upload button retains the
board core's own upload method.

Profiles are independent named blocks in `programmers.txt` and
`platform.local.txt`. Arduino CLI did not discover profiles placed only in
`programmers.local.txt`; the installer intentionally updates `programmers.txt`.
Existing entries are preserved and first-install backups use `.pico-backup`.
Rerunning the same profile updates it; different profile names coexist. Remove one:

```sh
python3 tools/install_arduino_linux.py --core /path/to/core \
  --profile stm32f4 --remove --apply
```

A profile applies to every board in that core when selected. Select the matching
profile for the chip. Reinstall after a core upgrade. Each file replacement is
atomic; the two-file operation is not a single transaction. Rerun after interruption.
Installations are serialized by a lock. Absolute tool/checkout paths are recorded;
keep those locations stable or reinstall after moving the package.

## Linux USB access

Inspect without changing hardware:

```sh
python3 tools/linux_doctor.py --openocd /path/to/openocd --gdb /path/to/gdb
```

For systemd/udev desktop sessions, an administrator can install the supplied rule:

```sh
sudo install -m 0644 linux/60-pico-universal.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
```

Unplug/replug the Pico afterwards. Rules grant the active desktop session access
to the three development USB identities and discourage ModemManager probing.
Headless sessions may require your distribution's serial-device group policy.
Do not run Arduino IDE as root. Flatpak/Snap confinement can independently block
USB or tool execution; use a native installation for initial qualification.

## Verification

```sh
python3 -m unittest discover -s tests -v
python3 tools/test_arduino_integration.py
```

The CLI integration test uses an isolated sketchbook, recording tools and a
pseudo-terminal. It exercises all four recipes and failure propagation without
writing to a real target or modifying installed cores. It also accepts `--platform`,
`--fqbn`, `--images` and `--engines` to test copied real-core metadata/artifacts.
Locally, smoke sketches compiled for STM32F407VE, classic ESP32 and CH32V003F4;
their real-core recipes passed recording-tool upload checks. Physical uploads, GUI
behavior and USB permissions remain hardware qualification tasks.

OpenOCD uploads use private snapshots of the selected build files. Generic
multi-region uploads require one ELF: separate binary programming commands can
erase each other's data within a flash sector. Espressif binary segments must
occupy separate 4 KiB erase sectors. The standard recipe satisfies this constraint.
Additional review findings are recorded in [PRE_HARDWARE_REVIEW.md](PRE_HARDWARE_REVIEW.md).

References: [Arduino platform specification](https://docs.arduino.cc/arduino-cli/platform-specification),
[OpenOCD flash commands](https://openocd.org/doc/html/Flash-Commands.html).
