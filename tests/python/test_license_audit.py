import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from tools.license_audit import audit_components, generate_spdx_sbom  # noqa: E402


class TestLicenseAudit(unittest.TestCase):
    def test_license_audit_rejects_unattributed_and_flags_gpl_for_source_offer(self) -> None:
        ok, issues = audit_components(
            [
                {"name": "pico-sdk", "spdx": "BSD-3-Clause", "notice_included": True, "source_offer": False},
                {"name": "openocd", "spdx": "GPL-2.0-or-later", "notice_included": True, "source_offer": False},
            ]
        )
        self.assertFalse(ok)
        self.assertTrue(
            any("source_offer" in msg and "openocd" in msg for msg in issues),
            msg=f"expected source_offer flag for openocd, got {issues}",
        )

        ok2, issues2 = audit_components(
            [
                {"name": "tinyusb", "spdx": "MIT", "notice_included": False, "source_offer": False},
            ]
        )
        self.assertFalse(ok2)
        self.assertTrue(any("notice_included" in msg and "tinyusb" in msg for msg in issues2))

    def test_cli_verify_and_sbom_generation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            sbom_path = Path(tmp) / "sbom.spdx.json"
            proc = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tools" / "license_audit.py"),
                    "--verify",
                    "--sbom-out",
                    str(sbom_path),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, msg=proc.stdout + proc.stderr)
            sbom = json.loads(sbom_path.read_text(encoding="utf-8"))
            self.assertEqual(sbom["spdxVersion"], "SPDX-2.3")
            self.assertEqual(sbom["dataLicense"], "CC0-1.0")
            self.assertGreaterEqual(len(sbom["packages"]), 8)

    def test_generate_spdx_sbom_structure(self) -> None:
        sbom = generate_spdx_sbom(
            [{"name": "pico-sdk", "version": "2.1.1", "spdx": "BSD-3-Clause", "homepage": "https://example.com"}]
        )
        self.assertEqual(sbom["packages"][0]["SPDXID"], "SPDXRef-Package-pico-sdk")


if __name__ == "__main__":
    unittest.main()
