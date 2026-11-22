#!/usr/bin/env python3
"""4 KiB sector-padded RP2040 UF2 packager and BOOTSEL ROM flash simulator.

Reads config/flash_layout.json as the single source of truth, validates partition
sizes and non-overlap against journal/factory sectors, optionally stamps valid
pu_image_descriptor_t CRC32 headers into A/B slot binaries, pads every touched
4 KiB erase sector to all 16 x 256-byte pages before UF2 block numbering, and
models the RP2040 BOOTSEL ROM's block_no // 16 sector-erase bitmap + NOR AND
programming semantics.
"""

from __future__ import annotations

import argparse
from collections.abc import Sequence
import os
import random
import struct
import sys
import tempfile
import zlib
from pathlib import Path

try:
    from tools.gen_flash_layout import DEFAULT_IN as DEFAULT_LAYOUT_PATH
    from tools.gen_flash_layout import load_and_validate_layout
except ImportError:
    from gen_flash_layout import DEFAULT_IN as DEFAULT_LAYOUT_PATH  # type: ignore[no-redef]
    from gen_flash_layout import load_and_validate_layout  # type: ignore[no-redef]

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY = 0x00002000
RP2040_FAMILY_ID = 0xE48BFF56
UF2_BLOCK_BYTES = 512
UF2_PAYLOAD_BYTES = 256

PU_IMAGE_DESC_MAGIC = 0x5055494D
PU_IMAGE_DESC_VERSION = 1
PU_IMAGE_HEADER_SIZE = 256
PU_RP2040_SRAM_BASE = 0x20000000
PU_RP2040_SRAM_TOP = 0x20042000


class UF2PackageError(ValueError):
    """Raised when an image or UF2 stream violates RP2040 flash/partition invariants."""


def uf2_to_pages(uf2_bytes: bytes, *, layout: dict | None = None) -> dict[int, bytes]:
    """Validate an RP2040 family-tagged UF2 stream and return {target_addr: 256B_page}."""
    if layout is None:
        layout = load_and_validate_layout(DEFAULT_LAYOUT_PATH)

    page_size = layout["page_size"]
    flash_base = layout["flash_base"]
    flash_end = flash_base + layout["flash_size"]

    if not uf2_bytes or len(uf2_bytes) % UF2_BLOCK_BYTES != 0:
        raise UF2PackageError("UF2 stream must be non-empty and a multiple of 512 bytes")

    block_count = len(uf2_bytes) // UF2_BLOCK_BYTES
    pages: dict[int, bytes] = {}
    seen_numbers: set[int] = set()

    for off in range(0, len(uf2_bytes), UF2_BLOCK_BYTES):
        block = uf2_bytes[off : off + UF2_BLOCK_BYTES]
        magic0, magic1, flags, target_addr, payload_len, block_no, total_blocks, family = (
            struct.unpack_from("<8I", block, 0)
        )
        (magic_end,) = struct.unpack_from("<I", block, 508)

        if (magic0, magic1, magic_end) != (UF2_MAGIC_START0, UF2_MAGIC_START1, UF2_MAGIC_END):
            raise UF2PackageError(f"invalid UF2 block magic at byte offset {off}")
        if flags != UF2_FLAG_FAMILY or family != RP2040_FAMILY_ID:
            raise UF2PackageError("only RP2040 family-tagged flash UF2 blocks are supported")
        if payload_len != page_size or target_addr % page_size != 0:
            raise UF2PackageError(f"UF2 block at 0x{target_addr:08X} must be {page_size}-byte aligned")
        if not (flash_base <= target_addr <= flash_end - page_size):
            raise UF2PackageError(f"UF2 target address 0x{target_addr:08X} is outside physical flash")
        if total_blocks != block_count or block_no >= block_count or block_no in seen_numbers:
            raise UF2PackageError("inconsistent, out-of-range, or duplicate UF2 block numbering")
        if target_addr in pages:
            raise UF2PackageError(f"duplicate UF2 target address 0x{target_addr:08X}")

        seen_numbers.add(block_no)
        pages[target_addr] = bytes(block[32 : 32 + page_size])

    return pages


