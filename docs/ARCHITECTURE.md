# Current architecture

The supervisor selects one of three independent RP2040 executable images. It is
not a resident service after handover. All three probe builds integrate shared
status indication on core 0. Button selection and persistent saves execute only
in the supervisor; the running probe ignores the button until reset. Scratch
register 3 carries the PicoRVD fault marker; checked one-shot save requests from
older probe builds remain readable for compatibility. Hardware transitions remain
unqualified.

## Flash layout

`config/flash_layout.json` is canonical. `tools/flash_layout.py` generates the C
header, and the packager reads the JSON directly. CMake derives the supervisor's
linker size from the same layout, while flash APIs retain the physical 2 MiB bound.

| Address | Size | Purpose |
|---|---:|---|
| `0x10000000` | 60 KiB | Supervisor |
| `0x1000F000` | 4 KiB | Configuration journal A / legacy settings |
| `0x10010000` | 512 KiB | CMSIS-DAP slot |
| `0x10090000` | 512 KiB | Black Magic slot |
| `0x10110000` | 512 KiB | PicoRVD slot |
| `0x10190000` | 4 KiB | Configuration journal B |
| `0x10191000` | 444 KiB | Reserved |

The last 256-byte page of each probe slot is an integrity descriptor, reducing
executable capacity to 524,032 bytes. Each image includes boot2 at its slot base
and a vector table at base + 0x100. Applications must be linked at their assigned
addresses. The supervisor enters their reset handler directly; it does not run
their boot2 as a standalone boot image.

## Probe descriptor, version 1

The first 32 bytes of the final slot page contain eight little-endian uint32 words:

| Offset | Field |
|---|---|
| 0 | Magic `0x5550494D` |
| 4 | Version `1` |
| 8 | Linked slot base |
| 12 | Image byte count, page aligned, excluding descriptor |
| 16 | CRC-32 of all image bytes including final erased-value page padding |
| 20 | Mode: 0 CMSIS-DAP, 1 Black Magic, 2 PicoRVD |
| 24 | Flags, currently zero |
| 28 | CRC-32 of bytes 0–27 |

Remaining descriptor-page bytes are `0xFF`. CRC uses CRC-32/ISO-HDLC, matching
Python `zlib.crc32`. This detects accidental damage and incomplete writes, not
malicious replacement or authenticity. A failed update can make that slot
unavailable; the design provides recovery selection, not an A/B firmware rollback.

The packager adds descriptors, validates existing descriptors when re-packaging,
and never includes configuration sectors. It rejects images with missing pages,
wrong slot addresses, invalid vectors, and partition overflow. Link correctness
beyond these invariants must be established by the source build and target tests.

Each touched 4 KiB erase sector is emitted as a complete group of sixteen UF2
pages, filling unused bytes with `0xFF`. RP2040 BOOTSEL indexes its erase bitmap
by UF2 block number divided by sixteen, rather than by physical sector address.
Without padding, sparse slots and descriptor pages can cause incorrect erases
and corrupt otherwise valid images. Untouched sectors, including both settings
banks, are omitted. Descriptor image length excludes this extra transport padding.
Host validation models the erase/write behavior before validating the image CRCs.

## Boot and recovery

1. Normal ROM flash boot initializes XIP and starts supervisor crt0.
2. An initializer ordered `00000` runs after data/BSS initialization, before SDK
   runtime initializers. It consumes scratch registers 1/2, a request and its bitwise
   complement, and accepts only a watchdog-originated, valid one-shot request.
3. A valid request rechecks the complete descriptor/image and vectors. Assembly
   disables and clears interrupts/SysTick, installs VTOR/MSP/CONTROL, then enters
   the probe reset handler. No C stack accesses occur after switching MSP.
4. Otherwise normal SDK initialization and startup selection proceed. Scratch 0
   stores a temporary mode; flash supplies the persistent default when it is invalid.
5. GP15 debounce and LED updates run cooperatively. Short release cycles modes;
   a two-second hold saves a valid mode. The five-second timeout starts after the
   complete button/debounce activity. Invalid images show an error and remain
   selectable. A valid selection writes the checked request and asks the SDK for
   a normal flash watchdog reboot.
6. Probe startup locks the mode. Its external mode LED and onboard status LED
   remain steady; button activity cannot interrupt the running probe. A physical
   reset or power cycle returns to selection. Selection/save/error indication
   changes only the onboard LED, retaining the selected external mode LED.

Scratch registers 4–7 remain reserved for SDK/ROM reboot handling. The early hook
is tied to the pinned SDK initializer ordering, which must be reviewed on upgrades.
Hardware boot/USB behavior is still awaiting qualification.

## Persistent configuration

Two independent 4 KiB sectors hold 16 page-sized records each. Each record starts
with magic `0x55504346`, version 1, a wrapping sequence number, mode, and CRC-32 of
the preceding 16 bytes. The remainder of its page stays erased.

Reads select the newest valid sequence across both banks. Writes append to an
erased page in the current bank. On exhaustion, the other bank is erased and the
new record written there; the bank containing the previous committed record is
retained. Failed verification leaves the current scratch selection unchanged.
Saving an already committed default does not program flash.

The supervisor is single-core and excludes interrupts during flash operations.
Probe engines do not invoke this writer while another core executes from flash;
saving is available only during startup selection. Host tests simulate power loss at all 4,353
byte boundaries of bank erase plus page programming. Real flash power-cut and
brownout qualification remains required.

Legacy XOR-checksummed settings are read if no journal record exists. Migration
occurs only on an explicit save and preserves the original record until the new
format has been committed.

## PicoRVD fault recovery

The integrated PicoRVD wrapper arms an eight-second hardware watchdog before
engine initialization and feeds it from the main service loop. A checked scratch
marker identifies unexpected watchdog resets. The supervisor consumes it and
stays in interactive error recovery instead of automatically restarting a stuck
engine. A startup tap selects another mode; a successful startup long-press save
also permits a retry after release and the five-second wait. Host tests cover
marker validation and suppression of automatic boot after a fault; hardware
watchdog timing still requires qualification.
