import hashlib
import json
import re
import shutil
import subprocess
import sys
import unittest
from functools import lru_cache
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


@lru_cache(maxsize=1)
def discover_c_cxx_compilers() -> tuple[list[str] | None, list[str] | None]:
    c_cmd: list[str] | None = None
    cxx_cmd: list[str] | None = None

    for cand in ("cc", "gcc", "clang"):
        found = shutil.which(cand)
        if found:
            c_cmd = [found]
            break
    for cand in ("c++", "g++", "clang++"):
        found = shutil.which(cand)
        if found:
            cxx_cmd = [found]
            break

    if c_cmd is None or cxx_cmd is None:
        arduino_bins = sorted(
            (Path.home() / ".arduino15" / "packages" / "rp2040" / "tools" / "pqt-gcc").glob("*/bin"),
            reverse=True,
        )
        for bin_dir in arduino_bins:
            arm_gcc = bin_dir / "arm-none-eabi-gcc"
            arm_gxx = bin_dir / "arm-none-eabi-g++"
            if c_cmd is None and arm_gcc.is_file():
                c_cmd = [str(arm_gcc)]
            if cxx_cmd is None and arm_gxx.is_file():
                cxx_cmd = [str(arm_gxx)]
            if c_cmd and cxx_cmd:
                break

    if (c_cmd is None or cxx_cmd is None) and shutil.which("toolbox"):
        probe_c = subprocess.run(
            ["toolbox", "run", "-c", "dsd", "gcc", "--version"],
            capture_output=True,
            text=True,
        )
        if c_cmd is None and probe_c.returncode == 0:
            c_cmd = ["toolbox", "run", "-c", "dsd", "gcc"]
        probe_cxx = subprocess.run(
            ["toolbox", "run", "-c", "dsd", "g++", "--version"],
            capture_output=True,
            text=True,
        )
        if cxx_cmd is None and probe_cxx.returncode == 0:
            cxx_cmd = ["toolbox", "run", "-c", "dsd", "g++"]

    return c_cmd, cxx_cmd


def _eval_c_int(token: str, defines: dict[str, int | str]) -> int:
    expr = token.strip()
    if not expr:
        return 1

    def repl_ident(match: re.Match[str]) -> str:
        ident = match.group(0)
        if ident.startswith(("0x", "0X")) or ident[0].isdigit():
            return re.sub(r"[uUlL]+$", "", ident)
        return str(defines.get(ident, 0))

    expr = re.sub(r"\b(?:0[xX][0-9a-fA-F]+|\d+)[uUlL]*\b|\b[A-Za-z_][A-Za-z0-9_]*\b", repl_ident, expr)
    if not re.fullmatch(r"[0-9xXa-fA-F+\-*/%<>!=&|^~()\s]+", expr):
        raise ValueError(f"Unsupported macro expression: {token!r}")
    return int(eval(expr, {"__builtins__": {}}, {}))


def _eval_c_cond(expr: str, defines: dict[str, int | str]) -> bool:
    s = re.sub(
        r"\bdefined\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)|\bdefined\s+([A-Za-z_][A-Za-z0-9_]*)\b",
        lambda m: "1" if (m.group(1) or m.group(2)) in defines else "0",
        expr,
    )

    def repl_token(match: re.Match[str]) -> str:
        tok = match.group(0)
        if tok.startswith(("0x", "0X")) or tok[0].isdigit():
            return re.sub(r"[uUlL]+$", "", tok)
        return str(defines.get(tok, 0))

    s = re.sub(r"\b(?:0[xX][0-9a-fA-F]+|\d+)[uUlL]*\b|\b[A-Za-z_][A-Za-z0-9_]*\b", repl_token, s)
    s = s.replace("&&", " and ").replace("||", " or ")
    s = re.sub(r"!(?!=)", " not ", s)
    return bool(eval(s, {"__builtins__": {}}, {}))