def _encode_uf2_block(target_addr: int, page: bytes, block_no: int, total_blocks: int) -> bytes:
    if len(page) != UF2_PAYLOAD_BYTES:
        raise UF2PackageError(f"UF2 page must be {UF2_PAYLOAD_BYTES} bytes, got {len(page)}")
    header = struct.pack(
        "<8I",
        UF2_MAGIC_START0,
        UF2_MAGIC_START1,
        UF2_FLAG_FAMILY,
        target_addr,
        UF2_PAYLOAD_BYTES,
        block_no,
        total_blocks,
        RP2040_FAMILY_ID,
    )
    return header + page + bytes(476 - UF2_PAYLOAD_BYTES) + struct.pack("<I", UF2_MAGIC_END)


def _validate_cortex_m0_vectors(
    vector_region: bytes,
    *,
    vector_addr: int,
    code_limit_len: int,
    label: str,
) -> None:
    """Validate RP2040 Cortex-M0+ initial MSP and Thumb Reset_Handler at vector_addr."""
    initial_msp, entry_point = struct.unpack_from("<2I", vector_region, 0)
    if not (PU_RP2040_SRAM_BASE < initial_msp <= PU_RP2040_SRAM_TOP) or (initial_msp & 0x7) != 0:
        raise UF2PackageError(
            f"{label}: invalid initial MSP 0x{initial_msp:08X}; expected 8-byte aligned SRAM in "
            f"(0x{PU_RP2040_SRAM_BASE:08X}, 0x{PU_RP2040_SRAM_TOP:08X}]"
        )
    if (entry_point & 1) == 0:
        raise UF2PackageError(f"{label}: entry point 0x{entry_point:08X} lacks Cortex-M0+ Thumb bit 0")

    entry_addr = entry_point & ~1
    if not (vector_addr + 8 <= entry_addr <= vector_addr + code_limit_len - 2):
        raise UF2PackageError(
            f"{label}: entry point 0x{entry_point:08X} is outside executable region "
            f"[0x{vector_addr + 8:08X}, 0x{vector_addr + code_limit_len:08X})"
        )

    entry_off = entry_addr - vector_addr
    if vector_region[entry_off : entry_off + 2] == b"\xFF\xFF":
        raise UF2PackageError(f"{label}: entry point 0x{entry_point:08X} points to erased flash (0xFFFF)")


def _validate_bootloader_image(boot_bytes: bytes | bytearray, *, part_base: int, part_size: int) -> None:
    raw = bytes(boot_bytes)
    if len(raw) < PU_IMAGE_HEADER_SIZE + 16:
        raise UF2PackageError(
            f"bootloader image at 0x{part_base:08X} is too small ({len(raw)} bytes) for boot2 + vectors"
        )
    if len(raw) > part_size:
        raise UF2PackageError(
            f"bootloader image at 0x{part_base:08X} ({len(raw)} bytes) exceeds partition size ({part_size} bytes)"
        )
    vec_region = raw[PU_IMAGE_HEADER_SIZE:]
    _validate_cortex_m0_vectors(
        vec_region,
        vector_addr=part_base + PU_IMAGE_HEADER_SIZE,
        code_limit_len=len(vec_region),
        label="bootloader",
    )


