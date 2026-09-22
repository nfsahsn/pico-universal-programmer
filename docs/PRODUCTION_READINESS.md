# Production readiness

Status: development; no production release has been qualified. The previously
committed UF2 files are legacy artifacts, not outputs of a verified release pipeline.

The product objective remains a three-mode original-Pico programmer: CMSIS-DAP
with ARM SWD and the advertised JTAG targets, Black Magic direct GDB, and
CH32V003 SWIO, with physical mode selection, persistent defaults, status LEDs,
and tested Arduino upload workflows. Unsupported claims must not be presented
as working features while integration is in progress.

## Release requirements and evidence

| Requirement | Required evidence | Current status |
|---|---|---|
| Reproducible supervisor and three probe builds | Pinned sources, patches, toolchain, clean build and artifact hashes | All engines build locally from pinned sources; candidate manifests include hashes; independent clean rebuild comparison pending |
| Correct flash layout and executable placement | Shared layout, linker limits, rejecting packager, automated boundary tests | All four images build within assigned slots; combined package passes C image validation |
| Image integrity and recoverable boot | Corruption tests, verified handover, invalid-image recovery on hardware | CRC descriptors, early handover and host recovery tests pass; hardware pending |
| Five-second startup selection and runtime mode lock | Shared probe integration and hardware transition tests for all modes | Host tests cover timeout restarts, held buttons, stable LEDs and ignored runtime presses; hardware pending |
| Persistent defaults survive interrupted writes | Versioned redundant records, corruption and power-cut tests | Two-bank journal passes 4,353 simulated interruption points; physical tests pending |
| ARM SWD and JTAG target support | Pin/configuration review and successful flash/readback/debug on target matrix | Incomplete |
| Black Magic and CH32V003 operation | Direct GDB connect, flash/readback, reset and debug on hardware | Incomplete |
| Arduino workflows | Pinned core/tool versions, expanded command tests and real uploads | Linux recipes pass real CLI expansion and failure tests; STM32/ESP32/CH32V003 smoke builds pass; hardware and Windows pending |
| Repeatable release process | CI, source provenance, notices, checksums and explicit qualification gate | Linux candidate packager verifies builds/tests, includes pinned third-party source and hashes; public-release review and hardware qualification pending |
| Accurate user documentation | One maintained source and verified wiring, installation and recovery steps | Incomplete |

Local validation now includes Python packaging tests, native C tests using actual
firmware logic, generated CMSIS-DAP GPIO/SWJ integration tests, ARM builds of all
three engines, combined-package validation by the C boot validator, and
Release/Debug supervisor builds with the pinned SDK and toolchain. CI is configured but has not been run by a remote service in this
session. No physical Pico or target board has yet been exercised.

## Qualification matrix

Record exact board revisions, flash size, target part/package, host OS, tool
versions, firmware hashes, wiring and test output. At minimum test original
RP2040 Pico (2 MiB), an RP2040 SWD target, STM32 SWD target, ESP32 JTAG target,
and CH32V003 SWIO target. Add every advertised WCH/ARM family before claiming
support for that family. Test Windows and Linux host installation/upload;
declare any other host unqualified until tested.

For each mode test cold boot, reset, selection, USB enumeration, flashing,
readback verification, debug halt/step/resume, target reset, disconnect/reconnect,
and ignored mode-button presses during an active session. Reset to change modes.
Exercise missing/corrupt images,
configuration corruption, interrupted save/update, held and bouncing buttons,
and repeated switching. Software-only tests do not substitute for these gates.

## Work sequence

1. Enforce shared flash geometry and strict atomic packaging with negative tests.
2. Pin the toolchain/SDK, constrain linking, build and test supervisor recovery.
3. Add image integrity and redundant configuration with fault-injection tests.
4. Build all probe engines from pinned sources with slot and control integration.
5. Implement and verify Arduino integration against pinned board cores.
6. Reconcile wiring and manuals with actual probe configurations; qualify hardware.
7. Publish a release only after all evidence above exists and passes.

## Linux software candidate

`tools/package_linux.py` reruns the automated checks and emits a hardware-test
archive, never a production-qualified release. Its manifest records each check,
file hashes, source fingerprint, Git state and dependency revisions. The archive
includes the integrated source, host upload tools, documentation, firmware and
pinned third-party source trees with their original notices. This is evidence
for qualification; no public-release licensing determination is implied.

The Linux upload backends cover OpenOCD ELF, Espressif multi-image programming,
Black Magic GDB and PicoRVD GDB. Target drivers remain specific to supported
MCUs; SWIO in this pinned engine is CH32V003-specific. No universal all-MCU
support claim is made. See `LINUX_HARDWARE_TEST.md` for the physical test gates.

See [the pre-hardware review](PRE_HARDWARE_REVIEW.md) for additional bugs found
and corrected after the first candidate, regression coverage, and remaining limits.
