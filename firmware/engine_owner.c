#include "firmware/engine_owner.h"

#include <stddef.h>
#include <string.h>

#if !defined(PU_HOST_TEST) && (defined(__linux__) || defined(_WIN32) || defined(__APPLE__))
#define PU_HOST_TEST 1
#endif

#if !defined(PU_HOST_TEST)
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wundef"
#endif
#include "hardware/claim.h"
#include "hardware/sync.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static int s_spinlock_num = -1;

static spin_lock_t *engine_spinlock(void) {
    int lock_num = *(volatile const int *)&s_spinlock_num;
    if (__builtin_expect(lock_num >= 0, 1)) {
        __mem_fence_acquire();
        return spin_lock_instance((uint)lock_num);
    }

    /*
     * Lazily claim one exclusive hardware spinlock for the firmware lifetime.
     * spin_lock_claim_unused() and spin_lock_unclaim() internally acquire
     * hw_claim_lock(), so call them outside our hw_claim_lock() section to
     * prevent same-core spinlock deadlock while keeping local IRQs masked.
     */
    uint32_t irq_save = save_and_disable_interrupts();
    uint32_t claim_irq = hw_claim_lock();
    lock_num = s_spinlock_num;
    hw_claim_unlock(claim_irq);

    if (lock_num < 0) {
        int candidate = spin_lock_claim_unused(true);
        (void)spin_lock_init((uint)candidate);

        claim_irq = hw_claim_lock();
        if (s_spinlock_num < 0) {
            __mem_fence_release();
            *(volatile int *)&s_spinlock_num = candidate;
            __mem_fence_release();
            lock_num = candidate;
            candidate = -1;
        } else {
            lock_num = s_spinlock_num;
        }
        hw_claim_unlock(claim_irq);

        if (candidate >= 0) {
            spin_lock_unclaim((uint)candidate);
        }
    }

    restore_interrupts(irq_save);
    __mem_fence_acquire();
    return spin_lock_instance((uint)lock_num);
}
#endif

static pu_pin_hal_t s_hal;
static pu_engine_state_t s_state = PU_ENGINE_IDLE;
static uint64_t s_deadline_us = 0u;
static uint32_t s_fault_mask = 0u;
static uint32_t s_epoch = 0u;
static bool s_hal_running = false;
static bool s_teardown_pending = false;
static bool s_cut_vtarg_pending = false;
static bool s_target_idle_after_teardown = false;

static inline uint32_t engine_lock(void) {
#if !defined(PU_HOST_TEST)
    uint32_t saved_irq = save_and_disable_interrupts();
    spin_lock_unsafe_blocking(engine_spinlock());
    return saved_irq;
#else
    return 0u;
#endif
}

static inline void engine_unlock(uint32_t saved_irq) {
#if !defined(PU_HOST_TEST)
    spin_unlock(engine_spinlock(), saved_irq);
#else
    (void)saved_irq;
#endif
}

static bool is_acquirable_engine(pu_engine_state_t state) {
    switch (state) {
    case PU_ENGINE_SWD:
    case PU_ENGINE_JTAG:
    case PU_ENGINE_SWIO:
    case PU_ENGINE_RECOVERY:
        return true;
    case PU_ENGINE_IDLE:
    case PU_ENGINE_FAULT:
    default:
        return false;
    }
}

/*
 * Wrap-safe monotonic deadline comparison equivalent to two's-complement
 * (int64_t)(now_us - deadline_us) >= 0 without implementation-defined
 * unsigned-to-signed conversion when bit 63 is set.
 */
static bool is_deadline_expired(uint64_t now_us, uint64_t deadline_us) {
    return ((now_us - deadline_us) & (UINT64_C(1) << 63)) == 0u;
}

