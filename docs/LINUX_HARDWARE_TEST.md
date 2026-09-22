# Linux candidate hardware qualification

Use a spare target whose flash may be overwritten. This checklist records evidence;
none of its hardware checks have been completed by software-only tests.

## Record the setup

Record candidate archive SHA-256, firmware SHA-256 from `manifest.json`, host
OS/kernel, native Arduino IDE version, CLI/core/tool versions, exact target chip
marking, board revision, power supply, wiring and debug clock. Save verbose upload
output and `tools/linux_doctor.py` output. A generic family name is insufficient.

## Prepare the programmer

1. Verify the archive's `.sha256` file using `sha256sum -c` in its directory.
2. Extract the package and hold Pico BOOTSEL while connecting USB.
3. Copy `firmware/pico_universal_development.uf2` to the Pico's mounted RPI-RP2
   drive. Do not use legacy root-level UF2 files from the original repository.
4. Fit the GP15-to-GND button and optional mode LEDs as documented in
   `HARDWARE_WIRING.md`. Confirm cold boot and selection before connecting a target.
   Each tap must advance exactly one mode and restart a five-second wait after
   release. Then the selected mode LED and onboard LED must stay on. Confirm a
   held button delays startup without repeated cycling. Reset to select again.
   Repeat for all three modes and confirm the matching USB device enumerates;
   a solid mode LED alone indicates selection, not successful handover. An evenly
   blinking onboard error LED with no USB device is a failed startup. Candidates
   made before the BOOTSEL sector-padding fix could produce this in every mode;
   reflash the rebuilt complete UF2 to replace the damaged probe images.
5. Install Linux access rules if needed. Confirm the current USB mode with the
   doctor. Reconnect after mode changes; serial device numbers can change.

## First target: STM32F407VE

Confirm the exact chip marking and choose its matching board variant. The compiled
example used STM32 core 2.12.0, Generic STM32F4 / Black F407VE. It is a representative
profile, not proof of the user's board identity.

Connect GP2 to SWCLK, GP3 to SWDIO, GP6 to NRST and common ground. Power the target
appropriately with 3.3 V signal levels. Use short wires and start at 100 kHz.
Install the `target/stm32f4x.cfg` profile with the matching OpenOCD distribution.
Select CMSIS-DAP mode, compile `examples/UploadSmoke`, and use Upload Using Programmer.

Pass criteria: target identified correctly; erase/program/readback verification
succeeds; upload process returns success; target starts after reset. An LED is
observable only when the chosen board defines and wires LED_BUILTIN correctly.
Repeat with a changed sketch to rule out a previously running image.

Repeat through Black Magic with an ARM-capable GDB and the GDB serial interface.
Record scan output and verify that section comparisons all match.

## ESP32 and CH32V003

Identify the exact ESP32 variant before wiring JTAG or selecting an OpenOCD target
file. Test the four-image ESP32 profile on a spare board with the standard tested
core layout. Verify that the application boots after power cycling. GPIO straps,
JTAG availability and flash voltage depend on the actual module.

For CH32V003 use GP28 to PD1/SWIO with a 1-kilohm pull-up to the target's 3.3 V rail
and common ground. Select PicoRVD and a RISC-V GDB. Qualify the installed board core
and its ELF linking before relying on the IDE profile.

## Reliability and failure cases

For each qualified combination record results for:

- 20 consecutive changed-image uploads with successful verification.
- 20 reset/selection cycles, confirming the five-second wait, USB re-enumeration
  and a subsequent successful upload.
- Cold boots, long-press default saves, then power removal and default restoration.
- Wrong selected mode, missing target, disconnected signal and USB unplug during
  upload: each must report failure, never false success; retry must be recoverable.
- Tapping and holding the mode button during an upload: the mode stays locked,
  USB stays connected and the upload completes. Resetting during an upload must
  instead make the host report failure and reopen startup selection.
- Debug halt/step/resume and target reset when advertised by that engine/target.
  For PicoRVD use `set breakpoint auto-hw off` and normal software `break`
  commands; hardware breakpoints are unsupported. Verify original flash contents
  after removing breakpoints and disconnecting.
- Missing/corrupt probe image recovery and BOOTSEL restoration of the combined UF2.
- Interrupted default-setting writes and interrupted firmware updates on a spare
  programmer, confirming recovery behavior rather than assuming rollback exists.

Close serial monitors before GDB uploads. After timeout or unplugging, restore
connections and reset/reselect the probe before retrying. The upstream engines
still contain target-dependent blocking operations. PicoRVD has an eight-second
service-loop watchdog that returns to supervisor recovery; exercise it with a
missing or unresponsive target. Black Magic and CMSIS-DAP also need disconnect
and retry qualification.

Do not promote this candidate to production based on a single successful upload.
The compatibility list should name only qualified combinations. Public release
also needs final hardware qualification, product USB identities, and a completed
project/third-party licensing review.
