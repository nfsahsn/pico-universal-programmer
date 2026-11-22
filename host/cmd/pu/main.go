package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"strconv"
	"strings"

	"github.com/nfsahsn/pico-universal-programmer/host/internal/catalog"
	"github.com/nfsahsn/pico-universal-programmer/host/internal/gdbpipe"
	"github.com/nfsahsn/pico-universal-programmer/host/internal/usbprobe"
)

var version = "0.1.0"

type commandSpec struct {
	Name string
	Args []string
	Env  []string
}

type commandRunner interface {
	LookPath(file string) (string, error)
	Run(cmd commandSpec, stdout, stderr io.Writer) error
}

type osCommandRunner struct{}

func (osCommandRunner) LookPath(file string) (string, error) {
	return catalog.ResolveHostTool(file)
}

func (osCommandRunner) Run(spec commandSpec, stdout, stderr io.Writer) error {
	c := exec.Command(spec.Name, spec.Args...)
	if len(spec.Env) > 0 {
		c.Env = append(os.Environ(), spec.Env...)
	}
	c.Stdout = stdout
	c.Stderr = stderr
	return c.Run()
}

type cliEnv struct {
	ctx             context.Context
	stdin           io.Reader
	stdout          io.Writer
	stderr          io.Writer
	getenv          func(string) string
	geteuid         func() int
	goos            string
	cmdRunner       commandRunner
	discoverProbes  func(vid, pid uint16) ([]usbprobe.ProbeDevice, error)
	portOpener      gdbpipe.PortOpener
	ctrlTransceiver func(probe usbprobe.ProbeDevice, req []byte) ([]byte, error)
	onGDBListen     func(net.Addr)
}

type cliContext struct {
	runCtx          context.Context
	stdin           io.Reader
	stdout          io.Writer
	stderr          io.Writer
	getenv          func(string) string
	geteuid         func() int
	goos            string
	cmdRunner       commandRunner
	discoverProbes  func(vid, pid uint16) ([]usbprobe.ProbeDevice, error)
	portOpener      gdbpipe.PortOpener
	ctrlTransceiver func(probe usbprobe.ProbeDevice, req []byte) ([]byte, error)
	onGDBListen     func(net.Addr)
	globalPID       string
	hasFakeDiscover bool
}

type subcommandFunc func(ctx cliContext, args []string) int

// dispatchHooks routes live Task 8 commands and preserves clean extension points for Tasks 11/12.
var dispatchHooks = map[string]subcommandFunc{
	"check":         runCheck,
	"upload":        runUpload,
	"gdb-pipe":      runGDBPipe,
	"recover":       runRecover,
	"export-vscode": runExportVSCode,
	"factory-qc":    runFactoryQC,
	"bench":         runBench,
}

func stubSubcommand(name, phase string) subcommandFunc {
	return func(ctx cliContext, _ []string) int {
		fmt.Fprintf(ctx.stderr, "pu %s: not yet implemented (%s)\n", name, phase)
		return 2
	}
}

func main() {
	sigCtx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	code := runWithEnv(os.Args[1:], cliEnv{
		ctx:    sigCtx,
		stdin:  os.Stdin,
		stdout: os.Stdout,
		stderr: os.Stderr,
		getenv: os.Getenv,
	})
	stop()
	os.Exit(code)
}

func run(args []string, stdout, stderr io.Writer, getenv func(string) string) int {
	return runWithEnv(args, cliEnv{
		stdout: stdout,
		stderr: stderr,
		getenv: getenv,
	})
}

func runWithEnv(args []string, env cliEnv) int {
	runCtx := env.ctx
	if runCtx == nil {
		runCtx = context.Background()
	}
	stdout := env.stdout
	if stdout == nil {
		stdout = io.Discard
	}
	stderr := env.stderr
	if stderr == nil {
		stderr = io.Discard
	}
	getenv := env.getenv
	if getenv == nil {
		getenv = os.Getenv
	}
	geteuid := env.geteuid
	if geteuid == nil {
		geteuid = os.Geteuid
	}
	goos := env.goos
	if goos == "" {
		goos = runtime.GOOS
	}
	cmdRunner := env.cmdRunner
	if cmdRunner == nil {
		cmdRunner = osCommandRunner{}
	}
	discoverProbes := env.discoverProbes
	if discoverProbes == nil {
		discoverProbes = func(vid, pid uint16) ([]usbprobe.ProbeDevice, error) {
			return usbprobe.DiscoverProbesWithID("", vid, pid)
		}
	}

	fs := flag.NewFlagSet("pu", flag.ContinueOnError)
	fs.SetOutput(stderr)
	pidFlag := fs.String("pid", "", "Approved USB PID hex (overrides PU_USB_PID; 0x0000 matches 'Pico Universal*' on 0x2E8A)")
	verFlag := fs.Bool("version", false, "Print pu version and exit")
	fs.Usage = func() {
		printUsage(stderr)
	}

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}

	if *verFlag {
		fmt.Fprintf(stdout, "pu version %s (%s/%s)\n", version, goos, runtime.GOARCH)
		return 0
	}

	rest := fs.Args()
	if len(rest) == 0 {
		printUsage(stderr)
		return 2
	}

	ctx := cliContext{
		runCtx:          runCtx,
		stdin:           env.stdin,
		stdout:          stdout,
		stderr:          stderr,
		getenv:          getenv,
		geteuid:         geteuid,
		goos:            goos,
		cmdRunner:       cmdRunner,
		discoverProbes:  discoverProbes,
		portOpener:      env.portOpener,
		ctrlTransceiver: env.ctrlTransceiver,
		onGDBListen:     env.onGDBListen,
		globalPID:       *pidFlag,
		hasFakeDiscover: env.discoverProbes != nil,
	}

	sub := rest[0]
	subArgs := rest[1:]

	switch sub {
	case "version":
		fmt.Fprintf(stdout, "pu version %s (%s/%s)\n", version, goos, runtime.GOARCH)
		return 0
	case "help", "-h", "--help":
		printUsage(stdout)
		return 0
	case "setup":
		return runSetup(ctx, subArgs)
	case "list":
		return runList(ctx, subArgs)
	default:
		if handler, ok := dispatchHooks[sub]; ok {
			return handler(ctx, subArgs)
		}
		fmt.Fprintf(stderr, "pu: unknown subcommand %q\n\n", sub)
		printUsage(stderr)
		return 2
	}
}

