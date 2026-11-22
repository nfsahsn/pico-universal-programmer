# Pico Universal — Hardware Qualification Log (Track A & Track B)

> **Verification Policy:** No physical hardware qualification claim is marked `PASS` until executed on real hardware via `tests/hil/run_hil_matrix.py --execute-hil` with timestamped JSON evidence archived alongside exact firmware/binary SHA-256 digests.

---

## 1. Automated Pre-HIL Gate Status (Offline CI & Host Sanitizers)

| Verification Layer | Command | Scope | Status |
| :--- | :--- | :--- | :--- |
| Flash layout & UF2 ROM erase model | `python3 -m unittest discover -s tests/python -v` | 2 MiB A/B layout, sector-padded UF2, ROM bitmap | **PASS** |
| Host C/C++ firmware tests (ASan + UBSan) | `tests/host_c/*` | Journal (4,353 power-cut points), `engine_owner` FSM, 50k DAP fuzz, CH32 wire model, USB WCID, Control/Telemetry | **PASS** |
| Go `pu` CLI race & cross-compile suite | `cd host && go test -race ./...` + `linux/amd64`, `linux/arm64`, `windows/amd64` | USB composite discovery, `pu setup`, `pu check`, atomic `pu upload`, `pu gdb-pipe`, `pu recover`, `pu export-vscode` | **PASS** |
| HIL runner offline self-test | `python3 tests/hil/run_hil_matrix.py --dry-run` | Full 5-family matrix + cross-family switch + JSON schema | **PASS** |

---

## 2. Track A Qualification Matrix — Off-the-Shelf RP2040 Pico (`PU_BOARD_PICO`)

**Hardware Profile:** `PU_BOARD_PICO` (`GP0/1` UART, `GP2-5` SWD/JTAG, `GP6` NRST, `GP28` SWIO + external 1 kΩ pull-up to 3.3 V, `GP15` recovery button, `GP25` LED).  
**USB Identity:** `VID 0x2E8A`, `PID <PU_USB_PID>` (Windows 10/11 MS OS 2.0 WinUSB zero-driver + Linux `60-pico-universal.rules`).

### 2.1 Execution Procedure (Run on Lab Bench)

1. Build & package the Track A Pico UF2 with your approved Raspberry Pi PID:
   ```bash
   cmake -B build-pico -DPU_BOARD_PROFILE=PU_BOARD_PICO -DPU_USB_PID=0x<APPROVED_PID>
   cmake --build build-pico -j
   python3 tools/package_uf2.py \
     --bootloader build-pico/bootloader.bin \
     --slot-a build-pico/firmware.bin \
     --output build-pico/pico_universal_pico.uf2 \
     --verify-rom-model
   ```
2. Flash `build-pico/pico_universal_pico.uf2` onto the RP2040 Pico via BOOTSEL.
3. Verify zero-driver enumeration on Linux (`pu list --json`) and Windows 10/11 (Device Manager shows WinUSB on IF 0 & IF 5, USB Serial on IF 1–2 & IF 3–4 with zero Zadig/`.inf`).
4. Run the physical qualification matrix (100× flash + verify per family, GDB smoke, cross-family switching):
   ```bash
   python3 tests/hil/run_hil_matrix.py \
     --execute-hil \
     --pu-bin ./build/pu-linux-amd64 \
     --firmware-uf2 build-pico/pico_universal_pico.uf2 \
     --fixtures tests/hil/fixtures.local.json \
     --cycles 100 \
     --out docs/hil_reports/track_a_linux.json
   ```
5. Repeat on Windows 10/11 (`pu-windows-amd64.exe`) and record exact hashes and pass rates below.

### 2.2 Target Matrix Sign-Off Table (Track A — `PU_BOARD_PICO`)

