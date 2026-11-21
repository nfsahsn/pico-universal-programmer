package puid

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/nfsahsn/pico-universal-programmer/host/internal/usbprobe"
)

type fakeRunner struct {
	bins  map[string]string
	onRun func(spec CommandSpec, stdout, stderr io.Writer) error
	ran   []CommandSpec
}

func (f *fakeRunner) LookPath(file string) (string, error) {
	if p, ok := f.bins[file]; ok {
		return p, nil
	}
	return "", fmt.Errorf("executable %q not found", file)
}

func (f *fakeRunner) Run(spec CommandSpec, stdout, stderr io.Writer) error {
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

func TestCheckReportStructContract(t *testing.T) {
	r := CheckReport{
		Family:      "stm32f1",
		Board:       "STM32F103C8",
		ProbeSerial: "E660583883278628",
		ChipID:      "0x1BA01477/0x0410",
		Verdict:     "OK",
		Diagnosis:   "Genuine STM32F1 medium-density target ready",
		IsClone:     false,
		FlashKB:     64,
		VTrefMV:     3300,
		Ready:       true,
		Remediation: nil,
	}
	raw, err := r.FormatJSON()
	if err != nil {
		t.Fatalf("FormatJSON failed: %v", err)
	}
	var decoded CheckReport
	if err := json.Unmarshal(raw, &decoded); err != nil {
		t.Fatalf("json.Unmarshal failed: %v", err)
	}
	if decoded.Family != r.Family || decoded.ProbeSerial != r.ProbeSerial || !decoded.Ready || decoded.FlashKB != 64 || decoded.VTrefMV != 3300 {
		t.Fatalf("round-trip mismatch: got %+v, want %+v", decoded, r)
	}
	human := r.FormatHuman()
	if !strings.Contains(human, "E660583883278628") || !strings.Contains(human, "stm32f1") {
		t.Fatalf("FormatHuman missing core fields:\n%s", human)
	}
}

func TestClassifyGenuineSTM32F1AndSTM32F4(t *testing.T) {
	f1Cases := []struct {
		devID uint32
		want  string
	}{
		{0x0410, "Medium-Density"},
		{0x0412, "Low-Density"},
		{0x0414, "High-Density"},
	}
	for _, tc := range f1Cases {
		rep, err := ClassifyTarget(TargetObservation{
			Family:       "stm32f1",
			DPIDCode:     0x1BA01477,
			DBGMCUIDCode: 0x20030000 | tc.devID,
			FlashKB:      64,
			VTrefMV:      3300,
		})
		if err != nil {
			t.Fatalf("stm32f1 dev_id=0x%03x failed: %v", tc.devID, err)
		}
		if !rep.Ready || rep.IsClone {
			t.Fatalf("expected genuine ready STM32F1 for dev_id=0x%03x, got %+v", tc.devID, rep)
		}
		if !strings.Contains(rep.Diagnosis, tc.want) && !strings.Contains(rep.Board, tc.want) {
			t.Fatalf("expected %q in STM32F1 report, got %+v", tc.want, rep)
		}
	}

	f4DevIDs := []uint32{0x0413, 0x0423, 0x0431, 0x0421}
	for _, devID := range f4DevIDs {
		rep, err := ClassifyTarget(TargetObservation{
			Family:       "stm32f4",
			DPIDCode:     0x2BA01477,
			DBGMCUIDCode: 0x10000000 | devID,
			FlashKB:      512,
			VTrefMV:      3300,
		})
		if err != nil {
			t.Fatalf("stm32f4 dev_id=0x%03x failed: %v", devID, err)
		}
		if !rep.Ready || rep.IsClone {
			t.Fatalf("expected genuine ready STM32F4 for dev_id=0x%03x, got %+v", devID, rep)
		}
	}
}

func TestClassifySTM32BluePillClones(t *testing.T) {
	cases := []struct {
		name      string
		obs       TargetObservation
		wantToken string
	}{
		{
			name: "CS32F103_CKS32F103_2BA01477_on_F1",
			obs: TargetObservation{
				Family:       "stm32f1",
				DPIDCode:     0x2BA01477,
				DBGMCUIDCode: 0x20036410,
				FlashKB:      64,
			},
			wantToken: "CS32F103",
		},
		{
			name: "GD32F103_JEP106_0x7A3",
			obs: TargetObservation{
				Family:       "stm32f1",
				DPIDCode:     0x1BA01477,
				DBGMCUIDCode: 0x13030410,
				ROMJEP106:    0x07A3,
				FlashKB:      64,
			},
			wantToken: "GD32F103",
		},
		{
			name: "APM32F103_JEP106",
			obs: TargetObservation{
				Family:       "stm32f1",
				DPIDCode:     0x2BA01477,
				DBGMCUIDCode: 0x20036410,
				ROMJEP106:    JEP106APM32,
				FlashKB:      64,
			},
			wantToken: "APM32",
		},
		{
			name: "HK32F103_JEP106",
			obs: TargetObservation{
				Family:       "stm32f1",
				DPIDCode:     0x2BA01477,
				DBGMCUIDCode: 0x00000000,
				ROMJEP106:    JEP106HK32,
				FlashKB:      64,
			},
			wantToken: "HK32",
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			rep, err := ClassifyTarget(tc.obs)
			if err != nil {
				t.Fatalf("ClassifyTarget failed: %v", err)
			}
			if !rep.IsClone {
				t.Fatalf("expected IsClone=true for %s, got %+v", tc.name, rep)
			}
			if rep.Ready {
				t.Fatalf("expected Ready=false on flagged clone %s without override, got %+v", tc.name, rep)
			}
			combined := rep.Board + " " + rep.Verdict + " " + rep.Diagnosis
			if !strings.Contains(combined, tc.wantToken) {
				t.Fatalf("expected %q in clone diagnosis, got %+v", tc.wantToken, rep)
			}
			if len(rep.Remediation) == 0 {
				t.Fatalf("expected consumer remediation steps for %s, got empty", tc.name)
			}
		})
	}
}

