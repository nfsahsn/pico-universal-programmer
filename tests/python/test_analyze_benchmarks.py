import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from paper.scripts.analyze_benchmarks import (  # noqa: E402
    analyze_directory,
    compute_summary_stats,
    mann_whitney_u_pvalue,
)


class TestAnalyzeBenchmarks(unittest.TestCase):
    def test_compute_summary_stats_produces_ci95_and_p99(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp) / "e1.csv"
            with p.open("w", newline="", encoding="utf-8") as f:
                w = csv.writer(f)
                w.writerow(["probe", "target", "payload_kib", "trial", "throughput_kibs"])
                for i in range(30):
                    w.writerow(["pico_universal", "stm32f407", 256, i, 120.0 + (i % 5)])

            stats = compute_summary_stats(
                p,
                value_col="throughput_kibs",
                group_cols=["probe", "target", "payload_kib"],
            )
            row = stats[("pico_universal", "stm32f407", "256")]
            self.assertEqual(row["n"], 30)
            self.assertGreater(row["mean"], 121.5)
            self.assertLess(row["mean"], 122.5)
            self.assertGreater(row["ci95"], 0.0)
            self.assertGreaterEqual(row["p99"], row["p95"])
            self.assertGreaterEqual(row["p95"], row["median"])

    def test_mann_whitney_distinguishes_shifted_distributions(self) -> None:
        p_diff = mann_whitney_u_pvalue([10, 11, 12, 13, 14, 15], [50, 51, 52, 53, 54, 55])
        p_same = mann_whitney_u_pvalue([10, 11, 12, 13, 14, 15], [10, 11, 12, 13, 14, 15])
        self.assertLess(p_diff, 0.01)
        self.assertGreater(p_same, 0.5)

    def test_analyze_directory_exports_latex_macros_json_and_svg(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            data_dir = Path(tmp) / "data"
            out_dir = Path(tmp) / "generated"
            data_dir.mkdir()
            with (data_dir / "e1_throughput.csv").open("w", newline="", encoding="utf-8") as f:
                w = csv.writer(f)
                w.writerow(["timestamp_utc", "probe", "target", "payload_kib", "trial", "elapsed_ms", "throughput_kibs", "verified"])
                for i in range(10):
                    w.writerow(["2026-11-20T00:00:00Z", "pico_universal", "stm32f407", "64", str(i), "520.0", str(123.0 + i * 0.2), "true"])

            analyze_directory(data_dir, out_dir)
            macros = (out_dir / "results_macros.tex").read_text(encoding="utf-8")
            self.assertIn("\\newcommand{\\ResEOneMean", macros)
            self.assertIn("\\BenchDatasetMode", macros)
            summary = json.loads((out_dir / "summary_stats.json").read_text(encoding="utf-8"))
            self.assertIn("pico_universal|stm32f407|64", summary["e1_throughput"])
            self.assertTrue((out_dir / "fig_e1_throughput.svg").is_file())


if __name__ == "__main__":
    unittest.main()
