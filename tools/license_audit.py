#!/usr/bin/env python3
"""SPDX 2.3 SBOM generator and commercial redistribution license compliance auditor."""

from __future__ import annotations

import argparse
import datetime as dt
import json
import re
from pathlib import Path
from typing import Any, Dict, List, Optional, Sequence, Tuple

ROOT = Path(__file__).resolve().parents[1]

COPYLEFT_PREFIXES = ("GPL-", "LGPL-", "AGPL-")

DEFAULT_COMPONENTS: List[Dict[str, Any]] = [
    {
        "name": "pico-sdk",
        "version": "2.1.1",
        "spdx": "BSD-3-Clause",
        "scope": "firmware",
        "bundled": True,
        "notice_included": True,
        "source_offer": False,
        "homepage": "https://github.com/raspberrypi/pico-sdk",
    },
    {
        "name": "tinyusb",
        "version": "0.17.0",
        "spdx": "MIT",
        "scope": "firmware",
        "bundled": True,
        "notice_included": True,
        "source_offer": False,
        "homepage": "https://github.com/hathach/tinyusb",
    },
    {
        "name": "cmsis-dap",
        "version": "2.1.1",
        "spdx": "Apache-2.0",
        "scope": "firmware",
        "bundled": True,
        "notice_included": True,
        "source_offer": False,
        "homepage": "https://github.com/ARM-software/CMSIS-DAP",
    },
    {
        "name": "go-stdlib",
        "version": "1.22+",
        "spdx": "BSD-3-Clause",
        "scope": "host-cli",
        "bundled": True,
        "notice_included": True,
        "source_offer": False,
        "homepage": "https://go.dev/",
    },
    {
        "name": "openocd",
        "version": "0.12.0",
        "spdx": "GPL-2.0-or-later",
        "scope": "host-external-tool",
        "bundled": False,
        "notice_included": True,
        "source_offer": True,
        "homepage": "https://openocd.org/",
    },
    {
        "name": "riscv-none-elf-gdb",
        "version": "15.2.0",
        "spdx": "GPL-3.0-or-later",
        "scope": "host-external-tool",
        "bundled": False,
        "notice_included": True,
        "source_offer": True,
        "homepage": "https://www.gnu.org/software/gdb/",
    },
    {
        "name": "stm32duino",
        "version": "2.12.0",
        "spdx": "BSD-3-Clause",
        "scope": "arduino-core",
        "bundled": False,
        "notice_included": True,
        "source_offer": False,
        "homepage": "https://github.com/stm32duino/Arduino_Core_STM32",
    },
    {
        "name": "arduino-pico",
        "version": "6.1.1",
        "spdx": "LGPL-2.1-or-later",
        "scope": "arduino-core",
        "bundled": False,
        "notice_included": True,
        "source_offer": True,
        "homepage": "https://github.com/earlephilhower/arduino-pico",
    },
    {
        "name": "arduino-esp32",
        "version": "3.3.11",
        "spdx": "LGPL-2.1-or-later",
        "scope": "arduino-core",
        "bundled": False,
        "notice_included": True,
        "source_offer": True,
        "homepage": "https://github.com/espressif/arduino-esp32",
    },
    {
        "name": "arduino-core-ch32",
        "version": "1.0.4",
        "spdx": "MIT",
        "scope": "arduino-core",
        "bundled": False,
        "notice_included": True,
        "source_offer": False,
        "homepage": "https://github.com/openwch/arduino_core_ch32",
    },
]


def is_copyleft_spdx(spdx_id: str) -> bool:
    upper = spdx_id.strip().upper()
    return any(upper.startswith(prefix) for prefix in COPYLEFT_PREFIXES)


def audit_components(components: Sequence[Dict[str, Any]]) -> Tuple[bool, List[str]]:
    """Verify attribution, valid SPDX tags, and copyleft source-offer compliance."""
    issues: List[str] = []
    seen: set[str] = set()
    for idx, comp in enumerate(components):
        name = str(comp.get("name", "")).strip()
        spdx = str(comp.get("spdx", "")).strip()
        notice_included = bool(comp.get("notice_included", False))
        source_offer = bool(comp.get("source_offer", False))

        if not name:
            issues.append(f"component[{idx}]: missing component name")
            continue
        if name in seen:
            issues.append(f"{name}: duplicate component entry")
        seen.add(name)

        if not spdx or not re.match(r"^[A-Za-z0-9.+-]+$", spdx):
            issues.append(f"{name}: invalid or missing SPDX identifier {spdx!r}")
        if not notice_included:
            issues.append(f"{name}: missing required attribution notice (notice_included=False)")
        if is_copyleft_spdx(spdx) and not source_offer:
            issues.append(
                f"{name}: copyleft license ({spdx}) requires written source_offer=True for commercial redistribution"
            )

    return (len(issues) == 0), issues