func TestClassifyRP2040RP2350AndEspressifJTAG(t *testing.T) {
	rp2040Rep, err := ClassifyTarget(TargetObservation{
		Family:   "rp2040",
		DPIDCode: 0x0BC12477,
		VTrefMV:  3300,
	})
	if err != nil || !rp2040Rep.Ready || rp2040Rep.IsClone || !strings.Contains(rp2040Rep.Board, "RP2040") {
		t.Fatalf("unexpected RP2040 classification: %+v err=%v", rp2040Rep, err)
	}

	rp2350Rep, err := ClassifyTarget(TargetObservation{
		Family:   "rp2350",
		DPIDCode: 0x4C013477,
		VTrefMV:  3300,
	})
	if err != nil || !rp2350Rep.Ready || rp2350Rep.IsClone || !strings.Contains(rp2350Rep.Board, "RP2350") {
		t.Fatalf("unexpected RP2350 classification: %+v err=%v", rp2350Rep, err)
	}

	// Multidrop RP2350 IDCODE passed under --family rp2040 must also be recognized!
	rp2350ViaRP2040, err := ClassifyTarget(TargetObservation{
		Family:   "rp2040",
		DPIDCode: 0x4C013477,
	})
	if err != nil || !rp2350ViaRP2040.Ready || !strings.Contains(rp2350ViaRP2040.Board, "RP2350") {
		t.Fatalf("expected RP2350 recognition under rp2040 family, got %+v err=%v", rp2350ViaRP2040, err)
	}

	esp32Rep, err := ClassifyTarget(TargetObservation{
		Family:   "esp32",
		DPIDCode: 0x120034E5,
		VTrefMV:  3300,
	})
	if err != nil || !esp32Rep.Ready || !strings.Contains(esp32Rep.Board, "ESP32") {
		t.Fatalf("unexpected ESP32 classification: %+v err=%v", esp32Rep, err)
	}

	esp32S3Rep, err := ClassifyTarget(TargetObservation{
		Family:   "esp32s3",
		DPIDCode: 0x120034E5,
		VTrefMV:  3300,
	})
	if err != nil || !esp32S3Rep.Ready || !strings.Contains(esp32S3Rep.Board, "ESP32-S3") {
		t.Fatalf("unexpected ESP32-S3 classification: %+v err=%v", esp32S3Rep, err)
	}
}

