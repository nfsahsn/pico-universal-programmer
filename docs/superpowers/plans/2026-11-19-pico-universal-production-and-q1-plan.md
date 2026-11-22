# Pico Universal Production Product & Q1 Research Paper Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a commercial consumer-grade, RP2040-based universal USB programmer/debugger (STM32 F1/F4, RP2040, classic ESP32, WCH CH32V003) with an approved Raspberry Pi USB PID, unified zero-reboot composite firmware, cross-platform Go `pu` CLI + turnkey Arduino IDE 2 / VS Code integration, custom 4-layer production PCB, factory QC jig, and a submission-ready Q1 research manuscript for IEEE Transactions on Instrumentation and Measurement (TIM) / IEEE Access.

**Architecture:** Dual-Track Phased Qualification unified in one monorepo (`nfsahsn/pico-universal-programmer`). Track A merges `origin/linux-focused` and `pico-universal-setup` into a single-firmware RP2040 composite USB device (`VID 0x2E8A`, `PID PU_USB_PID`) exposing CMSIS-DAP v2, CH32V003 SWIO GDB/`PUID1`, DMA UART bridge, and Control/Telemetry arbitrated by a deterministic `engine_owner` FSM across Core 0/Core 1 and PIO0/PIO1, paired with a zero-dependency Go host CLI (`pu`). Track B delivers the 4-layer consumer production PCB (`PU_BOARD_PRO_PCB`) with 1.8 V–5.0 V directional level shifting, ESD/overcurrent protection, switchable CH32V003 SWIO pull-up/power-cycle unbrick, and a pogo-pin factory QC fixture. Track C executes automated comparative benchmarks (E1–E6) against commercial probes and produces the IEEEtran LaTeX paper + Zenodo replication bundle.

**Tech Stack:** C11 / C++17 (Raspberry Pi Pico SDK, TinyUSB, RP2040 PIO), Python 3.10+ (build packaging, UF2 ROM model, statistical plotting), Go 1.22+ (static `pu` CLI for Linux & Windows), OpenOCD & `riscv-none-elf-gdb`, KiCad 8 (4-layer PCB & schematic), FreeCAD/OpenSCAD (enclosure), GitHub Actions CI (ASan/UBSan + `go test -race`), IEEEtran LaTeX + Python matplotlib/scipy.

**Spec:** `docs/superpowers/specs/2026-11-19-pico-universal-production-and-q1-plan-design.md`

---

## Global Constraints

- **Target v1 MCU Matrix:** STM32 F1/F4 (`F103C8/CB` incl. clone detection, `F401CC/RE`, `F411CE/RE`, `F407VE/VG`, `F446RE`), Raspberry Pi Pico/Pico W (`RP2040`; expansion `RP2350`), Classic ESP32-WROOM-32 (incl. Zbit `ZB25VQ32` QIO flash; expansion `ESP32-S3`), and WCH `CH32V003F4P6` (incl. PD1-reused/read-protected unbrick).
- **USB Identity:** Raspberry Pi VID `0x2E8A`, configurable approved PID macro `PU_USB_PID` (CMake cache variable, never hardcoded to an unapproved ID), unique 16-hex serial derived from QSPI flash 64-bit UID, Microsoft OS 2.0 (WCID) WinUSB zero-driver descriptors on Windows 10/11, and `udev` + `ID_MM_DEVICE_IGNORE="1"` rules on Linux.
- **USB Interface Map:** 6 interfaces on EP0–EP6: `IF 0` CMSIS-DAP v2 Bulk (EP1), `IF 1–2` CH32 SWIO GDB/`PUID1` CDC (EP2/EP3), `IF 3–4` Target UART CDC (EP4/EP5), `IF 5` Control/Telemetry Vendor (EP6).
- **Board Dual-Profile Compatibility:** Single `config/board_pinout.h` supporting both `PU_BOARD_PICO` (preserving legacy `GP28` SWIO, `GP0/1` UART, `GP2–5` SWD/JTAG, `GP15` button, `GP25` LED) and `PU_BOARD_PRO_PCB` (`GP6` SWDIO_DIR, `GP7` NRST, `GP8` SWIO, `GP9` 1 kΩ PU, `GP10/11` VTARG switch, `GP12–14` RGB, `GP26/27` ADC telemetry).
- **Flash Layout:** Shared 2 MiB base map (`config/flash_layout.json`): `bootloader` (`0x000000`, 60 KiB), `journal_a` (`0x00F000`, 4 KiB), `app_slot_a` (`0x010000`, 960 KiB), `app_slot_b` (`0x100000`, 960 KiB), `journal_b` (`0x1F0000`, 4 KiB), `factory_meta` (`0x1FF000`, 4 KiB). All UF2 outputs must pad touched 4 KiB sectors before block numbering.
- **Consumer Hardware Constraints:** Target BOM $8–$12 USD @ 100–500 units; directional level shifters (`SN74AXC4T774` / `SN74LVC2T45`, 1.8 V–5.0 V with `I_off`), 22–33 Ω series damping, low-capacitance TVS ESD (<1 pF), USB-C with 5.1 kΩ CC1/CC2, 500 mA PPTC polyfuse, `AP22811`/`TPS2553` VTARG load switch, RoHS-compliant parts.
- **Host OS & IDE Scope:** Linux x86-64/ARM64 + Windows x86-64 for v1; Arduino IDE 2 turnkey upload + debug primary; VS Code / OpenOCD / GDB supported.
- **Verification Discipline:** All firmware C/C++ logic compiled and tested on host under ASan + UBSan; all Go packages pass `go test -race ./...`; no unsupported hardware claims prior to passing HIL qualification; commercial license audit (SPDX SBOM) required before public release.
- **Q1 Publication Standard:** All experimental datasets (E1–E6) collected via automated `pu bench` scripts with $N \ge 30$ (throughput) and $N \ge 100$ (latency) trials, reporting mean, SD, 95% CI, p95/p99, oscilloscope waveforms, and archived in a reproducible Zenodo bundle.

## Review Focus

1. **Simultaneous USB contention across CMSIS-DAP (IF 0) and CH32 SWIO (IF 1–2):** If a user leaves Arduino IDE's CH32 GDB session open and starts an STM32 OpenOCD upload, the second interface must receive an immediate deterministic busy error (`DAP_ERROR` / `PUID1:ERR_ENGINE_BUSY`) without touching PIO registers or corrupting the active transfer; pinned by Task 3 (`test_engine_owner_rejects_cross_interface_contention`).
2. **Floating-high (`0xFFFFFFFF`) and shorted-low (`0x00000000`) silent CH32V003 SWIO wires:** A dead/unpowered or pull-up-missing CH32V003 wire clocks in all-1s or all-0s on PIO; the firmware must never misclassify either bit pattern as "already halted" or "blank flash" and must emit the 5-cause remediation diagnostic; pinned by Task 5 (`test_ch32_silent_bus_never_misclassified`).
3. **Mid-transfer USB unplug or host process crash:** If OpenOCD or GDB dies without sending `DAP_Disconnect` or GDB detach, the probe must detect USB suspend/unmount or wire deadline expiry, drain PIO FIFOs, release SIO overrides, disable the SWIO 1 kΩ pull-up, and return pins to Hi-Z so the next session succeeds without replugging; pinned by Task 3 (`test_engine_owner_teardown_on_usb_reset_and_deadline`) and Task 4 (`test_dap_guard_snapshot_survives_buffer_reuse`).
4. **Overlapping ESP32 flash segments or mutated ELF files during Arduino rebuild:** If a user clicks Upload while a background build modifies the ELF, or supplies ESP32 binary segments sharing a 4 KiB erase sector, `pu upload` must copy inputs to a private atomic snapshot and reject overlapping sectors before touching target flash; pinned by Task 8 (`TestUploadSnapshotAndSectorOverlapRejection`).
5. **Hardware `VTARG` overcurrent fault during active flashing:** If a consumer accidentally shorts target 3.3 V to GND mid-session on `PU_BOARD_PRO_PCB`, the load-switch fault IRQ (`VTARG_FAULT_N`) must transition `engine_owner` to `FAULT`, tri-state all target pins immediately, latch the red LED fault pattern, and reject new debug operations until the host explicitly clears the fault; pinned by Task 6 (`test_telemetry_overcurrent_forces_engine_fault`).

---

## Repository File Structure Map

