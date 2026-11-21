#!/usr/bin/env python3
"""Automated Track A / Track B Hardware-in-the-Loop (HIL) qualification runner.

Supports two execution modes:
  --dry-run      Runs offline self-test of the full qualification pipeline and
                 JSON report generator without requiring physical USB hardware.
  --execute-hil  Executes physical HIL qualification against connected Pico
                 Universal hardware and target boards (STM32 F1/F4, RP2040,
                 classic ESP32, WCH CH32V003).
"""

from __future__ import annotations

import argparse
import dataclasses
import datetime as dt
import hashlib
import json
import platform
import shutil
import subprocess
import time
from pathlib import Path
from typing import Any, Callable, Dict, List, Optional, Sequence

SCHEMA_VERSION = "1.0.0"

# Canonical v1 qualification matrix matching Spec Section 4.3.
V1_TARGET_FAMILIES: List[Dict[str, Any]] = [
    {
        "family": "stm32f1",
        "board": "STM32F103C8/CB (Blue Pill)",
        "transport": "CMSIS-DAP v2 SWD",
        "gdb_supported": True,
        "requires_segments": False,
    },
    {
        "family": "stm32f4",
        "board": "STM32F407VE/F411CE (DevEBox/Black Pill/Nucleo)",
        "transport": "CMSIS-DAP v2 SWD",
        "gdb_supported": True,
        "requires_segments": False,
    },
    {
        "family": "rp2040",
        "board": "Raspberry Pi Pico / Pico W (RP2040)",
        "transport": "CMSIS-DAP v2 SWD (Multidrop)",
        "gdb_supported": True,
        "requires_segments": False,
    },
    {
        "family": "esp32",
        "board": "ESP32-WROOM-32 DevKit (incl. Zbit QIO)",
        "transport": "CMSIS-DAP v2 4-Wire JTAG",
        "gdb_supported": True,
        "requires_segments": True,
    },
    {
        "family": "ch32v003",
        "board": "WCH CH32V003F4P6 Dev Board",
        "transport": "PIO1 1-Wire SWIO + GDB",
        "gdb_supported": True,
        "requires_segments": False,
    },
]

CROSS_FAMILY_SWITCH_ORDER = ["stm32f1", "esp32", "ch32v003", "rp2040"]


@dataclasses.dataclass
class CommandResult:
    returncode: int
    stdout: str
    stderr: str
    elapsed_ms: float


CommandExecutor = Callable[[Sequence[str], float], CommandResult]


def sha256_file(path: Optional[Path]) -> str:
    if path is None or not path.is_file():
        return ""
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def real_command_executor(cmd: Sequence[str], timeout_s: float) -> CommandResult:
    t0 = time.monotonic()
    proc = subprocess.run(
        list(cmd),
        capture_output=True,
        text=True,
        timeout=timeout_s,
        check=False,
    )
    elapsed_ms = (time.monotonic() - t0) * 1000.0
    return CommandResult(
        returncode=proc.returncode,
        stdout=proc.stdout,
        stderr=proc.stderr,
        elapsed_ms=round(elapsed_ms, 3),
    )


