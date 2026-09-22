#define _GNU_SOURCE
#ifdef NDEBUG
#error Firmware test assertions must remain enabled
#endif
#include <assert.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#include "config/app_config.h"
#include "config/memory_map.h"
#include "include/bootloader.h"
#include "include/button.h"
#include "include/image_validator.h"
#include "include/led_indicator.h"
#include "include/storage.h"
#include "include/crc32.h"
#include "include/image_format.h"
#include "hardware/structs/watchdog.h"
#include "pico/time.h"

static uint64_t clock_ms;
static bool gpio_levels[30];
static watchdog_hw_t watchdog_registers;
watchdog_hw_t *watchdog_hw = &watchdog_registers;
static bool watchdog_reset;
static jmp_buf escape;
static unsigned reboot_count, jump_count;
static uint32_t jumped_vtor, jumped_msp, jumped_entry;
static uint64_t stop_at, press_at, release_at;
static bool drive_ui, ignore_flash_program;
static unsigned erase_count, program_count;
static int64_t power_cut_after = -1;
static bool (*runtime_tick)(repeating_timer_t *);
static uint64_t extra_presses[3][2];
static unsigned extra_press_count;
static bool check_mode_leds;
static probe_mode_t expected_led_mode;

static void assert_mode_leds(probe_mode_t mode) {
    assert(gpio_levels[PIN_LED_MODE1_DAP] == (mode == MODE_CMSIS_DAP));
    assert(gpio_levels[PIN_LED_MODE2_BMP] == (mode == MODE_BLACKMAGIC));
    assert(gpio_levels[PIN_LED_MODE3_WCH] == (mode == MODE_PICORVD));
}

bool add_repeating_timer_ms(int32_t delay_ms, bool (*callback)(repeating_timer_t *),
                            void *user_data, repeating_timer_t *timer) {
    assert(delay_ms == -5 && user_data == NULL && timer != NULL);
    runtime_tick = callback;
    return true;
}

static void flash_step(void) {
    if (power_cut_after == 0) longjmp(escape, 4);
    if (power_cut_after > 0) --power_cut_after;
}

void gpio_init(unsigned pin) { (void)pin; }
void gpio_set_dir(unsigned pin, bool output) { (void)pin; (void)output; }
void gpio_pull_up(unsigned pin) { gpio_levels[pin] = true; }
bool gpio_get(unsigned pin) {
    if (drive_ui && pin == PIN_MODE_BUTTON) {
        for (unsigned i = 0; i < extra_press_count; ++i)
            if (clock_ms >= extra_presses[i][0] && clock_ms < extra_presses[i][1]) return false;
        return !(clock_ms >= press_at && clock_ms < release_at);
    }
    return gpio_levels[pin];
}
void gpio_put(unsigned pin, bool value) { gpio_levels[pin] = value; }
absolute_time_t get_absolute_time(void) { return clock_ms; }
uint32_t to_ms_since_boot(absolute_time_t time) { return (uint32_t)time; }
void sleep_ms(uint32_t ms) {
    if (check_mode_leds) assert_mode_leds(expected_led_mode);
    clock_ms += ms;
    if (drive_ui && clock_ms >= stop_at) longjmp(escape, 3);
}
void tight_loop_contents(void) { assert(!"unexpected spin"); }
bool watchdog_caused_reboot(void) { return watchdog_reset; }
void watchdog_reboot(uint32_t pc, uint32_t sp, uint32_t delay_ms) {
    assert(pc == 0 && sp == 0 && delay_ms == 10);
    reboot_count++;
    longjmp(escape, 1);
}
void bootloader_enter_image(uint32_t vtor, uint32_t msp, uint32_t entry) {
    jump_count++;
    jumped_vtor = vtor;
    jumped_msp = msp;
    jumped_entry = entry;
    longjmp(escape, 2);
}
uint32_t save_and_disable_interrupts(void) { return 1; }
void restore_interrupts(uint32_t state) { assert(state == 1); }
void flash_range_erase(uint32_t offset, size_t count) {
    assert((offset == SLOT_CONFIG_OFFSET || offset == SLOT_CONFIG_BACKUP_OFFSET) && count == 4096);
    uint8_t *destination = (uint8_t *)(uintptr_t)(FLASH_BASE_ADDR + offset);
    for (size_t byte = 0; byte < count; ++byte) {
        flash_step();
        destination[byte] = 0xff;
    }
    erase_count++;
}
void flash_range_program(uint32_t offset, const uint8_t *data, size_t count) {
    assert(count == 256 && offset % 256 == 0);
    assert((offset >= SLOT_CONFIG_OFFSET && offset < SLOT_CONFIG_OFFSET + SLOT_CONFIG_SIZE) ||
           (offset >= SLOT_CONFIG_BACKUP_OFFSET && offset < SLOT_CONFIG_BACKUP_OFFSET + SLOT_CONFIG_BACKUP_SIZE));
    uint8_t *destination = (uint8_t *)(uintptr_t)(FLASH_BASE_ADDR + offset);
    if (!ignore_flash_program) {
        for (size_t byte = 0; byte < count; ++byte) {
            flash_step();
            assert((destination[byte] & data[byte]) == data[byte]);
            destination[byte] &= data[byte];
        }
    }
    program_count++;
}
extern void (*test_early_init)(void);
#define main supervisor_main
#include "src/main.c"
#undef main

