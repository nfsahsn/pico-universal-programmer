package usbprobe

import (
	"crypto/sha256"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
)

// ErrProbeLocked is returned when another process or goroutine holds the session lock
// for the requested Pico Universal probe serial number.
var ErrProbeLocked = errors.New("usbprobe: probe session is locked by another active session")

var (
	inProcMu    sync.Mutex
	inProcLocks = make(map[string]struct{})
)

type sessionLock struct {
	key     string
	osLock  io.Closer
	once    sync.Once
	closeEr error
}

func (l *sessionLock) Close() error {
	l.once.Do(func() {
		err := l.osLock.Close()
		inProcMu.Lock()
		delete(inProcLocks, l.key)
		inProcMu.Unlock()
		l.closeEr = err
	})
	return l.closeEr
}

// AcquireSessionLock acquires an exclusive non-blocking cross-process lock for serial
// inside the default per-user lock directory (or PU_LOCK_DIR when set).
func AcquireSessionLock(serial string) (io.Closer, error) {
	dir, err := defaultLockDir()
	if err != nil {
		return nil, err
	}
	return AcquireSessionLockInDir(dir, serial)
}

// AcquireSessionLockInDir acquires an exclusive non-blocking cross-process lock for serial
// inside dir.
func AcquireSessionLockInDir(dir, serial string) (io.Closer, error) {
	if strings.TrimSpace(dir) == "" {
		return nil, errors.New("usbprobe: lock directory must not be empty")
	}
	safeSerial, err := sanitizeSerial(serial)
	if err != nil {
		return nil, err
	}
	if err := os.MkdirAll(dir, 0o700); err != nil {
		return nil, fmt.Errorf("usbprobe: create lock dir %s: %w", dir, err)
	}

	lockPath, err := filepath.Abs(filepath.Join(dir, "pu-probe-"+safeSerial+".lock"))
	if err != nil {
		return nil, fmt.Errorf("usbprobe: resolve lock path: %w", err)
	}

	inProcMu.Lock()
	if _, busy := inProcLocks[lockPath]; busy {
		inProcMu.Unlock()
		return nil, fmt.Errorf("%w: serial %s (%s)", ErrProbeLocked, safeSerial, lockPath)
	}
	inProcLocks[lockPath] = struct{}{}
	inProcMu.Unlock()

	osLk, err := lockOSFile(lockPath)
	if err != nil {
		inProcMu.Lock()
		delete(inProcLocks, lockPath)
		inProcMu.Unlock()
		return nil, err
	}

	return &sessionLock{
		key:    lockPath,
		osLock: osLk,
	}, nil
}

func defaultLockDir() (string, error) {
	if env := strings.TrimSpace(os.Getenv("PU_LOCK_DIR")); env != "" {
		return env, nil
	}
	if xdg := strings.TrimSpace(os.Getenv("XDG_RUNTIME_DIR")); xdg != "" {
		return filepath.Join(xdg, "pico-universal", "locks"), nil
	}
	if cacheDir, err := os.UserCacheDir(); err == nil && cacheDir != "" {
		return filepath.Join(cacheDir, "pico-universal", "locks"), nil
	}
	return filepath.Join(os.TempDir(), "pico-universal-locks"), nil
}

func sanitizeSerial(serial string) (string, error) {
	trimmed := strings.TrimSpace(serial)
	if trimmed == "" {
		return "", errors.New("usbprobe: probe serial must not be empty")
	}
	upper := strings.ToUpper(trimmed)

	var b strings.Builder
	b.Grow(len(upper))
	modified := false
	for i := 0; i < len(upper); i++ {
		c := upper[i]
		if (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' {
			b.WriteByte(c)
		} else {
			b.WriteByte('_')
			modified = true
		}
	}

	clean := b.String()
	if len(clean) > 48 {
		clean = clean[:48]
		modified = true
	}
	if modified {
		sum := sha256.Sum256([]byte(upper))
		clean = clean + "-" + hex.EncodeToString(sum[:6])
	}
	return clean, nil
}

const windowsErrorLockViolation syscall.Errno = 33 // ERROR_LOCK_VIOLATION

func mapWindowsLockFileError(lockPath string, callErr error) error {
	if errors.Is(callErr, windowsErrorLockViolation) {
		return fmt.Errorf("%w: %s", ErrProbeLocked, lockPath)
	}
	return fmt.Errorf("usbprobe: LockFileEx %s: %w", lockPath, callErr)
}
