# Development firmware wiring

This pin assignment describes the integrated source builds for the original
RP2040 Pico. It is not a hardware qualification report. Use GPIO names below;
confirm header positions and target signals against your exact board schematic.
Legacy UF2 files and stock Debug Probe firmware can use different assignments.

## Controls

Connect a normally open button between GP15 and GND. The firmware enables its
internal pull-up. During startup, each short press/release selects the next mode;
a hold of at least two seconds saves the selected valid mode as the default.
Five seconds after the button is fully released and debounced, the selected probe
starts. It ignores the mode button until the Pico is reset or power-cycled.
For a reset button, use a separate normally open switch between Pico RUN and GND;
BOOTSEL is for firmware loading, not mode selection. A reset disconnects any debug
session, so finish the upload before resetting to select another mode.

Optional mode LEDs use GP16 (CMSIS-DAP), GP17 (Black Magic), and GP18 (PicoRVD).
Connect each GPIO through a 330-ohm resistor to its LED anode, with the cathode
at GND. GP25 drives the original Pico onboard indicator. Pico W is not the
configured board for these builds.

The selected external LED stays on; the other two stay off. GP25 blinks the mode
number during selection and becomes steady when the probe starts. Saves flash
only GP25 five times; an error toggles only GP25 every 250 ms and keeps selection
available. Errors do not advance the mode. If GP25 continues the error pattern,
reflash the complete `build/pico_universal_development.uf2`, not the supervisor-only
UF2 or a legacy root-level artifact. If selection never reaches its timeout,
check that GP15 is high when the button is released and low when pressed.

## CMSIS-DAP and Black Magic

| Pico GPIO | SWD function | JTAG function |
|---|---|---|
| GP2 | SWCLK | TCK |
| GP3 | SWDIO | TMS |
| GP4 | Unused | TDI: probe output to target input |
| GP5 | Unused | TDO: target output to probe input |
| GP6 | Target nRESET | Target nRESET |
| GP7 | Unused | nTRST, CMSIS-DAP only |
| GP0 | UART TX to target RX | UART TX to target RX |
| GP1 | UART RX from target TX | UART RX from target TX |
| GND | Common target ground | Common target ground |

Reset outputs are released by changing the GPIO to input and asserted low.
Black Magic has no GP7 binding or target-voltage measurement in this board port.
JTAG GPIO support in the CMSIS-DAP build does not establish that a particular
ESP32, FPGA, or WCH device can be programmed: host configuration and target
support must also be qualified. No board-specific JTAG mapping is assumed here.

## PicoRVD / CH32V003

Connect GP28 to the target SWIO signal (PD1), connect grounds, and fit a 1-kilohm
pull-up from SWIO to the target's 3.3 V rail, as specified by the pinned PicoRVD
source README. Check the target package for the physical PD1, supply, and ground
pins; package pin numbers are not interchangeable.

GP0 provides PicoRVD diagnostic UART output at 1,000,000 baud. UART command input
on GP1 is disabled in this candidate; the upstream development console could
erase flash or run destructive tests. These pins are not a target UART bridge
in this mode.

## Electrical scope

The current design assumes 3.3 V target signalling and a common ground. It has
no integrated level conversion, target-voltage sensing, or target-power control.
Do not connect 5 V signals directly. Power the target from an appropriate supply;
connecting the Pico supply to an already powered target is not part of this
wiring specification. Other target voltages require a separately reviewed debug
interface circuit and qualification.