static void run_teardown_callbacks(const pu_pin_hal_t *hal, bool cut_vtarg_power) {
    if (hal->drain_pio != NULL) {
        hal->drain_pio();
    }
    if (hal->tristate_debug_pins != NULL) {
        hal->tristate_debug_pins();
    }
    if (hal->set_swio_pullup != NULL) {
        hal->set_swio_pullup(false);
    }
    if (cut_vtarg_power && hal->set_vtarg_power != NULL) {
        hal->set_vtarg_power(false);
    }
}

/*
 * Executes coalesced pending teardown passes as the sole active HAL runner.
 * Bounded to at most 2 passes (e.g., upgrading an in-flight non-fault teardown
 * to a VTARG-cutting fault teardown); once a fault teardown pass completes
 * while s_hal_running blocks all acquires, re-latching inside drain_pio() stops.
 */
static void finish_hal_runner_locked(uint32_t *irq_state) {
    bool completed_fault_teardown = false;

    for (unsigned pass = 0u; pass < 2u && s_teardown_pending; ++pass) {
        bool cut_vtarg = s_cut_vtarg_pending || (s_state == PU_ENGINE_FAULT);
        s_teardown_pending = false;
        s_cut_vtarg_pending = false;
        if (cut_vtarg && completed_fault_teardown) {
            break;
        }
        pu_pin_hal_t hal = s_hal;
        engine_unlock(*irq_state);

        run_teardown_callbacks(&hal, cut_vtarg);

        *irq_state = engine_lock();
        if (cut_vtarg) {
            completed_fault_teardown = true;
        }
    }

    s_teardown_pending = false;
    s_cut_vtarg_pending = false;
    if (s_target_idle_after_teardown && s_state != PU_ENGINE_FAULT) {
        s_state = PU_ENGINE_IDLE;
        s_deadline_us = 0u;
    }
    s_target_idle_after_teardown = false;
    s_hal_running = false;
}

static void schedule_or_run_idle_teardown_locked(uint32_t irq_state) {
    s_deadline_us = 0u;
    ++s_epoch;
    s_teardown_pending = true;
    s_target_idle_after_teardown = true;
    if (s_hal_running) {
        engine_unlock(irq_state);
        return;
    }

    s_hal_running = true;
    finish_hal_runner_locked(&irq_state);
    engine_unlock(irq_state);
}

void pu_engine_init(const pu_pin_hal_t *hal) {
    uint32_t irq_state = engine_lock();
    if (hal != NULL) {
        s_hal = *hal;
    } else {
        memset(&s_hal, 0, sizeof(s_hal));
    }
    s_state = PU_ENGINE_IDLE;
    s_deadline_us = 0u;
    s_fault_mask = 0u;
    ++s_epoch;
    s_hal_running = false;
    s_teardown_pending = false;
    s_cut_vtarg_pending = false;
    s_target_idle_after_teardown = false;
    engine_unlock(irq_state);
}

pu_status_t pu_engine_acquire(pu_engine_state_t target, uint64_t now_us, uint32_t timeout_us) {
    if (!is_acquirable_engine(target)) {
        return PU_ERR_INVALID_ARG;
    }

    uint32_t irq_state = engine_lock();
    if (s_state == PU_ENGINE_FAULT) {
        engine_unlock(irq_state);
        return PU_ERR_FAULT_LATCHED;
    }
    if (s_state != PU_ENGINE_IDLE || s_hal_running || s_teardown_pending) {
        engine_unlock(irq_state);
        return PU_ERR_ENGINE_BUSY;
    }

    s_state = target;
    s_deadline_us = now_us + (uint64_t)timeout_us;
    uint32_t acquire_epoch = ++s_epoch;
    pu_pin_hal_t hal = s_hal;
    bool enable_swio_pu = (target == PU_ENGINE_SWIO && hal.set_swio_pullup != NULL);
    if (!enable_swio_pu) {
        engine_unlock(irq_state);
        return PU_OK;
    }

    s_hal_running = true;
    engine_unlock(irq_state);

    hal.set_swio_pullup(true);

    irq_state = engine_lock();
    if (s_epoch != acquire_epoch && !s_teardown_pending) {
        s_teardown_pending = true;
        if (s_state == PU_ENGINE_FAULT) {
            s_cut_vtarg_pending = true;
        }
    }
    finish_hal_runner_locked(&irq_state);
    bool preempted = (s_epoch != acquire_epoch);
    bool faulted = (s_state == PU_ENGINE_FAULT);
    engine_unlock(irq_state);

    if (preempted) {
        return faulted ? PU_ERR_FAULT_LATCHED : PU_ERR_ENGINE_BUSY;
    }
    return PU_OK;
}

