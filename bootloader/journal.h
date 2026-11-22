#ifndef PU_BOOTLOADER_JOURNAL_H
#define PU_BOOTLOADER_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bootloader/image_validator.h"

#if !defined(PU_HOST_TEST) && (defined(__linux__) || defined(_WIN32) || defined(__APPLE__))
#define PU_HOST_TEST 1
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PU_SLOT_A                    (0u)
#define PU_SLOT_B                    (1u)
#define PU_SLOT_NONE                 (0xFFu)

#define PU_JOURNAL_MAX_BOOT_ATTEMPTS (3u)

#define PU_JOURNAL_FLAG_TRIAL        (1u << 0)
#define PU_JOURNAL_FLAG_ROLLED_BACK  (1u << 1)

typedef struct {
    uint8_t active_slot;
    uint8_t pending_slot;
    uint8_t boot_attempts;
    uint8_t flags;
} pu_journal_state_t;

typedef enum {
    PU_BOOT_ACTION_BOOT_SLOT = 0,
    PU_BOOT_ACTION_ENTER_RECOVERY = 1
} pu_boot_action_t;

typedef struct {
    pu_boot_action_t action;
    uint8_t selected_slot;
    bool journal_updated;
    pu_image_info_t image_info;
} pu_boot_decision_t;

bool pu_journal_load(pu_journal_state_t *out);
bool pu_journal_load_with_seq(pu_journal_state_t *out, uint64_t *seq_out);
bool pu_journal_commit(const pu_journal_state_t *next);

/* Deterministic A/B chainloader policy with 3-strike pending rollback. */
pu_boot_decision_t pu_bootloader_select_slot(bool recovery_button_pressed);

#ifdef PU_HOST_TEST
void pu_journal_bind_test_banks(uint8_t *bank_a, uint8_t *bank_b);
void pu_journal_inject_power_cut_after_bytes(size_t byte_limit);
#endif

#ifdef __cplusplus
}
#endif

#endif /* PU_BOOTLOADER_JOURNAL_H */