```text
pico-universal-programmer/
├── .github/workflows/ci.yml              # Host ASan/UBSan C/C++, Go race, Python UF2, ARM build CI
├── CMakeLists.txt                        # Top-level RP2040 CMake (bootloader + unified firmware + host tests)
├── config/
│   ├── board_pinout.h                    # PU_BOARD_PICO vs PU_BOARD_PRO_PCB GPIO & feature map
│   ├── flash_layout.json                 # Single source of truth for 2 MiB / 4 MiB A/B partition table
│   └── flash_layout.h                    # Generated C header from flash_layout.json
├── bootloader/
│   ├── main.c                            # Immutable A/B CRC32 chainloader & recovery button handler
│   ├── handover.S                        # Clean IRQ-off, VTOR, MSP, BX handover assembly
│   ├── image_validator.c / .h            # CRC32 descriptor & slot bounds verification
│   └── journal.c / .h                    # Two-bank power-cut-safe persistent active-slot journal
├── firmware/
│   ├── main.cpp                          # Core 0 supervisor (TinyUSB, telemetry, LEDs) + Core 1 launcher
│   ├── usb_descriptors.c / .h            # VID 0x2E8A + PU_USB_PID, WCID MS OS 2.0, IF0..IF5 descriptors
│   ├── tusb_config.h                     # TinyUSB composite buffer & endpoint config
│   ├── engine_owner.c / .h               # Formal mutual-exclusion FSM (IDLE/SWD/JTAG/SWIO/RECOVERY/FAULT)
│   ├── dap_engine.c / .h                 # Hardened CMSIS-DAP v2 dispatcher + dap_guard + Core 1 PIO0
│   ├── swio_engine.cpp / .h              # Hardened CH32V003 PIO1 SWIO + ch32_identity + SoftBreak
│   ├── uart_bridge.c / .h                # Concurrent DMA UART0 <-> IF3-4 CDC bridge
│   ├── control_telemetry.c / .h          # IF5 Vendor Control, ADC VTREF/I_TARG, VTARG load switch & fault IRQ
│   └── pio/                              # swd.pio, jtag.pio, singlewire.pio
├── host/
│   ├── go.mod / go.sum                   # Zero-external-runtime Go CLI module
│   ├── cmd/pu/                           # main.go, setup.go, check.go, upload.go, gdbpipe.go, recover.go, bench.go, factory_qc.go
│   └── internal/
│       ├── catalog/                      # Embedded targets.json & SHA-256 lock verification
│       ├── usbprobe/                     # Linux sysfs / Windows SetupAPI VID:PID + serial discovery & flock
│       ├── puid/                         # PUID1 CH32 + SWD/JTAG ROM-table & STM32 clone fingerprinting
│       ├── uploader/                     # Atomic snapshot + OpenOCD (incl. pu-esp32.cfg) + SWIO upload
│       ├── gdbpipe/                      # Bidirectional GDB stdio/TCP relay with clean teardown
│       └── bench/                        # E1–E6 statistical runner & CSV/JSON schema writer
├── catalog/
│   ├── targets.json                      # Unified pinned cores, boards, FQBNs, tool hashes
│   └── openocd/pu-esp32.cfg              # Embedded classic ESP32 JTAG Zbit/halt/detach config
├── linux/60-pico-universal.rules         # udev uaccess + ModemManager ignore rules for 0x2E8A:<PID>
├── tools/
│   ├── gen_flash_layout.py               # Generates config/flash_layout.h from flash_layout.json
│   ├── package_uf2.py                    # 4 KiB sector-padded A/B UF2 packager + ROM erase simulator
│   └── license_audit.py                  # Generates SPDX SBOM & verifies commercial redistribution compliance
├── hardware/
│   ├── pcb/                              # KiCad 8 schematic, 4-layer PCB, BOM CSV, CPL, Gerbers
│   ├── enclosure/                        # Parametric FreeCAD/OpenSCAD snap-fit shell + pinout legend SVG
│   └── factory-jig/                      # Pogo-pin bed-of-nails fixture schematic + BOM
├── paper/
│   ├── main.tex                          # IEEEtran two-column manuscript for IEEE TIM / IEEE Access
│   ├── references.bib                    # Peer-reviewed citations (SWD/JTAG/PIO/fault-injection/probes)
│   ├── scripts/analyze_benchmarks.py     # Generates publication PDF plots, 95% CI tables, LaTeX macros
│   └── data/                             # Raw CSV/waveform datasets archived to Zenodo
└── tests/
    ├── host_c/                           # ASan/UBSan C/C++ unit + fuzz + wire-model + journal tests
    ├── python/                           # UF2 ROM-model & flash-layout generator tests
    └── hil/                              # Automated physical target-matrix & factory QC test suite
```

---

## Phase 0: Foundation, Monorepo Unification & CI Pipeline

### Task 1: Unify Repository Baseline, Flash Layout Generator & CI Pipeline

**Files:**
- Create: `config/flash_layout.json`
- Create: `config/flash_layout.h`
- Create: `config/board_pinout.h`
- Create: `tools/gen_flash_layout.py`
- Create: `catalog/targets.json`
- Create: `catalog/openocd/pu-esp32.cfg`
- Create: `linux/60-pico-universal.rules`
- Create: `.github/workflows/ci.yml`
- Test: `tests/python/test_flash_layout.py`

**Interfaces:**
- Consumes: `origin/linux-focused` (`34daa75`) proven journal/validator/packager design, `~/.gemini/antigravity/scratch/pico-universal-setup/catalog/targets.json`, `~/pu phase0/pu-esp32.cfg`.
- Produces: Generated `config/flash_layout.h` (`SLOT_BOOTLOADER_OFFSET`, `SLOT_JOURNAL_A_OFFSET`, `SLOT_APP_A_OFFSET`, `SLOT_APP_B_OFFSET`, `SLOT_JOURNAL_B_OFFSET`, `SLOT_FACTORY_META_OFFSET`, all aligned to 4 KiB); `config/board_pinout.h` (`PU_BOARD_PICO` / `PU_BOARD_PRO_PCB` macros); `PU_USB_PID` CMake cache parameter.

- [ ] **Step 1: Write failing test for shared 2 MiB A/B flash layout invariants**

```python
# tests/python/test_flash_layout.py
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

def test_flash_layout_non_overlapping_and_sector_aligned():
    layout = json.loads((ROOT / "config" / "flash_layout.json").read_text())
    sector = layout["sector_size"]
    assert sector == 4096
    assert layout["flash_size"] == 2 * 1024 * 1024
    parts = sorted(layout["partitions"], key=lambda p: p["offset"])
    cursor = 0
    for p in parts:
        assert p["offset"] % sector == 0, f"{p['name']} offset not 4 KiB aligned"
        assert p["size"] % sector == 0 or p["name"] == "bootloader"
        assert p["offset"] >= cursor, f"{p['name']} overlaps previous partition"
        cursor = p["offset"] + p["size"]
    assert cursor <= layout["flash_size"]

def test_gen_flash_layout_matches_committed_header(tmp_path):
    out = tmp_path / "flash_layout.h"
    subprocess.run(
        [sys.executable, str(ROOT / "tools" / "gen_flash_layout.py"),
         "--in", str(ROOT / "config" / "flash_layout.json"), "--out", str(out)],
        check=True,
    )
    assert out.read_text() == (ROOT / "config" / "flash_layout.h").read_text()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/python/test_flash_layout.py -v`
Expected: FAIL (`FileNotFoundError` for `config/flash_layout.json`)

- [ ] **Step 3: Create `config/flash_layout.json`, `tools/gen_flash_layout.py`, generate `config/flash_layout.h`, create `config/board_pinout.h`, import `catalog/targets.json` & `catalog/openocd/pu-esp32.cfg`, add `linux/60-pico-universal.rules` and `.github/workflows/ci.yml`**

```json
{
  "flash_base": 268435456,
  "flash_size": 2097152,
  "page_size": 256,
  "sector_size": 4096,
  "partitions": [
    {"name": "bootloader", "macro": "SLOT_BOOTLOADER", "offset": 0, "size": 61440, "executable": true},
    {"name": "journal_a", "macro": "SLOT_JOURNAL_A", "offset": 61440, "size": 4096, "executable": false},
    {"name": "app_slot_a", "macro": "SLOT_APP_A", "offset": 65536, "size": 983040, "executable": true},
    {"name": "app_slot_b", "macro": "SLOT_APP_B", "offset": 1048576, "size": 983040, "executable": true},
    {"name": "journal_b", "macro": "SLOT_JOURNAL_B", "offset": 2031616, "size": 4096, "executable": false},
    {"name": "factory_meta", "macro": "SLOT_FACTORY_META", "offset": 2093056, "size": 4096, "executable": false}
  ]
}
```

