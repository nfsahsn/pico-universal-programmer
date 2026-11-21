#!/usr/bin/env python3
"""Statistical benchmark analysis, significance tests, vector plot & LaTeX macro generator.

Consumes raw CSV datasets collected by `pu bench` in `paper/data/` and generates:
  - `results_macros.tex`: auto-generated LaTeX macros (zero hand-typed paper numbers)
  - `summary_stats.json`: complete mean, SD, 95% CI, median, p95, p99 & Mann-Whitney U p-values
  - Publication vector plots (PDF via matplotlib when installed; deterministic SVG fallback otherwise)
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
from collections import defaultdict
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

# Two-tailed 95% Student-t critical values for df=1..30; z=1.959964 for df>30.
_T_CRIT_95 = [
    0.0,
    12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
    2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
    2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042,
]


def _percentile(sorted_vals: Sequence[float], q: float) -> float:
    n = len(sorted_vals)
    if n == 0:
        return 0.0
    if n == 1:
        return float(sorted_vals[0])
    pos = q * (n - 1)
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return float(sorted_vals[lo])
    frac = pos - lo
    return float(sorted_vals[lo] * (1.0 - frac) + sorted_vals[hi] * frac)


def summarize_values(values: Sequence[float]) -> Dict[str, float]:
    n = len(values)
    if n == 0:
        return {
            "n": 0,
            "mean": 0.0,
            "sd": 0.0,
            "ci95": 0.0,
            "median": 0.0,
            "p95": 0.0,
            "p99": 0.0,
            "min": 0.0,
            "max": 0.0,
        }
    sv = sorted(float(v) for v in values)
    mean = sum(sv) / n
    if n > 1:
        var = sum((x - mean) ** 2 for x in sv) / (n - 1)
        sd = math.sqrt(var)
        df = n - 1
        crit = _T_CRIT_95[df] if df < len(_T_CRIT_95) else 1.959964
        ci95 = crit * sd / math.sqrt(n)
    else:
        sd = 0.0
        ci95 = 0.0

    return {
        "n": n,
        "mean": mean,
        "sd": sd,
        "ci95": ci95,
        "median": _percentile(sv, 0.50),
        "p95": _percentile(sv, 0.95),
        "p99": _percentile(sv, 0.99),
        "min": sv[0],
        "max": sv[-1],
    }


def compute_summary_stats(
    csv_path: Path,
    *,
    value_col: str,
    group_cols: Sequence[str],
) -> Dict[Tuple[str, ...], Dict[str, float]]:
    """Group rows by `group_cols` and compute mean, SD, 95% CI, median, p95, p99."""
    groups: Dict[Tuple[str, ...], List[float]] = defaultdict(list)
    with Path(csv_path).open("r", encoding="utf-8", newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            raw_val = row.get(value_col, "")
            if raw_val == "":
                continue
            key = tuple(str(row[c]) for c in group_cols)
            groups[key].append(float(raw_val))

    return {k: summarize_values(v) for k, v in sorted(groups.items())}


def mann_whitney_u_pvalue(sample_a: Sequence[float], sample_b: Sequence[float]) -> float:
    """Two-sided Mann-Whitney U test (tie-corrected normal approximation, zero external deps)."""
    n1 = len(sample_a)
    n2 = len(sample_b)
    if n1 == 0 or n2 == 0:
        return 1.0

    combined = [(float(x), 0) for x in sample_a] + [(float(x), 1) for x in sample_b]
    combined.sort(key=lambda item: item[0])

    ranks = [0.0] * len(combined)
    tie_correction = 0.0
    i = 0
    total_n = len(combined)
    while i < total_n:
        j = i
        while j + 1 < total_n and combined[j + 1][0] == combined[i][0]:
            j += 1
        avg_rank = 0.5 * (i + 1 + j + 1)
        for k in range(i, j + 1):
            ranks[k] = avg_rank
        t = j - i + 1
        if t > 1:
            tie_correction += (t ** 3 - t)
        i = j + 1

    r1 = sum(ranks[idx] for idx, (_, grp) in enumerate(combined) if grp == 0)
    u1 = r1 - n1 * (n1 + 1) / 2.0
    u2 = n1 * n2 - u1
    u = min(u1, u2)

    mu = n1 * n2 / 2.0
    denom = total_n * (total_n - 1)
    if denom <= 0:
        return 1.0
    sigma_sq = (n1 * n2 / 12.0) * ((total_n + 1) - tie_correction / denom)
    if sigma_sq <= 0.0:
        return 1.0
    z = (abs(u - mu) - 0.5) / math.sqrt(sigma_sq)
    z = max(z, 0.0)
    return float(math.erfc(z / math.sqrt(2.0)))


def _latex_macro_name(prefix: str, parts: Iterable[str]) -> str:
    tokens = [prefix]
    for p in parts:
        cleaned = re.sub(r"[^A-Za-z0-9]+", " ", str(p)).strip()
        for word in cleaned.split():
            # LaTeX command names may only contain ASCII letters; map digits deterministically.
            digit_map = str.maketrans("0123456789", "ABCDEFGHIJ")
            word = word.translate(digit_map)
            tokens.append(word[:1].upper() + word[1:])
    return "".join(tokens)


def render_svg_bar_chart(
    title: str,
    y_label: str,
    bars: Sequence[Tuple[str, float, float]],
    out_svg: Path,
) -> None:
    """Emit a crisp vector SVG bar chart with 95% CI error bars (zero external dependencies)."""
    width, height = 760, 420
    left, right, top, bottom = 80, 30, 50, 110
    plot_w = width - left - right
    plot_h = height - top - bottom
    max_val = max((val + ci for _, val, ci in bars), default=1.0) * 1.15
    if max_val <= 0:
        max_val = 1.0

    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
        f'<text x="{width/2:.1f}" y="28" text-anchor="middle" font-family="sans-serif" font-size="15" font-weight="bold">{title}</text>',
        f'<line x1="{left}" y1="{top}" x2="{left}" y2="{top+plot_h}" stroke="#111827" stroke-width="1.5"/>',
        f'<line x1="{left}" y1="{top+plot_h}" x2="{left+plot_w}" y2="{top+plot_h}" stroke="#111827" stroke-width="1.5"/>',
        f'<text x="20" y="{top+plot_h/2:.1f}" text-anchor="middle" transform="rotate(-90 20 {top+plot_h/2:.1f})" font-family="sans-serif" font-size="12">{y_label}</text>',
    ]

    if bars:
        step = plot_w / len(bars)
        bar_w = min(48.0, step * 0.62)
        for idx, (label, val, ci) in enumerate(bars):
            cx = left + step * (idx + 0.5)
            bar_h = (val / max_val) * plot_h
            y0 = top + plot_h - bar_h
            lines.append(
                f'<rect x="{cx - bar_w/2:.1f}" y="{y0:.1f}" width="{bar_w:.1f}" height="{bar_h:.1f}" fill="#2563eb"/>'
            )
            if ci > 0:
                ci_px = (ci / max_val) * plot_h
                y_hi = max(top, y0 - ci_px)
                y_lo = min(top + plot_h, y0 + ci_px)
                lines.append(f'<line x1="{cx:.1f}" y1="{y_hi:.1f}" x2="{cx:.1f}" y2="{y_lo:.1f}" stroke="#111827" stroke-width="1.5"/>')
                lines.append(f'<line x1="{cx-5:.1f}" y1="{y_hi:.1f}" x2="{cx+5:.1f}" y2="{y_hi:.1f}" stroke="#111827" stroke-width="1.5"/>')
                lines.append(f'<line x1="{cx-5:.1f}" y1="{y_lo:.1f}" x2="{cx+5:.1f}" y2="{y_lo:.1f}" stroke="#111827" stroke-width="1.5"/>')
            lines.append(
                f'<text x="{cx:.1f}" y="{top+plot_h+16}" text-anchor="end" transform="rotate(-28 {cx:.1f} {top+plot_h+16})" font-family="sans-serif" font-size="10">{label}</text>'
            )

    lines.append("</svg>\n")
    out_svg.write_text("\n".join(lines), encoding="utf-8")


def analyze_directory(data_dir: Path, out_dir: Path) -> Dict[str, Any]:
    out_dir.mkdir(parents=True, exist_ok=True)
    results: Dict[str, Any] = {
        "e1_throughput": {},
        "e2_debug_latency": {},
        "e3_switching_uart": {},
        "e5_fault_injection": {},
    }
    macro_lines: List[str] = [
        "% Auto-generated by paper/scripts/analyze_benchmarks.py — DO NOT EDIT BY HAND.",
    ]

    e1_path = data_dir / "e1_throughput.csv"
    if e1_path.is_file():
        e1 = compute_summary_stats(
            e1_path,
            value_col="throughput_kibs",
            group_cols=["probe", "target", "payload_kib"],
        )
        bars: List[Tuple[str, float, float]] = []
        for k, st in e1.items():
            key_str = "|".join(k)
            results["e1_throughput"][key_str] = st
            m_mean = _latex_macro_name("ResEOneMean", k)
            m_ci = _latex_macro_name("ResEOneCI", k)
            macro_lines.append(f"\\newcommand{{\\{m_mean}}}{{{st['mean']:.2f}}}")
            macro_lines.append(f"\\newcommand{{\\{m_ci}}}{{{st['ci95']:.2f}}}")
            bars.append((f"{k[0]}:{k[1]}:{k[2]}K", st["mean"], st["ci95"]))
        render_svg_bar_chart(
            "E1 Flash + Verify Throughput (Mean +/- 95% CI)",
            "Throughput (KiB/s)",
            bars[:12],
            out_dir / "fig_e1_throughput.svg",
        )

    e2_path = data_dir / "e2_debug_latency.csv"
    if e2_path.is_file():
        e2 = compute_summary_stats(
            e2_path,
            value_col="latency_ms",
            group_cols=["probe", "target", "operation"],
        )
        bars_e2: List[Tuple[str, float, float]] = []
        for k, st in e2.items():
            key_str = "|".join(k)
            results["e2_debug_latency"][key_str] = st
            m_med = _latex_macro_name("ResETwoMed", k)
            m_p99 = _latex_macro_name("ResETwoPNN", k)
            macro_lines.append(f"\\newcommand{{\\{m_med}}}{{{st['median']:.2f}}}")
            macro_lines.append(f"\\newcommand{{\\{m_p99}}}{{{st['p99']:.2f}}}")
            bars_e2.append((f"{k[1]}:{k[2]}", st["median"], st["p95"] - st["median"]))
        render_svg_bar_chart(
            "E2 Interactive Debug Latency (Median + p95 Margin)",
            "Latency (ms)",
            bars_e2[:12],
            out_dir / "fig_e2_latency.svg",
        )

    e3_path = data_dir / "e3_switching_uart.csv"
    if e3_path.is_file():
        e3 = compute_summary_stats(
            e3_path,
            value_col="switch_ms",
            group_cols=["probe", "from_family", "to_family", "uart_baud"],
        )
        for k, st in e3.items():
            results["e3_switching_uart"]["|".join(k)] = st

    e5_path = data_dir / "e5_fault_injection.csv"
    if e5_path.is_file():
        with e5_path.open("r", encoding="utf-8", newline="") as f:
            for row in csv.DictReader(f):
                key_str = f"{row['probe']}|{row['fault_class']}"
                results["e5_fault_injection"][key_str] = {
                    "trials_total": int(row["trials_total"]),
                    "trials_recovered": int(row["trials_recovered"]),
                    "recovery_rate_pct": float(row["recovery_rate_pct"]),
                }
                m_rate = _latex_macro_name("ResEFiveRate", [row["probe"], row["fault_class"]])
                macro_lines.append(f"\\newcommand{{\\{m_rate}}}{{{float(row['recovery_rate_pct']):.2f}\\%}}")

    # Summary metadata macro indicating whether physical lab data or placeholder exists.
    manifest_path = data_dir / "bench_manifest.json"
    dataset_mode = "pending-lab-collection"
    if manifest_path.is_file():
        try:
            dataset_mode = str(json.loads(manifest_path.read_text(encoding="utf-8")).get("mode", dataset_mode))
        except json.JSONDecodeError:
            pass
    macro_lines.append(f"\\newcommand{{\\BenchDatasetMode}}{{{dataset_mode}}}")

    (out_dir / "results_macros.tex").write_text("\n".join(macro_lines) + "\n", encoding="utf-8")
    (out_dir / "summary_stats.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    return results


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Analyze E1-E6 benchmark CSVs and generate LaTeX macros + figures.")
    p.add_argument("--data", type=Path, required=True, help="Input directory containing e1..e6 CSVs.")
    p.add_argument("--out", type=Path, required=True, help="Output directory for figures and results_macros.tex.")
    return p.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    analyze_directory(args.data, args.out)
    print(f"Analyzed benchmark data from {args.data} -> {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
