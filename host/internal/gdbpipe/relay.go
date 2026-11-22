package gdbpipe

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/nfsahsn/pico-universal-programmer/host/internal/usbprobe"
)

// PortOpener opens a probe's bidirectional GDB CDC serial transport.
type PortOpener func(portPath string) (io.ReadWriteCloser, error)

// OpenSerialPort opens the OS serial/CDC device in raw, non-blocking / bounded binary mode.
func OpenSerialPort(portPath string) (io.ReadWriteCloser, error) {
	return openOSSerialPort(portPath)
}

// DefaultPortOpener opens the OS serial/CDC device in raw transparent binary mode.
func DefaultPortOpener(portPath string) (io.ReadWriteCloser, error) {
	return OpenSerialPort(portPath)
}

func normalizeLoopbackAddr(raw string) (string, error) {
	addr := strings.TrimSpace(raw)
	if addr == "" {
		return "", errors.New("gdbpipe: listen address must not be empty")
	}
	host, port, err := net.SplitHostPort(addr)
	if err != nil {
		return "", fmt.Errorf("gdbpipe: invalid --listen address %q: %w", raw, err)
	}
	if host == "" {
		return net.JoinHostPort("127.0.0.1", port), nil
	}
	if strings.EqualFold(host, "localhost") {
		return net.JoinHostPort("127.0.0.1", port), nil
	}
	ip := net.ParseIP(host)
	if ip == nil || !ip.IsLoopback() {
		return "", fmt.Errorf("gdbpipe: refusing non-loopback --listen host %q (must bind 127.0.0.1, ::1, or localhost)", host)
	}
	return net.JoinHostPort(host, port), nil
}

// RelayGDB acquires the cross-process session lock for probe.Serial, opens probe.GDBPort
// via opener (defaulting to DefaultPortOpener), and relays GDB RSP traffic bidirectionally
// between in/out and the probe with leak-free half-close and cancellation teardown.
func RelayGDB(ctx context.Context, probe usbprobe.ProbeDevice, in io.Reader, out io.Writer, opener PortOpener) error {
	if ctx == nil {
		ctx = context.Background()
	}
	if strings.TrimSpace(probe.Serial) == "" {
		return errors.New("gdbpipe: probe serial must not be empty")
	}
	if strings.TrimSpace(probe.GDBPort) == "" {
		return fmt.Errorf("gdbpipe: probe %s has no GDB CDC port", probe.Serial)
	}
	if opener == nil {
		opener = DefaultPortOpener
	}
	if in == nil {
		in = bytes.NewReader(nil)
	}
	if out == nil {
		out = io.Discard
	}

	lk, err := usbprobe.AcquireSessionLock(probe.Serial)
	if err != nil {
		return err
	}
	defer lk.Close()

	if err := ctx.Err(); err != nil {
		return err
	}

	port, err := opener(probe.GDBPort)
	if err != nil {
		return fmt.Errorf("gdbpipe: open %s: %w", probe.GDBPort, err)
	}

	return relayStreams(ctx, in, out, port)
}

// ServeGDBTCP acquires the cross-process session lock for probe.Serial, binds a strictly
// loopback TCP listener (e.g. 127.0.0.1:3333) for VS Code cortex-debug / GDB, and relays
// the accepted GDB client session to probe.GDBPort with clean disconnect teardown.
func ServeGDBTCP(ctx context.Context, listenAddr string, probe usbprobe.ProbeDevice, opener PortOpener, onReady func(net.Addr)) error {
	if ctx == nil {
		ctx = context.Background()
	}
	normAddr, err := normalizeLoopbackAddr(listenAddr)
	if err != nil {
		return err
	}
	if strings.TrimSpace(probe.Serial) == "" {
		return errors.New("gdbpipe: probe serial must not be empty")
	}
	if strings.TrimSpace(probe.GDBPort) == "" {
		return fmt.Errorf("gdbpipe: probe %s has no GDB CDC port", probe.Serial)
	}
	if opener == nil {
		opener = DefaultPortOpener
	}

	lk, err := usbprobe.AcquireSessionLock(probe.Serial)
	if err != nil {
		return err
	}
	defer lk.Close()

	ln, err := net.Listen("tcp", normAddr)
	if err != nil {
		return fmt.Errorf("gdbpipe: listen %s: %w", normAddr, err)
	}

	var lnOnce sync.Once
	closeLn := func() {
		lnOnce.Do(func() {
			_ = ln.Close()
		})
	}
	defer closeLn()

	if onReady != nil {
		onReady(ln.Addr())
	}

	acceptStop := make(chan struct{})
	var acceptWatchWG sync.WaitGroup
	acceptWatchWG.Add(1)
	go func() {
		defer acceptWatchWG.Done()
		select {
		case <-ctx.Done():
			closeLn()
		case <-acceptStop:
		}
	}()

	conn, acceptErr := ln.Accept()
	close(acceptStop)
	acceptWatchWG.Wait()
	closeLn()

	if acceptErr != nil {
		if ctxErr := ctx.Err(); ctxErr != nil {
			return ctxErr
		}
		return fmt.Errorf("gdbpipe: accept TCP client: %w", acceptErr)
	}
	defer conn.Close()

	port, err := opener(probe.GDBPort)
	if err != nil {
		return fmt.Errorf("gdbpipe: open %s: %w", probe.GDBPort, err)
	}

	return relayStreams(ctx, conn, conn, port)
}

const (
	stdioEOFIdleDrain = 80 * time.Millisecond
	stdioEOFMaxDrain  = 500 * time.Millisecond
)

