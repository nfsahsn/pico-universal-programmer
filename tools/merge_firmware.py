#!/usr/bin/env python3
"""
merge_firmware.py - Packs multiple firmware binaries into a single RP2040 UF2 file.

Partitions:
  - Bootloader:     0x10000000 (Offset 0x00000000)
  - Slot 1 (CMSIS): 0x10010000 (Offset 0x00010000)
  - Slot 2 (BMP):   0x10090000 (Offset 0x00090000)
  - Slot 3 (PicoRVD): 0x10110000 (Offset 0x00110000)
"""

import struct
import sys
import os
import argparse

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END    = 0x0AB16F30
UF2_FLAG_FAMILY  = 0x00002000
RP2040_FAMILY_ID = 0xE4849208
FLASH_BASE       = 0x10000000
BLOCK_SIZE       = 256

SLOTS = {
    "bootloader": 0x10000000,
    "cmsis_dap":   0x10010000,
    "blackmagic":  0x10090000,
    "picorvd":     0x10110000,
}

def uf2_to_bin(uf2_data):
    """Extracts raw binary and base address from a UF2 file."""
    blocks = len(uf2_data) // 512
    flash_chunks = {}
    for i in range(blocks):
        block = uf2_data[i * 512 : (i + 1) * 512]
        magic0, magic1, flags, target_addr, payload_len, block_no, total_blocks, family_id = struct.unpack(
            "<IIIIIIII", block[:32]
        )
        if magic0 != UF2_MAGIC_START0 or magic1 != UF2_MAGIC_START1:
            continue
        data = block[32 : 32 + payload_len]
        flash_chunks[target_addr] = data
    return flash_chunks

def create_uf2_block(target_addr, data, block_no, total_blocks):
    """Constructs a single 512-byte UF2 block."""
    header = struct.pack(
        "<IIIIIIII",
        UF2_MAGIC_START0,
        UF2_MAGIC_START1,
        UF2_FLAG_FAMILY,
        target_addr,
        len(data),
        block_no,
        total_blocks,
        RP2040_FAMILY_ID,
    )
    padding = b"\x00" * (476 - len(data))
    footer = struct.pack("<I", UF2_MAGIC_END)
    return header + data + padding + footer

def merge_and_export(images, output_uf2_path):
    """Takes a dict of {target_addr: raw_bytes} and generates a unified UF2 file."""
    flash_map = {}

    for name, path in images.items():
        if not path or not os.path.exists(path):
            print(f"[-] Skipping {name}: file not found ({path})")
            continue

        with open(path, "rb") as f:
            content = f.read()

        target_base = SLOTS.get(name, 0x10000000)

        if path.endswith(".uf2"):
            chunks = uf2_to_bin(content)
            # Rebase chunks if necessary
            for addr, chunk in chunks.items():
                flash_map[addr] = chunk
            print(f"[+] Added UF2 {name} ({len(chunks)} blocks)")
        else:
            # Raw binary (.bin)
            for offset in range(0, len(content), BLOCK_SIZE):
                chunk = content[offset : offset + BLOCK_SIZE]
                flash_map[target_base + offset] = chunk
            print(f"[+] Added BIN {name} ({len(content)} bytes at 0x{target_base:08X})")

    if not flash_map:
        print("[!] Error: No valid image data to write.")
        return False

    sorted_addrs = sorted(flash_map.keys())
    total_blocks = len(sorted_addrs)

    print(f"[*] Packaging {total_blocks} blocks into {output_uf2_path}...")
    with open(output_uf2_path, "wb") as out_f:
        for block_no, addr in enumerate(sorted_addrs):
            data = flash_map[addr]
            block = create_uf2_block(addr, data, block_no, total_blocks)
            out_f.write(block)

    print(f"[SUCCESS] Unified UF2 successfully created: {output_uf2_path}")
    return True

def main():
    parser = argparse.ArgumentParser(description="Merge multiple RP2040 binaries into a single UF2")
    parser.add_argument("--bootloader", help="Path to bootloader .uf2 or .bin", default="")
    parser.add_argument("--cmsis", help="Path to CMSIS-DAP .uf2 or .bin", default="")
    parser.add_argument("--bmp", help="Path to Black Magic Probe .uf2 or .bin", default="")
    parser.add_argument("--picorvd", help="Path to PicoRVD .uf2 or .bin", default="")
    parser.add_argument("-o", "--output", help="Output .uf2 file path", default="pico_universal_programmer_full.uf2")

    args = parser.parse_args()
    images = {
        "bootloader": args.bootloader,
        "cmsis_dap": args.cmsis,
        "blackmagic": args.bmp,
        "picorvd": args.picorvd,
    }
    merge_and_export(images, args.output)

if __name__ == "__main__":
    main()
