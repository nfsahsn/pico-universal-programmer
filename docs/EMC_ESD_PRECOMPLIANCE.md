# Pico Universal Pro — EMC & ESD Pre-Compliance Engineering Checklist

**Target Standards:**
- **United States:** FCC Title 47 CFR Part 15 Subpart B (Class B Digital Device — Residential / Consumer)
- **European Union / UK:** CE / UKCA EN 55032 (Emissions, Class B) & EN 55035 (Immunity, incl. IEC 61000-4-2 ESD)
- **Environmental:** EU RoHS 3 (2015/863/EU) & REACH (verified via `hardware/pcb/bom_rohs.csv` — 100% RoHS=Yes)

---

## 1. Design-Stage EMC/EMI Mitigations Implemented in `PU_BOARD_PRO_PCB`

| Mitigation Area | Hardware Implementation | Target EMC/ESD Risk Addressed |
| :--- | :--- | :--- |
| Continuous Ground Reference | 4-layer stackup (`JLC04161H-7628`) with unbroken `In1.Cu` solid `GND` plane + ≤3 mm perimeter stitching vias | Minimizes common-mode return loop area & radiated emissions (30–1000 MHz) |
| USB 2.0 Differential Integrity | `27 Ω` 1% series resistors (`R3/R4`) + `90 Ω ±10%` differential routing over `In1.Cu` | Suppresses USB Full-Speed harmonic ringing & impedance reflections |
| Fast Edge-Rate Damping | `27 Ω` 1% series damping resistors (`R5..R12`) on all PIO SWD/JTAG/SWIO/UART outputs | Limits $dV/dt$ harmonics radiating down 25–50 cm target ribbon cables |
| USB-C & Target Port ESD | `USBLC6-2SC6` (`D1`) on USB-C + dual `TPD4E05U06` (`D2/D3`, `<0.5 pF`) behind `J2/J3` | Clamps IEC 61000-4-2 ESD strikes (±4 kV contact / ±8 kV air) before reaching RP2040 |
| Overcurrent & Short-Circuit Safety | `500 mA` PPTC polyfuse (`F1`) on USB VBUS + `AP22811` (`U7`) 400 mA current-limited VTARG switch | Prevents USB port brownout/fire hazard on shorted target boards |

---

## 2. Pre-Compliance Lab Scan Procedure (Execute on Rev B Hardware)

- [ ] **Radiated Emissions (30 MHz – 1 GHz, Class B, 3 m/10 m semi-anechoic chamber):**
  - Operating mode: Worst-case simultaneous activity (active 4 MHz SWD flashing + 2 Mbps UART streaming + 25 cm ribbon cable attached to STM32F4 target).
  - Margin target: ≥6 dB below FCC Part 15 Class B / EN 55032 Class B quasi-peak limit line.
- [ ] **Conducted Emissions (150 kHz – 30 MHz on host USB power supply via LISN):**
  - Margin target: ≥6 dB below Class B limit.
- [ ] **Electrostatic Discharge (IEC 61000-4-2):**
  - Contact discharge: ±4 kV to USB-C shield and exposed ground pins (Criterion B: probe recovers cleanly without permanent lockup).
  - Air discharge: ±8 kV to ASA snap-fit enclosure seams, RGB light pipe, and recessed button aperture (Criterion A/B).
- [ ] **Accredited Lab Sign-Off:** Archive official SDoC (FCC) and Technical Construction File / Declaration of Conformity (CE) prior to retail shipment.