func printUsage(w io.Writer) {
	fmt.Fprintln(w, "Usage: pu [-pid HEX] <subcommand> [options]")
	fmt.Fprintln(w, "")
	fmt.Fprintln(w, "Subcommands:")
	fmt.Fprintln(w, "  setup          Install/verify/uninstall Arduino IDE 2 profiles and USB rules")
	fmt.Fprintln(w, "  list           Discover connected Pico Universal composite USB probes")
	fmt.Fprintln(w, "  check          Non-destructive target MCU & probe pre-flight check")
	fmt.Fprintln(w, "  upload         Atomic firmware upload with readback verification")
	fmt.Fprintln(w, "  gdb-pipe       Cross-platform GDB RSP relay with probe session locking")
	fmt.Fprintln(w, "  recover        Guided CH32V003 power-cycle unbrick / STM32 recovery")
	fmt.Fprintln(w, "  export-vscode  Generate VS Code launch.json and OpenOCD debug configs")
	fmt.Fprintln(w, "  factory-qc     Automated bed-of-nails factory QC runner")
	fmt.Fprintln(w, "  bench          Automated E1-E6 benchmark suite")
	fmt.Fprintln(w, "  version        Print pu version")
}

// ParseHexUint16 parses a 16-bit hexadecimal identifier such as "0x1050", "1050", or "0x0000".
func ParseHexUint16(raw string) (uint16, error) {
	s := strings.TrimSpace(raw)
	if s == "" {
		return 0, errors.New("empty hex uint16 value")
	}
	if strings.HasPrefix(s, "0x") || strings.HasPrefix(s, "0X") {
		s = s[2:]
	}
	s = strings.TrimSuffix(strings.TrimSuffix(s, "u"), "U")
	if len(s) == 0 || len(s) > 4 {
		return 0, fmt.Errorf("invalid 16-bit hex value %q", raw)
	}
	v, err := strconv.ParseUint(s, 16, 16)
	if err != nil {
		return 0, fmt.Errorf("invalid 16-bit hex value %q: %w", raw, err)
	}
	return uint16(v), nil
}

// ResolveUSBPID resolves the USB Product ID from an explicit CLI flag, falling back to
// PU_USB_PID in the environment, or 0x0000 (wildcard "Pico Universal*" on VID 0x2E8A).
func ResolveUSBPID(flagVal string, getenv func(string) string) (uint16, error) {
	if strings.TrimSpace(flagVal) != "" {
		return ParseHexUint16(flagVal)
	}
	if getenv != nil {
		if env := strings.TrimSpace(getenv("PU_USB_PID")); env != "" {
			return ParseHexUint16(env)
		}
	}
	return usbprobe.DefaultPID, nil
}

func runList(ctx cliContext, args []string) int {
	fs := flag.NewFlagSet("pu list", flag.ContinueOnError)
	fs.SetOutput(ctx.stderr)
	serialFlag := fs.String("serial", "", "Filter by probe hardware serial number")
	vidFlag := fs.String("vid", "0x2E8A", "USB Vendor ID in hex")
	pidFlag := fs.String("pid", "", "USB Product ID in hex (overrides PU_USB_PID)")
	jsonFlag := fs.Bool("json", false, "Emit discovered probes as JSON")

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}

	vid, err := ParseHexUint16(*vidFlag)
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu list: invalid --vid: %v\n", err)
		return 2
	}

	effectivePIDFlag := *pidFlag
	if effectivePIDFlag == "" {
		effectivePIDFlag = ctx.globalPID
	}
	pid, err := ResolveUSBPID(effectivePIDFlag, ctx.getenv)
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu list: invalid PID: %v\n", err)
		return 2
	}

	allProbes, err := ctx.discoverProbes(vid, pid)
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu list: discovery failed: %v\n", err)
		return 1
	}
	var probes []usbprobe.ProbeDevice
	wantSerial := strings.TrimSpace(*serialFlag)
	if wantSerial == "" {
		probes = allProbes
	} else {
		for _, p := range allProbes {
			if strings.EqualFold(p.Serial, wantSerial) {
				probes = append(probes, p)
			}
		}
	}

	if *jsonFlag {
		if probes == nil {
			probes = []usbprobe.ProbeDevice{}
		}
		enc := json.NewEncoder(ctx.stdout)
		enc.SetIndent("", "  ")
		if err := enc.Encode(probes); err != nil {
			fmt.Fprintf(ctx.stderr, "pu list: encode json: %v\n", err)
			return 1
		}
		return 0
	}

	if len(probes) == 0 {
		fmt.Fprintln(ctx.stdout, "No Pico Universal probes detected.")
		return 0
	}
	for _, p := range probes {
		fmt.Fprintf(ctx.stdout, "%s\t%04x:%04x\tGDB=%s\tUART=%s\tDAP=%s\n",
			p.Serial, p.VID, p.PID, p.GDBPort, p.UARTPort, p.DAPPath)
	}
	return 0
}
