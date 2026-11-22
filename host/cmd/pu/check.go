package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"strings"

	"github.com/nfsahsn/pico-universal-programmer/host/internal/puid"
	"github.com/nfsahsn/pico-universal-programmer/host/internal/usbprobe"
)

type puidRunnerAdapter struct {
	inner commandRunner
}

func (a puidRunnerAdapter) LookPath(file string) (string, error) {
	return a.inner.LookPath(file)
}

func (a puidRunnerAdapter) Run(spec puid.CommandSpec, stdout, stderr io.Writer) error {
	return a.inner.Run(commandSpec{
		Name: spec.Name,
		Args: spec.Args,
		Env:  spec.Env,
	}, stdout, stderr)
}

func resolveSingleProbe(ctx cliContext, serialFilter, pidFlagVal string) (usbprobe.ProbeDevice, error) {
	effectivePID := strings.TrimSpace(pidFlagVal)
	if effectivePID == "" {
		effectivePID = ctx.globalPID
	}
	pid, err := ResolveUSBPID(effectivePID, ctx.getenv)
	if err != nil {
		return usbprobe.ProbeDevice{}, fmt.Errorf("invalid USB PID: %w", err)
	}

	allProbes, err := ctx.discoverProbes(usbprobe.DefaultVID, pid)
	if err != nil {
		return usbprobe.ProbeDevice{}, fmt.Errorf("probe discovery failed: %w", err)
	}

	wantSerial := strings.TrimSpace(serialFilter)
	var matched []usbprobe.ProbeDevice
	if wantSerial == "" {
		matched = allProbes
	} else {
		for _, p := range allProbes {
			if strings.EqualFold(p.Serial, wantSerial) {
				matched = append(matched, p)
			}
		}
	}

	if len(matched) == 0 {
		if wantSerial != "" {
			return usbprobe.ProbeDevice{}, fmt.Errorf("no Pico Universal probe found with serial %q", wantSerial)
		}
		return usbprobe.ProbeDevice{}, errors.New("no Pico Universal probes detected; connect probe via USB-C or run 'pu list'")
	}
	if len(matched) > 1 {
		serials := make([]string, 0, len(matched))
		for _, p := range matched {
			serials = append(serials, p.Serial)
		}
		return usbprobe.ProbeDevice{}, fmt.Errorf("multiple Pico Universal probes detected (%s); pass --serial <SN> to select one", strings.Join(serials, ", "))
	}
	return matched[0], nil
}

func runCheck(ctx cliContext, args []string) int {
	fs := flag.NewFlagSet("pu check", flag.ContinueOnError)
	fs.SetOutput(ctx.stderr)

	familyFlag := fs.String("family", "", "Target MCU family (stm32f1|stm32f4|rp2040|rp2350|esp32|esp32s3|ch32v003)")
	serialFlag := fs.String("serial", "", "Select specific Pico Universal probe hardware serial number")
	pidFlag := fs.String("pid", "", "Approved USB PID in hex (overrides PU_USB_PID)")
	jsonFlag := fs.Bool("json", false, "Emit structured CheckReport as JSON")

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}

	if strings.TrimSpace(*familyFlag) == "" {
		fmt.Fprintln(ctx.stderr, "pu check: --family is required (stm32f1|stm32f4|rp2040|esp32|ch32v003)")
		return 2
	}
	family, err := puid.NormalizeFamily(*familyFlag)
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu check: %v\n", err)
		return 2
	}

	probe, err := resolveSingleProbe(ctx, *serialFlag, *pidFlag)
	if err != nil {
		if *jsonFlag {
			rep := puid.CheckReport{
				Family:      family,
				ProbeSerial: strings.TrimSpace(*serialFlag),
				Verdict:     "ERR_NO_PROBE",
				Diagnosis:   err.Error(),
				Ready:       false,
				Remediation: []string{
					"Connect Pico Universal via USB-C and verify enumeration with 'pu list'.",
				},
			}
			if raw, jErr := rep.FormatJSON(); jErr == nil {
				_, _ = ctx.stdout.Write(raw)
			}
		} else {
			fmt.Fprintf(ctx.stderr, "pu check: %v\n", err)
		}
		return 1
	}

	report, err := puid.CheckTargetWithOptions(probe, family, puid.CheckOptions{
		Runner:          puidRunnerAdapter{inner: ctx.cmdRunner},
		PortOpener:      ctx.portOpener,
		CtrlTransceiver: ctx.ctrlTransceiver,
	})
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu check: %v\n", err)
		return 1
	}

	if *jsonFlag {
		raw, jErr := report.FormatJSON()
		if jErr != nil {
			fmt.Fprintf(ctx.stderr, "pu check: encode json: %v\n", jErr)
			return 1
		}
		_, _ = ctx.stdout.Write(raw)
	} else {
		_, _ = io.WriteString(ctx.stdout, report.FormatHuman())
	}

	if !report.Ready {
		return 1
	}
	return 0
}
