/*
 * Host fault-injection & unit test suite for Task 2:
 *   - IEEE 802.3 CRC32 (bootloader/crc32.h)
 *   - Two-bank 4 KiB NOR flash journal across 4,353+ power-cut byte offsets
 *   - Slot image descriptor & Cortex-M0+ vector validation (bootloader/image_validator.h)
 *   - Fail-safe A/B boot attempt tracking & automatic 3-strike rollback
 */

#ifdef NDEBUG
#error "Assertions must remain enabled in host tests"
#endif

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config/flash_layout.h"
#include "bootloader/crc32.h"
#include "bootloader/image_format.h"
#include "bootloader/image_validator.h"
#include "bootloader/journal.h"

static uint8_t g_bank_a[FLASH_SECTOR_SIZE];
static uint8_t g_bank_b[FLASH_SECTOR_SIZE];
static uint8_t g_snap_a[FLASH_SECTOR_SIZE];
static uint8_t g_snap_b[FLASH_SECTOR_SIZE];
static uint8_t g_fake_flash[FLASH_TOTAL_SIZE];

static void reset_journal_banks(void) {
    memset(g_bank_a, 0xFF, sizeof(g_bank_a));
    memset(g_bank_b, 0xFF, sizeof(g_bank_b));
    pu_journal_bind_test_banks(g_bank_a, g_bank_b);
    pu_journal_inject_power_cut_after_bytes(SIZE_MAX);
}

static void reset_fake_flash(void) {
    memset(g_fake_flash, 0xFF, sizeof(g_fake_flash));
    pu_image_bind_test_flash(g_fake_flash, sizeof(g_fake_flash));
}

static void write_u32_le(uint8_t *dst, uint32_t val) {
    dst[0] = (uint8_t)(val & 0xFFu);
    dst[1] = (uint8_t)((val >> 8) & 0xFFu);
    dst[2] = (uint8_t)((val >> 16) & 0xFFu);
    dst[3] = (uint8_t)((val >> 24) & 0xFFu);
}

static void write_u16_le(uint8_t *dst, uint16_t val) {
    dst[0] = (uint8_t)(val & 0xFFu);
    dst[1] = (uint8_t)((val >> 8) & 0xFFu);
}

/* Construct a valid stamped slot image directly from wire format specifications. */
static void install_valid_slot_image(uint32_t slot_addr, uint32_t payload_size, uint32_t version) {
    assert(slot_addr >= FLASH_BASE_ADDR);
    uint32_t flash_off = slot_addr - FLASH_BASE_ADDR;
    assert(flash_off + PU_IMAGE_HEADER_SIZE + payload_size <= sizeof(g_fake_flash));
    assert(payload_size >= 0x100u);

    uint8_t *slot = &g_fake_flash[flash_off];
    memset(slot, 0xFF, PU_IMAGE_HEADER_SIZE);

    uint8_t *payload = slot + PU_IMAGE_HEADER_SIZE;
    for (uint32_t i = 0; i < payload_size; ++i) {
        payload[i] = (uint8_t)((i * 37u + version * 13u) & 0xFFu);
    }

    /* Cortex-M0+ vector table at slot_addr + 0x100 */
    uint32_t msp = 0x20042000u;
    uint32_t entry = slot_addr + PU_IMAGE_HEADER_SIZE + 0xC0u + 1u; /* Thumb bit set */
    write_u32_le(payload + 0, msp);
    write_u32_le(payload + 4, entry);
    /* Thumb 'bx lr' (0x4770) at entry & ~1 */
    write_u16_le(payload + 0xC0u, 0x4770u);

    uint32_t payload_crc = pu_crc32(payload, payload_size);

    pu_image_descriptor_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.magic = PU_IMAGE_DESC_MAGIC;
    desc.header_version = PU_IMAGE_DESC_VERSION;
    desc.image_version = version;
    desc.slot_addr = slot_addr;
    desc.image_size = payload_size;
    desc.payload_crc32 = payload_crc;
    desc.flags = 0u;
    desc.header_crc32 = pu_crc32(&desc, offsetof(pu_image_descriptor_t, header_crc32));

    memcpy(slot, &desc, sizeof(desc));
}

