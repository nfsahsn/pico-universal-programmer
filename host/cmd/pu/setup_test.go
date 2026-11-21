package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/nfsahsn/pico-universal-programmer/host/internal/usbprobe"
)

func testRepoRoot(t *testing.T) string {
	t.Helper()
	abs, err := filepath.Abs(filepath.Join("..", "..", ".."))
	if err != nil {
		t.Fatalf("resolve repo root: %v", err)
	}
	return abs
}

var testCoreRelDirs = []struct {
	family   string
	pkg      string
	version  string
	relDir   string
	profiles []string
}{
	{"stm32", "STMicroelectronics:stm32", "2.12.0", filepath.Join("packages", "STMicroelectronics", "hardware", "stm32", "2.12.0"), []string{"stm32f1", "stm32f4"}},
	{"rp2040", "rp2040:rp2040", "6.1.1", filepath.Join("packages", "rp2040", "hardware", "rp2040", "6.1.1"), []string{"rp2040", "rp2350"}},
	{"esp32", "esp32:esp32", "3.3.11", filepath.Join("packages", "esp32", "hardware", "esp32", "3.3.11"), []string{"esp32", "esp32s3"}},
	{"ch32v", "WCH:ch32v", "1.0.4", filepath.Join("packages", "WCH", "hardware", "ch32v", "1.0.4"), []string{"ch32v003"}},
}

func seedInstalledCores(t *testing.T, arduinoDir string) {
	t.Helper()
	for _, c := range testCoreRelDirs {
		hwDir := filepath.Join(arduinoDir, c.relDir)
		if err := os.MkdirAll(hwDir, 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(hwDir, "boards.txt"), []byte("# "+c.family+" boards\n"), 0o644); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(hwDir, "platform.txt"), []byte("name="+c.family+"\nversion="+c.version+"\n"), 0o644); err != nil {
			t.Fatal(err)
		}
	}
}

type recordedCmd struct {
	name string
	args []string
	env  []string
}

type fakeCommandRunner struct {
	bins  map[string]string
	onRun func(cmd commandSpec, stdout, stderr io.Writer) error
	ran   []recordedCmd
}

func (f *fakeCommandRunner) LookPath(file string) (string, error) {
	if p, ok := f.bins[file]; ok {
		return p, nil
	}
	return "", fmt.Errorf("executable %q not found in PATH", file)
}

func (f *fakeCommandRunner) Run(cmd commandSpec, stdout, stderr io.Writer) error {
	f.ran = append(f.ran, recordedCmd{
		name: cmd.Name,
		args: append([]string(nil), cmd.Args...),
		env:  append([]string(nil), cmd.Env...),
	})
	if f.onRun != nil {
		return f.onRun(cmd, stdout, stderr)
	}
	return nil
}

func TestResolveUSBPIDPrecedenceAndValidation(t *testing.T) {
	env := map[string]string{}
	getenv := func(k string) string { return env[k] }

	// Default when neither flag nor env is provided -> 0x0000
	pid, err := ResolveUSBPID("", getenv)
	if err != nil || pid != 0x0000 {
		t.Fatalf("expected default PID 0x0000, got 0x%04x err=%v", pid, err)
	}

	// Environment variable PU_USB_PID
	env["PU_USB_PID"] = "0x1050"
	pid, err = ResolveUSBPID("", getenv)
	if err != nil || pid != 0x1050 {
		t.Fatalf("expected env PID 0x1050, got 0x%04x err=%v", pid, err)
	}

	// Explicit -pid flag overrides PU_USB_PID env
	pid, err = ResolveUSBPID("1a2b", getenv)
	if err != nil || pid != 0x1A2B {
		t.Fatalf("expected flag PID 0x1a2b, got 0x%04x err=%v", pid, err)
	}

	// Invalid hex PID must fail
	if _, err := ResolveUSBPID("0xZZZZ", getenv); err == nil {
		t.Fatal("expected invalid hex PID to return error")
	}
}

func TestManagedBlockEdgeCasesTrailingNewlinesAndSiblingPrefixes(t *testing.T) {
	// Existing content WITHOUT trailing newline + sibling longer-prefix markers that must never match!
	existingNoNL := strings.Join([]string{
		"user_prog.name=User Custom Programmer",
		"# >>> PICO-UNIVERSAL-BEGIN-THIRDPARTY",
		"thirdparty.name=Do Not Touch",
		"# <<< PICO-UNIVERSAL-END-THIRDPARTY",
		"trailing.line=preserve_no_nl",
	}, "\n")

	merged, err := MergeManagedBlock(existingNoNL, "pico_universal.name=Pico Universal")
	if err != nil {
		t.Fatalf("MergeManagedBlock failed: %v", err)
	}
	if !strings.Contains(merged, "# >>> PICO-UNIVERSAL-BEGIN-THIRDPARTY\nthirdparty.name=Do Not Touch\n# <<< PICO-UNIVERSAL-END-THIRDPARTY") {
		t.Fatalf("sibling longer-prefix block corrupted:\n%s", merged)
	}
	if !strings.Contains(merged, "trailing.line=preserve_no_nl\n# >>> PICO-UNIVERSAL-BEGIN\npico_universal.name=Pico Universal\n# <<< PICO-UNIVERSAL-END\n") {
		t.Fatalf("unexpected merged output:\n%s", merged)
	}

	// Idempotent re-merge
	merged2, err := MergeManagedBlock(merged, "pico_universal.name=Pico Universal")
	if err != nil {
		t.Fatalf("second MergeManagedBlock failed: %v", err)
	}
	if merged2 != merged {
		t.Fatalf("MergeManagedBlock is not idempotent:\ngot:\n%s\nwant:\n%s", merged2, merged)
	}

	// Strip trailing newline off the managed block itself and verify RemoveManagedBlock still works cleanly!
	mergedWithoutTrailingNL := strings.TrimSuffix(merged, "\n")
	removed, err := RemoveManagedBlock(mergedWithoutTrailingNL)
	if err != nil {
		t.Fatalf("RemoveManagedBlock without trailing newline failed: %v", err)
	}
	if strings.Contains(removed, "# >>> PICO-UNIVERSAL-BEGIN\n") || strings.Contains(removed, "pico_universal.name=") {
		t.Fatalf("Pico Universal block was not removed:\n%s", removed)
	}
	if !strings.Contains(removed, "# >>> PICO-UNIVERSAL-BEGIN-THIRDPARTY") || !strings.Contains(removed, "trailing.line=preserve_no_nl") {
		t.Fatalf("third-party content lost during RemoveManagedBlock:\n%s", removed)
	}

	// Middle-of-file removal where final line has no trailing newline
	midFile := "before=1\n# >>> PICO-UNIVERSAL-BEGIN\npico=1\n# <<< PICO-UNIVERSAL-END\nafter_no_nl=2"
	midRemoved, err := RemoveManagedBlock(midFile)
	if err != nil {
		t.Fatalf("middle RemoveManagedBlock failed: %v", err)
	}
	if midRemoved != "before=1\nafter_no_nl=2" {
		t.Fatalf("unexpected middle removal result: %q", midRemoved)
	}

	// Unbalanced marker block must be rejected without corrupting user data
	if _, err := MergeManagedBlock("# >>> PICO-UNIVERSAL-BEGIN\norphan=1\n", "x=1"); err == nil {
		t.Fatal("expected unbalanced BEGIN marker to return error")
	}
	if _, err := RemoveManagedBlock("# <<< PICO-UNIVERSAL-END\n"); err == nil {
		t.Fatal("expected orphan END marker to return error")
	}
}