static void reset_test(void) {
    clock_ms = 0;
    memset(gpio_levels, 0, sizeof(gpio_levels));
    memset(&watchdog_registers, 0, sizeof(watchdog_registers));
    memset((void *)(uintptr_t)FLASH_BASE_ADDR, 0xff, FLASH_TOTAL_SIZE);
    watchdog_reset = false;
    reboot_count = jump_count = erase_count = program_count = 0;
    drive_ui = ignore_flash_program = false;
    power_cut_after = -1;
    extra_press_count = 0;
    check_mode_leds = false;
    runtime_tick = NULL;
    button_init();
    led_init();
}

static void seal_image(uint32_t base) {
    image_descriptor_t descriptor = {
        .magic = IMAGE_DESCRIPTOR_MAGIC, .version = IMAGE_DESCRIPTOR_VERSION,
        .base = base, .length = 768,
        .mode = base == SLOT_1_CMSIS_DAP_ADDR ? MODE_CMSIS_DAP :
                base == SLOT_2_BLACKMAGIC_ADDR ? MODE_BLACKMAGIC : MODE_PICORVD,
    };
    descriptor.image_crc32 = programmer_crc32((void *)(uintptr_t)base, descriptor.length);
    descriptor.header_crc32 = programmer_crc32(&descriptor, 28);
    memcpy((void *)(uintptr_t)(base + bootloader_get_slot_size((probe_mode_t)descriptor.mode) - 256),
           &descriptor, sizeof(descriptor));
}

static void valid_image(uint32_t base) {
    uint32_t *vectors = (uint32_t *)(uintptr_t)(base + 256);
    vectors[0] = 0x20042000;
    vectors[1] = base + 0x201;
    *(uint16_t *)(uintptr_t)(base + 0x200) = 0x4770;
    seal_image(base);
}

static button_event_t poll_after(uint32_t ms, bool pressed) {
    clock_ms += ms;
    gpio_levels[PIN_MODE_BUTTON] = !pressed;
    return button_poll();
}

static void test_button(void) {
    reset_test();
    assert(poll_after(0, true) == BUTTON_EVENT_NONE);
    assert(button_is_busy());
    assert(poll_after(5, false) == BUTTON_EVENT_NONE); /* contact bounce */
    assert(!button_is_busy());
    assert(poll_after(1, true) == BUTTON_EVENT_NONE);
    assert(poll_after(20, true) == BUTTON_EVENT_NONE);
    assert(poll_after(100, false) == BUTTON_EVENT_NONE);
    assert(button_is_busy());
    assert(poll_after(19, false) == BUTTON_EVENT_NONE);
    assert(poll_after(1, false) == BUTTON_EVENT_SHORT_PRESS);
    assert(!button_is_busy());
    assert(poll_after(1, false) == BUTTON_EVENT_NONE);

    assert(poll_after(1, true) == BUTTON_EVENT_NONE);
    assert(poll_after(20, true) == BUTTON_EVENT_NONE);
    assert(poll_after(1999, true) == BUTTON_EVENT_NONE);
    assert(poll_after(1, true) == BUTTON_EVENT_LONG_PRESS);
    assert(poll_after(2000, true) == BUTTON_EVENT_NONE);
    assert(poll_after(1, false) == BUTTON_EVENT_NONE);
    assert(poll_after(20, false) == BUTTON_EVENT_NONE);

    reset_test();
    clock_ms = UINT32_MAX - 10u;
    assert(poll_after(0, true) == BUTTON_EVENT_NONE);
    assert(poll_after(20, true) == BUTTON_EVENT_NONE); /* 32-bit wrap */
    assert(poll_after(10, false) == BUTTON_EVENT_NONE);
    assert(poll_after(20, false) == BUTTON_EVENT_SHORT_PRESS);
}

