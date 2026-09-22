"""Host regression tests: no SDK, board or third-party Python dependencies."""

import json
import random
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from tools import flash_layout
from tools import merge_firmware as uf2
from tools.bootrom_model import simulate_write


def image(base, size=768):
    data = bytearray(size)
    struct.pack_into('<2I', data, 256, 0x20042000, base + 0x201)
    return bytes(data)


def encode(base, data):
    pages = [data[i:i + 256].ljust(256, b'\xff') for i in range(0, len(data), 256)]
    return b''.join(uf2.create_uf2_block(base + i * 256, page, i, len(pages))
                    for i, page in enumerate(pages))


class PackagingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.output = self.root / 'output.uf2'

    def write(self, name, data):
        path = self.root / name
        path.write_bytes(data)
        return path

    def inputs(self, suffix='.bin'):
        return {name: self.write(name + suffix, encode(base, image(base)) if suffix == '.uf2'
                                 else image(base)) for name, base in uf2.SLOTS.items()}

    def test_full_binary_package_round_trip_and_config_preservation(self):
        uf2.merge_and_export(self.inputs(), self.output)
        pages = uf2.uf2_to_bin(self.output.read_bytes())
        self.assertEqual(len(pages), 7 * 16)  # Four image sectors + three trailer sectors.
        for base in uf2.SLOTS.values():
            self.assertEqual(b''.join(pages[base + i] for i in (0, 256, 512)), image(base))
        self.assertNotIn(0x1000f000, pages)
        self.assertNotIn(0x10190000, pages)

    def test_bootsel_write_preserves_images_and_untouched_sectors(self):
        inputs = {name: self.write(name + '.bin', image(base, size))
                  for (name, base), size in zip(uf2.SLOTS.items(), (768, 5632, 8448, 1280))}
        uf2.merge_and_export(inputs, self.output)
        data = self.output.read_bytes()
        pages = uf2.uf2_to_bin(data)
        count = len(data) // 512
        shuffled = list(range(count))
        random.Random(2040).shuffle(shuffled)
        for value in (0x00, 0xa5, 0xff):
            initial = bytes([value]) * (uf2.FLASH_END - uf2.FLASH_BASE)
            expected = bytearray(initial)
            for address, page in pages.items():
                offset = address - uf2.FLASH_BASE
                expected[offset:offset + 256] = page
            for order in (range(count), range(count - 1, -1, -1), shuffled + shuffled[:16]):
                actual = simulate_write(data, initial, order)
                self.assertEqual(actual, expected)
                for name, path in inputs.items():
                    offset = uf2.SLOTS[name] - uf2.FLASH_BASE
                    self.assertEqual(actual[offset:offset + path.stat().st_size], path.read_bytes())

    def test_previous_sparse_encoding_corrupts_images_during_bootsel_write(self):
        # Recreate the old encoder: sparse pages, including trailers, numbered
        # consecutively without sector padding. The parser alone accepts this.
        pages = {}
        for name, path in self.inputs().items():
            chunks = uf2.read_image(name, path)
            pages.update(chunks)
            if name in uf2.MODES:
                pages[uf2.SLOTS[name] + uf2.PARTITIONS[name]['size'] - 256] = uf2.image_descriptor(name, chunks)
        stream = b''.join(uf2.create_uf2_block(address, pages[address], i, len(pages))
                          for i, address in enumerate(sorted(pages)))
        self.assertEqual(uf2.uf2_to_bin(stream), pages)
        actual = simulate_write(stream, bytes(uf2.FLASH_END - uf2.FLASH_BASE))
        for name in uf2.MODES:
            offset = uf2.SLOTS[name] - uf2.FLASH_BASE
            self.assertNotEqual(actual[offset:offset + 768], image(uf2.SLOTS[name]))

    def test_partial_bootsel_write_preserves_other_slots_and_settings(self):
        name = 'blackmagic'
        uf2.merge_and_export({name: self.inputs()[name]}, self.output, allow_partial=True)
        initial = bytes([0xa5]) * (uf2.FLASH_END - uf2.FLASH_BASE)
        actual = simulate_write(self.output.read_bytes(), initial)
        start = uf2.SLOTS[name] - uf2.FLASH_BASE
        end = start + uf2.PARTITIONS[name]['size']
        self.assertEqual(actual[:start], initial[:start])
        self.assertEqual(actual[end:], initial[end:])
        self.assertEqual(actual[start:start + 768], image(uf2.SLOTS[name]))

    def test_full_uf2_package_is_deterministic(self):
        inputs = self.inputs('.uf2')
        uf2.merge_and_export(inputs, self.output)
        first = self.output.read_bytes()
        uf2.merge_and_export(dict(reversed(list(inputs.items()))), self.output)
        self.assertEqual(first, self.output.read_bytes())

    def test_sealed_probe_round_trip_and_corruption_rejection(self):
        name = 'cmsis_dap'
        source = self.inputs()[name]
        uf2.merge_and_export({name: source}, self.output, allow_partial=True)
        self.assertEqual(uf2.read_image(name, source), uf2.read_image(name, self.output))
        good = self.output.read_bytes()
        for offset in [32, 512 + 32 + 40, 3 * 512 + 32, len(good) - 512 + 32 + 16]:
            corrupted = bytearray(good)
            corrupted[offset] ^= 1
            self.output.write_bytes(corrupted)
            with self.subTest(offset=offset), self.assertRaisesRegex(uf2.FirmwareError, 'corrupted image'):
                uf2.read_image(name, self.output)

    def test_probe_descriptor_page_is_reserved(self):
        name = 'cmsis_dap'
        path = self.write('too-large.bin', image(uf2.SLOTS[name], uf2.PARTITIONS[name]['size']))
        with self.assertRaisesRegex(uf2.FirmwareError, 'oversized'):
            uf2.read_image(name, path)

    def test_partial_requires_explicit_opt_in(self):
        inputs = {'cmsis_dap': self.inputs()['cmsis_dap']}
        with self.assertRaisesRegex(uf2.FirmwareError, 'complete package'):
            uf2.merge_and_export(inputs, self.output)
        uf2.merge_and_export(inputs, self.output, allow_partial=True)
        self.assertEqual(min(uf2.uf2_to_bin(self.output.read_bytes())), uf2.SLOTS['cmsis_dap'])

    def test_standalone_probe_cannot_overwrite_supervisor(self):
        inputs = self.inputs()
        inputs['cmsis_dap'] = self.write('standalone.uf2', encode(uf2.FLASH_BASE, image(uf2.FLASH_BASE)))
        self.output.write_bytes(b'previous release')
        with self.assertRaisesRegex(uf2.FirmwareError, 'outside its partition'):
            uf2.merge_and_export(inputs, self.output)
        self.assertEqual(self.output.read_bytes(), b'previous release')

    def test_missing_input_fails_without_replacing_output(self):
        inputs = self.inputs()
        inputs['blackmagic'].unlink()
        self.output.write_bytes(b'previous release')
        with self.assertRaises(FileNotFoundError):
            uf2.merge_and_export(inputs, self.output)
        self.assertEqual(self.output.read_bytes(), b'previous release')

    def test_output_cannot_alias_input(self):
        inputs = self.inputs()
        with self.assertRaisesRegex(uf2.FirmwareError, 'overwrite an input'):
            uf2.merge_and_export(inputs, inputs['bootloader'])
        self.output.hardlink_to(inputs['bootloader'])
        with self.assertRaisesRegex(uf2.FirmwareError, 'overwrite an input'):
            uf2.merge_and_export(inputs, self.output)

    def test_atomic_replace_failure_preserves_previous_and_cleans_temporary(self):
        inputs = self.inputs()
        self.output.write_bytes(b'previous release')
        before = set(self.root.iterdir())
        with mock.patch.object(uf2.os, 'replace', side_effect=OSError('simulated failure')):
            with self.assertRaises(OSError):
                uf2.merge_and_export(inputs, self.output)
        self.assertEqual(self.output.read_bytes(), b'previous release')
        self.assertEqual(set(self.root.iterdir()), before)

    def test_binary_padding_is_erased_value(self):
        data = image(uf2.FLASH_BASE, 513)
        path = self.write('short.bin', data)
        pages = uf2.read_image('bootloader', path)
        self.assertEqual(pages[uf2.FLASH_BASE + 512][1:], b'\xff' * 255)

    def test_oversized_binary_rejected(self):
        path = self.write('large.bin', bytes(uf2.PARTITIONS['bootloader']['size'] + 1))
        with self.assertRaisesRegex(uf2.FirmwareError, 'oversized'):
            uf2.read_image('bootloader', path)

    def test_unknown_partition_and_extension_rejected(self):
        with self.assertRaisesRegex(uf2.FirmwareError, 'unknown partitions'):
            uf2.merge_and_export({'typo': 'x'}, self.output, allow_partial=True)
        path = self.write('firmware.hex', image(uf2.FLASH_BASE))
        with self.assertRaisesRegex(uf2.FirmwareError, 'expected .uf2 or'):
            uf2.read_image('bootloader', path)

    def test_missing_vector_table_rejected(self):
        with self.assertRaisesRegex(uf2.FirmwareError, 'missing vector table'):
            uf2.validate_image('bootloader', {uf2.FLASH_BASE: bytes(256)})

    def test_invalid_vectors_rejected(self):
        base = uf2.FLASH_BASE
        cases = [(0x20000000, base + 0x201), (0x20042008, base + 0x201),
                 (0x20041004, base + 0x201), (0xffffffff, 0xffffffff),
                 (0x20042000, base + 0x200), (0x20042000, base + 1),
                 (0x20042000, base + 0xf001), (0x20042000, base + 0x1001)]
        for msp, entry in cases:
            with self.subTest(msp=hex(msp), entry=hex(entry)):
                data = bytearray(image(base))
                struct.pack_into('<2I', data, 256, msp, entry)
                with self.assertRaises(uf2.FirmwareError):
                    uf2.validate_image('bootloader', uf2.uf2_to_bin(encode(base, data)))

    def test_erased_reset_handler_rejected(self):
        data = bytearray(image(uf2.FLASH_BASE))
        data[512:514] = b'\xff\xff'
        with self.assertRaisesRegex(uf2.FirmwareError, 'erased flash'):
            uf2.validate_image('bootloader', uf2.uf2_to_bin(encode(uf2.FLASH_BASE, data)))

    def test_slot_overflow_and_missing_pages_rejected(self):
        pages = uf2.uf2_to_bin(encode(uf2.FLASH_BASE, image(uf2.FLASH_BASE)))
        pages[uf2.FLASH_BASE + 0xf000] = bytes(256)
        with self.assertRaisesRegex(uf2.FirmwareError, 'outside its partition'):
            uf2.validate_image('bootloader', pages)
        del pages[uf2.FLASH_BASE + 0xf000]
        pages[uf2.FLASH_BASE + 1024] = bytes(256)
        with self.assertRaisesRegex(uf2.FirmwareError, 'missing pages'):
            uf2.validate_image('bootloader', pages)

    def test_cli_errors_have_nonzero_exit(self):
        for args in [[], ['--allow-partial', '--bootloader', str(self.root / 'missing.bin')]]:
            result = subprocess.run([sys.executable, str(flash_layout.ROOT / 'tools/merge_firmware.py'),
                                     '-o', str(self.output), *args], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('error:', result.stderr)
            self.assertFalse(self.output.exists())


class ParserTests(unittest.TestCase):
    def test_out_of_order_blocks_accepted(self):
        data = encode(uf2.FLASH_BASE, image(uf2.FLASH_BASE))
        shuffled = data[1024:] + data[:1024]
        self.assertEqual(uf2.uf2_to_bin(data), uf2.uf2_to_bin(shuffled))

    def test_invalid_block_fields_rejected(self):
        # Offset in first block, replacement value. Includes optional UF2 flags
        # that this RP2040 flash-only packager intentionally does not interpret.
        fields = [(0, 0), (4, 0), (508, 0), (8, 0), (8, 0x2001), (28, 0),
                  (16, 0), (16, 255), (16, 476), (16, 0xffffffff),
                  (12, uf2.FLASH_BASE + 1), (12, 0x20000000), (12, uf2.FLASH_END),
                  (20, 3), (24, 0), (24, 4)]
        for offset, value in fields:
            with self.subTest(offset=offset, value=value):
                data = bytearray(encode(uf2.FLASH_BASE, image(uf2.FLASH_BASE)))
                struct.pack_into('<I', data, offset, value)
                with self.assertRaises(uf2.FirmwareError):
                    uf2.uf2_to_bin(data)

    def test_truncated_empty_and_duplicate_blocks_rejected(self):
        data = encode(uf2.FLASH_BASE, image(uf2.FLASH_BASE))
        for invalid in [b'', data[:-1], data[:512], data + b'x', data[:512] * 3]:
            with self.subTest(length=len(invalid)):
                with self.assertRaises(uf2.FirmwareError):
                    uf2.uf2_to_bin(invalid)
        duplicate_address = bytearray(data)
        struct.pack_into('<I', duplicate_address, 512 + 12, uf2.FLASH_BASE)
        with self.assertRaisesRegex(uf2.FirmwareError, 'duplicate UF2 target'):
            uf2.uf2_to_bin(duplicate_address)

    def test_encoder_rejects_invalid_pages(self):
        for addr, page, num, count in [(uf2.FLASH_BASE, b'', 0, 1),
                                      (uf2.FLASH_BASE + 1, bytes(256), 0, 1),
                                      (uf2.FLASH_END, bytes(256), 0, 1),
                                      (uf2.FLASH_BASE, bytes(256), 1, 1)]:
            with self.assertRaises(uf2.FirmwareError):
                uf2.create_uf2_block(addr, page, num, count)


class LayoutTests(unittest.TestCase):
    def test_generated_header_current(self):
        self.assertEqual(flash_layout.HEADER_PATH.read_text(),
                         flash_layout.render_header(flash_layout.load_layout()))

    def test_invalid_layout_rejected(self):
        for mutation in ['overlap', 'overflow', 'duplicate', 'unaligned', 'boot_origin', 'config_executable']:
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as td:
                layout = flash_layout.load_layout()
                if mutation == 'overlap': layout['partitions'][1]['offset'] = 0
                if mutation == 'overflow': layout['partitions'][-1]['size'] = 0x200000
                if mutation == 'duplicate': layout['partitions'][-1]['name'] = 'bootloader'
                if mutation == 'unaligned': layout['partitions'][-1]['size'] = 1
                if mutation == 'boot_origin': layout['partitions'][0]['offset'] = 4096
                if mutation == 'config_executable': layout['partitions'][1]['executable'] = True
                path = Path(td) / 'layout.json'
                path.write_text(json.dumps(layout))
                with self.assertRaises(ValueError):
                    flash_layout.load_layout(path)


if __name__ == '__main__':
    unittest.main()
