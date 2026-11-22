package main

import (
	"bytes"
	"errors"
	"flag"
	"fmt"
	"strings"

	"github.com/nfsahsn/pico-universal-programmer/host/internal/catalog"
	"github.com/nfsahsn/pico-universal-programmer/host/internal/usbprobe"
)

func runRecover(ctx cliContext, args []string) int {
	fs := flag.NewFlagSet("pu recover", flag.ContinueOnError)
	fs.SetOutput(ctx.stderr)

	familyFlag := fs.String("family", "", "Target MCU family to recover (ch32v003|stm32f1|stm32f4)")
	serialFlag := fs.String("serial", "", "Select specific Pico Universal probe hardware serial number")
	pidFlag := fs.String("pid", "", "Approved USB PID in hex (overrides PU_USB_PID)")

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}

	family := strings.ToLower(strings.TrimSpace(*familyFlag))
	if family == "ch32v" {
		family = "ch32v003"
	}
	switch family {
	case "ch32v003", "stm32f1", "stm32f4":
	case "":
		fmt.Fprintln(ctx.stderr, "pu recover: --family is required (ch32v003|stm32f1|stm32f4)")
		return 2
	default:
		fmt.Fprintf(ctx.stderr, "pu recover: unsupported recovery family %q (expected ch32v003, stm32f1, or stm32f4)\n", *familyFlag)
		return 2
	}

	probe, err := resolveSingleProbe(ctx, *serialFlag, *pidFlag)
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu recover: %v\n", err)
		return 1
	}

	lk, err := usbprobe.AcquireSessionLock(probe.Serial)
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu recover: %v\n", err)
		return 1
	}
	defer lk.Close()

	if family == "ch32v003" {
		return recoverCH32V003(ctx, probe)
	}
	return recoverSTM32(ctx, probe, family)
}

func recoverCH32V003(ctx cliContext, probe usbprobe.ProbeDevice) int {
	if ctx.ctrlTransceiver != nil || strings.TrimSpace(probe.CtrlPath) != "" {
		status, err := usbprobe.RequestCH32Unbrick(probe, ctx.ctrlTransceiver)
		if err != nil {
			if !errors.Is(err, usbprobe.ErrCtrlUnsupported) {
				fmt.Fprintf(ctx.stderr, "pu recover: IF 5 CH32 unbrick control request failed: %v\n", err)
				return 1
			}
		} else {
			switch status {
			case usbprobe.CtrlStatusOK:
				fmt.Fprintf(ctx.stdout, "CH32V003 hardware VTARG power-cycle + SWIO hold unbrick succeeded on probe %s.\n", probe.Serial)
				fmt.Fprintln(ctx.stdout, "Next step: run 'pu upload --family ch32v003 --file <clean.elf>' FIRST to replace any PD1-remapping firmware, then verify with 'pu check --family ch32v003'.")
				return 0
			case usbprobe.CtrlStatusBusy:
				fmt.Fprintln(ctx.stderr, "pu recover: probe debug engine is busy; close active GDB/OpenOCD sessions and retry")
				return 1
			case usbprobe.CtrlStatusFaultLatched:
				fmt.Fprintln(ctx.stderr, "pu recover: probe VTARG overcurrent fault is latched; inspect target wiring for shorts")
				return 1
			case usbprobe.CtrlStatusUnsupported:
				// Fall through to guided manual power-cycle procedure for PU_BOARD_PICO dev hardware.
			default:
				fmt.Fprintf(ctx.stderr, "pu recover: probe reported CH32 unbrick error status 0x%02x\n", status)
				return 1
			}
		}
	}

	fmt.Fprintf(ctx.stdout, "CH32V003 Guided Power-Cycle Unbrick (Probe %s):\n", probe.Serial)
	fmt.Fprintln(ctx.stdout, "  1. Verify GND is shared and a 1 kΩ pull-up resistor connects SWIO (PD1) to 3.3 V (required on Pico dev board).")
	fmt.Fprintln(ctx.stdout, "  2. Disconnect CH32V003 VDD (3.3 V) for 2 seconds to clear any PD1 GPIO sketch remap or brownout latch.")
	fmt.Fprintln(ctx.stdout, "  3. Reconnect CH32V003 VDD while immediately running 'pu upload --family ch32v003 --file <clean.elf>' FIRST to halt and replace firmware before user code executes.")
	fmt.Fprintln(ctx.stdout, "  4. Verify recovery with 'pu check --family ch32v003'.")
	return 0
}

