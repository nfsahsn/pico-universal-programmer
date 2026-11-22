package main

import (
	"crypto/sha256"
	"encoding/csv"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/nfsahsn/pico-universal-programmer/host/internal/usbprobe"
)

const (
	qcExpectedLoopbackMask uint8   = 0x3F // SWCLK, SWDIO, TDI, TDO, NRST/SWIO, UART
	qcMaxADCRelError       float64 = 0.03 // +/- 3% factory calibration tolerance
)

// QCReading captures all electrical and self-test measurements for one production unit.
type QCReading struct {
	Serial             string `json:"serial,omitempty"`
	VID                uint16 `json:"vid,omitempty"`
	PID                uint16 `json:"pid,omitempty"`
	FWVersion          string `json:"fw_version,omitempty"`
	BoardIsProPCB      bool   `json:"board_is_pro_pcb,omitempty"`
	SelfTestMask       uint32 `json:"self_test_mask,omitempty"`
	VRefMeasuredMV     int    `json:"vref_measured_mv"`
	VRefExpectedMV     int    `json:"vref_expected_mv"`
	ITargMeasuredMA    int    `json:"itarg_measured_ma,omitempty"`
	ITargExpectedMA    int    `json:"itarg_expected_ma,omitempty"`
	LoopbackMask       uint8  `json:"loopback_mask"`
	OvercurrentTripped bool   `json:"overcurrent_tripped"`
}

// QCCertificate is the tamper-evident manufacturing record emitted per serial number.
type QCCertificate struct {
	SchemaVersion string    `json:"schema_version"`
	TimestampUTC  string    `json:"timestamp_utc"`
	UnitSerial    string    `json:"unit_serial"`
	Passed        bool      `json:"passed"`
	FailureReason string    `json:"failure_reason,omitempty"`
	Reading       QCReading `json:"reading"`
	SHA256Digest  string    `json:"sha256_digest"`
}

// EvaluateQCUnit enforces production pass/fail gates:
//   - ADC VTREF (and optional I_TARG) accuracy within +/- 3%
//   - Full 6-channel level-shifter loopback continuity (0x3F)
//   - Hardware VTARG overcurrent trip verification
//   - All 4 firmware self-test bits when SelfTestMask is populated
func EvaluateQCUnit(r QCReading) error {
	if r.VRefExpectedMV <= 0 {
		return fmt.Errorf("factory-qc: expected VTREF must be positive (got %d mV)", r.VRefExpectedMV)
	}
	vrefErr := math.Abs(float64(r.VRefMeasuredMV-r.VRefExpectedMV)) / float64(r.VRefExpectedMV)
	if vrefErr > qcMaxADCRelError {
		return fmt.Errorf(
			"factory-qc: ADC VTREF error %.2f%% exceeds +/-3.00%% limit (measured %d mV vs expected %d mV)",
			vrefErr*100.0, r.VRefMeasuredMV, r.VRefExpectedMV,
		)
	}

	if r.ITargExpectedMA > 0 {
		itargErr := math.Abs(float64(r.ITargMeasuredMA-r.ITargExpectedMA)) / float64(r.ITargExpectedMA)
		if itargErr > qcMaxADCRelError {
			return fmt.Errorf(
				"factory-qc: ADC I_TARG error %.2f%% exceeds +/-3.00%% limit (measured %d mA vs expected %d mA)",
				itargErr*100.0, r.ITargMeasuredMA, r.ITargExpectedMA,
			)
		}
	}

	if r.LoopbackMask != qcExpectedLoopbackMask {
		return fmt.Errorf(
			"factory-qc: target level-shifter loopback failed: got mask 0x%02X, expected 0x%02X",
			r.LoopbackMask, qcExpectedLoopbackMask,
		)
	}

	if !r.OvercurrentTripped {
		return errors.New("factory-qc: VTARG load-switch overcurrent trip did not assert")
	}

	if r.SelfTestMask != 0 && (r.SelfTestMask&usbprobe.SelfTestAllOK) != usbprobe.SelfTestAllOK {
		return fmt.Errorf(
			"factory-qc: firmware SELF_TEST failed: mask 0x%02X (expected all bits 0x%02X)",
			r.SelfTestMask, usbprobe.SelfTestAllOK,
		)
	}

	return nil
}

