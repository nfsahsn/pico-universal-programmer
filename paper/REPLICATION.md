# Pico Universal — Zenodo Open Replication Package Guide

This document provides clean-room instructions to reproduce all firmware tests, host CLI checks, statistical summaries, vector figures, and the IEEEtran manuscript (`paper/main.pdf`) from the archived source and raw CSV benchmark datasets.

## 1. Archive Contents

- `bootloader/`, `firmware/`, `config/`: RP2040 A/B CRC32 bootloader, `engine_owner` FSM, CMSIS-DAP v2, CH32V003 SWIO, DMA UART, and IF 5 Control/Telemetry C/C++/PIO sources.
- `host/`: Static zero-dependency Go `pu` CLI (`setup`, `check`, `upload`, `gdb-pipe`, `recover`, `export-vscode`, `factory-qc`, `bench`).
- `hardware/`: KiCad 8 4-layer `PU_BOARD_PRO_PCB` schematic/PCB, RoHS BOM (`bom_rohs.csv`), OpenSCAD snap-fit enclosure, pinout SVG, and bed-of-nails factory QC fixture.
- `tests/`: Host C/C++ ASan/UBSan test suites (4,353-point flash journal, 50,000-packet DAP fuzzer, CH32 silent-bus wire model, USB WCID), Python UF2 ROM-model tests, and HIL qualification runner.
- `paper/`: `main.tex`, `references.bib`, `Makefile`, `scripts/analyze_benchmarks.py`, raw CSV datasets (`data/`), and generated figures/macros (`generated/`).

## 2. One-Command Statistical & Manuscript Reproduction

### Step 1: Regenerate Statistical Tables, Macros, and Vector Figures from Raw CSVs
```bash
python3 paper/scripts/analyze_benchmarks.py --data paper/data --out paper/generated
```
This reads `paper/data/e1_throughput.csv`, `e2_debug_latency.csv`, `e3_switching_uart.csv`, and `e5_fault_injection.csv`, writing:
- `paper/generated/results_macros.tex` (auto-imported by `paper/main.tex`)
- `paper/generated/summary_stats.json` (mean, SD, 95% Student-$t$ CI, median, p95, p99)
- `paper/generated/fig_*.svg` (vector publication charts)

### Step 2: Build and Verify the IEEEtran PDF
```bash
make -C paper pdf check
```
Requires TeX Live (`pdflatex`, `bibtex`, `IEEEtran.cls`). `make check` verifies zero unresolved citations (`??`) and zero BibTeX errors.

### Step 3: Re-Run Automated Verification Gates (No Physical Bench Required)
```bash
python3 -m unittest discover -s tests/python -v
python3 tests/hil/run_hil_matrix.py --dry-run
cd host && go test -race ./... && cd ..
python3 tools/license_audit.py --verify
```

### Step 4: Collect Fresh Lab Datasets on Physical Hardware (Experiments E1–E6)
```bash
./build/pu-linux-amd64 bench --exp E1,E2,E3,E5 --trials-e1 30 --trials-e2 100 --hil-cycles 1000 --out paper/data
python3 paper/scripts/analyze_benchmarks.py --data paper/data --out paper/generated
make -C paper pdf check zenodo
```