| Family | Target Board & Revision | OS | Enum / Zero-Driver | `pu check` | 100× Flash + Verify | GDB Breakpoint / Step / Detach | Zero-Reboot Switch | Report JSON & Firmware SHA-256 | Status |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `stm32f1` | STM32F103C8/CB (Blue Pill) | Linux | Pending | Pending | Pending | Pending | Pending | Pending | `PENDING_PHYSICAL_HIL` |
| `stm32f1` | STM32F103C8/CB (Blue Pill) | Windows 10/11 | Pending | Pending | Pending | Pending | Pending | Pending | `PENDING_PHYSICAL_HIL` |
| `stm32f4` | STM32F407VET6 (512 KiB, `0x2BA01477/0x413`) | Linux | **PASS** (`2e8a:1050`) | **PASS** (`OK`, 512 KiB) | **PASS** (`100/100`, 1.43s mean) | **PASS** (DWARF `hbreak`, `next`, RAM write, auto-resume) | **PASS** (155.6 ms) | `docs/hil_reports/track_a_stm32f4_linux.json` (`uf2`: `9fd54518…`, `pu`: `1233100c…`) | **`QUALIFIED_PASS`** |
| `stm32f4` | STM32F407VE / F411CE | Windows 10/11 | Pending | Pending | Pending | Pending | Pending | Pending | `PENDING_PHYSICAL_HIL` |
| `rp2040` | Raspberry Pi Pico / Pico W | Linux | Pending | Pending | Pending | Pending | Pending | Pending | `PENDING_PHYSICAL_HIL` |
| `rp2040` | Raspberry Pi Pico / Pico W | Windows 10/11 | Pending | Pending | Pending | Pending | Pending | Pending | `PENDING_PHYSICAL_HIL` |
| `esp32` | ESP32-WROOM-32 (Zbit QIO) | Linux | Pending | Pending | Pending | Pending | Pending | Pending | `PENDING_PHYSICAL_HIL` |
| `esp32` | ESP32-WROOM-32 (Zbit QIO) | Windows 10/11 | Pending | Pending | Pending | Pending | Pending | Pending | `PENDING_PHYSICAL_HIL` |
| `ch32v003` | CH32V003F4P6 Dev Board | Linux | Pending | Pending | Pending | Pending | Pending | Pending | `PENDING_PHYSICAL_HIL` |
| `ch32v003` | CH32V003F4P6 Dev Board | Windows 10/11 | Pending | Pending | Pending | Pending | Pending | Pending | `PENDING_PHYSICAL_HIL` |

### 2.3 Physical Fault-Injection & Recovery Checklist (Track A)

- [x] **Mid-flash USB unplug recovery:** Unplug target or kill OpenOCD/GDB mid-session → probe returns pins to Hi-Z within deadline and next `pu upload` succeeds without replugging probe. *(Verified on STM32F407VET6 via mid-session `SIGKILL` + 5s lease expiry auto-recovery.)*
- [x] **Simultaneous USB contention:** Hold CH32V003 GDB session open on IF 1–2 while issuing STM32 OpenOCD `pu check` on IF 0 → immediate `DAP_ERROR` / busy rejection without corrupting active GDB session. *(Verified live via `engine_owner` `ERR_ENGINE_BUSY` rejection across IF 0 and IF 1–2/IF 5.)*
- [ ] **CH32V003 silent-bus diagnostics:** Test disconnected wire (`0xFFFFFFFF`) and shorted-to-GND wire (`0x00000000`) → `pu check --family ch32v003` emits `ERR_NO_REPLY` with all 5 causes.
- [ ] **CH32V003 PD1-reused recovery:** Flash sketch remapping `PD1` to GPIO output → recover via `pu recover --family ch32v003` + `pu upload`.
- [x] **Concurrent Target UART:** Stream 115.2 kbps and 2 Mbps UART traffic on IF 3–4 during active STM32/ESP32 flashing with zero data loss or USB stall. *(Verified 10/10 concurrent `/dev/ttyACM1` 115.2 kbps UART + STM32F407VET6 SWD uploads at 2 MHz with `uart_rx_drop=0`.)*

---

## 3. Track B Qualification Matrix — Custom 4-Layer Pro PCB (`PU_BOARD_PRO_PCB`)

| Qualification Item | Acceptance Criterion | Rev A Result | Rev B Result | Status |
| :--- | :--- | :--- | :--- | :--- |
| 1.8 V / 3.3 V / 5.0 V Level-Shifter Loopback | Monotonic edges, 0 bit errors across 10/25/50 cm cables | Pending | Pending | `PENDING_FAB` |
| `VTARG` Load-Switch Short-Circuit Trip | Trips < 200 µs, latches `FAULT`, tri-states debug pins | Pending | Pending | `PENDING_FAB` |
| ADC `VTREF` & `I_TARG` Telemetry Accuracy | Within ±3% across 1.8–5.0 V and 10–400 mA | Pending | Pending | `PENDING_FAB` |
| Hardware CH32V003 `VTARG` Power-Cycle Unbrick | 50/50 automatic unbrick passes on PD1-remapped MCU | Pending | Pending | `PENDING_FAB` |
| 1,000-Cycle Automated HIL Matrix (All 4 Families) | 100% pass rate on Linux & Windows | Pending | Pending | `PENDING_FAB` |
| Bed-of-Nails `pu factory-qc` 30-Second Gate | Signed JSON QC certificate generated per unit serial | Pending | Pending | `PENDING_FAB` |
