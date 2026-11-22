import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "hil"))

import run_hil_matrix  # noqa: E402


class TestHILMatrixRunner(unittest.TestCase):
    def test_dry_run_cli_passes_and_writes_valid_report(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            out_path = Path(tmp) / "hil_report.json"
            proc = subprocess.run(
                [
                    sys.executable,
                    str(ROOT / "tests" / "hil" / "run_hil_matrix.py"),
                    "--dry-run",
                    "--out",
                    str(out_path),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, msg=proc.stderr + proc.stdout)
            self.assertTrue(out_path.is_file())

            report = json.loads(out_path.read_text(encoding="utf-8"))
            self.assertEqual(report["schema_version"], "1.0.0")
            self.assertEqual(report["mode"], "dry-run")
            self.assertTrue(report["summary"]["overall_passed"])
            self.assertEqual(report["summary"]["families_tested"], 5)
            self.assertEqual(report["summary"]["families_passed"], 5)
            self.assertTrue(report["cross_family_switch"]["passed"])

            fams = {r["family"] for r in report["families"]}
            self.assertEqual(fams, {"stm32f1", "stm32f4", "rp2040", "esp32", "ch32v003"})
            for row in report["families"]:
                self.assertTrue(row["passed"])
                self.assertEqual(row["upload_pass_rate"], 1.0)
                self.assertEqual(row["vtref_mv"], 3300)

    def test_simulated_family_failure_fails_overall_gate(self) -> None:
        report = run_hil_matrix.run_qualification(
            mode="dry-run",
            pu_bin="pu",
            families=["stm32f1", "ch32v003"],
            cycles=2,
            executor=run_hil_matrix.dry_run_command_executor(fail_family="ch32v003"),
        )
        self.assertFalse(report["summary"]["overall_passed"])
        self.assertEqual(report["summary"]["families_tested"], 2)
        self.assertEqual(report["summary"]["families_passed"], 1)
        by_fam = {r["family"]: r for r in report["families"]}
        self.assertTrue(by_fam["stm32f1"]["passed"])
        self.assertFalse(by_fam["ch32v003"]["passed"])

    def test_single_stm32f4_family_selection_passes_switch_gate(self) -> None:
        report = run_hil_matrix.run_qualification(
            mode="dry-run",
            pu_bin="pu",
            families=["stm32f4"],
            cycles=2,
        )
        self.assertTrue(report["cross_family_switch"]["passed"])
        self.assertTrue(report["summary"]["overall_passed"])


if __name__ == "__main__":
    unittest.main()