func TestSetupVerifyRejectsCorruptedStagingArchiveAndTamperedCatalog(t *testing.T) {
	root := testRepoRoot(t)
	catCopy := t.TempDir()
	for _, fn := range []string{"targets.json", "dependencies.lock.json"} {
		raw, err := os.ReadFile(filepath.Join(root, "catalog", fn))
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(catCopy, fn), raw, 0o644); err != nil {
			t.Fatal(err)
		}
	}

	arduinoDir := filepath.Join(t.TempDir(), "Arduino15")
	seedInstalledCores(t, arduinoDir)
	udevDir := filepath.Join(t.TempDir(), "udev")

	var out, errBuf bytes.Buffer
	if code := run([]string{"setup", "--offline", "--arduino-dir", arduinoDir, "--catalog-dir", catCopy, "--udev-dir", udevDir}, &out, &errBuf, nil); code != 0 {
		t.Fatalf("initial setup failed: %s", errBuf.String())
	}

	// Place a corrupted cached archive matching a locked filename in staging/packages.
	stagingDir := filepath.Join(arduinoDir, "staging", "packages")
	if err := os.MkdirAll(stagingDir, 0o755); err != nil {
		t.Fatal(err)
	}
	badArchive := filepath.Join(stagingDir, "CMSIS-6.2.0.tar.bz2")
	if err := os.WriteFile(badArchive, []byte("corrupted-bytes"), 0o644); err != nil {
		t.Fatal(err)
	}
	errBuf.Reset()
	if code := run([]string{"setup", "--verify", "--arduino-dir", arduinoDir, "--catalog-dir", catCopy, "--udev-dir", udevDir}, &out, &errBuf, nil); code == 0 {
		t.Fatal("expected --verify to fail on corrupted cached archive SHA-256")
	}
	_ = os.Remove(badArchive)

	// Tamper with targets.json and confirm --verify rejects catalog_sha256 mismatch.
	if err := os.WriteFile(filepath.Join(catCopy, "targets.json"), []byte("{}"), 0o644); err != nil {
		t.Fatal(err)
	}
	errBuf.Reset()
	if code := run([]string{"setup", "--verify", "--arduino-dir", arduinoDir, "--catalog-dir", catCopy, "--udev-dir", udevDir}, &out, &errBuf, nil); code == 0 {
		t.Fatal("expected --verify to fail on tampered targets.json")
	}
}

