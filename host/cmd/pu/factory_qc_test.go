package main

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/nfsahsn/pico-universal-programmer/host/internal/usbprobe"
)

func TestFactoryQCGatesRejectFaultyUnits(t *testing.T) {
	good := QCReading{
		VRefMeasuredMV:     3310,
		VRefExpectedMV:     3300,
		ITargMeasuredMA:    102,
		ITargExpectedMA:    100,
		LoopbackMask:       0x3F,
		OvercurrentTripped: true,
		SelfTestMask:       usbprobe.SelfTestAllOK,
	}
	if err := EvaluateQCUnit(good); err != nil {
		t.Fatalf("expected good unit to pass, got %v", err)
	}

	badADC := good
	badADC.VRefMeasuredMV = 3500 // >3% error
	if err := EvaluateQCUnit(badADC); err == nil {
		t.Fatal("expected >3% ADC VTREF error to fail QC")
	}

	badITarg := good
	badITarg.ITargMeasuredMA = 120 // >3% error
	if err := EvaluateQCUnit(badITarg); err == nil {
		t.Fatal("expected >3% ADC I_TARG error to fail QC")
	}

	badLoopback := good
	badLoopback.LoopbackMask = 0x3E // missing 1 bit
	if err := EvaluateQCUnit(badLoopback); err == nil {
		t.Fatal("expected failed loopback bit to fail QC")
	}

	badOC := good
	badOC.OvercurrentTripped = false
	if err := EvaluateQCUnit(badOC); err == nil {
		t.Fatal("expected missing overcurrent trip to fail QC")
	}

	badSelfTest := good
	badSelfTest.SelfTestMask = usbprobe.SelfTestEngineOK // missing journal/ADC/no-fault bits
	if err := EvaluateQCUnit(badSelfTest); err == nil {
		t.Fatal("expected incomplete SELF_TEST mask to fail QC")
	}
}

func TestFactoryQCCLIWritesCertificateAndCSV(t *testing.T) {
	t.Setenv("PU_LOCK_DIR", filepath.Join(t.TempDir(), "locks"))

	outDir := t.TempDir()
	certPath := filepath.Join(outDir, "cert.json")
	csvPath := filepath.Join(outDir, "qc_log.csv")

	telemCalls := 0
	mockCtrl := func(_ usbprobe.ProbeDevice, req []byte) ([]byte, error) {
		switch req[0] {
		case usbprobe.CtrlCmdGetInfo:
			return []byte{0x01, 0x00, 1, 0, 0, 1, 0x07, 0, 0, 0, 0, 0, 0}, nil
		case usbprobe.CtrlCmdSelfTest:
			return []byte{0x07, 0x00, 0x0F, 0x00, 0x00, 0x00}, nil
		case usbprobe.CtrlCmdSetVTarg:
			return []byte{0x03, 0x00, req[1]}, nil
		case usbprobe.CtrlCmdGetTelemetry:
			telemCalls++
			// First call: VTREF=3310mV (0x0CEE), ITARG=100mA (0x0064), OC=0
			// Second call: OC=1 (jig short asserted)
			oc := byte(0)
			if telemCalls >= 2 {
				oc = 1
			}
			return []byte{0x02, 0x00, 0xEE, 0x0C, 0x64, 0x00, 1, oc, 1, 0, 0, 0, 0}, nil
		case usbprobe.CtrlCmdClearFault:
			return []byte{0x04, 0x00}, nil
		default:
			return []byte{req[0], usbprobe.CtrlStatusUnsupported}, nil
		}
	}

	var out, errBuf bytes.Buffer
	code := runWithEnv([]string{
		"factory-qc",
		"--expected-vref-mv", "3300",
		"--expected-itarg-ma", "100",
		"--loopback-mask", "0x3F",
		"--cert-out", certPath,
		"--csv-log", csvPath,
		"--json",
	}, cliEnv{
		stdout:          &out,
		stderr:          &errBuf,
		discoverProbes:  singleFakeProbe(),
		ctrlTransceiver: mockCtrl,
	})
	if code != 0 {
		t.Fatalf("pu factory-qc failed: code=%d stderr=%s", code, errBuf.String())
	}

	var cert QCCertificate
	if err := json.Unmarshal(out.Bytes(), &cert); err != nil {
		t.Fatalf("invalid QC certificate JSON: %v\n%s", err, out.String())
	}
	if !cert.Passed || cert.UnitSerial != "E660583883278628" || len(cert.SHA256Digest) != 64 {
		t.Fatalf("unexpected QC certificate: %+v", cert)
	}

	certDisk, err := os.ReadFile(certPath)
	if err != nil || !strings.Contains(string(certDisk), cert.SHA256Digest) {
		t.Fatalf("expected persisted cert at %s, err=%v", certPath, err)
	}
	csvDisk, err := os.ReadFile(csvPath)
	if err != nil || !strings.Contains(string(csvDisk), "E660583883278628") || !strings.Contains(string(csvDisk), "true") {
		t.Fatalf("expected persisted CSV log at %s, err=%v content=%s", csvPath, err, string(csvDisk))
	}
}
