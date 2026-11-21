//go:build windows

package gdbpipe

import (
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

var (
	modKernel32                 = syscall.NewLazyDLL("kernel32.dll")
	procGetCommState            = modKernel32.NewProc("GetCommState")
	procSetCommState            = modKernel32.NewProc("SetCommState")
	procSetCommTimeouts         = modKernel32.NewProc("SetCommTimeouts")
	procEscapeCommFunction      = modKernel32.NewProc("EscapeCommFunction")
	procCreateEventW            = modKernel32.NewProc("CreateEventW")
	procSetEvent                = modKernel32.NewProc("SetEvent")
	procResetEvent              = modKernel32.NewProc("ResetEvent")
	procWaitForMultipleObjects  = modKernel32.NewProc("WaitForMultipleObjects")
	procCancelIoEx              = modKernel32.NewProc("CancelIoEx")
	procGetOverlappedResult     = modKernel32.NewProc("GetOverlappedResult")
)

const (
	winDCBFBinary          uint32 = 0x00000001
	winDCBDtrControlEnable uint32 = 0x00000010
	winSetDTR              uint32 = 3
	winClrDTR              uint32 = 4
	winWaitObject0         uint32 = 0x00000000
)

type winDCB struct {
	DCBlength  uint32
	BaudRate   uint32
	Flags      uint32
	wReserved  uint16
	XonLim     uint16
	XoffLim    uint16
	ByteSize   byte
	Parity     byte
	StopBits   byte
	XonChar    byte
	XoffChar   byte
	ErrorChar  byte
	EofChar    byte
	EvtChar    byte
	wReserved1 uint16
}

type winCommTimeouts struct {
	ReadIntervalTimeout         uint32
	ReadTotalTimeoutMultiplier  uint32
	ReadTotalTimeoutConstant    uint32
	WriteTotalTimeoutMultiplier uint32
	WriteTotalTimeoutConstant   uint32
}

type winSerialPort struct {
	handle     syscall.Handle
	closeEvent syscall.Handle

	mu            sync.Mutex
	cond          *sync.Cond
	activeIO      int
	closed        bool
	readDeadline  time.Time
	writeDeadline time.Time
	closeOnce     sync.Once
}

func openOSSerialPort(portPath string) (io.ReadWriteCloser, error) {
	p := strings.TrimSpace(portPath)
	if strings.HasPrefix(strings.ToUpper(p), "COM") && !strings.HasPrefix(p, `\\.\`) {
		p = `\\.\` + p
	}
	pathUTF16, err := syscall.UTF16PtrFromString(p)
	if err != nil {
		return nil, fmt.Errorf("encode COM path %q: %w", p, err)
	}

	handle, err := syscall.CreateFile(
		pathUTF16,
		syscall.GENERIC_READ|syscall.GENERIC_WRITE,
		0,
		nil,
		syscall.OPEN_EXISTING,
		syscall.FILE_ATTRIBUTE_NORMAL|syscall.FILE_FLAG_OVERLAPPED,
		0,
	)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", p, err)
	}

	var dcb winDCB
	dcb.DCBlength = uint32(unsafe.Sizeof(dcb))
	if r1, _, callErr := procGetCommState.Call(uintptr(handle), uintptr(unsafe.Pointer(&dcb))); r1 == 0 {
		_ = syscall.CloseHandle(handle)
		return nil, fmt.Errorf("GetCommState %s: %w", p, callErr)
	}

	dcb.BaudRate = 115200
	dcb.ByteSize = 8
	dcb.Parity = 0
	dcb.StopBits = 0
	// Assert fBinary (0x01) and fDtrControl = DTR_CONTROL_ENABLE (0x10) so DTR=1 is asserted
	// while open and drops to DTR=0 on CloseHandle (matching firmware/main.cpp tud_cdc_line_state_cb).
	dcb.Flags = winDCBFBinary | winDCBDtrControlEnable
	if r1, _, callErr := procSetCommState.Call(uintptr(handle), uintptr(unsafe.Pointer(&dcb))); r1 == 0 {
		_ = syscall.CloseHandle(handle)
		return nil, fmt.Errorf("SetCommState %s: %w", p, callErr)
	}

	to := winCommTimeouts{
		ReadIntervalTimeout:         10,
		ReadTotalTimeoutMultiplier:  0,
		ReadTotalTimeoutConstant:    50,
		WriteTotalTimeoutMultiplier: 0,
		WriteTotalTimeoutConstant:   500,
	}
	if r1, _, callErr := procSetCommTimeouts.Call(uintptr(handle), uintptr(unsafe.Pointer(&to))); r1 == 0 {
		_ = syscall.CloseHandle(handle)
		return nil, fmt.Errorf("SetCommTimeouts %s: %w", p, callErr)
	}

	_, _, _ = procEscapeCommFunction.Call(uintptr(handle), uintptr(winSetDTR))

	closeEv, _, evErr := procCreateEventW.Call(0, 1, 0, 0)
	if closeEv == 0 {
		_ = syscall.CloseHandle(handle)
		return nil, fmt.Errorf("CreateEventW %s: %w", p, evErr)
	}

	sp := &winSerialPort{
		handle:     handle,
		closeEvent: syscall.Handle(closeEv),
	}
	sp.cond = sync.NewCond(&sp.mu)
	return sp, nil
}

func (p *winSerialPort) acquireIO(isWrite bool) (time.Time, bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.closed {
		return time.Time{}, false
	}
	p.activeIO++
	if isWrite {
		return p.writeDeadline, true
	}
	return p.readDeadline, true
}

func (p *winSerialPort) releaseIO() {
	p.mu.Lock()
	p.activeIO--
	if p.activeIO == 0 {
		p.cond.Broadcast()
	}
	p.mu.Unlock()
}

func (p *winSerialPort) isClosed() bool {
	p.mu.Lock()
	c := p.closed
	p.mu.Unlock()
	return c
}

func (p *winSerialPort) SetReadDeadline(t time.Time) error {
	p.mu.Lock()
	p.readDeadline = t
	p.mu.Unlock()
	if !t.IsZero() && !time.Now().Before(t) {
		_, _, _ = procCancelIoEx.Call(uintptr(p.handle), 0)
	}
	return nil
}

func (p *winSerialPort) SetWriteDeadline(t time.Time) error {
	p.mu.Lock()
	p.writeDeadline = t
	p.mu.Unlock()
	if !t.IsZero() && !time.Now().Before(t) {
		_, _, _ = procCancelIoEx.Call(uintptr(p.handle), 0)
	}
	return nil
}

func (p *winSerialPort) Read(b []byte) (int, error) {
	if len(b) == 0 {
		return 0, nil
	}
	opEvRaw, _, evErr := procCreateEventW.Call(0, 1, 0, 0)
	if opEvRaw == 0 {
		return 0, fmt.Errorf("CreateEventW read: %w", evErr)
	}
	opEv := syscall.Handle(opEvRaw)
	defer syscall.CloseHandle(opEv)

	for {
		dl, ok := p.acquireIO(false)
		if !ok {
			return 0, net.ErrClosed
		}
		if !dl.IsZero() && !time.Now().Before(dl) {
			p.releaseIO()
			return 0, os.ErrDeadlineExceeded
		}

		_, _, _ = procResetEvent.Call(uintptr(opEv))
		var ov syscall.Overlapped
		ov.HEvent = opEv
		var done uint32

		rErr := syscall.ReadFile(p.handle, b, &done, &ov)
		if rErr == syscall.ERROR_IO_PENDING {
			handles := [2]syscall.Handle{opEv, p.closeEvent}
			waitMS := uint32(200)
			if !dl.IsZero() {
				rem := time.Until(dl)
				if rem <= 0 {
					waitMS = 0
				} else if rem < 200*time.Millisecond {
					waitMS = uint32(rem.Milliseconds()) + 1
				}
			}
			waitRes, _, _ := procWaitForMultipleObjects.Call(
				2,
				uintptr(unsafe.Pointer(&handles[0])),
				0,
				uintptr(waitMS),
			)
			if uint32(waitRes) != winWaitObject0 {
				_, _, _ = procCancelIoEx.Call(uintptr(p.handle), uintptr(unsafe.Pointer(&ov)))
			}
			okRes, _, gErr := procGetOverlappedResult.Call(
				uintptr(p.handle),
				uintptr(unsafe.Pointer(&ov)),
				uintptr(unsafe.Pointer(&done)),
				1,
			)
			if okRes == 0 {
				rErr = gErr
			} else {
				rErr = nil
			}
		}
		p.releaseIO()

		if done > 0 {
			return int(done), nil
		}
		if p.isClosed() {
			return 0, net.ErrClosed
		}
		if rErr == syscall.ERROR_OPERATION_ABORTED {
			if !dl.IsZero() && !time.Now().Before(dl) {
				return 0, os.ErrDeadlineExceeded
			}
			return 0, net.ErrClosed
		}
		if rErr != nil {
			return 0, rErr
		}
		if !dl.IsZero() && !time.Now().Before(dl) {
			return 0, os.ErrDeadlineExceeded
		}
	}
}

func (p *winSerialPort) Write(b []byte) (int, error) {
	if len(b) == 0 {
		return 0, nil
	}
	opEvRaw, _, evErr := procCreateEventW.Call(0, 1, 0, 0)
	if opEvRaw == 0 {
		return 0, fmt.Errorf("CreateEventW write: %w", evErr)
	}
	opEv := syscall.Handle(opEvRaw)
	defer syscall.CloseHandle(opEv)

	total := 0
	for total < len(b) {
		dl, ok := p.acquireIO(true)
		if !ok {
			return total, net.ErrClosed
		}
		if !dl.IsZero() && !time.Now().Before(dl) {
			p.releaseIO()
			return total, os.ErrDeadlineExceeded
		}

		_, _, _ = procResetEvent.Call(uintptr(opEv))
		var ov syscall.Overlapped
		ov.HEvent = opEv
		var done uint32

		wErr := syscall.WriteFile(p.handle, b[total:], &done, &ov)
		if wErr == syscall.ERROR_IO_PENDING {
			handles := [2]syscall.Handle{opEv, p.closeEvent}
			waitRes, _, _ := procWaitForMultipleObjects.Call(
				2,
				uintptr(unsafe.Pointer(&handles[0])),
				0,
				1000,
			)
			if uint32(waitRes) != winWaitObject0 {
				_, _, _ = procCancelIoEx.Call(uintptr(p.handle), uintptr(unsafe.Pointer(&ov)))
			}
			okRes, _, gErr := procGetOverlappedResult.Call(
				uintptr(p.handle),
				uintptr(unsafe.Pointer(&ov)),
				uintptr(unsafe.Pointer(&done)),
				1,
			)
			if okRes == 0 {
				wErr = gErr
			} else {
				wErr = nil
			}
		}
		p.releaseIO()

		total += int(done)
		if p.isClosed() {
			return total, net.ErrClosed
		}
		if wErr != nil {
			if wErr == syscall.ERROR_OPERATION_ABORTED {
				return total, net.ErrClosed
			}
			return total, wErr
		}
		if done == 0 {
			return total, os.ErrDeadlineExceeded
		}
	}
	return total, nil
}

func (p *winSerialPort) Close() error {
	var closeErr error
	p.closeOnce.Do(func() {
		p.mu.Lock()
		p.closed = true
		p.mu.Unlock()

		_, _, _ = procSetEvent.Call(uintptr(p.closeEvent))
		_, _, _ = procCancelIoEx.Call(uintptr(p.handle), 0)

		p.mu.Lock()
		for p.activeIO > 0 {
			p.cond.Wait()
		}
		p.mu.Unlock()

		_, _, _ = procEscapeCommFunction.Call(uintptr(p.handle), uintptr(winClrDTR))
		closeErr = syscall.CloseHandle(p.handle)
		_ = syscall.CloseHandle(p.closeEvent)
	})
	return closeErr
}