func TestSetupInstallVerifyIdempotencyAndUninstall(t *testing.T) {
	root := testRepoRoot(t)
	catDir := filepath.Join(root, "catalog")
	arduinoDir := filepath.Join(t.TempDir(), "Arduino15")
	seedInstalledCores(t, arduinoDir)
	udevDir := filepath.Join(t.TempDir(), "udev-rules")

	// Pre-populate STM32 core files with third-party content (no trailing newline on programmers.local.txt!)
	// and a third-party entry in debug_custom.json.
	stmDir := filepath.Join(arduinoDir, "packages", "STMicroelectronics", "hardware", "stm32", "2.12.0")
	thirdPartyProg := "# >>> PICO-UNIVERSAL-BEGIN-CUSTOM\ncustom_stlink.name=Custom STLink\n# <<< PICO-UNIVERSAL-END-CUSTOM"
	if err := os.WriteFile(filepath.Join(stmDir, "programmers.local.txt"), []byte(thirdPartyProg), 0o644); err != nil {
		t.Fatal(err)
	}
	thirdPartyDebug := `[{"configId":"custom-jlink","name":"Custom J-Link","servertype":"jlink"}]`
	if err := os.WriteFile(filepath.Join(stmDir, "debug_custom.json"), []byte(thirdPartyDebug), 0o644); err != nil {
		t.Fatal(err)
	}

	runCLI := func(args ...string) (int, string, string) {
		var out, errBuf bytes.Buffer
		code := run(args, &out, &errBuf, func(string) string { return "" })
		return code, out.String(), errBuf.String()
	}

	// Verify BEFORE install must fail cleanly.
	if code, _, _ := runCLI("setup", "--verify", "--arduino-dir", arduinoDir, "--catalog-dir", catDir, "--udev-dir", udevDir, "--pid", "0x1050"); code == 0 {
		t.Fatal("expected --verify before install to fail")
	}

	// 1. Run `pu setup --offline`
	code, stdout, stderr := runCLI("setup", "--offline", "--arduino-dir", arduinoDir, "--catalog-dir", catDir, "--udev-dir", udevDir, "--pid", "0x1050")
	if code != 0 {
		t.Fatalf("pu setup failed (code=%d): stdout=%s stderr=%s", code, stdout, stderr)
	}

	stagedESP32Cfg := filepath.Join(arduinoDir, "pico-universal", "openocd", "pu-esp32.cfg")
	stagedESP32Slash := filepath.ToSlash(stagedESP32Cfg)
	if _, err := os.Stat(stagedESP32Cfg); err != nil {
		t.Fatalf("expected staged pu-esp32.cfg at %s: %v", stagedESP32Cfg, err)
	}

	snapshotFirst := make(map[string]string)
	for _, fam := range testCoreRelDirs {
		coreDir := filepath.Join(arduinoDir, fam.relDir)
		for _, fn := range []string{"programmers.local.txt", "platform.local.txt", "debug_custom.json"} {
			full := filepath.Join(coreDir, fn)
			raw, err := os.ReadFile(full)
			if err != nil {
				t.Fatalf("missing %s for %s: %v", fn, fam.family, err)
			}
			snapshotFirst[full] = string(raw)
		}

		// Validate debug_custom.json contains explicit entries for EVERY profile in the core!
		var dbgEntries []map[string]any
		if err := json.Unmarshal([]byte(snapshotFirst[filepath.Join(coreDir, "debug_custom.json")]), &dbgEntries); err != nil {
			t.Fatalf("invalid JSON in %s debug_custom.json: %v", fam.family, err)
		}
		byID := make(map[string]map[string]any)
		for _, e := range dbgEntries {
			if id, _ := e["configId"].(string); id != "" {
				byID[id] = e
			}
		}
		for _, prof := range fam.profiles {
			wantID := "pico-universal-" + prof
			entry, ok := byID[wantID]
			if !ok {
				t.Fatalf("missing %s in %s debug_custom.json: %+v", wantID, fam.family, dbgEntries)
			}
			switch prof {
			case "stm32f1":
				if !strings.Contains(fmt.Sprint(entry["configFiles"]), "target/stm32f1x.cfg") {
					t.Fatalf("stm32f1 missing target/stm32f1x.cfg: %+v", entry)
				}
			case "stm32f4":
				if !strings.Contains(fmt.Sprint(entry["configFiles"]), "target/stm32f4x.cfg") {
					t.Fatalf("stm32f4 missing target/stm32f4x.cfg: %+v", entry)
				}
			case "rp2040":
				if !strings.Contains(fmt.Sprint(entry["configFiles"]), "target/rp2040.cfg") {
					t.Fatalf("rp2040 missing target/rp2040.cfg: %+v", entry)
				}
			case "rp2350":
				if !strings.Contains(fmt.Sprint(entry["configFiles"]), "target/rp2350.cfg") {
					t.Fatalf("rp2350 missing target/rp2350.cfg: %+v", entry)
				}
			case "esp32":
				if !strings.Contains(fmt.Sprint(entry["configFiles"]), stagedESP32Slash) {
					t.Fatalf("esp32 debug config does not reference staged %s: %+v", stagedESP32Slash, entry)
				}
				if !strings.Contains(fmt.Sprint(entry["preLaunchCommands"]), "set remotetimeout 20") {
					t.Fatalf("esp32 debug config missing preLaunchCommands set remotetimeout 20: %+v", entry)
				}
			case "esp32s3":
				if !strings.Contains(fmt.Sprint(entry["configFiles"]), "target/esp32s3.cfg") {
					t.Fatalf("esp32s3 missing target/esp32s3.cfg: %+v", entry)
				}
				if !strings.Contains(fmt.Sprint(entry["preLaunchCommands"]), "set remotetimeout 20") {
					t.Fatalf("esp32s3 debug config missing preLaunchCommands set remotetimeout 20: %+v", entry)
				}
			case "ch32v003":
				if !strings.Contains(fmt.Sprint(entry["gdbTarget"]), "gdb-pipe") {
					t.Fatalf("ch32v003 missing gdb-pipe gdbTarget: %+v", entry)
				}
			}
		}
	}

	// Confirm third-party STM32 entries were preserved alongside Pico Universal!
	stmProgAfter := snapshotFirst[filepath.Join(stmDir, "programmers.local.txt")]
	if !strings.Contains(stmProgAfter, "custom_stlink.name=Custom STLink") || !strings.Contains(stmProgAfter, "# >>> PICO-UNIVERSAL-BEGIN-CUSTOM") {
		t.Fatalf("STM32 third-party programmers.local.txt lost:\n%s", stmProgAfter)
	}
	if !strings.Contains(snapshotFirst[filepath.Join(stmDir, "debug_custom.json")], `"custom-jlink"`) {
		t.Fatalf("STM32 third-party debug_custom.json entry lost:\n%s", snapshotFirst[filepath.Join(stmDir, "debug_custom.json")])
	}

	// Confirm udev rule rendered with PID 1050
	udevBytes, err := os.ReadFile(filepath.Join(udevDir, "60-pico-universal.rules"))
	if err != nil {
		t.Fatalf("read rendered udev rules: %v", err)
	}
	udevStr := string(udevBytes)
	if !strings.Contains(udevStr, `SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", ATTRS{idProduct}=="1050", TAG+="uaccess", ENV{ID_MM_DEVICE_IGNORE}="1"`) {
		t.Fatalf("rendered udev rules missing uncommented PID 1050 rule:\n%s", udevStr)
	}

	// 2. Re-run `pu setup` to prove byte-for-byte idempotency
	code, _, stderr = runCLI("setup", "--offline", "--arduino-dir", arduinoDir, "--catalog-dir", catDir, "--udev-dir", udevDir, "--pid", "0x1050")
	if code != 0 {
		t.Fatalf("second idempotent pu setup failed: %s", stderr)
	}
	for full, want := range snapshotFirst {
		got, err := os.ReadFile(full)
		if err != nil {
			t.Fatal(err)
		}
		if string(got) != want {
			t.Fatalf("non-idempotent output in %s:\ngot:\n%s\nwant:\n%s", full, string(got), want)
		}
	}

	// 3. Run `pu setup --verify` -> must PASS
	code, _, stderr = runCLI("setup", "--verify", "--arduino-dir", arduinoDir, "--catalog-dir", catDir, "--udev-dir", udevDir, "--pid", "0x1050")
	if code != 0 {
		t.Fatalf("pu setup --verify failed: %s", stderr)
	}

	// 4. Run `pu setup --uninstall` -> removes ONLY Pico Universal entries, preserving third-party!
	code, _, stderr = runCLI("setup", "--uninstall", "--arduino-dir", arduinoDir, "--catalog-dir", catDir, "--udev-dir", udevDir)
	if code != 0 {
		t.Fatalf("pu setup --uninstall failed: %s", stderr)
	}

	stmProgFinal, err := os.ReadFile(filepath.Join(stmDir, "programmers.local.txt"))
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(stmProgFinal), "pico_universal.name=") {
		t.Fatalf("Pico Universal block remained after uninstall:\n%s", string(stmProgFinal))
	}
	if !strings.Contains(string(stmProgFinal), "custom_stlink.name=Custom STLink") || !strings.Contains(string(stmProgFinal), "# >>> PICO-UNIVERSAL-BEGIN-CUSTOM") {
		t.Fatalf("third-party block damaged by uninstall:\n%s", string(stmProgFinal))
	}

	stmDbgFinal, err := os.ReadFile(filepath.Join(stmDir, "debug_custom.json"))
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(stmDbgFinal), "pico-universal-stm32") || !strings.Contains(string(stmDbgFinal), `"custom-jlink"`) {
		t.Fatalf("unexpected debug_custom.json after uninstall:\n%s", string(stmDbgFinal))
	}

	// Verify AFTER uninstall must fail cleanly.
	if code, _, _ := runCLI("setup", "--verify", "--arduino-dir", arduinoDir, "--catalog-dir", catDir, "--udev-dir", udevDir, "--pid", "0x1050"); code == 0 {
		t.Fatal("expected --verify after uninstall to fail")
	}
}