```c
/* config/board_pinout.h */
#ifndef PU_BOARD_PINOUT_H
#define PU_BOARD_PINOUT_H

#if !defined(PU_BOARD_PICO) && !defined(PU_BOARD_PRO_PCB)
#define PU_BOARD_PICO 1
#endif

#define PU_PIN_UART_TX        0u
#define PU_PIN_UART_RX        1u
#define PU_PIN_SWCLK_TCK      2u
#define PU_PIN_SWDIO_TMS      3u
#define PU_PIN_JTAG_TDI       4u
#define PU_PIN_JTAG_TDO       5u
#define PU_PIN_RECOVERY_BTN   15u

#if defined(PU_BOARD_PRO_PCB)
#define PU_HAS_LEVEL_SHIFTER  1
#define PU_HAS_VTARG_SWITCH   1
#define PU_HAS_ADC_TELEMETRY  1
#define PU_PIN_SWDIO_DIR      6u
#define PU_PIN_TARGET_NRST    7u
#define PU_PIN_SWIO           8u
#define PU_PIN_SWIO_PU_EN     9u
#define PU_PIN_VTARG_EN       10u
#define PU_PIN_VTARG_FAULT_N  11u
#define PU_PIN_LED_R          12u
#define PU_PIN_LED_G          13u
#define PU_PIN_LED_B          14u
#define PU_PIN_ADC_VTREF      26u
#define PU_PIN_ADC_ITARG      27u
#else
#define PU_HAS_LEVEL_SHIFTER  0
#define PU_HAS_VTARG_SWITCH   0
#define PU_HAS_ADC_TELEMETRY  0
#define PU_PIN_TARGET_NRST    6u
#define PU_PIN_SWIO           28u
#define PU_PIN_LED_STATUS     25u
#endif

#endif /* PU_BOARD_PINOUT_H */
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python3 -m pytest tests/python/test_flash_layout.py -v`
Expected: PASS (2 passed)

- [ ] **Step 5: Commit**

```bash
git add config/ tools/gen_flash_layout.py catalog/ linux/60-pico-universal.rules .github/workflows/ci.yml tests/python/test_flash_layout.py
git commit -m "feat(foundation): unify flash layout, board pinouts, target catalog, and CI"
```

---

## Phase 1: Unified RP2040 Composite Firmware, Fail-Safe Bootloader & Approved USB PID

### Task 2: Fail-Safe A/B CRC32 Bootloader, Two-Bank Flash Journal & Sector-Padded UF2 Packager

**Files:**
- Create: `bootloader/journal.h`, `bootloader/journal.c`
- Create: `bootloader/image_validator.h`, `bootloader/image_validator.c`
- Create: `bootloader/handover.S`, `bootloader/main.c`
- Create: `tools/package_uf2.py`
- Test: `tests/host_c/test_journal_powercut.c`
- Test: `tests/python/test_package_uf2_rom_model.py`

**Interfaces:**
- Consumes: `config/flash_layout.h` (`SLOT_BOOTLOADER_*`, `SLOT_JOURNAL_A_*`, `SLOT_JOURNAL_B_*`, `SLOT_APP_A_*`, `SLOT_APP_B_*`).
- Produces:
  - `bool pu_journal_load(pu_journal_state_t *out)`
  - `bool pu_journal_commit(const pu_journal_state_t *next)`
  - `bool pu_image_validate_slot(uint32_t slot_addr, uint32_t slot_size, pu_image_info_t *info)`
  - `tools/package_uf2.py` CLI accepting `--bootloader`, `--slot-a`, `--slot-b`, `--output` and enforcing 4 KiB sector padding + RP2040 boot ROM erase simulation.

- [ ] **Step 1: Write failing C power-cut test (4,096+ interruption points) and failing Python UF2 BOOTSEL ROM-model test**

```c
/* tests/host_c/test_journal_powercut.c */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "bootloader/journal.h"

/* Simulated 2-bank 4 KiB NOR flash where erase sets 0xFF and program clears bits;
   injected power cut truncates writes after N bytes. */
static uint8_t bank_a[4096], bank_b[4096];

int main(void) {
    memset(bank_a, 0xFF, sizeof(bank_a));
    memset(bank_b, 0xFF, sizeof(bank_b));
    pu_journal_bind_test_banks(bank_a, bank_b);

    pu_journal_state_t s0 = { .active_slot = 0, .boot_attempts = 0, .flags = 0 };
    assert(pu_journal_commit(&s0));

    pu_journal_state_t s1 = { .active_slot = 1, .boot_attempts = 1, .flags = 0 };
    for (size_t cut = 0; cut <= 128; ++cut) {
        pu_journal_inject_power_cut_after_bytes(cut);
        (void)pu_journal_commit(&s1);
        pu_journal_inject_power_cut_after_bytes(SIZE_MAX);

        pu_journal_state_t recovered = {0};
        assert(pu_journal_load(&recovered));
        assert(recovered.active_slot == 0 || recovered.active_slot == 1);
    }
    return 0;
}
```

```python
# tests/python/test_package_uf2_rom_model.py
import subprocess
import sys
from pathlib import Path
from tools.package_uf2 import simulate_rp2040_bootsel_flash, pack_unified_uf2

def test_bootsel_rom_model_preserves_adjacent_slots_under_shuffled_delivery(tmp_path):
    boot = bytes([0xAA] * 8192)
    slot_a = bytes([0x55] * 12288)
    uf2 = pack_unified_uf2(bootloader=boot, slot_a=slot_a)
    flash = simulate_rp2040_bootsel_flash(uf2, order="shuffled")
    assert flash[0:8192] == boot
    assert flash[0x10000:0x10000 + 12288] == slot_a
    # Journal sectors must remain untouched (0xFF)
    assert all(b == 0xFF for b in flash[0x0F000:0x10000])
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `cc -fsanitize=address,undefined -I. tests/host_c/test_journal_powercut.c -o /tmp/test_journal && /tmp/test_journal` and `python3 -m pytest tests/python/test_package_uf2_rom_model.py -v`
Expected: FAIL (missing headers/module)

- [ ] **Step 3: Implement `bootloader/journal.{h,c}`, `bootloader/image_validator.{h,c}`, `bootloader/handover.S`, `bootloader/main.c` (adapting the proven logic from `origin/linux-focused` to the A/B unified slot map) and `tools/package_uf2.py`**

Key requirements:
- Monotonic 64-bit sequence counter + CRC32 over each journal record; alternate writes between `journal_a` and `journal_b` so an interrupted erase/program always leaves the peer bank intact.
- Bootloader checks `boot_attempts`: if `app_slot_a` fails to clear its health flag within 3 boots, automatically rolls back to `app_slot_b`.
- `package_uf2.py` pads every touched 4 KiB erase sector with `0xFF` (or existing slot bytes) before assigning UF2 block numbers and rejects any image exceeding its partition size.

- [ ] **Step 4: Run ASan/UBSan C tests and Python ROM-model tests**

Run: `cc -Wall -Wextra -Werror -fsanitize=address,undefined -I. bootloader/journal.c bootloader/image_validator.c tests/host_c/test_journal_powercut.c -o /tmp/test_journal && /tmp/test_journal && python3 -m pytest tests/python/test_package_uf2_rom_model.py -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add bootloader/ tools/package_uf2.py tests/host_c/test_journal_powercut.c tests/python/test_package_uf2_rom_model.py
git commit -m "feat(bootloader): add A/B CRC32 chainloader, power-cut journal, and sector-padded UF2 packager"
```

---

### Task 3: Deterministic PIO & Pin Engine Arbitrator (`engine_owner` FSM)

**Files:**
- Create: `firmware/engine_owner.h`
- Create: `firmware/engine_owner.c`
- Test: `tests/host_c/test_engine_owner.c`

**Interfaces:**
- Consumes: `config/board_pinout.h` and a platform HAL callback struct (`pu_pin_hal_t`) for PIO drain, SIO override release, SWIO pull-up switch, and pin tri-state (mocked cleanly in host tests).
- Produces:
  - `typedef enum { PU_ENGINE_IDLE, PU_ENGINE_SWD, PU_ENGINE_JTAG, PU_ENGINE_SWIO, PU_ENGINE_RECOVERY, PU_ENGINE_FAULT } pu_engine_state_t;`
  - `pu_status_t pu_engine_acquire(pu_engine_state_t target, uint64_t now_us, uint32_t timeout_us);`
  - `pu_status_t pu_engine_release(pu_engine_state_t expected_owner);`
  - `void pu_engine_tick(uint64_t now_us);`
  - `void pu_engine_on_usb_reset(void);`
  - `void pu_engine_latch_fault(uint32_t fault_mask);`
  - `pu_status_t pu_engine_clear_fault(void);`
  - `pu_engine_state_t pu_engine_current(void);`

- [ ] **Step 1: Write failing tests covering Review Focus #1 (cross-interface contention) and #3 (USB reset & wire deadline teardown)**

```c
/* tests/host_c/test_engine_owner.c */
#include <assert.h>
#include <stdbool.h>
#include "firmware/engine_owner.h"