static void test_crc32_known_vectors(void) {
    assert(pu_crc32(NULL, 0) == 0x00000000u);
    assert(pu_crc32("", 0) == 0x00000000u);
    /* Canonical IEEE 802.3 check value for ASCII "123456789" */
    assert(pu_crc32("123456789", 9) == 0xCBF43926u);

    const uint8_t zeros[4] = {0, 0, 0, 0};
    assert(pu_crc32(zeros, sizeof(zeros)) == 0x2144DF1Cu);
}

static void test_journal_blank_and_invalid_inputs(void) {
    reset_journal_banks();

    assert(!pu_journal_load(NULL));
    assert(!pu_journal_commit(NULL));

    pu_journal_state_t out = {
        .active_slot = 99,
        .pending_slot = 99,
        .boot_attempts = 99,
        .flags = 99,
    };
    /* Blank 0xFF flash has no committed record: returns false and sets safe defaults. */
    assert(!pu_journal_load(&out));
    assert(out.active_slot == PU_SLOT_A);
    assert(out.pending_slot == PU_SLOT_NONE);
    assert(out.boot_attempts == 0u);
    assert(out.flags == 0u);

    /* Reject out-of-range slot identifiers. */
    pu_journal_state_t bad_active = {
        .active_slot = 2u,
        .pending_slot = PU_SLOT_NONE,
        .boot_attempts = 0u,
        .flags = 0u,
    };
    assert(!pu_journal_commit(&bad_active));

    pu_journal_state_t bad_pending = {
        .active_slot = PU_SLOT_A,
        .pending_slot = 2u,
        .boot_attempts = 0u,
        .flags = 0u,
    };
    assert(!pu_journal_commit(&bad_pending));
}

static void test_journal_multi_commit_bank_alternation_and_monotonic_seq(void) {
    reset_journal_banks();

    /*
     * 16 pages per 4 KiB sector -> 48 commits span:
     *   commits  1..16 : Bank A pages 0..15
     *   commits 17..32 : Erase Bank B + Bank B pages 0..15
     *   commits 33..48 : Erase Bank A + Bank A pages 0..15
     */
    for (uint64_t step = 1; step <= 48; ++step) {
        pu_journal_state_t desired = {
            .active_slot = (uint8_t)(step & 1u),
            .pending_slot = (step % 3u == 0u) ? PU_SLOT_NONE : (uint8_t)((step + 1u) & 1u),
            .boot_attempts = (uint8_t)(step % 4u),
            .flags = (uint8_t)(step & 0x0Fu),
        };
        assert(pu_journal_commit(&desired));

        pu_journal_state_t loaded = {0};
        uint64_t seq = 0;
        assert(pu_journal_load_with_seq(&loaded, &seq));
        assert(seq == step);
        assert(loaded.active_slot == desired.active_slot);
        assert(loaded.pending_slot == desired.pending_slot);
        assert(loaded.boot_attempts == desired.boot_attempts);
        assert(loaded.flags == desired.flags);
    }

    /* Corrupt the newest record (step 48 in Bank A page 15) and verify fallback to step 47. */
    g_bank_a[15u * FLASH_PAGE_SIZE + 16u] ^= 0x01u;
    pu_journal_state_t fallback = {0};
    uint64_t fallback_seq = 0;
    assert(pu_journal_load_with_seq(&fallback, &fallback_seq));
    assert(fallback_seq == 47u);
    assert(fallback.active_slot == (uint8_t)(47u & 1u));
}