func TestSetupUnprivilegedUdevPrintsSudoCommand(t *testing.T) {
	if runtime.GOOS == "windows" || os.Geteuid() == 0 {
		t.Skip("permission test applies to unprivileged Unix user")
	}
	root := testRepoRoot(t)
	catDir := filepath.Join(root, "catalog")
	arduinoDir := filepath.Join(t.TempDir(), "Arduino15")
	seedInstalledCores(t, arduinoDir)
	roUdevDir := filepath.Join(t.TempDir(), "readonly-udev")
	if err := os.MkdirAll(roUdevDir, 0o555); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chmod(roUdevDir, 0o755) })

	var out, errBuf bytes.Buffer
	code := run([]string{
		"setup", "--offline",
		"--arduino-dir", arduinoDir,
		"--catalog-dir", catDir,
		"--udev-dir", roUdevDir,
		"--pid", "0x1050",
	}, &out, &errBuf, func(string) string { return "" })

	if code != 0 {
		t.Fatalf("expected unprivileged setup to succeed with sudo instructions, got code=%d stderr=%s", code, errBuf.String())
	}
	if !strings.Contains(out.String(), "sudo install -m 0644") {
		t.Fatalf("expected sudo install command in stdout, got:\n%s", out.String())
	}
}

func TestEmbeddedUdevTemplateMatchesRepoRuleFile(t *testing.T) {
	root := testRepoRoot(t)
	committed, err := os.ReadFile(filepath.Join(root, "linux", "60-pico-universal.rules"))
	if err != nil {
		t.Fatalf("read linux/60-pico-universal.rules: %v", err)
	}
	if string(committed) != defaultUdevRulesTemplate {
		t.Fatal("defaultUdevRulesTemplate in setup.go drifted from linux/60-pico-universal.rules")
	}
}

func TestCLISubcommandRouterVersionListAndDispatchHooks(t *testing.T) {
	var out, errBuf bytes.Buffer
	if code := run([]string{"version"}, &out, &errBuf, func(string) string { return "" }); code != 0 || !strings.Contains(out.String(), "pu ") {
		t.Fatalf("version failed: code=%d out=%q err=%q", code, out.String(), errBuf.String())
	}

	out.Reset()
	errBuf.Reset()
	if code := run([]string{"-pid", "0x1050", "list", "--json"}, &out, &errBuf, func(string) string { return "" }); code != 0 {
		t.Fatalf("list --json failed: code=%d err=%q", code, errBuf.String())
	}

	// Verify Task 8 / 11 / 12 dispatch hooks exist and are routed cleanly (not "unknown command").
	for _, sub := range []string{"check", "upload", "gdb-pipe", "recover", "export-vscode", "factory-qc", "bench"} {
		out.Reset()
		errBuf.Reset()
		code := run([]string{sub}, &out, &errBuf, func(string) string { return "" })
		if code == 0 {
			t.Fatalf("expected stub subcommand %q to return non-zero until implemented", sub)
		}
		if strings.Contains(strings.ToLower(errBuf.String()), "unknown subcommand") {
			t.Fatalf("subcommand %q was not registered in router: %s", sub, errBuf.String())
		}
	}
}

