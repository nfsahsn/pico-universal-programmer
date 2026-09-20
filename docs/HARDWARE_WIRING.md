# Hardware Wiring & Schematic Guide

This document specifies the hardware wiring for the **Pico Universal Programmer**, including the mode selector button, status LEDs, and target connections.

---

## 1. Programmer Control Hardware

```
                         Raspberry Pi Pico (RP2040)
                       +----------------------------+
                       |                            |
       GND ------------| Pin 38 (GND)               |
                       |                            |
[ Pushbutton ] --------| Pin 20 (GP15) [Mode Button]| (Internal pull-up enabled)
                       |                            |
[ 330Ω ] + [LED Green]-| Pin 21 (GP16) [Mode 1: DAP]| (Anode to GP16, Cathode to GND)
                       |                            |
[ 330Ω ] + [LED Yellow]| Pin 22 (GP17) [Mode 2: BMP]| (Anode to GP17, Cathode to GND)
                       |                            |
[ 330Ω ] + [LED Blue] -| Pin 24 (GP18) [Mode 3: WCH]| (Anode to GP18, Cathode to GND)
                       +----------------------------+
```

### Controls:
* **Short Press GP15 (< 1s):** Advances to next mode (`Mode 1 -> Mode 2 -> Mode 3 -> Mode 1`).
* **Long Press GP15 (> 2s):** Writes current mode into Flash memory as the persistent default power-on mode.
* **Onboard LED (GP25):** 
  * 1 Blink = Mode 1 (CMSIS-DAP)
  * 2 Blinks = Mode 2 (Black Magic Probe)
  * 3 Blinks = Mode 3 (PicoRVD)

---

## 2. Target Wiring by Board Family

### A. WCH CH32V003 (1-Wire SWIO)

```
Pico Programmer                                 Target CH32V003
---------------                                 ---------------
Pin 34 (GP28)  -------------------------------- Pin 8 (PD1 / SWIO)
                      |
                   [ 1kΩ ] (Mandatory Pull-Up)
                      |
Pin 36 (3V3)   -------+------------------------ Pin 2 (VDD / 3.3V)
Pin 38 (GND)   -------------------------------- Pin 7 (GND)
```

> [!IMPORTANT]
> The **1kΩ pull-up resistor** between 3.3V and PD1 is mandatory. Without it, the 1-wire protocol cannot pull the signal high fast enough and communication will fail.

---

### B. ARM Cortex-M SWD (STM32, RP2040, SAMD, nRF52)

| Pico Pin | Pico GPIO | Target Function | Target Pin (e.g., STM32) |
| :--- | :--- | :--- | :--- |
| **Pin 4** | **GP2** | **SWCLK** | SWCLK |
| **Pin 5** | **GP3** | **SWDIO** | SWDIO |
| **Pin 8** | **GND** | **GND** | Common Ground |
| **Pin 36**| **3V3** | **VDD (Optional)** | 3.3V Power |
| **Pin 1** | **GP0** | **UART TX (Listen)** | Target TX (for Serial Monitor) |
| **Pin 2** | **GP1** | **UART RX (Send)**   | Target RX (for Serial Monitor) |

---

### C. Standard 4-Wire JTAG (ESP32, FPGAs, WCH CH32V307)

| Pico Pin | Pico GPIO | JTAG Signal | ESP32 Target Pin |
| :--- | :--- | :--- | :--- |
| **Pin 4** | **GP2** | **TCK (Clock)** | GPIO 13 |
| **Pin 5** | **GP3** | **TMS (Mode)**  | GPIO 14 |
| **Pin 6** | **GP4** | **TDI (Data In)** | GPIO 12 |
| **Pin 7** | **GP5** | **TDO (Data Out)**| GPIO 15 |
| **Pin 8** | **GND** | **GND** | GND |

---

## 3. Voltage Safety Precautions

> [!WARNING]
> **RP2040 GPIO pins are strictly 3.3V (NOT 5V tolerant).**
> - If your target board runs at **5V**, you **must use a bidirectional logic level shifter** on all signal lines (SWCLK, SWDIO, SWIO, TDI, TDO, UART).
> - Feeding 5V into the Pico will permanently damage the RP2040 microcontroller.