static void test_journal_power_cut_every_erase_and_program_byte(void) {
    reset_journal_banks();

    /*
     * Populate 32 commits so:
     *   Bank A holds older valid records seq 1..16
     *   Bank B holds newer valid records seq 17..32 (full bank, seq 32 at page 15)
     * Commit 33 MUST erase Bank A (4,096 bytes) and program Bank A page 0 (256 bytes)
     * = 4,352 byte operations (4,353 cut points: 0..4352 inclusive).
     */
    pu_journal_state_t prev_state = {0};
    for (unsigned i = 1; i <= 32; ++i) {
        prev_state.active_slot = PU_SLOT_A;
        prev_state.pending_slot = PU_SLOT_NONE;
        prev_state.boot_attempts = (uint8_t)(i % 3u);
        prev_state.flags = 0x04u;
        assert(pu_journal_commit(&prev_state));
    }

    uint64_t baseline_seq = 0;
    pu_journal_state_t check = {0};
    assert(pu_journal_load_with_seq(&check, &baseline_seq));
    assert(baseline_seq == 32u);

    memcpy(g_snap_a, g_bank_a, sizeof(g_snap_a));
    memcpy(g_snap_b, g_bank_b, sizeof(g_snap_b));

    const pu_journal_state_t next_state = {
        .active_slot = PU_SLOT_B,
        .pending_slot = PU_SLOT_B,
        .boot_attempts = 1u,
        .flags = PU_JOURNAL_FLAG_TRIAL,
    };

    bool saw_old_state = false;
    bool saw_new_state = false;
    bool transitioned_to_new = false;

    const size_t total_points = FLASH_SECTOR_SIZE + FLASH_PAGE_SIZE; /* 4096 + 256 = 4352 */
    for (size_t cut = 0; cut <= total_points; ++cut) {
        memcpy(g_bank_a, g_snap_a, sizeof(g_bank_a));
        memcpy(g_bank_b, g_snap_b, sizeof(g_bank_b));

        pu_journal_inject_power_cut_after_bytes(cut);
        bool ok = pu_journal_commit(&next_state);
        pu_journal_inject_power_cut_after_bytes(SIZE_MAX);

        if (cut < total_points) {
            assert(!ok);
        } else {
            assert(ok);
        }

        /* Active bank (Bank B) holding seq 32 MUST never be modified during Bank A erase/write. */
        assert(memcmp(g_bank_b, g_snap_b, sizeof(g_bank_b)) == 0);

        pu_journal_state_t recovered = {0};
        uint64_t rec_seq = 0;
        assert(pu_journal_load_with_seq(&recovered, &rec_seq));

        if (rec_seq == 32u) {
            assert(!transitioned_to_new);
            assert(recovered.active_slot == prev_state.active_slot);
            assert(recovered.pending_slot == prev_state.pending_slot);
            assert(recovered.boot_attempts == prev_state.boot_attempts);
            assert(recovered.flags == prev_state.flags);
            saw_old_state = true;
        } else if (rec_seq == 33u) {
            transitioned_to_new = true;
            assert(recovered.active_slot == next_state.active_slot);
            assert(recovered.pending_slot == next_state.pending_slot);
            assert(recovered.boot_attempts == next_state.boot_attempts);
            assert(recovered.flags == next_state.flags);
            saw_new_state = true;
        } else {
            assert(!"Unexpected sequence recovered after power cut!");
        }

        /* Verify post-recovery commit always succeeds cleanly and advances monotonically. */
        pu_journal_state_t followup = {
            .active_slot = PU_SLOT_A,
            .pending_slot = PU_SLOT_NONE,
            .boot_attempts = 0u,
            .flags = 0u,
        };
        assert(pu_journal_commit(&followup));
        uint64_t post_seq = 0;
        pu_journal_state_t post_state = {0};
        assert(pu_journal_load_with_seq(&post_state, &post_seq));
        assert(post_seq == rec_seq + 1u);
        assert(post_state.active_slot == followup.active_slot);
    }

    assert(saw_old_state && saw_new_state);

    /* Also exercise cumulative repeated power cuts without restoring banks (dirty-page skipping). */
    reset_journal_banks();
    pu_journal_state_t s0 = {
        .active_slot = PU_SLOT_A,
        .pending_slot = PU_SLOT_NONE,
        .boot_attempts = 0u,
        .flags = 0u,
    };
    assert(pu_journal_commit(&s0));

    pu_journal_state_t s1 = {
        .active_slot = PU_SLOT_B,
        .pending_slot = PU_SLOT_B,
        .boot_attempts = 1u,
        .flags = PU_JOURNAL_FLAG_TRIAL,
    };
    uint64_t last_seq = 1u;
    for (size_t cut = 0; cut <= total_points; ++cut) {
        pu_journal_inject_power_cut_after_bytes(cut);
        (void)pu_journal_commit(&s1);
        pu_journal_inject_power_cut_after_bytes(SIZE_MAX);

        pu_journal_state_t rec = {0};
        uint64_t seq = 0;
        assert(pu_journal_load_with_seq(&rec, &seq));
        assert(seq >= last_seq);
        last_seq = seq;
        assert(rec.active_slot == PU_SLOT_A || rec.active_slot == PU_SLOT_B);
    }
    assert(last_seq > 1u);
}