pu_status_t pu_engine_renew_deadline(pu_engine_state_t expected_owner,
                                     uint64_t now_us,
                                     uint32_t timeout_us) {
    if (!is_acquirable_engine(expected_owner)) {
        return PU_ERR_INVALID_ARG;
    }

    uint32_t irq_state = engine_lock();
    if (s_state == PU_ENGINE_FAULT) {
        engine_unlock(irq_state);
        return PU_ERR_FAULT_LATCHED;
    }
    if (s_state != expected_owner || s_target_idle_after_teardown) {
        engine_unlock(irq_state);
        return PU_ERR_NOT_OWNER;
    }

    s_deadline_us = now_us + (uint64_t)timeout_us;
    engine_unlock(irq_state);
    return PU_OK;
}

pu_status_t pu_engine_release(pu_engine_state_t expected_owner) {
    if (!is_acquirable_engine(expected_owner)) {
        return PU_ERR_INVALID_ARG;
    }

    uint32_t irq_state = engine_lock();
    if (s_state != expected_owner || s_target_idle_after_teardown) {
        engine_unlock(irq_state);
        return PU_ERR_NOT_OWNER;
    }

    schedule_or_run_idle_teardown_locked(irq_state);
    return PU_OK;
}

void pu_engine_tick(uint64_t now_us) {
    uint32_t irq_state = engine_lock();
    if (!is_acquirable_engine(s_state) || s_target_idle_after_teardown ||
        !is_deadline_expired(now_us, s_deadline_us)) {
        engine_unlock(irq_state);
        return;
    }

    schedule_or_run_idle_teardown_locked(irq_state);
}

void pu_engine_on_usb_reset(void) {
    uint32_t irq_state = engine_lock();
    if (!is_acquirable_engine(s_state) || s_target_idle_after_teardown) {
        engine_unlock(irq_state);
        return;
    }

    schedule_or_run_idle_teardown_locked(irq_state);
}

void pu_engine_latch_fault(uint32_t fault_mask) {
    uint32_t effective_mask = (fault_mask != 0u) ? fault_mask : PU_FAULT_UNSPECIFIED;

    uint32_t irq_state = engine_lock();
    s_state = PU_ENGINE_FAULT;
    s_deadline_us = 0u;
    s_fault_mask |= effective_mask;
    ++s_epoch;
    s_teardown_pending = true;
    s_cut_vtarg_pending = true;
    if (s_hal_running) {
        engine_unlock(irq_state);
        return;
    }

    s_hal_running = true;
    finish_hal_runner_locked(&irq_state);
    engine_unlock(irq_state);
}

uint32_t pu_engine_fault_mask(void) {
    uint32_t irq_state = engine_lock();
    uint32_t mask = s_fault_mask;
    engine_unlock(irq_state);
    return mask;
}

pu_status_t pu_engine_clear_fault(void) {
    uint32_t irq_state = engine_lock();
    if (s_hal_running || s_teardown_pending || is_acquirable_engine(s_state)) {
        engine_unlock(irq_state);
        return PU_ERR_ENGINE_BUSY;
    }

    s_fault_mask = 0u;
    s_state = PU_ENGINE_IDLE;
    s_deadline_us = 0u;
    ++s_epoch;
    engine_unlock(irq_state);
    return PU_OK;
}

pu_engine_state_t pu_engine_current(void) {
    uint32_t irq_state = engine_lock();
    pu_engine_state_t current = s_state;
    engine_unlock(irq_state);
    return current;
}
