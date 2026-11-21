package main

import (
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"strings"

	"github.com/nfsahsn/pico-universal-programmer/host/internal/bench"
)

func runBench(ctx cliContext, args []string) int {
	fs := flag.NewFlagSet("pu bench", flag.ContinueOnError)
	fs.SetOutput(ctx.stderr)

	expFlag := fs.String("exp", "E1,E2,E3,E5", "Comma-separated experiment IDs to run (E1,E2,E3,E5)")
	probeFlag := fs.String("probe", "pico_universal", "Probe label recorded in CSV datasets")
	targetsFlag := fs.String("targets", "stm32f407,rp2040,esp32,ch32v003", "Comma-separated target identifiers")
	trialsE1 := fs.Int("trials-e1", 30, "Number of flash+verify trials per payload size in E1")
	trialsE2 := fs.Int("trials-e2", 100, "Number of trials per debug operation in E2")
	trialsE3 := fs.Int("trials-e3", 30, "Number of protocol switch trials in E3")
	hilCycles := fs.Int("hil-cycles", 1000, "HIL endurance cycle count in E5")
	outFlag := fs.String("out", "paper/data", "Output directory for CSV datasets and bench_manifest.json")
	dryRun := fs.Bool("dry-run", false, "Run offline synthetic schema self-test without physical bench hardware")
	jsonFlag := fs.Bool("json", false, "Emit run manifest as JSON to stdout")

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}

	var targets []string
	for _, t := range strings.Split(*targetsFlag, ",") {
		if s := strings.TrimSpace(t); s != "" {
			targets = append(targets, s)
		}
	}

	manifest, err := bench.Run(bench.Config{
		Experiments: []string{*expFlag},
		Probe:       *probeFlag,
		Targets:     targets,
		TrialsE1:    *trialsE1,
		TrialsE2:    *trialsE2,
		TrialsE3:    *trialsE3,
		HILCycles:   *hilCycles,
		OutDir:      *outFlag,
		DryRun:      *dryRun,
	})
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu bench: %v\n", err)
		return 1
	}

	if *jsonFlag {
		enc := json.NewEncoder(ctx.stdout)
		enc.SetIndent("", "  ")
		_ = enc.Encode(manifest)
	} else {
		fmt.Fprintf(ctx.stdout, "Bench [%s] complete for probe=%s (%s): wrote %d artifacts in %s\n",
			manifest.Mode, manifest.Probe, strings.Join(manifest.Experiments, ","), len(manifest.FilesWritten), *outFlag)
	}
	return 0
}