static void test_image_validator_bounds_and_crc(void) {
    reset_fake_flash();

    install_valid_slot_image(SLOT_APP_A_ADDR, 1024u, 7u);
    pu_image_info_t info = {0};
    assert(pu_image_validate_slot(SLOT_APP_A_ADDR, SLOT_APP_A_SIZE, &info));
    assert(info.initial_msp == 0x20042000u);
    assert(info.entry_point == SLOT_APP_A_ADDR + PU_IMAGE_HEADER_SIZE + 0xC1u);
    assert(info.image_size == 1024u);
    assert(info.version == 7u);
    /* Null info pointer is permitted for validation-only checks. */
    assert(pu_image_validate_slot(SLOT_APP_A_ADDR, SLOT_APP_A_SIZE, NULL));

    uint8_t *slot_a = &g_fake_flash[SLOT_APP_A_OFFSET];

    /* 1. Corrupted payload byte -> CRC32 mismatch */
    slot_a[PU_IMAGE_HEADER_SIZE + 500u] ^= 0x80u;
    assert(!pu_image_validate_slot(SLOT_APP_A_ADDR, SLOT_APP_A_SIZE, &info));
    slot_a[PU_IMAGE_HEADER_SIZE + 500u] ^= 0x80u;
    assert(pu_image_validate_slot(SLOT_APP_A_ADDR, SLOT_APP_A_SIZE, &info));

    /* 2. Bad descriptor magic */
    slot_a[0] ^= 0x01u;
    assert(!pu_image_validate_slot(SLOT_APP_A_ADDR, SLOT_APP_A_SIZE, &info));
    slot_a[0] ^= 0x01u;

    /* 3. Slot A image copied verbatim into Slot B (descriptor slot_addr mismatch) */
    memcpy(&g_fake_flash[SLOT_APP_B_OFFSET], slot_a, PU_IMAGE_HEADER_SIZE + 1024u);
    assert(!pu_image_validate_slot(SLOT_APP_B_ADDR, SLOT_APP_B_SIZE, &info));

    /* 4. Out-of-range or misaligned MSP values */
    const uint32_t bad_msps[] = {
        0x20000000u, /* Exact SRAM base: zero stack space */
        0x1FFFFFF8u, /* Below SRAM */
        0x20042008u, /* Above 264 KiB RP2040 SRAM top */
        0x20020004u, /* Not 8-byte AAPCS aligned */
        0xFFFFFFFFu, /* Erased */
    };
    for (size_t i = 0; i < sizeof(bad_msps) / sizeof(bad_msps[0]); ++i) {
        install_valid_slot_image(SLOT_APP_A_ADDR, 1024u, 1u);
        write_u32_le(slot_a + PU_IMAGE_HEADER_SIZE + 0u, bad_msps[i]);
        /* Re-seal CRCs so only MSP check triggers rejection */
        pu_image_descriptor_t desc;
        memcpy(&desc, slot_a, sizeof(desc));
        desc.payload_crc32 = pu_crc32(slot_a + PU_IMAGE_HEADER_SIZE, 1024u);
        desc.header_crc32 = pu_crc32(&desc, offsetof(pu_image_descriptor_t, header_crc32));
        memcpy(slot_a, &desc, sizeof(desc));
        assert(!pu_image_validate_slot(SLOT_APP_A_ADDR, SLOT_APP_A_SIZE, &info));
    }

    /* 5. Non-Thumb reset vector, out-of-slot entry, and erased instruction at entry */
    const uint32_t bad_entries[] = {
        SLOT_APP_A_ADDR + PU_IMAGE_HEADER_SIZE + 0xC0u,       /* Bit 0 == 0 (ARM mode, invalid on M0+) */
        SLOT_APP_A_ADDR + 0x001u,                             /* Inside descriptor header */
        SLOT_APP_A_ADDR + PU_IMAGE_HEADER_SIZE + 0x005u,      /* Inside initial MSP/Reset vectors */
        SLOT_APP_A_ADDR + PU_IMAGE_HEADER_SIZE + 1024u + 1u,  /* Past end of image_size */
        SLOT_APP_B_ADDR + PU_IMAGE_HEADER_SIZE + 0xC1u,       /* Points into peer slot */
    };
    for (size_t i = 0; i < sizeof(bad_entries) / sizeof(bad_entries[0]); ++i) {
        install_valid_slot_image(SLOT_APP_A_ADDR, 1024u, 1u);
        write_u32_le(slot_a + PU_IMAGE_HEADER_SIZE + 4u, bad_entries[i]);
        pu_image_descriptor_t desc;
        memcpy(&desc, slot_a, sizeof(desc));
        desc.payload_crc32 = pu_crc32(slot_a + PU_IMAGE_HEADER_SIZE, 1024u);
        desc.header_crc32 = pu_crc32(&desc, offsetof(pu_image_descriptor_t, header_crc32));
        memcpy(slot_a, &desc, sizeof(desc));
        assert(!pu_image_validate_slot(SLOT_APP_A_ADDR, SLOT_APP_A_SIZE, &info));
    }

    /* Entry pointing to erased 0xFFFF instruction */
    install_valid_slot_image(SLOT_APP_A_ADDR, 1024u, 1u);
    write_u16_le(slot_a + PU_IMAGE_HEADER_SIZE + 0xC0u, 0xFFFFu);
    {
        pu_image_descriptor_t desc;
        memcpy(&desc, slot_a, sizeof(desc));
        desc.payload_crc32 = pu_crc32(slot_a + PU_IMAGE_HEADER_SIZE, 1024u);
        desc.header_crc32 = pu_crc32(&desc, offsetof(pu_image_descriptor_t, header_crc32));
        memcpy(slot_a, &desc, sizeof(desc));
        assert(!pu_image_validate_slot(SLOT_APP_A_ADDR, SLOT_APP_A_SIZE, &info));
    }

    /* 6. Truncated and oversized descriptor image_size + invalid slot bounds */
    const uint32_t bad_sizes[] = {
        0u,
        8u,
        513u,                                    /* Odd length */
        SLOT_APP_A_SIZE - PU_IMAGE_HEADER_SIZE + 2u, /* Exceeds slot capacity */
        SLOT_APP_A_SIZE,
        0xFFFFFFFFu,
    };
    for (size_t i = 0; i < sizeof(bad_sizes) / sizeof(bad_sizes[0]); ++i) {
        install_valid_slot_image(SLOT_APP_A_ADDR, 1024u, 1u);
        pu_image_descriptor_t desc;
        memcpy(&desc, slot_a, sizeof(desc));
        desc.image_size = bad_sizes[i];
        desc.header_crc32 = pu_crc32(&desc, offsetof(pu_image_descriptor_t, header_crc32));
        memcpy(slot_a, &desc, sizeof(desc));
        assert(!pu_image_validate_slot(SLOT_APP_A_ADDR, SLOT_APP_A_SIZE, &info));
    }

    install_valid_slot_image(SLOT_APP_A_ADDR, 1024u, 1u);
    assert(!pu_image_validate_slot(SLOT_APP_A_ADDR + 4u, SLOT_APP_A_SIZE, &info));
    assert(!pu_image_validate_slot(0x00000000u, SLOT_APP_A_SIZE, &info));
    assert(!pu_image_validate_slot(SLOT_APP_A_ADDR, UINT32_MAX, &info));
    assert(!pu_image_validate_slot(FLASH_BASE_ADDR + FLASH_TOTAL_SIZE, 4096u, &info));
    assert(!pu_image_validate_slot(SLOT_APP_A_ADDR, 128u, &info));
}