func TestCH32PUID1QRcmdHelperAndAllErrorCodes(t *testing.T) {
	pkt := BuildGDBQRcmdPacket("puid1")
	if !strings.HasPrefix(pkt, "$qRcmd,7075696431#") || len(pkt) != len("$qRcmd,7075696431#")+2 {
		t.Fatalf("unexpected qRcmd packet: %q", pkt)
	}

	// Verify RSP hex reply decoding works for firmware encode_ascii_as_hex_packet replies
	asciiLine := "PUID1:OK CHIP_ID=0x00300500 FLASH_KB=16 FLASH_WRITES=0 RESUMED=1 OUTEN_OFF=1\n"
	hexPayload := strings.ToUpper(hex.EncodeToString([]byte(asciiLine)))
	var csum byte
	for i := 0; i < len(hexPayload); i++ {
		csum += hexPayload[i]
	}
	rspFrame := fmt.Sprintf("+$%s#%02x", hexPayload, csum)
	decoded, err := DecodeGDBRSPHexReply(rspFrame)
	if err != nil || decoded != asciiLine {
		t.Fatalf("DecodeGDBRSPHexReply failed: got %q err=%v", decoded, err)
	}

	// Both colon-delimited and firmware space-delimited PUID1:OK formats must parse cleanly
	for _, okInput := range []string{
		"PUID1:OK:ID=0x00300500:FLASH_KB=16",
		rspFrame,
	} {
		rep, err := ParsePUID1Response(okInput)
		if err != nil {
			t.Fatalf("ParsePUID1Response(%q) failed: %v", okInput, err)
		}
		if !rep.Ready || rep.ChipID != "0x00300500" || rep.FlashKB != 16 || rep.IsClone {
			t.Fatalf("unexpected PUID1 OK report for %q: %+v", okInput, rep)
		}
	}

	// ERR_NO_REPLY MUST emit all 5 causes!
	noReply, err := ParsePUID1Response("PUID1:ERR_NO_REPLY DIAG=0x1F CAUSES=POWER,GROUND,PULLUP,PD1_REUSE,READ_PROTECT")
	if err != nil {
		t.Fatalf("ParsePUID1Response ERR_NO_REPLY failed: %v", err)
	}
	if noReply.Ready || len(noReply.Remediation) < 5 {
		t.Fatalf("expected Ready=false and >=5 remediation causes, got %+v", noReply)
	}
	joinedRem := strings.ToLower(strings.Join(noReply.Remediation, " | "))
	for _, wantCause := range []string{"power", "ground", "1 k", "pd1", "read protection"} {
		if !strings.Contains(joinedRem, wantCause) {
			t.Fatalf("missing required cause %q in remediation: %v", wantCause, noReply.Remediation)
		}
	}

	// All remaining PUID1 error codes
	errCodes := []string{
		"PUID1:ERR_ALREADY_HALTED",
		"PUID1:ERR_DETACH_FAILED RESUMED=0 OUTEN_OFF=0",
		"PUID1:ERR_ENGINE_BUSY",
		"PUID1:ERR_FAULT_LATCHED",
		"PUID1:ERR_UNEXPECTED_STATE",
	}
	for _, rawErr := range errCodes {
		rep, err := ParsePUID1Response(rawErr)
		if err != nil {
			t.Fatalf("ParsePUID1Response(%q) failed: %v", rawErr, err)
		}
		if rep.Ready || len(rep.Remediation) == 0 || rep.Diagnosis == "" {
			t.Fatalf("expected non-ready report with diagnosis+remediation for %q, got %+v", rawErr, rep)
		}
	}
}

func frameRSPPacket(payload string) string {
	var csum byte
	for i := 0; i < len(payload); i++ {
		csum += payload[i]
	}
	return fmt.Sprintf("$%s#%02x", payload, csum)
}

