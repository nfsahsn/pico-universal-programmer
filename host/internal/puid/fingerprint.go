package puid

import (
	"bytes"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/nfsahsn/pico-universal-programmer/host/internal/catalog"
	"github.com/nfsahsn/pico-universal-programmer/host/internal/gdbpipe"
	"github.com/nfsahsn/pico-universal-programmer/host/internal/usbprobe"
)

const (
	// SWD / JTAG IDCODEs across supported target architectures.
	IDCodeSTM32F1SWD  uint32 = 0x1BA01477
	IDCodeCortexM4SWD uint32 = 0x2BA01477
	IDCodeCortexM0SWD uint32 = 0x0BB11477
	IDCodeCortexM0P   uint32 = 0x0BC11477
	IDCodeRP2040SWD   uint32 = 0x0BC12477
	IDCodeRP2350SWD   uint32 = 0x4C013477
	IDCodeESP32JTAG   uint32 = 0x120034E5

	// 11-bit JEP106 manufacturer codes ((continuation << 7) | id7).
	JEP106STMicro uint16 = 0x0020
	JEP106ARM     uint16 = 0x023B
	JEP106GD32    uint16 = 0x07A3 // GigaDevice Semiconductor (Bank 16, ID 0x23)
	JEP106APM32   uint16 = 0x052B // Geehy Semiconductor (Bank 11, ID 0x2B)
	JEP106HK32    uint16 = 0x0555 // Shenzhen Hangshun Chip Technology

	minValidVTrefMV = 1650
	maxValidVTrefMV = 5500
)

// CheckReport captures a structured pre-flight probe and target MCU diagnostic verdict.
type CheckReport struct {
	Family      string   `json:"family"`
	Board       string   `json:"board,omitempty"`
	ProbeSerial string   `json:"probe_serial"`
	ChipID      string   `json:"chip_id,omitempty"`
	Verdict     string   `json:"verdict"`
	Diagnosis   string   `json:"diagnosis,omitempty"`
	IsClone     bool     `json:"is_clone"`
	FlashKB     int      `json:"flash_kb,omitempty"`
	VTrefMV     int      `json:"vtref_mv,omitempty"`
	ITargMA     int      `json:"itarg_ma,omitempty"`
	Ready       bool     `json:"ready"`
	Remediation []string `json:"remediation,omitempty"`
}

