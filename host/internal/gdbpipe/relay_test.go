package gdbpipe

import (
	"bytes"
	"context"
	"errors"
	"io"
	"net"
	"path/filepath"
	"runtime"
	"testing"
	"time"

	"github.com/nfsahsn/pico-universal-programmer/host/internal/usbprobe"
)

func waitForGoroutines(t *testing.T, baseline int) {
	t.Helper()
	deadline := time.Now().Add(1 * time.Second)
	for time.Now().Before(deadline) {
		runtime.Gosched()
		if runtime.NumGoroutine() <= baseline {
			return
		}
		time.Sleep(10 * time.Millisecond)
	}
	t.Fatalf("goroutine leak detected: baseline=%d, current=%d", baseline, runtime.NumGoroutine())
}

func TestRelayGDBStdioBidirectionalAndSessionLock(t *testing.T) {
	t.Setenv("PU_LOCK_DIR", filepath.Join(t.TempDir(), "locks"))

	probe := usbprobe.ProbeDevice{
		Serial:  "E660GDBPIPE00001",
		VID:     0x2E8A,
		PID:     0x1050,
		GDBPort: "/dev/ttyACM0",
	}

	// Verify session lock contention returns ErrProbeLocked immediately.
	held, err := usbprobe.AcquireSessionLock(probe.Serial)
	if err != nil {
		t.Fatal(err)
	}
	err = RelayGDB(context.Background(), probe, nil, io.Discard, func(string) (io.ReadWriteCloser, error) {
		t.Fatal("opener must not be called when probe lock is already held")
		return nil, nil
	})
	if !errors.Is(err, usbprobe.ErrProbeLocked) {
		_ = held.Close()
		t.Fatalf("expected ErrProbeLocked, got %v", err)
	}
	_ = held.Close()

	baselineG := runtime.NumGoroutine()

	gdbClient, relaySide := net.Pipe()
	probeServer, probePort := net.Pipe()

	opener := func(portPath string) (io.ReadWriteCloser, error) {
		if portPath != probe.GDBPort {
			return nil, errors.New("unexpected port path: " + portPath)
		}
		return probePort, nil
	}

	// Simulated firmware SWIO RSP responder.
	serverErrCh := make(chan error, 1)
	go func() {
		defer probeServer.Close()
		expectedReq := []string{"$qSupported#37", "$D#44"}
		replies := []string{"+$PacketSize=800#ea", "+$OK#9a"}
		buf := make([]byte, 128)
		for i, want := range expectedReq {
			n, rErr := io.ReadFull(probeServer, buf[:len(want)])
			if rErr != nil {
				serverErrCh <- rErr
				return
			}
			if string(buf[:n]) != want {
				serverErrCh <- errors.New("unexpected RSP packet: " + string(buf[:n]))
				return
			}
			if _, wErr := probeServer.Write([]byte(replies[i])); wErr != nil {
				serverErrCh <- wErr
				return
			}
		}
		serverErrCh <- nil
	}()

	relayDone := make(chan error, 1)
	go func() {
		relayDone <- RelayGDB(context.Background(), probe, relaySide, relaySide, opener)
	}()

	// Exchange 2 RSP round-trips from GDB client, then close GDB client stream.
	for _, pair := range []struct{ req, resp string }{
		{"$qSupported#37", "+$PacketSize=800#ea"},
		{"$D#44", "+$OK#9a"},
	} {
		if _, err := gdbClient.Write([]byte(pair.req)); err != nil {
			t.Fatalf("gdbClient write failed: %v", err)
		}
		got := make([]byte, len(pair.resp))
		if _, err := io.ReadFull(gdbClient, got); err != nil {
			t.Fatalf("gdbClient read failed: %v", err)
		}
		if string(got) != pair.resp {
			t.Fatalf("got %q, want %q", string(got), pair.resp)
		}
	}
	_ = gdbClient.Close()

	select {
	case rErr := <-relayDone:
		if rErr != nil {
			t.Fatalf("expected clean nil exit on GDB EOF, got %v", rErr)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("RelayGDB hung after GDB side closed")
	}

	if sErr := <-serverErrCh; sErr != nil {
		t.Fatalf("simulated probe server error: %v", sErr)
	}

	// Probe session lock must already be released once RelayGDB returns.
	reacquired, err := usbprobe.AcquireSessionLock(probe.Serial)
	if err != nil {
		t.Fatalf("expected probe lock to be released immediately after RelayGDB, got %v", err)
	}
	_ = reacquired.Close()

	waitForGoroutines(t, baselineG)
}

func TestRelayGDBProbeDisconnectMidTransferDoesNotHangOrLeak(t *testing.T) {
	t.Setenv("PU_LOCK_DIR", filepath.Join(t.TempDir(), "locks"))
	baselineG := runtime.NumGoroutine()

	probe := usbprobe.ProbeDevice{
		Serial:  "E660GDBDROP00002",
		VID:     0x2E8A,
		PID:     0x1050,
		GDBPort: "/dev/ttyACM1",
	}

	// Use io.Pipe for `in` (which has NO SetReadDeadline) to prove half-close teardown never hangs!
	inR, inW := io.Pipe()
	defer inW.Close()
	outR, outW := io.Pipe()
	defer outR.Close()
	defer outW.Close()

	probeServer, probePort := net.Pipe()
	opened := make(chan struct{})
	opener := func(string) (io.ReadWriteCloser, error) {
		close(opened)
		return probePort, nil
	}

	done := make(chan error, 1)
	go func() {
		done <- RelayGDB(context.Background(), probe, inR, outW, opener)
	}()

	<-opened
	// Simulate sudden USB unplug on probe CDC port while GDB stdin (inR) is still blocked in Read!
	_ = probeServer.Close()

	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("RelayGDB hung when probe port disconnected mid-session while stdin was open")
	}

	waitForGoroutines(t, baselineG)
}

