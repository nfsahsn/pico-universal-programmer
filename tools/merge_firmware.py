#!/usr/bin/env python3
"""Package slot-linked RP2040 images without relocating code or erasing settings.

A complete package requires all four executables. --allow-partial is an explicit
maintenance/development operation; it does not produce a qualified factory image.
Only original-Pico flash UF2 blocks (256-byte payload, RP2040 family) are accepted.
"""

import argparse
import os
from pathlib import Path
import struct
import sys
import tempfile
import zlib

try:
    from .flash_layout import load_layout
except ImportError:  # Direct script invocation.
    from flash_layout import load_layout

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY = 0x00002000
RP2040_FAMILY_ID = 0xE48BFF56
BLOCK_SIZE = 256
SECTOR_SIZE = 4096
_LAYOUT = load_layout()
FLASH_BASE = _LAYOUT["flash_base"]
FLASH_END = FLASH_BASE + _LAYOUT["flash_size"]
PARTITIONS = {p["name"]: p for p in _LAYOUT["partitions"] if p["executable"]}
SLOTS = {name: FLASH_BASE + p["offset"] for name, p in PARTITIONS.items()}
MODES = {"cmsis_dap": 0, "blackmagic": 1, "picorvd": 2}
IMAGE_DESCRIPTOR_MAGIC = 0x5550494D
IMAGE_DESCRIPTOR_VERSION = 1


class FirmwareError(ValueError):
    """An input cannot safely be packaged for this flash layout."""


def uf2_to_bin(uf2_data):
    """Return address -> page after validating a complete RP2040 flash UF2."""
    if not uf2_data or len(uf2_data) % 512:
        raise FirmwareError("UF2 must contain complete 512-byte blocks")
    count = len(uf2_data) // 512
    chunks, numbers = {}, set()
    for offset in range(0, len(uf2_data), 512):
        block = uf2_data[offset:offset + 512]
        magic0, magic1, flags, addr, length, number, total, family = struct.unpack_from("<8I", block)
        footer, = struct.unpack_from("<I", block, 508)
        if (magic0, magic1, footer) != (UF2_MAGIC_START0, UF2_MAGIC_START1, UF2_MAGIC_END):
            raise FirmwareError(f"bad UF2 magic at file offset {offset}")
        if flags != UF2_FLAG_FAMILY or family != RP2040_FAMILY_ID:
            raise FirmwareError("only RP2040 family-tagged flash UF2 is supported")
        if length != BLOCK_SIZE or addr % BLOCK_SIZE:
            raise FirmwareError("RP2040 UF2 requires aligned 256-byte flash pages")
        if not FLASH_BASE <= addr <= FLASH_END - BLOCK_SIZE:
            raise FirmwareError(f"UF2 address 0x{addr:08x} is outside physical flash")
        if total != count or number >= count or number in numbers:
            raise FirmwareError("inconsistent, missing or duplicate UF2 block numbers")
        if addr in chunks:
            raise FirmwareError(f"duplicate UF2 target address 0x{addr:08x}")
        numbers.add(number)
        chunks[addr] = block[32:32 + BLOCK_SIZE]
    return chunks


def create_uf2_block(target_addr, data, block_no, total_blocks):
    """Encode one validated physical flash page."""
    if len(data) != BLOCK_SIZE or target_addr % BLOCK_SIZE:
        raise FirmwareError("output requires aligned 256-byte pages")
    if not FLASH_BASE <= target_addr <= FLASH_END - BLOCK_SIZE:
        raise FirmwareError("output address is outside physical flash")
    if not 0 <= block_no < total_blocks <= 0xFFFFFFFF:
        raise FirmwareError("invalid output block numbering")
    header = struct.pack("<8I", UF2_MAGIC_START0, UF2_MAGIC_START1,
                         UF2_FLAG_FAMILY, target_addr, BLOCK_SIZE,
                         block_no, total_blocks, RP2040_FAMILY_ID)
    return header + data + bytes(476 - BLOCK_SIZE) + struct.pack("<I", UF2_MAGIC_END)