func TestCheckTargetAcquiresLockAndInvokesTransportsOffline(t *testing.T) {
	t.Setenv("PU_LOCK_DIR", filepath.Join(t.TempDir(), "locks"))

	probe := usbprobe.ProbeDevice{
		Serial:  "E660583883278628",
		VID:     0x2E8A,
		PID:     0x1050,
		DAPPath: "/dev/bus/usb/001/002",
		GDBPort: "/dev/ttyACM0",
	}

	// Hold session lock externally -> CheckTarget must fail with ErrProbeLocked without running tools!
	held, err := usbprobe.AcquireSessionLock(probe.Serial)
	if err != nil {
		t.Fatal(err)
	}
	runner := &fakeRunner{
		bins: map[string]string{
			"openocd":            "/usr/bin/openocd",
			"riscv-none-elf-gdb": "/usr/bin/riscv-none-elf-gdb",
		},
	}
	if _, err := CheckTarget(probe, "stm32f1", runner); !errors.Is(err, usbprobe.ErrProbeLocked) {
		_ = held.Close()
		t.Fatalf("expected ErrProbeLocked, got %v", err)
	}
	if len(runner.ran) != 0 {
		_ = held.Close()
		t.Fatalf("expected zero commands executed while probe locked, got %d", len(runner.ran))
	}
	_ = held.Close()

	// Check STM32F1 CS32 clone detection through OpenOCD output parsing
	runner.onRun = func(spec CommandSpec, stdout, stderr io.Writer) error {
		fmt.Fprintln(stderr, "Info : SWD DPIDR 0x2ba01477")
		fmt.Fprintln(stdout, "0xe0042000: 20036410")
		fmt.Fprintln(stdout, "0x1ffff7e0: 0040")
		return nil
	}
	rep, err := CheckTarget(probe, "stm32f1", runner)
	if err != nil {
		t.Fatalf("CheckTarget stm32f1 failed: %v", err)
	}
	if !rep.IsClone || rep.Ready || rep.ProbeSerial != probe.Serial || rep.FlashKB != 64 {
		t.Fatalf("unexpected stm32f1 clone CheckTarget report: %+v", rep)
	}
	if !strings.Contains(strings.Join(runner.ran[0].Args, " "), "adapter serial "+probe.Serial) {
		t.Fatalf("expected adapter serial injection in OpenOCD args: %v", runner.ran[0].Args)
	}

	// Check classic ESP32 stages embedded pu-esp32.cfg during CheckTarget and cleans it up
	var stagedCfgSeen string
	runner.ran = nil
	runner.onRun = func(spec CommandSpec, stdout, stderr io.Writer) error {
		for i := 0; i+1 < len(spec.Args); i++ {
			if spec.Args[i] == "-f" && strings.Contains(spec.Args[i+1], "pu-esp32.cfg") {
				stagedCfgSeen = spec.Args[i+1]
				raw, readErr := os.ReadFile(stagedCfgSeen)
				if readErr != nil || !strings.Contains(string(raw), "ESP32_FLASH_VOLTAGE 3.3") {
					t.Errorf("staged pu-esp32.cfg invalid: err=%v", readErr)
				}
			}
		}
		fmt.Fprintln(stderr, "Info : JTAG tap: esp32.cpu0 tap/device found: 0x120034e5")
		return nil
	}
	espRep, err := CheckTarget(probe, "esp32", runner)
	if err != nil || !espRep.Ready || espRep.ProbeSerial != probe.Serial {
		t.Fatalf("CheckTarget esp32 failed: rep=%+v err=%v", espRep, err)
	}
	if stagedCfgSeen == "" {
		t.Fatal("expected staged pu-esp32.cfg to be passed to OpenOCD")
	}
	if !strings.Contains(strings.Join(runner.ran[0].Args, " | "), "init; resume; exit") {
		t.Fatalf("expected non-destructive 'init; resume; exit' for esp32 check, got args: %v", runner.ran[0].Args)
	}
	if _, statErr := os.Stat(stagedCfgSeen); !errors.Is(statErr, os.ErrNotExist) {
		t.Fatalf("expected staged pu-esp32.cfg temp dir to be cleaned up, statErr=%v", statErr)
	}

	// Verify ERR_NO_TARGET remediation only suggests `pu recover` for supported families (stm32f1/stm32f4/ch32v003)
	// and provides EN/BOOT + SDIO/GPIO12..15 guidance for esp32/esp32s3.
	for _, fam := range []string{"stm32f1", "stm32f4"} {
		noTgt, err := ClassifyTarget(TargetObservation{Family: fam, DPIDCode: 0})
		if err != nil || noTgt.Verdict != "ERR_NO_TARGET" {
			t.Fatalf("expected ERR_NO_TARGET for %s, got %+v err=%v", fam, noTgt, err)
		}
		joined := strings.Join(noTgt.Remediation, " ")
		if !strings.Contains(joined, "pu recover --family "+fam) {
			t.Fatalf("expected pu recover guidance for %s, got %v", fam, noTgt.Remediation)
		}
	}
	for _, fam := range []string{"esp32", "esp32s3"} {
		noTgt, err := ClassifyTarget(TargetObservation{Family: fam, DPIDCode: 0})
		if err != nil || noTgt.Verdict != "ERR_NO_TARGET" {
			t.Fatalf("expected ERR_NO_TARGET for %s, got %+v err=%v", fam, noTgt, err)
		}
		joined := strings.Join(noTgt.Remediation, " ")
		if strings.Contains(joined, "pu recover") {
			t.Fatalf("must not suggest unsupported 'pu recover' for %s: %v", fam, noTgt.Remediation)
		}
		for _, want := range []string{"EN/BOOT", "SDIO", "GPIO12..15", "deep sleep"} {
			if !strings.Contains(joined, want) {
				t.Fatalf("expected %q in %s ERR_NO_TARGET remediation, got %v", want, fam, noTgt.Remediation)
			}
		}
	}
	for _, fam := range []string{"rp2040", "rp2350"} {
		noTgt, err := ClassifyTarget(TargetObservation{Family: fam, DPIDCode: 0})
		if err != nil || noTgt.Verdict != "ERR_NO_TARGET" {
			t.Fatalf("expected ERR_NO_TARGET for %s, got %+v err=%v", fam, noTgt, err)
		}
		if strings.Contains(strings.Join(noTgt.Remediation, " "), "pu recover") {
			t.Fatalf("must not suggest unsupported 'pu recover' for %s: %v", fam, noTgt.Remediation)
		}
	}

	// Check CH32V003 via raw CDC PortOpener (MUST NOT spawn riscv-none-elf-gdb!)
	runner.ran = nil
	delete(runner.bins, "riscv-none-elf-gdb")

	// 1. Standard RSP $O<hex>#.. + $OK#9a stream over net.Pipe()
	srv1, cli1 := net.Pipe()
	go func() {
		defer srv1.Close()
		wantPkt := BuildGDBQRcmdPacket("puid1")
		got := make([]byte, len(wantPkt))
		if _, rErr := io.ReadFull(srv1, got); rErr != nil || string(got) != wantPkt {
			return
		}
		hexLine := strings.ToUpper(hex.EncodeToString([]byte("PUID1:OK CHIP_ID=0x00300500 FLASH_KB=16 FLASH_WRITES=2 RESUMED=1 OUTEN_OFF=1\n")))
		_, _ = io.WriteString(srv1, "+"+frameRSPPacket("O"+hexLine)+frameRSPPacket("OK"))
	}()
	chRep, err := CheckTargetWithOptions(probe, "ch32v003", CheckOptions{
		Runner: runner,
		PortOpener: func(portPath string) (io.ReadWriteCloser, error) {
			if portPath != probe.GDBPort {
				return nil, fmt.Errorf("unexpected port %q", portPath)
			}
			return cli1, nil
		},
	})
	if err != nil || !chRep.Ready || chRep.FlashKB != 16 || chRep.ChipID != "0x00300500" {
		t.Fatalf("CheckTargetWithOptions ch32v003 ($O+$OK) failed: rep=%+v err=%v", chRep, err)
	}
	if len(runner.ran) != 0 {
		t.Fatalf("expected ZERO external GDB invocations for ch32v003 check, got %d: %+v", len(runner.ran), runner.ran)
	}

	// 2. Direct firmware encode_ascii_as_hex_packet ($<hex>#.. without trailing $OK, port stays open)
	srv2, cli2 := net.Pipe()
	defer srv2.Close()
	go func() {
		wantPkt := BuildGDBQRcmdPacket("puid1")
		got := make([]byte, len(wantPkt))
		if _, rErr := io.ReadFull(srv2, got); rErr != nil {
			return
		}
		hexLine := strings.ToUpper(hex.EncodeToString([]byte("PUID1:ERR_ALREADY_HALTED\n")))
		_, _ = io.WriteString(srv2, "+"+frameRSPPacket(hexLine))
	}()
	haltedRep, err := CheckTargetWithOptions(probe, "ch32v003", CheckOptions{
		Runner:     runner,
		PortOpener: func(string) (io.ReadWriteCloser, error) { return cli2, nil },
	})
	if err != nil || haltedRep.Ready || haltedRep.Verdict != "ERR_ALREADY_HALTED" {
		t.Fatalf("expected ERR_ALREADY_HALTED from raw firmware RSP packet, got rep=%+v err=%v", haltedRep, err)
	}

	// 3. RSP error reply ($E01#a6) must surface a clean error
	srv3, cli3 := net.Pipe()
	go func() {
		defer srv3.Close()
		buf := make([]byte, 32)
		_, _ = srv3.Read(buf)
		_, _ = io.WriteString(srv3, "+"+frameRSPPacket("E01"))
	}()
	if _, err := CheckTargetWithOptions(probe, "ch32v003", CheckOptions{
		Runner:     runner,
		PortOpener: func(string) (io.ReadWriteCloser, error) { return cli3, nil },
	}); err == nil {
		t.Fatal("expected error when probe replies $E01 to qRcmd puid1")
	}

	// CoreSight ROM table PIDR4/PIDR1/PIDR2 -> JEP106 0x7A3 (GD32F103) + VTREF out-of-range check
	romOut := strings.Join([]string{
		"Info : SWD DPIDR 0x1ba01477",
		"0xe0042000: 13030410",
		"0x1ffff7e0: 0040",
		"0xe00fffd0: 0000000f 00000000 00000000 00000000",
		"0xe00fffe0: 00000010 00000030 0000000a 00000000",
	}, "\n")
	obs, err := ParseOpenOCDCheckOutput("stm32f1", romOut)
	if err != nil || obs.ROMJEP106 != JEP106GD32 {
		t.Fatalf("expected ROM table JEP106=0x%03X, got 0x%03X err=%v", JEP106GD32, obs.ROMJEP106, err)
	}
	gdRep, err := ClassifyTarget(obs)
	if err != nil || !gdRep.IsClone || !strings.Contains(gdRep.Board, "GD32F103") {
		t.Fatalf("expected GD32F103 clone from ROM table, got %+v err=%v", gdRep, err)
	}

	lowVRep, err := ClassifyTarget(TargetObservation{
		Family:   "rp2040",
		DPIDCode: 0x0BC12477,
		VTrefMV:  450,
	})
	if err != nil || lowVRep.Ready || lowVRep.Verdict != "ERR_VTREF_OUT_OF_RANGE" {
		t.Fatalf("expected out-of-range VTREF failure, got %+v err=%v", lowVRep, err)
	}

	// Echoed config line mentioning CPUTAPID 0x1ba01477 before UNEXPECTED: 0x2ba01477 must NOT mask a CS32 clone!
	echoedOut := "Debug: CPUTAPID 0x1ba01477 configured\nWarn : UNEXPECTED: 0x2ba01477 (mfg: 0x23b)\nError: expected 1 of 1: 0x1ba01477\n"
	echoObs, err := ParseOpenOCDCheckOutput("stm32f1", echoedOut)
	if err != nil || echoObs.DPIDCode != 0x2BA01477 {
		t.Fatalf("expected wire UNEXPECTED 0x2BA01477 to win over config CPUTAPID 0x1BA01477, got 0x%08X err=%v", echoObs.DPIDCode, err)
	}
	echoRep, err := ClassifyTarget(echoObs)
	if err != nil || !echoRep.IsClone {
		t.Fatalf("expected CS32 clone detection from UNEXPECTED line, got %+v err=%v", echoRep, err)
	}
}

