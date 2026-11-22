# Pico Universal Pro (`PU_BOARD_PRO_PCB`) — 4-Layer PCB Design Rules & Bring-Up Checklist

## 1. Board Geometry & 4-Layer Stackup (`JLC04161H-7628`)

| Layer | Copper Weight | Role | Controlled Impedance / Routing Rule |
| :--- | :--- | :--- | :--- |
| **L1 (`F.Cu`)** | 1 oz (35 µm) | Primary SMT components, high-speed signals | USB DP/DM 90 Ω ±10% differential (`W=0.21 mm`, `S=0.15 mm` over `0.2104 mm` FR-4-7628 prepreg, $\varepsilon_r = 4.6$); SWD/JTAG/SWIO length-matched within ±1.5 mm. |
| **L2 (`In1.Cu`)** | 0.5 oz (15.2 µm) | **Solid unbroken `GND` plane** | Zero splits under USB DP/DM, RP2040 crystal, QSPI flash, or target header return paths. |
| **L3 (`In2.Cu`)** | 0.5 oz (15.2 µm) | Power distribution (`+3V3`, `+5V_USB`, `VTREF`, `VTARG_OUT`) + `GND` copper pour | Stitching vias placed every ≤3 mm along board perimeter and beside every level shifter. |
| **L4 (`B.Cu`)** | 1 oz (35 µm) | Low-speed return traces + **16 gold-plated ENIG pogo test pads (`TP1..TP16`)** | Single-sided SMT component placement on `F.Cu` reduces PCBA reflow cost and keeps `B.Cu` flat for bed-of-nails factory QC fixture. |

- **Dimensions:** `55.0 mm × 24.0 mm × 1.6 mm` (thumb-drive form factor).
- **Target BOM Cost @ 250 qty:** **$10.54 USD** (from `bom_rohs.csv`, including 4-layer ENIG PCBA share + snap-fit enclosure + ribbon cable — within the $8–$12 consumer BOM target).

---

## 2. Electrical & Signal-Integrity Design Verification Rules

1. **Zero Back-Powering (`I_off` Guarantee):** Every target-facing level shifter (`SN74AXC4T774` / `SN74LVC2T45`) supports `I_off` partial-power-down isolation so disconnecting target power (`VTREF = 0 V`) leaves all target pins high-impedance (<5 µA leakage).
2. **Directional Shifter Stability (1.8 V–5.0 V):** `GP6` (`SWDIO_DIR`) drives the `SWDIO` transceiver direction pin in lockstep with RP2040 PIO0 `pindirs`, eliminating auto-direction oscillation on 25–50 cm ribbon cables.
3. **Series Damping & Low-Cap ESD:** Every target line has a `27 Ω 1%` series damping resistor placed within 3 mm of the shifter output, followed by `<0.5 pF` TVS ESD diodes (`TPD4E05U06`) placed directly behind the target connectors (`J2`, `J3`).
4. **CH32V003 Switchable 1 kΩ Pull-Up & VTARG Unbrick:** `GP9` controls P-FET `Q3` connecting `R19` (1 kΩ) between `SWIO` and `VTREF` only when `engine_owner == SWIO` or `RECOVERY`. `GP10/GP11` control `AP22811` (`U7`) current-limited (400 mA) VTARG load switch.
5. **USB-C Compliance:** Dual independent `5.1 kΩ 1%` CC1/CC2 pull-downs (`R1/R2`), `USBLC6-2SC6` (`D1`) directly behind `J1`, `500 mA` PPTC polyfuse (`F1`), and `27 Ω` USB DP/DM series resistors (`R3/R4`).

---

## 3. KiCad 8 Automated Verification Gates (Run Before Generating Fab Gerbers)

```bash
kicad-cli sch erc --severity-error --exit-code-violations hardware/pcb/pico-universal-pro.kicad_sch
kicad-cli pcb drc --severity-error --exit-code-violations hardware/pcb/pico-universal-pro.kicad_pcb
```

- [ ] ERC: 0 errors, 0 unconnected power pins (`PENDING_KICAD8_CHECK` — KiCad 8 CLI not installed in current container).
- [ ] DRC: 0 errors, 0 clearance/silkscreen/courtyard violations (`PENDING_KICAD8_CHECK`).

---

## 4. Rev A Prototype Bring-Up → Rev B DFM Polish Checklist

- [ ] **Rail Bring-Up (No MCU Load):** Verify `+5V_USB` (5.0 V ±5%), `+3V3` (3.30 V ±2%), quiescent current <15 mA prior to flashing.
- [ ] **BOOTSEL & USB Enumeration:** Hold recessed `SW1` (`GP15`), plug USB-C, flash `pico_universal_pro.uf2`, confirm `0x2E8A:<PU_USB_PID>` zero-driver enumeration on Linux and Windows 10/11.
- [ ] **Oscilloscope Signal-Integrity Sweep (E4):** Capture `SWCLK`, `SWDIO`, and `SWIO` waveforms at `1.8 V`, `3.3 V`, and `5.0 V` across `10 cm`, `25 cm`, and `50 cm` cables; verify monotonic edges, overshoot <15% $V_{DD}$, and max error-free clock.
- [ ] **Overcurrent Short-Circuit Trip Test:** Short `VTARG_OUT` to `GND` during active SWD session → verify `U7` trips `<200 µs`, `GP11` (`VTARG_FAULT_N`) asserts, `engine_owner` latches `FAULT`, RGB LED blinks red fault pattern, and debug pins tri-state immediately.
- [ ] **ADC Calibration Check:** Verify `ADC0` (`VTREF`) and `ADC1` (`INA180A1` `I_TARG`) read within ±3% across 1.8–5.0 V and 10–400 mA.
- [ ] **Rev B DFM Freeze:** Incorporate any silkscreen, mechanical clearance, or damping resistor adjustments found in Rev A bring-up before ordering 50–100 pilot units.
