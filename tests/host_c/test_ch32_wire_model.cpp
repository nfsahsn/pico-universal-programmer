/*
 * Host unit & wire-model test suite for Task 5:
 *   Hardened CH32V003 PIO1 SWIO Engine, PUID1 Non-Destructive Identity
 *   & Cross-Page SoftBreak (firmware/swio_engine.h)
 */

#ifdef NDEBUG
#error "Assertions must remain enabled in host tests"
#endif

#ifndef PU_HOST_TEST
#define PU_HOST_TEST 1
#endif

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "config/board_pinout.h"
#include "firmware/engine_owner.h"
#include "firmware/swio_engine.h"

namespace {

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string extract_gdb_payload(const uint8_t *buf, size_t n) {
    const std::string wire(reinterpret_cast<const char *>(buf), n);
    size_t dollar = wire.find('$');
    assert(dollar != std::string::npos);
    size_t hash = wire.find('#', dollar + 1u);
    assert(hash != std::string::npos && hash + 2u < wire.size());

    uint8_t csum = 0u;
    std::string unescaped;
    for (size_t i = dollar + 1u; i < hash; ++i) {
        csum = static_cast<uint8_t>(csum + static_cast<uint8_t>(wire[i]));
        if (wire[i] == '}' && i + 1u < hash) {
            ++i;
            csum = static_cast<uint8_t>(csum + static_cast<uint8_t>(wire[i]));
            unescaped.push_back(static_cast<char>(wire[i] ^ 0x20));
        } else {
            unescaped.push_back(wire[i]);
        }
    }
    int hi = hex_nibble(wire[hash + 1u]);
    int lo = hex_nibble(wire[hash + 2u]);
    assert(hi >= 0 && lo >= 0);
    assert(static_cast<uint8_t>((hi << 4) | lo) == csum);
    return unescaped;
}

std::string decode_hex_ascii(const std::string &hex) {
    assert((hex.size() % 2u) == 0u);
    std::string out;
    out.reserve(hex.size() / 2u);
    for (size_t i = 0u; i < hex.size(); i += 2u) {
        int hi = hex_nibble(hex[i]);
        int lo = hex_nibble(hex[i + 1u]);
        assert(hi >= 0 && lo >= 0);
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    return out;
}

/* Custom wire HAL for testing reserved/contradictory DMSTATUS & wire timeouts */
uint32_t g_custom_dmstatus = 0u;
bool g_custom_wire_timeout = false;

bool custom_read_reg(uint8_t reg_addr, uint32_t *out_val, uint32_t /*timeout_us*/) {
    if (g_custom_wire_timeout) {
        return false;
    }
    if (reg_addr == 0x11u /* DMSTATUS */) {
        *out_val = g_custom_dmstatus;
        return true;
    }
    *out_val = 0u;
    return true;
}

bool custom_write_reg(uint8_t /*reg_addr*/, uint32_t /*val*/, uint32_t /*timeout_us*/) {
    return !g_custom_wire_timeout;
}

void test_ch32_silent_bus_never_misclassified() {
    uint8_t out[256] = {};

    /* 1. Silent-high (0xFFFFFFFF): floating pull-up / unplugged target */
    pu_swio_set_fake_wire_mode(PU_WIRE_SILENT_HIGH_FF);
    pu_ch32_check_result_t r_hi = pu_ch32_non_destructive_check(1000u);
    assert(r_hi.status == PU_CH32_ERR_NO_REPLY);
    assert(r_hi.flash_writes == 0u);
    assert(r_hi.diagnosis_mask == PU_CH32_DIAG_ALL_FIVE_CAUSES);
    assert(r_hi.resumed_running == false);
    assert(r_hi.outen_disabled_verified == false);
    assert(pu_engine_current() == PU_ENGINE_IDLE);

    /* GDB memory read & flash erase on 0xFFFFFFFF bus must NEVER claim blank flash or OK */
    assert(pu_ch32_gdb_send_checked("$m08000000,10#", out, sizeof(out), 1100u) != PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out)))[0] == 'E');
    assert(pu_ch32_gdb_send_checked("$vFlashErase:08000000,40#", out, sizeof(out), 1200u) != PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out)))[0] == 'E');
    pu_ch32_on_disconnect();
    assert(pu_swio_fake_flash_matches_original() == true);

    /* 2. Silent-low (0x00000000): shorted-to-GND / unpowered clamping */
    pu_swio_set_fake_wire_mode(PU_WIRE_SILENT_LOW_00);
    pu_ch32_check_result_t r_lo = pu_ch32_non_destructive_check(2000u);
    assert(r_lo.status == PU_CH32_ERR_NO_REPLY);
    assert(r_lo.flash_writes == 0u);
    assert(r_lo.diagnosis_mask == PU_CH32_DIAG_ALL_FIVE_CAUSES);
    assert(r_lo.resumed_running == false);
    assert(r_lo.outen_disabled_verified == false);
    assert(pu_engine_current() == PU_ENGINE_IDLE);

    assert(pu_ch32_gdb_send_checked("$m08000000,10#", out, sizeof(out), 2100u) != PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out)))[0] == 'E');
    assert(pu_ch32_gdb_send_checked("$vFlashErase:08000000,40#", out, sizeof(out), 2200u) != PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out)))[0] == 'E');
    pu_ch32_on_disconnect();
    assert(pu_swio_fake_flash_matches_original() == true);

    /* 3. DMSTATUS with non-zero reserved bits or contradictory ANYHALTED|ALLRUNNING */
    pu_swio_wire_hal_t custom_hal = {};
    custom_hal.sys_clock_hz = 125000000u;
    custom_hal.read_reg = custom_read_reg;
    custom_hal.write_reg = custom_write_reg;
    pu_swio_init(&custom_hal);

    /* Non-zero reserved bit [20] on otherwise halted-looking DMSTATUS */
    g_custom_wire_timeout = false;
    g_custom_dmstatus = (1u << 20) | (1u << 9) | (1u << 8) | (1u << 7) | 0x2u;
    pu_ch32_check_result_t r_resv = pu_ch32_non_destructive_check(2300u);
    assert(r_resv.status == PU_CH32_ERR_NO_REPLY);
    assert(r_resv.diagnosis_mask == PU_CH32_DIAG_ALL_FIVE_CAUSES);
    assert(r_resv.flash_writes == 0u);

    /* Contradictory ANYHALTED && ALLRUNNING */
    g_custom_dmstatus = (1u << 11) | (1u << 10) | (1u << 9) | (1u << 8) | (1u << 7) | 0x2u;
    pu_ch32_check_result_t r_contra = pu_ch32_non_destructive_check(2400u);
    assert(r_contra.status == PU_CH32_ERR_NO_REPLY);
    assert(r_contra.diagnosis_mask == PU_CH32_DIAG_ALL_FIVE_CAUSES);
    assert(r_contra.flash_writes == 0u);

    /* Bounded wire timeout on unresponsive bus */
    g_custom_wire_timeout = true;
    pu_ch32_check_result_t r_tout = pu_ch32_non_destructive_check(2500u);
    assert(r_tout.status == PU_CH32_ERR_NO_REPLY);
    assert(r_tout.diagnosis_mask == PU_CH32_DIAG_ALL_FIVE_CAUSES);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
}

void test_ch32_already_halted_rejected() {
    pu_swio_set_fake_wire_mode(PU_WIRE_ALREADY_HALTED);
    pu_ch32_check_result_t r = pu_ch32_non_destructive_check(2800u);
    assert(r.status == PU_CH32_ERR_ALREADY_HALTED);
    assert(r.flash_writes == 0u);
    assert(r.resumed_running == false);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(pu_swio_fake_flash_matches_original() == true);
}

void test_ch32_check_preserves_running_sketch_and_resets_outen() {
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    pu_ch32_check_result_t r = pu_ch32_non_destructive_check(3000u);
    assert(r.status == PU_CH32_OK);
    assert(r.chip_id != 0u && r.chip_id != 0xFFFFFFFFu);
    assert(r.flash_kb == 16u);
    assert(r.flash_writes == 0u);
    assert(r.diagnosis_mask == 0u);
    assert(r.resumed_running == true);
    assert(r.outen_disabled_verified == true);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(pu_swio_fake_flash_matches_original() == true);

    /* Monitor command qRcmd,"puid1" over CDC stream */
    uint8_t out[256] = {};
    assert(pu_ch32_gdb_send_checked("$qRcmd,7075696431#", out, sizeof(out), 3200u) == PU_OK);
    std::string hex_reply = extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out)));
    std::string text = decode_hex_ascii(hex_reply);
    assert(text.find("PUID1:OK") == 0u);
    assert(text.find("FLASH_KB=16") != std::string::npos);
    assert(text.find("FLASH_WRITES=0") != std::string::npos);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
}