static int g_tristate_calls = 0;
static int g_pio_drain_calls = 0;
static bool g_swio_pu_enabled = false;

static void fake_pio_drain(void) { g_pio_drain_calls++; }
static void fake_tristate_all(void) { g_tristate_calls++; g_swio_pu_enabled = false; }
static void fake_set_swio_pu(bool en) { g_swio_pu_enabled = en; }

static const pu_pin_hal_t kFakeHal = {
    .drain_pio = fake_pio_drain,
    .tristate_debug_pins = fake_tristate_all,
    .set_swio_pullup = fake_set_swio_pu,
};

static void test_engine_owner_rejects_cross_interface_contention(void) {
    pu_engine_init(&kFakeHal);
    assert(pu_engine_acquire(PU_ENGINE_SWIO, 1000, 500000) == PU_OK);
    assert(g_swio_pu_enabled == true);
    /* CMSIS-DAP SWD must be rejected immediately while SWIO holds the bus */
    assert(pu_engine_acquire(PU_ENGINE_SWD, 1200, 500000) == PU_ERR_ENGINE_BUSY);
    assert(pu_engine_current() == PU_ENGINE_SWIO);
    assert(pu_engine_release(PU_ENGINE_SWIO) == PU_OK);
    assert(g_swio_pu_enabled == false);
    assert(pu_engine_acquire(PU_ENGINE_SWD, 1500, 500000) == PU_OK);
}

static void test_engine_owner_teardown_on_usb_reset_and_deadline(void) {
    pu_engine_init(&kFakeHal);
    g_tristate_calls = 0;
    g_pio_drain_calls = 0;
    assert(pu_engine_acquire(PU_ENGINE_JTAG, 1000, 50000) == PU_OK);
    pu_engine_tick(51001); /* Past deadline */
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(g_pio_drain_calls == 1 && g_tristate_calls == 1);

    assert(pu_engine_acquire(PU_ENGINE_SWIO, 60000, 500000) == PU_OK);
    pu_engine_on_usb_reset();
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(g_swio_pu_enabled == false);
    assert(g_tristate_calls == 2);
}