static void test_vectors_and_slots(void) {
    reset_test();
    for (unsigned mode = 0; mode < MODE_COUNT; ++mode) {
        uint32_t base = bootloader_get_slot_address((probe_mode_t)mode);
        assert(!bootloader_mode_is_valid((probe_mode_t)mode));
        valid_image(base);
        assert(bootloader_mode_is_valid((probe_mode_t)mode));
        uint32_t *v = (uint32_t *)(uintptr_t)(base + 256);
        v[0] = 0x20000000;
        seal_image(base);
        assert(!bootloader_mode_is_valid((probe_mode_t)mode));
        v[0] = 0x20042000;
        v[1] &= ~1u;
        seal_image(base);
        assert(!bootloader_mode_is_valid((probe_mode_t)mode));
    }
    assert(bootloader_get_slot_address(MODE_BLACKMAGIC) == SLOT_2_BLACKMAGIC_ADDR);
    assert(bootloader_get_slot_address((probe_mode_t)255) == 0);
    assert(bootloader_get_slot_size((probe_mode_t)255) == 0);
    assert(!bootloader_mode_is_valid((probe_mode_t)255));
    assert(!image_validate_slot(0xffffffff, 512));
    assert(!image_validate_slot(FLASH_BASE_ADDR, UINT32_MAX));
    assert(!image_validate_slot(FLASH_BASE_ADDR + FLASH_TOTAL_SIZE, 512));
    assert(!image_validate_slot(FLASH_BASE_ADDR, 0));
}

static void test_handover(void) {
    reset_test();
    assert(!bootloader_jump_to_mode(MODE_BLACKMAGIC));
    assert(reboot_count == 0);
    valid_image(SLOT_2_BLACKMAGIC_ADDR);
    int reason = setjmp(escape);
    if (reason == 0) bootloader_jump_to_mode(MODE_BLACKMAGIC);
    assert(reason == 1 && reboot_count == 1);
    assert(watchdog_hw->scratch[2] == ~watchdog_hw->scratch[1]);
    watchdog_reset = true;
    reason = setjmp(escape);
    if (reason == 0) test_early_init();
    assert(reason == 2 && jump_count == 1);
    assert(jumped_vtor == SLOT_2_BLACKMAGIC_ADDR + 256);
    assert(jumped_msp == 0x20042000 && jumped_entry == SLOT_2_BLACKMAGIC_ADDR + 0x201);
    assert(watchdog_hw->scratch[1] == 0 && watchdog_hw->scratch[2] == 0);
    test_early_init(); /* request consumed, no second jump */
    assert(jump_count == 1);
}

static void test_led_recovery(void) {
    for (unsigned mode = 0; mode < MODE_COUNT; ++mode) {
        reset_test();
        led_set_mode((probe_mode_t)mode);
        led_set_error();
        assert_mode_leds((probe_mode_t)mode);
        for (unsigned tick = 0; tick < 8; ++tick) {
            bool previous = gpio_levels[PIN_ONBOARD_LED];
            clock_ms += 250;
            led_tick();
            assert(gpio_levels[PIN_ONBOARD_LED] != previous);
            assert_mode_leds((probe_mode_t)mode);
        }
        /* Saving never makes the mode LEDs look like a cycling selection. */
        check_mode_leds = true;
        expected_led_mode = (probe_mode_t)mode;
        led_flash_save_confirmation();
        assert_mode_leds((probe_mode_t)mode);
        /* Successful save clears the error and resumes the selection pattern. */
        clock_ms += LED_BLINK_PULSE_MS;
        led_tick();
        assert(!gpio_levels[PIN_ONBOARD_LED]);
        led_set_mode(MODE_BLACKMAGIC);
        assert_mode_leds(MODE_BLACKMAGIC);
    }
}