func TestSetupOfflineRejectsMissingCoresWithoutCreatingEmptyVersionDirs(t *testing.T) {
	arduinoDir := filepath.Join(t.TempDir(), "Arduino15")
	udevDir := filepath.Join(t.TempDir(), "udev")

	var out, errBuf bytes.Buffer
	code := run([]string{"setup", "--offline", "--arduino-dir", arduinoDir, "--udev-dir", udevDir}, &out, &errBuf, nil)
	if code == 0 {
		t.Fatal("expected --offline setup on empty Arduino15 dir to fail when cores are missing")
	}
	if !strings.Contains(errBuf.String(), "boards.txt") && !strings.Contains(errBuf.String(), "platform.txt") {
		t.Fatalf("expected actionable missing boards.txt/platform.txt error, got: %s", errBuf.String())
	}
	if _, err := os.Stat(filepath.Join(arduinoDir, "packages")); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("expected no bare packages/<vendor>/... directories to be created, stat err=%v", err)
	}
}

func TestSetupOfflineCacheDirVerification(t *testing.T) {
	// Build a compact locked catalog fixture with real SHA-256 hashes for tiny test archives.
	catDir := t.TempDir()
	cacheDir := t.TempDir()

	targetsObj := map[string]any{
		"schema_version": 1,
		"cores": map[string]any{
			"stm32":  map[string]any{"package": "STMicroelectronics:stm32", "version": "2.12.0", "index": "https://example.invalid/stm32.json", "source": "https://example.invalid/stm32"},
			"rp2040": map[string]any{"package": "rp2040:rp2040", "version": "6.1.1", "index": "https://example.invalid/rp2040.json", "source": "https://example.invalid/rp2040"},
			"esp32":  map[string]any{"package": "esp32:esp32", "version": "3.3.11", "index": "https://example.invalid/esp32.json", "source": "https://example.invalid/esp32"},
			"ch32v":  map[string]any{"package": "WCH:ch32v", "version": "1.0.4", "index": "https://example.invalid/ch32v.json", "source": "https://example.invalid/ch32v"},
		},
		"profiles": map[string]any{
			"stm32f1":  map[string]any{"engine": "openocd", "mode": "cmsis-dap", "transport": "swd", "target": "target/stm32f1x.cfg", "tool_family": "stm32"},
			"stm32f4":  map[string]any{"engine": "openocd", "mode": "cmsis-dap", "transport": "swd", "target": "target/stm32f4x.cfg", "tool_family": "stm32"},
			"rp2040":   map[string]any{"engine": "openocd", "mode": "cmsis-dap", "transport": "swd", "target": "target/rp2040.cfg", "tool_family": "rp2040"},
			"rp2350":   map[string]any{"engine": "openocd", "mode": "cmsis-dap", "transport": "swd", "target": "target/rp2350.cfg", "tool_family": "rp2040"},
			"esp32":    map[string]any{"engine": "esp32", "mode": "cmsis-dap", "transport": "jtag", "target": "target/esp32.cfg", "openocd_cfg": "catalog/openocd/pu-esp32.cfg", "tool_family": "esp32"},
			"esp32s3":  map[string]any{"engine": "esp32", "mode": "cmsis-dap", "transport": "jtag", "target": "target/esp32s3.cfg", "tool_family": "esp32"},
			"ch32v003": map[string]any{"engine": "picorvd", "mode": "composite-swio", "transport": "swio", "tool_family": "riscv-gdb"},
		},
		"boards": []map[string]any{
			{"id": "bp", "name": "BP", "core": "stm32", "fqbn": "STMicroelectronics:stm32:GenF1", "profile": "stm32f1", "phase": "initial"},
			{"id": "bk", "name": "BK", "core": "stm32", "fqbn": "STMicroelectronics:stm32:GenF4", "profile": "stm32f4", "phase": "initial"},
			{"id": "p1", "name": "P1", "core": "rp2040", "fqbn": "rp2040:rp2040:rpipico", "profile": "rp2040", "phase": "initial"},
			{"id": "p2", "name": "P2", "core": "rp2040", "fqbn": "rp2040:rp2040:rpipico2", "profile": "rp2350", "phase": "expansion"},
			{"id": "e1", "name": "E1", "core": "esp32", "fqbn": "esp32:esp32:esp32", "profile": "esp32", "phase": "initial"},
			{"id": "e3", "name": "E3", "core": "esp32", "fqbn": "esp32:esp32:esp32s3", "profile": "esp32s3", "phase": "expansion"},
			{"id": "ch", "name": "CH", "core": "ch32v", "fqbn": "WCH:ch32v:CH32V00x_EVT", "profile": "ch32v003", "phase": "initial"},
		},
	}
	targetsRaw, err := json.MarshalIndent(targetsObj, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(catDir, "targets.json"), targetsRaw, 0o644); err != nil {
		t.Fatal(err)
	}
	tSum := sha256.Sum256(targetsRaw)

	artifacts := map[string]any{}
	coresLock := map[string]any{}
	for _, c := range testCoreRelDirs {
		fn := c.family + "-core.tar.bz2"
		payload := []byte("archive-payload-" + c.family)
		aSum := sha256.Sum256(payload)
		if err := os.WriteFile(filepath.Join(cacheDir, fn), payload, 0o644); err != nil {
			t.Fatal(err)
		}
		artifacts["core/"+c.family] = map[string]any{
			"url":      "https://example.invalid/" + fn,
			"filename": fn,
			"sha256":   hex.EncodeToString(aSum[:]),
			"size":     len(payload),
		}
		coresLock[c.family] = map[string]any{
			"package":  c.pkg,
			"version":  c.version,
			"index":    "https://example.invalid/" + c.family + ".json",
			"source":   "https://example.invalid/" + c.family,
			"artifact": "core/" + c.family,
		}
	}
	lockObj := map[string]any{
		"schema_version": 1,
		"catalog_sha256": hex.EncodeToString(tSum[:]),
		"cores":          coresLock,
		"artifacts":      artifacts,
	}
	lockRaw, err := json.MarshalIndent(lockObj, "", "  ")
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(catDir, "dependencies.lock.json"), lockRaw, 0o644); err != nil {
		t.Fatal(err)
	}

	arduinoDir := filepath.Join(t.TempDir(), "Arduino15")
	seedInstalledCores(t, arduinoDir)
	udevDir := filepath.Join(t.TempDir(), "udev")

	// 1. Valid cache-dir + installed cores -> PASS
	var out, errBuf bytes.Buffer
	if code := run([]string{"setup", "--offline", "--cache-dir", cacheDir, "--arduino-dir", arduinoDir, "--catalog-dir", catDir, "--udev-dir", udevDir}, &out, &errBuf, nil); code != 0 {
		t.Fatalf("expected offline setup with valid cache-dir to pass, got: %s", errBuf.String())
	}

	// 2. Corrupt one archive in cache-dir -> FAIL
	if err := os.WriteFile(filepath.Join(cacheDir, "esp32-core.tar.bz2"), []byte("tampered"), 0o644); err != nil {
		t.Fatal(err)
	}
	errBuf.Reset()
	if code := run([]string{"setup", "--offline", "--cache-dir", cacheDir, "--arduino-dir", arduinoDir, "--catalog-dir", catDir, "--udev-dir", udevDir}, &out, &errBuf, nil); code == 0 {
		t.Fatal("expected offline setup to fail on corrupted archive in --cache-dir")
	}

	// 3. Missing required core archive in cache-dir -> FAIL with actionable error
	if err := os.Remove(filepath.Join(cacheDir, "esp32-core.tar.bz2")); err != nil {
		t.Fatal(err)
	}
	errBuf.Reset()
	if code := run([]string{"setup", "--offline", "--cache-dir", cacheDir, "--arduino-dir", arduinoDir, "--catalog-dir", catDir, "--udev-dir", udevDir}, &out, &errBuf, nil); code == 0 {
		t.Fatal("expected offline setup to fail when required core archive is missing from --cache-dir")
	}
	if !strings.Contains(errBuf.String(), "esp32-core.tar.bz2") {
		t.Fatalf("expected missing archive filename in error, got: %s", errBuf.String())
	}
}

