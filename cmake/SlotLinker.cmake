# Generate partition-bounded RP2040 linker scripts directly from config/flash_layout.json
# so bootloader (60 KiB at 0x10000000) and A/B firmware slots (960 KiB - 256 B header at
# +0x100 without duplicate boot2) are enforced at link time.

function(pu_configure_flash_slot TARGET SLOT_NAME WITH_BOOT2)
    # Only the sector-padded unified UF2 from tools/package_uf2.py is user-facing.
    set(PICO_NO_UF2 1 PARENT_SCOPE)

    file(READ "${CMAKE_CURRENT_LIST_DIR}/config/flash_layout.json" LAYOUT_JSON)
    string(JSON FLASH_BASE GET "${LAYOUT_JSON}" flash_base)
    string(JSON PART_COUNT LENGTH "${LAYOUT_JSON}" partitions)
    math(EXPR LAST_IDX "${PART_COUNT} - 1")

    set(FOUND FALSE)
    foreach(I RANGE ${LAST_IDX})
        string(JSON PNAME GET "${LAYOUT_JSON}" partitions ${I} name)
        if(PNAME STREQUAL "${SLOT_NAME}")
            string(JSON POFFSET GET "${LAYOUT_JSON}" partitions ${I} offset)
            string(JSON PSIZE GET "${LAYOUT_JSON}" partitions ${I} size)
            # Both bootloader and chainloaded A/B slots link at their partition base so
            # Pico SDK's 256-byte .boot2 occupies +0x000..0x0FF and .vectors starts at
            # +0x100; tools/package_uf2.py overwrites slot +0x000..0x0FF with pu_image_descriptor_t.
            math(EXPR SLOT_ORIGIN "${FLASH_BASE} + ${POFFSET}" OUTPUT_FORMAT HEXADECIMAL)
            set(SLOT_LENGTH "${PSIZE}")
            set(FOUND TRUE)
        endif()
    endforeach()

    if(NOT FOUND)
        message(FATAL_ERROR "Unknown flash_layout.json partition '${SLOT_NAME}' for target ${TARGET}")
    endif()

    if(COMMAND pico_add_linker_script_override_path)
        set(OVERRIDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/ld_override_${TARGET}_${SLOT_NAME}")
        file(MAKE_DIRECTORY "${OVERRIDE_DIR}")
        file(WRITE "${OVERRIDE_DIR}/memory_flash.incl"
            "MEMORY\n{\n    FLASH(rx) : ORIGIN = ${SLOT_ORIGIN}, LENGTH = ${SLOT_LENGTH}\n}\n")
        pico_add_linker_script_override_path(${TARGET} "${OVERRIDE_DIR}" FILES memory_flash.incl)
    else()
        set(BASE_LD "${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2040/memmap_default.ld")
        if(NOT EXISTS "${BASE_LD}")
            set(BASE_LD "${PICO_SDK_PATH}/src/rp2_common/pico_standard_link/memmap_default.ld")
        endif()
        if(NOT EXISTS "${BASE_LD}")
            message(FATAL_ERROR "Cannot locate RP2040 memmap_default.ld inside PICO_SDK_PATH=${PICO_SDK_PATH}")
        endif()

        file(READ "${BASE_LD}" LD_CONTENT)
        string(REGEX REPLACE
            "FLASH\\(rx\\)[ \t]*:[ \t]*ORIGIN[ \t]*=[ \t]*0x[0-9A-Fa-f]+,[ \t]*LENGTH[ \t]*=[ \t]*[0-9A-Za-z_]+"
            "FLASH(rx) : ORIGIN = ${SLOT_ORIGIN}, LENGTH = ${SLOT_LENGTH}"
            LD_CONTENT "${LD_CONTENT}")
        string(REPLACE
            "INCLUDE \"pico_flash_region.ld\""
            "FLASH(rx) : ORIGIN = ${SLOT_ORIGIN}, LENGTH = ${SLOT_LENGTH}"
            LD_CONTENT "${LD_CONTENT}")

        set(GEN_LD "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}_${SLOT_NAME}.ld")
        file(WRITE "${GEN_LD}" "${LD_CONTENT}")
        pico_set_linker_script(${TARGET} "${GEN_LD}")
    endif()
endfunction()
