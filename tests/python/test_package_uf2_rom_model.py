import json
import random
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools.package_uf2 import (  # noqa: E402
    UF2PackageError,
    pack_unified_uf2,
    simulate_rp2040_bootsel_flash,
    stamp_slot_descriptor,
)

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY = 0x00002000
RP2040_FAMILY_ID = 0xE48BFF56

PU_IMAGE_DESC_MAGIC = 0x5055494D
PU_IMAGE_DESC_VERSION = 1


def _build_valid_slot_binary(slot_addr: int, payload_len: int = 4300) -> bytes:
    """Create an unstamped slot binary with valid Cortex-M0+ vectors at +0x100."""
    buf = bytearray(b"\xFF" * 256)
    payload = bytearray((i * 29 + 7) & 0xFF for i in range(payload_len))
    msp = 0x20042000
    entry = slot_addr + 0x100 + 0xC0 + 1  # Thumb bit set
    struct.pack_into("<2I", payload, 0, msp, entry)
    struct.pack_into("<H", payload, 0xC0, 0x4770)  # bx lr
    return bytes(buf + payload)


def _encode_sparse_uf2(address_to_page: dict[int, bytes]) -> bytes:
    """Deliberately build an unpadded sparse UF2 to demonstrate the ROM erase bug."""
    addrs = sorted(address_to_page)
    total = len(addrs)
    blocks = []
    for idx, addr in enumerate(addrs):
        page = address_to_page[addr]
        assert len(page) == 256
        hdr = struct.pack(
            "<8I",
            UF2_MAGIC_START0,
            UF2_MAGIC_START1,
            UF2_FLAG_FAMILY,
            addr,
            256,
            idx,
            total,
            RP2040_FAMILY_ID,
        )
        blocks.append(hdr + page + bytes(476 - 256) + struct.pack("<I", UF2_MAGIC_END))
    return b"".join(blocks)