func TestCheckTargetIF5PreFlightDetectsBusyFaultAndPopulatesTelemetry(t *testing.T) {
	t.Setenv("PU_LOCK_DIR", filepath.Join(t.TempDir(), "locks"))

	probe := usbprobe.ProbeDevice{
		Serial:   "E660IF5CHECK0001",
		VID:      0x2E8A,
		PID:      0x1050,
		DAPPath:  "/dev/bus/usb/001/020",
		CtrlPath: "/dev/bus/usb/001/020",
	}

	runner := &fakeRunner{
		bins: map[string]string{"openocd": "/usr/bin/openocd"},
	}

	// 1. IF 5 reports PU_ENGINE_SWD active -> short-circuits with ERR_ENGINE_BUSY BEFORE OpenOCD runs!
	busyRep, err := CheckTargetWithOptions(probe, "stm32f1", CheckOptions{
		Runner: runner,
		CtrlTransceiver: func(_ usbprobe.ProbeDevice, req []byte) ([]byte, error) {
			if req[0] == usbprobe.CtrlCmdGetInfo {
				return []byte{0x01, 0x00, 1, 0, 0, 1, 0x07, usbprobe.EngineSWD, 0, 0, 0, 0, 0}, nil
			}
			return []byte{0x02, 0x00, 0xE4, 0x0C, 0x10, 0x00, 1, 0, 1, 0, 0, 0, 0}, nil
		},
	})
	if err != nil || busyRep.Ready || busyRep.Verdict != "ERR_ENGINE_BUSY" {
		t.Fatalf("expected pre-flight ERR_ENGINE_BUSY, got rep=%+v err=%v", busyRep, err)
	}
	if len(runner.ran) != 0 {
		t.Fatalf("OpenOCD must NOT run when IF 5 reports engine busy; ran=%d", len(runner.ran))
	}

	// 2. IF 5 reports PU_ENGINE_FAULT / overcurrent -> short-circuits with ERR_FAULT_LATCHED BEFORE OpenOCD runs!
	faultRep, err := CheckTargetWithOptions(probe, "rp2040", CheckOptions{
		Runner: runner,
		CtrlTransceiver: func(_ usbprobe.ProbeDevice, req []byte) ([]byte, error) {
			if req[0] == usbprobe.CtrlCmdGetInfo {
				return []byte{0x01, 0x00, 1, 0, 0, 1, 0x07, usbprobe.EngineFault, 0, 0x01, 0, 0, 0}, nil
			}
			return []byte{0x02, 0x00, 0x00, 0x00, 0x20, 0x02, 0, 1, 1, 0, 0, 0, 0}, nil
		},
	})
	if err != nil || faultRep.Ready || faultRep.Verdict != "ERR_FAULT_LATCHED" {
		t.Fatalf("expected pre-flight ERR_FAULT_LATCHED, got rep=%+v err=%v", faultRep, err)
	}
	if len(runner.ran) != 0 {
		t.Fatalf("OpenOCD must NOT run when IF 5 reports latched fault; ran=%d", len(runner.ran))
	}

	// 3. Healthy IF 5 populates live VTrefMV & ITargMA, and esp32s3 injects explicit adapter speed 5000!
	runner.onRun = func(spec CommandSpec, stdout, stderr io.Writer) error {
		fmt.Fprintln(stderr, "Info : JTAG tap: esp32s3.cpu0 tap/device found: 0x120034e5")
		return nil
	}
	s3Rep, err := CheckTargetWithOptions(probe, "esp32s3", CheckOptions{
		Runner: runner,
		CtrlTransceiver: func(_ usbprobe.ProbeDevice, req []byte) ([]byte, error) {
			if req[0] == usbprobe.CtrlCmdGetInfo {
				return []byte{0x01, 0x00, 1, 0, 0, 1, 0x07, usbprobe.EngineIdle, 0, 0, 0, 0, 0}, nil
			}
			// VTrefMV = 3315 (0x0CF3), ITargMA = 38 (0x0026)
			return []byte{0x02, 0x00, 0xF3, 0x0C, 0x26, 0x00, 1, 0, 1, 0, 0, 0, 0}, nil
		},
	})
	if err != nil || !s3Rep.Ready || s3Rep.VTrefMV != 3315 || s3Rep.ITargMA != 38 {
		t.Fatalf("expected live VTrefMV=3315 and ITargMA=38 on esp32s3 report, got rep=%+v err=%v", s3Rep, err)
	}
	if len(runner.ran) != 1 || !strings.Contains(strings.Join(runner.ran[0].Args, " | "), "adapter speed 5000") {
		t.Fatalf("expected explicit 'adapter speed 5000' in esp32s3 OpenOCD check args: %+v", runner.ran)
	}
}