def evaluate_c_header_defines(
    header_path: Path,
    initial_defines: dict[str, int | str] | None = None,
) -> dict[str, int | str]:
    text = header_path.read_text(encoding="utf-8")
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)

    defines: dict[str, int | str] = dict(initial_defines or {})
    # Each frame is (parent_active, current_branch_active, any_branch_taken)
    stack: list[tuple[bool, bool, bool]] = []

    def is_active() -> bool:
        return all(frame[1] for frame in stack)

    logical_lines: list[str] = []
    buf = ""
    for raw_line in text.splitlines():
        if raw_line.endswith("\\"):
            buf += raw_line[:-1] + " "
        else:
            logical_lines.append(buf + raw_line)
            buf = ""
    if buf:
        logical_lines.append(buf)

    for line in logical_lines:
        stripped = line.strip()
        if not stripped.startswith("#"):
            continue
        directive = stripped[1:].strip()
        parts = directive.split(None, 1)
        if not parts:
            continue
        kw = parts[0]
        rest = parts[1].strip() if len(parts) > 1 else ""

        if kw == "ifdef":
            parent = is_active()
            cond = parent and (rest in defines)
            stack.append((parent, cond, cond))
        elif kw == "ifndef":
            parent = is_active()
            cond = parent and (rest not in defines)
            stack.append((parent, cond, cond))
        elif kw == "if":
            parent = is_active()
            cond = parent and _eval_c_cond(rest, defines)
            stack.append((parent, cond, cond))
        elif kw == "elif":
            parent, _, taken = stack.pop()
            cond = parent and (not taken) and _eval_c_cond(rest, defines)
            stack.append((parent, cond, taken or cond))
        elif kw == "else":
            parent, _, taken = stack.pop()
            cond = parent and (not taken)
            stack.append((parent, cond, True))
        elif kw == "endif":
            stack.pop()
        elif is_active():
            if kw == "error":
                raise RuntimeError(f"Preprocessor #error: {rest}")
            if kw == "define":
                m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)(?:\s+(.*))?$", rest)
                if m and "(" not in rest.split()[0]:
                    name = m.group(1)
                    val_str = (m.group(2) or "").strip()
                    defines[name] = "" if val_str == "" else _eval_c_int(val_str, defines)
            elif kw == "undef":
                defines.pop(rest, None)

    if stack:
        raise AssertionError("Unterminated preprocessor conditional block")
    return defines