func TestSetupOnlineInvokesArduinoCLIOrErrorsWhenAbsent(t *testing.T) {
	// 1. Online mode without arduino-cli and without --profiles-only -> MUST fail without creating empty dirs!
	emptyArduinoDir := filepath.Join(t.TempDir(), "Arduino15-empty")
	udevDir := filepath.Join(t.TempDir(), "udev")
	absentRunner := &fakeCommandRunner{bins: map[string]string{}}

	var out, errBuf bytes.Buffer
	code := runWithEnv([]string{"setup", "--arduino-dir", emptyArduinoDir, "--udev-dir", udevDir}, cliEnv{
		stdout:    &out,
		stderr:    &errBuf,
		getenv:    func(string) string { return "" },
		geteuid:   func() int { return 1000 },
		goos:      "linux",
		cmdRunner: absentRunner,
	})
	if code == 0 {
		t.Fatal("expected online setup without arduino-cli and without --profiles-only to fail")
	}
	if !strings.Contains(errBuf.String(), "arduino-cli") {
		t.Fatalf("expected error mentioning arduino-cli, got: %s", errBuf.String())
	}
	if _, err := os.Stat(filepath.Join(emptyArduinoDir, "packages")); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("expected no fake bare core directories when arduino-cli is absent, stat err=%v", err)
	}

	// 2. Online mode WITH injectable arduino-cli -> installs missing cores with pinned version & locked index URL!
	installArduinoDir := filepath.Join(t.TempDir(), "Arduino15-install")
	presentRunner := &fakeCommandRunner{
		bins: map[string]string{"arduino-cli": "/usr/local/bin/arduino-cli"},
		onRun: func(cmd commandSpec, _, _ io.Writer) error {
			// Simulate arduino-cli core install populating boards.txt and platform.txt for the requested package@version.
			if len(cmd.Args) >= 3 && cmd.Args[0] == "core" && cmd.Args[1] == "install" {
				spec := cmd.Args[2]
				for _, c := range testCoreRelDirs {
					if spec == c.pkg+"@"+c.version {
						hwDir := filepath.Join(installArduinoDir, c.relDir)
						if err := os.MkdirAll(hwDir, 0o755); err != nil {
							return err
						}
						if err := os.WriteFile(filepath.Join(hwDir, "boards.txt"), []byte("# "+c.family+"\n"), 0o644); err != nil {
							return err
						}
						if err := os.WriteFile(filepath.Join(hwDir, "platform.txt"), []byte("name="+c.family+"\n"), 0o644); err != nil {
							return err
						}
					}
				}
			}
			return nil
		},
	}

	out.Reset()
	errBuf.Reset()
	code = runWithEnv([]string{"setup", "--arduino-dir", installArduinoDir, "--udev-dir", udevDir, "--pid", "0x1050"}, cliEnv{
		stdout:    &out,
		stderr:    &errBuf,
		getenv:    func(string) string { return "" },
		geteuid:   func() int { return 1000 },
		goos:      "linux",
		cmdRunner: presentRunner,
	})
	if code != 0 {
		t.Fatalf("expected online setup with arduino-cli runner to succeed, got code=%d stderr=%s", code, errBuf.String())
	}
	if len(presentRunner.ran) != 4 {
		t.Fatalf("expected 4 arduino-cli core install invocations, got %d: %+v", len(presentRunner.ran), presentRunner.ran)
	}
	for i, c := range testCoreRelDirs {
		gotArgs := strings.Join(presentRunner.ran[i].args, " ")
		wantSub := "core install " + c.pkg + "@" + c.version + " --additional-urls "
		if !strings.Contains(gotArgs, wantSub) {
			t.Fatalf("invocation %d args %q missing %q", i, gotArgs, wantSub)
		}
	}

	// Verify passes afterwards!
	errBuf.Reset()
	if code := run([]string{"setup", "--verify", "--arduino-dir", installArduinoDir, "--udev-dir", udevDir, "--pid", "0x1050"}, &out, &errBuf, nil); code != 0 {
		t.Fatalf("expected --verify after arduino-cli install to pass, got: %s", errBuf.String())
	}
}