def dry_run_command_executor(
    fail_family: Optional[str] = None,
) -> CommandExecutor:
    """Offline deterministic executor for --dry-run CI verification."""

    def _exec(cmd: Sequence[str], timeout_s: float) -> CommandResult:
        _ = timeout_s
        args = list(cmd)
        sub = args[1] if len(args) > 1 else ""
        if sub == "--pid" and len(args) > 3:
            sub = args[3]
        family = ""
        if "--family" in args:
            idx = args.index("--family")
            if idx + 1 < len(args):
                family = args[idx + 1]

        if fail_family and family == fail_family and sub in ("check", "upload"):
            return CommandResult(
                returncode=1,
                stdout="",
                stderr=f"simulated HIL failure on {family} ({sub})",
                elapsed_ms=1.5,
            )

        if sub == "list":
            payload = [
                {
                    "Serial": "DRYRUN0000000001",
                    "VID": 0x2E8A,
                    "PID": 0x1050,
                    "Product": "Pico Universal (Dry-Run)",
                    "DAPPath": "/dev/bus/usb/dryrun/001",
                    "GDBPort": "/dev/ttyACM_DRY0",
                    "UARTPort": "/dev/ttyACM_DRY1",
                    "CtrlPath": "/dev/bus/usb/dryrun/001:1.5",
                }
            ]
            return CommandResult(0, json.dumps(payload), "", 0.8)

        if sub == "check":
            report = {
                "family": family,
                "probe_serial": "DRYRUN0000000001",
                "verdict": "OK",
                "ready": True,
                "diagnosis": f"Dry-run simulated {family} identity OK",
                "vtref_mv": 3300,
                "itarg_ma": 24,
            }
            return CommandResult(0, json.dumps(report), "", 4.2)

        if sub == "upload":
            return CommandResult(0, f"Uploaded firmware to {family} (dry-run)\n", "", 12.5)

        if sub == "recover":
            return CommandResult(0, f"Recovered {family} (dry-run)\n", "", 5.0)

        return CommandResult(0, "ok\n", "", 1.0)

    return _exec


def load_fixture_manifest(path: Optional[Path]) -> Dict[str, Any]:
    if path is None:
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def build_upload_args(
    pu_bin: str,
    family: str,
    serial: str,
    pid: str,
    fixture: Dict[str, Any],
) -> List[str]:
    cmd = [pu_bin]
    if pid:
        cmd.extend(["--pid", pid])
    cmd.extend(["upload", "--family", family])
    if serial:
        cmd.extend(["--serial", serial])

    segments = fixture.get("segments")
    if segments:
        for seg in segments:
            cmd.extend(["--segment", str(seg)])
    else:
        fw_file = fixture.get("file", f"dryrun_{family}.elf")
        cmd.extend(["--file", str(fw_file)])
    return cmd


