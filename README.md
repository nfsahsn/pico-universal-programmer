# Pico Universal Programmer

A three-mode RP2040 programmer under active production engineering: CMSIS-DAP
(SWD/JTAG), Black Magic direct GDB, and CH32V003 SWIO. The hardware baseline is
the original Raspberry Pi Pico with 2 MiB flash, GP15 mode button, GP25 onboard
indicator, and optional mode LEDs on GP16–18.

**This is not yet a qualified production release.** All three probe engines build from pinned sources with shared runtime controls.
Linux Arduino upload integration and automated core checks are implemented.
Physical qualification, Windows integration and public-release review remain open.
See [production requirements and evidence](docs/PRODUCTION_READINESS.md).

## Current implementation

- Shared flash layout and an enforced 60 KiB supervisor linker region.
- Strict UF2 parsing, partition checks, atomic output replacement, and explicit
  partial-update mode. Stock binaries are never silently relocated.
- CRC-32 descriptors covering each probe image; invalid or missing images retain
  the interactive selection UI instead of executing unchecked vectors.
- One-shot watchdog reboot requests and an assembly handover before SDK runtime
  initialization, with slot validation on both sides of the reset.
- Debounced startup selection, mode LEDs, and nonblocking error indication.
- Two-sector configuration journal with CRC-32, append-only records, unchanged-save
  wear avoidance, and read migration from the legacy configuration format.
- Python packaging tests and native tests of actual firmware logic, including
  simulated power loss during configuration writes.

## Build and test

Follow [BUILDING.md](docs/BUILDING.md) for pinned dependencies, host tests and
Release/Debug ARM builds. `python tools/build.py --all` produces the supervisor,
all three slot-linked engines, and `build/pico_universal_development.uf2`.

Use **`build/pico_universal_development.uf2`** to flash the Pico. It contains the
supervisor and all three probe engines. Obsolete root-level and stock UF2 copies
have been removed, and component builds produce BIN/ELF files rather than
additional UF2 choices. The candidate archive includes this same combined firmware.

## Supervisor controls

During startup selection, a short GP15 press advances the mode. A hold of at least
two seconds saves a valid selected mode as the default. After **five seconds**
without button activity, the selected valid image boots and **the mode locks until
reset or power cycling**. Each tap restarts the five-second wait after release and
debouncing. A held button delays boot and never cycles modes repeatedly.

Only the selected external mode LED stays on, including during saves and errors.
The onboard LED blinks the mode number during selection, becomes steady when the
probe starts, flashes five times on save, and toggles every 250 ms on error.
A failed save, invalid image or detected probe watchdog fault keeps recovery
selection available instead of repeatedly rebooting. Button presses while a probe
is running are ignored, so they cannot interrupt an upload. Reset the Pico to
choose another mode. Host logic tests cover these paths; physical transitions
remain unqualified.
Physical BOOTSEL recovery remains available for reflashing the Pico.

## Documentation

- [Build and packaging instructions](docs/BUILDING.md)
- [Linux Arduino integration](docs/ARDUINO_LINUX.md)
- [Linux hardware qualification procedure](docs/LINUX_HARDWARE_TEST.md)
- [Pre-hardware bug review and regression coverage](docs/PRE_HARDWARE_REVIEW.md)
- [Current architecture and image format](docs/ARCHITECTURE.md)
- [Development firmware pin assignments](docs/HARDWARE_WIRING.md)
- [Production qualification checklist](docs/PRODUCTION_READINESS.md)

The older Arduino guide, illustrated manual and PDF describe the
original intended product and contain unverified assumptions. They are awaiting
reconciliation with the integrated probe builds. In particular, stock Raspberry Pi
Debug Probe does not supply the advertised JTAG support and uses a different UART
pin assignment. Do not treat the older diagrams as a qualified wiring specification.