func buildQCCertificate(serial string, reading QCReading, evalErr error) QCCertificate {
	cert := QCCertificate{
		SchemaVersion: "1.0.0",
		TimestampUTC:  time.Now().UTC().Format(time.RFC3339),
		UnitSerial:    serial,
		Passed:        evalErr == nil,
		Reading:       reading,
	}
	if evalErr != nil {
		cert.FailureReason = evalErr.Error()
	}
	canonical, _ := json.Marshal(struct {
		SchemaVersion string    `json:"schema_version"`
		TimestampUTC  string    `json:"timestamp_utc"`
		UnitSerial    string    `json:"unit_serial"`
		Passed        bool      `json:"passed"`
		FailureReason string    `json:"failure_reason,omitempty"`
		Reading       QCReading `json:"reading"`
	}{
		SchemaVersion: cert.SchemaVersion,
		TimestampUTC:  cert.TimestampUTC,
		UnitSerial:    cert.UnitSerial,
		Passed:        cert.Passed,
		FailureReason: cert.FailureReason,
		Reading:       cert.Reading,
	})
	sum := sha256.Sum256(canonical)
	cert.SHA256Digest = hex.EncodeToString(sum[:])
	return cert
}

func appendQCCSVRow(csvPath string, cert QCCertificate) error {
	if err := os.MkdirAll(filepath.Dir(csvPath), 0o755); err != nil {
		return err
	}
	needHeader := false
	if st, err := os.Stat(csvPath); errors.Is(err, os.ErrNotExist) || (err == nil && st.Size() == 0) {
		needHeader = true
	}
	f, err := os.OpenFile(csvPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return err
	}
	defer f.Close()

	w := csv.NewWriter(f)
	if needHeader {
		if err := w.Write([]string{
			"timestamp_utc", "unit_serial", "vid", "pid", "fw_version",
			"vref_expected_mv", "vref_measured_mv", "itarg_expected_ma", "itarg_measured_ma",
			"loopback_mask", "overcurrent_tripped", "self_test_mask", "passed", "failure_reason", "sha256_digest",
		}); err != nil {
			return err
		}
	}
	r := cert.Reading
	if err := w.Write([]string{
		cert.TimestampUTC,
		cert.UnitSerial,
		fmt.Sprintf("0x%04X", r.VID),
		fmt.Sprintf("0x%04X", r.PID),
		r.FWVersion,
		strconv.Itoa(r.VRefExpectedMV),
		strconv.Itoa(r.VRefMeasuredMV),
		strconv.Itoa(r.ITargExpectedMA),
		strconv.Itoa(r.ITargMeasuredMA),
		fmt.Sprintf("0x%02X", r.LoopbackMask),
		strconv.FormatBool(r.OvercurrentTripped),
		fmt.Sprintf("0x%02X", r.SelfTestMask),
		strconv.FormatBool(cert.Passed),
		cert.FailureReason,
		cert.SHA256Digest,
	}); err != nil {
		return err
	}
	w.Flush()
	return w.Error()
}