static void test_ab_bootloader_rollback_after_three_attempts(void) {
    reset_journal_banks();
    reset_fake_flash();

    install_valid_slot_image(SLOT_APP_A_ADDR, 1024u, 10u);
    install_valid_slot_image(SLOT_APP_B_ADDR, 1024u, 20u);

    /* 1. Recovery button held -> enter BOOTSEL recovery without modifying journal. */
    pu_boot_decision_t rec_btn = pu_bootloader_select_slot(true);
    assert(rec_btn.action == PU_BOOT_ACTION_ENTER_RECOVERY);

    /* 2. Fresh blank journal defaults to Slot A cleanly. */
    pu_boot_decision_t first_boot = pu_bootloader_select_slot(false);
    assert(first_boot.action == PU_BOOT_ACTION_BOOT_SLOT);
    assert(first_boot.selected_slot == PU_SLOT_A);
    assert(first_boot.image_info.version == 10u);

    /* 3. Stage pending OTA update on Slot B while Slot A is confirmed active. */
    pu_journal_state_t staged = {
        .active_slot = PU_SLOT_A,
        .pending_slot = PU_SLOT_B,
        .boot_attempts = 0u,
        .flags = PU_JOURNAL_FLAG_TRIAL,
    };
    assert(pu_journal_commit(&staged));

    /* Attempts 1, 2, 3 boot pending Slot B and increment boot_attempts 1 -> 2 -> 3. */
    for (uint8_t attempt = 1u; attempt <= PU_JOURNAL_MAX_BOOT_ATTEMPTS; ++attempt) {
        pu_boot_decision_t d = pu_bootloader_select_slot(false);
        assert(d.action == PU_BOOT_ACTION_BOOT_SLOT);
        assert(d.selected_slot == PU_SLOT_B);
        assert(d.image_info.version == 20u);

        pu_journal_state_t cur = {0};
        assert(pu_journal_load(&cur));
        assert(cur.boot_attempts == attempt);
    }

    /* 4th boot without firmware health confirmation: boot_attempts == 3 -> ROLLBACK to Slot A! */
    pu_boot_decision_t rolled_back = pu_bootloader_select_slot(false);
    assert(rolled_back.action == PU_BOOT_ACTION_BOOT_SLOT);
    assert(rolled_back.selected_slot == PU_SLOT_A);
    assert(rolled_back.image_info.version == 10u);

    pu_journal_state_t post_rb = {0};
    uint64_t rb_seq = 0;
    assert(pu_journal_load_with_seq(&post_rb, &rb_seq));
    assert(post_rb.active_slot == PU_SLOT_A);
    assert(post_rb.pending_slot == PU_SLOT_NONE);
    assert(post_rb.boot_attempts == 0u);
    assert((post_rb.flags & PU_JOURNAL_FLAG_ROLLED_BACK) != 0u);
    assert((post_rb.flags & PU_JOURNAL_FLAG_TRIAL) == 0u);

    /* Subsequent steady-state boot stays on Slot A without extra journal writes. */
    pu_boot_decision_t steady = pu_bootloader_select_slot(false);
    uint64_t steady_seq = 0;
    assert(steady.action == PU_BOOT_ACTION_BOOT_SLOT);
    assert(steady.selected_slot == PU_SLOT_A);
    assert(pu_journal_load_with_seq(&post_rb, &steady_seq));
    assert(steady_seq == rb_seq);

    /* 4. Pending slot with corrupt CRC rolls back immediately on first attempt. */
    g_fake_flash[SLOT_APP_B_OFFSET + PU_IMAGE_HEADER_SIZE + 64u] ^= 0xFFu;
    assert(pu_journal_commit(&staged));
    pu_boot_decision_t instant_rb = pu_bootloader_select_slot(false);
    assert(instant_rb.action == PU_BOOT_ACTION_BOOT_SLOT);
    assert(instant_rb.selected_slot == PU_SLOT_A);

    /* 5. Both slots corrupt -> enters recovery instead of branching to bad code. */
    g_fake_flash[SLOT_APP_A_OFFSET + PU_IMAGE_HEADER_SIZE + 64u] ^= 0xFFu;
    pu_boot_decision_t both_bad = pu_bootloader_select_slot(false);
    assert(both_bad.action == PU_BOOT_ACTION_ENTER_RECOVERY);
}

