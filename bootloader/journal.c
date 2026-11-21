#include "bootloader/journal.h"

#include <string.h>

#include "bootloader/crc32.h"
#include "config/flash_layout.h"

enum {
    PU_LAYOUT_FLASH_PAGE_SIZE = FLASH_PAGE_SIZE,
    PU_LAYOUT_FLASH_SECTOR_SIZE = FLASH_SECTOR_SIZE,
};

#ifndef PU_HOST_TEST
#undef FLASH_PAGE_SIZE
#undef FLASH_SECTOR_SIZE
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#endif
#include "hardware/flash.h"
#include "hardware/sync.h"
#if defined(__has_include)
#if __has_include("pico/flash.h")
#include "pico/flash.h"
#define PU_HAS_PICO_FLASH 1
#endif
#endif
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#ifndef PU_HAS_PICO_FLASH
#define PU_HAS_PICO_FLASH 0
#endif
_Static_assert(FLASH_PAGE_SIZE == PU_LAYOUT_FLASH_PAGE_SIZE, "SDK FLASH_PAGE_SIZE mismatch");
_Static_assert(FLASH_SECTOR_SIZE == PU_LAYOUT_FLASH_SECTOR_SIZE, "SDK FLASH_SECTOR_SIZE mismatch");
#endif

#ifdef PICO_FLASH_SIZE_BYTES
_Static_assert(PICO_FLASH_SIZE_BYTES >= (SLOT_JOURNAL_B_OFFSET + PU_LAYOUT_FLASH_SECTOR_SIZE), "PICO_FLASH_SIZE_BYTES must span both journal sectors");
#endif

/*
 * 32-byte on-flash journal record stored at the start of each 256-byte NOR page:
 *   [0..3]   magic          = 0x50554A4Eu ("PUJN")
 *   [4..5]   version        = 1u
 *   [6..7]   record_size    = 32u
 *   [8..15]  sequence       = monotonic uint64_t (>= 1, != UINT64_MAX)
 *   [16]     active_slot    = 0 or 1
 *   [17]     pending_slot   = 0, 1, or 0xFF
 *   [18]     boot_attempts  = 0..255
 *   [19]     flags          = bitmask
 *   [20..23] reserved       = 0
 *   [24..27] crc32          = IEEE CRC32 over bytes [0..23]
 *   [28..31] commit_trailer = 0x5A0F5A0Fu (contains zero 0xFF bytes)
 */
#define PU_JOURNAL_MAGIC          (0x50554A4Eu)
#define PU_JOURNAL_VERSION        (1u)
#define PU_JOURNAL_RECORD_SIZE    (32u)
#define PU_JOURNAL_CRC_BYTES      (24u)
#define PU_JOURNAL_COMMIT_TRAILER (0x5A0F5A0Fu)

#if defined(__cplusplus)
static_assert(SLOT_JOURNAL_A_SIZE == FLASH_SECTOR_SIZE, "journal_a must be one 4 KiB sector");
static_assert(SLOT_JOURNAL_B_SIZE == FLASH_SECTOR_SIZE, "journal_b must be one 4 KiB sector");
#else
_Static_assert(SLOT_JOURNAL_A_SIZE == FLASH_SECTOR_SIZE, "journal_a must be one 4 KiB sector");
_Static_assert(SLOT_JOURNAL_B_SIZE == FLASH_SECTOR_SIZE, "journal_b must be one 4 KiB sector");
#endif

typedef struct {
    uint64_t sequence;
    pu_journal_state_t state;
    unsigned bank;
} parsed_record_t;

#ifdef PU_HOST_TEST
static uint8_t *s_test_banks[2] = {NULL, NULL};
static size_t s_power_cut_limit = SIZE_MAX;

void pu_journal_bind_test_banks(uint8_t *bank_a, uint8_t *bank_b) {
    s_test_banks[0] = bank_a;
    s_test_banks[1] = bank_b;
    s_power_cut_limit = SIZE_MAX;
}