int main(void) {
    test_engine_owner_rejects_cross_interface_contention();
    test_engine_owner_teardown_on_usb_reset_and_deadline();
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -fsanitize=address,undefined -I. tests/host_c/test_engine_owner.c -o /tmp/test_engine_owner`
Expected: FAIL (`firmware/engine_owner.h: No such file or directory`)

- [ ] **Step 3: Implement `firmware/engine_owner.h` and `firmware/engine_owner.c`**

Implement the exact FSM with spinlock/critical-section protection on RP2040 (no-op on host tests), atomic teardown (`drain_pio()` → `tristate_debug_pins()` → `set_swio_pullup(false)`), strict `FAULT` latching that blocks all acquisitions until `pu_engine_clear_fault()`, and monotonic `deadline_us` tracking.

- [ ] **Step 4: Run test under ASan/UBSan to verify it passes**

Run: `cc -Wall -Wextra -Werror -fsanitize=address,undefined -I. firmware/engine_owner.c tests/host_c/test_engine_owner.c -o /tmp/test_engine_owner && /tmp/test_engine_owner`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add firmware/engine_owner.h firmware/engine_owner.c tests/host_c/test_engine_owner.c
git commit -m "feat(firmware): add deterministic PIO/pin engine_owner mutual-exclusion FSM"
```

---

### Task 4: Hardened CMSIS-DAP v2 Engine (PIO0 SWD/JTAG) & USB Composite Descriptors (`0x2E8A:<PID>`)

**Files:**
- Create: `firmware/usb_descriptors.h`, `firmware/usb_descriptors.c`, `firmware/tusb_config.h`
- Create: `firmware/dap_engine.h`, `firmware/dap_engine.c`
- Create: `firmware/pio/swd.pio`, `firmware/pio/jtag.pio`
- Test: `tests/host_c/test_dap_guard_fuzz.c`
- Test: `tests/host_c/test_usb_descriptors.c`

**Interfaces:**
- Consumes: `firmware/engine_owner.h` (`pu_engine_acquire`, `pu_engine_release`), `PU_USB_PID` compile definition.
- Produces:
  - `size_t pu_dap_process_packet(const uint8_t *req, size_t req_len, uint8_t *resp, size_t resp_cap, uint64_t now_us);`
  - TinyUSB descriptor callbacks (`tud_descriptor_device_cb`, `tud_descriptor_configuration_cb`, `tud_descriptor_bos_cb`, `tud_vendor_control_xfer_cb` serving MS OS 2.0 WCID descriptors binding IF 0 & IF 5 to `WINUSB`).

- [ ] **Step 1: Write failing C tests for 50,000-packet CMSIS-DAP fuzzing, packet snapshotting (Review Focus #3), and USB WCID descriptor validation**

```c
/* tests/host_c/test_dap_guard_fuzz.c */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "firmware/dap_engine.h"

static uint32_t xorshift32(uint32_t *s) {
    uint32_t x = *s; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return *s = x;
}

static void test_dap_guard_snapshot_survives_buffer_reuse(void) {
    uint8_t shared_buf[64] = { 0x00 /* ID_DAP_Info */, 0x01 /* Vendor */ };
    uint8_t resp[64] = {0};
    size_t n = pu_dap_process_packet(shared_buf, 2, resp, sizeof(resp), 1000);
    assert(n >= 2 && n <= sizeof(resp));
    assert(resp[0] == 0x00);
}

int main(void) {
    pu_dap_init_for_test();
    test_dap_guard_snapshot_survives_buffer_reuse();

    uint32_t seed = 0xC0FFEE42u;
    uint8_t req[64], resp[64];
    for (int i = 0; i < 50000; ++i) {
        size_t len = xorshift32(&seed) % 65;
        for (size_t j = 0; j < len; ++j) req[j] = (uint8_t)xorshift32(&seed);
        size_t out = pu_dap_process_packet(req, len, resp, sizeof(resp), (uint64_t)i * 100u);
        assert(out <= sizeof(resp));
    }
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -fsanitize=address,undefined -I. tests/host_c/test_dap_guard_fuzz.c -o /tmp/test_dap`
Expected: FAIL (`firmware/dap_engine.h: No such file or directory`)

- [ ] **Step 3: Implement `usb_descriptors.{h,c}`, `tusb_config.h`, `dap_engine.{h,c}`, and PIO0 `swd.pio` / `jtag.pio`**

Incorporate all hardened fixes from `origin/linux-focused` (`dap_guard.c`, `swj_sequence.c`, `pin_control.c`, 64-bit clock math with 1 kHz floor, PIO-to-SIO pin override handoff, response-ring backpressure) and bind `DAP_Connect(SWD/JTAG)` and `DAP_Disconnect` directly to `pu_engine_acquire` / `pu_engine_release`. Configure `VID = 0x2E8A`, `PID = PU_USB_PID`, and MS OS 2.0 compatible ID `"WINUSB"` on IF 0 and IF 5.

- [ ] **Step 4: Run fuzz & descriptor tests under ASan/UBSan**

Run: `cc -Wall -Wextra -Werror -fsanitize=address,undefined -I. firmware/engine_owner.c firmware/dap_engine.c tests/host_c/test_dap_guard_fuzz.c -o /tmp/test_dap && /tmp/test_dap`
Expected: PASS (50,000 packets, zero leaks/UB)

- [ ] **Step 5: Commit**

```bash
git add firmware/usb_descriptors.* firmware/tusb_config.h firmware/dap_engine.* firmware/pio/swd.pio firmware/pio/jtag.pio tests/host_c/test_dap_guard_fuzz.c tests/host_c/test_usb_descriptors.c
git commit -m "feat(firmware): add composite USB descriptors with MS OS 2.0 WCID and hardened CMSIS-DAP v2 engine"
```

---

### Task 5: Hardened CH32V003 PIO1 SWIO Engine, `PUID1` Non-Destructive Identity & Cross-Page SoftBreak

**Files:**
- Create: `firmware/pio/singlewire.pio`
- Create: `firmware/swio_engine.h`, `firmware/swio_engine.cpp`
- Test: `tests/host_c/test_ch32_wire_model.cpp`

**Interfaces:**
- Consumes: `firmware/engine_owner.h` (`PU_ENGINE_SWIO`, `PU_ENGINE_RECOVERY`), `pu_swio_wire_hal_t` (mockable 1-wire bus supporting silent-high `0xFFFFFFFF`, silent-low `0x00000000`, and simulated QingKe V2 DM registers + 16 KiB flash).
- Produces:
  - `pu_ch32_check_result_t pu_ch32_non_destructive_check(uint64_t now_us);`
  - `size_t pu_ch32_gdb_feed(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap, uint64_t now_us);`
  - `pu_status_t pu_ch32_power_cycle_unbrick(uint64_t now_us);`

- [ ] **Step 1: Write failing C++ wire-model tests covering Review Focus #2 (silent-high `0xFFFFFFFF` & silent-low `0x00000000`), `PUID1` non-destructive check + `OUTEN` reset verification, and cross-page 4-byte SoftBreak**

```cpp
// tests/host_c/test_ch32_wire_model.cpp
#include <cassert>
#include <cstdint>
#include "firmware/swio_engine.h"

static void test_ch32_silent_bus_never_misclassified() {
    pu_swio_set_fake_wire_mode(PU_WIRE_SILENT_HIGH_FF);
    auto r_hi = pu_ch32_non_destructive_check(1000);
    assert(r_hi.status == PU_CH32_ERR_NO_REPLY);
    assert(r_hi.flash_writes == 0);
    assert(r_hi.diagnosis_mask == PU_CH32_DIAG_ALL_FIVE_CAUSES);

    pu_swio_set_fake_wire_mode(PU_WIRE_SILENT_LOW_00);
    auto r_lo = pu_ch32_non_destructive_check(2000);
    assert(r_lo.status == PU_CH32_ERR_NO_REPLY);
    assert(r_lo.flash_writes == 0);
}

static void test_ch32_check_preserves_running_sketch_and_resets_outen() {
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    auto r = pu_ch32_non_destructive_check(3000);
    assert(r.status == PU_CH32_OK);
    assert(r.flash_writes == 0);
    assert(r.resumed_running == true);
    assert(r.outen_disabled_verified == true);
}

static void test_ch32_softbreak_cross_page_and_disconnect_restore() {
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    // Insert 4-byte software breakpoint straddling 64-byte flash page boundary 0x0800003E
    uint8_t out[128];
    const char *ins = "$Z0,0800003e,4#";
    assert(pu_ch32_gdb_send_checked(ins, out, sizeof(out), 4000) == PU_OK);
    pu_ch32_on_disconnect();
    assert(pu_swio_fake_flash_matches_original() == true);
}

int main() {
    test_ch32_silent_bus_never_misclassified();
    test_ch32_check_preserves_running_sketch_and_resets_outen();
    test_ch32_softbreak_cross_page_and_disconnect_restore();
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `c++ -std=c++17 -fsanitize=address,undefined -I. tests/host_c/test_ch32_wire_model.cpp -o /tmp/test_ch32`
Expected: FAIL (`firmware/swio_engine.h: No such file or directory`)

- [ ] **Step 3: Implement `firmware/pio/singlewire.pio`, `firmware/swio_engine.h`, and `firmware/swio_engine.cpp`**

Enforce strict QingKe V2 Table 3-5 `DMSTATUS` bitfield checks (including reserved-zero bits), verify `WCH_DM_SHDWCFGR.OUTEN == 0` readback before latching `CFGR`, return `PUID1:ERR_DETACH_FAILED` on failed power-on interface reset, reject `Z1`/`z1` hardware breakpoints cleanly (empty reply per GDB RSP), track dual-page state for cross-page `Z0`/`z0`, and implement deterministic `VTARG` power-cycle + immediate SWIO hold unbrick when `PU_HAS_VTARG_SWITCH == 1`.

- [ ] **Step 4: Run wire-model test under ASan/UBSan**

Run: `c++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -I. firmware/engine_owner.c firmware/swio_engine.cpp tests/host_c/test_ch32_wire_model.cpp -o /tmp/test_ch32 && /tmp/test_ch32`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add firmware/pio/singlewire.pio firmware/swio_engine.* tests/host_c/test_ch32_wire_model.cpp
git commit -m "feat(firmware): add hardened CH32V003 SWIO engine, PUID1 check, and cross-page SoftBreak"
```

---

### Task 6: Concurrent DMA UART Bridge, Control/Telemetry (`IF 5`) & Dual-Core RP2040 Integration

**Files:**
- Create: `firmware/uart_bridge.h`, `firmware/uart_bridge.c`
- Create: `firmware/control_telemetry.h`, `firmware/control_telemetry.c`
- Create: `firmware/main.cpp`
- Modify: `CMakeLists.txt`
- Test: `tests/host_c/test_control_telemetry.c`

**Interfaces:**
- Consumes: `firmware/engine_owner.h`, `firmware/dap_engine.h`, `firmware/swio_engine.h`, `firmware/usb_descriptors.h`, `config/board_pinout.h`.
- Produces:
  - `size_t pu_ctrl_handle_request(const uint8_t *req, size_t len, uint8_t *resp, size_t cap);` (commands: `GET_INFO`, `GET_TELEMETRY`, `SET_VTARG`, `CLEAR_FAULT`, `ENTER_BOOTSEL`, `SELF_TEST`)
  - `void pu_telemetry_on_overcurrent_irq(void);`
  - Complete RP2040 ARM builds for both `PU_BOARD_PICO` and `PU_BOARD_PRO_PCB` packaged into unified `.uf2` images via `tools/package_uf2.py`.

- [ ] **Step 1: Write failing C test covering Review Focus #5 (VTARG overcurrent fault latches `engine_owner` into `FAULT` and requires explicit host `CLEAR_FAULT`)**

```c
/* tests/host_c/test_control_telemetry.c */
#include <assert.h>
#include <stdint.h>
#include "firmware/control_telemetry.h"
#include "firmware/engine_owner.h"

static void test_telemetry_overcurrent_forces_engine_fault(void) {
    pu_ctrl_init_for_test();
    assert(pu_engine_acquire(PU_ENGINE_SWD, 1000, 500000) == PU_OK);
    pu_telemetry_on_overcurrent_irq();
    assert(pu_engine_current() == PU_ENGINE_FAULT);
    assert(pu_engine_acquire(PU_ENGINE_SWD, 2000, 500000) == PU_ERR_FAULT_LATCHED);

    uint8_t clear_req[2] = { PU_CTRL_CMD_CLEAR_FAULT, 0 };
    uint8_t resp[16] = {0};
    size_t n = pu_ctrl_handle_request(clear_req, 2, resp, sizeof(resp));
    assert(n >= 2 && resp[1] == PU_OK);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
}

int main(void) {
    test_telemetry_overcurrent_forces_engine_fault();
    return 0;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cc -fsanitize=address,undefined -I. tests/host_c/test_control_telemetry.c -o /tmp/test_ctrl`
Expected: FAIL (`firmware/control_telemetry.h: No such file or directory`)

- [ ] **Step 3: Implement `uart_bridge.{h,c}`, `control_telemetry.{h,c}`, dual-core `firmware/main.cpp` (Core 0 USB/telemetry/supervisor, Core 1 SPSC PIO worker), and wire `CMakeLists.txt` with `-DPU_BOARD=PICO|PRO_PCB` and `-DPU_USB_PID=0xXXXX`**

- [ ] **Step 4: Run host ASan/UBSan test AND cross-compile both RP2040 board profiles + package verified unified UF2**

Run:
```bash
cc -Wall -Wextra -Werror -fsanitize=address,undefined -I. firmware/engine_owner.c firmware/control_telemetry.c tests/host_c/test_control_telemetry.c -o /tmp/test_ctrl && /tmp/test_ctrl
cmake -B build-pico -DPU_BOARD=PICO -DPU_USB_PID=${PU_USB_PID:-0x0000} && cmake --build build-pico -j
cmake -B build-pro -DPU_BOARD=PRO_PCB -DPU_USB_PID=${PU_USB_PID:-0x0000} && cmake --build build-pro -j
python3 tools/package_uf2.py --bootloader build-pico/bootloader.bin --slot-a build-pico/firmware.bin --output build-pico/pico_universal_pico.uf2 --verify-rom-model
```
Expected: PASS (all host tests green, both ARM builds fit within `SLOT_APP_A` 960 KiB, UF2 ROM simulation passes).

- [ ] **Step 5: Commit**

```bash
git add firmware/uart_bridge.* firmware/control_telemetry.* firmware/main.cpp CMakeLists.txt tests/host_c/test_control_telemetry.c
git commit -m "feat(firmware): integrate dual-core composite USB firmware, DMA UART, and control/telemetry"
```

---

## Phase 2: Cross-Platform Go Host CLI (`pu`) & Turnkey Arduino IDE 2 / VS Code Integration

### Task 7: Go `pu` Core, USB Device-Tree Discovery (`linux_tree` / `windows_tree`) & `pu setup`

**Files:**
- Create: `host/go.mod`, `host/cmd/pu/main.go`, `host/cmd/pu/setup.go`
- Create: `host/internal/catalog/catalog.go`
- Create: `host/internal/usbprobe/discover.go`, `host/internal/usbprobe/linux_tree.go`, `host/internal/usbprobe/windows_tree.go`, `host/internal/usbprobe/lock.go`
- Test: `host/internal/usbprobe/discover_test.go`, `host/cmd/pu/setup_test.go`

**Interfaces:**
- Consumes: `catalog/targets.json`, `linux/60-pico-universal.rules`, approved `VID 0x2E8A` + `PU_USB_PID`.
- Produces:
  - `type ProbeDevice struct { Serial string; VID, PID uint16; DAPPath, GDBPort, UARTPort string }`
  - `func DiscoverProbes(filterSerial string) ([]ProbeDevice, error)`
  - `func AcquireSessionLock(serial string) (io.Closer, error)`
  - `pu setup [--offline] [--verify] [--uninstall] [--arduino-dir PATH]`

- [ ] **Step 1: Write failing Go unit tests for Linux sysfs & Windows SetupAPI multi-probe serial correlation and idempotent `pu setup` install/uninstall**

```go
// host/internal/usbprobe/discover_test.go
package usbprobe

import "testing"

func TestCorrelateCompositeInterfacesBySerial(t *testing.T) {
    nodes := []RawInterfaceNode{
        {VID: 0x2E8A, PID: 0x1234, Serial: "E660583883278628", InterfaceNum: 1, DevPath: "/dev/ttyACM0"},
        {VID: 0x2E8A, PID: 0x1234, Serial: "E660583883278628", InterfaceNum: 3, DevPath: "/dev/ttyACM1"},
        {VID: 0x2E8A, PID: 0x1234, Serial: "E660583883999999", InterfaceNum: 1, DevPath: "/dev/ttyACM2"},
    }
    probes := CorrelateNodes(nodes, 0x2E8A, 0x1234)
    if len(probes) != 2 {
        t.Fatalf("expected 2 probes, got %d", len(probes))
    }
    if probes[0].GDBPort != "/dev/ttyACM0" || probes[0].UARTPort != "/dev/ttyACM1" {
        t.Fatalf("misassigned composite ports: %+v", probes[0])
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd host && go test -race ./...`
Expected: FAIL (missing packages)

- [ ] **Step 3: Implement `host/internal/catalog`, `host/internal/usbprobe` (Linux sysfs + Windows SetupAPI + flock/Mutex session locks), and `pu setup` (SHA-256 verified Arduino IDE 2 core/recipe/debug_custom.json installer & clean marker-safe uninstaller)**

- [ ] **Step 4: Run Go test suite + cross-compile Linux and Windows binaries**

Run:
```bash
cd host && go test -race ./...
GOOS=linux GOARCH=amd64 go build -o ../build/pu-linux-amd64 ./cmd/pu
GOOS=windows GOARCH=amd64 go build -o ../build/pu-windows-amd64.exe ./cmd/pu
```
Expected: PASS (all tests green, both static binaries built cleanly).

- [ ] **Step 5: Commit**

```bash
git add host/
git commit -m "feat(host): add Go pu CLI core, cross-platform USB composite discovery, and pu setup"
```

---

### Task 8: `pu check` (Multi-ISA & Clone Fingerprinting), `pu upload` (Atomic Snapshot & ESP32 Safety), `pu gdb-pipe` & `pu recover`

**Files:**
- Create: `host/internal/puid/fingerprint.go`, `host/internal/puid/fingerprint_test.go`
- Create: `host/internal/uploader/uploader.go`, `host/internal/uploader/uploader_test.go`
- Create: `host/internal/gdbpipe/relay.go`, `host/internal/gdbpipe/relay_test.go`
- Create: `host/cmd/pu/check.go`, `host/cmd/pu/upload.go`, `host/cmd/pu/gdbpipe.go`, `host/cmd/pu/recover.go`

**Interfaces:**
- Consumes: `host/internal/usbprobe`, `catalog/openocd/pu-esp32.cfg`.
- Produces:
  - `func CheckTarget(probe ProbeDevice, family string) (CheckReport, error)` (detects wiring faults, VTREF out-of-range, STM32 genuine vs CS32/CKS32/GD32 clone IDCODEs, CH32V003 `PUID1` status)
  - `func UploadAtomic(probe ProbeDevice, req UploadRequest) error`
  - `func RelayGDB(ctx context.Context, probe ProbeDevice, in io.Reader, out io.Writer) error`

- [ ] **Step 1: Write failing Go tests covering Review Focus #4 (atomic snapshot + ESP32 4 KiB sector overlap rejection) and STM32 clone fingerprinting**

```go
// host/internal/uploader/uploader_test.go
package uploader

import (
    "os"
    "path/filepath"
    "testing"
)

func TestUploadSnapshotAndSectorOverlapRejection(t *testing.T) {
    dir := t.TempDir()
    f1 := filepath.Join(dir, "boot.bin")
    f2 := filepath.Join(dir, "part.bin")
    _ = os.WriteFile(f1, make([]byte, 4096), 0o600)
    _ = os.WriteFile(f2, make([]byte, 2048), 0o600)

    // Overlapping within same 4 KiB erase sector (0x1000 + 0x1000 vs 0x1800) must be rejected
    err := ValidateEspressifSegments([]Segment{
        {Addr: 0x1000, Path: f1},
        {Addr: 0x1800, Path: f2},
    })
    if err == nil {
        t.Fatal("expected sector overlap error, got nil")
    }

    // Snapshot must isolate file contents from later mutation
    snap, cleanup, err := CreatePrivateSnapshot(f1)
    if err != nil {
        t.Fatal(err)
    }
    defer cleanup()
    _ = os.WriteFile(f1, []byte("mutated-mid-upload"), 0o600)
    got, _ := os.ReadFile(snap)
    if len(got) != 4096 {
        t.Fatalf("snapshot mutated! expected 4096 bytes, got %d", len(got))
    }
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd host && go test -race ./internal/uploader/... ./internal/puid/... ./internal/gdbpipe/...`
Expected: FAIL (missing packages)

- [ ] **Step 3: Implement `puid`, `uploader` (embedding `pu-esp32.cfg` Zbit QIO / halt / detach fixes), `gdbpipe` (with disconnect cleanup & `set breakpoint auto-hw off` profile), `recover` (CH32V003 power-cycle unbrick + STM32 connect-under-reset), and Arduino IDE 2 / VS Code config generators (`pu export-vscode`)**

- [ ] **Step 4: Run full Go test suite with `-race` and verify Arduino CLI recipe expansion**

Run: `cd host && go test -race -v ./...`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add host/
git commit -m "feat(host): add pu check, atomic upload, gdb-pipe, recover, and IDE integration"
```

---

### Task 9: Track A Hardware-in-the-Loop (HIL) Qualification on Pico Dev Board

**Files:**
- Create: `tests/hil/run_hil_matrix.py`
- Create: `docs/HARDWARE_QUALIFICATION_LOG.md`

**Interfaces:**
- Consumes: `build-pico/pico_universal_pico.uf2`, `build/pu-linux-amd64` / `pu-windows-amd64.exe`, physical target boards (STM32F103C8, STM32F407VE/F411CE, RP2040 Pico target, classic ESP32-WROOM-32, CH32V003F4P6).
- Produces: Timestamped JSON qualification report recording exact board revisions, SHA-256 firmware hashes, 100× flash+verify pass rate, GDB breakpoint/step/detach pass rate, zero-reboot cross-family switch pass rate, and Windows/Linux zero-driver enumeration proof.

- [ ] **Step 1: Implement `tests/hil/run_hil_matrix.py` with `--dry-run` self-test mode (runs offline in CI) and `--execute-hil` physical mode**
- [ ] **Step 2: Verify `--dry-run` passes in CI**

Run: `python3 tests/hil/run_hil_matrix.py --dry-run`
Expected: PASS

- [ ] **Step 3: Execute physical HIL qualification across all 4 v1 target families on Linux and Windows 10/11; record evidence in `docs/HARDWARE_QUALIFICATION_LOG.md`**
- [ ] **Step 4: Commit**

```bash
git add tests/hil/run_hil_matrix.py docs/HARDWARE_QUALIFICATION_LOG.md
git commit -m "test(hil): add automated target-matrix HIL runner and Track A qualification log"
```

---

## Phase 3: Track B — Custom 4-Layer Consumer Production PCB, Enclosure & Factory QC Jig

### Task 10: KiCad 8 Schematic, 4-Layer Production PCB (`PU_BOARD_PRO_PCB` Rev A → Rev B) & Consumer Enclosure

**Files:**
- Create: `hardware/pcb/pico-universal-pro.kicad_pro`
- Create: `hardware/pcb/pico-universal-pro.kicad_sch`
- Create: `hardware/pcb/pico-universal-pro.kicad_pcb`
- Create: `hardware/pcb/bom_rohs.csv`
- Create: `hardware/pcb/design_rules_checklist.md`
- Create: `hardware/enclosure/pico-universal-case.scad`
- Create: `hardware/enclosure/pinout_legend.svg`

**Interfaces:**
- Consumes: `config/board_pinout.h` (`PU_BOARD_PRO_PCB` GPIO assignments) and Section 2 electrical specifications.
- Produces: ERC-clean schematic, DRC-clean 4-layer Gerbers/drill/CPL/BOM ready for JLCPCB/PCBWay PCBA (target BOM $8–$12 @ 100–500 qty), and snap-fit enclosure STL/STEP.

- [ ] **Step 1: Capture full schematic in KiCad 8 implementing every Section 2.3 subsystem**
  - RP2040 + 12 MHz ±20 ppm crystal + W25Q32JV (4 MiB QSPI)
  - USB-C 16-pin + dual 5.1 kΩ CC1/CC2 + `USBLC6-2SC6` + 500 mA PPTC polyfuse + `AP2112K-3.3`
  - Directional level shifters (`SN74AXC4T774` / `SN74LVC2T45`, 1.8 V–5.0 V with `I_off`) + 22–33 Ω series termination + low-cap TVS ESD (`TPD4E05U06`)
  - Open-drain CH32V003 SWIO stage + `GP9` switchable 1 kΩ pull-up + `AP22811`/`TPS2553` VTARG load switch (`GP10/11`)
  - `ADC0` 1% VTREF divider + `ADC1` `INA180A1` + 100 mΩ shunt
  - Keyed 10-pin 2.54 mm universal header + 10-pin 1.27 mm ARM Cortex header + RGB LED + recessed recovery switch + bottom pogo test pads for every signal/rail
- [ ] **Step 2: Run KiCad ERC and verify zero errors/unconnected pins**

Run: `kicad-cli sch erc --severity-error --exit-code-violations hardware/pcb/pico-universal-pro.kicad_sch`
Expected: Exit code 0 (0 ERC errors)

- [ ] **Step 3: Complete 4-layer PCB layout (`Signal / Solid GND / Power+GND / Signal`, 90 Ω USB DP/DM differential routing, length-matched SWD/JTAG, solid GND return under target connector) and run KiCad DRC**

Run: `kicad-cli pcb drc --severity-error --exit-code-violations hardware/pcb/pico-universal-pro.kicad_pcb`
Expected: Exit code 0 (0 DRC errors)

- [ ] **Step 4: Fabricate 5–10 Rev A PCBA units, run bring-up & oscilloscope signal-integrity checks (1.8 V / 3.3 V / 5.0 V + short-circuit trip), apply Rev B polish fixes, and finalize snap-fit enclosure CAD**
- [ ] **Step 5: Commit hardware files and release Gerbers/BOM**

```bash
git add hardware/pcb/ hardware/enclosure/
git commit -m "feat(hardware): add ERC/DRC-verified 4-layer PU_BOARD_PRO_PCB and snap-fit enclosure"
```

---

### Task 11: Bed-of-Nails Factory QC Test Jig & `pu factory-qc` Automation

**Files:**
- Create: `hardware/factory-jig/qc_jig.kicad_sch`
- Create: `host/cmd/pu/factory_qc.go`
- Test: `host/cmd/pu/factory_qc_test.go`

**Interfaces:**
- Consumes: `build-pro/pico_universal_pro.uf2`, `firmware/control_telemetry.h` (`SELF_TEST`, `GET_TELEMETRY`, `SET_VTARG`), `host/internal/usbprobe`.
- Produces: Automated 30-second pass/fail QC runner verifying rail quiescent current, BOOTSEL UF2 flash, `0x2E8A:<PID>` enumeration, loopback across 1.8 V/3.3 V/5.0 V, ADC VTREF/I_TARG calibration (±3%), and VTARG overcurrent trip, emitting signed JSON/CSV QC certificates per unit serial number.

- [ ] **Step 1: Write failing Go test verifying `pu factory-qc` fails any unit with >3% ADC error, failed loopback bit, or missing overcurrent trip**

```go
// host/cmd/pu/factory_qc_test.go
package main

import "testing"

func TestFactoryQCGatesRejectFaultyUnits(t *testing.T) {
    good := QCReading{VRefMeasuredMV: 3310, VRefExpectedMV: 3300, LoopbackMask: 0x3F, OvercurrentTripped: true}
    if err := EvaluateQCUnit(good); err != nil {
        t.Fatalf("expected good unit to pass, got %v", err)
    }
    badADC := good
    badADC.VRefMeasuredMV = 3500 // >3% error
    if err := EvaluateQCUnit(badADC); err == nil {
        t.Fatal("expected >3% ADC error to fail QC")
    }
    badOC := good
    badOC.OvercurrentTripped = false
    if err := EvaluateQCUnit(badOC); err == nil {
        t.Fatal("expected missing overcurrent trip to fail QC")
    }
}
```
- [ ] **Step 2: Run test to verify it fails**

Run: `cd host && go test -race ./cmd/pu -run TestFactoryQC`
Expected: FAIL

- [ ] **Step 3: Implement `host/cmd/pu/factory_qc.go` and document the pogo-pin fixture schematic in `hardware/factory-jig/qc_jig.kicad_sch`**
- [ ] **Step 4: Run test to verify it passes, then qualify against Rev B units**

Run: `cd host && go test -race ./cmd/pu -run TestFactoryQC`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add hardware/factory-jig/ host/cmd/pu/factory_qc.go host/cmd/pu/factory_qc_test.go
git commit -m "feat(manufacturing): add pogo-pin factory QC fixture and automated pu factory-qc runner"
```

---

## Phase 4: Track C — Q1 Research Paper Benchmarking (`pu bench`), Signal Integrity & IEEE TIM / IEEE Access Manuscript

### Task 12: Automated Comparative Benchmark Suite (`pu bench`, Experiments E1–E6) & Statistical Analysis Pipeline

**Files:**
- Create: `host/internal/bench/runner.go`, `host/internal/bench/runner_test.go`
- Create: `host/cmd/pu/bench.go`
- Create: `paper/scripts/analyze_benchmarks.py`
- Test: `tests/python/test_analyze_benchmarks.py`

**Interfaces:**
- Consumes: Rev B `PU_BOARD_PRO_PCB` + commercial reference probes (**SEGGER J-Link**, **ST-Link V2/V3**, **Raspberry Pi Debug Probe**, **WCH-LinkE**, **ESP-Prog**) + target boards (STM32F103/F407, RP2040, classic ESP32, CH32V003).
- Produces:
  - Raw CSV datasets in `paper/data/` (`e1_throughput.csv`, `e2_debug_latency.csv`, `e3_switching_uart.csv`, `e4_signal_integrity.csv`, `e5_fault_injection.csv`, `e6_cost_efficiency.csv`).
  - Publication-ready vector figures (PDF/EPS) + auto-generated LaTeX summary macros (`paper/generated/results_macros.tex`) computing mean, SD, 95% CI, median, p95, p99, and Wilcoxon/Mann-Whitney significance tests.

- [ ] **Step 1: Write failing tests verifying `pu bench` CSV schema and `analyze_benchmarks.py` statistical calculations (mean, 95% CI, p95/p99, LaTeX macro export)**

```python
# tests/python/test_analyze_benchmarks.py
import csv
from pathlib import Path
from paper.scripts.analyze_benchmarks import compute_summary_stats

def test_compute_summary_stats_produces_ci95_and_p99(tmp_path):
    p = tmp_path / "e1.csv"
    with p.open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["probe", "target", "payload_kib", "trial", "throughput_kibs"])
        for i in range(30):
            w.writerow(["pico_universal", "stm32f407", 256, i, 120.0 + (i % 5)])
    stats = compute_summary_stats(p, value_col="throughput_kibs", group_cols=["probe", "target", "payload_kib"])
    row = stats[("pico_universal", "stm32f407", "256")]
    assert row["n"] == 30
    assert 121.5 < row["mean"] < 122.5
    assert row["ci95"] > 0.0
    assert row["p99"] >= row["p95"]
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `python3 -m pytest tests/python/test_analyze_benchmarks.py -v && cd host && go test -race ./internal/bench/...`
Expected: FAIL

- [ ] **Step 3: Implement `host/internal/bench/runner.go`, `host/cmd/pu/bench.go`, and `paper/scripts/analyze_benchmarks.py`**
- [ ] **Step 4: Run unit tests to verify they pass, then execute physical Experiments E1–E6 on Rev B + commercial reference probes + oscilloscope/logic analyzer**

Run:
```bash
python3 -m pytest tests/python/test_analyze_benchmarks.py -v
cd host && go test -race ./internal/bench/...
# Physical benchmark collection (run on lab bench):
./build/pu-linux-amd64 bench --exp E1,E2,E3,E5 --trials-e1 30 --trials-e2 100 --hil-cycles 1000 --out paper/data/
python3 paper/scripts/analyze_benchmarks.py --data paper/data/ --out paper/generated/
```
Expected: All CSV datasets collected with zero missing trials; vector figures and `paper/generated/results_macros.tex` generated cleanly.

- [ ] **Step 5: Commit**

```bash
git add host/internal/bench/ host/cmd/pu/bench.go paper/scripts/ tests/python/test_analyze_benchmarks.py
git commit -m "feat(research): add pu bench E1-E6 runner and statistical analysis pipeline"
```

---

### Task 13: IEEE TIM / IEEE Access LaTeX Manuscript & Zenodo Open Replication Package

**Files:**
- Create: `paper/main.tex`
- Create: `paper/references.bib`
- Create: `paper/Makefile`
- Create: `paper/REPLICATION.md`

**Interfaces:**
- Consumes: `paper/generated/results_macros.tex`, `paper/generated/*.pdf` figures, oscilloscope captures (`E4`), formal `engine_owner` FSM & timing equations (Section IV), and commercial BOM/cost data (`E6`).
- Produces: Cleanly compiled 10–12 page IEEEtran two-column PDF (`paper/main.pdf`) with zero LaTeX/BibTeX warnings, zero hand-typed stale numbers (all metrics pulled via `\input{generated/results_macros.tex}`), and a checksummed Zenodo archive (`pico-universal-replication-v1.0.tar.gz`) containing firmware source, KiCad schematics/Gerbers, `pu bench`, raw CSVs, and plotting scripts.

- [ ] **Step 1: Author `paper/main.tex` (Sections I–VIII matching Spec Section 6.3) and `paper/references.bib` (30–45 peer-reviewed IEEE/ACM/vendor references covering ARM CoreSight SWD, IEEE 1149.1 JTAG, RP2040 PIO, RISC-V Debug Spec / WCH SWIO, USB CMSIS-DAP, embedded fault injection, and commercial instrumentation probes)**
- [ ] **Step 2: Compile manuscript and verify zero undefined references, zero overfull hbox > 5 pt, and exact IEEEtran formatting**

Run: `make -C paper pdf check`
Expected: Exit code 0, `paper/main.pdf` built (10–12 pages), 0 BibTeX errors, 0 unresolved `??` citations.

- [ ] **Step 3: Perform internal Q1 peer-review audit checklist**
  - Verify Threats to Validity & Limitations section transparently discusses USB Full-Speed 12 Mbps ceiling vs USB High-Speed probes, non-isolated base consumer SKU, and EMC pre-compliance scope.
  - Verify every claim in Abstract & Conclusion traces directly to a table/figure from `paper/generated/`.
  - Build and test the clean-room Zenodo replication tarball (`REPLICATION.md` reproduces all figures from raw CSVs in one command).
- [ ] **Step 4: Commit manuscript & replication tooling**

```bash
git add paper/
git commit -m "docs(paper): add IEEE TIM/Access LaTeX manuscript and Zenodo replication pipeline"
```

---

## Phase 5: Commercial Production Readiness, Licensing Audit & Pilot Release

### Task 14: SPDX License Compliance Audit, EMC/ESD Pre-Compliance & Pilot Release Packaging

**Files:**
- Create: `tools/license_audit.py`
- Create: `THIRD_PARTY_NOTICES.md`
- Create: `docs/EMC_ESD_PRECOMPLIANCE.md`
- Create: `docs/CONSUMER_QUICKSTART.md`
- Test: `tests/python/test_license_audit.py`

**Interfaces:**
- Consumes: All pinned firmware/host dependencies, Rev B hardware, Factory QC logs, and Track A/B HIL logs.
- Produces: Machine-generated SPDX SBOM (`release/sbom.spdx.json`), complete `THIRD_PARTY_NOTICES.md` (with source-offer bundle for any GPL host tools), FCC Part 15 Class B / CE EN 55032 / EN 55035 pre-compliance report, and signed `v1.0.0` release artifacts (`pico_universal_pro.uf2`, `pu-linux-amd64`, `pu-windows-amd64.exe`, `SHA256SUMS`).

- [ ] **Step 1: Write failing test ensuring `tools/license_audit.py` detects any unattributed third-party component and verifies SPDX compliance**

```python
# tests/python/test_license_audit.py
import json
from tools.license_audit import audit_components

def test_license_audit_rejects_unattributed_and_flags_gpl_for_source_offer():
    ok, issues = audit_components([
        {"name": "pico-sdk", "spdx": "BSD-3-Clause", "notice_included": True, "source_offer": False},
        {"name": "openocd", "spdx": "GPL-2.0-or-later", "notice_included": True, "source_offer": False},
    ])
    assert not ok
    assert any("source_offer" in msg and "openocd" in msg for msg in issues)
```
- [ ] **Step 2: Run test to verify it fails**

Run: `python3 -m pytest tests/python/test_license_audit.py -v`
Expected: FAIL

- [ ] **Step 3: Implement `tools/license_audit.py`, generate `THIRD_PARTY_NOTICES.md` & SPDX SBOM, complete EMC/ESD pre-compliance checklist (`docs/EMC_ESD_PRECOMPLIANCE.md`), and write consumer Quick-Start Guide (`docs/CONSUMER_QUICKSTART.md`)**
- [ ] **Step 4: Run full repo verification suite (Python + ASan/UBSan C/C++ + Go `-race` + double-build reproducibility SHA-256 check + license audit)**

Run:
```bash
python3 -m pytest tests/python/ -v
cd host && go test -race ./... && cd ..
python3 tools/license_audit.py --verify
```
Expected: PASS (100% green across all automated gates).

- [ ] **Step 5: Commit**

```bash
git add tools/license_audit.py THIRD_PARTY_NOTICES.md docs/EMC_ESD_PRECOMPLIANCE.md docs/CONSUMER_QUICKSTART.md tests/python/test_license_audit.py
git commit -m "chore(release): add SPDX license audit, EMC/ESD pre-compliance, and consumer quickstart"
```