func buildSTM32RecoverArgs(serial, family, resetCfg string) []string {
	switch family {
	case "stm32f1":
		return []string{
			"-f", "interface/cmsis-dap.cfg",
			"-c", "adapter serial " + serial,
			"-c", "transport select swd",
			"-c", "set CPUTAPID 0",
			"-f", "target/stm32f1x.cfg",
			"-c", "adapter speed 100; " + resetCfg + "; init; reset halt; stm32f1x unlock 0; reset halt; stm32f1x mass_erase 0; reset halt; exit",
		}
	default:
		return []string{
			"-f", "interface/cmsis-dap.cfg",
			"-c", "adapter serial " + serial,
			"-c", "transport select swd",
			"-f", "target/stm32f4x.cfg",
			"-c", "adapter speed 100; " + resetCfg + "; init; reset halt; stm32f2x unlock 0; reset halt; stm32f2x mass_erase 0; reset halt; exit",
		}
	}
}

func checkOpenOCDRecoverFailure(combined string) string {
	lower := strings.ToLower(combined)
	for _, bad := range []string{
		"error:",
		"unlock failed",
		"mass erase failed",
		"failed erasing",
		"target not halted",
		"timed out",
		"unable to find a matching cmsis-dap",
		"swd dp initialization failed",
	} {
		if strings.Contains(lower, bad) {
			return bad
		}
	}
	return ""
}

func recoverSTM32(ctx cliContext, probe usbprobe.ProbeDevice, family string) int {
	ocdBin, err := ctx.cmdRunner.LookPath(catalog.OpenOCDLookupNameForFamily(family))
	if err != nil {
		ocdBin, err = ctx.cmdRunner.LookPath("openocd")
		if err != nil {
			fmt.Fprintf(ctx.stderr, "pu recover: openocd not found in PATH: %v\n", err)
			return 1
		}
	}

	runAttempt := func(resetCfg string) (string, error) {
		var outBuf, errBuf bytes.Buffer
		args := buildSTM32RecoverArgs(probe.Serial, family, resetCfg)
		runErr := ctx.cmdRunner.Run(commandSpec{Name: ocdBin, Args: args}, &outBuf, &errBuf)
		return strings.TrimSpace(outBuf.String() + "\n" + errBuf.String()), runErr
	}

	combined, runErr := runAttempt("reset_config srst_only srst_nogate connect_assert_srst")
	lower := strings.ToLower(combined)
	srstDisconnectedFallback := strings.Contains(lower, "examination succeed") &&
		strings.Contains(lower, "timed out while waiting for target halted") &&
		!strings.Contains(lower, "unlock failed") &&
		!strings.Contains(lower, "mass erase failed")

	usedSysReset := false
	if srstDisconnectedFallback {
		combined, runErr = runAttempt("reset_config none")
		usedSysReset = true
	}

	if runErr != nil {
		fmt.Fprintf(ctx.stderr, "pu recover: %s mass erase failed: %v (%s)\n", family, runErr, combined)
		if strings.Contains(strings.ToLower(combined), "swd dp initialization failed") || strings.Contains(strings.ToLower(combined), "error connecting dp") {
			fmt.Fprintln(ctx.stderr, "Hint: connect Pico GP6 (Pin 9) to STM32 NRST (or hold physical RESET while running 'pu recover') if firmware disabled SWD pins.")
		}
		return 1
	}
	if bad := checkOpenOCDRecoverFailure(combined); bad != "" {
		fmt.Fprintf(ctx.stderr, "pu recover: %s recovery reported OpenOCD error (%q): %s\n", family, bad, combined)
		if strings.Contains(strings.ToLower(combined), "swd dp initialization failed") || strings.Contains(strings.ToLower(combined), "error connecting dp") {
			fmt.Fprintln(ctx.stderr, "Hint: connect Pico GP6 (Pin 9) to STM32 NRST (or hold physical RESET while running 'pu recover') if firmware disabled SWD pins.")
		}
		return 1
	}

	if usedSysReset {
		fmt.Fprintf(ctx.stdout, "Recovered %s target via software SYSRESETREQ unlock + mass_erase on probe %s (NRST not connected).\n", family, probe.Serial)
	} else {
		fmt.Fprintf(ctx.stdout, "Recovered %s target via connect_assert_srst unlock + mass_erase on probe %s.\n", family, probe.Serial)
	}
	fmt.Fprintln(ctx.stdout, "Note: power-cycle target VDD if flash Option Bytes (RDP) were previously locked.")
	return 0
}
