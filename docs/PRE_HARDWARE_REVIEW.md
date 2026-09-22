# Pre-hardware software review

This review found additional bugs after the first Linux candidate was packaged.
The rebuilt candidate includes the fixes below. Passing the automated checks is
not evidence of physical uploads, USB timing, or a zero-bug product.

## Confirmed findings and fixes

| Area | Failure | Correction and regression coverage |
|---|---|---|
| BOOTSEL UF2 sector erases | Sparse UF2 block numbering crossed physical erase-sector boundaries. File-only validation passed, but the RP2040 loader could erase previously written code or skip erasing another sector; modeling the reported candidate corrupted all three probe images, leaving selection in error recovery without USB enumeration | Pad every touched 4 KiB sector before numbering UF2 blocks. Model the ROM erase bitmap, NOR programming and duplicate delivery; verify forward/reverse/shuffled transfers over existing flash and run the C validator on the modeled flash contents. Settings sectors remain untouched |
| Mode selection | The selection delay was 2.5 seconds and runtime button presses could reboot into another mode, contrary to the requested startup-only selection | Wait five seconds after complete button release/debounce, then lock the mode until reset; tests cover all three defaults, repeated taps, held buttons and ignored runtime taps/holds |
| Mode LED indication | Save/error patterns flashed the three mode LEDs, obscuring which mode was selected | Keep only the selected mode LED on; use the onboard LED for selection/save/error and make it steady after startup; tests check LEDs throughout save/error and runtime button activity |
| CMSIS-DAP request parsing | Truncated commands and unchecked JTAG chain counts could read/write outside their buffers; large read requests could overflow a reply | Validate actual USB lengths, command counts, chain sizes, and worst-case response sizes before execution; tests cover truncation, batches and 50,000 deterministic random packets |
| CMSIS-DAP batches | Early target errors could report partial request consumption and misalign subsequent commands; USB reset could replace request bytes during execution | Execute a private packet snapshot and advance through the previously validated command boundaries; test simulated buffer reuse during a batch |
| CMSIS-DAP USB queue | Replies could overwrite a full response ring | Wait for response space before consuming more requests |
| CMSIS-DAP SWD pins | SIO writes did not drive clock/data pins while PIO owned them | Wait for queued PIO transfers, apply GPIO output overrides, and release overrides before the next SWD operation; host tests check ownership transitions |
| CMSIS-DAP low clock rates | Delay conversion could overflow or produce zero for a division-based clock API | Use 64-bit conversion, a 1 kHz floor, and invalidate the clock cache when entering SWD |
| PicoRVD software breakpoints | Four-byte instructions could cross flash pages or be misaligned for native pointer stores; final flash instructions were rejected | Track both affected pages, copy bytes safely, validate overlap and bounds; test final and cross-page instructions |
| PicoRVD breakpoint lifecycle | Clearing breakpoints left stale page bookkeeping; failed inserts were reported as successful; hardware breakpoints were silently implemented as flash patches | Restore and clear all page state, propagate failures, make duplicate operations idempotent, and report unsupported hardware breakpoint packets correctly |
| PicoRVD connection recovery | Partial flash cache survived disconnect; reset did not synchronize the software halt state | Discard pending cache on disconnect/reset and synchronize halt state; test reconnect and reset behavior |
| PicoRVD packet parsing | Invalid checksum digits could be accepted; address ranges could wrap; malformed run/step commands could change execution | Reject invalid checksums, wrapped ranges and trailing malformed fields before mutation; resynchronize interrupted packet framing |
| PicoRVD USB output | Full USB buffers could drop reply bytes while the protocol state advanced | Pause protocol progress until space is available; keep disconnect handling active; test byte-for-byte framed output during stalls |
| PicoRVD development console | UART command input exposed upstream erase/test commands and could interfere with debugger state | Disable the interactive console in the shipped build while retaining diagnostic output |
| Black Magic USB transport | A disconnect during blocked output could wait indefinitely; unread bytes could survive into a new session | Recheck connection state, poll disconnect during reads, and discard old buffered input; compile and test the actual generated transport code |
| OpenOCD uploads | Rebuilding files during upload could change programming/verification inputs | Copy all inputs to private temporary files, then validate and program those snapshots |
| Segmented uploads | Separate generic `program` calls could erase an earlier segment even when byte ranges did not overlap | Require an ELF for generic multi-region uploads; reject Espressif segments sharing a 4 KiB erase sector |
| Arduino profile removal | Removal failed when the final marker lacked a newline, or could confuse a longer profile name with the requested one | Match complete marker lines, support end-of-file markers and preserve other profiles |
| Candidate packaging | A selected Arduino data directory was not passed to STM32 and WCH board checks | Pass the selected directory to both compile/integration checks |

Breakpoint packet semantics were checked against the
[GDB remote protocol documentation](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Packets.html).
Flash command behavior was checked against the
[OpenOCD flash commands](https://openocd.org/doc/html/Flash-Commands.html).
The BOOTSEL erase model follows Raspberry Pi's
[RP2040 boot ROM](https://github.com/raspberrypi/pico-bootrom/blob/master/bootrom/virtual_disk.c).
Raspberry Pi's [elf2uf2 converter](https://github.com/raspberrypi/picotool/blob/master/elf2uf2/elf2uf2.cpp)
documents and implements the same sector-padding workaround.

## Verification

The package pipeline runs Python unit tests; supervisor tests including simulated
interrupted saves; CMSIS-DAP bindings and parser checks; actual PicoRVD packet,
server and breakpoint code with fake target memory; and actual Black Magic
transport code with fake USB/RTOS services. Supervisor and probe tests also run
with undefined-behavior sanitization enabled. The new breakpoint regression
failed against the previous unpatched implementation and passes after the fix.

The real RISC-V GDB test loads the compiled CH32V003 sketch through the actual
PicoRVD server with simulated target memory. All loaded sections must match,
and an injected mismatch must fail. STM32, ESP32 and CH32V003 Arduino builds and
recipe expansion remain part of the pipeline. ARM builds and the C image
validator check the resulting combined UF2. Exact results and hashes are stored
in the candidate archive's manifest and verification logs.

UF2 validation now simulates BOOTSEL erases and NOR writes before the C validator
reads the resulting flash. Previously the harness copied payloads directly into
an erased array, which could not detect this failure. The new write-model check
failed against the earlier candidate. Regression tests retain an unsafe sparse
package example to demonstrate the corruption, and check preservation of all
untouched sectors during complete and partial updates. This remains a write-path
model, not physical USB or processor emulation.

AddressSanitizer was not available in the local compiler environment. The host
tests do not execute the physical SWD/SWIO/JTAG engines or prove USB concurrency
and timing. No exhaustive audit of every upstream MCU driver is claimed.

## Still required on hardware

Follow `LINUX_HARDWARE_TEST.md`, including uploads/readback, mode changes,
disconnect/reconnect, power interruption and debugger halt/step/resume. Include
SWD direct pin control, JTAG chain discovery, USB stalls, breakpoint removal on
disconnect, and repeated upload after an interrupted transfer. Observe watchdog
recovery with a missing/stalled CH32 target. These tests are still pending.

PicoRVD implements software breakpoints by temporarily patching target flash;
hardware breakpoint packets are unsupported. For its GDB breakpoint tests use
`set breakpoint auto-hw off` before inserting a normal `break` command, and check
that removing breakpoints/disconnecting restores the original code.

The product remains a Linux hardware-test candidate. Windows qualification,
electrical design, target-family qualification and public-release review remain
separate outstanding work.
