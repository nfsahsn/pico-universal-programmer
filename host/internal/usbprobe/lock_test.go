package usbprobe

import (
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"sync/atomic"
	"syscall"
	"testing"
)

func TestAcquireSessionLockExclusivityAndIdempotentClose(t *testing.T) {
	dir := t.TempDir()

	l1, err := AcquireSessionLockInDir(dir, "E660583883278628")
	if err != nil {
		t.Fatalf("first AcquireSessionLockInDir failed: %v", err)
	}

	// Case-insensitive match for the same physical serial must fail immediately.
	if _, err := AcquireSessionLockInDir(dir, "e660583883278628"); !errors.Is(err, ErrProbeLocked) {
		t.Fatalf("expected ErrProbeLocked on second acquire, got %v", err)
	}

	// Different probe serial in the same directory must succeed concurrently.
	lOther, err := AcquireSessionLockInDir(dir, "E660583883999999")
	if err != nil {
		t.Fatalf("expected second distinct probe serial to lock cleanly, got %v", err)
	}
	defer lOther.Close()

	// Release first lock; subsequent acquire must succeed.
	if err := l1.Close(); err != nil {
		t.Fatalf("first Close failed: %v", err)
	}

	l2, err := AcquireSessionLockInDir(dir, "E660583883278628")
	if err != nil {
		t.Fatalf("re-acquire after Close failed: %v", err)
	}
	defer l2.Close()

	// Calling Close() again on the already-closed l1 must NOT disturb l2!
	if err := l1.Close(); err != nil {
		t.Fatalf("second Close on l1 returned error: %v", err)
	}
	if _, err := AcquireSessionLockInDir(dir, "E660583883278628"); !errors.Is(err, ErrProbeLocked) {
		t.Fatalf("stale double-Close released active l2! err=%v", err)
	}
}

func TestAcquireSessionLockConcurrentContention(t *testing.T) {
	dir := t.TempDir()
	const workers = 24
	var winners int32
	var losers int32
	ready := make(chan struct{})
	attempted := make(chan struct{}, workers)
	release := make(chan struct{})
	var wg sync.WaitGroup

	for i := 0; i < workers; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			<-ready
			lk, err := AcquireSessionLockInDir(dir, "E660CONCURRENT01")
			if err == nil {
				atomic.AddInt32(&winners, 1)
				attempted <- struct{}{}
				<-release
				_ = lk.Close()
				return
			}
			if errors.Is(err, ErrProbeLocked) {
				atomic.AddInt32(&losers, 1)
			} else {
				t.Errorf("unexpected error during contention: %v", err)
			}
			attempted <- struct{}{}
		}()
	}

	close(ready)
	for i := 0; i < workers; i++ {
		<-attempted
	}
	if gotW, gotL := atomic.LoadInt32(&winners), atomic.LoadInt32(&losers); gotW != 1 || gotL != workers-1 {
		close(release)
		wg.Wait()
		t.Fatalf("expected 1 winner and %d losers, got winners=%d losers=%d", workers-1, gotW, gotL)
	}
	close(release)
	wg.Wait()
}

func TestAcquireSessionLockCrossProcessContention(t *testing.T) {
	dir := t.TempDir()
	serial := "E660CROSSPROC123"

	lk, err := AcquireSessionLockInDir(dir, serial)
	if err != nil {
		t.Fatalf("parent lock failed: %v", err)
	}

	runChild := func() error {
		cmd := exec.Command(os.Args[0], "-test.run=^TestSessionLockSubprocessHelper$")
		cmd.Env = append(os.Environ(),
			"PU_TEST_SUBPROC_LOCK=1",
			"PU_TEST_LOCK_DIR="+dir,
			"PU_TEST_LOCK_SERIAL="+serial,
		)
		return cmd.Run()
	}

	// While parent holds lock, child process must fail to acquire it.
	if err := runChild(); err == nil {
		_ = lk.Close()
		t.Fatal("expected child process lock attempt to fail while parent holds lock")
	}

	// Once parent releases lock, child process must acquire and release cleanly.
	if err := lk.Close(); err != nil {
		t.Fatalf("parent Close failed: %v", err)
	}
	if err := runChild(); err != nil {
		t.Fatalf("expected child process lock attempt to succeed after parent Close, got %v", err)
	}
}

func TestSessionLockSubprocessHelper(t *testing.T) {
	if os.Getenv("PU_TEST_SUBPROC_LOCK") != "1" {
		return
	}
	dir := os.Getenv("PU_TEST_LOCK_DIR")
	serial := os.Getenv("PU_TEST_LOCK_SERIAL")
	lk, err := AcquireSessionLockInDir(dir, serial)
	if err != nil {
		os.Exit(2)
	}
	_ = lk.Close()
	os.Exit(0)
}

func TestAcquireSessionLockSanitizesSerialAndHonorsEnv(t *testing.T) {
	if _, err := AcquireSessionLockInDir(t.TempDir(), "   "); err == nil {
		t.Fatal("expected empty serial to fail")
	}

	envDir := filepath.Join(t.TempDir(), "locks")
	t.Setenv("PU_LOCK_DIR", envDir)

	// Hostile path-traversal serial must stay strictly inside envDir.
	lk, err := AcquireSessionLock("../../outside-escape")
	if err != nil {
		t.Fatalf("AcquireSessionLock with hostile serial failed: %v", err)
	}
	defer lk.Close()

	entries, err := os.ReadDir(envDir)
	if err != nil || len(entries) != 1 {
		t.Fatalf("expected 1 sanitized lock file inside %s, got entries=%v err=%v", envDir, entries, err)
	}
}

func TestMapWindowsLockFileErrorOnlyMapsErrno33(t *testing.T) {
	lockedErr := mapWindowsLockFileError(`C:\locks\pu-probe-TEST.lock`, syscall.Errno(33))
	if !errors.Is(lockedErr, ErrProbeLocked) {
		t.Fatalf("expected syscall.Errno(33) to map to ErrProbeLocked, got %v", lockedErr)
	}

	for _, errno := range []syscall.Errno{5, 6, 32, 87} {
		otherErr := mapWindowsLockFileError(`C:\locks\pu-probe-TEST.lock`, errno)
		if errors.Is(otherErr, ErrProbeLocked) {
			t.Fatalf("expected syscall.Errno(%d) NOT to map to ErrProbeLocked, got %v", errno, otherErr)
		}
		if !errors.Is(otherErr, errno) {
			t.Fatalf("expected syscall.Errno(%d) to be preserved via errors.Is, got %v", errno, otherErr)
		}
	}
}
