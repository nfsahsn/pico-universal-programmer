cmake_minimum_required(VERSION 3.13)

# Board profile, slot selection, & approved Raspberry Pi USB Product ID (VID 0x2E8A)
set(PU_BOARD "PICO" CACHE STRING "Target hardware profile: PICO or PRO_PCB")
set_property(CACHE PU_BOARD PROPERTY STRINGS "PICO" "PRO_PCB")
set(PU_SLOT "A" CACHE STRING "Target A/B firmware link slot: A or B")
set_property(CACHE PU_SLOT PROPERTY STRINGS "A" "B")
set(PU_USB_PID "0x0000" CACHE STRING "Approved 16-bit Raspberry Pi USB Product ID (e.g. 0x1234)")
set(PICO_NO_UF2 1)

if(PU_BOARD STREQUAL "PRO_PCB")
    set(PU_BOARD_DEFINE "PU_BOARD_PRO_PCB=1")
elseif(PU_BOARD STREQUAL "PICO")
    set(PU_BOARD_DEFINE "PU_BOARD_PICO=1")
else()
    message(FATAL_ERROR "Unsupported PU_BOARD='${PU_BOARD}'. Expected PICO or PRO_PCB.")
endif()

if(PU_SLOT STREQUAL "A")
    set(PU_SLOT_NAME "app_slot_a")
    set(PU_SLOT_UF2_FLAG "--slot-a")
elseif(PU_SLOT STREQUAL "B")
    set(PU_SLOT_NAME "app_slot_b")
    set(PU_SLOT_UF2_FLAG "--slot-b")
else()
    message(FATAL_ERROR "Unsupported PU_SLOT='${PU_SLOT}'. Expected A or B.")
endif()

# Include Pico SDK configuration & slot linker generator
include(pico_sdk_import.cmake)
include(cmake/SlotLinker.cmake)

project(pico_universal_programmer C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# ----------------------------------------------------------------------------
# Target 1: Immutable A/B CRC32 Chainloader (Slot 0: 0x10000000, 60 KiB)
# ----------------------------------------------------------------------------
set(PU_BOOTLOADER_SOURCES
    bootloader/main.c
    bootloader/handover.S
    bootloader/crc32.c
    bootloader/journal.c
    bootloader/image_validator.c
)

add_executable(pu_bootloader ${PU_BOOTLOADER_SOURCES})

set_source_files_properties(
    bootloader/main.c
    bootloader/crc32.c
    bootloader/journal.c
    bootloader/image_validator.c
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror"
)

target_include_directories(pu_bootloader PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/config
)

target_compile_definitions(pu_bootloader PRIVATE
    ${PU_BOARD_DEFINE}
    PU_USB_PID=${PU_USB_PID}
    PICO_FLASH_SIZE_BYTES=2097152
)

target_link_libraries(pu_bootloader PRIVATE
    pico_stdlib
    pico_bootrom
    pico_flash
    hardware_flash
    hardware_gpio
    hardware_resets
    hardware_sync
)

pico_enable_stdio_usb(pu_bootloader 0)
pico_enable_stdio_uart(pu_bootloader 0)
pu_configure_flash_slot(pu_bootloader bootloader TRUE)
pico_add_extra_outputs(pu_bootloader)

# ----------------------------------------------------------------------------
# Target 2: Unified Dual-Core Composite USB Firmware (Slot A / B: 960 KiB)
# ----------------------------------------------------------------------------
set(PU_FIRMWARE_SOURCES
    firmware/main.cpp
    firmware/engine_owner.c
    firmware/usb_descriptors.c
    firmware/dap_engine.c
    firmware/swio_engine.cpp
    firmware/uart_bridge.c
    firmware/control_telemetry.c
    bootloader/crc32.c
    bootloader/journal.c
    bootloader/image_validator.c
)

add_executable(pu_firmware ${PU_FIRMWARE_SOURCES})

set_source_files_properties(
    ${PU_FIRMWARE_SOURCES}
    PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror"
)

pico_generate_pio_header(pu_firmware ${CMAKE_CURRENT_LIST_DIR}/firmware/pio/swd.pio)
pico_generate_pio_header(pu_firmware ${CMAKE_CURRENT_LIST_DIR}/firmware/pio/jtag.pio)
pico_generate_pio_header(pu_firmware ${CMAKE_CURRENT_LIST_DIR}/firmware/pio/singlewire.pio)

target_include_directories(pu_firmware PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/config
    ${CMAKE_CURRENT_LIST_DIR}/firmware
)

target_compile_definitions(pu_firmware PRIVATE
    ${PU_BOARD_DEFINE}
    PU_USB_PID=${PU_USB_PID}
    PICO_FLASH_SIZE_BYTES=2097152
    CFG_TUSB_MCU=OPT_MCU_RP2040
)

target_link_libraries(pu_firmware PRIVATE
    pico_stdlib
    pico_bootrom
    pico_flash
    pico_multicore
    tinyusb_device
    tinyusb_board
    hardware_adc
    hardware_clocks
    hardware_dma
    hardware_flash
    hardware_gpio
    hardware_irq
    hardware_pio
    hardware_sync
    hardware_uart
    hardware_watchdog
)

pico_enable_stdio_usb(pu_firmware 0)
pico_enable_stdio_uart(pu_firmware 0)
pu_configure_flash_slot(pu_firmware ${PU_SLOT_NAME} FALSE)
pico_add_extra_outputs(pu_firmware)

# Unified 4 KiB sector-padded UF2 packaging target
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_custom_target(pu_unified_uf2
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/package_uf2.py
            --bootloader $<TARGET_FILE_DIR:pu_bootloader>/pu_bootloader.bin
            ${PU_SLOT_UF2_FLAG} $<TARGET_FILE_DIR:pu_firmware>/pu_firmware.bin
            --output ${CMAKE_CURRENT_BINARY_DIR}/pico_universal_${PU_BOARD}_slot${PU_SLOT}.uf2
            --verify-rom-model
        DEPENDS pu_bootloader pu_firmware
        COMMENT "Packaging verified 4 KiB sector-padded unified UF2 (${PU_BOARD}, Slot ${PU_SLOT})"
    )
endif()
