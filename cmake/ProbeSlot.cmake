# Configure a slot-specific linker script without changing physical flash bounds.
function(programmer_probe_slot TARGET SLOT_NAME BINARY_TYPE)
    # Only the combined package is a user-facing UF2. Slot BIN/ELF files remain
    # available for linking, verification and packaging.
    set(PICO_NO_UF2 1 PARENT_SCOPE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    file(READ "${PROGRAMMER_ROOT}/config/flash_layout.json" LAYOUT)
    string(JSON BASE GET "${LAYOUT}" flash_base)
    string(JSON COUNT LENGTH "${LAYOUT}" partitions)
    math(EXPR LAST "${COUNT} - 1")
    set(FOUND FALSE)
    foreach(I RANGE ${LAST})
        string(JSON NAME GET "${LAYOUT}" partitions ${I} name)
        if(NAME STREQUAL SLOT_NAME)
            string(JSON OFFSET GET "${LAYOUT}" partitions ${I} offset)
            string(JSON SIZE GET "${LAYOUT}" partitions ${I} size)
            math(EXPR ORIGIN "${BASE} + ${OFFSET}" OUTPUT_FORMAT HEXADECIMAL)
            math(EXPR CAPACITY "${SIZE} - 256")
            set(FOUND TRUE)
        endif()
    endforeach()
    if(NOT FOUND)
        message(FATAL_ERROR "Unknown probe slot ${SLOT_NAME}")
    endif()
    file(READ "${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2040/memmap_${BINARY_TYPE}.ld" SCRIPT)
    string(REPLACE "INCLUDE \"pico_flash_region.ld\""
        "FLASH(rx) : ORIGIN = ${ORIGIN}, LENGTH = ${CAPACITY}" SCRIPT "${SCRIPT}")
    set(LINKER "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_slot.ld")
    file(WRITE "${LINKER}" "${SCRIPT}")
    pico_set_linker_script(${TARGET} "${LINKER}")
    pico_set_binary_type(${TARGET} ${BINARY_TYPE})
    target_sources(${TARGET} PRIVATE
        "${PROGRAMMER_ROOT}/src/probe_controls.c"
        "${PROGRAMMER_ROOT}/src/button.c"
        "${PROGRAMMER_ROOT}/src/led_indicator.c")
    target_include_directories(${TARGET} PRIVATE "${PROGRAMMER_ROOT}")
    target_link_libraries(${TARGET} hardware_watchdog)
endfunction()