static void test_storage(void) {
    reset_test();
    assert(storage_get_default_mode() == MODE_CMSIS_DAP);
    storage_set_active_mode(MODE_PICORVD);
    assert(storage_get_active_mode() == MODE_PICORVD);
    assert(program_count == 0 && erase_count == 0);
    assert(storage_save_default_mode(MODE_BLACKMAGIC));
    watchdog_hw->scratch[0] = 0;
    assert(storage_get_active_mode() == MODE_BLACKMAGIC);
    unsigned writes_before = program_count;
    assert(storage_save_default_mode(MODE_BLACKMAGIC));
    assert(program_count == writes_before); /* no-op save avoids flash wear */
    assert(!storage_save_default_mode((probe_mode_t)255));
    ignore_flash_program = true;
    assert(!storage_save_default_mode(MODE_PICORVD));
    assert(storage_get_default_mode() == MODE_BLACKMAGIC);
    assert(storage_get_active_mode() == MODE_BLACKMAGIC);
}

static void test_storage_power_cuts(void) {
    reset_test();
    assert(programmer_crc32("123456789", 9) == 0xcbf43926u);
    assert(programmer_crc32("", 0) == 0);
    /* Fill the first sector; latest mode differs from fallback so losing all
     * durable records cannot accidentally pass this test. */
    for (unsigned i = 0; i < 16; ++i)
        assert(storage_save_default_mode((probe_mode_t)((i + 1) % MODE_COUNT)));
    assert(storage_get_default_mode() == MODE_BLACKMAGIC);
    uint8_t original[4096], backup[4096];
    memcpy(original, (void *)(uintptr_t)SLOT_CONFIG_ADDR, sizeof(original));
    memcpy(backup, (void *)(uintptr_t)SLOT_CONFIG_BACKUP_ADDR, sizeof(backup));
    /* Cut after every possible byte of inactive-bank erase and page program. */
    for (volatile unsigned cut = 0; cut <= 4096 + 256; ++cut) {
        memcpy((void *)(uintptr_t)SLOT_CONFIG_ADDR, original, sizeof(original));
        memcpy((void *)(uintptr_t)SLOT_CONFIG_BACKUP_ADDR, backup, sizeof(backup));
        power_cut_after = cut;
        int reason = setjmp(escape);
        if (reason == 0) assert(storage_save_default_mode(MODE_PICORVD));
        else assert(reason == 4);
        power_cut_after = -1;
        watchdog_hw->scratch[0] = 0;
        storage_init();
        probe_mode_t recovered = storage_get_active_mode();
        assert(recovered == MODE_BLACKMAGIC || recovered == MODE_PICORVD);
        assert(memcmp((void *)(uintptr_t)SLOT_CONFIG_ADDR, original, sizeof(original)) == 0);
    }
    /* Corrupt the newest record: the previous committed bank still wins. */
    ((uint8_t *)(uintptr_t)SLOT_CONFIG_BACKUP_ADDR)[16] ^= 1;
    assert(storage_get_default_mode() == MODE_BLACKMAGIC);

    reset_test();
    persistent_config_t legacy = {CONFIG_SECTOR_MAGIC, MODE_PICORVD, 17, 0};
    legacy.crc32 = legacy.magic ^ legacy.default_mode ^ legacy.boot_count;
    memcpy((void *)(uintptr_t)SLOT_CONFIG_ADDR, &legacy, sizeof(legacy));
    assert(storage_get_default_mode() == MODE_PICORVD);
    assert(storage_save_default_mode(MODE_PICORVD));
    assert(memcmp((void *)(uintptr_t)SLOT_CONFIG_ADDR, &legacy, sizeof(legacy)) == 0);
    assert(storage_get_default_mode() == MODE_PICORVD);
}

static void test_ui_deadline_and_invalid_slot(void) {
    reset_test();
    valid_image(SLOT_1_CMSIS_DAP_ADDR);
    valid_image(SLOT_2_BLACKMAGIC_ADDR);
    drive_ui = true;
    press_at = BOOT_HANDOVER_DELAY_MS - 50;
    release_at = BOOT_HANDOVER_DELAY_MS + 5;
    stop_at = BOOT_HANDOVER_DELAY_MS * 3;
    int reason = setjmp(escape);
    if (reason == 0) supervisor_main();
    assert(reason == 1);
    assert(storage_get_active_mode() == MODE_BLACKMAGIC);
    assert(clock_ms >= release_at + BUTTON_DEBOUNCE_MS + BOOT_HANDOVER_DELAY_MS);

    reset_test();
    drive_ui = true;
    press_at = release_at = UINT64_MAX;
    stop_at = BOOT_HANDOVER_DELAY_MS * 2;
    reason = setjmp(escape);
    if (reason == 0) supervisor_main();
    assert(reason == 3 && reboot_count == 0); /* invalid image stays interactive */
}

