# Pico Universal — Consumer Quick-Start Guide

**Pico Universal** programs and debugs **STM32 (F1/F4)**, **Raspberry Pi Pico (RP2040)**, **Espressif ESP32**, and **WCH CH32V003** from a single USB-C probe—with **zero manual mode-switching reboots**, **zero Windows driver installs**, and a concurrent **Target UART Serial Monitor**.

---

## 1. One-Command Setup (Linux & Windows 10/11)

1. Download `pu-linux-amd64` (Linux) or `pu-windows-amd64.exe` (Windows) from the signed release and verify `SHA256SUMS`.
2. Run one-command setup to install SHA-256-verified Arduino IDE 2 cores, OpenOCD/GDB profiles, and Linux `udev` rules:
   ```bash
   # Linux
   ./pu-linux-amd64 setup

   # Windows (PowerShell)
   .\pu-windows-amd64.exe setup
   ```
   *(On Windows 10/11, Microsoft OS 2.0 WCID descriptors bind WinUSB automatically—no Zadig or `.inf` drivers needed.)*
3. Plug in your **Pico Universal** probe via USB-C and verify detection:
   ```bash
   pu list
   ```

---

## 2. Connecting Your Target Board

| Target Family | Required Pins (from 10-Pin 2.54 mm Header) | Notes |
| :--- | :--- | :--- |
| **STM32 (F1/F4)** | `VTREF`, `GND`, `SWCLK`, `SWDIO`, `NRST` | Supports genuine & clone (CS32/CKS32/GD32) detection |
| **RP2040 Pico** | `VTREF`, `GND`, `SWCLK`, `SWDIO` | Connects directly to Pico 3-pin `DEBUG` header |
| **Classic ESP32** | `VTREF`, `GND`, `TCK (IO13)`, `TMS (IO14)`, `TDI (IO12)`, `TDO (IO15)` | Automatic 3.3 V MTDI strapping & Zbit QIO flash support |
| **WCH CH32V003** | `VTREF`, `VTARG_OUT` (opt.), `GND`, `SWIO (PD1)` | Pro PCB includes internal switchable 1 kΩ pull-up; Pico dev board requires an external 1 kΩ pull-up between `GP28` and `3.3 V` |
| **Target Serial (Optional)** | `UART_TX`, `UART_RX`, `GND` | Streams concurrently in Arduino Serial Monitor during debugging |

---

## 3. Pre-Flight Check, Upload & Debugging

### Arduino IDE 2 (Turnkey)
1. Select your target board in **Tools → Board**.
2. Select **Pico Universal** in **Tools → Programmer**.
3. Click **Upload Using Programmer** (`Ctrl+Shift+U`) or **Start Debugging** (hardware/software breakpoints, stepping, registers, live variables).

### Command Line (`pu`)
```bash
# Non-destructive health, voltage, and MCU/clone check
pu check --family stm32f1
pu check --family ch32v003

# Atomic verified upload
pu upload --family stm32f4 --file build/firmware.elf
pu upload --family esp32 \
  --segment 0x1000:bootloader.bin \
  --segment 0x8000:partitions.bin \
  --segment 0x10000:app.bin

# Generate VS Code cortex-debug launch.json & OpenOCD config
pu export-vscode --family stm32f4 --out-dir .
```

---

## 4. RGB LED Status Codes & Recovery

| LED Pattern | Meaning |
| :--- | :--- |
| **Solid Green** | Idle / Ready |
| **Blinking Blue** | SWD / JTAG session active |
| **Blinking Cyan** | CH32V003 1-wire SWIO session active |
| **Pulsing Amber** | Recovery / unbrick in progress |
| **Fast Red Blink** | **`VTARG` Overcurrent FAULT latched** — disconnect target, check for VDD/GND short |

### Recovering a Locked / Bricked MCU
- **CH32V003 (PD1 remapped to GPIO or read-protected):**
  ```bash
  pu recover --family ch32v003
  pu upload --family ch32v003 --file clean_blink.elf
  ```
- **STM32 (SWD disabled or RDP locked):**
  ```bash
  pu recover --family stm32f1
  ```