def stamp_slot_descriptor(
    slot_bytes: bytes | bytearray,
    *,
    slot_addr: int,
    slot_size: int,
    image_version: int = 1,
) -> bytes:
    """Validate Cortex-M0+ vectors at +0x100 and stamp pu_image_descriptor_t at +0x000."""
    raw = bytes(slot_bytes)
    if len(raw) < PU_IMAGE_HEADER_SIZE + 16:
        raise UF2PackageError(
            f"slot image at 0x{slot_addr:08X} is too small ({len(raw)} bytes) for header + vectors"
        )

    unpadded = raw[PU_IMAGE_HEADER_SIZE:]
    padded = unpadded + b"\xFF" if (len(unpadded) % 2 != 0) else unpadded
    if PU_IMAGE_HEADER_SIZE + len(padded) > slot_size:
        raise UF2PackageError(
            f"slot image at 0x{slot_addr:08X} ({PU_IMAGE_HEADER_SIZE + len(padded)} bytes) "
            f"exceeds partition size ({slot_size} bytes)"
        )

    _validate_cortex_m0_vectors(
        unpadded,
        vector_addr=slot_addr + PU_IMAGE_HEADER_SIZE,
        code_limit_len=len(unpadded),
        label="slot",
    )

    payload_crc = zlib.crc32(padded) & 0xFFFFFFFF
    desc_body = struct.pack(
        "<7I",
        PU_IMAGE_DESC_MAGIC,
        PU_IMAGE_DESC_VERSION,
        image_version & 0xFFFFFFFF,
        slot_addr,
        len(padded),
        payload_crc,
        0,
    )
    header_crc = zlib.crc32(desc_body) & 0xFFFFFFFF
    header_page = (desc_body + struct.pack("<I", header_crc)).ljust(PU_IMAGE_HEADER_SIZE, b"\xFF")
    return header_page + padded


def _load_partition_pages(
    part_name: str,
    source: bytes | bytearray | str | Path,
    *,
    part_info: dict,
    layout: dict,
    stamp_descriptor: bool,
    validate_bootloader: bool = False,
    image_version: int,
) -> dict[int, bytes]:
    if not part_info.get("executable", False):
        raise UF2PackageError(f"partition {part_name!r} is non-executable and cannot be packaged")

    flash_base = layout["flash_base"]
    page_size = layout["page_size"]
    part_base = flash_base + part_info["offset"]
    part_size = part_info["size"]
    part_end = part_base + part_size

    if isinstance(source, (str, Path)):
        path = Path(source)
        suffix = path.suffix.lower()
        if suffix not in (".bin", ".uf2"):
            raise UF2PackageError(
                f"{part_name}: unsupported file extension {path.suffix!r}; expected '.bin' or '.uf2'"
            )
        data = path.read_bytes()
        is_uf2 = suffix == ".uf2"
    elif isinstance(source, (bytes, bytearray)):
        data = bytes(source)
        is_uf2 = False
    else:
        raise UF2PackageError(f"unsupported input type for {part_name}: {type(source).__name__}")

    if not data:
        raise UF2PackageError(f"{part_name}: input image must not be empty")
    if data.startswith(b"\x7fELF"):
        raise UF2PackageError(
            f"{part_name}: ELF inputs are not supported; provide a raw .bin or .uf2 image"
        )

    if is_uf2:
        pages = uf2_to_pages(data, layout=layout)
        for addr in pages:
            if not (part_base <= addr < part_end):
                raise UF2PackageError(
                    f"{part_name}: UF2 block at 0x{addr:08X} is outside partition "
                    f"[0x{part_base:08X}, 0x{part_end:08X})"
                )
        min_addr = min(pages)
        max_addr = max(pages)
        if min_addr != part_base:
            raise UF2PackageError(
                f"{part_name}: UF2 image must start at partition base 0x{part_base:08X}, "
                f"got 0x{min_addr:08X}"
            )
        if validate_bootloader and part_name == "bootloader":
            contiguous_len = (max_addr - part_base) + page_size
            assembled = bytearray(b"\xFF" * contiguous_len)
            for addr, pg in pages.items():
                off = addr - part_base
                assembled[off : off + page_size] = pg
            _validate_bootloader_image(assembled, part_base=part_base, part_size=part_size)
        if stamp_descriptor and part_name in ("app_slot_a", "app_slot_b"):
            contiguous_len = (max_addr - part_base) + page_size
            assembled = bytearray(b"\xFF" * contiguous_len)
            for addr, pg in pages.items():
                off = addr - part_base
                assembled[off : off + page_size] = pg
            stamped = stamp_slot_descriptor(
                assembled,
                slot_addr=part_base,
                slot_size=part_size,
                image_version=image_version,
            )
            return {
                part_base + off: stamped[off : off + page_size].ljust(page_size, b"\xFF")
                for off in range(0, len(stamped), page_size)
            }
        return pages

    if len(data) > part_size:
        raise UF2PackageError(
            f"{part_name}: binary size ({len(data)} bytes) exceeds partition size ({part_size} bytes)"
        )

    if validate_bootloader and part_name == "bootloader":
        _validate_bootloader_image(data, part_base=part_base, part_size=part_size)

    if stamp_descriptor and part_name in ("app_slot_a", "app_slot_b"):
        data = stamp_slot_descriptor(
            data,
            slot_addr=part_base,
            slot_size=part_size,
            image_version=image_version,
        )

    return {
        part_base + off: data[off : off + page_size].ljust(page_size, b"\xFF")
        for off in range(0, len(data), page_size)
    }


