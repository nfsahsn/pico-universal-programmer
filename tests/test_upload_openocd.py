import argparse
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from tools.upload_openocd import command, main, tcl_word


class UploadTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.image = Path(self.temp.name) / 'sketch [evil] $value "name".elf'
        self.image.write_bytes(b'\x7fELF' + bytes(60))
        self.args = argparse.Namespace(image=self.image, speed=100, target='target/stm32f4x.cfg',
                                       openocd='openocd', scripts=[], transport='swd', serial=None)
        mock = patch('tools.upload_openocd.shutil.which', return_value='/usr/bin/openocd')
        mock.start()
        self.addCleanup(mock.stop)

    def test_verified_upload_and_target_selection(self):
        result = command(self.args)
        self.assertIn('transport select swd', result)
        self.assertIn(self.args.target, result)
        self.assertEqual(result[-1], 'program ' + tcl_word(self.image) + ' verify reset exit')
        self.args.transport = 'jtag'
        self.args.target = 'target/other.cfg'
        self.assertIn('transport select jtag', command(self.args))
        self.assertIn('target/other.cfg', command(self.args))

    def test_tcl_substitution_is_escaped(self):
        self.assertEqual(tcl_word('$x[exit]"\\\n'), '"\\$x\\[exit\\]\\"\\\\\\n"')

    def test_reject_bad_image(self):
        self.image.write_bytes(b'not ELF')
        with self.assertRaises(ValueError):
            command(self.args)

    def test_reject_bad_speed(self):
        self.args.speed = 0
        with self.assertRaises(ValueError):
            command(self.args)

    def test_probe_selection(self):
        self.args.serial = 'probe [exit]'
        self.assertIn('adapter serial "probe \\[exit\\]"', command(self.args))

    def test_failure_propagates(self):
        with patch('tools.upload_openocd.subprocess.run') as run:
            run.return_value.returncode = 7
            self.assertEqual(main(['--target', self.args.target, '--transport', 'swd',
                                   '--image', str(self.image)]), 7)
            self.assertNotIn('shell', run.call_args.kwargs)

    def test_dry_run_does_not_launch(self):
        with patch('tools.upload_openocd.subprocess.run') as run, patch('builtins.print'):
            self.assertEqual(main(['--target', self.args.target, '--transport', 'swd',
                                   '--image', str(self.image), '--dry-run']), 0)
            run.assert_not_called()

    def test_upload_uses_immutable_snapshot(self):
        original = self.image.read_bytes()
        def run(arguments, **kwargs):
            snapshot_command = arguments[-1]
            self.assertNotIn(str(self.image), snapshot_command)
            snapshot = Path(snapshot_command.split('"')[1])
            self.image.write_bytes(b'concurrent build')
            self.assertEqual(snapshot.read_bytes(), original)
            return argparse.Namespace(returncode=0)
        with patch('tools.upload_openocd.subprocess.run', side_effect=run):
            self.assertEqual(main(['--target', self.args.target, '--transport', 'swd',
                                   '--image', str(self.image)]), 0)

    def test_generic_multi_segment_upload_rejected(self):
        self.args.image = None
        self.args.segment = [('0', str(self.image)), ('0x1000', str(self.image))]
        with self.assertRaisesRegex(ValueError, 'ELF'):
            command(self.args)


class InstallerTests(unittest.TestCase):
    def test_preserve_user_recipes_and_idempotence(self):
        from tools.install_arduino_linux import merge
        original = 'custom.name=My programmer\n'
        installed = merge(original, 'pico.name=First')
        self.assertTrue(installed.startswith(original))
        self.assertEqual(merge(installed, 'pico.name=First'), installed)
        updated = merge(installed, 'pico.name=Second')
        self.assertIn('pico.name=Second', updated)
        self.assertNotIn('pico.name=First', updated)
        self.assertTrue(updated.startswith(original))

    def test_malformed_marker_rejected(self):
        from tools.install_arduino_linux import merge, START
        with self.assertRaises(ValueError):
            merge(START, 'new')

    def test_property_injection_rejected(self):
        from tools.install_arduino_linux import safe_property
        for value in ['path" --other', 'path\nnew.property=bad', '{runtime.os}']:
            with self.assertRaises(ValueError):
                safe_property(value)
