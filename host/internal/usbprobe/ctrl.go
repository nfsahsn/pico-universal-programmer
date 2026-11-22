package usbprobe

import (
	"encoding/binary"
	"errors"
	"fmt"
	"strings"
)

const (
	// IF 5 vendor bulk control/telemetry USB descriptors matching firmware/usb_descriptors.h.
	CtrlInterfaceNum = 5
	CtrlEndpointOut  = 0x06
	CtrlEndpointIn   = 0x86
	CtrlPacketSize   = 64
	CtrlTimeoutMS    = 2000

	// IF 5 command opcodes matching firmware/control_telemetry.h.
	CtrlCmdGetInfo      byte = 0x01
	CtrlCmdGetTelemetry byte = 0x02
	CtrlCmdSetVTarg     byte = 0x03
	CtrlCmdClearFault   byte = 0x04
	CtrlCmdCH32Unbrick  byte = 0x05
	CtrlCmdEnterBootsel byte = 0x06
	CtrlCmdSelfTest     byte = 0x07

	// IF 5 status codes matching firmware/control_telemetry.h.
	CtrlStatusOK           byte = 0x00
	CtrlStatusInvalidArg   byte = 0x01
	CtrlStatusUnsupported  byte = 0x02
	CtrlStatusBusy         byte = 0x03
	CtrlStatusFaultLatched byte = 0x04
	CtrlStatusError        byte = 0x05

	// Hardware capability bitmask matching PU_CAP_* in firmware/control_telemetry.h.
	CapLevelShifter uint8 = 1 << 0
	CapVTargSwitch  uint8 = 1 << 1
	CapADCTelemetry uint8 = 1 << 2

	// Self-test bitmask matching PU_SELFTEST_* in firmware/control_telemetry.h.
	SelfTestEngineOK  uint32 = 1 << 0
	SelfTestJournalOK uint32 = 1 << 1
	SelfTestADCRailOK uint32 = 1 << 2
	SelfTestNoFaultOK uint32 = 1 << 3
	SelfTestAllOK     uint32 = SelfTestEngineOK | SelfTestJournalOK | SelfTestADCRailOK | SelfTestNoFaultOK

	// Engine arbitrator states matching pu_engine_state_t in firmware/engine_owner.h.
	EngineIdle     uint8 = 0
	EngineSWD      uint8 = 1
	EngineJTAG     uint8 = 2
	EngineSWIO     uint8 = 3
	EngineRecovery uint8 = 4
	EngineFault    uint8 = 5

	// Sticky fault bits matching PU_FAULT_* in firmware/engine_owner.h.
	FaultOvercurrent uint32 = 1 << 0
	FaultWatchdog    uint32 = 1 << 1
	FaultWireStall   uint32 = 1 << 2
	FaultUnspecified uint32 = 1 << 31
)

// ErrCtrlUnsupported indicates IF 5 USB bulk control transport is unavailable on the host OS.
var ErrCtrlUnsupported = errors.New("usbprobe: IF 5 USB control transport is not supported on this OS")

// CtrlTransceiver exchanges one binary IF 5 vendor bulk request/response frame with probe.
type CtrlTransceiver func(probe ProbeDevice, req []byte) ([]byte, error)

// CtrlReplyError reports a non-OK status byte returned by the probe's IF 5 handler.
type CtrlReplyError struct {
	Cmd    byte
	Status byte
}

func (e *CtrlReplyError) Error() string {
	return fmt.Sprintf("usbprobe: IF 5 command 0x%02x returned status 0x%02x", e.Cmd, e.Status)
}

// CtrlInfo represents the 13-byte PU_CTRL_CMD_GET_INFO response from firmware/control_telemetry.c.
type CtrlInfo struct {
	FWVersionMajor uint8
	FWVersionMinor uint8
	FWVersionPatch uint8
	BoardIsProPCB  bool
	Capabilities   uint8
	EngineState    uint8
	ActiveSlot     uint8
	FaultMask      uint32
}

// CtrlTelemetry represents the 13-byte PU_CTRL_CMD_GET_TELEMETRY response from firmware/control_telemetry.c.
type CtrlTelemetry struct {
	VTrefMV         uint16
	ITargMA         uint16
	VTargEnabled    bool
	Overcurrent     bool
	HasADCTelemetry bool
	UARTRXDropCount uint32
}