def pack_unified_uf2(
    bootloader: bytes | bytearray | str | Path | None = None,
    slot_a: bytes | bytearray | str | Path | None = None,
    slot_b: bytes | bytearray | str | Path | None = None,
    *,
    stamp_descriptor: bool = False,
    validate_bootloader: bool | None = None,
    image_version: int = 1,
    layout_path: Path | None = None,
) -> bytes:
    """Pack bootloader and A/B slot binaries into a 4 KiB sector-padded RP2040 UF2."""
    layout = load_and_validate_layout(layout_path or DEFAULT_LAYOUT_PATH)
    flash_base = layout["flash_base"]
    page_size = layout["page_size"]
    sector_size = layout["sector_size"]
    if page_size != UF2_PAYLOAD_BYTES:
        raise UF2PackageError(f"unsupported flash page_size {page_size}; UF2 expects 256")

    parts_by_name = {p["name"]: p for p in layout["partitions"]}
    protected_ranges = [
        (flash_base + p["offset"], flash_base + p["offset"] + p["size"], p["name"])
        for p in layout["partitions"]
        if not p["executable"]
    ]

    requested = [
        ("bootloader", bootloader),
        ("app_slot_a", slot_a),
        ("app_slot_b", slot_b),
    ]
    active_inputs = [(name, src) for name, src in requested if src is not None]
    if not active_inputs:
        raise UF2PackageError("at least one of bootloader, slot_a, or slot_b must be supplied")

    should_validate_bootloader = (
        stamp_descriptor if validate_bootloader is None else validate_bootloader
    )

    flash_pages: dict[int, bytes] = {}
    for part_name, src in active_inputs:
        if part_name not in parts_by_name:
            raise UF2PackageError(f"missing partition {part_name!r} in flash layout")
        part_pages = _load_partition_pages(
            part_name,
            src,
            part_info=parts_by_name[part_name],
            layout=layout,
            stamp_descriptor=stamp_descriptor,
            validate_bootloader=should_validate_bootloader,
            image_version=image_version,
        )
        overlap = flash_pages.keys() & part_pages.keys()
        if overlap:
            bad = min(overlap)
            raise UF2PackageError(f"overlapping page at 0x{bad:08X} while adding {part_name}")
        flash_pages.update(part_pages)

    # Pad EVERY touched 4 KiB erase sector to all 16 x 256-byte pages BEFORE numbering
    # UF2 blocks so RP2040 BOOTSEL's (block_no // 16) sector-erase bitmap maps 1-to-1
    # onto physical 4 KiB flash sectors regardless of USB MSC block delivery order.
    touched_sectors = sorted({(addr // sector_size) * sector_size for addr in flash_pages})
    blank_page = b"\xFF" * page_size

    for sector_addr in touched_sectors:
        sector_end = sector_addr + sector_size
        for prot_start, prot_end, prot_name in protected_ranges:
            if sector_addr < prot_end and sector_end > prot_start:
                raise UF2PackageError(
                    f"touched sector 0x{sector_addr:08X} overlaps protected partition {prot_name!r}"
                )
        for page_addr in range(sector_addr, sector_end, page_size):
            flash_pages.setdefault(page_addr, blank_page)

    ordered_addrs = sorted(flash_pages)
    total_blocks = len(ordered_addrs)
    blocks = [
        _encode_uf2_block(addr, flash_pages[addr], idx, total_blocks)
        for idx, addr in enumerate(ordered_addrs)
    ]
    return b"".join(blocks)


def simulate_rp2040_bootsel_flash(
    uf2_bytes: bytes,
    order: str | Sequence[int] = "forward",
    initial_flash: bytes | bytearray | None = None,
    *,
    seed: int = 2040,
    layout_path: Path | None = None,
) -> bytes:
    """Simulate RP2040 BOOTSEL virtual_disk.c UF2 flashing on physical NOR flash.

    Models the exact RP2040 boot ROM behavior:
      1. Retransmitted/duplicate block_no deliveries are ignored after first write.
      2. Erase tracking bitmap is indexed by (block_no // 16), NOT target_addr // 4096;
         on first touch of (block_no // 16), the 4 KiB sector containing target_addr
         is erased to 0xFF.
      3. Page programming performs bitwise NOR AND (flash[i] &= data[i]).
    """
    layout = load_and_validate_layout(layout_path or DEFAULT_LAYOUT_PATH)
    uf2_to_pages(uf2_bytes, layout=layout)

    flash_base = layout["flash_base"]
    flash_size = layout["flash_size"]
    sector_size = layout["sector_size"]
    page_size = layout["page_size"]
    pages_per_sector = sector_size // page_size

    if initial_flash is None:
        flash = bytearray(b"\xFF" * flash_size)
    else:
        if len(initial_flash) != flash_size:
            raise UF2PackageError(
                f"initial_flash must be exactly {flash_size} bytes, got {len(initial_flash)}"
            )
        flash = bytearray(initial_flash)

    block_count = len(uf2_bytes) // UF2_BLOCK_BYTES

    if isinstance(order, str):
        if order == "forward":
            indices: list[int] = list(range(block_count))
        elif order == "reverse":
            indices = list(range(block_count - 1, -1, -1))
        elif order == "shuffled":
            indices = list(range(block_count))
            random.Random(seed).shuffle(indices)
        elif order in ("duplicate", "shuffled_duplicates"):
            indices = list(range(block_count))
            rng = random.Random(seed)
            rng.shuffle(indices)
            dup_slice = indices[: max(1, block_count // 4)]
            indices = dup_slice + indices + dup_slice
        else:
            raise ValueError(f"unsupported delivery order mode: {order!r}")
    else:
        indices = list(order)

    written_blocks: set[int] = set()
    erased_groups: set[int] = set()

    for idx in indices:
        if not (0 <= idx < block_count):
            raise ValueError(f"transfer block index {idx} out of range [0, {block_count})")
        block = uf2_bytes[idx * UF2_BLOCK_BYTES : (idx + 1) * UF2_BLOCK_BYTES]
        target_addr, _, block_no = struct.unpack_from("<3I", block, 12)

        if block_no in written_blocks:
            continue

        offset = target_addr - flash_base
        erase_group = block_no // pages_per_sector
        if erase_group not in erased_groups:
            sector_off = offset & ~(sector_size - 1)
            flash[sector_off : sector_off + sector_size] = b"\xFF" * sector_size
            erased_groups.add(erase_group)

        payload = block[32 : 32 + page_size]
        cur_page = flash[offset : offset + page_size]
        flash[offset : offset + page_size] = bytes(a & b for a, b in zip(cur_page, payload))
        written_blocks.add(block_no)

    if len(written_blocks) != block_count:
        raise ValueError("simulated transfer did not deliver all UF2 blocks")

    return bytes(flash)


def verify_uf2_against_rom_model(uf2_bytes: bytes, *, layout_path: Path | None = None) -> None:
    """Verify UF2 stream across forward, reverse, shuffled, and duplicate ROM deliveries."""
    layout = load_and_validate_layout(layout_path or DEFAULT_LAYOUT_PATH)
    flash_base = layout["flash_base"]
    flash_size = layout["flash_size"]
    pages = uf2_to_pages(uf2_bytes, layout=layout)

    sentinel_flash = bytearray(b"\x5A" * flash_size)
    expected = bytearray(sentinel_flash)
    for addr, pg in pages.items():
        off = addr - flash_base
        expected[off : off + len(pg)] = pg
    expected_bytes = bytes(expected)

    for mode in ("forward", "reverse", "shuffled", "shuffled_duplicates"):
        actual = simulate_rp2040_bootsel_flash(
            uf2_bytes,
            order=mode,
            initial_flash=sentinel_flash,
            layout_path=layout_path,
        )
        if actual != expected_bytes:
            raise UF2PackageError(f"ROM flash model verification failed under {mode!r} order")


def _atomic_write_bytes(output_path: Path, data: bytes) -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            dir=output_path.parent,
            prefix=f".{output_path.name}.",
            suffix=".tmp",
            delete=False,
        ) as fh:
            tmp_path = Path(fh.name)
            fh.write(data)
            fh.flush()
            os.fsync(fh.fileno())
        os.replace(tmp_path, output_path)
        tmp_path = None
    finally:
        if tmp_path is not None:
            tmp_path.unlink(missing_ok=True)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootloader", type=Path, help="Bootloader .bin or .uf2 path")
    parser.add_argument("--slot-a", dest="slot_a", type=Path, help="Slot A .bin or .uf2 path")
    parser.add_argument("--slot-b", dest="slot_b", type=Path, help="Optional Slot B .bin or .uf2 path")
    parser.add_argument("-o", "--output", type=Path, required=True, help="Output .uf2 file path")
    parser.add_argument(
        "--stamp-descriptor",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="Stamp valid pu_image_descriptor_t CRC32 header into slot binaries (default: enabled)",
    )
    parser.add_argument(
        "--image-version",
        type=int,
        default=1,
        help="Firmware version stamped into pu_image_descriptor_t (default: 1)",
    )
    parser.add_argument(
        "--verify-rom-model",
        action="store_true",
        help="Verify generated UF2 against RP2040 BOOTSEL ROM flash simulation",
    )
    parser.add_argument(
        "--layout",
        type=Path,
        default=DEFAULT_LAYOUT_PATH,
        help="Path to flash_layout.json (default: config/flash_layout.json)",
    )
    args = parser.parse_args(argv)

    inputs = [p for p in (args.bootloader, args.slot_a, args.slot_b) if p is not None]
    out_resolved = args.output.resolve()
    for inp in inputs:
        if inp.resolve() == out_resolved or (
            args.output.exists() and inp.exists() and os.path.samefile(args.output, inp)
        ):
            print("package_uf2.py: error: output must not overwrite an input image", file=sys.stderr)
            return 1

    try:
        uf2_bytes = pack_unified_uf2(
            bootloader=args.bootloader,
            slot_a=args.slot_a,
            slot_b=args.slot_b,
            stamp_descriptor=args.stamp_descriptor,
            image_version=args.image_version,
            layout_path=args.layout,
        )
        if args.verify_rom_model:
            verify_uf2_against_rom_model(uf2_bytes, layout_path=args.layout)
        _atomic_write_bytes(args.output, uf2_bytes)
    except (OSError, ValueError, UF2PackageError) as exc:
        print(f"package_uf2.py: error: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
