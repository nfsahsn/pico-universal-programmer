//go:build linux

package gdbpipe

import (
	"fmt"
	"os"
	"path/filepath"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

func openLinuxPTYPair(t *testing.T) (master *os.File, slavePath string) {
	t.Helper()
	mfd, err := syscall.Open("/dev/ptmx", syscall.O_RDWR|syscall.O_NOCTTY|syscall.O_CLOEXEC, 0)
	if err != nil {
		t.Skipf("cannot open /dev/ptmx in this environment: %v", err)
	}
	var unlock int32 = 0
	if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(mfd), uintptr(syscall.TIOCSPTLCK), uintptr(unsafe.Pointer(&unlock))); errno != 0 {
		_ = syscall.Close(mfd)
		t.Fatalf("TIOCSPTLCK failed: %v", errno)
	}
	var ptyNum uint32
	if _, _, errno := syscall.Syscall(syscall.SYS_IOCTL, uintptr(mfd), uintptr(syscall.TIOCGPTN), uintptr(unsafe.Pointer(&ptyNum))); errno != 0 {
		_ = syscall.Close(mfd)
		t.Fatalf("TIOCGPTN failed: %v", errno)
	}
	if err := syscall.SetNonblock(mfd, true); err != nil {
		_ = syscall.Close(mfd)
		t.Fatalf("SetNonblock master failed: %v", err)
	}
	return os.NewFile(uintptr(mfd), "/dev/ptmx"), fmt.Sprintf("/dev/pts/%d", ptyNum)
}

func TestOpenSerialPortLinuxRejectsNonTTYAndConfiguresNonBlockingRawPTY(t *testing.T) {
	// 1. Regular file must fail TCGETS cleanly instead of silently running without raw termios.
	regFile := filepath.Join(t.TempDir(), "not_a_tty.bin")
	if err := os.WriteFile(regFile, []byte("x"), 0o600); err != nil {
		t.Fatal(err)
	}
	if p, err := OpenSerialPort(regFile); err == nil {
		_ = p.Close()
		t.Fatal("expected OpenSerialPort on regular file to fail TCGETS with ENOTTY, got nil")
	}

	// 2. Real Linux PTY slave must configure 8N1 raw mode with zero echo and wake Read promptly on Close.
	master, slavePath := openLinuxPTYPair(t)
	defer master.Close()

	port, err := OpenSerialPort(slavePath)
	if err != nil {
		t.Fatalf("OpenSerialPort(%s) failed: %v", slavePath, err)
	}

	// Send bytes including CR, LF, and Ctrl-C (0x03) from master -> slave; verify raw transparent delivery and no echo.
	payload := []byte("$qRcmd,7075696431#54\r\n\x03")
	if _, err := master.Write(payload); err != nil {
		_ = port.Close()
		t.Fatalf("master write failed: %v", err)
	}

	got := make([]byte, len(payload))
	readTotal := 0
	deadline := time.Now().Add(1 * time.Second)
	for readTotal < len(payload) && time.Now().Before(deadline) {
		n, rErr := port.Read(got[readTotal:])
		if n > 0 {
			readTotal += n
		}
		if rErr != nil {
			_ = port.Close()
			t.Fatalf("port.Read failed after %d bytes: %v", readTotal, rErr)
		}
	}
	if string(got[:readTotal]) != string(payload) {
		_ = port.Close()
		t.Fatalf("raw TTY mismatch: got %x, want %x", got[:readTotal], payload)
	}

	// Verify slave did NOT echo anything back to master.
	_ = master.SetReadDeadline(time.Now().Add(30 * time.Millisecond))
	echoBuf := make([]byte, 64)
	if echoN, _ := master.Read(echoBuf); echoN > 0 {
		_ = port.Close()
		t.Fatalf("expected zero TTY echo in raw mode, got %d echoed bytes: %q", echoN, string(echoBuf[:echoN]))
	}

	// Verify Close() immediately wakes up a concurrent blocked Read() without hanging.
	readDone := make(chan error, 1)
	go func() {
		buf := make([]byte, 64)
		_, rErr := port.Read(buf)
		readDone <- rErr
	}()
	time.Sleep(25 * time.Millisecond)
	if err := port.Close(); err != nil {
		t.Fatalf("port.Close() failed: %v", err)
	}

	select {
	case rErr := <-readDone:
		if rErr == nil {
			t.Fatal("expected non-nil error from blocked Read after Close()")
		}
	case <-time.After(300 * time.Millisecond):
		t.Fatal("port.Read hung after port.Close() on Linux TTY")
	}
}
