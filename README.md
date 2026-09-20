# Pico Universal Multi-Target Programmer & Debugger

An open-source, production-grade 3-in-1 hardware programmer and debugger for the **Raspberry Pi Pico (RP2040)** with physical button mode switching, multi-pattern status LED feedback, and **100% turnkey Arduino IDE integration**.

---

## 🌟 Key Features

* **3 Operational Modes in 1 Board:**
  * **Mode 1 (Green / 1 Blink):** Official **Raspberry Pi Debug Probe (CMSIS-DAP v2)** — High-speed SWD and 4-wire JTAG for ARM Cortex-M, ESP32, and standard RISC-V.
  * **Mode 2 (Yellow / 2 Blinks):** **Black Magic Probe (BMP)** — Self-contained GDB server inside the Pico. Connect GDB directly to a COM port with zero background tools.
  * **Mode 3 (Blue / 3 Blinks):** **PicoRVD (WCH 1-Wire SWIO)** — Dedicated programmer and GDB debugger for the ultra-low-cost **WCH CH32V003**.
* **One-Touch Hardware Mode Switching:**
  * **Short Press (GP15):** Instantly cycle between modes.
  * **Long Press (GP15 > 2s):** Permanently save the selected mode as the default power-on mode in Flash memory.
* **Turnkey Arduino IDE Workflow:**
  * Program any board (**CH32V003**, **STM32**, **RP2040**, **ESP32**) directly from Arduino IDE menus via **Sketch > Upload Using Programmer** (`Ctrl + Shift + U`). No external IDEs or command-line flashing tools needed.
* **Fault-Tolerant Multi-Boot Engine:**
  * Validates target image stack pointer and reset vector before jumping, preventing CPU lockups.

---

## 🔌 Hardware Wiring

### 1. Mode Controls & Indicators

```
                     Raspberry Pi Pico (RP2040)
                   +----------------------------+
                   |                            |
   GND ------------| Pin 38 (GND)               |
                   |                            |
[ Pushbutton ] ----| Pin 20 (GP15) [Mode Button]| (Active-LOW, internal pull-up)
                   |                            |
[ 330Ω ] + [Green] | Pin 21 (GP16) [Mode 1: DAP]| (Anode to GP16, Cathode to GND)
                   |                            |
[ 330Ω ] + [Yellow]| Pin 22 (GP17) [Mode 2: BMP]| (Anode to GP17, Cathode to GND)
                   |                            |
[ 330Ω ] + [Blue]  | Pin 24 (GP18) [Mode 3: WCH]| (Anode to GP18, Cathode to GND)
                   +----------------------------+
```

*(Note: The onboard green LED on **GP25** also blinks 1, 2, or 3 times to indicate the mode, so discrete LEDs are optional!)*

### 2. Target Pinout Connections

| Pico Pin | Pico GPIO | Target Function | Target Board / Pin |
| :--- | :--- | :--- | :--- |
| **Pin 34** | **GP28** | **SWIO** | **CH32V003 Pin 8 (PD1)** *(Requires 1kΩ pull-up to 3.3V)* |
| **Pin 4**  | **GP2**  | **SWCLK / TCK** | ARM SWCLK / JTAG TCK (STM32, RP2040, ESP32) |
| **Pin 5**  | **GP3**  | **SWDIO / TMS** | ARM SWDIO / JTAG TMS (STM32, RP2040, ESP32) |
| **Pin 6**  | **GP4**  | **TDI** | JTAG TDI (ESP32 GPIO 12) |
| **Pin 7**  | **GP5**  | **TDO** | JTAG TDO (ESP32 GPIO 15) |
| **Pin 1**  | **GP0**  | **UART TX (Listen)**| Target RX (for Serial Monitor) |
| **Pin 2**  | **GP1**  | **UART RX (Send)**  | Target TX (for Serial Monitor) |
| **Pin 8**  | **GND**  | **GND** | **Always connect common ground** |
| **Pin 36** | **3V3**  | **3.3V Out** | Target VDD (3.3V logic only!) |

---

## 🚀 Quick Start (Arduino IDE Integration)

1. Open `d:\Picoprojects\pico-universal-programmer\arduino\` and run:
   ```cmd
   install_arduino_support.bat
   ```
   *(This automatically detects your Arduino cores in `%LOCALAPPDATA%\Arduino15` and installs the upload recipes).*
2. **Restart Arduino IDE.**
3. Choose your board in **Tools > Board**.
4. Select the matching programmer in **Tools > Programmer**:
   * For **CH32V003**: `Pico Universal: PicoRVD (CH32V003 1-Wire SWIO)`
   * For **STM32 / RP2040 / WCH V203**: `Pico Universal: CMSIS-DAP`
5. Press **`Ctrl + Shift + U` (Upload Using Programmer)**!

For detailed walkthroughs, see [docs/ARDUINO_WORKFLOW.md](docs/ARDUINO_WORKFLOW.md).

---

## 🛠️ Codebase Structure

```
pico-universal-programmer/
├── CMakeLists.txt                  # Strict Pico SDK build configuration
├── pico_sdk_import.cmake           # Official SDK import script
├── .clang-format                   # Professional LLVM C/C++ style rules
├── .gitignore                      # Git ignore patterns
├── config/
│   ├── app_config.h                # Pin assignments, debounce and LED timings
│   └── memory_map.h                # Flash slot layout and mode magic tokens
├── include/
│   ├── bootloader.h                # CPU handover and VTOR relocation
│   ├── button.h                    # Non-blocking debouncer (short/long press)
│   ├── led_indicator.h             # Non-blocking LED sequencer
│   ├── storage.h                   # RAM scratch register & Flash storage
│   └── image_validator.h          # MSP and Reset Vector validation
├── src/
│   ├── main.c                      # Entry point, supervisor, and event loop
│   ├── bootloader.c                # CPU handover implementation
│   ├── button.c                    # State-machine button debouncer
│   ├── led_indicator.c             # Multi-pattern LED engine
│   ├── storage.c                   # RAM & Flash persistence driver
│   └── image_validator.c          # Image sanity checker
├── arduino/
│   ├── install_arduino_support.bat # One-click Arduino IDE recipe installer
│   ├── ch32v/                      # WCH CH32V recipes
│   ├── stm32/                      # STM32duino recipes
│   └── rp2040/                     # Earle Philhower RP2040 recipes
├── tools/
│   └── merge_firmware.py           # Python tool to package a single unified .uf2
└── docs/
    ├── ARDUINO_WORKFLOW.md         # Step-by-step Arduino guide
    ├── HARDWARE_WIRING.md          # Wiring schematics and level-shifter guide
    └── ARCHITECTURE.md             # Technical memory map and state diagrams
```

---

## 📦 Building the Unified Firmware

To build the multi-boot supervisor and generate the final all-in-one `.uf2`:

```bash
mkdir build && cd build
cmake ..
cmake --build .
```

To stitch the bootloader and the three mode firmwares into one drag-and-drop `.uf2`:

```bash
python tools/merge_firmware.py \
    --bootloader build/pico_universal_programmer.bin \
    --cmsis firmware/debugprobe_on_pico.bin \
    --bmp firmware/blackmagic.bin \
    --picorvd firmware/picorvd.bin \
    -o pico_universal_programmer_full.uf2
```