def verify_notices_file(
    components: Sequence[Dict[str, Any]],
    notices_path: Path,
) -> Tuple[bool, List[str]]:
    if not notices_path.is_file():
        return False, [f"missing notices file: {notices_path}"]
    text = notices_path.read_text(encoding="utf-8")
    issues: List[str] = []
    for comp in components:
        name = str(comp["name"])
        spdx = str(comp["spdx"])
        if name.lower() not in text.lower():
            issues.append(f"{notices_path.name}: missing attribution entry for {name}")
        if spdx not in text:
            issues.append(f"{notices_path.name}: missing SPDX identifier {spdx} for {name}")
    if "source offer" not in text.lower() and "source code" not in text.lower():
        issues.append(f"{notices_path.name}: missing GPL/LGPL written source-offer section")
    return (len(issues) == 0), issues


def generate_spdx_sbom(components: Sequence[Dict[str, Any]]) -> Dict[str, Any]:
    created = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    packages = []
    for comp in components:
        pkg_id = "SPDXRef-Package-" + re.sub(r"[^A-Za-z0-9.-]+", "-", str(comp["name"]))
        packages.append(
            {
                "SPDXID": pkg_id,
                "name": comp["name"],
                "versionInfo": comp.get("version", "unknown"),
                "downloadLocation": comp.get("homepage", "NOASSERTION"),
                "filesAnalyzed": False,
                "licenseConcluded": comp["spdx"],
                "licenseDeclared": comp["spdx"],
                "copyrightText": "See THIRD_PARTY_NOTICES.md",
                "comment": f"scope={comp.get('scope', 'unknown')}, bundled={comp.get('bundled', False)}, source_offer={comp.get('source_offer', False)}",
            }
        )
    return {
        "spdxVersion": "SPDX-2.3",
        "dataLicense": "CC0-1.0",
        "SPDXID": "SPDXRef-DOCUMENT",
        "name": "pico-universal-programmer-v1.0.0",
        "documentNamespace": "https://github.com/nfsahsn/pico-universal-programmer/spdx/v1.0.0",
        "creationInfo": {
            "created": created,
            "creators": ["Tool: pico-universal-license-audit-1.0.0"],
        },
        "packages": packages,
    }


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Audit third-party licenses and emit SPDX 2.3 SBOM.")
    p.add_argument("--verify", action="store_true", help="Verify component compliance and THIRD_PARTY_NOTICES.md.")
    p.add_argument(
        "--notices",
        type=Path,
        default=ROOT / "THIRD_PARTY_NOTICES.md",
        help="Path to THIRD_PARTY_NOTICES.md.",
    )
    p.add_argument(
        "--sbom-out",
        type=Path,
        default=None,
        help="Optional output path for SPDX 2.3 JSON SBOM (e.g. release/sbom.spdx.json).",
    )
    return p.parse_args(argv)


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    ok_comp, comp_issues = audit_components(DEFAULT_COMPONENTS)
    ok_notices, notice_issues = (True, [])
    if args.verify:
        ok_notices, notice_issues = verify_notices_file(DEFAULT_COMPONENTS, args.notices)

    all_issues = comp_issues + notice_issues
    if all_issues:
        for msg in all_issues:
            print(f"FAIL: {msg}")
        return 1

    if args.sbom_out is not None:
        args.sbom_out.parent.mkdir(parents=True, exist_ok=True)
        sbom = generate_spdx_sbom(DEFAULT_COMPONENTS)
        args.sbom_out.write_text(json.dumps(sbom, indent=2) + "\n", encoding="utf-8")

    print(f"License audit PASS ({len(DEFAULT_COMPONENTS)} components verified against SPDX 2.3 & notices)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