void test_ch32_outen_reset_and_resume_failures() {
    /* Failed WCH_DM_SHDWCFGR.OUTEN == 0 verification MUST return PU_CH32_ERR_DETACH_FAILED */
    pu_swio_set_fake_wire_mode(PU_WIRE_OUTEN_RESET_FAIL);
    pu_ch32_check_result_t r_outen = pu_ch32_non_destructive_check(3500u);
    assert(r_outen.status == PU_CH32_ERR_DETACH_FAILED);
    assert(r_outen.flash_writes == 0u);
    assert(r_outen.outen_disabled_verified == false);
    assert(pu_engine_current() == PU_ENGINE_IDLE);

    /* Failed post-resume running verification MUST fail */
    pu_swio_set_fake_wire_mode(PU_WIRE_RESUME_FAIL);
    pu_ch32_check_result_t r_res = pu_ch32_non_destructive_check(3700u);
    assert(r_res.status == PU_CH32_ERR_DETACH_FAILED);
    assert(r_res.flash_writes == 0u);
    assert(r_res.resumed_running == false);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
}

void test_ch32_softbreak_cross_page_and_disconnect_restore() {
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    uint8_t out[256] = {};

    /* 1. Cross-page 4-byte software breakpoint at 0x0800003E (bytes 62..65 -> Pages 0 & 1) */
    const char *ins = "$Z0,0800003e,4#";
    assert(pu_ch32_gdb_send_checked(ins, out, sizeof(out), 4000u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "OK");
    assert(pu_swio_fake_flash_matches_original() == false);

    /* Duplicate insert is idempotent */
    assert(pu_ch32_gdb_send_checked(ins, out, sizeof(out), 4050u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "OK");

    /* Overlapping breakpoints (second halfword in next page, preceding word, mismatched size) rejected */
    assert(pu_ch32_gdb_send_checked("$Z0,08000040,2#", out, sizeof(out), 4100u) != PU_OK);
    assert(pu_ch32_gdb_send_checked("$Z0,0800003c,4#", out, sizeof(out), 4110u) != PU_OK);
    assert(pu_ch32_gdb_send_checked("$Z0,0800003e,2#", out, sizeof(out), 4120u) != PU_OK);

    /* 2. Final 2-byte compressed instruction at 0x08003FFE valid; 4-byte at 0x08003FFE & past-end rejected */
    assert(pu_ch32_gdb_send_checked("$Z0,08003ffe,2#", out, sizeof(out), 4200u) == PU_OK);
    assert(pu_ch32_gdb_send_checked("$Z0,08003ffe,4#", out, sizeof(out), 4210u) != PU_OK);
    assert(pu_ch32_gdb_send_checked("$Z0,08004000,2#", out, sizeof(out), 4220u) != PU_OK);
    assert(pu_ch32_gdb_send_checked("$Z0,07fffffe,2#", out, sizeof(out), 4230u) != PU_OK);

    /* Duplicate remove is idempotent */
    assert(pu_ch32_gdb_send_checked("$z0,08003ffe,2#", out, sizeof(out), 4240u) == PU_OK);
    assert(pu_ch32_gdb_send_checked("$z0,08003ffe,2#", out, sizeof(out), 4250u) == PU_OK);

    /* 3. Z1 / z1 flash breakpoints route to SoftBreak for GDB 10..16 <memory type="flash"> compatibility */
    assert(pu_ch32_gdb_send_checked("$Z1,08000080,2#", out, sizeof(out), 4300u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "OK");
    assert(pu_ch32_gdb_send_checked("$z1,08000080,2#", out, sizeof(out), 4310u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "OK");

    /* Stage partial vFlashWrite data before disconnect to confirm cache cleanup */
    assert(pu_ch32_gdb_send_checked("$vFlashWrite:08000100:\xAA\xBB#", out, sizeof(out), 4350u) == PU_OK);

    /* 4. Disconnect restores ALL original flash bytes across both pages & drops stale vFlash cache */
    pu_ch32_on_disconnect();
    assert(pu_swio_fake_flash_matches_original() == true);
    assert(pu_engine_current() == PU_ENGINE_IDLE);

    /* Subsequent vFlashDone in a fresh session must not flush the discarded partial write */
    assert(pu_ch32_gdb_send_checked("$vFlashDone#", out, sizeof(out), 4400u) == PU_OK);
    pu_ch32_on_disconnect();
    assert(pu_swio_fake_flash_matches_original() == true);

    /* 5. Flash write failure on second page of cross-page SoftBreak propagates failure & rolls back */
    pu_swio_inject_fake_flash_write_fail(1);
    assert(pu_ch32_gdb_send_checked("$Z0,0800003e,4#", out, sizeof(out), 4500u) != PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out)))[0] == 'E');
    pu_swio_inject_fake_flash_write_fail(-1);
    pu_ch32_on_disconnect();
    assert(pu_swio_fake_flash_matches_original() == true);
}

void test_ch32_gdb_parser_hardening() {
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    uint8_t out[256] = {};

    /* 1. Malformed checksum & non-hex checksum digits -> NACK '-', no engine acquisition */
    const char *bad_csum = "$m08000000,4#00";
    size_t n = pu_ch32_gdb_feed(reinterpret_cast<const uint8_t *>(bad_csum),
                                std::strlen(bad_csum), out, sizeof(out), 5000u);
    assert(n == 1u && out[0] == '-');
    assert(pu_engine_current() == PU_ENGINE_IDLE);

    const char *non_hex_csum = "$?#zz";
    n = pu_ch32_gdb_feed(reinterpret_cast<const uint8_t *>(non_hex_csum),
                         std::strlen(non_hex_csum), out, sizeof(out), 5010u);
    assert(n == 1u && out[0] == '-');
    assert(pu_engine_current() == PU_ENGINE_IDLE);

    /* 2. Interrupted '$' framing resynchronizes cleanly onto the second '$' */
    const char *resync = "$truncated_partial$?#3f";
    n = pu_ch32_gdb_feed(reinterpret_cast<const uint8_t *>(resync),
                         std::strlen(resync), out, sizeof(out), 5020u);
    assert(n > 0u && out[0] == '+');
    assert(extract_gdb_payload(out, n) == "T05");

    /* 3. Wrapped uint32 ranges (0xFFFFFFF0,20) and trailing garbage rejected */
    assert(pu_ch32_gdb_send_checked("$mfffffff0,20#", out, sizeof(out), 5100u) != PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "E01");

    assert(pu_ch32_gdb_send_checked(
               "$Mfffffff0,20:00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff#",
               out, sizeof(out), 5110u) != PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "E01");

    assert(pu_ch32_gdb_send_checked("$qCRC:fffffff0,20#", out, sizeof(out), 5120u) != PU_OK);
    assert(pu_ch32_gdb_send_checked("$vFlashErase:fffffff0,40#", out, sizeof(out), 5130u) != PU_OK);
    assert(pu_ch32_gdb_send_checked("$m08000000,4trailing#", out, sizeof(out), 5140u) != PU_OK);
    assert(pu_ch32_gdb_send_checked("$vFlashErase:08000000,40garbage#", out, sizeof(out), 5150u) != PU_OK);

    pu_ch32_on_disconnect();
    assert(pu_swio_fake_flash_matches_original() == true);
}

void test_ch32_contention_fault_and_unbrick() {
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    uint8_t out[256] = {};

    /* Cross-interface contention: CMSIS-DAP SWD owns debug pins */
    assert(pu_engine_acquire(PU_ENGINE_SWD, 6000u, 500000u) == PU_OK);

    pu_ch32_check_result_t r_busy = pu_ch32_non_destructive_check(6100u);
    assert(r_busy.status == PU_CH32_ERR_BUSY);
    assert(r_busy.flash_writes == 0u);
    assert(pu_engine_current() == PU_ENGINE_SWD);

    assert(pu_ch32_gdb_send_checked("$?#", out, sizeof(out), 6200u) == PU_ERR_ENGINE_BUSY);
    assert(pu_engine_current() == PU_ENGINE_SWD);

    assert(pu_ch32_power_cycle_unbrick(6300u) == PU_ERR_ENGINE_BUSY);
    assert(pu_engine_current() == PU_ENGINE_SWD);

    assert(pu_engine_release(PU_ENGINE_SWD) == PU_OK);

    /* Latched hardware overcurrent fault blocks SWIO & recovery */
    pu_engine_latch_fault(PU_FAULT_OVERCURRENT);
    pu_ch32_check_result_t r_fault = pu_ch32_non_destructive_check(6400u);
    assert(r_fault.status == PU_CH32_ERR_FAULT);
    assert(pu_ch32_power_cycle_unbrick(6500u) == PU_ERR_FAULT_LATCHED);
    assert(pu_engine_clear_fault() == PU_OK);

    /* Power-cycle unbrick acquires & cleanly releases PU_ENGINE_RECOVERY */
    assert(pu_ch32_power_cycle_unbrick(7000u) == PU_OK);
    assert(pu_engine_current() == PU_ENGINE_IDLE);

    /* Review Focus #3: After deadline expiry revokes a crashed GDB session,
     * the very next GDB session must seamlessly re-acquire PU_ENGINE_SWIO without replugging. */
    assert(pu_ch32_gdb_send_checked("$?#", out, sizeof(out), 8000u) == PU_OK);
    assert(pu_engine_current() == PU_ENGINE_SWIO);
    pu_engine_tick(8000u + PU_SWIO_SESSION_TIMEOUT_US + 10u);
    assert(pu_engine_current() == PU_ENGINE_IDLE);

    assert(pu_ch32_gdb_send_checked("$?#", out, sizeof(out), 9000000u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "T05");
    assert(pu_engine_current() == PU_ENGINE_SWIO);
    pu_ch32_on_disconnect();
    assert(pu_engine_current() == PU_ENGINE_IDLE);
}

void test_ch32_register_preservation_step_and_vflash_roundtrip() {
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    uint8_t out[512] = {};

    /* 1. PUID1 check + GDB memory/SoftBreak must preserve scratch GPRs x8 (s0/fp) & x9 (s1) */
    pu_ch32_check_result_t r = pu_ch32_non_destructive_check(10000u);
    assert(r.status == PU_CH32_OK);

    assert(pu_ch32_gdb_send_checked("$p8#", out, sizeof(out), 10100u) == PU_OK);
    const std::string x8_before = extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out)));
    assert(pu_ch32_gdb_send_checked("$p9#", out, sizeof(out), 10110u) == PU_OK);
    const std::string x9_before = extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out)));

    /* Cross-page SoftBreak at 0x0800003E + memory read across boundary returns clean bytes */
    assert(pu_ch32_gdb_send_checked("$m0800003e,4#", out, sizeof(out), 10200u) == PU_OK);
    const std::string orig_insn_hex = extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out)));
    assert(pu_ch32_gdb_send_checked("$Z0,0800003e,4#", out, sizeof(out), 10210u) == PU_OK);
    assert(pu_ch32_gdb_send_checked("$m0800003e,4#", out, sizeof(out), 10220u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == orig_insn_hex);

    /* Single-stepping directly from 0x0800003E while Z0 is active steps the real instruction */
    assert(pu_ch32_gdb_send_checked("$s0800003e#", out, sizeof(out), 10230u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "T05");
    assert(pu_ch32_gdb_send_checked("$p10#", out, sizeof(out), 10240u) == PU_OK);
    /* PC (reg 0x10 = 16) must have advanced past 0x0800003E (not trapped in-place on ebreak!) */
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) != "3e000008");

    /* Verify x8 & x9 were completely preserved */
    assert(pu_ch32_gdb_send_checked("$p8#", out, sizeof(out), 10250u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == x8_before);
    assert(pu_ch32_gdb_send_checked("$p9#", out, sizeof(out), 10260u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == x9_before);

    pu_ch32_on_disconnect();
    assert(pu_swio_fake_flash_matches_original() == true);
}

/* Silicon-accurate custom HAL fixture for real CH32V003 DMSTATUS/IDCODE/MMIO tests */
struct SiliconHalModel {
    bool halted = false;
    bool resume_ack = false;
    bool have_reset = true;
    bool dm_active = false;
    bool cfgr_outen = false;
    bool shdw_outen = false;
    bool pending_halt = false;
    bool pending_resume = false;
    uint32_t polls_until_halt = 0u;
    uint32_t polls_until_resume = 0u;
    uint64_t sim_now_us = 10u;
    uint64_t now_step_us = 50u;
    uint32_t data0 = 0u;
    uint32_t progbuf[8] = {};
    uint32_t gpr[16] = {};
    uint32_t dpc = 0x08000000u;
    uint32_t dcsr = 0u;
    uint8_t cmderr = 0u;
    uint32_t forced_dmstatus_or = 0u;
    uint32_t last_load_addr = 0u;
    uint32_t gpiod_cfglr = 0x44444444u;
    uint16_t gpiod_outdr = 0x0000u;
    uint32_t pfic_ienr1 = 0x00000000u;
    uint32_t lw_count = 0u;
    uint32_t sw_count = 0u;
    uint32_t lhu_count = 0u;
    uint32_t sh_count = 0u;
    bool bus_is_open = false;
    uint32_t bus_close_count = 0u;
    bool resume_halts_immediately = false;
    bool suppress_resume_ack = false;
    bool por_unbrick_mode = false;
    bool powered = true;
    bool pd1_bricked = false;
    uint64_t vtarg_on_time_us = 0u;
    uint64_t delay_after_vtarg_on_us = 0u;
    uint32_t vtarg_off_count = 0u;
    uint32_t vtarg_on_count = 0u;
};

SiliconHalModel g_si;

void si_bus_open() { g_si.bus_is_open = true; }
void si_bus_close() {
    g_si.bus_is_open = false;
    ++g_si.bus_close_count;
}

void si_set_vtarg(bool enabled) {
    if (!enabled) {
        g_si.powered = false;
        ++g_si.vtarg_off_count;
    } else {
        g_si.powered = true;
        ++g_si.vtarg_on_count;
        g_si.vtarg_on_time_us = g_si.sim_now_us;
        g_si.delay_after_vtarg_on_us = 0u;
        g_si.pd1_bricked = false;
        g_si.halted = false;
        g_si.dm_active = false;
        g_si.cfgr_outen = false;
        g_si.shdw_outen = false;
        g_si.have_reset = true;
    }
}

void si_delay_us(uint32_t delay_us) {
    g_si.sim_now_us += delay_us;
    if (g_si.powered && g_si.vtarg_on_count > 0u) {
        g_si.delay_after_vtarg_on_us += delay_us;
    }
}

bool si_por_swio_responsive() {
    if (!g_si.powered) {
        return false;
    }
    if (!g_si.por_unbrick_mode) {
        return !g_si.pd1_bricked;
    }
    if (g_si.vtarg_on_count == 0u) {
        return false; /* Bricked by PD1 reuse prior to VTARG power cycle */
    }
    const uint64_t since_on = g_si.sim_now_us - g_si.vtarg_on_time_us;
    if (since_on < 120u) {
        return false; /* VDD still ramping through POR */
    }
    if (!g_si.halted && since_on > 450u) {
        g_si.pd1_bricked = true; /* User sketch ran main() and disabled PD1 SWIO! */
    }
    return !g_si.pd1_bricked;
}

uint64_t si_now_us() {
    const uint64_t cur = g_si.sim_now_us;
    g_si.sim_now_us += g_si.now_step_us;
    return cur;
}

bool si_read_reg(uint8_t reg_addr, uint32_t *out_val, uint32_t /*timeout_us*/) {
    if (g_si.por_unbrick_mode) {
        g_si.sim_now_us += 15u;
    }
    if (!si_por_swio_responsive() || !g_si.cfgr_outen) {
        *out_val = 0xFFFFFFFFu;
        return true;
    }
    switch (reg_addr) {
    case 0x04u:
        *out_val = g_si.data0;
        return true;
    case 0x10u:
        *out_val = g_si.dm_active ? 1u : 0u;
        return true;
    case 0x11u: {
        if (g_si.pending_halt) {
            if (g_si.polls_until_halt > 0u) {
                --g_si.polls_until_halt;
            } else {
                g_si.pending_halt = false;
                g_si.halted = true;
            }
        }
        if (g_si.pending_resume) {
            if (g_si.polls_until_resume > 0u) {
                --g_si.polls_until_resume;
            } else {
                g_si.pending_resume = false;
                g_si.halted = false;
                g_si.resume_ack = true;
            }
        }
        /* Real CH32V003 silicon sets impebreak (bit 22 = 0x00400000):
         *   Running after reset : 0x004C0C82
         *   Halted  after reset : 0x004C0382 */
        uint32_t st = (1u << 22) | (1u << 7) | 0x2u;
        if (g_si.have_reset) {
            st |= (1u << 19) | (1u << 18);
        }
        if (g_si.resume_ack && !g_si.suppress_resume_ack) {
            st |= (1u << 17) | (1u << 16);
        }
        if (g_si.halted) {
            st |= (1u << 9) | (1u << 8);
        } else {
            st |= (1u << 11) | (1u << 10);
        }
        *out_val = st | g_si.forced_dmstatus_or;
        return true;
    }
    case 0x16u:
        *out_val = (8u << 24) | (static_cast<uint32_t>(g_si.cmderr) << 8) | 2u;
        return true;
    case 0x7Du:
        *out_val = g_si.cfgr_outen ? 0x00000400u : 0u;
        return true;
    case 0x7Eu:
        *out_val = g_si.shdw_outen ? 0x00000400u : 0u;
        return true;
    default:
        *out_val = 0u;
        return true;
    }
}

bool si_write_reg(uint8_t reg_addr, uint32_t val, uint32_t /*timeout_us*/) {
    if (g_si.por_unbrick_mode) {
        g_si.sim_now_us += 15u;
    }
    if (!si_por_swio_responsive()) {
        return true;
    }
    if (reg_addr == 0x7Eu) {
        if ((val & 0xFFFF0000u) == 0x5AA50000u) {
            g_si.shdw_outen = (val & 0x00000400u) != 0u;
        }
        return true;
    }
    if (reg_addr == 0x7Du) {
        if ((val & 0xFFFF0000u) == 0x5AA50000u && (val & 0x00000400u) != 0u) {
            g_si.cfgr_outen = g_si.shdw_outen;
        }
        return true;
    }
    if (reg_addr == 0x10u) {
        if ((val & 1u) == 0u) {
            g_si.dm_active = false;
            return true;
        }
        g_si.dm_active = true;
        if ((val & (1u << 28)) != 0u) {
            g_si.have_reset = false;
        }
        if ((val & (1u << 31)) != 0u) {
            g_si.resume_ack = false;
            if (g_si.polls_until_halt > 0u) {
                g_si.pending_halt = true;
            } else {
                g_si.halted = true;
            }
        } else if ((val & (1u << 30)) != 0u) {
            if (g_si.resume_halts_immediately) {
                g_si.halted = true;
                g_si.resume_ack = true;
            } else if (g_si.polls_until_resume > 0u) {
                g_si.pending_resume = true;
            } else {
                g_si.halted = false;
                g_si.resume_ack = true;
            }
        }
        return true;
    }
    if (!g_si.dm_active) {
        return true;
    }
    if (reg_addr == 0x04u) {
        g_si.data0 = val;
        return true;
    }
    if (reg_addr >= 0x20u && reg_addr < 0x28u) {
        g_si.progbuf[reg_addr - 0x20u] = val;
        return true;
    }
    if (reg_addr == 0x16u) {
        if ((val & (7u << 8)) != 0u) {
            g_si.cmderr = 0u;
        }
        return true;
    }
    if (reg_addr == 0x17u) {
        const bool postexec = (val & (1u << 18)) != 0u;
        const bool transfer = (val & (1u << 17)) != 0u;
        const bool write = (val & (1u << 16)) != 0u;
        const uint16_t regno = static_cast<uint16_t>(val & 0xFFFFu);
        if (transfer) {
            if (regno >= 0x1000u && regno < 0x1010u) {
                const size_t idx = regno - 0x1000u;
                if (write) {
                    if (idx != 0u) g_si.gpr[idx] = g_si.data0;
                } else {
                    g_si.data0 = (idx == 0u) ? 0u : g_si.gpr[idx];
                }
            } else if (regno == 0x07B0u) {
                if (write) g_si.dcsr = g_si.data0;
                else g_si.data0 = g_si.dcsr;
            } else if (regno == 0x07B1u) {
                if (write) g_si.dpc = g_si.data0;
                else g_si.data0 = g_si.dpc;
            }
        }
        if (postexec && g_si.cmderr == 0u) {
            const uint32_t insn = g_si.progbuf[0];
            const uint32_t addr = g_si.gpr[8];
            if (insn == 0x00042483u /* lw x9, 0(x8) */) {
                if ((addr & 3u) != 0u) {
                    g_si.cmderr = 3u;
                } else {
                    ++g_si.lw_count;
                    g_si.last_load_addr = addr;
                    if (addr == 0x1FFFF7C4u) g_si.gpr[9] = 0x00300500u;
                    else if (addr == 0x1FFFF704u) g_si.gpr[9] = 0xFFFFFFFFu;
                    else if (addr == 0x1FFFF7E0u) g_si.gpr[9] = 0x00000010u;
                    else if (addr == 0x1FFFF7E8u) g_si.gpr[9] = 0x32563033u;
                    else if (addr == 0x40011400u) g_si.gpr[9] = g_si.gpiod_cfglr;
                    else if (addr == 0xE000E000u) g_si.gpr[9] = g_si.pfic_ienr1;
                    else g_si.gpr[9] = 0xFFFFFFFFu;
                }
            } else if (insn == 0x00045483u /* lhu x9, 0(x8) */) {
                if ((addr & 1u) != 0u) {
                    g_si.cmderr = 3u;
                } else {
                    ++g_si.lhu_count;
                    if (addr == 0x4001140Cu) g_si.gpr[9] = g_si.gpiod_outdr;
                    else g_si.gpr[9] = 0u;
                }
            } else if (insn == 0x00942023u /* sw x9, 0(x8) */) {
                if ((addr & 3u) != 0u) {
                    g_si.cmderr = 3u;
                } else {
                    ++g_si.sw_count;
                    if (addr == 0x40011400u) g_si.gpiod_cfglr = g_si.gpr[9];
                    else if (addr == 0xE000E000u) g_si.pfic_ienr1 = g_si.gpr[9];
                }
            } else if (insn == 0x00941023u /* sh x9, 0(x8) */) {
                if ((addr & 1u) != 0u) {
                    g_si.cmderr = 3u;
                } else {
                    ++g_si.sh_count;
                    if (addr == 0x4001140Cu) g_si.gpiod_outdr = static_cast<uint16_t>(g_si.gpr[9] & 0xFFFFu);
                }
            } else if (insn == 0x00044483u /* lbu */ || insn == 0x00940023u /* sb */) {
                /* Real CH32V003 MMIO & sig registers fault on 8-bit accesses */
                if (addr >= 0x40000000u || (addr >= 0x1FFFF000u && addr <= 0x1FFFFFFFu)) {
                    g_si.cmderr = 3u;
                }
            } else {
                g_si.cmderr = 3u;
            }
        }
        return true;
    }
    return true;
}

void test_ch32_impebreak_and_idcode_1ffff7c4() {
    g_si = SiliconHalModel{};
    g_si.halted = false;
    g_si.have_reset = true;

    /* Sanity-verify exact silicon DMSTATUS words produced by fixture */
    g_si.cfgr_outen = true;
    uint32_t raw_run = 0u;
    (void)si_read_reg(0x11u, &raw_run, 0u);
    assert(raw_run == 0x004C0C82u);
    g_si.halted = true;
    uint32_t raw_halt = 0u;
    (void)si_read_reg(0x11u, &raw_halt, 0u);
    assert(raw_halt == 0x004C0382u);
    g_si.cfgr_outen = false;

    pu_swio_wire_hal_t hal = {};
    hal.sys_clock_hz = 125000000u;
    hal.read_reg = si_read_reg;
    hal.write_reg = si_write_reg;
    pu_swio_init(&hal);

    /* 1. 0x004C0382 -> classified as Halted (PU_CH32_ERR_ALREADY_HALTED, not NO_REPLY) */
    g_si.halted = true;
    g_si.have_reset = true;
    pu_ch32_check_result_t r_halted = pu_ch32_non_destructive_check(11000u);
    assert(r_halted.status == PU_CH32_ERR_ALREADY_HALTED);

    /* 2. 0x004C0C82 -> classified as Running, halts to 0x004C0382, reads 0x1FFFF7C4 -> 0x00300500 */
    g_si = SiliconHalModel{};
    g_si.halted = false;
    g_si.have_reset = true;
    pu_ch32_check_result_t r_run = pu_ch32_non_destructive_check(11100u);
    assert(r_run.status == PU_CH32_OK);
    assert(r_run.chip_id == 0x00300500u);
    assert(r_run.resumed_running == true);
    assert(r_run.outen_disabled_verified == true);

    /* 3. Adjacent true reserved bits [21] and [23] must STILL be rejected */
    g_si = SiliconHalModel{};
    g_si.forced_dmstatus_or = (1u << 21);
    assert(pu_ch32_non_destructive_check(11200u).status == PU_CH32_ERR_NO_REPLY);
    g_si = SiliconHalModel{};
    g_si.forced_dmstatus_or = (1u << 23);
    assert(pu_ch32_non_destructive_check(11300u).status == PU_CH32_ERR_NO_REPLY);

    /* 4. Host fake target also uses 0x1FFFF7C4 (not 0x1FFFF704) */
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    pu_ch32_check_result_t r_fake = pu_ch32_non_destructive_check(11400u);
    assert(r_fake.status == PU_CH32_OK);
    assert(r_fake.chip_id == 0x00300500u);
    uint8_t out[128] = {};
    assert(pu_ch32_gdb_send_checked("$m1ffff704,4#", out, sizeof(out), 11500u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "ffffffff");
    pu_ch32_on_disconnect();
}

void test_ch32_softbreak_vma_zero_alias() {
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    uint8_t out[256] = {};

    /* Read clean bytes at VMA 0x0000003E before patching */
    assert(pu_ch32_gdb_send_checked("$m0000003e,4#", out, sizeof(out), 12000u) == PU_OK);
    const std::string orig_vma = extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out)));

    /* Insert cross-page 4-byte SoftBreak using WCH Link.ld VMA 0x0000003E */
    assert(pu_ch32_gdb_send_checked("$Z0,0000003e,4#", out, sizeof(out), 12010u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "OK");
    assert(pu_swio_fake_flash_matches_original() == false);

    /* Physical alias 0x0800003E maps to the exact same slot (idempotent duplicate) */
    assert(pu_ch32_gdb_send_checked("$Z0,0800003e,4#", out, sizeof(out), 12020u) == PU_OK);
    assert(pu_ch32_gdb_send_checked("$Z0,00000040,2#", out, sizeof(out), 12030u) != PU_OK);
    assert(pu_ch32_gdb_send_checked("$Z0,0000003e,2#", out, sizeof(out), 12040u) != PU_OK);

    /* Both VMA and physical alias reads hide the patched ebreak */
    assert(pu_ch32_gdb_send_checked("$m0000003e,4#", out, sizeof(out), 12050u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == orig_vma);
    assert(pu_ch32_gdb_send_checked("$m0800003e,4#", out, sizeof(out), 12060u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == orig_vma);

    /* Single-stepping from VMA 0x0000003E steps past the breakpoint */
    assert(pu_ch32_gdb_send_checked("$s0000003e#", out, sizeof(out), 12070u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "T05");
    assert(pu_ch32_gdb_send_checked("$p10#", out, sizeof(out), 12080u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) != "3e000000");

    /* VMA boundary checks at 0x00003FFE / 0x00004000 */
    assert(pu_ch32_gdb_send_checked("$Z0,00003ffe,2#", out, sizeof(out), 12100u) == PU_OK);
    assert(pu_ch32_gdb_send_checked("$Z0,00003ffe,4#", out, sizeof(out), 12110u) != PU_OK);
    assert(pu_ch32_gdb_send_checked("$Z0,00004000,2#", out, sizeof(out), 12120u) != PU_OK);

    /* Clear via VMA addresses restores flash completely */
    assert(pu_ch32_gdb_send_checked("$z0,0000003e,4#", out, sizeof(out), 12130u) == PU_OK);
    assert(pu_ch32_gdb_send_checked("$z0,00003ffe,2#", out, sizeof(out), 12140u) == PU_OK);
    assert(pu_ch32_gdb_send_checked("$z0,00003ffe,2#", out, sizeof(out), 12150u) == PU_OK);
    assert(pu_swio_fake_flash_matches_original() == true);

    pu_ch32_on_disconnect();
}

void test_ch32_poll_timestamp_no_underflow_and_aligned_mmio() {
    /* 1. HAL now_us() starts at 10 us (< start_us = 9000000 us) while target needs 4 polls:
     *    must NOT underflow (now - start_us) to UINT64_MAX on poll #1! */
    g_si = SiliconHalModel{};
    g_si.sim_now_us = 10u;
    g_si.now_step_us = 50u;
    g_si.polls_until_halt = 4u;
    g_si.polls_until_resume = 4u;

    pu_swio_wire_hal_t hal = {};
    hal.sys_clock_hz = 125000000u;
    hal.read_reg = si_read_reg;
    hal.write_reg = si_write_reg;
    hal.now_us = si_now_us;
    pu_swio_init(&hal);

    pu_ch32_check_result_t r_ok = pu_ch32_non_destructive_check(9000000u);
    assert(r_ok.status == PU_CH32_OK);
    assert(r_ok.chip_id == 0x00300500u);

    /* Genuine timeout (>= 500000 us elapsed on HAL clock) still aborts promptly */
    g_si = SiliconHalModel{};
    g_si.sim_now_us = 10u;
    g_si.now_step_us = 150000u;
    g_si.polls_until_halt = 100u;
    assert(pu_ch32_non_destructive_check(9500000u).status == PU_CH32_ERR_NO_REPLY);

    /* 2. Aligned 32-bit lw/sw and 16-bit lhu/sh on CH32V003 MMIO & signature addresses */
    g_si = SiliconHalModel{};
    pu_swio_init(&hal);
    uint8_t out[256] = {};

    /* 32-bit MMIO peripheral register (GPIOD->CFGLR @ 0x40011400) */
    assert(pu_ch32_gdb_send_checked("$M40011400,4:78563412#", out, sizeof(out), 13000u) == PU_OK);
    assert(g_si.sw_count == 1u && g_si.gpiod_cfglr == 0x12345678u);
    assert(pu_ch32_gdb_send_checked("$m40011400,4#", out, sizeof(out), 13010u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "78563412");

    /* 16-bit MMIO peripheral register (GPIOD->OUTDR @ 0x4001140C) */
    assert(pu_ch32_gdb_send_checked("$M4001140c,2:cdab#", out, sizeof(out), 13020u) == PU_OK);
    assert(g_si.sh_count == 1u && g_si.gpiod_outdr == 0xABCDu);
    assert(pu_ch32_gdb_send_checked("$m4001140c,2#", out, sizeof(out), 13030u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "cdab");
    assert(g_si.lhu_count == 1u);

    /* 32-bit QingKe V2 PFIC/SysTick MMIO (0xE000E000) & DBGMCU_IDCODE (0x1FFFF7C4) */
    assert(pu_ch32_gdb_send_checked("$Me000e000,4:efbeadde#", out, sizeof(out), 13040u) == PU_OK);
    assert(pu_ch32_gdb_send_checked("$me000e000,4#", out, sizeof(out), 13050u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "efbeadde");
    assert(pu_ch32_gdb_send_checked("$m1ffff7c4,4#", out, sizeof(out), 13060u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "00053000");
    pu_ch32_on_disconnect();

    /* 3. Unaligned SRAM read/write (1B + 2B + 4B) + healthy fake MMIO roundtrip */
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    assert(pu_ch32_gdb_send_checked("$m1ffff7c4,4#", out, sizeof(out), 13100u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "00053000");
    assert(pu_ch32_gdb_send_checked("$M20000001,7:11223344556677#", out, sizeof(out), 13110u) == PU_OK);
    assert(pu_ch32_gdb_send_checked("$m20000001,7#", out, sizeof(out), 13120u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "11223344556677");
    pu_ch32_on_disconnect();
}

void test_ch32_clear_bp_rollback_and_vflash_coherence() {
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    uint8_t out[256] = {};

    /* 1. Cross-page clear_soft_breakpoint failure on p1 MUST roll back p0 to patched ebreak */
    assert(pu_ch32_gdb_send_checked("$Z0,0800003e,4#", out, sizeof(out), 14000u) == PU_OK);
    pu_swio_inject_fake_flash_write_fail(1); /* p0 unpatches (seq 0), p1 fails (seq 1) */
    assert(pu_ch32_gdb_send_checked("$z0,0800003e,4#", out, sizeof(out), 14010u) != PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out)))[0] == 'E');

    /* Verify hardware flash at 0x0800003E..0x08000041 STILL holds full 4B ebreak (0x00100073) */
    const char *cont_bp = "$c0800003e#23";
    size_t n_bp = pu_ch32_gdb_feed(reinterpret_cast<const uint8_t *>(cont_bp),
                                   std::strlen(cont_bp), out, sizeof(out), 14015u);
    assert(n_bp > 1u && out[0] == '+');
    assert(extract_gdb_payload(out, n_bp) == "T05");

    /* Block p0 restore (seq 0) while allowing p1 restore (seq 1) on disconnect:
     * if p0 was properly rolled back to ebreak during clear_soft_breakpoint, flash != original! */
    pu_swio_inject_fake_flash_write_fail(0);
    pu_ch32_on_disconnect();
    assert(pu_swio_fake_flash_matches_original() == false);

    /* Subsequent clean disconnect restores p0 as well (also proves Issue 6 non-wiping on fail!) */
    pu_swio_inject_fake_flash_write_fail(-1);
    pu_ch32_on_disconnect();
    assert(pu_swio_fake_flash_matches_original() == true);

    /* 2. vFlashErase clears overlapping single-page & cross-page breakpoints coherently */
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    assert(pu_ch32_gdb_send_checked("$m08000040,4#", out, sizeof(out), 14100u) == PU_OK);
    const std::string orig_p1_head =
        extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out)));

    assert(pu_ch32_gdb_send_checked("$Z0,08000010,2#", out, sizeof(out), 14110u) == PU_OK);
    assert(pu_ch32_gdb_send_checked("$Z0,0800003e,4#", out, sizeof(out), 14120u) == PU_OK);
    assert(pu_ch32_gdb_send_checked("$vFlashErase:08000000,40#", out, sizeof(out), 14130u) == PU_OK);

    const char vfw_p0[] = "$vFlashWrite:08000000:\x37\x55\x34\x12#";
    assert(pu_ch32_gdb_send_checked(vfw_p0, out, sizeof(out), 14140u) == PU_OK);
    assert(pu_ch32_gdb_send_checked("$vFlashDone#", out, sizeof(out), 14150u) == PU_OK);
    pu_ch32_on_disconnect();

    /* Reconnect and verify Page 0 kept new image and Page 1 has no half-ebreak */
    assert(pu_ch32_gdb_send_checked("$m08000000,8#", out, sizeof(out), 14160u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) ==
           "37553412ffffffff");
    assert(pu_ch32_gdb_send_checked("$m08000040,4#", out, sizeof(out), 14170u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == orig_p1_head);
    pu_ch32_on_disconnect();

    /* 3. Partial vFlashWrite on a page with active Z0 updates clean snapshot without ebreak poison */
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    assert(pu_ch32_gdb_send_checked("$m08000080,2#", out, sizeof(out), 14200u) == PU_OK);
    const std::string orig_p2_bp =
        extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out)));
    assert(pu_ch32_gdb_send_checked("$Z0,08000080,2#", out, sizeof(out), 14210u) == PU_OK);
    const char vfw_p2[] = "$vFlashWrite:08000090:\xDE\xAD\xBE\xEF#";
    assert(pu_ch32_gdb_send_checked(vfw_p2, out, sizeof(out), 14220u) == PU_OK);
    assert(pu_ch32_gdb_send_checked("$vFlashDone#", out, sizeof(out), 14230u) == PU_OK);
    assert(pu_ch32_gdb_send_checked("$z0,08000080,2#", out, sizeof(out), 14240u) == PU_OK);
    pu_ch32_on_disconnect();

    assert(pu_ch32_gdb_send_checked("$m08000080,2#", out, sizeof(out), 14250u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == orig_p2_bp);
    assert(pu_ch32_gdb_send_checked("$m08000090,4#", out, sizeof(out), 14260u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "deadbeef");
    pu_ch32_on_disconnect();
}

std::string frame_gdb_packet(const std::string &body) {
    uint8_t csum = 0u;
    for (char c : body) {
        csum = static_cast<uint8_t>(csum + static_cast<uint8_t>(c));
    }
    char tail[8] = {};
    std::snprintf(tail, sizeof(tail), "#%02x", static_cast<unsigned>(csum));
    return "$" + body + tail;
}

void test_ch32_gdb_continue_poll_ctrlc_and_fast_bp() {
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    uint8_t out[256] = {};

    /* 1. All-stop 'c' MUST return ONLY '+' (no $OK#9a), enter kGdbRunning,
     *    renew deadline via pu_ch32_gdb_poll, and emit $T05 once breakpoint hits. */
    assert(pu_ch32_gdb_send_checked("$Z0,08000008,2#", out, sizeof(out), 15000u) == PU_OK);
    const std::string cont_0 = frame_gdb_packet("c08000000");
    size_t n = pu_ch32_gdb_feed(reinterpret_cast<const uint8_t *>(cont_0.data()),
                                cont_0.size(), out, sizeof(out), 15010u);
    assert(n == 1u && out[0] == '+');

    uint64_t t = 15010u;
    bool saw_stop = false;
    for (int poll_idx = 0; poll_idx < 16; ++poll_idx) {
        t += 2000000u; /* +2s per poll -> spans >5s PU_SWIO_SESSION_TIMEOUT_US */
        n = pu_ch32_gdb_poll(out, sizeof(out), t);
        pu_engine_tick(t);
        assert(pu_engine_current() == PU_ENGINE_SWIO);
        if (n > 0u) {
            assert(extract_gdb_payload(out, n) == "T05");
            saw_stop = true;
            break;
        }
    }
    assert(saw_stop == true);
    assert(pu_ch32_gdb_send_checked("$p10#", out, sizeof(out), t + 10u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "08000008");

    /* 2. Zero-length feed polls & Ctrl-C (0x03) while running halts and emits $T02 */
    assert(pu_ch32_gdb_send_checked("$z0,08000008,2#", out, sizeof(out), t + 20u) == PU_OK);
    n = pu_ch32_gdb_feed(reinterpret_cast<const uint8_t *>(cont_0.data()),
                         cont_0.size(), out, sizeof(out), t + 30u);
    assert(n == 1u && out[0] == '+');

    n = pu_ch32_gdb_feed(nullptr, 0u, out, sizeof(out), t + 3000000u);
    assert(n == 0u);
    pu_engine_tick(t + 3000000u);
    assert(pu_engine_current() == PU_ENGINE_SWIO);

    const uint8_t ctrl_c = 0x03u;
    n = pu_ch32_gdb_feed(&ctrl_c, 1u, out, sizeof(out), t + 3100000u);
    assert(n > 0u);
    assert(extract_gdb_payload(out, n) == "T02");

    /* 3. Fast-breakpoint race: resuming onto an immediate ebreak (DMSTATUS = ALLRESUMEACK | ALLHALTED)
     *    must emit '+' + '$T05#b8' immediately instead of failing with E01! */
    assert(pu_ch32_gdb_send_checked("$Z0,08000004,2#", out, sizeof(out), t + 3200000u) == PU_OK);
    const std::string cont_fast = frame_gdb_packet("c08000004");
    n = pu_ch32_gdb_feed(reinterpret_cast<const uint8_t *>(cont_fast.data()),
                         cont_fast.size(), out, sizeof(out), t + 3210000u);
    assert(n > 1u && out[0] == '+');
    assert(extract_gdb_payload(out, n) == "T05");
    pu_ch32_on_disconnect();

    /* 4. Non-destructive check STILL requires genuine kRunning after resume */
    g_si = SiliconHalModel{};
    g_si.resume_halts_immediately = true;
    pu_swio_wire_hal_t hal = {};
    hal.sys_clock_hz = 125000000u;
    hal.read_reg = si_read_reg;
    hal.write_reg = si_write_reg;
    pu_swio_init(&hal);
    pu_ch32_check_result_t r_fast = pu_ch32_non_destructive_check(t + 4000000u);
    assert(r_fast.status == PU_CH32_ERR_DETACH_FAILED);
    assert(r_fast.resumed_running == false);
}

void test_ch32_timeout_usb_reset_and_kill_cleanup() {
    uint8_t out[256] = {};

    /* 1. wire_bus_close() MUST run in pu_ch32_on_disconnect() even after engine timeout or USB reset */
    g_si = SiliconHalModel{};
    pu_swio_wire_hal_t hal = {};
    hal.sys_clock_hz = 125000000u;
    hal.bus_open = si_bus_open;
    hal.bus_close = si_bus_close;
    hal.read_reg = si_read_reg;
    hal.write_reg = si_write_reg;
    pu_swio_init(&hal);

    assert(pu_ch32_gdb_send_checked("$?#", out, sizeof(out), 16000u) == PU_OK);
    assert(g_si.bus_is_open == true);
    pu_engine_tick(16000u + PU_SWIO_SESSION_TIMEOUT_US + 10u);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(g_si.bus_is_open == true); /* PIO1 still loaded until swio_engine closes it */
    pu_ch32_on_disconnect();
    assert(g_si.bus_is_open == false);

    assert(pu_ch32_gdb_send_checked("$?#", out, sizeof(out), 25000000u) == PU_OK);
    assert(g_si.bus_is_open == true);
    pu_engine_on_usb_reset();
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    pu_ch32_on_disconnect();
    assert(g_si.bus_is_open == false);

    /* 2. Reconnecting after engine timeout MUST NOT wipe s_breakpoints without restoring flash */
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    assert(pu_ch32_gdb_send_checked("$Z0,0800003e,4#", out, sizeof(out), 30000000u) == PU_OK);
    assert(pu_swio_fake_flash_matches_original() == false);

    /* Active session polling renews deadline past 5s */
    (void)pu_ch32_gdb_poll(out, sizeof(out), 33000000u);
    pu_engine_tick(36000000u);
    assert(pu_engine_current() == PU_ENGINE_SWIO);

    /* Once unpolled past deadline, engine_owner revokes lease, then reconnect restores flash! */
    pu_engine_tick(40000000u);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(pu_swio_fake_flash_matches_original() == false);

    assert(pu_ch32_gdb_send_checked("$?#", out, sizeof(out), 41000000u) == PU_OK);
    assert(pu_ch32_gdb_send_checked("$D#", out, sizeof(out), 41001000u) == PU_OK);
    assert(pu_swio_fake_flash_matches_original() == true);
    assert(pu_engine_current() == PU_ENGINE_IDLE);

    /* 3. GDB 'k' (kill) restores breakpoints, detaches, and releases PU_ENGINE_SWIO */
    assert(pu_ch32_gdb_send_checked("$Z0,0800003e,4#", out, sizeof(out), 42000000u) == PU_OK);
    assert(pu_swio_fake_flash_matches_original() == false);
    const char *kill_pkt = "$k#6b";
    size_t n = pu_ch32_gdb_feed(reinterpret_cast<const uint8_t *>(kill_pkt),
                                std::strlen(kill_pkt), out, sizeof(out), 42001000u);
    assert(n == 1u && out[0] == '+');
    assert(pu_swio_fake_flash_matches_original() == true);
    assert(pu_engine_current() == PU_ENGINE_IDLE);

    /* 4. Disconnect while in kGdbRunning with an active breakpoint must halt, restore flash,
     *    AND resume target so subsequent non-destructive check sees Running (not AlreadyHalted)! */
    assert(pu_ch32_gdb_send_checked("$Z0,08000100,2#", out, sizeof(out), 43000000u) == PU_OK);
    const std::string cont_run = frame_gdb_packet("c08000000");
    n = pu_ch32_gdb_feed(reinterpret_cast<const uint8_t *>(cont_run.data()),
                         cont_run.size(), out, sizeof(out), 43001000u);
    assert(n == 1u && out[0] == '+');
    pu_ch32_on_disconnect();
    assert(pu_swio_fake_flash_matches_original() == true);
    assert(pu_ch32_non_destructive_check(43002000u).status == PU_CH32_OK);
}

void test_ch32_tx_ring_wraparound_and_backpressure() {
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    const std::string g_pkt = frame_gdb_packet("g");
    const std::string m_big = frame_gdb_packet("m08000000,400");

    /* 1. Circular wrap-around past 4352 bytes while queue is NEVER empty */
    std::string stream;
    uint8_t chunk[256] = {};
    size_t n = pu_ch32_gdb_feed(reinterpret_cast<const uint8_t *>(g_pkt.data()),
                                g_pkt.size(), chunk, 50u, 50000000u);
    assert(n == 50u);
    stream.append(reinterpret_cast<const char *>(chunk), n);

    for (int i = 0; i < 60; ++i) {
        n = pu_ch32_gdb_feed(reinterpret_cast<const uint8_t *>(g_pkt.data()),
                             g_pkt.size(), chunk, 141u, 50001000u + static_cast<uint64_t>(i) * 100u);
        assert(n == 141u);
        stream.append(reinterpret_cast<const char *>(chunk), n);
    }
    while ((n = pu_ch32_gdb_poll(chunk, sizeof(chunk), 50100000u)) > 0u) {
        stream.append(reinterpret_cast<const char *>(chunk), n);
    }
    assert(stream.size() == 61u * 141u);
    for (size_t idx = 0u; idx < 61u; ++idx) {
        const size_t base = idx * 141u;
        assert(stream[base] == '+');
        std::string payload = extract_gdb_payload(
            reinterpret_cast<const uint8_t *>(stream.data() + base), 141u);
        assert(payload.size() == 136u);
    }

    /* 2. Multi-packet burst (>12 KiB of replies) drained in 64B chunks without dropping bytes */
    std::string burst;
    for (int i = 0; i < 6; ++i) {
        burst += m_big;
    }
    std::string burst_out;
    n = pu_ch32_gdb_feed(reinterpret_cast<const uint8_t *>(burst.data()),
                         burst.size(), chunk, 64u, 51000000u);
    assert(n == 64u);
    burst_out.append(reinterpret_cast<const char *>(chunk), n);

    uint64_t poll_t = 51000100u;
    while ((n = pu_ch32_gdb_poll(chunk, 64u, poll_t)) > 0u) {
        burst_out.append(reinterpret_cast<const char *>(chunk), n);
        poll_t += 100u;
    }
    assert(burst_out.size() == 6u * 2053u);
    const std::string expected_p0 = extract_gdb_payload(
        reinterpret_cast<const uint8_t *>(burst_out.data()), 2053u);
    assert(expected_p0.size() == 2048u);
    for (size_t idx = 1u; idx < 6u; ++idx) {
        const size_t base = idx * 2053u;
        assert(burst_out[base] == '+');
        assert(extract_gdb_payload(reinterpret_cast<const uint8_t *>(burst_out.data() + base),
                                   2053u) == expected_p0);
    }

    /* 3. pu_ch32_on_disconnect() works cleanly even while TX is full and RX is paused */
    assert(pu_ch32_gdb_send_checked("$Z0,0800003e,4#", chunk, sizeof(chunk), 52000000u) == PU_OK);
    n = pu_ch32_gdb_feed(reinterpret_cast<const uint8_t *>(burst.data()),
                         burst.size(), chunk, 1u, 52001000u);
    assert(n == 1u);
    pu_ch32_on_disconnect();
    assert(pu_swio_fake_flash_matches_original() == true);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(pu_ch32_gdb_poll(chunk, sizeof(chunk), 52002000u) == 0u);
}

void test_ch32_unbrick_vtarg_guard_por_catch_and_dmactive() {
    /* 1. When PU_HAS_VTARG_SWITCH == 0 and no custom set_vtarg_power HAL callback is bound,
     *    pu_ch32_power_cycle_unbrick MUST return PU_ERR_INVALID_ARG! */
#if !PU_HAS_VTARG_SWITCH
    pu_swio_init(nullptr);
    assert(pu_ch32_power_cycle_unbrick(60000000u) == PU_ERR_INVALID_ARG);
    assert(pu_engine_current() == PU_ENGINE_IDLE);
#endif

    pu_swio_wire_hal_t no_vtarg_hal = {};
    no_vtarg_hal.sys_clock_hz = 125000000u;
    no_vtarg_hal.read_reg = si_read_reg;
    no_vtarg_hal.write_reg = si_write_reg;
    no_vtarg_hal.set_vtarg_power = nullptr;
    pu_swio_init(&no_vtarg_hal);
    assert(pu_ch32_power_cycle_unbrick(60001000u) == PU_ERR_INVALID_ARG);
    assert(pu_engine_current() == PU_ENGINE_IDLE);

    /* 2. Fast PD1-reusing sketch (450 us POR window; 1500 us sleep would miss!)
     *    must be caught immediately and left HALTED with DMACTIVE=1 & OUTEN=1! */
    g_si = SiliconHalModel{};
    g_si.por_unbrick_mode = true;
    g_si.pd1_bricked = true;
    g_si.sim_now_us = 1000u;
    g_si.now_step_us = 5u;

    pu_swio_wire_hal_t unbrick_hal = {};
    unbrick_hal.sys_clock_hz = 125000000u;
    unbrick_hal.bus_open = si_bus_open;
    unbrick_hal.bus_close = si_bus_close;
    unbrick_hal.read_reg = si_read_reg;
    unbrick_hal.write_reg = si_write_reg;
    unbrick_hal.set_vtarg_power = si_set_vtarg;
    unbrick_hal.delay_us = si_delay_us;
    unbrick_hal.now_us = si_now_us;
    pu_swio_init(&unbrick_hal);

    assert(pu_ch32_non_destructive_check(60500000u).status == PU_CH32_ERR_NO_REPLY);
    assert(pu_ch32_power_cycle_unbrick(61000000u) == PU_OK);
    assert(g_si.vtarg_off_count == 1u && g_si.vtarg_on_count == 1u);
    assert(g_si.delay_after_vtarg_on_us == 0u);
    assert(g_si.halted == true);
    assert(g_si.dm_active == true);
    assert(g_si.cfgr_outen == true);
    assert(g_si.bus_is_open == false);
    assert(pu_engine_current() == PU_ENGINE_IDLE);

    /* 3. Healthy fake target is also left HALTED with DMACTIVE=1 ready for vFlashErase */
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    assert(pu_ch32_power_cycle_unbrick(62000000u) == PU_OK);
    assert(pu_ch32_non_destructive_check(62001000u).status == PU_CH32_ERR_ALREADY_HALTED);
    uint8_t out[128] = {};
    assert(pu_ch32_gdb_send_checked("$vFlashErase:08000000,40#", out, sizeof(out), 62002000u) == PU_OK);
    pu_ch32_on_disconnect();
}

/* Cycle-accurate RP2040 PIO state-machine simulator for singlewire.pio */
void test_ch32_pio_cycle_accurate_timing() {
    size_t prog_len = 0u;
    const uint16_t *prog = pu_swio_get_pio_instructions(&prog_len);
    assert(prog != nullptr && prog_len == 22u);

    auto run_pio_frame = [&](const uint32_t *tx_words,
                             size_t tx_count,
                             uint32_t target_rx_word,
                             bool is_read,
                             uint32_t *out_rx_word,
                             uint32_t *out_min_zero_low,
                             uint32_t *out_max_zero_low,
                             uint32_t *out_one_low,
                             uint32_t *out_rx_trig_low,
                             uint32_t *out_rx_sample_cyc) {
        uint32_t pc = 0u;
        uint32_t x = 0u, y = 0u;
        uint32_t osr = 0u, isr = 0u;
        uint32_t osr_bits = 0u; /* remaining unshifted bits in OSR (0 => OSRE) */
        size_t tx_idx = 0u;
        bool pindir = false;
        uint32_t low_run = 0u;
        uint32_t bit_start_cyc = 0u;
        uint32_t cyc = 0u;
        int rx_bit_idx = 31;
        bool target_holding_low = false;
        uint32_t target_release_cyc = 0u;
        size_t pulses_seen = 0u;

        *out_min_zero_low = 999u;
        *out_max_zero_low = 0u;
        *out_one_low = 0u;
        *out_rx_trig_low = 0u;
        *out_rx_sample_cyc = 0u;

        for (uint32_t guard = 0u; guard < 4096u; ++guard) {
            const uint16_t insn = prog[pc];
            const uint16_t op = (insn >> 13) & 0x7u;
            const bool side_pindir = ((insn >> 12) & 0x1u) != 0u;
            const uint32_t delay = (insn >> 8) & 0xFu;
            const uint16_t arg1 = (insn >> 5) & 0x7u;
            const uint16_t arg2 = insn & 0x1Fu;

            if (op == 4u && (arg1 & 4u) != 0u && tx_idx >= tx_count) {
                /* Reached next frame's blocking PULL at frame_start -> done! */
                break;
            }

            const uint32_t total_cycles = 1u + delay;
            uint32_t next_pc = (pc == 21u) ? 0u : (pc + 1u);

            /* Rising/falling edge tracking on cycle 0 of instruction */
            if (!pindir && side_pindir) {
                bit_start_cyc = cyc;
                low_run = 0u;
                if (is_read && pulses_seen >= 9u && rx_bit_idx >= 0) {
                    const bool bit_val = ((target_rx_word >> rx_bit_idx) & 1u) != 0u;
                    if (!bit_val) {
                        target_holding_low = true;
                        target_release_cyc = cyc + 13u; /* Target holds '0' for 6.5T */
                    } else {
                        target_holding_low = false;
                    }
                }
            } else if (pindir && !side_pindir) {
                ++pulses_seen;
                if (is_read && pulses_seen > 9u) {
                    *out_rx_trig_low = low_run;
                } else if (low_run <= 6u) {
                    *out_one_low = low_run;
                } else {
                    if (low_run < *out_min_zero_low) *out_min_zero_low = low_run;
                    if (low_run > *out_max_zero_low) *out_max_zero_low = low_run;
                }
                low_run = 0u;
            }
            pindir = side_pindir;

            /* Execute PIO instruction semantics on first cycle */
            if (op == 0u) { /* JMP */
                bool cond = false;
                if (arg1 == 0u) cond = true;
                else if (arg1 == 1u) cond = (x == 0u);
                else if (arg1 == 2u) { cond = (x != 0u); x = (x - 1u); }
                else if (arg1 == 3u) cond = (y == 0u);
                else if (arg1 == 4u) { cond = (y != 0u); y = (y - 1u); }
                else if (arg1 == 7u) cond = (osr_bits > 0u); /* !OSRE */
                if (cond) next_pc = arg2;
            } else if (op == 2u) { /* IN pins, count (left shift) */
                if (target_holding_low && cyc >= target_release_cyc) {
                    target_holding_low = false;
                }
                const uint32_t line_high = (!pindir && !target_holding_low) ? 1u : 0u;
                *out_rx_sample_cyc = cyc - bit_start_cyc;
                isr = (isr << 1) | line_high;
                --rx_bit_idx;
            } else if (op == 3u) { /* OUT dest, count (left shift MSB-first) */
                const uint32_t count = (arg2 == 0u) ? 32u : arg2;
                const uint32_t shifted = (count == 32u) ? osr : (osr >> (32u - count));
                osr = (count == 32u) ? 0u : (osr << count);
                osr_bits = (osr_bits > count) ? (osr_bits - count) : 0u;
                if (arg1 == 1u) x = shifted;
                else if (arg1 == 2u) y = shifted;
            } else if (op == 4u) { /* PUSH / PULL */
                if ((arg1 & 4u) == 0u) { /* PUSH */
                    *out_rx_word = isr;
                    isr = 0u;
                } else { /* PULL */
                    assert(tx_idx < tx_count);
                    osr = tx_words[tx_idx++];
                    osr_bits = 32u;
                }
            } else if (op == 5u) { /* MOV / NOP (mov y, y) */
                (void)arg1;
            } else if (op == 7u) { /* SET */
                if (arg1 == 1u) x = arg2;
                else if (arg1 == 2u) y = arg2;
            }

            if (pindir) {
                low_run += total_cycles;
            }
            cyc += total_cycles;
            pc = next_pc;
        }
    };

    /* 1. Verify Write frame (mode=2, reg=0x10 DMCONTROL, val=0x80000001):
     *    TX '1' = 3 cyc (1.5T), TX '0' = 15 cyc (7.5T, widened from knife-edge 12 cyc = 6.0T!) */
    const uint32_t wr_words[2] = {(2u << 9) | (1u << 8) | (0x10u << 1) | 1u, 0x80000001u};
    uint32_t rx_out = 0u, min_z = 0u, max_z = 0u, one_low = 0u, rx_trig = 0u, rx_samp = 0u;
    run_pio_frame(wr_words, 2u, 0u, false, &rx_out, &min_z, &max_z, &one_low, &rx_trig, &rx_samp);
    assert(one_low == 3u);
    assert(min_z == 15u && max_z == 15u);

    /* 2. Verify Read frame (mode=1, reg=0x11 DMSTATUS -> 0x004C0C82):
     *    RX host trigger = 3 cyc (1.5T, widened from 2 cyc = 1.0T), sample @ 7 cyc (3.5T)! */
    const uint32_t rd_words[1] = {(1u << 9) | (1u << 8) | (0x11u << 1) | 0u};
    run_pio_frame(rd_words, 1u, 0x004C0C82u, true, &rx_out, &min_z, &max_z, &one_low, &rx_trig, &rx_samp);
    assert(rx_trig == 3u);
    assert(rx_samp == 7u);
    assert(rx_out == 0x004C0C82u);
}

void test_ch32_qingke_v2a_no_resumeack_extended_rsp_and_qrcmd_unbrick() {
    uint8_t out[1536] = {};

    /* 1. Real QingKe V2A silicon transitions DMSTATUS from 0x004C0382 -> 0x004C0C82
     *    (ALLRUNNING=1, ALLRESUMEACK=0) on RESUMEREQ: verify PUID1 check + GDB continue! */
    g_si = SiliconHalModel{};
    g_si.suppress_resume_ack = true;
    g_si.have_reset = true;
    pu_swio_wire_hal_t hal = {};
    hal.sys_clock_hz = 125000000u;
    hal.bus_open = si_bus_open;
    hal.bus_close = si_bus_close;
    hal.read_reg = si_read_reg;
    hal.write_reg = si_write_reg;
    hal.now_us = si_now_us;
    pu_swio_init(&hal);

    pu_ch32_check_result_t r_chk = pu_ch32_non_destructive_check(70000000u);
    assert(r_chk.status == PU_CH32_OK);
    assert(r_chk.resumed_running == true);
    assert(r_chk.outen_disabled_verified == true);

    /* Extended-remote '!' must return OK; memory-map must expose ROM/MMIO/PFIC regions */
    assert(pu_ch32_gdb_send_checked("$!#", out, sizeof(out), 70001000u) == PU_OK);
    assert(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out))) == "OK");

    assert(pu_ch32_gdb_send_checked("$qXfer:memory-map:read::0,3fa#", out, sizeof(out), 70001500u) == PU_OK);
    const std::string mmap = extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out)));
    assert(mmap.find("0x1ffff000") != std::string::npos);
    assert(mmap.find("0x40000000") != std::string::npos);
    assert(mmap.find("0xe0000000") != std::string::npos);

    /* GDB continue ('c') with ALLRESUMEACK=0 enters running cleanly */
    const std::string cont_pkt = frame_gdb_packet("c");
    size_t n = pu_ch32_gdb_feed(reinterpret_cast<const uint8_t *>(cont_pkt.data()),
                                cont_pkt.size(), out, sizeof(out), 70002000u);
    assert(n == 1u && out[0] == '+');
    pu_ch32_on_disconnect();

    /* Fast breakpoint hit with ALLRESUMEACK=0 & DCSR.CAUSE == 1 (EBREAK) emits immediate T05 */
    g_si = SiliconHalModel{};
    g_si.suppress_resume_ack = true;
    g_si.resume_halts_immediately = true;
    g_si.dcsr = (1u << 6); /* CSR_DCSR_CAUSE_EBREAK */
    pu_swio_init(&hal);
    assert(pu_ch32_gdb_send_checked("$?#", out, sizeof(out), 70003000u) == PU_OK);
    n = pu_ch32_gdb_feed(reinterpret_cast<const uint8_t *>(cont_pkt.data()),
                         cont_pkt.size(), out, sizeof(out), 70003500u);
    assert(n > 1u && out[0] == '+');
    assert(extract_gdb_payload(out, n) == "T05");
    pu_ch32_on_disconnect();

    /* 2. Track A qRcmd,unbrick ("756e627269636b") catches target, leaves HALTED, and honors contention/fault */
    pu_swio_set_fake_wire_mode(PU_WIRE_HEALTHY_CH32V003);
    assert(pu_engine_acquire(PU_ENGINE_SWD, 71000000u, 500000u) == PU_OK);
    assert(pu_ch32_gdb_send_checked("$qRcmd,756e627269636b#", out, sizeof(out), 71000100u) == PU_OK);
    assert(decode_hex_ascii(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out)))) ==
           "UNBRICK:ERR_ENGINE_BUSY\n");
    assert(pu_engine_release(PU_ENGINE_SWD) == PU_OK);

    assert(pu_ch32_gdb_send_checked("$qRcmd,756e627269636b#", out, sizeof(out), 71001000u) == PU_OK);
    assert(decode_hex_ascii(extract_gdb_payload(out, std::strlen(reinterpret_cast<const char *>(out)))) ==
           "UNBRICK:OK\n");
    assert(pu_engine_current() == PU_ENGINE_IDLE);
    assert(pu_ch32_non_destructive_check(71002000u).status == PU_CH32_ERR_ALREADY_HALTED);
    pu_ch32_on_disconnect();
}

} // namespace