// FormatJSON serializes CheckReport as indented JSON.
func (r CheckReport) FormatJSON() ([]byte, error) {
	var buf bytes.Buffer
	enc := json.NewEncoder(&buf)
	enc.SetIndent("", "  ")
	if err := enc.Encode(r); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// FormatHuman renders CheckReport as a clear consumer-friendly diagnostic block.
func (r CheckReport) FormatHuman() string {
	var b strings.Builder
	status := "NOT READY"
	if r.Ready {
		status = "READY"
	}
	fmt.Fprintf(&b, "Target Check [%s]\n", status)
	fmt.Fprintf(&b, "  Family:       %s\n", r.Family)
	if r.Board != "" {
		fmt.Fprintf(&b, "  Target:       %s\n", r.Board)
	}
	if r.ProbeSerial != "" {
		fmt.Fprintf(&b, "  Probe Serial: %s\n", r.ProbeSerial)
	}
	if r.ChipID != "" {
		fmt.Fprintf(&b, "  Chip ID:      %s\n", r.ChipID)
	}
	fmt.Fprintf(&b, "  Verdict:      %s\n", r.Verdict)
	if r.IsClone {
		fmt.Fprintf(&b, "  Clone MCU:    YES (counterfeit/compatible clone detected)\n")
	}
	if r.FlashKB > 0 {
		fmt.Fprintf(&b, "  Flash Size:   %d KiB\n", r.FlashKB)
	}
	if r.VTrefMV > 0 {
		fmt.Fprintf(&b, "  VTREF:        %d mV\n", r.VTrefMV)
	}
	if r.ITargMA > 0 {
		fmt.Fprintf(&b, "  ITARG:        %d mA\n", r.ITargMA)
	}
	if r.Diagnosis != "" {
		fmt.Fprintf(&b, "  Diagnosis:    %s\n", r.Diagnosis)
	}
	if len(r.Remediation) > 0 {
		fmt.Fprintf(&b, "  Remediation:\n")
		for _, step := range r.Remediation {
			fmt.Fprintf(&b, "    - %s\n", step)
		}
	}
	return b.String()
}

// CommandSpec describes a subprocess invocation for OpenOCD or GDB.
type CommandSpec struct {
	Name string
	Args []string
	Env  []string
}

// CommandRunner abstracts external tool execution so tests run strictly offline.
type CommandRunner interface {
	LookPath(file string) (string, error)
	Run(spec CommandSpec, stdout, stderr io.Writer) error
}

// OSCommandRunner executes real host processes when no fake runner is injected.
type OSCommandRunner struct{}

func (OSCommandRunner) LookPath(file string) (string, error) {
	return catalog.ResolveHostTool(file)
}

func (OSCommandRunner) Run(spec CommandSpec, stdout, stderr io.Writer) error {
	c := exec.Command(spec.Name, spec.Args...)
	if len(spec.Env) > 0 {
		c.Env = append(os.Environ(), spec.Env...)
	}
	c.Stdout = stdout
	c.Stderr = stderr
	return c.Run()
}

// TargetObservation holds raw register and wire observations for SWD/JTAG classification.
type TargetObservation struct {
	Family        string
	DPIDCode      uint32
	DBGMCUIDCode  uint32
	ROMJEP106     uint16
	FlashKB       int
	VTrefMV       int
	ITargMA       int
	ReadProtected bool
	EngineBusy    bool
	FaultLatched  bool
	RawOutput     string
}

// NormalizeFamily canonicalizes a target family name.
func NormalizeFamily(raw string) (string, error) {
	f := strings.ToLower(strings.TrimSpace(raw))
	switch f {
	case "stm32f1", "stm32f4", "rp2040", "rp2350", "esp32", "esp32s3", "ch32v003":
		return f, nil
	case "esp32-s3":
		return "esp32s3", nil
	case "ch32v":
		return "ch32v003", nil
	default:
		return "", fmt.Errorf("puid: unsupported target family %q (expected stm32f1, stm32f4, rp2040, rp2350, esp32, esp32s3, or ch32v003)", raw)
	}
}

// ClassifyTarget classifies an SWD/JTAG TargetObservation into a CheckReport,
// explicitly detecting STM32 Blue Pill clones (CS32/CKS32, GD32, APM32, HK32),
// genuine STM32F1/F4, RP2040/RP2350 multidrop SWD, and ESP32/ESP32-S3 JTAG.
func ClassifyTarget(obs TargetObservation) (CheckReport, error) {
	family, err := NormalizeFamily(obs.Family)
	if err != nil {
		return CheckReport{}, err
	}

	rep := CheckReport{
		Family:  family,
		FlashKB: obs.FlashKB,
		VTrefMV: obs.VTrefMV,
		ITargMA: obs.ITargMA,
	}

	if obs.FaultLatched {
		rep.Verdict = "ERR_FAULT_LATCHED"
		rep.Diagnosis = "Probe VTARG overcurrent or hardware protection fault is latched"
		rep.Remediation = []string{
			"Disconnect target board and check VCC/GND for short circuits.",
			"Clear the latched probe fault or reconnect USB before retrying.",
		}
		return rep, nil
	}
	if obs.EngineBusy {
		rep.Verdict = "ERR_ENGINE_BUSY"
		rep.Diagnosis = "Probe debug engine is currently owned by another active USB interface"
		rep.Remediation = []string{
			"Close any active GDB or OpenOCD debug session using this Pico Universal probe.",
		}
		return rep, nil
	}
	if obs.VTrefMV > 0 && (obs.VTrefMV < minValidVTrefMV || obs.VTrefMV > maxValidVTrefMV) {
		rep.Verdict = "ERR_VTREF_OUT_OF_RANGE"
		rep.Diagnosis = fmt.Sprintf("Measured target VTREF (%d mV) is outside safe 1.8 V - 5.0 V operating range", obs.VTrefMV)
		rep.Remediation = []string{
			"Verify target board 3.3 V / VCC power supply is turned on and connected to VTREF.",
			"Ensure target logic voltage is between 1.8 V (1800 mV) and 5.0 V (5000 mV).",
		}
		return rep, nil
	}
	if obs.ReadProtected {
		rep.Verdict = "ERR_READ_PROTECTED"
		rep.Diagnosis = "Target flash read-out protection (RDP) is active"
		rep.Remediation = []string{
			fmt.Sprintf("Run 'pu recover --family %s' to perform mass-erase unlock (erases existing firmware).", family),
		}
		return rep, nil
	}
	if obs.DPIDCode == 0 || obs.DPIDCode == 0xFFFFFFFF {
		rep.Verdict = "ERR_NO_TARGET"
		rep.Diagnosis = "No valid SWD/JTAG IDCODE reply from target MCU"
		switch family {
		case "stm32f1", "stm32f4", "ch32v003":
			rep.Remediation = []string{
				"Check target board power (3.3 V / VTREF) and shared GND connection.",
				"Verify SWCLK/SWDIO (or SWIO) ribbon wiring.",
				"If user firmware disabled debug pins or entered deep sleep, run 'pu recover --family " + family + "' or hold NRST.",
			}
		case "esp32", "esp32s3":
			rep.Remediation = []string{
				"Check target board power (3.3 V / VTREF) and shared GND connection.",
				"Verify 4-wire JTAG wiring (GPIO13=TCK, GPIO14=TMS, GPIO12=TDI, GPIO15=TDO).",
				"Check EN/BOOT state (ROM download mode muxes GPIO12..15 to SDIO, disabling JTAG) or flash a JTAG-safe image if user firmware reconfigured GPIO12..15 or entered deep sleep.",
			}
		default:
			rep.Remediation = []string{
				"Check target board power (3.3 V / VTREF) and shared GND connection.",
				"Verify SWCLK/SWDIO ribbon wiring.",
				"Power-cycle or hold RUN/BOOTSEL if user firmware disabled SWD pins or entered deep sleep.",
			}
		}
		return rep, nil
	}

	switch family {
	case "stm32f1":
		return classifySTM32F1(rep, obs), nil
	case "stm32f4":
		return classifySTM32F4(rep, obs), nil
	case "rp2040", "rp2350":
		return classifyRP2xxx(rep, obs), nil
	case "esp32", "esp32s3":
		return classifyEspressif(rep, obs), nil
	default:
		return CheckReport{}, fmt.Errorf("puid:ClassifyTarget does not handle %q", family)
	}
}

func classifySTM32F1(rep CheckReport, obs TargetObservation) CheckReport {
	devID := obs.DBGMCUIDCode & 0x0FFF
	if obs.DBGMCUIDCode != 0 {
		rep.ChipID = fmt.Sprintf("0x%08X/0x%03X", obs.DPIDCode, devID)
	} else {
		rep.ChipID = fmt.Sprintf("0x%08X", obs.DPIDCode)
	}

	rawLower := strings.ToLower(obs.RawOutput)

	// 1. Explicit JEP106 / ROM-table clone classification first.
	switch {
	case obs.ROMJEP106 == JEP106GD32 || strings.Contains(rawLower, "gd32"):
		rep.IsClone = true
		rep.Ready = false
		rep.Board = "GD32F103 (Blue Pill Clone)"
		rep.Verdict = "CLONE_DETECTED"
		rep.Diagnosis = fmt.Sprintf("Detected GigaDevice GD32F103 clone (JEP106 0x%03X, DP 0x%08X) on STM32F1 target", JEP106GD32, obs.DPIDCode)
		rep.Remediation = []string{
			"GD32F103 executes zero-wait-state cached flash faster than genuine STM32F103; verify software delay loops and USB pull-up resistors.",
			"Confirm actual flash density (64 KiB vs 128 KiB) and lower SWD clock if flash programming times out.",
		}
		return rep

	case obs.ROMJEP106 == JEP106APM32 || strings.Contains(rawLower, "apm32"):
		rep.IsClone = true
		rep.Ready = false
		rep.Board = "APM32F103 (Blue Pill Clone)"
		rep.Verdict = "CLONE_DETECTED"
		rep.Diagnosis = fmt.Sprintf("Detected Geehy APM32F103 clone (JEP106 0x%03X, DP 0x%08X) on STM32F1 target", JEP106APM32, obs.DPIDCode)
		rep.Remediation = []string{
			"Use '-c \"set CPUTAPID 0\"' in custom OpenOCD scripts if CPUTAPID 0x2BA01477 is rejected by stock stm32f1x.cfg.",
			"Verify APM32F103 peripheral errata (ADC/USB/CAN) before deploying timing-critical STM32F1 firmware.",
		}
		return rep

	case obs.ROMJEP106 == JEP106HK32 || strings.Contains(rawLower, "hk32") ||
		obs.DPIDCode == IDCodeCortexM0SWD || obs.DPIDCode == IDCodeCortexM0P:
		rep.IsClone = true
		rep.Ready = false
		rep.Board = "HK32F103 (Blue Pill Clone)"
		rep.Verdict = "CLONE_DETECTED"
		rep.Diagnosis = fmt.Sprintf("Detected Hangshun HK32 / non-M3 clone (JEP106 0x%03X, DP 0x%08X) on STM32F1 target", obs.ROMJEP106, obs.DPIDCode)
		rep.Remediation = []string{
			"Verify whether this package is an HK32F103C8T6 (Cortex-M3) or incompatible HK32F103C8M6 (Cortex-M0) remark.",
			"Use '-c \"set CPUTAPID 0\"' for OpenOCD SWD attachment and replace remark chips for genuine STM32F1 compatibility.",
		}
		return rep
	}

	// 2. CS32F103 / CKS32F103 clone: Cortex-M r2p0 SWD IDCODE 0x2BA01477 on an STM32F1 target!
	if obs.DPIDCode == IDCodeCortexM4SWD {
		rep.IsClone = true
		rep.Ready = false
		rep.Board = "CS32F103 / CKS32F103 (Blue Pill Clone)"
		rep.Verdict = "CLONE_DETECTED"
		rep.Diagnosis = "Detected CS32F103/CKS32F103 (or APM32/HK32) Blue Pill clone: SWD IDCODE 0x2BA01477 instead of genuine STM32F1 0x1BA01477"
		rep.Remediation = []string{
			"Stock OpenOCD stm32f1x.cfg rejects CPUTAPID 0x2BA01477; pass '-c \"set CPUTAPID 0x2ba01477\"' or '-c \"set CPUTAPID 0\"'.",
			"Replace counterfeit Blue Pill MCU with genuine STMicroelectronics STM32F103C8T6/CBT6 for hardware-qualified operation.",
		}
		return rep
	}

	// 3. Genuine STM32F10x: IDCODE 0x1BA01477 + STM32F1 DEV_ID.
	if obs.DPIDCode == IDCodeSTM32F1SWD {
		density := ""
		switch devID {
		case 0x412:
			density = "STM32F103 Low-Density"
		case 0x410:
			density = "STM32F103 Medium-Density (F103C8/CB)"
		case 0x414:
			density = "STM32F103 High-Density"
		case 0x418:
			density = "STM32F105/F107 Connectivity-Line"
		case 0x430:
			density = "STM32F103 XL-Density"
		}
		if density != "" {
			rep.Board = density
			rep.Verdict = "OK"
			rep.Diagnosis = fmt.Sprintf("Genuine %s verified (SWD 0x%08X, DEV_ID 0x%03X)", density, obs.DPIDCode, devID)
			rep.Ready = true
			return rep
		}
		if obs.DBGMCUIDCode == 0 {
			// Unlocked F1 returning 0x00000000 at DBGMCU_IDCODE is characteristic of HK32F103 clones.
			rep.IsClone = true
			rep.Ready = false
			rep.Board = "HK32F103 / Unverified F1 Clone"
			rep.Verdict = "CLONE_DETECTED"
			rep.Diagnosis = "SWD DP 0x1BA01477 responded, but DBGMCU_IDCODE read as 0x00000000 (characteristic of HK32F103 clone or sleeping debug domain)"
			rep.Remediation = []string{
				"Hold NRST or run 'pu recover --family stm32f1' if user firmware entered deep sleep.",
				"If DBGMCU_IDCODE remains 0x00000000 while halted, the MCU is an HK32F103 clone rather than genuine STM32F103.",
			}
			return rep
		}
		rep.Verdict = "ERR_FAMILY_MISMATCH"
		rep.Diagnosis = fmt.Sprintf("SWD DP 0x1BA01477 responded with unexpected DEV_ID 0x%03X for stm32f1", devID)
		rep.Remediation = []string{
			"Verify the connected board matches --family stm32f1.",
		}
		return rep
	}

	rep.Verdict = "ERR_UNEXPECTED_IDCODE"
	rep.Diagnosis = fmt.Sprintf("Unexpected SWD IDCODE 0x%08X for stm32f1 target", obs.DPIDCode)
	rep.Remediation = []string{
		"Verify target board family (--family stm32f4, rp2040, esp32, or ch32v003) and SWD wiring.",
	}
	return rep
}

func classifySTM32F4(rep CheckReport, obs TargetObservation) CheckReport {
	devID := obs.DBGMCUIDCode & 0x0FFF
	if obs.DBGMCUIDCode != 0 {
		rep.ChipID = fmt.Sprintf("0x%08X/0x%03X", obs.DPIDCode, devID)
	} else {
		rep.ChipID = fmt.Sprintf("0x%08X", obs.DPIDCode)
	}

	if obs.DPIDCode != IDCodeCortexM4SWD {
		rep.Verdict = "ERR_UNEXPECTED_IDCODE"
		rep.Diagnosis = fmt.Sprintf("Unexpected SWD IDCODE 0x%08X for stm32f4 (expected 0x2BA01477)", obs.DPIDCode)
		rep.Remediation = []string{
			"Check whether the connected board is STM32F1 (--family stm32f1) or RP2040 (--family rp2040).",
		}
		return rep
	}

	part := ""
	switch devID {
	case 0x413:
		part = "STM32F405/F407"
	case 0x419:
		part = "STM32F42x/F43x"
	case 0x423:
		part = "STM32F401xB/C"
	case 0x433:
		part = "STM32F401xD/E"
	case 0x431:
		part = "STM32F411xC/E"
	case 0x421:
		part = "STM32F446xx"
	case 0x434:
		part = "STM32F469/F479"
	case 0x441:
		part = "STM32F412"
	case 0x458:
		part = "STM32F410"
	case 0x463:
		part = "STM32F413/F423"
	}
	if part == "" {
		rep.Verdict = "ERR_FAMILY_MISMATCH"
		rep.Diagnosis = fmt.Sprintf("Cortex-M4 SWD DP 0x2BA01477 responded with unrecognized STM32F4 DEV_ID 0x%03X (Note: CS32F103 clones also report 0x2BA01477 with DEV_ID 0x410)", devID)
		rep.Remediation = []string{
			"If this is an STM32F103 Blue Pill, re-run with '--family stm32f1' to run clone diagnostics.",
			"Verify target power and NRST if DBGMCU_IDCODE could not be read.",
		}
		return rep
	}

	rep.Board = part
	rep.Verdict = "OK"
	rep.Diagnosis = fmt.Sprintf("Genuine %s verified (SWD 0x%08X, DEV_ID 0x%03X)", part, obs.DPIDCode, devID)
	rep.Ready = true
	return rep
}

func classifyRP2xxx(rep CheckReport, obs TargetObservation) CheckReport {
	rep.ChipID = fmt.Sprintf("0x%08X", obs.DPIDCode)
	switch obs.DPIDCode {
	case IDCodeRP2040SWD:
		if rep.Family == "rp2350" {
			rep.Verdict = "ERR_FAMILY_MISMATCH"
			rep.Board = "RP2040"
			rep.Diagnosis = "Detected RP2040 multidrop SWD IDCODE 0x0BC12477, but --family rp2350 was requested"
			rep.Remediation = []string{"Re-run with '--family rp2040' for Raspberry Pi Pico / Pico W."}
			return rep
		}
		rep.Board = "RP2040"
		rep.Verdict = "OK"
		rep.Diagnosis = "Raspberry Pi RP2040 dual-core SWD multidrop target verified (0x0BC12477)"
		rep.Ready = true
		return rep

	case IDCodeRP2350SWD:
		rep.Board = "RP2350"
		rep.Verdict = "OK"
		rep.Diagnosis = "Raspberry Pi RP2350 SWD multidrop target verified (0x4C013477)"
		rep.Ready = true
		return rep

	default:
		rep.Verdict = "ERR_UNEXPECTED_IDCODE"
		rep.Diagnosis = fmt.Sprintf("Unexpected SWD IDCODE 0x%08X for %s (expected 0x0BC12477 or 0x4C013477)", obs.DPIDCode, rep.Family)
		rep.Remediation = []string{
			"Verify SWCLK, SWDIO, and GND connections to the Pico SWD debug header.",
		}
		return rep
	}
}

func classifyEspressif(rep CheckReport, obs TargetObservation) CheckReport {
	rep.ChipID = fmt.Sprintf("0x%08X", obs.DPIDCode)
	if obs.DPIDCode != IDCodeESP32JTAG {
		rep.Verdict = "ERR_UNEXPECTED_IDCODE"
		rep.Diagnosis = fmt.Sprintf("Unexpected JTAG IDCODE 0x%08X for %s (expected Tensilica Xtensa 0x120034E5)", obs.DPIDCode, rep.Family)
		rep.Remediation = []string{
			"Verify 4-wire JTAG connections: GPIO13=TCK, GPIO14=TMS, GPIO12=TDI, GPIO15=TDO, plus GND and 3.3 V.",
			"Ensure user sketch has not reconfigured GPIO12..15 away from JTAG function.",
		}
		return rep
	}

	if rep.Family == "esp32s3" || strings.Contains(strings.ToLower(obs.RawOutput), "esp32s3") {
		rep.Board = "ESP32-S3"
		rep.Verdict = "OK"
		rep.Diagnosis = "Espressif ESP32-S3 Xtensa dual-core JTAG target verified (0x120034E5)"
		rep.Ready = true
		return rep
	}

	rep.Board = "ESP32"
	rep.Verdict = "OK"
	rep.Diagnosis = "Espressif classic ESP32 Xtensa dual-core JTAG target verified (0x120034E5)"
	rep.Ready = true
	return rep
}

// BuildGDBQRcmdPacket encodes a GDB monitor command (such as "puid1") as a complete
// GDB Remote Serial Protocol `$qRcmd,<hex>#<csum>` frame.
func BuildGDBQRcmdPacket(monitorCmd string) string {
	hexCmd := hex.EncodeToString([]byte(monitorCmd))
	body := "qRcmd," + hexCmd
	var csum byte
	for i := 0; i < len(body); i++ {
		csum += body[i]
	}
	return fmt.Sprintf("$%s#%02x", body, csum)
}

// DecodeGDBRSPHexReply decodes a GDB RSP packet (`$<hex>#<csum>` or `+$O<hex>#<csum>`)
// carrying hex-encoded ASCII monitor output from the probe's SWIO engine.
func DecodeGDBRSPHexReply(packet string) (string, error) {
	s := strings.TrimSpace(packet)
	s = strings.TrimLeft(s, "+")
	dollar := strings.IndexByte(s, '$')
	hash := strings.LastIndexByte(s, '#')
	if dollar < 0 || hash <= dollar || hash+2 >= len(s) {
		return "", fmt.Errorf("puid: malformed GDB RSP packet %q", packet)
	}
	payload := s[dollar+1 : hash]
	csumHex := s[hash+1 : hash+3]
	wantCsum, err := strconv.ParseUint(csumHex, 16, 8)
	if err != nil {
		return "", fmt.Errorf("puid: invalid GDB RSP checksum %q: %w", csumHex, err)
	}
	var gotCsum byte
	for i := 0; i < len(payload); i++ {
		gotCsum += payload[i]
	}
	if gotCsum != byte(wantCsum) {
		return "", fmt.Errorf("puid: GDB RSP checksum mismatch: got %02x, want %02x", gotCsum, byte(wantCsum))
	}

	hexBody := payload
	if strings.HasPrefix(hexBody, "O") && len(hexBody)%2 == 1 {
		hexBody = hexBody[1:]
	}
	decoded, err := hex.DecodeString(hexBody)
	if err != nil {
		return "", fmt.Errorf("puid: decode RSP hex payload: %w", err)
	}
	return string(decoded), nil
}

// ParsePUID1Response parses CH32V003 PUID1 check output (either decoded ASCII or a raw
// GDB RSP hex frame) into a structured CheckReport.
func ParsePUID1Response(raw string) (CheckReport, error) {
	text := strings.TrimSpace(raw)
	if text == "" {
		return CheckReport{}, errors.New("puid: empty PUID1 response")
	}

	if !strings.Contains(text, "PUID1:") && strings.Contains(text, "$") && strings.Contains(text, "#") {
		if decoded, err := DecodeGDBRSPHexReply(text); err == nil {
			text = strings.TrimSpace(decoded)
		}
	}

	line := ""
	for _, l := range strings.Split(text, "\n") {
		idx := strings.Index(l, "PUID1:")
		if idx >= 0 {
			line = strings.TrimSpace(l[idx:])
			break
		}
	}
	if line == "" {
		return CheckReport{}, fmt.Errorf("puid: no PUID1 token found in %q", raw)
	}

	rep := CheckReport{
		Family: "ch32v003",
		Board:  "CH32V003",
	}

	afterPrefix := strings.TrimPrefix(line, "PUID1:")
	// Split tokens across both ':' and whitespace so both
	// `PUID1:OK:ID=0x...:FLASH_KB=16` and `PUID1:OK CHIP_ID=0x... FLASH_KB=16` parse identically.
	tokens := strings.FieldsFunc(afterPrefix, func(r rune) bool {
		return r == ':' || r == ' ' || r == '\t' || r == '\r'
	})
	if len(tokens) == 0 {
		return CheckReport{}, fmt.Errorf("puid: malformed PUID1 line %q", line)
	}

	status := strings.ToUpper(tokens[0])
	kv := make(map[string]string, len(tokens))
	for _, tok := range tokens[1:] {
		if eq := strings.IndexByte(tok, '='); eq > 0 {
			k := strings.ToUpper(strings.TrimSpace(tok[:eq]))
			v := strings.TrimSpace(tok[eq+1:])
			kv[k] = v
		}
	}

	switch status {
	case "OK":
		chipID := kv["ID"]
		if chipID == "" {
			chipID = kv["CHIP_ID"]
		}
		if chipID != "" {
			if val, err := parseHexOrUint32(chipID); err == nil {
				chipID = fmt.Sprintf("0x%08X", val)
			}
		}
		flashKB := 16
		if rawKB, ok := kv["FLASH_KB"]; ok {
			if n, err := strconv.Atoi(rawKB); err == nil && n > 0 {
				flashKB = n
			}
		}
		rep.ChipID = chipID
		rep.FlashKB = flashKB
		rep.Verdict = "OK"
		rep.Ready = true
		rep.Diagnosis = fmt.Sprintf("WCH CH32V003 verified non-destructively over 1-wire SWIO (ID %s, %d KiB Flash)", chipID, flashKB)
		return rep, nil

	case "ERR_NO_REPLY":
		rep.Verdict = "ERR_NO_REPLY"
		rep.Ready = false
		rep.Diagnosis = "Silent or invalid CH32V003 1-wire SWIO bus response (0xFFFFFFFF / 0x00000000 disambiguated)"
		rep.Remediation = []string{
			"1. Target power: verify CH32V003 VDD/3.3 V rail is powered and stable.",
			"2. Common ground: verify GND is connected directly between Pico Universal and CH32V003.",
			"3. 1 kΩ pull-up: ensure a 1 kΩ pull-up resistor is present between SWIO (PD1) and VDD on breadboard / Pico dev wiring.",
			"4. PD1 sketch reuse: if user firmware reconfigured PD1 as GPIO, run 'pu recover --family ch32v003' to power-cycle unbrick.",
			"5. Read protection: if flash read protection (RDPR) is enabled, run 'pu recover --family ch32v003' to unlock and erase.",
		}
		return rep, nil

	case "ERR_ALREADY_HALTED":
		rep.Verdict = "ERR_ALREADY_HALTED"
		rep.Ready = false
		rep.Diagnosis = "CH32V003 CPU is already halted in debug mode; refusing non-destructive check to preserve halted state"
		rep.Remediation = []string{
			"Close any active GDB debug session or resume/reset the target board before running 'pu check'.",
		}
		return rep, nil

	case "ERR_DETACH_FAILED":
		rep.Verdict = "ERR_DETACH_FAILED"
		rep.Ready = false
		rep.Diagnosis = "CH32V003 responded on SWIO, but post-check CPU resume or SHDWCFGR.OUTEN=0 interface reset failed"
		rep.Remediation = []string{
			"Power-cycle the CH32V003 target ('pu recover --family ch32v003') and inspect SWIO wire capacitance and 1 kΩ pull-up.",
		}
		return rep, nil

	case "ERR_ENGINE_BUSY":
		rep.Verdict = "ERR_ENGINE_BUSY"
		rep.Ready = false
		rep.Diagnosis = "Probe engine_owner arbitrator rejected SWIO check because another debug interface is active"
		rep.Remediation = []string{
			"Close any concurrent OpenOCD or GDB session using this probe and retry.",
		}
		return rep, nil

	case "ERR_FAULT_LATCHED":
		rep.Verdict = "ERR_FAULT_LATCHED"
		rep.Ready = false
		rep.Diagnosis = "Probe VTARG overcurrent or watchdog fault is latched in FAULT state"
		rep.Remediation = []string{
			"Disconnect target board, inspect VDD/GND for shorts, and clear the probe fault or replug USB.",
		}
		return rep, nil

	case "ERR_UNEXPECTED_STATE":
		rep.Verdict = "ERR_UNEXPECTED_STATE"
		rep.Ready = false
		rep.Diagnosis = "CH32V003 SWIO bus responded, but chip identity or DMSTATUS state was unexpected"
		rep.Remediation = []string{
			"Inspect SWIO signal integrity, verify 1 kΩ pull-up to VDD, and run 'pu recover --family ch32v003' if locked up.",
		}
		return rep, nil

	default:
		rep.Verdict = status
		rep.Ready = false
		rep.Diagnosis = fmt.Sprintf("CH32V003 PUID1 check reported error state %s", status)
		rep.Remediation = []string{
			"Inspect SWIO wiring and run 'pu recover --family ch32v003' if the target is unresponsive.",
		}
		return rep, nil
	}
}

func parseHexOrUint32(raw string) (uint32, error) {
	s := strings.TrimSpace(raw)
	if strings.HasPrefix(s, "0x") || strings.HasPrefix(s, "0X") {
		v, err := strconv.ParseUint(s[2:], 16, 32)
		return uint32(v), err
	}
	v, err := strconv.ParseUint(s, 16, 32)
	return uint32(v), err
}

var (
	reDPIDR      = regexp.MustCompile(`(?i)(?:SWD\s+DPIDR|tap/device\s+found:|UNEXPECTED:|IDCODE[=:\s]+)\s*0x([0-9a-f]{8})`)
	reMDWLine    = regexp.MustCompile(`(?i)0x([0-9a-f]{8}):\s*([0-9a-f]+(?:\s+[0-9a-f]+)*)`)
	reJEP106Text = regexp.MustCompile(`(?i)(?:jep106|rom_jep106)[=:\s]+0x([0-9a-f]{1,4})`)
	reVTrefMV    = regexp.MustCompile(`(?i)vtref[=:\s]+([0-9]+)\s*mv`)
	reTargetVolt = regexp.MustCompile(`(?i)target\s+voltage:\s*([0-9]+(?:\.[0-9]+)?)`)
)

// ParseOpenOCDCheckOutput extracts SWD/JTAG IDCODE, DBGMCU_IDCODE, flash size,
// CoreSight ROM-table JEP106 identity, and protection/fault flags from OpenOCD output.
func ParseOpenOCDCheckOutput(family, output string) (TargetObservation, error) {
	normFamily, err := NormalizeFamily(family)
	if err != nil {
		return TargetObservation{}, err
	}
	obs := TargetObservation{
		Family:    normFamily,
		RawOutput: output,
	}
	lower := strings.ToLower(output)

	if strings.Contains(lower, "engine_busy") || strings.Contains(lower, "dap_error") && strings.Contains(lower, "busy") {
		obs.EngineBusy = true
	}
	if strings.Contains(lower, "fault_latched") || strings.Contains(lower, "overcurrent") {
		obs.FaultLatched = true
	}
	if strings.Contains(lower, "readout protection") || strings.Contains(lower, "device is protected") || strings.Contains(lower, "rdp level") {
		obs.ReadProtected = true
	}

	if m := reDPIDR.FindStringSubmatch(output); len(m) == 2 {
		if v, parseErr := strconv.ParseUint(m[1], 16, 32); parseErr == nil {
			obs.DPIDCode = uint32(v)
		}
	}
	if m := reJEP106Text.FindStringSubmatch(output); len(m) == 2 {
		if v, parseErr := strconv.ParseUint(m[1], 16, 16); parseErr == nil {
			obs.ROMJEP106 = uint16(v)
		}
	}
	if m := reVTrefMV.FindStringSubmatch(output); len(m) == 2 {
		if mv, parseErr := strconv.Atoi(m[1]); parseErr == nil {
			obs.VTrefMV = mv
		}
	} else if m := reTargetVolt.FindStringSubmatch(output); len(m) == 2 {
		if volts, parseErr := strconv.ParseFloat(m[1], 64); parseErr == nil && volts > 0 {
			obs.VTrefMV = int(volts*1000.0 + 0.5)
		}
	}

	memWords := make(map[uint32]uint32)
	for _, line := range strings.Split(output, "\n") {
		m := reMDWLine.FindStringSubmatch(line)
		if len(m) != 3 {
			continue
		}
		baseAddr64, addrErr := strconv.ParseUint(m[1], 16, 32)
		if addrErr != nil {
			continue
		}
		baseAddr := uint32(baseAddr64)
		fields := strings.Fields(m[2])
		for idx, tok := range fields {
			val64, valErr := strconv.ParseUint(tok, 16, 32)
			if valErr != nil {
				break
			}
			// mdh prints 4 hex chars (2-byte step), mdw prints 8 hex chars (4-byte step).
			step := uint32(4)
			if len(tok) <= 4 {
				step = 2
			}
			memWords[baseAddr+uint32(idx)*step] = uint32(val64)
		}
	}

	if v, ok := memWords[0xE0042000]; ok {
		obs.DBGMCUIDCode = v
	}
	if kb, ok := memWords[0x1FFFF7E0]; ok && kb > 0 && kb < 0xFFFF {
		obs.FlashKB = int(kb & 0xFFFF)
	} else if kb4, ok := memWords[0x1FFF7A22]; ok && kb4 > 0 && kb4 < 0xFFFF {
		obs.FlashKB = int(kb4 & 0xFFFF)
	}

	// CoreSight ROM table PIDR4 (0xE00FFFD0), PIDR1 (0xE00FFFE4), PIDR2 (0xE00FFFE8):
	// JEP106 11-bit ID = ((PIDR4 & 0x0F) << 7) | (((PIDR2 & 0x07) << 4) | ((PIDR1 >> 4) & 0x0F)).
	if obs.ROMJEP106 == 0 {
		pidr4, ok4 := memWords[0xE00FFFD0]
		pidr1, ok1 := memWords[0xE00FFFE4]
		pidr2, ok2 := memWords[0xE00FFFE8]
		if ok4 && ok1 && ok2 && (pidr2&0x08) != 0 {
			cont := uint16(pidr4 & 0x0F)
			id7 := uint16(((pidr2 & 0x07) << 4) | ((pidr1 >> 4) & 0x0F))
			obs.ROMJEP106 = (cont << 7) | id7
		}
	}

	return obs, nil
}

// CheckOptions configures transports for CheckTargetWithOptions so unit tests stay offline.
type CheckOptions struct {
	Runner          CommandRunner
	PortOpener      gdbpipe.PortOpener
	CtrlTransceiver usbprobe.CtrlTransceiver
}

func isHexByte(c byte) bool {
	return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')
}

func exchangeCH32PUID1(port io.ReadWriter) (string, error) {
	deadline := time.Now().Add(2 * time.Second)
	if rd, ok := port.(interface{ SetReadDeadline(time.Time) error }); ok {
		_ = rd.SetReadDeadline(deadline)
	}
	if wd, ok := port.(interface{ SetWriteDeadline(time.Time) error }); ok {
		_ = wd.SetWriteDeadline(deadline)
	}

	req := []byte(BuildGDBQRcmdPacket("puid1"))
	for written := 0; written < len(req); {
		n, err := port.Write(req[written:])
		if n > 0 {
			written += n
		}
		if err != nil {
			return "", fmt.Errorf("puid: write qRcmd puid1 packet: %w", err)
		}
	}

	var rxBuf []byte
	var consoleOut strings.Builder
	readChunk := make([]byte, 256)

	for {
		// Parse all complete RSP frames currently buffered in rxBuf.
		for {
			// Skip leading '+' ACKs and whitespace; fail immediately on '-' NAK before '$'.
			for len(rxBuf) > 0 && rxBuf[0] != '$' {
				if rxBuf[0] == '-' {
					return "", errors.New("puid: probe NAKed qRcmd puid1 packet")
				}
				rxBuf = rxBuf[1:]
			}
			if len(rxBuf) == 0 {
				break
			}
			hashIdx := bytes.IndexByte(rxBuf, '#')
			if hashIdx < 0 || hashIdx+2 >= len(rxBuf) {
				break
			}

			payload := string(rxBuf[1:hashIdx])
			csumHex := string(rxBuf[hashIdx+1 : hashIdx+3])
			rxBuf = rxBuf[hashIdx+3:]

			wantCsum, err := strconv.ParseUint(csumHex, 16, 8)
			if err != nil {
				return "", fmt.Errorf("puid: invalid GDB RSP checksum %q: %w", csumHex, err)
			}
			var gotCsum byte
			for i := 0; i < len(payload); i++ {
				gotCsum += payload[i]
			}
			if gotCsum != byte(wantCsum) {
				return "", fmt.Errorf("puid: GDB RSP checksum mismatch: got %02x, want %02x", gotCsum, byte(wantCsum))
			}

			switch {
			case payload == "OK":
				return consoleOut.String(), nil

			case payload == "":
				return "", errors.New("puid: probe returned empty RSP reply (qRcmd puid1 unsupported)")

			case len(payload) == 3 && (payload[0] == 'E' || payload[0] == 'e') && isHexByte(payload[1]) && isHexByte(payload[2]):
				if strings.Contains(consoleOut.String(), "PUID1:") {
					return consoleOut.String(), nil
				}
				return "", fmt.Errorf("puid: CH32V003 qRcmd puid1 failed with RSP error %q", payload)

			case len(payload) > 1 && payload[0] == 'O' && (len(payload)-1)%2 == 0:
				decoded, decErr := hex.DecodeString(payload[1:])
				if decErr != nil {
					return "", fmt.Errorf("puid: decode RSP $O console payload: %w", decErr)
				}
				consoleOut.Write(decoded)

			case len(payload) >= 2 && len(payload)%2 == 0:
				decoded, decErr := hex.DecodeString(payload)
				if decErr != nil {
					return "", fmt.Errorf("puid: decode RSP hex payload: %w", decErr)
				}
				consoleOut.Write(decoded)
				return consoleOut.String(), nil

			default:
				return "", fmt.Errorf("puid: unexpected GDB RSP reply %q", payload)
			}
		}

		if time.Now().After(deadline) {
			return "", errors.New("puid: timed out waiting for CH32V003 PUID1 RSP reply")
		}
		n, rErr := port.Read(readChunk)
		if n > 0 {
			rxBuf = append(rxBuf, readChunk[:n]...)
		}
		if rErr != nil {
			if errors.Is(rErr, io.EOF) && consoleOut.Len() > 0 {
				return consoleOut.String(), nil
			}
			return "", fmt.Errorf("puid: read CH32V003 PUID1 RSP reply: %w", rErr)
		}
	}
}

func isCtrlReplyStatus(err error, status byte) bool {
	var stErr *usbprobe.CtrlReplyError
	return errors.As(err, &stErr) && stErr.Status == status
}

// CheckTarget acquires the cross-process probe session lock for probe.Serial, runs a
// non-destructive target identity check via OpenOCD (SWD/JTAG) or raw CDC qRcmd puid1
// (CH32V003), and returns a structured CheckReport.
func CheckTarget(probe usbprobe.ProbeDevice, family string, runner CommandRunner) (CheckReport, error) {
	return CheckTargetWithOptions(probe, family, CheckOptions{
		Runner: runner,
	})
}

// CheckTargetWithOptions acquires the cross-process probe session lock, queries IF 5
// telemetry/engine health before touching target wires, and executes the target check.
func CheckTargetWithOptions(probe usbprobe.ProbeDevice, family string, opts CheckOptions) (CheckReport, error) {
	normFamily, err := NormalizeFamily(family)
	if err != nil {
		return CheckReport{}, err
	}
	if strings.TrimSpace(probe.Serial) == "" {
		return CheckReport{}, errors.New("puid: probe serial must not be empty")
	}
	runner := opts.Runner
	if runner == nil {
		runner = OSCommandRunner{}
	}
	portOpener := opts.PortOpener
	if portOpener == nil {
		portOpener = gdbpipe.OpenSerialPort
	}
	ctrlTx := opts.CtrlTransceiver
	if ctrlTx == nil {
		ctrlTx = usbprobe.DefaultCtrlTransceiver
	}

	lk, err := usbprobe.AcquireSessionLock(probe.Serial)
	if err != nil {
		return CheckReport{}, err
	}
	defer lk.Close()

	var liveVTrefMV, liveITargMA int
	if strings.TrimSpace(probe.CtrlPath) != "" {
		info, infoErr := usbprobe.QueryInfo(probe, ctrlTx)
		telem, telemErr := usbprobe.QueryTelemetry(probe, ctrlTx)
		if telemErr == nil && telem.HasADCTelemetry {
			liveVTrefMV = int(telem.VTrefMV)
			liveITargMA = int(telem.ITargMA)
		}

		faultLatched := (infoErr == nil && (info.EngineState == usbprobe.EngineFault || info.FaultMask != 0)) ||
			(telemErr == nil && telem.Overcurrent) ||
			isCtrlReplyStatus(infoErr, usbprobe.CtrlStatusFaultLatched) ||
			isCtrlReplyStatus(telemErr, usbprobe.CtrlStatusFaultLatched)
		if faultLatched {
			rep, clsErr := ClassifyTarget(TargetObservation{
				Family:       normFamily,
				VTrefMV:      liveVTrefMV,
				ITargMA:      liveITargMA,
				FaultLatched: true,
			})
			if clsErr != nil {
				return CheckReport{}, clsErr
			}
			rep.ProbeSerial = probe.Serial
			return rep, nil
		}

		engineBusy := (infoErr == nil && info.EngineState != usbprobe.EngineIdle) ||
			isCtrlReplyStatus(infoErr, usbprobe.CtrlStatusBusy) ||
			isCtrlReplyStatus(telemErr, usbprobe.CtrlStatusBusy)
		if engineBusy {
			rep, clsErr := ClassifyTarget(TargetObservation{
				Family:     normFamily,
				VTrefMV:    liveVTrefMV,
				ITargMA:    liveITargMA,
				EngineBusy: true,
			})
			if clsErr != nil {
				return CheckReport{}, clsErr
			}
			rep.ProbeSerial = probe.Serial
			return rep, nil
		}
	}

	if normFamily == "ch32v003" {
		if strings.TrimSpace(probe.GDBPort) == "" {
			return CheckReport{}, fmt.Errorf("puid: probe %s has no CH32 SWIO GDB CDC port", probe.Serial)
		}
		port, openErr := portOpener(probe.GDBPort)
		if openErr != nil {
			return CheckReport{}, fmt.Errorf("puid: open CH32 SWIO CDC port %s: %w", probe.GDBPort, openErr)
		}
		decoded, exErr := exchangeCH32PUID1(port)
		_ = port.Close()
		if exErr != nil {
			return CheckReport{}, exErr
		}

		rep, parseErr := ParsePUID1Response(decoded)
		if parseErr != nil {
			return CheckReport{}, parseErr
		}
		rep.ProbeSerial = probe.Serial
		if liveVTrefMV > 0 {
			rep.VTrefMV = liveVTrefMV
		}
		if liveITargMA > 0 {
			rep.ITargMA = liveITargMA
		}
		if rep.Ready && rep.VTrefMV > 0 && (rep.VTrefMV < minValidVTrefMV || rep.VTrefMV > maxValidVTrefMV) {
			rep.Ready = false
			rep.Verdict = "ERR_VTREF_OUT_OF_RANGE"
			rep.Diagnosis = fmt.Sprintf("Measured target VTREF (%d mV) is outside safe 1.8 V - 5.0 V operating range", rep.VTrefMV)
			rep.Remediation = []string{
				"Verify target board 3.3 V / VCC power supply is turned on and connected to VTREF.",
				"Ensure target logic voltage is between 1.8 V (1800 mV) and 5.0 V (5000 mV).",
			}
		}
		return rep, nil
	}

	ocdBin, err := runner.LookPath(catalog.OpenOCDLookupNameForFamily(normFamily))
	if err != nil {
		ocdBin, err = runner.LookPath("openocd")
		if err != nil {
			return CheckReport{}, fmt.Errorf("puid: openocd not found in PATH: %w", err)
		}
	}

	var args []string
	switch normFamily {
	case "esp32":
		tmpDir, mkErr := os.MkdirTemp("", "pu-check-esp32-*")
		if mkErr != nil {
			return CheckReport{}, fmt.Errorf("puid: create temp dir: %w", mkErr)
		}
		_ = os.Chmod(tmpDir, 0o700)
		defer os.RemoveAll(tmpDir)

		cfgPath := filepath.Join(tmpDir, "pu-esp32.cfg")
		if writeErr := os.WriteFile(cfgPath, catalog.EmbeddedESP32OpenOCDConfig(), 0o600); writeErr != nil {
			return CheckReport{}, fmt.Errorf("puid: write staged pu-esp32.cfg: %w", writeErr)
		}
		args = []string{
			"-c", "adapter serial " + probe.Serial,
			"-f", filepath.ToSlash(cfgPath),
			"-c", "init; resume; exit",
		}

	case "esp32s3":
		args = []string{
			"-f", "interface/cmsis-dap.cfg",
			"-c", "adapter serial " + probe.Serial,
			"-c", "transport select jtag",
			"-f", "target/esp32s3.cfg",
			"-c", "adapter speed 5000",
			"-c", "init; exit",
		}

	case "rp2040", "rp2350":
		targetCfg := "target/rp2040.cfg"
		if normFamily == "rp2350" {
			targetCfg = "target/rp2350.cfg"
		}
		args = []string{
			"-f", "interface/cmsis-dap.cfg",
			"-c", "adapter serial " + probe.Serial,
			"-c", "transport select swd",
			"-c", "adapter speed 100",
			"-f", targetCfg,
			"-c", "init; exit",
		}

	case "stm32f1":
		args = []string{
			"-f", "interface/cmsis-dap.cfg",
			"-c", "adapter serial " + probe.Serial,
			"-c", "transport select swd",
			"-c", "set CPUTAPID 0",
			"-f", "target/stm32f1x.cfg",
			"-c", "adapter speed 100; init; echo [mdw 0xE0042000 1]; echo [mdh 0x1FFFF7E0 1]; echo [mdw 0xE00FFFD0 8]; exit",
		}

	case "stm32f4":
		args = []string{
			"-f", "interface/cmsis-dap.cfg",
			"-c", "adapter serial " + probe.Serial,
			"-c", "transport select swd",
			"-f", "target/stm32f4x.cfg",
			"-c", "adapter speed 100; init; echo [mdw 0xE0042000 1]; echo [mdh 0x1FFF7A22 1]; echo [mdw 0xE00FFFD0 8]; exit",
		}
	}

	var stdout, stderr bytes.Buffer
	runErr := runner.Run(CommandSpec{Name: ocdBin, Args: args}, &stdout, &stderr)
	combined := stdout.String() + "\n" + stderr.String()
	if runErr != nil && strings.TrimSpace(combined) == "" {
		return CheckReport{}, fmt.Errorf("puid: openocd check failed: %w", runErr)
	}

	obs, err := ParseOpenOCDCheckOutput(normFamily, combined)
	if err != nil {
		return CheckReport{}, err
	}
	if obs.VTrefMV == 0 && liveVTrefMV > 0 {
		obs.VTrefMV = liveVTrefMV
	}
	if liveITargMA > 0 {
		obs.ITargMA = liveITargMA
	}
	rep, err := ClassifyTarget(obs)
	if err != nil {
		return CheckReport{}, err
	}
	rep.ProbeSerial = probe.Serial
	return rep, nil
}