static void test_ab_three_strike_invalid_fallback_stays_in_recovery_across_power_cycles(void) {
    reset_journal_banks();
    reset_fake_flash();

    /* Fallback Slot A is blank/invalid; trial Slot B has a valid descriptor/CRC image. */
    install_valid_slot_image(SLOT_APP_B_ADDR, 1024u, 20u);

    pu_journal_state_t staged = {
        .active_slot = PU_SLOT_A,
        .pending_slot = PU_SLOT_B,
        .boot_attempts = 0u,
        .flags = PU_JOURNAL_FLAG_TRIAL,
    };
    assert(pu_journal_commit(&staged));

    /* 3 counted trial boots of Slot B that crash before confirming health. */
    for (uint8_t attempt = 1u; attempt <= PU_JOURNAL_MAX_BOOT_ATTEMPTS; ++attempt) {
        pu_boot_decision_t d = pu_bootloader_select_slot(false);
        assert(d.action == PU_BOOT_ACTION_BOOT_SLOT);
        assert(d.selected_slot == PU_SLOT_B);
        assert(d.journal_updated);
    }

    uint64_t seq_after_strikes = 0u;
    pu_journal_state_t state_after_strikes = {0};
    assert(pu_journal_load_with_seq(&state_after_strikes, &seq_after_strikes));
    assert(state_after_strikes.boot_attempts == PU_JOURNAL_MAX_BOOT_ATTEMPTS);

    /*
     * 4th and subsequent power cycles: fallback Slot A is invalid, so bootloader MUST
     * stay in recovery with PU_JOURNAL_FLAG_TRIAL latched and NEVER promote/boot Slot B.
     */
    for (unsigned cycle = 0u; cycle < 6u; ++cycle) {
        pu_boot_decision_t rec = pu_bootloader_select_slot(false);
        assert(rec.action == PU_BOOT_ACTION_ENTER_RECOVERY);
        assert(rec.selected_slot == PU_SLOT_NONE);

        pu_journal_state_t latched = {0};
        uint64_t cur_seq = 0u;
        assert(pu_journal_load_with_seq(&latched, &cur_seq));
        assert((latched.flags & PU_JOURNAL_FLAG_TRIAL) != 0u);
        assert(latched.pending_slot == PU_SLOT_B);
        assert(latched.boot_attempts == PU_JOURNAL_MAX_BOOT_ATTEMPTS);
        assert(cur_seq == seq_after_strikes);
    }

    /* Also verify latching when trial was entered via pending_slot != active_slot without flag bit. */
    reset_journal_banks();
    pu_journal_state_t unflagged_exhausted = {
        .active_slot = PU_SLOT_A,
        .pending_slot = PU_SLOT_B,
        .boot_attempts = PU_JOURNAL_MAX_BOOT_ATTEMPTS,
        .flags = 0u,
    };
    assert(pu_journal_commit(&unflagged_exhausted));

    for (unsigned cycle = 0u; cycle < 4u; ++cycle) {
        pu_boot_decision_t rec = pu_bootloader_select_slot(false);
        assert(rec.action == PU_BOOT_ACTION_ENTER_RECOVERY);
        assert(rec.selected_slot == PU_SLOT_NONE);

        pu_journal_state_t latched = {0};
        assert(pu_journal_load(&latched));
        assert((latched.flags & PU_JOURNAL_FLAG_TRIAL) != 0u);
        assert(latched.pending_slot == PU_SLOT_B);
        assert(latched.boot_attempts == PU_JOURNAL_MAX_BOOT_ATTEMPTS);
    }
}

