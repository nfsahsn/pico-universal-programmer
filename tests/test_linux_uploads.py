import argparse
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
from tools.upload_openocd import command
from tools.upload_gdb import script
from tools.install_arduino_linux import merge, remove

ROOT = Path(__file__).resolve().parents[1]


class LinuxUploads(unittest.TestCase):
    def test_remove_profile_without_final_newline(self):
        installed = merge('custom=yes\n', 'a=1', 'arm').rstrip('\n')
        self.assertEqual(remove(installed, 'arm'), 'custom=yes\n')

    def test_remove_absent_profile_preserves_longer_name(self):
        installed = merge('custom=yes\n', 'a=1', 'arm_f4')
        self.assertEqual(remove(installed, 'arm'), installed)

    def test_multi_profile_prefixes_do_not_collide(self):
        text = merge('custom=yes\n', 'a=1', 'arm')
        text = merge(text, 'b=2', 'arm_f4')
        text = merge(text, 'a=3', 'arm')
        self.assertIn('b=2', text)
        self.assertIn('a=3', text)
        self.assertNotIn('a=1', text)

    def test_segments_overlap_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'app.bin'
            path.write_bytes(bytes(100))
            args = argparse.Namespace(segment=[('0x1000', str(path)), ('0x1020', str(path))], image=None, esp=True)
            with self.assertRaisesRegex(ValueError, 'overlap'):
                command(args)

    def test_esp_segments_must_not_share_erase_sector(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'app.bin'
            path.write_bytes(bytes(100))
            args = argparse.Namespace(segment=[('0x1000', str(path)), ('0x1100', str(path))], image=None, esp=True)
            with self.assertRaisesRegex(ValueError, 'erase sector'):
                command(args)

    def test_esp_segments_verify_each_image(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'app } [bad].bin'
            path.write_bytes(bytes(100))
            args = argparse.Namespace(segment=[('0x1000', str(path)), ('0x8000', str(path))],
                                      image=None, speed=100, target='target/esp32.cfg', scripts=[],
                                      serial=None, transport='jtag', openocd='openocd', esp=True)
            with patch('shutil.which', return_value='/bin/openocd'):
                commands = command(args)
            self.assertEqual(sum('program_esp' in item for item in commands), 2)
            self.assertEqual(sum('verify' in item for item in commands), 2)
            self.assertEqual(sum('shutdown error' in item for item in commands), 2)

    def test_gdb_verify_before_reset(self):
        commands = script('blackmagic', '/dev/serial/by-id/probe', 'swd', 1)
        self.assertLess(commands.index('compare-sections'), commands.index('monitor reset'))
        self.assertIn('raise gdb.GdbError', commands)
        self.assertIn('monitor swd_scan', commands)
        self.assertIn('attach 1', commands)

    def test_picorvd_does_not_use_blackmagic_scan(self):
        commands = script('picorvd', '/dev/ttyACM0', 'swd', 1)
        self.assertNotIn('scan', commands)
        self.assertNotIn('attach', commands)
        self.assertIn('detach', commands)

    def test_gdb_command_injection_rejected(self):
        for port in ['/dev/ttyACM0\nquit', '|shell', 'localhost:1234']:
            with self.assertRaises(ValueError):
                script('picorvd', port, 'swd', 1)

    def test_installer_preview_apply_update_remove(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            core = root / 'core with spaces'
            core.mkdir()
            for name in ['platform.txt', 'boards.txt']:
                (core / name).write_text('name=test\n')
            (core / 'programmers.txt').write_text('custom.name=Keep me\n')
            scripts = root / 'scripts'
            (scripts / 'target').mkdir(parents=True)
            (scripts / 'interface').mkdir()
            (scripts / 'target/test.cfg').write_text('')
            (scripts / 'interface/cmsis-dap.cfg').write_text('')
            args = [sys.executable, str(ROOT / 'tools/install_arduino_linux.py'), '--core', str(core),
                    '--profile', 'test', '--target', 'target/test.cfg', '--openocd', sys.executable,
                    '--scripts', str(scripts)]
            subprocess.run(args, check=True, capture_output=True)
            self.assertFalse((core / 'platform.local.txt').exists())
            for _ in range(2):
                subprocess.run(args + ['--apply'], check=True, capture_output=True)
            content = (core / 'programmers.txt').read_text()
            self.assertEqual(content.count('pico_universal_test.name='), 1)
            self.assertIn('custom.name=Keep me', content)
            self.assertEqual((core / 'programmers.txt.pico-backup').read_text(), 'custom.name=Keep me\n')
            subprocess.run(args + ['--apply', '--remove'], check=True, capture_output=True)
            self.assertEqual((core / 'programmers.txt').read_text(), 'custom.name=Keep me\n')

    def test_gdb_verification_rejects_mismatch_and_empty_output(self):
        import re
        from types import SimpleNamespace
        commands = script('blackmagic', '/dev/ttyACM0', 'swd', 1)
        verification = commands.split('\npython\n')[1].split('\nend\n')[0]
        for report, valid in [('Section .text, range 0x0 -- 0x10: matched.\n', True),
                              ('Section .text: MIS-MATCHED!\n', False),
                              ('', False),
                              ('Section .text: matched.\nwarning: failed to read .data', False)]:
            gdb = SimpleNamespace(execute=lambda *a, **k: report, write=lambda text: None, GdbError=ValueError)
            if valid:
                exec(verification, {'gdb': gdb, 're': re})
            else:
                with self.assertRaises(ValueError):
                    exec(verification, {'gdb': gdb, 're': re})