def validate_image(name, chunks):
    """Enforce partition ownership and basic linked-vector sanity.

    This is not proof that every absolute reference was correctly linked. A
    reproducible source build and runtime qualification are still required.
    """
    if name not in PARTITIONS:
        raise FirmwareError(f"unknown executable partition: {name}")
    base = SLOTS[name]
    end = base + PARTITIONS[name]["size"] - (BLOCK_SIZE if name in MODES else 0)
    if not chunks:
        raise FirmwareError(f"{name}: empty image")
    if any(addr < base or addr + len(page) > end for addr, page in chunks.items()):
        raise FirmwareError(f"{name}: image is outside its partition [0x{base:08x}, 0x{end:08x}); "
                            "rebuild with the correct linker origin, do not rebase UF2 addresses")
    addresses = sorted(chunks)
    if addresses != list(range(base, addresses[-1] + BLOCK_SIZE, BLOCK_SIZE)):
        raise FirmwareError(f"{name}: image must start at its slot base and contain no missing pages")
    if base + BLOCK_SIZE not in chunks:
        raise FirmwareError(f"{name}: missing vector table at slot + 0x100")
    msp, entry = struct.unpack_from("<2I", chunks[base + BLOCK_SIZE])
    if not 0x20000000 < msp <= 0x20042000 or msp % 8:
        raise FirmwareError(f"{name}: initial stack pointer is not aligned within RP2040 SRAM")
    if not entry & 1 or not base + BLOCK_SIZE <= (entry & ~1) < end:
        raise FirmwareError(f"{name}: reset vector is not Thumb code inside its linked slot")
    entry_page = (entry & ~1) // BLOCK_SIZE * BLOCK_SIZE
    if entry_page not in chunks:
        raise FirmwareError(f"{name}: reset vector points outside supplied image data")
    entry_offset = (entry & ~1) - entry_page
    if chunks[entry_page][entry_offset:entry_offset + 2] == b"\xff\xff":
        raise FirmwareError(f"{name}: reset vector points at erased flash")


def image_descriptor(name, chunks):
    """Integrity trailer for a previously validated contiguous probe image."""
    data = b''.join(chunks[addr] for addr in sorted(chunks))
    header = struct.pack('<7I', IMAGE_DESCRIPTOR_MAGIC, IMAGE_DESCRIPTOR_VERSION,
                         SLOTS[name], len(data), zlib.crc32(data), MODES[name], 0)
    return (header + struct.pack('<I', zlib.crc32(header))).ljust(BLOCK_SIZE, b'\xff')


def read_image(name, path):
    path = Path(path)
    data = path.read_bytes()
    if path.suffix.lower() == ".uf2":
        chunks = uf2_to_bin(data)
    elif path.suffix.lower() == ".bin":
        capacity = PARTITIONS[name]["size"] - (BLOCK_SIZE if name in MODES else 0)
        if not data or len(data) > capacity:
            raise FirmwareError(f"{name}: empty or oversized binary")
        base = SLOTS[name]
        chunks = {base + offset: data[offset:offset + BLOCK_SIZE].ljust(BLOCK_SIZE, b"\xff")
                  for offset in range(0, len(data), BLOCK_SIZE)}
    else:
        raise FirmwareError(f"{path}: expected .uf2 or slot-linked .bin")
    trailer = None
    if name in MODES:
        trailer_address = SLOTS[name] + PARTITIONS[name]["size"] - BLOCK_SIZE
        trailer = chunks.pop(trailer_address, None)
        if trailer is not None:
            magic, version, base, length, _, mode, flags, header_crc = struct.unpack_from('<8I', trailer)
            if (magic != IMAGE_DESCRIPTOR_MAGIC or version != IMAGE_DESCRIPTOR_VERSION or
                    base != SLOTS[name] or mode != MODES[name] or flags != 0 or
                    header_crc != zlib.crc32(trailer[:28]) or length < 512 or
                    length % BLOCK_SIZE or length > PARTITIONS[name]['size'] - BLOCK_SIZE):
                raise FirmwareError(f"{name}: invalid integrity descriptor or corrupted image")
            # A sealed UF2 contains erase-sector padding beyond the executable
            # length, including the otherwise empty part of the trailer sector.
            # Never silently discard other payload or addresses outside the slot.
            for address in list(chunks):
                if not base <= address < trailer_address:
                    raise FirmwareError(f"{name}: image is outside its partition")
                if address >= base + length:
                    if chunks[address] != b'\xff' * BLOCK_SIZE:
                        raise FirmwareError(f"{name}: non-erased padding or corrupted image")
                    del chunks[address]
    validate_image(name, chunks)
    if trailer is not None and trailer != image_descriptor(name, chunks):
        raise FirmwareError(f"{name}: invalid integrity descriptor or corrupted image")
    return chunks


