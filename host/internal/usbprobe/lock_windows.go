//go:build windows

package usbprobe

import (
	"fmt"
	"io"
	"os"
	"syscall"
	"unsafe"
)

var (
	modKernel32      = syscall.NewLazyDLL("kernel32.dll")
	procLockFileEx   = modKernel32.NewProc("LockFileEx")
	procUnlockFileEx = modKernel32.NewProc("UnlockFileEx")
)

const (
	lockfileFailImmediately = 0x00000001
	lockfileExclusiveLock   = 0x00000002
)

type windowsFileLock struct {
	f *os.File
}

func (w *windowsFileLock) Close() error {
	var ol syscall.Overlapped
	r1, _, unlockErr := procUnlockFileEx.Call(
		w.f.Fd(),
		0,
		1,
		0,
		uintptr(unsafe.Pointer(&ol)),
	)
	closeErr := w.f.Close()
	if r1 == 0 {
		return fmt.Errorf("usbprobe: UnlockFileEx: %w", unlockErr)
	}
	return closeErr
}

func lockOSFile(lockPath string) (io.Closer, error) {
	f, err := os.OpenFile(lockPath, os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		return nil, fmt.Errorf("usbprobe: open lock file %s: %w", lockPath, err)
	}

	var ol syscall.Overlapped
	r1, _, callErr := procLockFileEx.Call(
		f.Fd(),
		uintptr(lockfileExclusiveLock|lockfileFailImmediately),
		0,
		1,
		0,
		uintptr(unsafe.Pointer(&ol)),
	)
	if r1 == 0 {
		_ = f.Close()
		return nil, mapWindowsLockFileError(lockPath, callErr)
	}
	return &windowsFileLock{f: f}, nil
}
