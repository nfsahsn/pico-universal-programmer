cmake_minimum_required(VERSION 3.21)
get_filename_component(PROGRAMMER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${PROGRAMMER_ROOT}/pico_sdk_import.cmake")
project(universal_picorvd C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
pico_sdk_init()
if(NOT PICO_BOARD STREQUAL "pico" OR NOT PICO_SDK_VERSION_STRING VERSION_EQUAL "2.2.0")
    message(FATAL_ERROR "Use original Pico with pinned SDK 2.2.0")
endif()
if(NOT DEFINED PICORVD_SOURCE)
    set(PICORVD_SOURCE "${PROGRAMMER_ROOT}/.tools/picorvd")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/prepare.py"
    "${PICORVD_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/prepare.py")
# Include the upstream main through a stable directory alias for the wrapper.
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/upstream/PicoRVD/src")
configure_file("${CMAKE_CURRENT_BINARY_DIR}/generated/main.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/upstream/PicoRVD/src/main.cpp" COPYONLY)
add_executable(picorvd main.cpp)
foreach(SOURCE PicoSWIO RVDebug WCHFlash SoftBreak GDBServer utils usb_config)
    target_sources(picorvd PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/${SOURCE}.cpp")
endforeach()
target_include_directories(picorvd PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/upstream"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/generated" "${PICORVD_SOURCE}" "${PICORVD_SOURCE}/src")
pico_generate_pio_header(picorvd "${CMAKE_CURRENT_BINARY_DIR}/generated/singlewire.pio"
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/bin")
target_compile_definitions(picorvd PRIVATE PARAM_ASSERTIONS_ENABLE_ALL=1)
pico_enable_stdio_usb(picorvd 0)
pico_enable_stdio_uart(picorvd 1)
target_link_libraries(picorvd pico_stdlib hardware_pio tinyusb_device)
include("${PROGRAMMER_ROOT}/cmake/ProbeSlot.cmake")
programmer_probe_slot(picorvd picorvd default)
pico_add_extra_outputs(picorvd)