static void test_runtime_controls(void) {
    for (volatile unsigned mode = 0; mode < MODE_COUNT; ++mode) {
        reset_test();
        storage_set_active_mode((probe_mode_t)mode);
        watchdog_hw->scratch[3] = PROBE_WATCHDOG_TOKEN(mode);
        assert(probe_controls_init((probe_mode_t)mode));
        int reason = setjmp(escape);
        assert(reason == 0); /* Any button-triggered reboot fails this test. */
        for (unsigned tick = 0; tick < 4000; ++tick) {
            /* 20 seconds of noise, short taps, a long hold and release. */
            gpio_levels[PIN_MODE_BUTTON] = tick < 400 ? (tick % 2 != 0) :
                tick < 800 ? (tick % 40 >= 20) : tick >= 3000;
            clock_ms += 5;
            assert(runtime_tick(NULL));
            assert_mode_leds((probe_mode_t)mode);
            assert(gpio_levels[PIN_ONBOARD_LED]);
            assert(storage_get_active_mode() == (probe_mode_t)mode);
            assert(watchdog_hw->scratch[3] == PROBE_WATCHDOG_TOKEN(mode));
        }
        assert(reboot_count == 0 && program_count == 0 && erase_count == 0);
    }
    assert(!probe_controls_init((probe_mode_t)MODE_COUNT));
    assert(!probe_controls_init((probe_mode_t)-1));
    /* Keep accepting a checked pending request from a previous probe build. */
    reset_test();
    watchdog_reset = true;
    watchdog_hw->scratch[3] = 0xc0de0000u | ((MODE_PICORVD ^ 0xffu) << 8) | MODE_PICORVD;
    probe_mode_t mode;
    assert(probe_controls_take_save_request(&mode) && mode == MODE_PICORVD);
    assert(!probe_controls_take_save_request(&mode));
}

static void test_startup_selection(void) {
    assert(BOOT_HANDOVER_DELAY_MS == 5000u);
    for (volatile unsigned mode = 0; mode < MODE_COUNT; ++mode) {
        reset_test();
        valid_image(bootloader_get_slot_address((probe_mode_t)mode));
        storage_set_active_mode((probe_mode_t)mode);
        drive_ui = true;
        press_at = release_at = UINT64_MAX;
        stop_at = 6000;
        int reason = setjmp(escape);
        if (!reason) supervisor_main();
        assert(reason == 1 && reboot_count == 1 && clock_ms == 5000);
        assert_mode_leds((probe_mode_t)mode);
    }

    reset_test();
    valid_image(SLOT_1_CMSIS_DAP_ADDR);
    drive_ui = true;
    press_at = 100;
    release_at = 200;
    extra_presses[0][0] = 1000;
    extra_presses[0][1] = 1100;
    extra_presses[1][0] = 5500;
    extra_presses[1][1] = 5600;
    extra_press_count = 2;
    stop_at = 12000;
    int reason = setjmp(escape);
    if (!reason) supervisor_main();
    assert(reason == 1 && reboot_count == 1);
    assert(clock_ms == 5600 + BUTTON_DEBOUNCE_MS + 5000);
    assert(storage_get_active_mode() == MODE_CMSIS_DAP); /* three taps wrap once */

    reset_test();
    valid_image(SLOT_1_CMSIS_DAP_ADDR);
    drive_ui = true;
    press_at = 100;
    release_at = 10000;
    stop_at = 9000;
    reason = setjmp(escape);
    if (!reason) supervisor_main();
    assert(reason == 3 && reboot_count == 0); /* held button never auto-cycles */
    assert_mode_leds(MODE_CMSIS_DAP);
    assert(program_count == 1); /* long hold saves only once */

    reset_test();
    valid_image(SLOT_1_CMSIS_DAP_ADDR);
    drive_ui = true;
    press_at = 100;
    release_at = 10000;
    stop_at = 16000;
    reason = setjmp(escape);
    if (!reason) supervisor_main();
    assert(reason == 1 && reboot_count == 1);
    assert(clock_ms == release_at + BUTTON_DEBOUNCE_MS + 5000);
    assert(storage_get_active_mode() == MODE_CMSIS_DAP); /* no extra tap after hold */
    assert(program_count == 1);
}