int main() {
    test_ch32_silent_bus_never_misclassified();
    test_ch32_already_halted_rejected();
    test_ch32_check_preserves_running_sketch_and_resets_outen();
    test_ch32_outen_reset_and_resume_failures();
    test_ch32_softbreak_cross_page_and_disconnect_restore();
    test_ch32_gdb_parser_hardening();
    test_ch32_contention_fault_and_unbrick();
    test_ch32_register_preservation_step_and_vflash_roundtrip();
    test_ch32_impebreak_and_idcode_1ffff7c4();
    test_ch32_softbreak_vma_zero_alias();
    test_ch32_poll_timestamp_no_underflow_and_aligned_mmio();
    test_ch32_clear_bp_rollback_and_vflash_coherence();
    test_ch32_gdb_continue_poll_ctrlc_and_fast_bp();
    test_ch32_timeout_usb_reset_and_kill_cleanup();
    test_ch32_tx_ring_wraparound_and_backpressure();
    test_ch32_unbrick_vtarg_guard_por_catch_and_dmactive();
    test_ch32_pio_cycle_accurate_timing();
    test_ch32_qingke_v2a_no_resumeack_extended_rsp_and_qrcmd_unbrick();
    std::puts("test_ch32_wire_model: all checks passed");
    return 0;
}