func TestSetupProfilesOnlyAndVerifyRequiresInstalledCoreFiles(t *testing.T) {
	arduinoDir := filepath.Join(t.TempDir(), "Arduino15")
	udevDir := filepath.Join(t.TempDir(), "udev")
	absentRunner := &fakeCommandRunner{bins: map[string]string{}}

	var out, errBuf bytes.Buffer
	code := runWithEnv([]string{"setup", "--profiles-only", "--arduino-dir", arduinoDir, "--udev-dir", udevDir}, cliEnv{
		stdout:    &out,
		stderr:    &errBuf,
		getenv:    func(string) string { return "" },
		geteuid:   func() int { return 1000 },
		goos:      "linux",
		cmdRunner: absentRunner,
	})
	if code != 0 {
		t.Fatalf("expected --profiles-only to succeed without arduino-cli, got: %s", errBuf.String())
	}

	// --verify MUST fail because boards.txt and platform.txt do not exist yet!
	errBuf.Reset()
	if code := run([]string{"setup", "--verify", "--arduino-dir", arduinoDir, "--udev-dir", udevDir}, &out, &errBuf, nil); code == 0 {
		t.Fatal("expected --verify to fail when boards.txt and platform.txt are missing")
	}

	// Once boards.txt + platform.txt are present, --verify passes!
	seedInstalledCores(t, arduinoDir)
	errBuf.Reset()
	if code := run([]string{"setup", "--verify", "--arduino-dir", arduinoDir, "--udev-dir", udevDir}, &out, &errBuf, nil); code != 0 {
		t.Fatalf("expected --verify to pass once boards.txt and platform.txt exist, got: %s", errBuf.String())
	}
}