// DefaultCtrlTransceiver sends req on IF 5 EP 0x06 OUT and reads the reply from EP 0x86 IN
// with a bounded timeout using the host OS native stdlib USB driver (Linux usbdevfs / Windows WinUSB).
func DefaultCtrlTransceiver(probe ProbeDevice, req []byte) ([]byte, error) {
	ctrlPath := strings.TrimSpace(probe.CtrlPath)
	if ctrlPath == "" {
		return nil, errors.New("usbprobe: probe has no IF 5 CtrlPath")
	}
	if len(req) == 0 || len(req) > CtrlPacketSize {
		return nil, fmt.Errorf("usbprobe: invalid IF 5 request length %d (must be 1..%d)", len(req), CtrlPacketSize)
	}
	reqCopy := append([]byte(nil), req...)
	return transceiveOSCtrlBulk(ctrlPath, reqCopy)
}

// QueryInfo executes PU_CTRL_CMD_GET_INFO (0x01) over IF 5 and decodes the 13-byte response.
func QueryInfo(probe ProbeDevice, tx CtrlTransceiver) (CtrlInfo, error) {
	if tx == nil {
		tx = DefaultCtrlTransceiver
	}
	resp, err := tx(probe, []byte{CtrlCmdGetInfo})
	if err != nil {
		return CtrlInfo{}, err
	}
	if len(resp) < 2 {
		return CtrlInfo{}, fmt.Errorf("usbprobe: truncated GET_INFO reply (%d bytes)", len(resp))
	}
	if resp[0] != CtrlCmdGetInfo {
		return CtrlInfo{}, fmt.Errorf("usbprobe: unexpected GET_INFO command echo 0x%02x", resp[0])
	}
	if resp[1] != CtrlStatusOK {
		return CtrlInfo{}, &CtrlReplyError{Cmd: CtrlCmdGetInfo, Status: resp[1]}
	}
	if len(resp) != 13 {
		return CtrlInfo{}, fmt.Errorf("usbprobe: invalid GET_INFO reply length %d (expected 13)", len(resp))
	}
	return CtrlInfo{
		FWVersionMajor: resp[2],
		FWVersionMinor: resp[3],
		FWVersionPatch: resp[4],
		BoardIsProPCB:  resp[5] != 0,
		Capabilities:   resp[6],
		EngineState:    resp[7],
		ActiveSlot:     resp[8],
		FaultMask:      binary.LittleEndian.Uint32(resp[9:13]),
	}, nil
}

// QueryTelemetry executes PU_CTRL_CMD_GET_TELEMETRY (0x02) over IF 5 and decodes the 13-byte response.
func QueryTelemetry(probe ProbeDevice, tx CtrlTransceiver) (CtrlTelemetry, error) {
	if tx == nil {
		tx = DefaultCtrlTransceiver
	}
	resp, err := tx(probe, []byte{CtrlCmdGetTelemetry})
	if err != nil {
		return CtrlTelemetry{}, err
	}
	if len(resp) < 2 {
		return CtrlTelemetry{}, fmt.Errorf("usbprobe: truncated GET_TELEMETRY reply (%d bytes)", len(resp))
	}
	if resp[0] != CtrlCmdGetTelemetry {
		return CtrlTelemetry{}, fmt.Errorf("usbprobe: unexpected GET_TELEMETRY command echo 0x%02x", resp[0])
	}
	if resp[1] != CtrlStatusOK {
		return CtrlTelemetry{}, &CtrlReplyError{Cmd: CtrlCmdGetTelemetry, Status: resp[1]}
	}
	if len(resp) != 13 {
		return CtrlTelemetry{}, fmt.Errorf("usbprobe: invalid GET_TELEMETRY reply length %d (expected 13)", len(resp))
	}
	return CtrlTelemetry{
		VTrefMV:         binary.LittleEndian.Uint16(resp[2:4]),
		ITargMA:         binary.LittleEndian.Uint16(resp[4:6]),
		VTargEnabled:    resp[6] != 0,
		Overcurrent:     resp[7] != 0,
		HasADCTelemetry: resp[8] != 0,
		UARTRXDropCount: binary.LittleEndian.Uint32(resp[9:13]),
	}, nil
}

