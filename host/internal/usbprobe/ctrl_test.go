package usbprobe

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestQueryInfoDecodes13ByteFirmwareResponseAndRejectsMalformed(t *testing.T) {
	probe := ProbeDevice{
		Serial:   "E660CTRLINFO0001",
		VID:      0x2E8A,
		PID:      0x1050,
		CtrlPath: "/dev/bus/usb/001/010",
	}

	var gotReq []byte
	tx := func(_ ProbeDevice, req []byte) ([]byte, error) {
		gotReq = append([]byte(nil), req...)
		// Matches firmware/control_telemetry.c PU_CTRL_CMD_GET_INFO 13-byte layout:
		// [cmd=0x01, status=0x00, maj=1, min=0, pat=0, pro=1, caps=0x07, engine=3(SWIO), slot=1, fault_le=0x80000005]
		return []byte{
			0x01, 0x00,
			1, 0, 0,
			1,
			CapLevelShifter | CapVTargSwitch | CapADCTelemetry,
			EngineSWIO,
			1,
			0x05, 0x00, 0x00, 0x80,
		}, nil
	}

	info, err := QueryInfo(probe, tx)
	if err != nil {
		t.Fatalf("QueryInfo failed: %v", err)
	}
	if !bytes.Equal(gotReq, []byte{CtrlCmdGetInfo}) {
		t.Fatalf("unexpected GET_INFO request: %x", gotReq)
	}
	if info.FWVersionMajor != 1 || info.FWVersionMinor != 0 || info.FWVersionPatch != 0 {
		t.Fatalf("unexpected firmware version: %+v", info)
	}
	if !info.BoardIsProPCB || info.Capabilities != 0x07 || info.EngineState != EngineSWIO || info.ActiveSlot != 1 || info.FaultMask != 0x80000005 {
		t.Fatalf("unexpected CtrlInfo decoded fields: %+v", info)
	}

	// Non-OK status reply returns typed CtrlReplyError.
	_, err = QueryInfo(probe, func(ProbeDevice, []byte) ([]byte, error) {
		return []byte{CtrlCmdGetInfo, CtrlStatusFaultLatched}, nil
	})
	var stErr *CtrlReplyError
	if !errors.As(err, &stErr) || stErr.Status != CtrlStatusFaultLatched {
		t.Fatalf("expected CtrlReplyError(FaultLatched), got %v", err)
	}

	// Truncated 12-byte OK reply must fail.
	if _, err := QueryInfo(probe, func(ProbeDevice, []byte) ([]byte, error) {
		return make([]byte, 12), nil
	}); err == nil {
		t.Fatal("expected error on truncated GET_INFO response")
	}

	// Mismatched command echo must fail.
	if _, err := QueryInfo(probe, func(ProbeDevice, []byte) ([]byte, error) {
		b := make([]byte, 13)
		b[0] = CtrlCmdGetTelemetry
		return b, nil
	}); err == nil {
		t.Fatal("expected error on mismatched command echo")
	}
}

func TestQueryTelemetryDecodes13ByteFirmwareResponse(t *testing.T) {
	probe := ProbeDevice{
		Serial:   "E660CTRLTELEM002",
		VID:      0x2E8A,
		PID:      0x1050,
		CtrlPath: "/dev/bus/usb/001/011",
	}

	var gotReq []byte
	tx := func(_ ProbeDevice, req []byte) ([]byte, error) {
		gotReq = append([]byte(nil), req...)
		// Matches firmware/control_telemetry.c PU_CTRL_CMD_GET_TELEMETRY 13-byte layout:
		// [cmd=0x02, status=0x00, vtref_le=3312(0x0CF0), itarg_le=47(0x002F), vtarg_en=1, oc=0, has_adc=1, drops_le=0x01020304]
		return []byte{
			0x02, 0x00,
			0xF0, 0x0C,
			0x2F, 0x00,
			1,
			0,
			1,
			0x04, 0x03, 0x02, 0x01,
		}, nil
	}

	telem, err := QueryTelemetry(probe, tx)
	if err != nil {
		t.Fatalf("QueryTelemetry failed: %v", err)
	}
	if !bytes.Equal(gotReq, []byte{CtrlCmdGetTelemetry}) {
		t.Fatalf("unexpected GET_TELEMETRY request: %x", gotReq)
	}
	if telem.VTrefMV != 3312 || telem.ITargMA != 47 || !telem.VTargEnabled || telem.Overcurrent || !telem.HasADCTelemetry || telem.UARTRXDropCount != 0x01020304 {
		t.Fatalf("unexpected CtrlTelemetry decoded fields: %+v", telem)
	}

	// Truncated response must fail.
	if _, err := QueryTelemetry(probe, func(ProbeDevice, []byte) ([]byte, error) {
		return []byte{0x02, 0x00, 0xF0, 0x0C}, nil
	}); err == nil {
		t.Fatal("expected error on truncated GET_TELEMETRY response")
	}
}

