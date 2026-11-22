package main

import (
	"errors"
	"flag"
	"fmt"
	"net"
	"strings"

	"github.com/nfsahsn/pico-universal-programmer/host/internal/gdbpipe"
	"github.com/nfsahsn/pico-universal-programmer/host/internal/puid"
)

func runGDBPipe(ctx cliContext, args []string) int {
	fs := flag.NewFlagSet("pu gdb-pipe", flag.ContinueOnError)
	fs.SetOutput(ctx.stderr)

	serialFlag := fs.String("serial", "", "Select specific Pico Universal probe hardware serial number")
	listenFlag := fs.String("listen", "", "Bind local loopback TCP server (e.g. 127.0.0.1:3333) for VS Code cortex-debug")
	familyFlag := fs.String("family", "", "Optional target family hint (e.g. ch32v003 / ch32v)")
	pidFlag := fs.String("pid", "", "Approved USB PID in hex (overrides PU_USB_PID)")

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}

	if f := strings.TrimSpace(*familyFlag); f != "" {
		if _, err := puid.NormalizeFamily(f); err != nil {
			fmt.Fprintf(ctx.stderr, "pu gdb-pipe: %v\n", err)
			return 2
		}
	}

	listenAddr := strings.TrimSpace(*listenFlag)
	if listenAddr == "" && ctx.stdin == nil {
		fmt.Fprintln(ctx.stderr, "pu gdb-pipe: stdio stream is not attached and --listen was not specified")
		return 2
	}

	probe, err := resolveSingleProbe(ctx, *serialFlag, *pidFlag)
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu gdb-pipe: %v\n", err)
		return 1
	}

	if listenAddr != "" {
		err = gdbpipe.ServeGDBTCP(ctx.runCtx, listenAddr, probe, ctx.portOpener, func(a net.Addr) {
			fmt.Fprintf(ctx.stdout, "pu gdb-pipe listening on %s (probe %s)\n", a.String(), probe.Serial)
			if ctx.onGDBListen != nil {
				ctx.onGDBListen(a)
			}
		})
		if err != nil {
			fmt.Fprintf(ctx.stderr, "pu gdb-pipe: %v\n", err)
			return 1
		}
		return 0
	}

	if err := gdbpipe.RelayGDB(ctx.runCtx, probe, ctx.stdin, ctx.stdout, ctx.portOpener); err != nil {
		fmt.Fprintf(ctx.stderr, "pu gdb-pipe: %v\n", err)
		return 1
	}
	return 0
}
