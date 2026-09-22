cmake_minimum_required(VERSION 3.21)

# Original Pico is the hardware baseline. Pin SDK sources for repeatable builds.
set(PICO_BOARD pico CACHE STRING "Programmer hardware")
set(PICO_SDK_FETCH_FROM_GIT_TAG a1438dff1d38bd9c65dbd693f0e5db4b9ae91779 CACHE STRING "Pico SDK 2.2.0")

# Include Pico SDK configuration
include(pico_sdk_import.cmake)

project(pico_universal_programmer C CXX ASM)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/tools/flash_layout.py --check
    RESULT_VARIABLE LAYOUT_CHECK)
if(NOT LAYOUT_CHECK EQUAL 0)
    message(FATAL_ERROR "Generated flash layout is stale or invalid")
endif()
file(READ ${CMAKE_CURRENT_LIST_DIR}/config/flash_layout.json FLASH_LAYOUT_JSON)
string(JSON SUPERVISOR_SIZE GET "${FLASH_LAYOUT_JSON}" partitions 0 size)
string(JSON PHYSICAL_FLASH_SIZE GET "${FLASH_LAYOUT_JSON}" flash_size)
# The SDK linker region must stop before persistent configuration.
set(PICO_FLASH_SIZE_BYTES ${SUPERVISOR_SIZE})

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the Raspberry Pi Pico SDK
pico_sdk_init()
if(NOT PICO_BOARD STREQUAL "pico" OR NOT PICO_PLATFORM STREQUAL "rp2040")
    message(FATAL_ERROR "This hardware configuration supports the original RP2040 Pico only")
endif()
if(NOT PICO_SDK_VERSION_STRING VERSION_EQUAL "2.2.0")
    message(FATAL_ERROR "Use pinned Pico SDK 2.2.0; other SDKs have not been validated")
endif()

# Define the main executable
add_executable(pico_universal_programmer
    src/main.c
    src/button.c
    src/led_indicator.c
    src/storage.c
    src/crc32.c
    src/probe_control_request.c
    src/image_validator.c
    src/bootloader.c
    src/handover.S
)

# Include directories
target_include_directories(pico_universal_programmer PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
    ${CMAKE_CURRENT_LIST_DIR}/include
    ${CMAKE_CURRENT_LIST_DIR}/config
)

# Keep physical flash API bounds distinct from the supervisor linker capacity.
target_compile_definitions(pico_universal_programmer PRIVATE PICO_FLASH_SIZE_BYTES=${PHYSICAL_FLASH_SIZE})

# Strict compiler warnings for high code quality
target_compile_options(pico_universal_programmer PRIVATE
    -Wall
    -Wextra
    -Werror
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

# Component outputs are build inputs, not standalone firmware to flash.
# tools/build.py --all emits the single complete UF2 after merging all slots.
set(PICO_NO_UF2 1)
pico_add_extra_outputs(pico_universal_programmer)