func interruptBlockingIO(in io.Reader, out io.Writer) bool {
	now := time.Now()
	unblockedIn := false
	if in != os.Stdin {
		if rd, ok := in.(interface{ SetReadDeadline(time.Time) error }); ok {
			if err := rd.SetReadDeadline(now); err == nil {
				unblockedIn = true
			}
		}
		if !unblockedIn {
			if c, ok := in.(io.Closer); ok {
				_ = c.Close()
				unblockedIn = true
			}
		}
	}
	if wd, ok := out.(interface{ SetWriteDeadline(time.Time) error }); ok && out != os.Stdout && out != os.Stderr {
		_ = wd.SetWriteDeadline(now)
	}
	return unblockedIn
}

func isBenignShutdownErr(err error) bool {
	if err == nil || errors.Is(err, io.EOF) || errors.Is(err, io.ErrClosedPipe) || errors.Is(err, net.ErrClosed) || errors.Is(err, os.ErrDeadlineExceeded) {
		return true
	}
	msg := strings.ToLower(err.Error())
	return strings.Contains(msg, "use of closed") || strings.Contains(msg, "closed pipe") || strings.Contains(msg, "file already closed")
}

func drainPortAfterInputEOF(ctx context.Context, portDone <-chan struct{}, lastRxNano *atomic.Int64) {
	start := time.Now()
	ticker := time.NewTicker(10 * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-portDone:
			return
		case <-ctx.Done():
			return
		case now := <-ticker.C:
			if now.Sub(start) >= stdioEOFMaxDrain {
				return
			}
			lastActivity := start
			if rxNano := lastRxNano.Load(); rxNano > 0 {
				if rxTime := time.Unix(0, rxNano); rxTime.After(lastActivity) {
					lastActivity = rxTime
				}
			}
			if now.Sub(lastActivity) >= stdioEOFIdleDrain {
				return
			}
		}
	}
}

func relayStreams(ctx context.Context, in io.Reader, out io.Writer, port io.ReadWriteCloser) error {
	var shuttingDown atomic.Bool
	var unblockedIn atomic.Bool
	var lastPortRxNano atomic.Int64
	var shutdownOnce sync.Once
	shutdown := func() {
		shutdownOnce.Do(func() {
			shuttingDown.Store(true)
			_ = port.Close()
			unblockedIn.Store(interruptBlockingIO(in, out))
		})
	}

	stopWatch := make(chan struct{})
	var watchWG sync.WaitGroup
	watchWG.Add(1)
	go func() {
		defer watchWG.Done()
		select {
		case <-ctx.Done():
			shutdown()
		case <-stopWatch:
		}
	}()

	var errMu sync.Mutex
	var firstUnexpectedErr error
	recordErr := func(err error) {
		if err == nil {
			return
		}
		errMu.Lock()
		if firstUnexpectedErr == nil {
			firstUnexpectedErr = err
		}
		errMu.Unlock()
	}

	inDone := make(chan struct{})
	portDone := make(chan struct{})

	// GDB -> Probe CDC
	go func() {
		defer close(inDone)
		buf := make([]byte, 4096)
		cleanInputEOF := false
		for {
			n, rErr := in.Read(buf)
			if shuttingDown.Load() {
				break
			}
			if n > 0 {
				if _, wErr := port.Write(buf[:n]); wErr != nil {
					if !shuttingDown.Load() && !isBenignShutdownErr(wErr) {
						recordErr(fmt.Errorf("gdbpipe: write probe port: %w", wErr))
					}
					break
				}
			}
			if rErr != nil {
				if errors.Is(rErr, io.EOF) && !shuttingDown.Load() && ctx.Err() == nil {
					cleanInputEOF = true
				} else if !shuttingDown.Load() && !isBenignShutdownErr(rErr) {
					recordErr(fmt.Errorf("gdbpipe: read GDB input: %w", rErr))
				}
				break
			}
		}
		if cleanInputEOF {
			drainPortAfterInputEOF(ctx, portDone, &lastPortRxNano)
		}
		shutdown()
	}()

	// Probe CDC -> GDB
	go func() {
		defer close(portDone)
		buf := make([]byte, 4096)
		for {
			n, rErr := port.Read(buf)
			if n > 0 {
				lastPortRxNano.Store(time.Now().UnixNano())
				if _, wErr := out.Write(buf[:n]); wErr != nil {
					if !shuttingDown.Load() && !isBenignShutdownErr(wErr) {
						recordErr(fmt.Errorf("gdbpipe: write GDB output: %w", wErr))
					}
					break
				}
			}
			if rErr != nil {
				if !shuttingDown.Load() && !isBenignShutdownErr(rErr) {
					recordErr(fmt.Errorf("gdbpipe: read probe port: %w", rErr))
				}
				break
			}
		}
		shutdown()
	}()

	// Always wait for port -> out cleanup so port is closed and out writes have completely stopped.
	<-portDone
	shutdown()

	// Only wait on in -> port when input could be interrupted; never block forever on os.Stdin.Read.
	if unblockedIn.Load() {
		select {
		case <-inDone:
		case <-time.After(250 * time.Millisecond):
		}
	} else {
		select {
		case <-inDone:
		case <-time.After(10 * time.Millisecond):
		}
	}

	close(stopWatch)
	watchWG.Wait()

	if ctxErr := ctx.Err(); ctxErr != nil {
		return ctxErr
	}
	return firstUnexpectedErr
}