static void test_ab_trial_boot_commit_failure_refuses_uncounted_boot(void) {
    /* Case A: Fallback Slot A invalid -> commit failure during trial boot enters recovery. */
    reset_journal_banks();
    reset_fake_flash();
    install_valid_slot_image(SLOT_APP_B_ADDR, 1024u, 20u);

    pu_journal_state_t staged = {
        .active_slot = PU_SLOT_A,
        .pending_slot = PU_SLOT_B,
        .boot_attempts = 0u,
        .flags = PU_JOURNAL_FLAG_TRIAL,
    };
    assert(pu_journal_commit(&staged));

    for (unsigned attempt = 0u; attempt < 5u; ++attempt) {
        pu_journal_inject_power_cut_after_bytes(0u);
        pu_boot_decision_t d = pu_bootloader_select_slot(false);
        pu_journal_inject_power_cut_after_bytes(SIZE_MAX);

        assert(!d.journal_updated);
        assert(d.action == PU_BOOT_ACTION_ENTER_RECOVERY);
        assert(d.selected_slot == PU_SLOT_NONE);
    }

    /* Once flash commit succeeds again, attempt 1 is properly counted and allowed to boot. */
    pu_boot_decision_t counted = pu_bootloader_select_slot(false);
    assert(counted.journal_updated);
    assert(counted.action == PU_BOOT_ACTION_BOOT_SLOT);
    assert(counted.selected_slot == PU_SLOT_B);

    /* Case B: Fallback Slot A valid -> commit failure during trial boot falls back to Slot A, never Slot B. */
    reset_journal_banks();
    reset_fake_flash();
    install_valid_slot_image(SLOT_APP_A_ADDR, 1024u, 10u);
    install_valid_slot_image(SLOT_APP_B_ADDR, 1024u, 20u);
    assert(pu_journal_commit(&staged));

    /* Allow only 64 bytes so the trial-increment page write tears/fails, then rollback runs. */
    pu_journal_inject_power_cut_after_bytes(64u);
    pu_boot_decision_t fb = pu_bootloader_select_slot(false);
    pu_journal_inject_power_cut_after_bytes(SIZE_MAX);

    assert(fb.selected_slot != PU_SLOT_B);
    assert(fb.action == PU_BOOT_ACTION_BOOT_SLOT);
    assert(fb.selected_slot == PU_SLOT_A);
    assert(fb.image_info.version == 10u);
}

int main(void) {
    test_crc32_known_vectors();
    test_journal_blank_and_invalid_inputs();
    test_journal_multi_commit_bank_alternation_and_monotonic_seq();
    test_journal_power_cut_every_erase_and_program_byte();
    test_image_validator_bounds_and_crc();
    test_ab_bootloader_rollback_after_three_attempts();
    test_ab_three_strike_invalid_fallback_stays_in_recovery_across_power_cycles();
    test_ab_trial_boot_commit_failure_refuses_uncounted_boot();
    puts("PASS: test_journal_powercut (CRC32, 4,353-offset power-cut journal, validator, A/B rollback)");
    return 0;
}