def run_qualification(
    *,
    mode: str,
    pu_bin: str,
    families: Sequence[str],
    cycles: int,
    serial: str = "",
    pid: str = "",
    firmware_uf2: Optional[Path] = None,
    fixtures: Optional[Dict[str, Any]] = None,
    executor: Optional[CommandExecutor] = None,
    timeout_s: float = 120.0,
) -> Dict[str, Any]:
    if mode not in ("dry-run", "execute-hil"):
        raise ValueError(f"unsupported mode: {mode}")
    if cycles < 1:
        raise ValueError("cycles must be >= 1")

    exec_fn = executor
    if exec_fn is None:
        exec_fn = (
            dry_run_command_executor()
            if mode == "dry-run"
            else real_command_executor
        )

    fixtures = fixtures or {}
    family_meta = {item["family"]: item for item in V1_TARGET_FAMILIES}
    selected = list(families)
    for f in selected:
        if f not in family_meta:
            raise ValueError(f"unsupported target family: {f}")

    # Step 1: Discover probe via `pu list --json`
    list_cmd = [pu_bin]
    if pid:
        list_cmd.extend(["--pid", pid])
    list_cmd.extend(["list", "--json"])
    if serial:
        list_cmd.extend(["--serial", serial])

    list_res = exec_fn(list_cmd, timeout_s)
    probes: List[Dict[str, Any]] = []
    probe_error = ""
    if list_res.returncode != 0:
        probe_error = list_res.stderr.strip() or f"pu list exited {list_res.returncode}"
    else:
        try:
            probes = json.loads(list_res.stdout or "[]")
        except json.JSONDecodeError as exc:
            probe_error = f"invalid JSON from pu list: {exc}"

    active_probe: Dict[str, Any] = probes[0] if probes else {}
    effective_serial = serial or str(active_probe.get("Serial", ""))

    family_reports: List[Dict[str, Any]] = []
    all_passed = bool(probes) and not probe_error

    for fam in selected:
        meta = family_meta[fam]
        fam_fixture = fixtures.get(fam, {})

        # Pre-flight non-destructive `pu check --json`
        check_cmd = [pu_bin]
        if pid:
            check_cmd.extend(["--pid", pid])
        check_cmd.extend(["check", "--family", fam, "--json"])
        if effective_serial:
            check_cmd.extend(["--serial", effective_serial])

        check_res = exec_fn(check_cmd, timeout_s)
        check_ok = False
        check_payload: Dict[str, Any] = {}
        if check_res.returncode == 0:
            try:
                check_payload = json.loads(check_res.stdout or "{}")
                check_ok = bool(check_payload.get("ready", False))
            except json.JSONDecodeError:
                check_ok = False

        # Flash + readback verify cycles
        upload_cmd = build_upload_args(pu_bin, fam, effective_serial, pid, fam_fixture)
        passed_cycles = 0
        cycle_latencies_ms: List[float] = []
        last_upload_err = ""
        for _ in range(cycles):
            up_res = exec_fn(upload_cmd, timeout_s)
            cycle_latencies_ms.append(up_res.elapsed_ms)
            if up_res.returncode == 0:
                passed_cycles += 1
            else:
                last_upload_err = up_res.stderr.strip() or f"exit {up_res.returncode}"
                break

        # Post-upload non-destructive check (confirms target resumes & stays responsive)
        post_check_res = exec_fn(check_cmd, timeout_s)
        post_check_ok = post_check_res.returncode == 0

        # GDB breakpoint/step/detach check (simulated in dry-run, explicit fixture hook in HIL)
        gdb_passed = True
        gdb_note = "simulated-pass (dry-run)"
        if mode == "execute-hil":
            gdb_cmd = fam_fixture.get("gdb_smoke_cmd")
            if gdb_cmd:
                gdb_res = exec_fn(gdb_cmd, timeout_s)
                gdb_passed = gdb_res.returncode == 0
                gdb_note = "passed" if gdb_passed else (gdb_res.stderr.strip() or "failed")
            else:
                gdb_passed = False
                gdb_note = "missing gdb_smoke_cmd in fixture manifest"

        fam_pass = check_ok and (passed_cycles == cycles) and post_check_ok and gdb_passed
        if not fam_pass:
            all_passed = False

        family_reports.append(
            {
                "family": fam,
                "board": fam_fixture.get("board_revision", meta["board"]),
                "transport": meta["transport"],
                "preflight_check_ok": check_ok,
                "preflight_verdict": check_payload.get("verdict", "ERR" if not check_ok else "OK"),
                "vtref_mv": check_payload.get("vtref_mv", 0),
                "itarg_ma": check_payload.get("itarg_ma", 0),
                "upload_cycles_requested": cycles,
                "upload_cycles_passed": passed_cycles,
                "upload_pass_rate": round(passed_cycles / cycles, 4),
                "upload_mean_ms": round(
                    sum(cycle_latencies_ms) / max(len(cycle_latencies_ms), 1), 3
                ),
                "upload_last_error": last_upload_err,
                "post_upload_check_ok": post_check_ok,
                "gdb_smoke_ok": gdb_passed,
                "gdb_smoke_note": gdb_note,
                "passed": fam_pass,
            }
        )

    # Zero-reboot cross-family switching check across selected families in canonical order
    switch_order = [f for f in CROSS_FAMILY_SWITCH_ORDER if f in selected]
    switch_steps: List[Dict[str, Any]] = []
    switch_passed = len(switch_order) > 0
    for fam in switch_order:
        sw_cmd = [pu_bin]
        if pid:
            sw_cmd.extend(["--pid", pid])
        sw_cmd.extend(["check", "--family", fam, "--json"])
        if effective_serial:
            sw_cmd.extend(["--serial", effective_serial])
        sw_res = exec_fn(sw_cmd, timeout_s)
        ok = sw_res.returncode == 0
        if not ok:
            switch_passed = False
        switch_steps.append(
            {
                "family": fam,
                "ok": ok,
                "elapsed_ms": sw_res.elapsed_ms,
            }
        )

    if not switch_passed:
        all_passed = False

    pu_resolved = shutil.which(pu_bin)
    pu_path = Path(pu_resolved) if pu_resolved else (Path(pu_bin) if Path(pu_bin).is_file() else None)

    return {
        "schema_version": SCHEMA_VERSION,
        "mode": mode,
        "timestamp_utc": dt.datetime.now(dt.timezone.utc).isoformat(),
        "host": {
            "os": platform.system(),
            "release": platform.release(),
            "arch": platform.machine(),
            "python": platform.python_version(),
        },
        "artifacts": {
            "firmware_uf2": str(firmware_uf2) if firmware_uf2 else "",
            "firmware_uf2_sha256": sha256_file(firmware_uf2),
            "pu_binary": str(pu_path) if pu_path else pu_bin,
            "pu_binary_sha256": sha256_file(pu_path),
        },
        "probe": {
            "detected_count": len(probes),
            "serial": effective_serial,
            "vid": active_probe.get("VID", 0),
            "pid": active_probe.get("PID", 0),
            "product": active_probe.get("Product", ""),
            "error": probe_error,
        },
        "families": family_reports,
        "cross_family_switch": {
            "sequence": switch_order,
            "steps": switch_steps,
            "passed": switch_passed,
        },
        "summary": {
            "families_tested": len(family_reports),
            "families_passed": sum(1 for r in family_reports if r["passed"]),
            "overall_passed": all_passed,
        },
    }


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Pico Universal Track A/B Hardware-in-the-Loop (HIL) qualification runner."
    )
    mode_grp = p.add_mutually_exclusive_group(required=True)
    mode_grp.add_argument(
        "--dry-run",
        action="store_true",
        help="Run offline CI self-test without physical hardware.",
    )
    mode_grp.add_argument(
        "--execute-hil",
        action="store_true",
        help="Execute physical HIL qualification against connected boards.",
    )
    p.add_argument(
        "--pu-bin",
        default="pu",
        help="Path to compiled pu CLI binary (default: pu).",
    )
    p.add_argument(
        "--families",
        default=",".join(item["family"] for item in V1_TARGET_FAMILIES),
        help="Comma-separated list of target families to qualify.",
    )
    p.add_argument(
        "--cycles",
        type=int,
        default=None,
        help="Flash+verify cycles per family (default: 3 in --dry-run, 100 in --execute-hil).",
    )
    p.add_argument("--serial", default="", help="Pin specific probe hardware serial.")
    p.add_argument("--pid", default="", help="Override approved USB PID hex (e.g. 0x1050).")
    p.add_argument(
        "--firmware-uf2",
        type=Path,
        default=None,
        help="Optional UF2 firmware image path to hash into the report.",
    )
    p.add_argument(
        "--fixtures",
        type=Path,
        default=None,
        help="JSON fixture manifest mapping family -> firmware file/segments & GDB smoke cmd.",
    )
    p.add_argument(
        "--out",
        type=Path,
        default=None,
        help="Optional output path for timestamped JSON qualification report.",
    )
    return p.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    mode = "dry-run" if args.dry_run else "execute-hil"
    cycles = args.cycles if args.cycles is not None else (3 if args.dry_run else 100)
    families = [f.strip() for f in args.families.split(",") if f.strip()]
    fixtures = load_fixture_manifest(args.fixtures)

    report = run_qualification(
        mode=mode,
        pu_bin=args.pu_bin,
        families=families,
        cycles=cycles,
        serial=args.serial,
        pid=args.pid,
        firmware_uf2=args.firmware_uf2,
        fixtures=fixtures,
    )

    encoded = json.dumps(report, indent=2) + "\n"
    if args.out is not None:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(encoded, encoding="utf-8")

    summary = report["summary"]
    status = "PASS" if summary["overall_passed"] else "FAIL"
    print(
        f"HIL Qualification [{mode}]: {status} "
        f"({summary['families_passed']}/{summary['families_tested']} families passed, "
        f"cross-family switch={'PASS' if report['cross_family_switch']['passed'] else 'FAIL'})"
    )
    return 0 if summary["overall_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