void pu_journal_inject_power_cut_after_bytes(size_t byte_limit) {
    s_power_cut_limit = byte_limit;
}
#else
static const uint32_t kJournalOffsets[2] = {
    SLOT_JOURNAL_A_OFFSET,
    SLOT_JOURNAL_B_OFFSET,
};

#if PU_HAS_PICO_FLASH
#define PU_FLASH_SAFE_TIMEOUT_MS (1000u)

typedef struct {
    uint32_t offset;
} flash_erase_ctx_t;

typedef struct {
    uint32_t offset;
    const uint8_t *page;
} flash_program_ctx_t;

static void flash_erase_bank_cb(void *param) {
    const flash_erase_ctx_t *ctx = (const flash_erase_ctx_t *)param;
    flash_range_erase(ctx->offset, FLASH_SECTOR_SIZE);
}

static void flash_program_page_cb(void *param) {
    const flash_program_ctx_t *ctx = (const flash_program_ctx_t *)param;
    flash_range_program(ctx->offset, ctx->page, FLASH_PAGE_SIZE);
}
#endif
#endif

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t read_u64_le(const uint8_t *p) {
    return (uint64_t)read_u32_le(p) | ((uint64_t)read_u32_le(p + 4u) << 32);
}

static void write_u16_le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void write_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void write_u64_le(uint8_t *p, uint64_t v) {
    write_u32_le(p, (uint32_t)(v & 0xFFFFFFFFu));
    write_u32_le(p + 4u, (uint32_t)(v >> 32));
}

static bool state_fields_valid(const pu_journal_state_t *st) {
    if (st == NULL) {
        return false;
    }
    if (st->active_slot > PU_SLOT_B) {
        return false;
    }
    if (st->pending_slot > PU_SLOT_B && st->pending_slot != PU_SLOT_NONE) {
        return false;
    }
    return true;
}

static const uint8_t *bank_read_ptr(unsigned bank) {
    if (bank >= 2u) {
        return NULL;
    }
#ifdef PU_HOST_TEST
    return s_test_banks[bank];
#else
    return (const uint8_t *)(uintptr_t)(FLASH_BASE_ADDR + kJournalOffsets[bank]);
#endif
}

static bool parse_record(const uint8_t *page, pu_journal_state_t *out_state, uint64_t *out_seq) {
    if (read_u32_le(page + 0u) != PU_JOURNAL_MAGIC) {
        return false;
    }
    if (read_u16_le(page + 4u) != PU_JOURNAL_VERSION) {
        return false;
    }
    if (read_u16_le(page + 6u) != PU_JOURNAL_RECORD_SIZE) {
        return false;
    }
    if (read_u32_le(page + 28u) != PU_JOURNAL_COMMIT_TRAILER) {
        return false;
    }
    if (read_u32_le(page + 20u) != 0u) {
        return false;
    }

    uint64_t seq = read_u64_le(page + 8u);
    if (seq == 0u || seq == UINT64_MAX) {
        return false;
    }

    pu_journal_state_t st = {
        .active_slot = page[16],
        .pending_slot = page[17],
        .boot_attempts = page[18],
        .flags = page[19],
    };
    if (!state_fields_valid(&st)) {
        return false;
    }

    uint32_t expected_crc = pu_crc32(page, PU_JOURNAL_CRC_BYTES);
    if (read_u32_le(page + 24u) != expected_crc) {
        return false;
    }

    *out_state = st;
    *out_seq = seq;
    return true;
}

static bool find_latest_record(parsed_record_t *out) {
    bool found = false;
    parsed_record_t best;
    memset(&best, 0, sizeof(best));

    for (unsigned bank = 0u; bank < 2u; ++bank) {
        const uint8_t *base = bank_read_ptr(bank);
        if (base == NULL) {
            return false;
        }
        for (uint32_t off = 0u; off < FLASH_SECTOR_SIZE; off += FLASH_PAGE_SIZE) {
            pu_journal_state_t st;
            uint64_t seq = 0u;
            if (parse_record(base + off, &st, &seq)) {
                if (!found || seq > best.sequence) {
                    best.sequence = seq;
                    best.state = st;
                    best.bank = bank;
                    found = true;
                }
            }
        }
    }

    if (found && out != NULL) {
        *out = best;
    }
    return found;
}