class FoundationBaselineTests(unittest.TestCase):
    def setUp(self) -> None:
        self.build_root = ROOT / "build"
        self.build_existed = self.build_root.exists()
        self.scratch_dir = self.build_root / "test_flash_layout_scratch"
        if self.scratch_dir.exists():
            shutil.rmtree(self.scratch_dir)
        self.scratch_dir.mkdir(parents=True, exist_ok=True)

    def tearDown(self) -> None:
        if self.scratch_dir.exists():
            shutil.rmtree(self.scratch_dir)
        if not self.build_existed and self.build_root.exists() and not any(self.build_root.iterdir()):
            self.build_root.rmdir()

    def test_flash_layout_geometry_and_partitions(self) -> None:
        layout_path = ROOT / "config" / "flash_layout.json"
        self.assertTrue(layout_path.is_file(), "config/flash_layout.json must exist")

        layout = json.loads(layout_path.read_text(encoding="utf-8"))
        self.assertEqual(layout["flash_base"], 0x10000000)
        self.assertEqual(layout["flash_size"], 2097152)
        self.assertEqual(layout["page_size"], 256)
        self.assertEqual(layout["sector_size"], 4096)

        expected_partitions = [
            ("bootloader", "SLOT_BOOTLOADER", 0x000000, 61440, True),
            ("journal_a", "SLOT_JOURNAL_A", 0x00F000, 4096, False),
            ("app_slot_a", "SLOT_APP_A", 0x010000, 983040, True),
            ("app_slot_b", "SLOT_APP_B", 0x100000, 983040, True),
            ("journal_b", "SLOT_JOURNAL_B", 0x1F0000, 4096, False),
            ("factory_meta", "SLOT_FACTORY_META", 0x1FF000, 4096, False),
        ]

        parts = layout["partitions"]
        self.assertEqual(len(parts), 6)
        self.assertEqual([p["name"] for p in parts], [e[0] for e in expected_partitions])

        sector = layout["sector_size"]
        cursor = 0
        for part, (exp_name, exp_macro, exp_offset, exp_size, exp_exec) in zip(parts, expected_partitions):
            self.assertEqual(part["name"], exp_name)
            self.assertEqual(part["macro"], exp_macro)
            self.assertEqual(part["offset"], exp_offset)
            self.assertEqual(part["size"], exp_size)
            self.assertIs(part["executable"], exp_exec)
            self.assertEqual(part["offset"] % sector, 0, f"{part['name']} offset not 4 KiB aligned")
            self.assertEqual(part["size"] % sector, 0, f"{part['name']} size not 4 KiB aligned")
            self.assertGreaterEqual(part["offset"], cursor, f"{part['name']} overlaps previous partition")
            cursor = part["offset"] + part["size"]

        self.assertLessEqual(cursor, layout["flash_size"])

    def test_gen_flash_layout_matches_committed_header_and_macros(self) -> None:
        script_path = ROOT / "tools" / "gen_flash_layout.py"
        layout_path = ROOT / "config" / "flash_layout.json"
        committed_header = ROOT / "config" / "flash_layout.h"

        self.assertTrue(script_path.is_file(), "tools/gen_flash_layout.py must exist")
        self.assertTrue(committed_header.is_file(), "config/flash_layout.h must exist")

        generated_out = self.scratch_dir / "flash_layout.h"
        subprocess.run(
            [
                sys.executable,
                str(script_path),
                "--in",
                str(layout_path),
                "--out",
                str(generated_out),
            ],
            check=True,
        )
        self.assertEqual(
            generated_out.read_text(encoding="utf-8"),
            committed_header.read_text(encoding="utf-8"),
        )

        subprocess.run(
            [sys.executable, str(script_path), "--check"],
            check=True,
        )

        defines = evaluate_c_header_defines(committed_header)
        self.assertEqual(defines["FLASH_BASE_ADDR"], 0x10000000)
        self.assertEqual(defines["FLASH_TOTAL_SIZE"], 2097152)
        self.assertEqual(defines["FLASH_PAGE_SIZE"], 256)
        self.assertEqual(defines["FLASH_SECTOR_SIZE"], 4096)

        expected_macros = {
            "SLOT_BOOTLOADER": (0x000000, 61440),
            "SLOT_JOURNAL_A": (0x00F000, 4096),
            "SLOT_APP_A": (0x010000, 983040),
            "SLOT_APP_B": (0x100000, 983040),
            "SLOT_JOURNAL_B": (0x1F0000, 4096),
            "SLOT_FACTORY_META": (0x1FF000, 4096),
        }
        for macro, (offset, size) in expected_macros.items():
            self.assertEqual(defines[f"{macro}_OFFSET"], offset)
            self.assertEqual(defines[f"{macro}_ADDR"], 0x10000000 + offset)
            self.assertEqual(defines[f"{macro}_SIZE"], size)

    def test_gen_flash_layout_rejects_invalid_layouts(self) -> None:
        script_path = ROOT / "tools" / "gen_flash_layout.py"
        layout_path = ROOT / "config" / "flash_layout.json"
        self.assertTrue(script_path.is_file(), "tools/gen_flash_layout.py must exist")
        self.assertTrue(layout_path.is_file(), "config/flash_layout.json must exist")

        base_layout = json.loads(layout_path.read_text(encoding="utf-8"))

        overlapping = json.loads(json.dumps(base_layout))
        overlapping["partitions"][2]["offset"] = overlapping["partitions"][1]["offset"]
        bad_overlap_json = self.scratch_dir / "overlap.json"
        bad_overlap_json.write_text(json.dumps(overlapping), encoding="utf-8")
        res_overlap = subprocess.run(
            [
                sys.executable,
                str(script_path),
                "--in",
                str(bad_overlap_json),
                "--out",
                str(self.scratch_dir / "bad1.h"),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(res_overlap.returncode, 0)

        unaligned = json.loads(json.dumps(base_layout))
        unaligned["partitions"][1]["offset"] = 61440 + 256
        bad_align_json = self.scratch_dir / "unaligned.json"
        bad_align_json.write_text(json.dumps(unaligned), encoding="utf-8")
        res_align = subprocess.run(
            [
                sys.executable,
                str(script_path),
                "--in",
                str(bad_align_json),
                "--out",
                str(self.scratch_dir / "bad2.h"),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(res_align.returncode, 0)

    def test_board_pinout_profiles_and_usb_identity(self) -> None:
        pinout_path = ROOT / "config" / "board_pinout.h"
        self.assertTrue(pinout_path.is_file(), "config/board_pinout.h must exist")

        # 1. Default profile selects PU_BOARD_PICO and default placeholder PU_USB_PID
        pico_defs = evaluate_c_header_defines(pinout_path)
        self.assertEqual(pico_defs.get("PU_BOARD_PICO"), 1)
        self.assertNotIn("PU_BOARD_PRO_PCB", pico_defs)
        self.assertEqual(pico_defs.get("PU_USB_VID"), 0x2E8A)
        self.assertEqual(pico_defs.get("PU_USB_PID"), 0x0000)

        expected_pico = {
            "PU_HAS_LEVEL_SHIFTER": 0,
            "PU_HAS_VTARG_SWITCH": 0,
            "PU_HAS_ADC_TELEMETRY": 0,
            "PU_PIN_UART_TX": 0,
            "PU_PIN_UART_RX": 1,
            "PU_PIN_SWCLK_TCK": 2,
            "PU_PIN_SWDIO_TMS": 3,
            "PU_PIN_JTAG_TDI": 4,
            "PU_PIN_JTAG_TDO": 5,
            "PU_PIN_TARGET_NRST": 6,
            "PU_PIN_RECOVERY_BTN": 15,
            "PU_PIN_LED_STATUS": 25,
            "PU_PIN_SWIO": 28,
        }
        for key, val in expected_pico.items():
            self.assertEqual(pico_defs.get(key), val, f"PU_BOARD_PICO mismatch for {key}")

        pico_pins = [v for k, v in pico_defs.items() if k.startswith("PU_PIN_")]
        self.assertEqual(len(pico_pins), len(set(pico_pins)), "Duplicate GPIO assignment in PU_BOARD_PICO")

        # 2. PU_BOARD_PRO_PCB profile with injected approved PU_USB_PID
        pro_defs = evaluate_c_header_defines(
            pinout_path,
            {"PU_BOARD_PRO_PCB": 1, "PU_USB_PID": 0x1050},
        )
        self.assertEqual(pro_defs.get("PU_BOARD_PRO_PCB"), 1)
        self.assertNotIn("PU_BOARD_PICO", pro_defs)
        self.assertEqual(pro_defs.get("PU_USB_VID"), 0x2E8A)
        self.assertEqual(pro_defs.get("PU_USB_PID"), 0x1050)

        expected_pro = {
            "PU_HAS_LEVEL_SHIFTER": 1,
            "PU_HAS_VTARG_SWITCH": 1,
            "PU_HAS_ADC_TELEMETRY": 1,
            "PU_PIN_UART_TX": 0,
            "PU_PIN_UART_RX": 1,
            "PU_PIN_SWCLK_TCK": 2,
            "PU_PIN_SWDIO_TMS": 3,
            "PU_PIN_JTAG_TDI": 4,
            "PU_PIN_JTAG_TDO": 5,
            "PU_PIN_SWDIO_DIR": 6,
            "PU_PIN_TARGET_NRST": 7,
            "PU_PIN_SWIO": 8,
            "PU_PIN_SWIO_PU_EN": 9,
            "PU_PIN_VTARG_EN": 10,
            "PU_PIN_VTARG_FAULT_N": 11,
            "PU_PIN_LED_R": 12,
            "PU_PIN_LED_G": 13,
            "PU_PIN_LED_B": 14,
            "PU_PIN_RECOVERY_BTN": 15,
            "PU_PIN_ADC_VTREF": 26,
            "PU_PIN_ADC_ITARG": 27,
        }
        for key, val in expected_pro.items():
            self.assertEqual(pro_defs.get(key), val, f"PU_BOARD_PRO_PCB mismatch for {key}")

        pro_pins = [v for k, v in pro_defs.items() if k.startswith("PU_PIN_")]
        self.assertEqual(len(pro_pins), len(set(pro_pins)), "Duplicate GPIO assignment in PU_BOARD_PRO_PCB")

        # 3. Defining both profiles simultaneously must fail compilation
        with self.assertRaises(RuntimeError):
            evaluate_c_header_defines(pinout_path, {"PU_BOARD_PICO": 1, "PU_BOARD_PRO_PCB": 1})

    def test_board_pinout_rejects_invalid_usb_pid(self) -> None:
        pinout_path = ROOT / "config" / "board_pinout.h"
        flash_header_path = ROOT / "config" / "flash_layout.h"
        self.assertTrue(pinout_path.is_file(), "config/board_pinout.h must exist")

        # Valid 16-bit boundaries succeed cleanly in preprocessor evaluation
        self.assertEqual(evaluate_c_header_defines(pinout_path, {"PU_USB_PID": 0}).get("PU_USB_PID"), 0)
        self.assertEqual(evaluate_c_header_defines(pinout_path, {"PU_USB_PID": 0xFFFF}).get("PU_USB_PID"), 0xFFFF)

        # Negative, > 0xFFFF, and empty PU_USB_PID must trigger clean #error
        for bad_val in (-1, 0x10000, ""):
            with self.subTest(bad_val=bad_val):
                with self.assertRaisesRegex(RuntimeError, r"Preprocessor #error:"):
                    evaluate_c_header_defines(pinout_path, {"PU_USB_PID": bad_val})

        c_cmd, cxx_cmd = discover_c_cxx_compilers()
        compilers: list[tuple[list[str], str, str]] = []
        if c_cmd is not None:
            compilers.append((c_cmd, "-std=c11", "c"))
        if cxx_cmd is not None:
            compilers.append((cxx_cmd, "-std=c++17", "c++"))

        for cmd_prefix, std_flag, lang in compilers:
            base_flags = [std_flag, "-Wall", "-Wextra", "-Werror", "-Wundef", "-fsyntax-only", "-x", lang]
            for valid_extra, hdr in (
                ([], flash_header_path),
                ([], pinout_path),
                (["-DPU_BOARD_PRO_PCB=1", "-DPU_USB_PID=0x1050u"], pinout_path),
                (["-DPU_USB_PID=0"], pinout_path),
                (["-DPU_USB_PID=0xFFFF"], pinout_path),
            ):
                with self.subTest(compiler=cmd_prefix[0], lang=lang, valid_extra=valid_extra, hdr=hdr.name):
                    res = subprocess.run(
                        [*cmd_prefix, *base_flags, *valid_extra, str(hdr)],
                        capture_output=True,
                        text=True,
                    )
                    self.assertEqual(
                        res.returncode,
                        0,
                        f"Expected clean compile for {hdr.name} ({valid_extra}):\n{res.stderr}",
                    )
                    self.assertEqual(res.stderr.strip(), "")

            for bad_define in ("-DPU_USB_PID=-1", "-DPU_USB_PID=0x10000", "-DPU_USB_PID="):
                with self.subTest(compiler=cmd_prefix[0], lang=lang, bad_define=bad_define):
                    res = subprocess.run(
                        [*cmd_prefix, *base_flags, bad_define, str(pinout_path)],
                        capture_output=True,
                        text=True,
                    )
                    self.assertNotEqual(res.returncode, 0, f"{bad_define} should fail compilation")
                    self.assertIn("#error", res.stderr)
                    self.assertNotIn("changes sign", res.stderr)
                    self.assertNotIn("sign-compare", res.stderr)
                    self.assertNotIn("no left operand", res.stderr)

    def test_catalog_targets_json_contains_four_initial_families(self) -> None:
        catalog_path = ROOT / "catalog" / "targets.json"
        self.assertTrue(catalog_path.is_file(), "catalog/targets.json must exist")

        catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
        cores = catalog.get("cores", {})
        profiles = catalog.get("profiles", {})
        boards = catalog.get("boards", [])

        required_families = {"stm32", "rp2040", "esp32", "ch32v"}
        self.assertTrue(required_families.issubset(set(cores.keys())))

        initial_board_cores = {
            board["core"]
            for board in boards
            if board.get("phase") == "initial"
        }
        self.assertEqual(initial_board_cores, required_families)

        # Every board must reference a valid core and profile with unique IDs
        self.assertGreaterEqual(len(boards), 1)
        board_ids = [b["id"] for b in boards]
        self.assertEqual(len(board_ids), len(set(board_ids)), "Duplicate board IDs in catalog/targets.json")
        for board in boards:
            self.assertIn(board["core"], cores, f"Board {board['id']} references unknown core {board['core']!r}")
            self.assertIn(
                board["profile"],
                profiles,
                f"Board {board['id']} references unknown profile {board['profile']!r}",
            )
            core_pkg = cores[board["core"]]["package"]
            self.assertTrue(
                board["fqbn"].startswith(core_pkg + ":"),
                f"Board {board['id']} FQBN {board['fqbn']!r} does not match core package {core_pkg!r}",
            )

        # Every profile must declare an allowed transport and valid tool_family / configs
        allowed_transports = {"swd", "jtag", "swio"}
        allowed_tool_families = {"stm32", "rp2040", "esp32", "riscv-gdb"}
        self.assertGreaterEqual(len(profiles), 1)
        for prof_name, prof in profiles.items():
            self.assertIn(
                prof.get("transport"),
                allowed_transports,
                f"Profile {prof_name!r} uses invalid transport {prof.get('transport')!r}",
            )
            self.assertIn(
                prof.get("tool_family"),
                allowed_tool_families,
                f"Profile {prof_name!r} uses unknown tool_family {prof.get('tool_family')!r}",
            )
            if "openocd_cfg" in prof:
                cfg_rel = ROOT / prof["openocd_cfg"]
                self.assertTrue(
                    cfg_rel.is_file(),
                    f"Profile {prof_name!r} openocd_cfg {prof['openocd_cfg']!r} does not exist",
                )

        # CH32V003 uses 1-wire WCH SWIO over composite firmware, never SWD
        self.assertIn("ch32v003", profiles)
        self.assertEqual(profiles["ch32v003"].get("transport"), "swio")
        self.assertIn(profiles["ch32v003"].get("mode"), {"composite-swio", "swio"})

        # Classic ESP32 profile must reference catalog/openocd/pu-esp32.cfg and matching 5000 kHz JTAG clock
        self.assertIn("esp32", profiles)
        self.assertEqual(profiles["esp32"].get("openocd_cfg"), "catalog/openocd/pu-esp32.cfg")
        self.assertTrue((ROOT / profiles["esp32"]["openocd_cfg"]).is_file())
        self.assertEqual(profiles["esp32"].get("speed_khz"), 5000)

    def test_dependencies_lock_matches_catalog_and_pins_valid_sha256_digests(self) -> None:
        catalog_path = ROOT / "catalog" / "targets.json"
        lock_path = ROOT / "catalog" / "dependencies.lock.json"
        self.assertTrue(catalog_path.is_file(), "catalog/targets.json must exist")
        self.assertTrue(lock_path.is_file(), "catalog/dependencies.lock.json must exist")

        catalog_bytes = catalog_path.read_bytes()
        expected_catalog_sha = hashlib.sha256(catalog_bytes).hexdigest()

        catalog = json.loads(catalog_bytes.decode("utf-8"))
        lock = json.loads(lock_path.read_text(encoding="utf-8"))

        self.assertEqual(lock.get("schema_version"), 1)
        self.assertEqual(
            lock.get("catalog_sha256"),
            expected_catalog_sha,
            "catalog/dependencies.lock.json catalog_sha256 does not match catalog/targets.json",
        )

        # Every core in catalog/targets.json must be locked with matching version/package and valid artifact
        artifacts = lock.get("artifacts", {})
        self.assertGreater(len(artifacts), 0)
        for core_key, core_spec in catalog["cores"].items():
            self.assertIn(core_key, lock.get("cores", {}))
            locked_core = lock["cores"][core_key]
            self.assertEqual(locked_core["package"], core_spec["package"])
            self.assertEqual(locked_core["version"], core_spec["version"])
            self.assertIn(locked_core["artifact"], artifacts)

        # Every tool and release must pin both linux-x86_64 and windows-x86_64 artifacts
        hosts = ("linux-x86_64", "windows-x86_64")
        self.assertGreater(len(lock.get("tools", [])), 0)
        for tool in lock["tools"]:
            for host in hosts:
                aid = tool["artifacts"].get(host)
                self.assertIsNotNone(aid, f"Missing {host} artifact for tool {tool['name']}")
                self.assertIn(aid, artifacts)

        for rel_name, rel in lock.get("releases", {}).items():
            for host in hosts:
                aid = rel["artifacts"].get(host)
                self.assertIsNotNone(aid, f"Missing {host} artifact for release {rel_name}")
                self.assertIn(aid, artifacts)

        # Recursively verify all sha256 digest fields across the lockfile
        hex64_re = re.compile(r"^[0-9a-f]{64}$")
        digest_count = 0

        def walk_digests(node: object, path: str = "") -> None:
            nonlocal digest_count
            if isinstance(node, dict):
                for k, v in node.items():
                    subpath = f"{path}.{k}" if path else k
                    if "sha256" in k:
                        self.assertIsInstance(v, str, f"{subpath} must be a hex string")
                        self.assertRegex(v, hex64_re, f"Invalid SHA-256 digest at {subpath}: {v!r}")
                        digest_count += 1
                    walk_digests(v, subpath)
            elif isinstance(node, list):
                for idx, item in enumerate(node):
                    walk_digests(item, f"{path}[{idx}]")

        walk_digests(lock)
        self.assertGreaterEqual(digest_count, 100)

    def test_openocd_pu_esp32_cfg_workarounds_present(self) -> None:
        cfg_path = ROOT / "catalog" / "openocd" / "pu-esp32.cfg"
        self.assertTrue(cfg_path.is_file(), "catalog/openocd/pu-esp32.cfg must exist")

        content = cfg_path.read_text(encoding="utf-8")
        for required_token in ("ESP32_FLASH_VOLTAGE", "gdb-attach", "examine-end", "gdb-detach"):
            self.assertIn(required_token, content)

    def test_linux_udev_rules_valid_and_complete(self) -> None:
        rules_path = ROOT / "linux" / "60-pico-universal.rules"
        self.assertTrue(rules_path.is_file(), "linux/60-pico-universal.rules must exist")

        content = rules_path.read_text(encoding="utf-8")
        lines = content.splitlines()

        remove_guard_indices = [
            idx
            for idx, line in enumerate(lines)
            if not line.lstrip().startswith("#")
            and 'ACTION=="remove"' in line
            and 'GOTO="pico_universal_end"' in line
        ]
        label_end_indices = [
            idx
            for idx, line in enumerate(lines)
            if not line.lstrip().startswith("#") and 'LABEL="pico_universal_end"' in line
        ]
        template_indices = [
            idx
            for idx, line in enumerate(lines)
            if line.lstrip().startswith("#") and "<pu_usb_pid>" in line and "SUBSYSTEM==" in line
        ]
        self.assertEqual(len(remove_guard_indices), 1, "Expected single ACTION==\"remove\" guard")
        self.assertEqual(len(label_end_indices), 1, "Expected single LABEL=\"pico_universal_end\"")
        self.assertGreaterEqual(len(template_indices), 2, "Expected commented <pu_usb_pid> template rules")
        for t_idx in template_indices:
            self.assertGreater(
                t_idx,
                remove_guard_indices[0],
                "<pu_usb_pid> template rules must appear BELOW ACTION==\"remove\" guard",
            )
            self.assertLess(
                t_idx,
                label_end_indices[0],
                "<pu_usb_pid> template rules must appear ABOVE LABEL=\"pico_universal_end\"",
            )

        active_rules = [
            line.strip()
            for line in lines
            if line.strip() and not line.lstrip().startswith("#")
        ]
        self.assertGreaterEqual(len(active_rules), 1)

        vendor_2e8a_rules = 0
        for rule in active_rules:
            clauses = [c.strip() for c in rule.split(",")]
            for clause in clauses:
                self.assertRegex(
                    clause,
                    r'^[A-Za-z0-9_{}\[\]\-]+(?:==|!=|\+=|=|:=)"[^"]*"$',
                    f"Malformed udev rule clause: {clause!r}",
                )
            if 'ATTRS{idVendor}=="2e8a"' in clauses or 'ATTR{idVendor}=="2e8a"' in clauses:
                vendor_2e8a_rules += 1
                has_specific_pid = any(
                    re.fullmatch(r'ATTRS?\{idProduct\}=="[0-9a-f]{4}"', c) for c in clauses
                )
                has_product_prefix = 'ATTRS{product}=="Pico Universal*"' in clauses
                self.assertTrue(
                    has_specific_pid or has_product_prefix,
                    f"Active 2e8a rule must constrain idProduct or Pico Universal* product string: {rule!r}",
                )
                self.assertIn('TAG+="uaccess"', clauses, f"Active 2e8a rule missing uaccess tag: {rule!r}")
                self.assertIn(
                    'ENV{ID_MM_DEVICE_IGNORE}="1"',
                    clauses,
                    f"Active 2e8a rule missing ModemManager ignore env: {rule!r}",
                )

        self.assertGreaterEqual(vendor_2e8a_rules, 1, "Expected active 2e8a udev rules")

        udevadm = shutil.which("udevadm")
        if udevadm:
            res = subprocess.run(
                [udevadm, "verify", "--resolve-names=never", str(rules_path)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(res.returncode, 0, f"udevadm verify failed:\n{res.stderr}\n{res.stdout}")

    def test_ci_workflow_pins_actions_and_checks_mixed_c_cxx_and_windows(self) -> None:
        ci_path = ROOT / ".github" / "workflows" / "ci.yml"
        self.assertTrue(ci_path.is_file(), ".github/workflows/ci.yml must exist")
        ci_text = ci_path.read_text(encoding="utf-8")

        # Every `uses:` action must be pinned to a full 40-char commit SHA
        uses_refs = [
            m.group(1)
            for line in ci_text.splitlines()
            if (m := re.match(r"^\s*-?\s*uses:\s*([^\s#]+)", line))
        ]
        self.assertGreaterEqual(len(uses_refs), 6)
        for ref in uses_refs:
            self.assertRegex(
                ref,
                r"^[A-Za-z0-9_.\-]+/[A-Za-z0-9_.\-]+@[0-9a-f]{40}$",
                f"GitHub Action must be pinned to a 40-char commit SHA: {ref!r}",
            )

        linux_part, windows_part = ci_text.split("verify-windows:", 1)

        # Linux job must enforce -Wundef, check default C++17 board_pinout.h, and compile C tests to .o then link with c++
        self.assertIn("-Wundef", linux_part)
        self.assertRegex(
            linux_part,
            r"c\+\+\s+-std=c\+\+17\s+-Wall\s+-Wextra\s+-Werror\s+-Wundef\s+-fsyntax-only\s+-x\s+c\+\+\s+config/board_pinout\.h",
        )
        self.assertRegex(
            linux_part,
            re.compile(
                r'for tsrc in tests/host_c/test_\*\.c;.*?cc\s+-std=c11[^\n]*-c\s+"\$tsrc"\s+-o\s+"\$tobj".*?c\+\+[^\n]*"\$tobj"\s+"\$\{lib_args\[@\]\}"\s+-o\s+"\$bin"',
                re.DOTALL,
            ),
        )

        # Windows job must also check C/C++ headers with -Wundef and compile/run tests/host_c when present
        self.assertIn("-Wundef", windows_part)
        self.assertIn("config/board_pinout.h", windows_part)
        self.assertIn("tests/host_c", windows_part)

        # Every host C/C++ compilation command (CMake C/CXX flags + 4 fallback compile commands)
        # in both verify-linux and verify-windows must pass -DPU_HOST_TEST=1.
        for job_name, job_part in (("verify-linux", linux_part), ("verify-windows", windows_part)):
            host_step = job_part.split("tests/host_c/run_tests.sh", 1)[1]
            self.assertGreaterEqual(
                host_step.count("-DPU_HOST_TEST=1"),
                6,
                f"{job_name} host C/C++ step must pass -DPU_HOST_TEST=1 in CMake and all fallback compile commands",
            )
            for line in host_step.splitlines():
                stripped = line.strip()
                if stripped.startswith(("-DCMAKE_C_FLAGS=", "-DCMAKE_CXX_FLAGS=")) or (
                    " -c " in stripped and ("$src" in stripped or "$tsrc" in stripped)
                ):
                    self.assertIn(
                        "-DPU_HOST_TEST=1",
                        stripped,
                        f"{job_name} compile line missing -DPU_HOST_TEST=1: {stripped!r}",
                    )


if __name__ == "__main__":
    unittest.main()
