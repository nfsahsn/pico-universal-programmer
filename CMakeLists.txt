cmake_minimum_required(VERSION 3.13)

# Include Pico SDK configuration
include(pico_sdk_import.cmake)

project(pico_universal_programmer C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()

# Define the main executable
add_executable(pico_universal_programmer
    src/main.c
    src/button.c
    src/led_indicator.c
    src/storage.c
    src/image_validator.c
    src/bootloader.c
)

# Include directories
target_include_directories(pico_universal_programmer PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/config
)

# Strict compiler warnings for high code quality
target_compile_options(pico_universal_programmer PRIVATE
    -Wall
    -Wextra
    -Werror=implicit-function-declaration
    -Wno-unused-parameter
)

# Link hardware libraries from Pico SDK
target_link_libraries(pico_universal_programmer PRIVATE
    pico_stdlib
    hardware_flash
    hardware_resets
    hardware_watchdog
    hardware_sync
)

# Enable USB and UART outputs for debugging if needed
pico_enable_stdio_usb(pico_universal_programmer 0)
pico_enable_stdio_uart(pico_universal_programmer 0)

# Generate UF2, BIN, and map files automatically
pico_add_extra_outputs(pico_universal_programmer)