func TestSetupStandaloneWithoutCatalogDirAndVerifiesStagedESP32Cfg(t *testing.T) {
	root := testRepoRoot(t)
	wantESP32Cfg, err := os.ReadFile(filepath.Join(root, "catalog", "openocd", "pu-esp32.cfg"))
	if err != nil {
		t.Fatal(err)
	}

	origWD, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	isolatedWD := t.TempDir()
	if err := os.Chdir(isolatedWD); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chdir(origWD) })

	arduinoDir := filepath.Join(t.TempDir(), "Arduino15")
	seedInstalledCores(t, arduinoDir)
	udevDir := filepath.Join(t.TempDir(), "udev")

	var out, errBuf bytes.Buffer
	// Omit --catalog-dir completely in an isolated working directory!
	if code := run([]string{"setup", "--offline", "--arduino-dir", arduinoDir, "--udev-dir", udevDir}, &out, &errBuf, nil); code != 0 {
		t.Fatalf("standalone setup without --catalog-dir failed: %s", errBuf.String())
	}

	stagedCfg := filepath.Join(arduinoDir, "pico-universal", "openocd", "pu-esp32.cfg")
	gotCfg, err := os.ReadFile(stagedCfg)
	if err != nil {
		t.Fatalf("read staged pu-esp32.cfg: %v", err)
	}
	if string(gotCfg) != string(wantESP32Cfg) {
		t.Fatal("staged pu-esp32.cfg did not match catalog/openocd/pu-esp32.cfg byte-for-byte")
	}

	errBuf.Reset()
	if code := run([]string{"setup", "--verify", "--arduino-dir", arduinoDir, "--udev-dir", udevDir}, &out, &errBuf, nil); code != 0 {
		t.Fatalf("standalone --verify failed: %s", errBuf.String())
	}

	// Tamper with staged pu-esp32.cfg -> --verify must fail SHA-256 check!
	if err := os.WriteFile(stagedCfg, []byte("# tampered\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	errBuf.Reset()
	if code := run([]string{"setup", "--verify", "--arduino-dir", arduinoDir, "--udev-dir", udevDir}, &out, &errBuf, nil); code == 0 {
		t.Fatal("expected --verify to reject tampered staged pu-esp32.cfg")
	}

	// Restore staged pu-esp32.cfg, then tamper with esp32 debug_custom.json configFiles -> --verify must fail!
	if err := os.WriteFile(stagedCfg, wantESP32Cfg, 0o644); err != nil {
		t.Fatal(err)
	}
	espDbgPath := filepath.Join(arduinoDir, "packages", "esp32", "hardware", "esp32", "3.3.11", "debug_custom.json")
	rawDbg, err := os.ReadFile(espDbgPath)
	if err != nil {
		t.Fatal(err)
	}
	brokenDbg := strings.ReplaceAll(string(rawDbg), filepath.ToSlash(stagedCfg), "wrong/path/pu-esp32.cfg")
	if err := os.WriteFile(espDbgPath, []byte(brokenDbg), 0o644); err != nil {
		t.Fatal(err)
	}
	errBuf.Reset()
	if code := run([]string{"setup", "--verify", "--arduino-dir", arduinoDir, "--udev-dir", udevDir}, &out, &errBuf, nil); code == 0 {
		t.Fatal("expected --verify to reject esp32 debug_custom.json pointing away from staged pu-esp32.cfg")
	}
}

func TestVerifySetupWindowsWinUSBDiagnostic(t *testing.T) {
	arduinoDir := filepath.Join(t.TempDir(), "Arduino15")
	seedInstalledCores(t, arduinoDir)

	var out, errBuf bytes.Buffer
	if code := runWithEnv([]string{"setup", "--offline", "--arduino-dir", arduinoDir}, cliEnv{
		stdout: &out,
		stderr: &errBuf,
		getenv: func(string) string { return "" },
		goos:   "windows",
	}); code != 0 {
		t.Fatalf("Windows setup failed: %s", errBuf.String())
	}

	// 1. Connected Windows probe WITHOUT WinUSB bound (parsed through real ParseWindowsDeviceRecords + CorrelateNodes) -> --verify MUST fail!
	out.Reset()
	errBuf.Reset()
	code := runWithEnv([]string{"setup", "--verify", "--arduino-dir", arduinoDir}, cliEnv{
		stdout: &out,
		stderr: &errBuf,
		getenv: func(string) string { return "" },
		goos:   "windows",
		discoverProbes: func(vid, pid uint16) ([]usbprobe.ProbeDevice, error) {
			nodes, err := usbprobe.ParseWindowsDeviceRecords([]usbprobe.WindowsDevRecord{
				{InstanceID: `USB\VID_2E8A&PID_1050\E660583883BADUSB`, Product: "Pico Universal CMSIS-DAP", Service: "usbccgp"},
				{InstanceID: `USB\VID_2E8A&PID_1050&MI_00\6&bad&0&0000`, ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883BADUSB`, Service: "libusbK"},
				{InstanceID: `USB\VID_2E8A&PID_1050&MI_05\6&bad&0&0005`, ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883BADUSB`, Service: "WINUSB"},
			})
			if err != nil {
				return nil, err
			}
			return usbprobe.CorrelateNodes(nodes, vid, pid), nil
		},
	})
	if code == 0 {
		t.Fatal("expected Windows --verify to fail when connected probe is missing WinUSB binding")
	}
	if !strings.Contains(errBuf.String(), "WinUSB") || !strings.Contains(errBuf.String(), "E660583883BADUSB") {
		t.Fatalf("expected clear WinUSB diagnostic naming serial, got: %s", errBuf.String())
	}

	// 2. Connected Windows probe WITH active WinUSB bindings -> --verify MUST pass!
	out.Reset()
	errBuf.Reset()
	code = runWithEnv([]string{"setup", "--verify", "--arduino-dir", arduinoDir}, cliEnv{
		stdout: &out,
		stderr: &errBuf,
		getenv: func(string) string { return "" },
		goos:   "windows",
		discoverProbes: func(_, _ uint16) ([]usbprobe.ProbeDevice, error) {
			return []usbprobe.ProbeDevice{
				{Serial: "E660583883WINUSB", VID: 0x2E8A, PID: 0x1050, DAPPath: `\\?\usb#mi_00`, CtrlPath: `\\?\usb#mi_05`},
			}, nil
		},
	})
	if code != 0 {
		t.Fatalf("expected Windows --verify with active WinUSB bindings to pass, got: %s", errBuf.String())
	}
	if !strings.Contains(out.String(), "WinUSB") {
		t.Fatalf("expected Windows WinUSB confirmation in stdout, got: %s", out.String())
	}
}

func TestSetupLinuxRootReloadsUdevOnInstallAndUninstall(t *testing.T) {
	arduinoDir := filepath.Join(t.TempDir(), "Arduino15")
	seedInstalledCores(t, arduinoDir)
	udevDir := filepath.Join(t.TempDir(), "etc-udev-rules.d")

	runner := &fakeCommandRunner{
		bins: map[string]string{"udevadm": "/usr/bin/udevadm"},
	}

	var out, errBuf bytes.Buffer
	code := runWithEnv([]string{"setup", "--offline", "--arduino-dir", arduinoDir, "--udev-dir", udevDir, "--pid", "0x1050"}, cliEnv{
		stdout:    &out,
		stderr:    &errBuf,
		getenv:    func(string) string { return "" },
		geteuid:   func() int { return 0 },
		goos:      "linux",
		cmdRunner: runner,
	})
	if code != 0 {
		t.Fatalf("Linux root setup failed: %s", errBuf.String())
	}
	if len(runner.ran) != 2 {
		t.Fatalf("expected 2 udevadm commands on root install, got %d: %+v", len(runner.ran), runner.ran)
	}
	if strings.Join(runner.ran[0].args, " ") != "control --reload-rules" {
		t.Fatalf("unexpected first udevadm command: %+v", runner.ran[0])
	}
	if strings.Join(runner.ran[1].args, " ") != "trigger --subsystem-match=usb --subsystem-match=tty" {
		t.Fatalf("unexpected second udevadm command: %+v", runner.ran[1])
	}

	runner.ran = nil
	out.Reset()
	errBuf.Reset()
	code = runWithEnv([]string{"setup", "--uninstall", "--arduino-dir", arduinoDir, "--udev-dir", udevDir}, cliEnv{
		stdout:    &out,
		stderr:    &errBuf,
		getenv:    func(string) string { return "" },
		geteuid:   func() int { return 0 },
		goos:      "linux",
		cmdRunner: runner,
	})
	if code != 0 {
		t.Fatalf("Linux root uninstall failed: %s", errBuf.String())
	}
	if len(runner.ran) != 2 {
		t.Fatalf("expected 2 udevadm commands on root uninstall, got %d: %+v", len(runner.ran), runner.ran)
	}
	if strings.Join(runner.ran[0].args, " ") != "control --reload-rules" ||
		strings.Join(runner.ran[1].args, " ") != "trigger --subsystem-match=usb --subsystem-match=tty" {
		t.Fatalf("unexpected udevadm uninstall commands: %+v", runner.ran)
	}
}