func TestRequestCH32UnbrickStatusesAndValidation(t *testing.T) {
	probe := ProbeDevice{
		Serial:   "E660CTRLUNBRK003",
		VID:      0x2E8A,
		PID:      0x1050,
		CtrlPath: "/dev/bus/usb/001/012",
	}

	for _, wantStatus := range []byte{CtrlStatusOK, CtrlStatusUnsupported, CtrlStatusBusy, CtrlStatusFaultLatched} {
		var gotReq []byte
		st, err := RequestCH32Unbrick(probe, func(_ ProbeDevice, req []byte) ([]byte, error) {
			gotReq = append([]byte(nil), req...)
			return []byte{CtrlCmdCH32Unbrick, wantStatus}, nil
		})
		if err != nil {
			t.Fatalf("RequestCH32Unbrick(status=0x%02x) unexpected error: %v", wantStatus, err)
		}
		if !bytes.Equal(gotReq, []byte{CtrlCmdCH32Unbrick}) || st != wantStatus {
			t.Fatalf("got req=%x status=0x%02x, want status=0x%02x", gotReq, st, wantStatus)
		}
	}

	// Malformed 1-byte or wrong command ID must fail.
	if _, err := RequestCH32Unbrick(probe, func(ProbeDevice, []byte) ([]byte, error) {
		return []byte{CtrlCmdCH32Unbrick}, nil
	}); err == nil {
		t.Fatal("expected error on 1-byte CH32_UNBRICK response")
	}
	if _, err := RequestCH32Unbrick(probe, func(ProbeDevice, []byte) ([]byte, error) {
		return []byte{CtrlCmdGetInfo, CtrlStatusOK}, nil
	}); err == nil {
		t.Fatal("expected error on mismatched command byte")
	}
}

func TestDefaultCtrlTransceiverValidationAndOSDeviceHandling(t *testing.T) {
	// Empty CtrlPath must fail cleanly.
	if _, err := DefaultCtrlTransceiver(ProbeDevice{Serial: "SN1"}, []byte{CtrlCmdGetInfo}); err == nil {
		t.Fatal("expected error for empty CtrlPath")
	}
	// Empty or >64B request must fail cleanly.
	if _, err := DefaultCtrlTransceiver(ProbeDevice{Serial: "SN1", CtrlPath: "dev"}, nil); err == nil {
		t.Fatal("expected error for empty request")
	}
	if _, err := DefaultCtrlTransceiver(ProbeDevice{Serial: "SN1", CtrlPath: "dev"}, make([]byte, 65)); err == nil {
		t.Fatal("expected error for >64B bulk request")
	}

	// Non-USB regular file path (including optional :1.5 interface suffix) must fail cleanly without panic.
	fakeDev := filepath.Join(t.TempDir(), "fake_usb_node")
	if err := os.WriteFile(fakeDev, []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := DefaultCtrlTransceiver(ProbeDevice{Serial: "SN1", CtrlPath: fakeDev + ":1.5"}, []byte{CtrlCmdGetInfo}); err == nil {
		t.Fatal("expected OS USB bulk ioctl/WinUSB open on regular file to fail cleanly")
	}
}