static bool find_erased_page_in_bank(unsigned bank, uint32_t *page_offset_out) {
    const uint8_t *base = bank_read_ptr(bank);
    if (base == NULL) {
        return false;
    }

    for (uint32_t off = 0u; off < FLASH_SECTOR_SIZE; off += FLASH_PAGE_SIZE) {
        bool all_ff = true;
        for (uint32_t i = 0u; i < FLASH_PAGE_SIZE; ++i) {
            if (base[off + i] != 0xFFu) {
                all_ff = false;
                break;
            }
        }
        if (all_ff) {
            *page_offset_out = off;
            return true;
        }
    }
    return false;
}

static bool bank_is_fully_erased(unsigned bank) {
    const uint8_t *base = bank_read_ptr(bank);
    if (base == NULL) {
        return false;
    }
    for (uint32_t i = 0u; i < FLASH_SECTOR_SIZE; ++i) {
        if (base[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

static bool flash_erase_bank(unsigned bank) {
    if (bank >= 2u) {
        return false;
    }
#ifdef PU_HOST_TEST
    uint8_t *dst = s_test_banks[bank];
    if (dst == NULL) {
        return false;
    }
    for (size_t i = 0u; i < FLASH_SECTOR_SIZE; ++i) {
        if (s_power_cut_limit == 0u) {
            return false;
        }
        if (s_power_cut_limit != SIZE_MAX) {
            --s_power_cut_limit;
        }
        dst[i] = 0xFFu;
    }
    return true;
#elif PU_HAS_PICO_FLASH
    flash_erase_ctx_t ctx = {.offset = kJournalOffsets[bank]};
    return flash_safe_execute(flash_erase_bank_cb, &ctx, PU_FLASH_SAFE_TIMEOUT_MS) == PICO_OK;
#else
    uint32_t irq_state = save_and_disable_interrupts();
    flash_range_erase(kJournalOffsets[bank], FLASH_SECTOR_SIZE);
    restore_interrupts(irq_state);
    return true;
#endif
}

static bool flash_program_page(unsigned bank, uint32_t page_offset, const uint8_t page[FLASH_PAGE_SIZE]) {
    if (bank >= 2u || page_offset >= FLASH_SECTOR_SIZE || (page_offset & (FLASH_PAGE_SIZE - 1u)) != 0u) {
        return false;
    }
#ifdef PU_HOST_TEST
    uint8_t *dst = s_test_banks[bank];
    if (dst == NULL) {
        return false;
    }
    for (size_t i = 0u; i < FLASH_PAGE_SIZE; ++i) {
        if (s_power_cut_limit == 0u) {
            return false;
        }
        if (s_power_cut_limit != SIZE_MAX) {
            --s_power_cut_limit;
        }
        /* NOR flash programming can only clear bits (1 -> 0). */
        dst[page_offset + i] &= page[i];
    }
    return true;
#elif PU_HAS_PICO_FLASH
    flash_program_ctx_t ctx = {
        .offset = kJournalOffsets[bank] + page_offset,
        .page = page,
    };
    return flash_safe_execute(flash_program_page_cb, &ctx, PU_FLASH_SAFE_TIMEOUT_MS) == PICO_OK;
#else
    uint32_t irq_state = save_and_disable_interrupts();
    flash_range_program(kJournalOffsets[bank] + page_offset, page, FLASH_PAGE_SIZE);
    restore_interrupts(irq_state);
    return true;
#endif
}

bool pu_journal_load_with_seq(pu_journal_state_t *out, uint64_t *seq_out) {
    if (out == NULL) {
        return false;
    }

    out->active_slot = PU_SLOT_A;
    out->pending_slot = PU_SLOT_NONE;
    out->boot_attempts = 0u;
    out->flags = 0u;
    if (seq_out != NULL) {
        *seq_out = 0u;
    }

    if (bank_read_ptr(0u) == NULL || bank_read_ptr(1u) == NULL) {
        return false;
    }

    parsed_record_t latest;
    if (!find_latest_record(&latest)) {
        return false;
    }

    *out = latest.state;
    if (seq_out != NULL) {
        *seq_out = latest.sequence;
    }
    return true;
}

bool pu_journal_load(pu_journal_state_t *out) {
    return pu_journal_load_with_seq(out, NULL);
}

bool pu_journal_commit(const pu_journal_state_t *next) {
    if (!state_fields_valid(next)) {
        return false;
    }
    if (bank_read_ptr(0u) == NULL || bank_read_ptr(1u) == NULL) {
        return false;
    }

    parsed_record_t latest;
    bool has_latest = find_latest_record(&latest);
    if (has_latest && latest.sequence >= UINT64_MAX - 1u) {
        return false;
    }

    uint64_t next_seq = has_latest ? (latest.sequence + 1u) : 1u;
    unsigned target_bank = 0u;
    uint32_t page_offset = 0u;
    bool need_erase = false;

    if (has_latest) {
        if (find_erased_page_in_bank(latest.bank, &page_offset)) {
            target_bank = latest.bank;
            need_erase = false;
        } else {
            /* Current bank is full; erase peer bank while keeping current bank intact. */
            target_bank = latest.bank ^ 1u;
            page_offset = 0u;
            need_erase = true;
        }
    } else {
        target_bank = 0u;
        if (find_erased_page_in_bank(0u, &page_offset)) {
            need_erase = false;
        } else {
            page_offset = 0u;
            need_erase = !bank_is_fully_erased(0u);
        }
    }

    uint8_t page[FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof(page));
    write_u32_le(page + 0u, PU_JOURNAL_MAGIC);
    write_u16_le(page + 4u, PU_JOURNAL_VERSION);
    write_u16_le(page + 6u, PU_JOURNAL_RECORD_SIZE);
    write_u64_le(page + 8u, next_seq);
    page[16] = next->active_slot;
    page[17] = next->pending_slot;
    page[18] = next->boot_attempts;
    page[19] = next->flags;
    write_u32_le(page + 20u, 0u);
    uint32_t crc = pu_crc32(page, PU_JOURNAL_CRC_BYTES);
    write_u32_le(page + 24u, crc);
    write_u32_le(page + 28u, PU_JOURNAL_COMMIT_TRAILER);

    if (need_erase && !flash_erase_bank(target_bank)) {
        return false;
    }
    if (!flash_program_page(target_bank, page_offset, page)) {
        return false;
    }

    const uint8_t *written = bank_read_ptr(target_bank) + page_offset;
    if (memcmp(written, page, sizeof(page)) != 0) {
        return false;
    }

    pu_journal_state_t verify_state;
    uint64_t verify_seq = 0u;
    return parse_record(written, &verify_state, &verify_seq) && verify_seq == next_seq;
}

static uint32_t slot_addr_for_id(uint8_t slot_id) {
    return (slot_id == PU_SLOT_A) ? SLOT_APP_A_ADDR : SLOT_APP_B_ADDR;
}

static uint32_t slot_size_for_id(uint8_t slot_id) {
    return (slot_id == PU_SLOT_A) ? SLOT_APP_A_SIZE : SLOT_APP_B_SIZE;
}

pu_boot_decision_t pu_bootloader_select_slot(bool recovery_button_pressed) {
    pu_boot_decision_t decision;
    memset(&decision, 0, sizeof(decision));
    decision.action = PU_BOOT_ACTION_ENTER_RECOVERY;
    decision.selected_slot = PU_SLOT_NONE;

    if (recovery_button_pressed) {
        return decision;
    }

    pu_journal_state_t state;
    bool had_journal = pu_journal_load(&state);

    bool is_trial = (state.flags & PU_JOURNAL_FLAG_TRIAL) != 0u ||
                    (state.pending_slot <= PU_SLOT_B && state.pending_slot != state.active_slot);

    if (is_trial) {
        uint8_t trial_slot = (state.pending_slot <= PU_SLOT_B) ? state.pending_slot : state.active_slot;
        uint8_t fallback_slot = (state.pending_slot <= PU_SLOT_B && state.pending_slot != state.active_slot)
                                    ? state.active_slot
                                    : (uint8_t)(trial_slot ^ 1u);

        pu_image_info_t trial_info;
        if (state.boot_attempts < PU_JOURNAL_MAX_BOOT_ATTEMPTS &&
            pu_image_validate_slot(slot_addr_for_id(trial_slot), slot_size_for_id(trial_slot), &trial_info)) {
            pu_journal_state_t trial_next = state;
            trial_next.pending_slot = trial_slot;
            trial_next.boot_attempts = (uint8_t)(state.boot_attempts + 1u);
            trial_next.flags |= PU_JOURNAL_FLAG_TRIAL;
            decision.journal_updated = pu_journal_commit(&trial_next);
            if (decision.journal_updated) {
                decision.image_info = trial_info;
                decision.action = PU_BOOT_ACTION_BOOT_SLOT;
                decision.selected_slot = trial_slot;
                return decision;
            }
        }

        /*
         * Trial exhausted (boot_attempts >= 3), pending image corrupt, or attempt commit failed:
         * validate fallback_slot BEFORE clearing trial state so an invalid fallback never
         * unlatches a 3-strike-rejected trial image across power cycles.
         */
        pu_image_info_t fallback_info;
        if (pu_image_validate_slot(slot_addr_for_id(fallback_slot), slot_size_for_id(fallback_slot), &fallback_info)) {
            pu_journal_state_t rb_state = state;
            rb_state.active_slot = fallback_slot;
            rb_state.pending_slot = PU_SLOT_NONE;
            rb_state.boot_attempts = 0u;
            rb_state.flags = (uint8_t)((state.flags & ~PU_JOURNAL_FLAG_TRIAL) | PU_JOURNAL_FLAG_ROLLED_BACK);
            decision.journal_updated = pu_journal_commit(&rb_state);
            decision.image_info = fallback_info;
            decision.action = PU_BOOT_ACTION_BOOT_SLOT;
            decision.selected_slot = fallback_slot;
            return decision;
        }

        if (state.boot_attempts >= PU_JOURNAL_MAX_BOOT_ATTEMPTS &&
            ((state.flags & PU_JOURNAL_FLAG_TRIAL) == 0u ||
             state.pending_slot != trial_slot ||
             state.boot_attempts != PU_JOURNAL_MAX_BOOT_ATTEMPTS)) {
            pu_journal_state_t latched = state;
            latched.pending_slot = trial_slot;
            latched.boot_attempts = PU_JOURNAL_MAX_BOOT_ATTEMPTS;
            latched.flags |= PU_JOURNAL_FLAG_TRIAL;
            decision.journal_updated = pu_journal_commit(&latched);
        }
        return decision;
    }

    /* Normal steady-state boot: validate active slot first, then fail-safe peer slot. */
    uint8_t primary = (state.active_slot <= PU_SLOT_B) ? state.active_slot : PU_SLOT_A;
    if (pu_image_validate_slot(slot_addr_for_id(primary), slot_size_for_id(primary), &decision.image_info)) {
        decision.action = PU_BOOT_ACTION_BOOT_SLOT;
        decision.selected_slot = primary;
        return decision;
    }

    uint8_t peer = (uint8_t)(primary ^ 1u);
    if (pu_image_validate_slot(slot_addr_for_id(peer), slot_size_for_id(peer), &decision.image_info)) {
        if (had_journal) {
            state.active_slot = peer;
            state.pending_slot = PU_SLOT_NONE;
            state.boot_attempts = 0u;
            state.flags |= PU_JOURNAL_FLAG_ROLLED_BACK;
            decision.journal_updated = pu_journal_commit(&state);
        }
        decision.action = PU_BOOT_ACTION_BOOT_SLOT;
        decision.selected_slot = peer;
        return decision;
    }

    return decision;
}