func runFactoryQC(ctx cliContext, args []string) int {
	fs := flag.NewFlagSet("pu factory-qc", flag.ContinueOnError)
	fs.SetOutput(ctx.stderr)

	serialFlag := fs.String("serial", "", "Select specific unit serial number")
	pidFlag := fs.String("pid", "", "Approved USB PID in hex (overrides PU_USB_PID)")
	expectedVRef := fs.Int("expected-vref-mv", 3300, "Jig reference VTREF voltage in mV (1800, 3300, or 5000)")
	expectedITarg := fs.Int("expected-itarg-ma", 0, "Optional jig reference VTARG load current in mA")
	loopbackMaskStr := fs.String("loopback-mask", "0x3F", "6-channel loopback bitmask observed by test fixture")
	certOutFlag := fs.String("cert-out", "", "Optional output path for unit JSON QC certificate")
	csvLogFlag := fs.String("csv-log", "", "Optional manufacturing CSV log path to append result")
	jsonFlag := fs.Bool("json", false, "Emit QC certificate as JSON to stdout")

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}

	loopbackRaw := strings.TrimSpace(*loopbackMaskStr)
	loopbackVal, err := strconv.ParseUint(strings.TrimPrefix(strings.TrimPrefix(loopbackRaw, "0x"), "0X"), 16, 8)
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu factory-qc: invalid --loopback-mask %q: %v\n", *loopbackMaskStr, err)
		return 2
	}

	probe, err := resolveSingleProbe(ctx, *serialFlag, *pidFlag)
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu factory-qc: %v\n", err)
		return 1
	}

	lk, err := usbprobe.AcquireSessionLock(probe.Serial)
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu factory-qc: %v\n", err)
		return 1
	}
	defer lk.Close()

	info, err := usbprobe.QueryInfo(probe, ctx.ctrlTransceiver)
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu factory-qc: IF 5 GET_INFO failed: %v\n", err)
		return 1
	}
	selfTestMask, err := usbprobe.RunSelfTest(probe, ctx.ctrlTransceiver)
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu factory-qc: IF 5 SELF_TEST failed: %v\n", err)
		return 1
	}

	// Enable VTARG rail and query ADC VTREF / I_TARG telemetry.
	setSt, setErr := usbprobe.SetVTarg(probe, true, ctx.ctrlTransceiver)
	if setErr != nil {
		fmt.Fprintf(ctx.stderr, "pu factory-qc: IF 5 SET_VTARG(on) failed: %v\n", setErr)
		return 1
	}
	telem, err := usbprobe.QueryTelemetry(probe, ctx.ctrlTransceiver)
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu factory-qc: IF 5 GET_TELEMETRY failed: %v\n", err)
		return 1
	}

	// Verify overcurrent trip latch (either observed during SET_VTARG short pulse or live telemetry).
	ocTripped := setSt == usbprobe.CtrlStatusFaultLatched || telem.Overcurrent
	if !ocTripped {
		// Second probe after jig short assertion: re-read telemetry once.
		if postTelem, pErr := usbprobe.QueryTelemetry(probe, ctx.ctrlTransceiver); pErr == nil {
			ocTripped = postTelem.Overcurrent
		}
	}

	// Ensure VTARG is disabled cleanly and any latched fault is cleared after test.
	_, _ = usbprobe.SetVTarg(probe, false, ctx.ctrlTransceiver)
	if ocTripped {
		_, _ = usbprobe.ClearFault(probe, ctx.ctrlTransceiver)
	}

	reading := QCReading{
		Serial:             probe.Serial,
		VID:                probe.VID,
		PID:                probe.PID,
		FWVersion:          fmt.Sprintf("%d.%d.%d", info.FWVersionMajor, info.FWVersionMinor, info.FWVersionPatch),
		BoardIsProPCB:      info.BoardIsProPCB,
		SelfTestMask:       selfTestMask,
		VRefMeasuredMV:     int(telem.VTrefMV),
		VRefExpectedMV:     *expectedVRef,
		ITargMeasuredMA:    int(telem.ITargMA),
		ITargExpectedMA:    *expectedITarg,
		LoopbackMask:       uint8(loopbackVal),
		OvercurrentTripped: ocTripped,
	}

	evalErr := EvaluateQCUnit(reading)
	cert := buildQCCertificate(probe.Serial, reading, evalErr)

	encoded, mErr := json.MarshalIndent(cert, "", "  ")
	if mErr != nil {
		fmt.Fprintf(ctx.stderr, "pu factory-qc: marshal certificate: %v\n", mErr)
		return 1
	}
	encoded = append(encoded, '\n')

	if strings.TrimSpace(*certOutFlag) != "" {
		if mkErr := os.MkdirAll(filepath.Dir(*certOutFlag), 0o755); mkErr != nil {
			fmt.Fprintf(ctx.stderr, "pu factory-qc: create cert dir: %v\n", mkErr)
			return 1
		}
		if wErr := atomicWriteFile(*certOutFlag, encoded, 0o644); wErr != nil {
			fmt.Fprintf(ctx.stderr, "pu factory-qc: write certificate %s: %v\n", *certOutFlag, wErr)
			return 1
		}
	}
	if strings.TrimSpace(*csvLogFlag) != "" {
		if cErr := appendQCCSVRow(*csvLogFlag, cert); cErr != nil {
			fmt.Fprintf(ctx.stderr, "pu factory-qc: append CSV log %s: %v\n", *csvLogFlag, cErr)
			return 1
		}
	}

	if *jsonFlag {
		_, _ = ctx.stdout.Write(encoded)
	} else {
		verdict := "PASS"
		if !cert.Passed {
			verdict = "FAIL"
		}
		fmt.Fprintf(ctx.stdout, "Factory QC [%s]: %s (VTREF=%dmV/%dmV, I_TARG=%dmA, loopback=0x%02X, OC_trip=%v, sha256=%s)\n",
			cert.UnitSerial, verdict, reading.VRefMeasuredMV, reading.VRefExpectedMV,
			reading.ITargMeasuredMA, reading.LoopbackMask, reading.OvercurrentTripped, cert.SHA256Digest[:16])
		if evalErr != nil {
			fmt.Fprintf(ctx.stderr, "%v\n", evalErr)
		}
	}

	if !cert.Passed {
		return 1
	}
	return 0
}