def merge_and_export(images, output_uf2_path, *, allow_partial=False):
    """Validate everything before atomically replacing the destination UF2."""
    unknown = set(images) - set(PARTITIONS)
    if unknown:
        raise FirmwareError(f"unknown partitions: {', '.join(sorted(unknown))}")
    provided = {name: Path(path) for name, path in images.items() if path}
    if not provided:
        raise FirmwareError("no input images supplied")
    missing = set(PARTITIONS) - set(provided)
    if missing and not allow_partial:
        raise FirmwareError(f"complete package requires: {', '.join(sorted(missing))}; "
                            "use --allow-partial only for deliberate partial updates")
    output = Path(output_uf2_path)
    if any(path.resolve() == output.resolve() or
           (output.exists() and path.exists() and os.path.samefile(output, path))
           for path in provided.values()):
        raise FirmwareError("output must not overwrite an input image")
    flash_map = {}
    for name, path in provided.items():
        chunks = read_image(name, path)
        if name in MODES:
            descriptor = image_descriptor(name, chunks)
            chunks[SLOTS[name] + PARTITIONS[name]["size"] - BLOCK_SIZE] = descriptor
        if flash_map.keys() & chunks.keys():
            raise FirmwareError("input images overlap")
        flash_map.update(chunks)
    # RP2040 BOOTSEL tracks erased sectors by UF2 block_no / 16, not by
    # target_addr / 4096. Sparse images must therefore occupy complete groups
    # of 16 pages for every touched sector; otherwise later blocks can erase
    # earlier code or skip erasing a different sector. Fill only touched sectors
    # so neither settings bank nor other untouched partitions are erased.
    # Raspberry Pi's elf2uf2 implements the same sector-padding workaround.
    for sector in {address // SECTOR_SIZE * SECTOR_SIZE for address in flash_map}:
        for address in range(sector, sector + SECTOR_SIZE, BLOCK_SIZE):
            flash_map.setdefault(address, b'\xff' * BLOCK_SIZE)
    addresses = sorted(flash_map)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=output.parent, prefix=f".{output.name}.",
                                         suffix=".tmp", delete=False) as stream:
            temporary = Path(stream.name)
            for number, addr in enumerate(addresses):
                stream.write(create_uf2_block(addr, flash_map[addr], number, len(addresses)))
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, output)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return True


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootloader", help="slot-linked supervisor .uf2 or .bin")
    parser.add_argument("--cmsis", help="slot-linked CMSIS-DAP .uf2 or .bin")
    parser.add_argument("--bmp", help="slot-linked Black Magic .uf2 or .bin")
    parser.add_argument("--picorvd", help="slot-linked PicoRVD .uf2 or .bin")
    parser.add_argument("--allow-partial", action="store_true", help="allow an explicitly incomplete update")
    parser.add_argument("-o", "--output", default="pico_universal_programmer_full.uf2")
    args = parser.parse_args(argv)
    images = dict(bootloader=args.bootloader, cmsis_dap=args.cmsis,
                  blackmagic=args.bmp, picorvd=args.picorvd)
    try:
        merge_and_export(images, args.output, allow_partial=args.allow_partial)
    except (OSError, FirmwareError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"Wrote {'partial update' if args.allow_partial else 'complete package'}: {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
