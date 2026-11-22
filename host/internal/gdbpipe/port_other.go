//go:build !linux && !windows

package gdbpipe

import (
	"fmt"
	"io"
	"os"
)

func openOSSerialPort(portPath string) (io.ReadWriteCloser, error) {
	f, err := os.OpenFile(portPath, os.O_RDWR, 0)
	if err != nil {
		return nil, fmt.Errorf("open %s: %w", portPath, err)
	}
	return f, nil
}
