"""Model RP2040 BOOTSEL UF2 sector erases and NOR page programming for tests.

This is not a CPU/USB emulator. The erase bitmap is deliberately indexed by
UF2 block number / 16, NOT by target address, matching _write_uf2_page in:
https://github.com/raspberrypi/pico-bootrom/blob/master/bootrom/virtual_disk.c
That distinction is essential when testing sparse multi-image UF2 packages.
"""
import struct

try:
    from . import merge_firmware as uf2
except ImportError:
    import merge_firmware as uf2


def simulate_write(data, initial_flash=None, order=None):
    uf2.uf2_to_bin(data)  # Validate the stream before modeling flash operations.
    size = uf2.FLASH_END - uf2.FLASH_BASE
    flash = bytearray(b'\xff' * size if initial_flash is None else initial_flash)
    if len(flash) != size:
        raise ValueError('expected a complete 2 MiB initial flash image')
    count = len(data) // 512
    written, erased_groups = set(), set()
    for index in range(count) if order is None else order:
        if not 0 <= index < count:
            raise ValueError('invalid transfer block index')
        block = data[index * 512:(index + 1) * 512]
        address, _, number = struct.unpack_from('<3I', block, 12)
        if number in written:
            continue  # Retransmitted blocks do not erase or write a second time.
        offset = address - uf2.FLASH_BASE
        group = number // 16
        if group not in erased_groups:
            sector = offset & ~4095
            flash[sector:sector + 4096] = b'\xff' * 4096
            erased_groups.add(group)
        # Programming NOR flash can only clear bits; a missing erase matters.
        flash[offset:offset + 256] = bytes(old & new for old, new in
            zip(flash[offset:offset + 256], block[32:288]))
        written.add(number)
    if len(written) != count:
        raise ValueError('transfer did not deliver every UF2 block')
    return flash