static void test_probe_fault_recovery(void) {
    reset_test();
    watchdog_hw->scratch[3] = PROBE_WATCHDOG_TOKEN(MODE_PICORVD);
    assert(!probe_controls_take_fault_request()); /* cold reset is not a fault */
    watchdog_reset = true;
    watchdog_hw->scratch[3] = PROBE_WATCHDOG_TOKEN(MODE_PICORVD) ^ 0x100;
    assert(!probe_controls_take_fault_request());
    watchdog_hw->scratch[3] = PROBE_WATCHDOG_TOKEN(MODE_PICORVD);
    assert(probe_controls_take_fault_request());
    assert(!probe_controls_take_fault_request());
    valid_image(SLOT_1_CMSIS_DAP_ADDR);
    watchdog_hw->scratch[3] = PROBE_WATCHDOG_TOKEN(MODE_PICORVD);
    drive_ui = true;
    press_at = release_at = UINT64_MAX;
    stop_at = BOOT_HANDOVER_DELAY_MS * 2;
    int reason = setjmp(escape);
    if (!reason) supervisor_main();
    assert(reason == 3 && reboot_count == 0); /* no crash/reboot loop */

    reset_test();
    valid_image(SLOT_1_CMSIS_DAP_ADDR);
    watchdog_reset = true;
    watchdog_hw->scratch[3] = PROBE_WATCHDOG_TOKEN(MODE_CMSIS_DAP);
    drive_ui = true;
    press_at = 100;
    release_at = 3000;
    stop_at = 9000;
    reason = setjmp(escape);
    if (!reason) supervisor_main();
    assert(reason == 1 && reboot_count == 1); /* explicit save permits recovery retry */
    assert(clock_ms == release_at + BUTTON_DEBOUNCE_MS + 5000);
    assert_mode_leds(MODE_CMSIS_DAP);
}

static void test_packager_compatibility(const char *path) {
    reset_test();
    FILE *input = fopen(path, "rb");
    assert(input);
    assert(fread((void *)(uintptr_t)FLASH_BASE_ADDR, 1, FLASH_TOTAL_SIZE, input) == FLASH_TOTAL_SIZE);
    assert(fclose(input) == 0);
    for (unsigned mode = 0; mode < MODE_COUNT; ++mode) {
        assert(bootloader_mode_is_valid((probe_mode_t)mode));
        uint32_t base = bootloader_get_slot_address((probe_mode_t)mode);
        uint8_t *data = (uint8_t *)(uintptr_t)base;
        data[600] ^= 1;
        assert(!bootloader_mode_is_valid((probe_mode_t)mode));
        data[600] ^= 1;
        uint8_t *descriptor = data + bootloader_get_slot_size((probe_mode_t)mode) - 256;
        for (unsigned byte = 0; byte < sizeof(image_descriptor_t); ++byte) {
            descriptor[byte] ^= 1;
            assert(!bootloader_mode_is_valid((probe_mode_t)mode));
            descriptor[byte] ^= 1;
        }
        assert(bootloader_mode_is_valid((probe_mode_t)mode));
    }
}

int main(int argc, char **argv) {
    assert(argc == 2);
    void *flash = mmap((void *)(uintptr_t)FLASH_BASE_ADDR, FLASH_TOTAL_SIZE,
                       PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(flash == (void *)(uintptr_t)FLASH_BASE_ADDR);
    test_button();
    test_vectors_and_slots();
    test_handover();
    test_led_recovery();
    test_storage();
    test_storage_power_cuts();
    test_ui_deadline_and_invalid_slot();
    test_startup_selection();
    test_runtime_controls();
    test_probe_fault_recovery();
    test_packager_compatibility(argv[1]);
    munmap(flash, FLASH_TOTAL_SIZE);
    puts("Firmware host tests passed: debounce, wraparound, vectors, handover requests, LEDs, storage, five-second selection, runtime mode lock, UI recovery");
    return 0;
}
