package uploader

import (
	"bytes"
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

type fakeUploadRunner struct {
	bins  map[string]string
	onRun func(spec CommandSpec, stdout, stderr io.Writer) error
	ran   []CommandSpec
}

func (f *fakeUploadRunner) LookPath(file string) (string, error) {
	if p, ok := f.bins[file]; ok {
		return p, nil
	}
	return "", fmt.Errorf("executable %q not found", file)
}

func (f *fakeUploadRunner) Run(spec CommandSpec, stdout, stderr io.Writer) error {
	f.ran = append(f.ran, CommandSpec{
		Name: spec.Name,
		Args: append([]string(nil), spec.Args...),
		Env:  append([]string(nil), spec.Env...),
	})
	if f.onRun != nil {
		return f.onRun(spec, stdout, stderr)
	}
	return nil
}

func TestUploadSnapshotAndSectorOverlapRejection(t *testing.T) {
	dir := t.TempDir()
	f1 := filepath.Join(dir, "boot.bin")
	f2 := filepath.Join(dir, "part.bin")
	fEmpty := filepath.Join(dir, "empty.bin")
	fSmallA := filepath.Join(dir, "small_a.bin")
	fSmallB := filepath.Join(dir, "small_b.bin")
	fSpill := filepath.Join(dir, "spill.bin")

	if err := os.WriteFile(f1, bytes.Repeat([]byte{0xAA}, 4096), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(f2, bytes.Repeat([]byte{0x55}, 2048), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(fEmpty, nil, 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(fSmallA, make([]byte, 512), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(fSmallB, make([]byte, 512), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(fSpill, make([]byte, 4097), 0o600); err != nil {
		t.Fatal(err)
	}

	// 1. Overlapping within same 4 KiB erase sector (0x1000 + 0x1000 vs 0x1800) must be rejected.
	if err := ValidateEspressifSegments([]Segment{
		{Addr: 0x1000, Path: f1},
		{Addr: 0x1800, Path: f2},
	}); err == nil {
		t.Fatal("expected sector overlap error, got nil")
	}

	// 2. Disjoint byte ranges that STILL share a 4 KiB sector (0x1000..0x11FF and 0x1800..0x19FF) must be rejected!
	if err := ValidateEspressifSegments([]Segment{
		{Addr: 0x1000, Path: fSmallA},
		{Addr: 0x1800, Path: fSmallB},
	}); err == nil {
		t.Fatal("expected shared 4 KiB erase sector error for non-overlapping byte ranges, got nil")
	}

	// 3. 1-byte spill into next 4 KiB sector (0x1000 + 4097 touches 0x2000) must reject segment at 0x2000.
	if err := ValidateEspressifSegments([]Segment{
		{Addr: 0x1000, Path: fSpill},
		{Addr: 0x2000, Path: f2},
	}); err == nil {
		t.Fatal("expected 1-byte sector spill overlap rejection, got nil")
	}

	// 4. Missing file, empty file, and uint32 address wrap must all be rejected.
	if err := ValidateEspressifSegments([]Segment{{Addr: 0x1000, Path: filepath.Join(dir, "missing.bin")}}); err == nil {
		t.Fatal("expected missing segment file error")
	}
	if err := ValidateEspressifSegments([]Segment{{Addr: 0x1000, Path: fEmpty}}); err == nil {
		t.Fatal("expected empty segment file error")
	}
	if err := ValidateEspressifSegments([]Segment{{Addr: 0xFFFFF800, Path: f1}}); err == nil {
		t.Fatal("expected uint32 address wrap error")
	}

	// 5. Adjacent disjoint 4 KiB sectors (0x1000..0x1FFF and 0x2000..0x27FF) must pass.
	if err := ValidateEspressifSegments([]Segment{
		{Addr: 0x1000, Path: f1},
		{Addr: 0x2000, Path: f2},
	}); err != nil {
		t.Fatalf("expected valid disjoint sectors to pass, got %v", err)
	}

	// 6. Snapshot must isolate file contents from later concurrent rebuild mutation and use 0700 dir.
	snap, cleanup, err := CreatePrivateSnapshot(f1)
	if err != nil {
		t.Fatal(err)
	}
	snapDir := filepath.Dir(snap)
	if runtime.GOOS != "windows" {
		st, statErr := os.Stat(snapDir)
		if statErr != nil {
			cleanup()
			t.Fatal(statErr)
		}
		if perm := st.Mode().Perm(); perm != 0o700 {
			cleanup()
			t.Fatalf("expected private snapshot dir mode 0700, got %04o", perm)
		}
	}

	if err := os.WriteFile(f1, []byte("mutated-mid-upload"), 0o600); err != nil {
		cleanup()
		t.Fatal(err)
	}
	got, err := os.ReadFile(snap)
	if err != nil {
		cleanup()
		t.Fatal(err)
	}
	if len(got) != 4096 || got[0] != 0xAA {
		cleanup()
		t.Fatalf("snapshot mutated! expected 4096 bytes of 0xAA, got len=%d", len(got))
	}

	cleanup()
	if _, err := os.Stat(snapDir); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("expected cleanup() to remove %s, stat err=%v", snapDir, err)
	}
}

func extractBracedPath(arg string) string {
	openIdx := strings.IndexByte(arg, '{')
	closeIdx := strings.IndexByte(arg, '}')
	if openIdx >= 0 && closeIdx > openIdx {
		return arg[openIdx+1 : closeIdx]
	}
	return ""
}

func TestUploadAtomicSessionLockAndCleanupOnFailure(t *testing.T) {
	t.Setenv("PU_LOCK_DIR", filepath.Join(t.TempDir(), "locks"))

	dir := t.TempDir()
	elf := filepath.Join(dir, "sketch.elf")
	if err := os.WriteFile(elf, []byte("\x7fELF-fake-image"), 0o600); err != nil {
		t.Fatal(err)
	}

	probe := usbprobe.ProbeDevice{
		Serial:  "E660583883278628",
		VID:     0x2E8A,
		PID:     0x1050,
		DAPPath: "/dev/bus/usb/001/005",
		GDBPort: "/dev/ttyACM0",
	}

	held, err := usbprobe.AcquireSessionLock(probe.Serial)
	if err != nil {
		t.Fatal(err)
	}
	runner := &fakeUploadRunner{
		bins: map[string]string{"openocd": "/usr/bin/openocd"},
	}
	err = UploadAtomic(probe, UploadRequest{
		Family:   "stm32f1",
		FilePath: elf,
	}, runner)
	if !errors.Is(err, usbprobe.ErrProbeLocked) {
		_ = held.Close()
		t.Fatalf("expected ErrProbeLocked, got %v", err)
	}
	_ = held.Close()

	// Verify snapshot directory is removed even when OpenOCD returns an error!
	var capturedSnapDir string
	runner.onRun = func(spec CommandSpec, stdout, stderr io.Writer) error {
		for _, a := range spec.Args {
			if strings.HasPrefix(a, "program {") {
				if p := extractBracedPath(a); p != "" {
					capturedSnapDir = filepath.Dir(filepath.FromSlash(p))
				}
			}
		}
		fmt.Fprintln(stderr, "Error: flash write failed")
		return errors.New("exit status 1")
	}
	if err := UploadAtomic(probe, UploadRequest{
		Family:   "stm32f1",
		FilePath: elf,
	}, runner); err == nil {
		t.Fatal("expected UploadAtomic to propagate OpenOCD failure")
	}
	if capturedSnapDir == "" {
		t.Fatal("failed to capture JimTcl braced snapshot path from OpenOCD args")
	}
	if _, statErr := os.Stat(capturedSnapDir); !errors.Is(statErr, os.ErrNotExist) {
		t.Fatalf("expected snapshot temp dir %s to be cleaned up after failure, statErr=%v", capturedSnapDir, statErr)
	}
}

func TestUploadAtomicARMAndESP32OpenOCDCommands(t *testing.T) {
	t.Setenv("PU_LOCK_DIR", filepath.Join(t.TempDir(), "locks"))

	dir := t.TempDir()
	elf := filepath.Join(dir, "blink.elf")
	origBytes := []byte("\x7fELF-original-payload-bytes")
	if err := os.WriteFile(elf, origBytes, 0o600); err != nil {
		t.Fatal(err)
	}

	probe := usbprobe.ProbeDevice{
		Serial: "E660UPLOAD000001",
		VID:    0x2E8A,
		PID:    0x1050,
	}

	runner := &fakeUploadRunner{
		bins: map[string]string{"openocd": "/usr/bin/openocd"},
		onRun: func(spec CommandSpec, stdout, stderr io.Writer) error {
			// Mutate source ELF while OpenOCD is executing; snapshot must remain original!
			_ = os.WriteFile(elf, []byte("corrupted-by-background-rebuild"), 0o600)
			for _, a := range spec.Args {
				if strings.HasPrefix(a, "program {") {
					p := extractBracedPath(a)
					if p == "" {
						return errors.New("expected JimTcl brace-literal path in program command")
					}
					snapData, readErr := os.ReadFile(filepath.FromSlash(p))
					if readErr != nil {
						return readErr
					}
					if !bytes.Equal(snapData, origBytes) {
						return fmt.Errorf("snapshot corrupted during upload: %q", string(snapData))
					}
				}
			}
			return nil
		},
	}

	for _, swdFam := range []string{"rp2040", "rp2350", "stm32f1", "stm32f4"} {
		runner.ran = nil
		// Restore source ELF before each family test
		if err := os.WriteFile(elf, origBytes, 0o600); err != nil {
			t.Fatal(err)
		}
		if err := UploadAtomic(probe, UploadRequest{
			Family:   swdFam,
			FilePath: elf,
		}, runner); err != nil {
			t.Fatalf("UploadAtomic %s failed: %v", swdFam, err)
		}
		wantSpeed := "adapter speed 4000"
		if swdFam == "stm32f1" || swdFam == "stm32f4" {
			wantSpeed = "adapter speed 2000"
		}
		joinedArgs := strings.Join(runner.ran[0].Args, " | ")
		for _, want := range []string{
			"adapter serial " + probe.Serial,
			wantSpeed,
			"program {",
			"} verify reset exit",
		} {
			if !strings.Contains(joinedArgs, want) {
				t.Fatalf("missing %q in %s OpenOCD args: %s", want, swdFam, joinedArgs)
			}
		}
	}

	// Classic ESP32 and ESP32-S3 multi-segment uploads
	boot := filepath.Join(dir, "boot.bin")
	part := filepath.Join(dir, "partitions.bin")
	app := filepath.Join(dir, "app.bin")
	_ = os.WriteFile(boot, bytes.Repeat([]byte{0x11}, 4096), 0o600)
	_ = os.WriteFile(part, bytes.Repeat([]byte{0x22}, 3072), 0o600)
	_ = os.WriteFile(app, bytes.Repeat([]byte{0x33}, 8192), 0o600)

	runner.ran = nil
	var sawStagedESP32Cfg bool
	runner.onRun = func(spec CommandSpec, stdout, stderr io.Writer) error {
		for i := 0; i+1 < len(spec.Args); i++ {
			if spec.Args[i] == "-f" && strings.Contains(spec.Args[i+1], "pu-esp32.cfg") {
				raw, readErr := os.ReadFile(filepath.FromSlash(spec.Args[i+1]))
				if readErr == nil && strings.Contains(string(raw), "ESP32_FLASH_VOLTAGE 3.3") {
					sawStagedESP32Cfg = true
				}
			}
		}
		return nil
	}

	if err := UploadAtomic(probe, UploadRequest{
		Family: "esp32",
		Segments: []Segment{
			{Addr: 0x1000, Path: boot},
			{Addr: 0x8000, Path: part},
			{Addr: 0x10000, Path: app},
		},
	}, runner); err != nil {
		t.Fatalf("UploadAtomic esp32 multi-segment failed: %v", err)
	}
	if !sawStagedESP32Cfg {
		t.Fatal("expected UploadAtomic esp32 to stage embedded pu-esp32.cfg")
	}
	espArgs := strings.Join(runner.ran[0].Args, " | ")
	for _, want := range []string{
		"adapter serial " + probe.Serial,
		"program_esp {",
		"} 0x00001000 verify",
		"} 0x00008000 verify",
		"} 0x00010000 verify",
	} {
		if !strings.Contains(espArgs, want) {
			t.Fatalf("missing %q in esp32 OpenOCD args: %s", want, espArgs)
		}
	}

	// ESP32-S3 multi-segment upload must inject explicit adapter speed 5000!
	runner.ran = nil
	if err := UploadAtomic(probe, UploadRequest{
		Family: "esp32s3",
		Segments: []Segment{
			{Addr: 0x0, Path: boot},
			{Addr: 0x8000, Path: part},
			{Addr: 0x10000, Path: app},
		},
	}, runner); err != nil {
		t.Fatalf("UploadAtomic esp32s3 multi-segment failed: %v", err)
	}
	s3Args := strings.Join(runner.ran[0].Args, " | ")
	if !strings.Contains(s3Args, "adapter speed 5000") {
		t.Fatalf("missing 'adapter speed 5000' in esp32s3 OpenOCD args: %s", s3Args)
	}
}

func TestUploadAtomicOpenOCDTclSafetyAndEspressifSingleELFGuard(t *testing.T) {
	t.Setenv("PU_LOCK_DIR", filepath.Join(t.TempDir(), "locks"))

	dir := t.TempDir()
	probe := usbprobe.ProbeDevice{Serial: "E660TCLGUARD0001", VID: 0x2E8A, PID: 0x1050}
	runner := &fakeUploadRunner{
		bins: map[string]string{"openocd": "/usr/bin/openocd"},
	}

	// 1. Filename with $var and [cmd] must be safely enclosed in {...} JimTcl braces, never "..."
	tricky := filepath.Join(dir, "fw_$env[shutdown].elf")
	if err := os.WriteFile(tricky, []byte("\x7fELF-tricky"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := UploadAtomic(probe, UploadRequest{Family: "stm32f1", FilePath: tricky}, runner); err != nil {
		t.Fatalf("expected $ and [] filename to succeed safely inside JimTcl braces, got %v", err)
	}
	joined := strings.Join(runner.ran[0].Args, " ")
	if strings.Contains(joined, `"`) || !strings.Contains(joined, "fw_$env[shutdown].elf}") {
		t.Fatalf("expected braced literal without double quotes, got: %s", joined)
	}

	// 2. Filenames containing '{' or '}' must be rejected BEFORE launching OpenOCD!
	runner.ran = nil
	badBrace := filepath.Join(dir, "bad}inject.elf")
	if err := os.WriteFile(badBrace, []byte("\x7fELF-bad"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := UploadAtomic(probe, UploadRequest{Family: "stm32f1", FilePath: badBrace}, runner); err == nil {
		t.Fatal("expected path containing '}' to be rejected before OpenOCD")
	}
	if len(runner.ran) != 0 {
		t.Fatalf("OpenOCD must not be invoked on unsafe brace path, ran=%d", len(runner.ran))
	}

	// 3. esp32 and esp32s3 single-ELF --file upload without --segment must fail with clear guidance!
	cleanELF := filepath.Join(dir, "app.elf")
	if err := os.WriteFile(cleanELF, []byte("\x7fELF-esp"), 0o600); err != nil {
		t.Fatal(err)
	}
	for _, espFam := range []string{"esp32", "esp32s3"} {
		runner.ran = nil
		err := UploadAtomic(probe, UploadRequest{Family: espFam, FilePath: cleanELF}, runner)
		if err == nil {
			t.Fatalf("expected %s single-ELF upload without --segment to be rejected", espFam)
		}
		if !strings.Contains(err.Error(), "--segment") || !strings.Contains(err.Error(), "0x1000") || !strings.Contains(err.Error(), "0x8000") || !strings.Contains(err.Error(), "0x10000") {
			t.Fatalf("expected clear Espressif --segment remediation in error, got: %v", err)
		}
		if len(runner.ran) != 0 {
			t.Fatalf("OpenOCD must not run on rejected %s single-ELF upload", espFam)
		}
	}
}

func TestUploadAtomicCH32V003GDBBatchAndReadbackVerify(t *testing.T) {
	t.Setenv("PU_LOCK_DIR", filepath.Join(t.TempDir(), "locks"))

	dir := t.TempDir()
	elf := filepath.Join(dir, "ch32 blink space.elf")
	if err := os.WriteFile(elf, []byte("\x7fELF-ch32-payload"), 0o600); err != nil {
		t.Fatal(err)
	}

	probe := usbprobe.ProbeDevice{
		Serial:  "E660CH3200000001",
		VID:     0x2E8A,
		PID:     0x1050,
		GDBPort: "/dev/ttyACM0",
	}

	runner := &fakeUploadRunner{
		bins: map[string]string{"riscv-none-elf-gdb": "/opt/riscv/bin/riscv-none-elf-gdb"},
		onRun: func(spec CommandSpec, stdout, stderr io.Writer) error {
			fmt.Fprintln(stdout, "Loading section .text, size 0x200 lma 0x08000000")
			fmt.Fprintln(stdout, "Section .text, range 0x08000000 -- 0x08000200: matched.")
			return nil
		},
	}

	if err := UploadAtomic(probe, UploadRequest{
		Family:   "ch32v003",
		FilePath: elf,
	}, runner); err != nil {
		t.Fatalf("UploadAtomic ch32v003 failed: %v", err)
	}

	gdbArgs := strings.Join(runner.ran[0].Args, " | ")
	for _, want := range []string{
		"set remotetimeout 20",
		`file "`,
		`fw_ch32 blink space.elf"`,
		"target remote /dev/ttyACM0",
		"load",
		"compare-sections",
		"monitor reset",
		"detach",
		"quit",
	} {
		if !strings.Contains(gdbArgs, want) {
			t.Fatalf("missing %q in CH32 GDB batch args: %s", want, gdbArgs)
		}
	}

	// Empty GDB output (missing "matched" from compare-sections) MUST fail even with exit code 0!
	runner.onRun = func(spec CommandSpec, stdout, stderr io.Writer) error {
		return nil
	}
	if err := UploadAtomic(probe, UploadRequest{
		Family:   "ch32v003",
		FilePath: elf,
	}, runner); err == nil {
		t.Fatal("expected UploadAtomic to fail when GDB compare-sections did not emit 'matched'")
	}

	// Every GDB batch error marker (even when exit code == 0 and even if one section matched) MUST fail!
	errorOutputs := []string{
		"Section .text, range 0x08000000 -- 0x08000200: MIS-MATCHED!",
		"Section .init: matched.\nError: flash page erase failed",
		"Section .init: matched.\nLoad failed",
		"/dev/ttyACM0: No such file or directory.",
		"Section .init: matched.\nRemote failure reply: E01",
		"Section .init: matched.\nCannot access memory at address 0x08000000",
		"Could not open serial port",
	}
	for _, badOut := range errorOutputs {
		runner.onRun = func(spec CommandSpec, stdout, stderr io.Writer) error {
			fmt.Fprintln(stdout, badOut)
			return nil
		}
		if err := UploadAtomic(probe, UploadRequest{
			Family:   "ch32v003",
			FilePath: elf,
		}, runner); err == nil {
			t.Fatalf("expected UploadAtomic to fail on GDB error output %q", badOut)
		}
	}
}
