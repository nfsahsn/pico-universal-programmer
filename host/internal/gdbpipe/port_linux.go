//go:build linux

package gdbpipe

import (
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"sync/atomic"
	"syscall"
	"time"
	"unsafe"
)

const linuxCRTSCTS = 0x80000000

type linuxSerialPort struct {
	file   *os.File
	closed atomic.Bool
}

func (p *linuxSerialPort) Read(b []byte) (int, error) {
	for {
		n, err := p.file.Read(b)
		if err != nil && errors.Is(err, syscall.EAGAIN) {
			if p.closed.Load() {
				return 0, net.ErrClosed
			}
			time.Sleep(5 * time.Millisecond)
			continue
		}
		return n, err
	}
}

func (p *linuxSerialPort) Write(b []byte) (int, error) {
	total := 0
	for total < len(b) {
		n, err := p.file.Write(b[total:])
		if n > 0 {
			total += n
		}
		if err != nil {
			if errors.Is(err, syscall.EAGAIN) {
				if p.closed.Load() {
					return total, net.ErrClosed
				}
				time.Sleep(5 * time.Millisecond)
				continue
			}
			return total, err
		}
	}
	return total, nil
}

func (p *linuxSerialPort) SetReadDeadline(t time.Time) error {
	return p.file.SetReadDeadline(t)
}

func (p *linuxSerialPort) SetWriteDeadline(t time.Time) error {
	return p.file.SetWriteDeadline(t)
}

func (p *linuxSerialPort) Close() error {
	p.closed.Store(true)
	_ = p.file.SetDeadline(time.Now())
	return p.file.Close()
}

func openOSSerialPort(portPath string) (io.ReadWriteCloser, error) {
	fd, err := syscall.Open(portPath, syscall.O_RDWR|syscall.O_NOCTTY|syscall.O_NONBLOCK|syscall.O_CLOEXEC, 0)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", portPath, err)
	}

	var tio syscall.Termios
	if _, _, errno := syscall.Syscall6(
		syscall.SYS_IOCTL,
		uintptr(fd),
		uintptr(syscall.TCGETS),
		uintptr(unsafe.Pointer(&tio)),
		0, 0, 0,
	); errno != 0 {
		_ = syscall.Close(fd)
		return nil, fmt.Errorf("tcgets %s: %w", portPath, errno)
	}

	// Configure 8N1 raw mode (cfmakeraw) with bounded VMIN=0, VTIME=1 (100 ms) so Linux N_TTY
	// never echoes or buffers GDB RSP packets and reads never block indefinitely.
	tio.Iflag &^= syscall.IGNBRK | syscall.BRKINT | syscall.PARMRK | syscall.ISTRIP |
		syscall.INLCR | syscall.IGNCR | syscall.ICRNL | syscall.IXON | syscall.IXOFF
	tio.Oflag &^= syscall.OPOST
	tio.Lflag &^= syscall.ECHO | syscall.ECHONL | syscall.ICANON | syscall.ISIG | syscall.IEXTEN
	tio.Cflag &^= syscall.CSIZE | syscall.PARENB | syscall.CSTOPB | linuxCRTSCTS
	tio.Cflag |= syscall.CS8 | syscall.CLOCAL | syscall.CREAD | syscall.HUPCL
	tio.Cc[syscall.VMIN] = 0
	tio.Cc[syscall.VTIME] = 1

	if _, _, errno := syscall.Syscall6(
		syscall.SYS_IOCTL,
		uintptr(fd),
		uintptr(syscall.TCSETS),
		uintptr(unsafe.Pointer(&tio)),
		0, 0, 0,
	); errno != 0 {
		_ = syscall.Close(fd)
		return nil, fmt.Errorf("tcsets %s: %w", portPath, errno)
	}

	// Best-effort assert DTR=1/RTS=1 for USB CDC-ACM line state (PTY slaves ignore TIOCMBIS).
	modemBits := int32(syscall.TIOCM_DTR | syscall.TIOCM_RTS)
	_, _, _ = syscall.Syscall6(
		syscall.SYS_IOCTL,
		uintptr(fd),
		uintptr(syscall.TIOCMBIS),
		uintptr(unsafe.Pointer(&modemBits)),
		0, 0, 0,
	)

	f := os.NewFile(uintptr(fd), portPath)
	if f == nil {
		_ = syscall.Close(fd)
		return nil, fmt.Errorf("wrap descriptor %s: invalid fd", portPath)
	}
	return &linuxSerialPort{file: f}, nil
}
