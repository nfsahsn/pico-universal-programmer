//go:build windows

package usbprobe

import (
	"fmt"
	"runtime"
	"strings"
	"syscall"
	"unsafe"
)

var (
	modWinUSB               = syscall.NewLazyDLL("winusb.dll")
	procWinUsbInitialize    = modWinUSB.NewProc("WinUsb_Initialize")
	procWinUsbSetPipePolicy = modWinUSB.NewProc("WinUsb_SetPipePolicy")
	procWinUsbWritePipe     = modWinUSB.NewProc("WinUsb_WritePipe")
	procWinUsbReadPipe      = modWinUSB.NewProc("WinUsb_ReadPipe")
	procWinUsbFree          = modWinUSB.NewProc("WinUsb_Free")
)

const winusbPipeTransferTimeout = 0x03

func transceiveOSCtrlBulk(ctrlPath string, req []byte) ([]byte, error) {
	pathUTF16, err := syscall.UTF16PtrFromString(strings.TrimSpace(ctrlPath))
	if err != nil {
		return nil, fmt.Errorf("usbprobe: encode WinUSB path %q: %w", ctrlPath, err)
	}

	devHandle, err := syscall.CreateFile(
		pathUTF16,
		syscall.GENERIC_READ|syscall.GENERIC_WRITE,
		syscall.FILE_SHARE_READ|syscall.FILE_SHARE_WRITE,
		nil,
		syscall.OPEN_EXISTING,
		syscall.FILE_ATTRIBUTE_NORMAL|syscall.FILE_FLAG_OVERLAPPED,
		0,
	)
	if err != nil {
		return nil, fmt.Errorf("usbprobe: open WinUSB device %s: %w", ctrlPath, err)
	}
	defer syscall.CloseHandle(devHandle)

	var winusbHandle uintptr
	if r1, _, callErr := procWinUsbInitialize.Call(
		uintptr(devHandle),
		uintptr(unsafe.Pointer(&winusbHandle)),
	); r1 == 0 {
		return nil, fmt.Errorf("usbprobe: WinUsb_Initialize %s: %w", ctrlPath, callErr)
	}
	defer procWinUsbFree.Call(winusbHandle)

	timeoutMS := uint32(CtrlTimeoutMS)
	for _, ep := range []byte{CtrlEndpointOut, CtrlEndpointIn} {
		if r1, _, callErr := procWinUsbSetPipePolicy.Call(
			winusbHandle,
			uintptr(ep),
			uintptr(winusbPipeTransferTimeout),
			unsafe.Sizeof(timeoutMS),
			uintptr(unsafe.Pointer(&timeoutMS)),
		); r1 == 0 {
			return nil, fmt.Errorf("usbprobe: WinUsb_SetPipePolicy(0x%02x) %s: %w", ep, ctrlPath, callErr)
		}
	}

	var written uint32
	r1, _, writeErr := procWinUsbWritePipe.Call(
		winusbHandle,
		uintptr(CtrlEndpointOut),
		uintptr(unsafe.Pointer(&req[0])),
		uintptr(len(req)),
		uintptr(unsafe.Pointer(&written)),
		0,
	)
	runtime.KeepAlive(req)
	if r1 == 0 {
		return nil, fmt.Errorf("usbprobe: WinUsb_WritePipe(0x%02x) %s: %w", CtrlEndpointOut, ctrlPath, writeErr)
	}
	if int(written) != len(req) {
		return nil, fmt.Errorf("usbprobe: short WinUsb_WritePipe on %s (%d/%d bytes)", ctrlPath, written, len(req))
	}

	var respBuf [CtrlPacketSize]byte
	var readN uint32
	r1, _, readErr := procWinUsbReadPipe.Call(
		winusbHandle,
		uintptr(CtrlEndpointIn),
		uintptr(unsafe.Pointer(&respBuf[0])),
		uintptr(len(respBuf)),
		uintptr(unsafe.Pointer(&readN)),
		0,
	)
	runtime.KeepAlive(&respBuf)
	if r1 == 0 {
		return nil, fmt.Errorf("usbprobe: WinUsb_ReadPipe(0x%02x) %s: %w", CtrlEndpointIn, ctrlPath, readErr)
	}
	if readN > uint32(len(respBuf)) {
		readN = uint32(len(respBuf))
	}
	return append([]byte(nil), respBuf[:readN]...), nil
}