// RequestCH32Unbrick executes PU_CTRL_CMD_CH32_UNBRICK (0x05) over IF 5 and returns
// the 2-byte firmware status code (e.g. CtrlStatusOK, CtrlStatusUnsupported, CtrlStatusBusy, CtrlStatusFaultLatched).
func RequestCH32Unbrick(probe ProbeDevice, tx CtrlTransceiver) (byte, error) {
	if tx == nil {
		tx = DefaultCtrlTransceiver
	}
	resp, err := tx(probe, []byte{CtrlCmdCH32Unbrick})
	if err != nil {
		return 0, err
	}
	if len(resp) != 2 {
		return 0, fmt.Errorf("usbprobe: invalid CH32_UNBRICK reply length %d (expected 2)", len(resp))
	}
	if resp[0] != CtrlCmdCH32Unbrick {
		return 0, fmt.Errorf("usbprobe: unexpected CH32_UNBRICK command echo 0x%02x", resp[0])
	}
	return resp[1], nil
}

// SetVTarg executes PU_CTRL_CMD_SET_VTARG (0x03) over IF 5 and returns the firmware status byte.
func SetVTarg(probe ProbeDevice, enabled bool, tx CtrlTransceiver) (byte, error) {
	if tx == nil {
		tx = DefaultCtrlTransceiver
	}
	var flag byte
	if enabled {
		flag = 1
	}
	resp, err := tx(probe, []byte{CtrlCmdSetVTarg, flag})
	if err != nil {
		return 0, err
	}
	if len(resp) < 2 {
		return 0, fmt.Errorf("usbprobe: truncated SET_VTARG reply (%d bytes)", len(resp))
	}
	if resp[0] != CtrlCmdSetVTarg {
		return 0, fmt.Errorf("usbprobe: unexpected SET_VTARG command echo 0x%02x", resp[0])
	}
	if resp[1] == CtrlStatusOK && len(resp) != 3 {
		return 0, fmt.Errorf("usbprobe: invalid SET_VTARG OK reply length %d (expected 3)", len(resp))
	}
	return resp[1], nil
}

// ClearFault executes PU_CTRL_CMD_CLEAR_FAULT (0x04) over IF 5 and returns the firmware status byte.
func ClearFault(probe ProbeDevice, tx CtrlTransceiver) (byte, error) {
	if tx == nil {
		tx = DefaultCtrlTransceiver
	}
	resp, err := tx(probe, []byte{CtrlCmdClearFault})
	if err != nil {
		return 0, err
	}
	if len(resp) != 2 {
		return 0, fmt.Errorf("usbprobe: invalid CLEAR_FAULT reply length %d (expected 2)", len(resp))
	}
	if resp[0] != CtrlCmdClearFault {
		return 0, fmt.Errorf("usbprobe: unexpected CLEAR_FAULT command echo 0x%02x", resp[0])
	}
	return resp[1], nil
}

// RunSelfTest executes PU_CTRL_CMD_SELF_TEST (0x07) over IF 5 and decodes the 32-bit pass bitmask.
func RunSelfTest(probe ProbeDevice, tx CtrlTransceiver) (uint32, error) {
	if tx == nil {
		tx = DefaultCtrlTransceiver
	}
	resp, err := tx(probe, []byte{CtrlCmdSelfTest})
	if err != nil {
		return 0, err
	}
	if len(resp) < 2 {
		return 0, fmt.Errorf("usbprobe: truncated SELF_TEST reply (%d bytes)", len(resp))
	}
	if resp[0] != CtrlCmdSelfTest {
		return 0, fmt.Errorf("usbprobe: unexpected SELF_TEST command echo 0x%02x", resp[0])
	}
	if resp[1] != CtrlStatusOK {
		return 0, &CtrlReplyError{Cmd: CtrlCmdSelfTest, Status: resp[1]}
	}
	if len(resp) != 6 {
		return 0, fmt.Errorf("usbprobe: invalid SELF_TEST reply length %d (expected 6)", len(resp))
	}
	return binary.LittleEndian.Uint32(resp[2:6]), nil
}