class TestPackageUF2RomModel(unittest.TestCase):
    def setUp(self) -> None:
        self.layout = json.loads((ROOT / "config" / "flash_layout.json").read_text(encoding="utf-8"))
        self.flash_base = self.layout["flash_base"]
        self.flash_size = self.layout["flash_size"]
        self.parts = {p["name"]: p for p in self.layout["partitions"]}

    def _prepopulated_flash(self, fill: int = 0x5A) -> bytearray:
        flash = bytearray([fill] * self.flash_size)
        for name, marker in (
            ("journal_a", b"JRNL_A_COMMITTED_RECORD_V1\x00"),
            ("journal_b", b"JRNL_B_BACKUP_RECORD_V1\x00"),
            ("factory_meta", b"FACTORY_CAL_PRO_PCB_REV_B\x00"),
        ):
            off = self.parts[name]["offset"]
            size = self.parts[name]["size"]
            pattern = (marker * ((size // len(marker)) + 1))[:size]
            flash[off : off + size] = pattern
        return flash

    def test_bootsel_delivery_orders_and_duplicates_preserve_journals_and_factory(self) -> None:
        # Non-sector-multiple lengths verify trailing 0xFF padding within touched 4 KiB sectors.
        boot = bytes((i * 17 + 3) & 0xFF for i in range(5000))      # Touches 2 sectors in bootloader
        slot_a = bytes((i * 31 + 11) & 0xFF for i in range(9000))   # Touches 3 sectors in app_slot_a

        uf2_bytes = pack_unified_uf2(bootloader=boot, slot_a=slot_a)
        block_count = len(uf2_bytes) // 512
        # 2 sectors (32 pages) + 3 sectors (48 pages) = 80 blocks (divisible by 16)
        self.assertEqual(block_count, 80)

        initial = self._prepopulated_flash(fill=0xA5)
        ja_off, ja_sz = self.parts["journal_a"]["offset"], self.parts["journal_a"]["size"]
        jb_off, jb_sz = self.parts["journal_b"]["offset"], self.parts["journal_b"]["size"]
        fm_off, fm_sz = self.parts["factory_meta"]["offset"], self.parts["factory_meta"]["size"]
        sb_off, sb_sz = self.parts["app_slot_b"]["offset"], self.parts["app_slot_b"]["size"]
        sa_off = self.parts["app_slot_a"]["offset"]

        rng = random.Random(2040)
        dup_order = list(range(block_count))
        rng.shuffle(dup_order)
        # Inject duplicate block deliveries interleaved across the transfer
        dup_order = dup_order[:25] + dup_order[:15] + dup_order[25:] + dup_order[-10:]

        for mode in ("forward", "reverse", "shuffled", dup_order):
            with self.subTest(order=mode if isinstance(mode, str) else "shuffled_with_duplicates"):
                flashed = simulate_rp2040_bootsel_flash(
                    uf2_bytes,
                    order=mode,
                    initial_flash=bytes(initial),
                )
                self.assertEqual(len(flashed), self.flash_size)

                # Bootloader payload + sector tail 0xFF padding
                self.assertEqual(flashed[0 : len(boot)], boot)
                self.assertEqual(flashed[len(boot) : 8192], b"\xFF" * (8192 - len(boot)))
                # Untouched bootloader sectors preserved
                self.assertEqual(flashed[8192:ja_off], initial[8192:ja_off])

                # Slot A payload + sector tail 0xFF padding
                self.assertEqual(flashed[sa_off : sa_off + len(slot_a)], slot_a)
                self.assertEqual(
                    flashed[sa_off + len(slot_a) : sa_off + 12288],
                    b"\xFF" * (12288 - len(slot_a)),
                )
                # Untouched Slot A sectors preserved
                self.assertEqual(flashed[sa_off + 12288 : sb_off], initial[sa_off + 12288 : sb_off])

                # Untouched Slot B, journal_a, journal_b, and factory_meta preserved bit-for-bit
                self.assertEqual(flashed[ja_off : ja_off + ja_sz], initial[ja_off : ja_off + ja_sz])
                self.assertEqual(flashed[sb_off : sb_off + sb_sz], initial[sb_off : sb_off + sb_sz])
                self.assertEqual(flashed[jb_off : jb_off + jb_sz], initial[jb_off : jb_off + jb_sz])
                self.assertEqual(flashed[fm_off : fm_off + fm_sz], initial[fm_off : fm_off + fm_sz])

    def test_sparse_unpadded_uf2_corrupts_under_rom_model_while_packager_prevents_it(self) -> None:
        boot = bytes([0xCC] * 768)   # 3 pages at 0x10000000 (blocks 0..2 in sparse stream)
        slot_a = bytes([0xAA] * 768) # 3 pages at 0x10010000 (blocks 3..5 share group 0 // 16!)

        sparse_pages = {
            self.flash_base + i * 256: boot[i * 256 : (i + 1) * 256] for i in range(3)
        }
        sa_base = self.flash_base + self.parts["app_slot_a"]["offset"]
        for i in range(3):
            sparse_pages[sa_base + i * 256] = slot_a[i * 256 : (i + 1) * 256]

        sparse_uf2 = _encode_sparse_uf2(sparse_pages)
        initial = bytes(self._prepopulated_flash(fill=0x55))

        # Sparse UF2 shares erase-bitmap group 0 across bootloader & slot_a:
        # forward delivery skips erasing slot_a so NOR AND-programming yields 0x55 & 0xAA == 0x00!
        corrupted = simulate_rp2040_bootsel_flash(sparse_uf2, order="forward", initial_flash=initial)
        sa_off = self.parts["app_slot_a"]["offset"]
        self.assertNotEqual(corrupted[sa_off : sa_off + 768], slot_a)
        self.assertEqual(corrupted[sa_off : sa_off + 768], bytes([0x00] * 768))

        # Sector-padded pack_unified_uf2 eliminates the bug completely.
        safe_uf2 = pack_unified_uf2(bootloader=boot, slot_a=slot_a)
        clean = simulate_rp2040_bootsel_flash(safe_uf2, order="forward", initial_flash=initial)
        self.assertEqual(clean[0:768], boot)
        self.assertEqual(clean[sa_off : sa_off + 768], slot_a)

    def test_rejects_oversized_and_journal_overlapping_inputs(self) -> None:
        boot_cap = self.parts["bootloader"]["size"]
        slot_a_cap = self.parts["app_slot_a"]["size"]
        slot_b_cap = self.parts["app_slot_b"]["size"]

        with self.assertRaisesRegex(UF2PackageError, "bootloader.*exceeds|oversized"):
            pack_unified_uf2(bootloader=bytes(boot_cap + 1))

        with self.assertRaisesRegex(UF2PackageError, "app_slot_a.*exceeds|oversized"):
            pack_unified_uf2(slot_a=bytes(slot_a_cap + 1))

        with self.assertRaisesRegex(UF2PackageError, "app_slot_b.*exceeds|oversized"):
            pack_unified_uf2(slot_b=bytes(slot_b_cap + 1))

        with self.assertRaises(UF2PackageError):
            pack_unified_uf2()

        # UF2 file trying to write into journal_a (0x1000F000) or factory_meta (0x101FF000) must fail
        scratch_base = ROOT / "build" / "py_scratch"
        scratch_base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch_base) as td:
            td_path = Path(td)
            bad_journal_uf2 = td_path / "bad_journal.uf2"
            bad_journal_uf2.write_bytes(
                _encode_sparse_uf2({self.flash_base + self.parts["journal_a"]["offset"]: b"\x11" * 256})
            )
            with self.assertRaisesRegex(UF2PackageError, "outside|overlap|partition"):
                pack_unified_uf2(bootloader=bad_journal_uf2)

            bad_factory_uf2 = td_path / "bad_factory.uf2"
            bad_factory_uf2.write_bytes(
                _encode_sparse_uf2({self.flash_base + self.parts["factory_meta"]["offset"]: b"\x22" * 256})
            )
            with self.assertRaisesRegex(UF2PackageError, "outside|overlap|partition"):
                pack_unified_uf2(slot_b=bad_factory_uf2)

    def test_stamp_descriptor_and_cli_verify_rom_model(self) -> None:
        sa_addr = self.flash_base + self.parts["app_slot_a"]["offset"]
        sb_addr = self.flash_base + self.parts["app_slot_b"]["offset"]
        sa_off = self.parts["app_slot_a"]["offset"]
        sb_off = self.parts["app_slot_b"]["offset"]

        boot_bin = _build_valid_slot_binary(self.flash_base, payload_len=3840)
        slot_a_bin = _build_valid_slot_binary(sa_addr, payload_len=1500)
        slot_b_bin = _build_valid_slot_binary(sb_addr, payload_len=2048)

        scratch_base = ROOT / "build" / "py_scratch"
        scratch_base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch_base) as td:
            td_path = Path(td)
            p_boot = td_path / "bootloader.bin"
            p_sa = td_path / "slot_a.bin"
            p_sb = td_path / "slot_b.bin"
            p_out = td_path / "unified.uf2"

            p_boot.write_bytes(boot_bin)
            p_sa.write_bytes(slot_a_bin)
            p_sb.write_bytes(slot_b_bin)

            res = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools" / "package_uf2.py"),
                    "--bootloader",
                    str(p_boot),
                    "--slot-a",
                    str(p_sa),
                    "--slot-b",
                    str(p_sb),
                    "--stamp-descriptor",
                    "--verify-rom-model",
                    "--output",
                    str(p_out),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(res.returncode, 0, msg=res.stderr)
            self.assertTrue(p_out.is_file())

            flashed = simulate_rp2040_bootsel_flash(p_out.read_bytes(), order="shuffled")

            for slot_off, slot_addr, raw_bin in (
                (sa_off, sa_addr, slot_a_bin),
                (sb_off, sb_addr, slot_b_bin),
            ):
                hdr = flashed[slot_off : slot_off + 32]
                (
                    magic,
                    hdr_ver,
                    img_ver,
                    desc_addr,
                    img_size,
                    payload_crc,
                    flags,
                    hdr_crc,
                ) = struct.unpack("<8I", hdr)
                expected_payload = raw_bin[0x100:]
                self.assertEqual(magic, PU_IMAGE_DESC_MAGIC)
                self.assertEqual(hdr_ver, PU_IMAGE_DESC_VERSION)
                self.assertEqual(img_ver, 1)
                self.assertEqual(desc_addr, slot_addr)
                self.assertEqual(img_size, len(expected_payload))
                self.assertEqual(flags, 0)
                self.assertEqual(payload_crc, zlib.crc32(expected_payload) & 0xFFFFFFFF)
                self.assertEqual(hdr_crc, zlib.crc32(hdr[:28]) & 0xFFFFFFFF)
                self.assertEqual(
                    flashed[slot_off + 0x100 : slot_off + 0x100 + len(expected_payload)],
                    expected_payload,
                )

            # Invalid vector table in slot binary must be rejected when --stamp-descriptor is used
            bad_slot = bytearray(slot_a_bin)
            struct.pack_into("<I", bad_slot, 0x100, 0x20000000)  # Invalid MSP
            with self.assertRaises(UF2PackageError):
                pack_unified_uf2(slot_a=bytes(bad_slot), stamp_descriptor=True)

    def test_cli_defaults_to_stamping_and_validates_bootloader_vectors(self) -> None:
        sa_addr = self.flash_base + self.parts["app_slot_a"]["offset"]
        sa_off = self.parts["app_slot_a"]["offset"]

        valid_boot = _build_valid_slot_binary(self.flash_base, payload_len=1024)
        valid_slot_a = _build_valid_slot_binary(sa_addr, payload_len=1024)

        scratch_base = ROOT / "build" / "py_scratch"
        scratch_base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch_base) as td:
            td_path = Path(td)
            p_boot = td_path / "bootloader.bin"
            p_sa = td_path / "slot_a.bin"
            p_out_default = td_path / "default_stamped.uf2"
            p_out_raw = td_path / "no_stamp.uf2"

            p_boot.write_bytes(valid_boot)
            p_sa.write_bytes(valid_slot_a)

            # Default CLI invocation (no --stamp-descriptor flag) MUST stamp slot descriptors
            res_default = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools" / "package_uf2.py"),
                    "--bootloader",
                    str(p_boot),
                    "--slot-a",
                    str(p_sa),
                    "--output",
                    str(p_out_default),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(res_default.returncode, 0, msg=res_default.stderr)
            flashed_default = simulate_rp2040_bootsel_flash(p_out_default.read_bytes())
            (magic,) = struct.unpack_from("<I", flashed_default, sa_off)
            self.assertEqual(magic, PU_IMAGE_DESC_MAGIC)

            # Explicit --no-stamp-descriptor preserves raw 0xFF header page
            res_raw = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools" / "package_uf2.py"),
                    "--bootloader",
                    str(p_boot),
                    "--slot-a",
                    str(p_sa),
                    "--no-stamp-descriptor",
                    "--output",
                    str(p_out_raw),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(res_raw.returncode, 0, msg=res_raw.stderr)
            flashed_raw = simulate_rp2040_bootsel_flash(p_out_raw.read_bytes())
            self.assertEqual(flashed_raw[sa_off : sa_off + 256], b"\xFF" * 256)

            # Invalid bootloader vectors at 0x10000100 must be rejected when stamping is enabled
            p_bad_boot = td_path / "bad_boot.bin"
            p_bad_boot.write_bytes(bytes([0xE7] * 4096))
            res_bad_boot = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools" / "package_uf2.py"),
                    "--bootloader",
                    str(p_bad_boot),
                    "--slot-a",
                    str(p_sa),
                    "--output",
                    str(td_path / "bad_out.uf2"),
                ],
                capture_output=True,
                text=True,
            )
            self.assertNotEqual(res_bad_boot.returncode, 0)

    def test_odd_length_bin_payload_padded_with_ff_when_stamping(self) -> None:
        sa_addr = self.flash_base + self.parts["app_slot_a"]["offset"]
        sa_off = self.parts["app_slot_a"]["offset"]

        odd_slot_a = _build_valid_slot_binary(sa_addr, payload_len=1501)
        expected_padded_payload = odd_slot_a[0x100:] + b"\xFF"

        uf2_bytes = pack_unified_uf2(slot_a=odd_slot_a, stamp_descriptor=True)
        flashed = simulate_rp2040_bootsel_flash(uf2_bytes)

        hdr = flashed[sa_off : sa_off + 32]
        magic, hdr_ver, _, desc_addr, img_size, payload_crc, flags, hdr_crc = struct.unpack("<8I", hdr)
        self.assertEqual(magic, PU_IMAGE_DESC_MAGIC)
        self.assertEqual(hdr_ver, PU_IMAGE_DESC_VERSION)
        self.assertEqual(desc_addr, sa_addr)
        self.assertEqual(img_size, 1502)
        self.assertEqual(flags, 0)
        self.assertEqual(payload_crc, zlib.crc32(expected_padded_payload) & 0xFFFFFFFF)
        self.assertEqual(hdr_crc, zlib.crc32(hdr[:28]) & 0xFFFFFFFF)
        self.assertEqual(flashed[sa_off + 0x100 : sa_off + 0x100 + 1502], expected_padded_payload)

        # Boundary: if +1 padding pushes total size beyond slot_size, must reject
        with self.assertRaises(UF2PackageError):
            stamp_slot_descriptor(odd_slot_a, slot_addr=sa_addr, slot_size=256 + 1501)

    def test_rejects_elf_extension_and_elf_magic_inputs(self) -> None:
        sa_addr = self.flash_base + self.parts["app_slot_a"]["offset"]
        fake_elf = b"\x7fELF" + _build_valid_slot_binary(sa_addr, payload_len=512)[4:]

        # Raw bytes starting with ELF magic must be rejected even with stamp_descriptor=False
        with self.assertRaisesRegex(UF2PackageError, "ELF"):
            pack_unified_uf2(slot_a=fake_elf, stamp_descriptor=False)

        scratch_base = ROOT / "build" / "py_scratch"
        scratch_base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch_base) as td:
            td_path = Path(td)

            # Unsupported extension (.elf) must be rejected
            p_elf = td_path / "firmware.elf"
            p_elf.write_bytes(_build_valid_slot_binary(sa_addr, payload_len=512))
            with self.assertRaisesRegex(UF2PackageError, r"\.bin.*\.uf2|extension"):
                pack_unified_uf2(slot_a=p_elf, stamp_descriptor=False)

            # ELF contents disguised with .bin extension must also be rejected
            p_disguised = td_path / "disguised.bin"
            p_disguised.write_bytes(fake_elf)
            with self.assertRaisesRegex(UF2PackageError, "ELF"):
                pack_unified_uf2(bootloader=p_disguised, stamp_descriptor=False)

    def test_rejects_offset_uf2_not_starting_at_partition_base(self) -> None:
        sa_addr = self.flash_base + self.parts["app_slot_a"]["offset"]

        scratch_base = ROOT / "build" / "py_scratch"
        scratch_base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=scratch_base) as td:
            td_path = Path(td)

            offset_boot_uf2 = td_path / "offset_boot.uf2"
            offset_boot_uf2.write_bytes(
                _encode_sparse_uf2({self.flash_base + 0x1000: b"\xAA" * 256})
            )
            with self.assertRaisesRegex(UF2PackageError, "start|base"):
                pack_unified_uf2(bootloader=offset_boot_uf2, stamp_descriptor=False)

            offset_sa_uf2 = td_path / "offset_sa.uf2"
            offset_sa_uf2.write_bytes(
                _encode_sparse_uf2({sa_addr + 0x1000: b"\xBB" * 256})
            )
            with self.assertRaisesRegex(UF2PackageError, "start|base"):
                pack_unified_uf2(slot_a=offset_sa_uf2, stamp_descriptor=False)


if __name__ == "__main__":
    unittest.main()