func TestRelayGDBContextCancellationCleanTeardown(t *testing.T) {
	t.Setenv("PU_LOCK_DIR", filepath.Join(t.TempDir(), "locks"))
	baselineG := runtime.NumGoroutine()

	probe := usbprobe.ProbeDevice{
		Serial:  "E660GDBCANCEL003",
		VID:     0x2E8A,
		PID:     0x1050,
		GDBPort: "/dev/ttyACM2",
	}

	gdbClient, relaySide := net.Pipe()
	defer gdbClient.Close()
	probeServer, probePort := net.Pipe()
	defer probeServer.Close()

	ctx, cancel := context.WithCancel(context.Background())
	opened := make(chan struct{})
	done := make(chan error, 1)
	go func() {
		done <- RelayGDB(ctx, probe, relaySide, relaySide, func(string) (io.ReadWriteCloser, error) {
			close(opened)
			return probePort, nil
		})
	}()

	<-opened
	cancel()

	select {
	case err := <-done:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("expected context.Canceled, got %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("RelayGDB hung after context cancellation")
	}

	lk, err := usbprobe.AcquireSessionLock(probe.Serial)
	if err != nil {
		t.Fatalf("lock was not released after cancellation: %v", err)
	}
	_ = lk.Close()

	waitForGoroutines(t, baselineG)
}

func TestServeGDBTCPLoopbackAndRejectsNonLoopback(t *testing.T) {
	t.Setenv("PU_LOCK_DIR", filepath.Join(t.TempDir(), "locks"))
	baselineG := runtime.NumGoroutine()

	probe := usbprobe.ProbeDevice{
		Serial:  "E660GDBTCP000004",
		VID:     0x2E8A,
		PID:     0x1050,
		GDBPort: "/dev/ttyACM3",
	}

	// Non-loopback bind must be rejected immediately for host security.
	if err := ServeGDBTCP(context.Background(), "0.0.0.0:0", probe, func(string) (io.ReadWriteCloser, error) {
		return nil, nil
	}, nil); err == nil {
		t.Fatal("expected non-loopback 0.0.0.0 bind to be rejected")
	}

	probeServer, probePort := net.Pipe()
	go func() {
		defer probeServer.Close()
		buf := make([]byte, 5)
		if _, err := io.ReadFull(probeServer, buf); err == nil && string(buf) == "$?#3f" {
			_, _ = probeServer.Write([]byte("+$S05#b8"))
		}
	}()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	addrCh := make(chan net.Addr, 1)
	srvDone := make(chan error, 1)
	go func() {
		srvDone <- ServeGDBTCP(ctx, "127.0.0.1:0", probe, func(string) (io.ReadWriteCloser, error) {
			return probePort, nil
		}, func(a net.Addr) {
			addrCh <- a
		})
	}()

	var bound net.Addr
	select {
	case bound = <-addrCh:
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for ServeGDBTCP listener")
	}

	conn, err := net.DialTimeout("tcp", bound.String(), 2*time.Second)
	if err != nil {
		t.Fatalf("TCP dial %s failed: %v", bound.String(), err)
	}
	if _, err := conn.Write([]byte("$?#3f")); err != nil {
		_ = conn.Close()
		t.Fatalf("TCP write failed: %v", err)
	}
	reply := make([]byte, 8)
	if _, err := io.ReadFull(conn, reply); err != nil {
		_ = conn.Close()
		t.Fatalf("TCP read failed: %v", err)
	}
	if string(reply) != "+$S05#b8" {
		_ = conn.Close()
		t.Fatalf("unexpected TCP GDB reply %q", string(reply))
	}
	_ = conn.Close()

	// Single-session relay completes cleanly once client disconnects (or on ctx cancel).
	select {
	case srvErr := <-srvDone:
		if srvErr != nil && !errors.Is(srvErr, context.Canceled) {
			t.Fatalf("ServeGDBTCP returned unexpected error: %v", srvErr)
		}
	case <-time.After(2 * time.Second):
		cancel()
		t.Fatal("ServeGDBTCP hung after TCP client disconnect")
	}

	waitForGoroutines(t, baselineG)
}

func TestRelayGDBStdioEOFDrainsInFlightRSPWithoutTruncation(t *testing.T) {
	t.Setenv("PU_LOCK_DIR", filepath.Join(t.TempDir(), "locks"))
	baselineG := runtime.NumGoroutine()

	probe := usbprobe.ProbeDevice{
		Serial:  "E660GDBEOF000005",
		VID:     0x2E8A,
		PID:     0x1050,
		GDBPort: "/dev/ttyACM0",
	}

	probeServer, probePort := net.Pipe()
	defer probeServer.Close()

	go func() {
		buf := make([]byte, 5)
		if _, err := io.ReadFull(probeServer, buf); err == nil && string(buf) == "$g#67" {
			// Simulate SWIO wire round-trip latency while stdin has already hit EOF,
			// and keep probeServer open like a real /dev/ttyACM0 device.
			time.Sleep(25 * time.Millisecond)
			_, _ = probeServer.Write([]byte("+$00000000#80"))
		}
	}()

	var out bytes.Buffer
	err := RelayGDB(context.Background(), probe, stringsNewReader("$g#67"), &out, func(string) (io.ReadWriteCloser, error) {
		return probePort, nil
	})
	if err != nil {
		t.Fatalf("RelayGDB failed: %v", err)
	}
	if out.String() != "+$00000000#80" {
		t.Fatalf("expected full in-flight RSP reply, got %q", out.String())
	}

	waitForGoroutines(t, baselineG)
}

type uninterruptibleBlockingReader struct {
	unblock <-chan struct{}
}

func (r uninterruptibleBlockingReader) Read(_ []byte) (int, error) {
	<-r.unblock
	return 0, io.EOF
}

func stringsNewReader(s string) io.Reader {
	return bytes.NewBufferString(s)
}

func TestRelayGDBUninterruptibleStdinDoesNotBlockLockReleaseOnDisconnect(t *testing.T) {
	t.Setenv("PU_LOCK_DIR", filepath.Join(t.TempDir(), "locks"))

	probe := usbprobe.ProbeDevice{
		Serial:  "E660GDBSTDIN0006",
		VID:     0x2E8A,
		PID:     0x1050,
		GDBPort: "/dev/ttyACM0",
	}

	unblockStdin := make(chan struct{})
	defer close(unblockStdin)

	probeServer, probePort := net.Pipe()
	opened := make(chan struct{})
	done := make(chan error, 1)
	go func() {
		done <- RelayGDB(context.Background(), probe, uninterruptibleBlockingReader{unblock: unblockStdin}, io.Discard, func(string) (io.ReadWriteCloser, error) {
			close(opened)
			return probePort, nil
		})
	}()

	<-opened
	// Disconnect probe while uninterruptible stdin reader (simulating os.Stdin) remains blocked.
	_ = probeServer.Close()

	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("unexpected error on probe disconnect: %v", err)
		}
	case <-time.After(400 * time.Millisecond):
		t.Fatal("RelayGDB blocked waiting on uninterruptible stdin Read instead of releasing lock promptly")
	}

	lk, err := usbprobe.AcquireSessionLock(probe.Serial)
	if err != nil {
		t.Fatalf("expected probe session lock to be released immediately, got %v", err)
	}
	_ = lk.Close()
}
