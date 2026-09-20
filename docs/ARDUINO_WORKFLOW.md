# Arduino IDE Workflow Guide (Pico Universal Programmer)

This guide walks you through using the **Pico Universal Programmer** exclusively inside the **Arduino IDE** for all supported target boards.

---

## Step 1: Run the One-Click Integration Installer

1. Ensure your target board cores are installed in Arduino IDE:
   * **WCH CH32V:** Via Additional Boards Manager URL:  
     `https://github.com/openwch/board_manager_files/raw/main/package_ch32v_index.json`
   * **STM32:** Search for `STM32 MCU based boards` (STM32duino) in Boards Manager.
   * **RP2040:** Search for `Raspberry Pi Pico/RP2040` (by Earle F. Philhower, III).
2. Double-click the installer script located at:
   ```
   d:\Picoprojects\pico-universal-programmer\arduino\install_arduino_support.bat
   ```
3. The script will auto-detect your installed cores and install the custom programmer recipes.
4. **Restart Arduino IDE.**

---

## Step 2: Uploading to WCH CH32V003

1. **Set Mode on Pico:**
   * Tap the **Mode Button (GP15)** until the Pico is in **Mode 3** (**Blue LED / 3 Blinks**).
2. **Wire to CH32V003:**
   * Pico **GP28 (Pin 34)** $\rightarrow$ CH32V003 **PD1** (SWIO).
   * **1kΩ pull-up resistor** between 3.3V and PD1.
   * Pico **GND (Pin 38)** $\rightarrow$ CH32V003 **GND**.
   * Pico **3V3 (Pin 36)** $\rightarrow$ CH32V003 **VDD**.
3. **In Arduino IDE:**
   * Go to **Tools > Board > CH32V EVT Boards** $\rightarrow$ Select **`CH32V003 EVT`**.
   * Go to **Tools > Port** $\rightarrow$ Select your Pico's **COM port**.
   * Go to **Tools > Programmer** $\rightarrow$ Select **`Pico Universal: PicoRVD (CH32V003 1-Wire SWIO)`**.
4. **Upload:**
   * Press **`Ctrl + Shift + U`** (or go to **Sketch > Upload Using Programmer**).

---

## Step 3: Uploading to STM32 (Blue Pill / Black Pill)

1. **Set Mode on Pico:**
   * Tap the **Mode Button (GP15)** until the Pico is in **Mode 1** (**Green LED / 1 Blink**).
2. **Wire to STM32:**
   * Pico **GP2 (Pin 4)** $\rightarrow$ STM32 **SWCLK**.
   * Pico **GP3 (Pin 5)** $\rightarrow$ STM32 **SWDIO**.
   * Pico **GND (Pin 8)** $\rightarrow$ STM32 **GND**.
3. **In Arduino IDE:**
   * Select your STM32 board under **Tools > Board > STM32 boards groups**.
   * Under **Tools > Upload method**, select **`OpenOCD`**.
   * Under **Tools > Programmer**, select **`Pico Universal: CMSIS-DAP (SWD)`**.
4. **Upload:**
   * Press **`Ctrl + Shift + U`** (**Upload Using Programmer**).

---

## Step 4: Uploading to Another Raspberry Pi Pico (RP2040)

1. **Set Mode on Pico:**
   * Mode 1 (**Green LED / 1 Blink**).
2. **Wire to Target Pico:**
   * Pico **GP2 (Pin 4)** $\rightarrow$ Target Pico **SWCLK** (Debug Header).
   * Pico **GP3 (Pin 5)** $\rightarrow$ Target Pico **SWDIO** (Debug Header).
   * Pico **GND (Pin 8)** $\rightarrow$ Target Pico **GND** (Debug Header).
3. **In Arduino IDE:**
   * Select **Tools > Board > Raspberry Pi RP2040 Boards > Raspberry Pi Pico**.
   * Under **Tools > Programmer**, select **`Pico Universal: CMSIS-DAP (Picoprobe)`**.
4. **Upload:**
   * Press **`Ctrl + Shift + U`**.

---

## Summary of Shortcuts & Menus

| Target Board | Mode on Pico | Tools > Programmer | Upload Action |
| :--- | :--- | :--- | :--- |
| **CH32V003** | Mode 3 (Blue / 3 Blinks) | `Pico Universal: PicoRVD` | **`Ctrl + Shift + U`** |
| **CH32V203 / V307** | Mode 1 (Green / 1 Blink) | `Pico Universal: CMSIS-DAP` | **`Ctrl + Shift + U`** |
| **STM32** | Mode 1 (Green / 1 Blink) | `Pico Universal: CMSIS-DAP` | **`Ctrl + Shift + U`** |
| **RP2040** | Mode 1 (Green / 1 Blink) | `Pico Universal: CMSIS-DAP` | **`Ctrl + Shift + U`** |
| **ESP32 (JTAG)** | Mode 1 (Green / 1 Blink) | `CMSIS-DAP` | **`Ctrl + Shift + U`** |
