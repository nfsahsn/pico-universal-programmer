/** Redundant append-only configuration journal, with legacy read migration. */
#include "include/storage.h"
#include "include/crc32.h"
#include "config/memory_map.h"
#include "hardware/flash.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include <stddef.h>
#include <string.h>

#define JOURNAL_MAGIC 0x55504346u
#define JOURNAL_VERSION 1u

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t sequence;
    uint32_t mode;
    uint32_t crc32;
} journal_record_t;

_Static_assert(sizeof(journal_record_t) == 20, "journal format must be stable");
_Static_assert(SLOT_CONFIG_SIZE == FLASH_SECTOR_SIZE, "journal sector size");
_Static_assert(SLOT_CONFIG_BACKUP_SIZE == FLASH_SECTOR_SIZE, "journal backup size");

static const uint32_t sector_offsets[2] = {SLOT_CONFIG_OFFSET, SLOT_CONFIG_BACKUP_OFFSET};

static const uint8_t *flash_at(uint32_t offset) {
    return (const uint8_t *)(uintptr_t)(FLASH_BASE_ADDR + offset);
}

static bool record_valid(const journal_record_t *record) {
    return record->magic == JOURNAL_MAGIC && record->version == JOURNAL_VERSION &&
           record->mode < MODE_COUNT &&
           record->crc32 == programmer_crc32(record, offsetof(journal_record_t, crc32));
}

static bool newer(uint32_t candidate, uint32_t reference) {
    uint32_t delta = candidate - reference;
    return delta != 0 && delta < 0x80000000u;
}

static const journal_record_t *latest_record(unsigned *sector) {
    const journal_record_t *latest = NULL;
    for (unsigned bank = 0; bank < 2; ++bank) {
        for (unsigned page = 0; page < FLASH_SECTOR_SIZE; page += FLASH_PAGE_SIZE) {
            const journal_record_t *record = (const journal_record_t *)flash_at(sector_offsets[bank] + page);
            if (record_valid(record) && (!latest || newer(record->sequence, latest->sequence))) {
                latest = record;
                *sector = bank;
            }
        }
    }
    return latest;
}

static bool find_erased_page(unsigned bank, uint32_t *offset) {
    for (unsigned page = 0; page < FLASH_SECTOR_SIZE; page += FLASH_PAGE_SIZE) {
        const uint8_t *data = flash_at(sector_offsets[bank] + page);
        bool erased = true;
        for (unsigned byte = 0; byte < FLASH_PAGE_SIZE; ++byte) {
            if (data[byte] != 0xff) {
                erased = false;
                break;
            }
        }
        if (erased) {
            *offset = sector_offsets[bank] + page;
            return true;
        }
    }
    return false;
}

void storage_init(void) {}

probe_mode_t storage_get_active_mode(void) {
    uint32_t value = watchdog_hw->scratch[0];
    return IS_VALID_MODE_MAGIC(value) ? EXTRACT_MODE_FROM_MAGIC(value) : storage_get_default_mode();
}

void storage_set_active_mode(probe_mode_t mode) {
    if ((unsigned)mode >= MODE_COUNT) mode = MODE_CMSIS_DAP;
    watchdog_hw->scratch[0] = MAKE_MODE_MAGIC(mode);
}

probe_mode_t storage_get_default_mode(void) {
    unsigned bank = 0;
    const journal_record_t *latest = latest_record(&bank);
    if (latest) return (probe_mode_t)latest->mode;

    /* Old releases used an XOR-protected 16-byte record. Read it without
     * rewriting flash; the next explicit save creates the new journal format. */
    const persistent_config_t *legacy = (const persistent_config_t *)flash_at(SLOT_CONFIG_OFFSET);
    if (legacy->magic == CONFIG_SECTOR_MAGIC && legacy->default_mode < MODE_COUNT &&
        legacy->crc32 == (legacy->magic ^ legacy->default_mode ^ legacy->boot_count)) {
        return (probe_mode_t)legacy->default_mode;
    }
    return MODE_CMSIS_DAP;
}

bool storage_save_default_mode(probe_mode_t mode) {
    if ((unsigned)mode >= MODE_COUNT) return false;
    unsigned bank = 0;
    const journal_record_t *latest = latest_record(&bank);
    if (latest && latest->mode == (uint32_t)mode) {
        storage_set_active_mode(mode);
        return true; /* No wear when the requested default is already durable. */
    }

    journal_record_t record = {
        .magic = JOURNAL_MAGIC, .version = JOURNAL_VERSION,
        .sequence = latest ? latest->sequence + 1u : 0u, .mode = (uint32_t)mode,
    };
    record.crc32 = programmer_crc32(&record, offsetof(journal_record_t, crc32));
    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xff, sizeof(page));
    memcpy(page, &record, sizeof(record));

    uint32_t offset;
    bool erase = !find_erased_page(bank, &offset);
    if (erase) {
        /* Never erase the bank containing the latest committed record. If
         * there is only legacy data, preserve that bank during migration too. */
        bank ^= 1u;
        offset = sector_offsets[bank];
    }
    /* The supervisor is single-core; IRQ exclusion suffices here. Probe engines
     * must request a supervisor save, never call this while core 1 uses XIP. */
    uint32_t interrupts = save_and_disable_interrupts();
    if (erase) flash_range_erase(sector_offsets[bank], FLASH_SECTOR_SIZE);
    flash_range_program(offset, page, sizeof(page));
    restore_interrupts(interrupts);

    if (memcmp(flash_at(offset), page, sizeof(page)) != 0) return false;
    storage_set_active_mode(mode);
    return true;
}
