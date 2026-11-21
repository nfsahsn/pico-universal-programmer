//go:build !windows

package usbprobe

import (
	"errors"
	"fmt"
	"io"
	"os"
	"syscall"
)

type unixFileLock struct {
	f *os.File
}

func (u *unixFileLock) Close() error {
	unlockErr := syscall.Flock(int(u.f.Fd()), syscall.LOCK_UN)
	closeErr := u.f.Close()
	if unlockErr != nil {
		return unlockErr
	}
	return closeErr
}

func lockOSFile(lockPath string) (io.Closer, error) {
	f, err := os.OpenFile(lockPath, os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		return nil, fmt.Errorf("usbprobe: open lock file %s: %w", lockPath, err)
	}

	for {
		err = syscall.Flock(int(f.Fd()), syscall.LOCK_EX|syscall.LOCK_NB)
		if !errors.Is(err, syscall.EINTR) {
			break
		}
	}
	if err != nil {
		_ = f.Close()
		if errors.Is(err, syscall.EWOULDBLOCK) || errors.Is(err, syscall.EAGAIN) {
			return nil, fmt.Errorf("%w: %s", ErrProbeLocked, lockPath)
		}
		return nil, fmt.Errorf("usbprobe: flock %s: %w", lockPath, err)
	}

	return &unixFileLock{f: f}, nil
}
