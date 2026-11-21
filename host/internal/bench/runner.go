package bench

import (
	"encoding/csv"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

// ProbeExecutor executes one timed target operation (flash+verify, debug op, switch, fault)
// and returns elapsed duration in milliseconds, optional metric payload, and error.
type ProbeExecutor interface {
	FlashAndVerify(probe, target string, payloadKiB int, trial int) (elapsedMS float64, verified bool, err error)
	DebugOp(probe, target, op string, trial int) (latencyMS float64, err error)
	SwitchProtocol(probe, fromFamily, toFamily string, uartBaud, trial int) (switchMS float64, uartDrops int, throughputKiBs float64, err error)
	InjectFault(probe, faultClass string, totalTrials int) (recovered int, err error)
}

// DryRunExecutor provides deterministic offline measurements solely for CI pipeline and
// CSV/statistical schema verification (never used for physical paper measurements).
type DryRunExecutor struct{}

func (DryRunExecutor) FlashAndVerify(probe, target string, payloadKiB int, trial int) (float64, bool, error) {
	baseRate := 120.0
	switch target {
	case "ch32v003":
		baseRate = 28.0
	case "esp32":
		baseRate = 95.0
	case "rp2040":
		baseRate = 135.0
	}
	if probe == "jlink" {
		baseRate *= 1.8
	}
	jitter := 1.0 + float64((trial%5)-2)*0.015
	rate := baseRate * jitter
	elapsedMS := (float64(payloadKiB) / rate) * 1000.0
	return elapsedMS, true, nil
}

func (DryRunExecutor) DebugOp(probe, target, op string, trial int) (float64, error) {
	base := 4.2
	switch op {
	case "halt":
		base = 3.1
	case "read_regs":
		base = 5.4
	case "single_step":
		base = 4.6
	case "breakpoint":
		base = 6.2
	case "detach_resume":
		base = 2.8
	}
	return base + float64(trial%7)*0.12, nil
}

func (DryRunExecutor) SwitchProtocol(probe, fromFamily, toFamily string, uartBaud, trial int) (float64, int, float64, error) {
	return 8.5 + float64(trial%4)*0.3, 0, 118.5 - float64(uartBaud)/2_000_000.0*1.2, nil
}

func (DryRunExecutor) InjectFault(probe, faultClass string, totalTrials int) (int, error) {
	return totalTrials, nil
}

// Config controls an automated E1-E6 benchmark run.
type Config struct {
	Experiments []string
	Probe       string
	Targets     []string
	PayloadsKiB []int
	TrialsE1    int
	TrialsE2    int
	TrialsE3    int
	HILCycles   int
	OutDir      string
	DryRun      bool
	Executor    ProbeExecutor
}

// SummaryStats holds descriptive & tail-latency statistics for a series of observations.
type SummaryStats struct {
	N      int     `json:"n"`
	Mean   float64 `json:"mean"`
	SD     float64 `json:"sd"`
	CI95   float64 `json:"ci95"`
	Median float64 `json:"median"`
	P95    float64 `json:"p95"`
	P99    float64 `json:"p99"`
	Min    float64 `json:"min"`
	Max    float64 `json:"max"`
}

// RunManifest records metadata and generated CSV file paths for reproducibility.
type RunManifest struct {
	SchemaVersion string   `json:"schema_version"`
	TimestampUTC  string   `json:"timestamp_utc"`
	Mode          string   `json:"mode"`
	Probe         string   `json:"probe"`
	Targets       []string `json:"targets"`
	Experiments   []string `json:"experiments"`
	FilesWritten  []string `json:"files_written"`
}

// Student-t 95% two-tailed critical values (df=1..30); falls back to z=1.959964 for df>30.
var tCrit95 = [...]float64{
	0, 12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
	2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
	2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042,
}

func percentileSorted(sorted []float64, p float64) float64 {
	n := len(sorted)
	if n == 0 {
		return 0
	}
	if n == 1 {
		return sorted[0]
	}
	pos := p * float64(n-1)
	lo := int(math.Floor(pos))
	hi := int(math.Ceil(pos))
	if lo == hi {
		return sorted[lo]
	}
	frac := pos - float64(lo)
	return sorted[lo]*(1-frac) + sorted[hi]*frac
}

// ComputeStats calculates mean, sample SD, 95% confidence half-width, median, p95, and p99.
func ComputeStats(values []float64) SummaryStats {
	n := len(values)
	if n == 0 {
		return SummaryStats{}
	}
	cp := append([]float64(nil), values...)
	sort.Float64s(cp)

	sum := 0.0
	for _, v := range cp {
		sum += v
	}
	mean := sum / float64(n)

	sd := 0.0
	ci95 := 0.0
	if n > 1 {
		varSum := 0.0
		for _, v := range cp {
			d := v - mean
			varSum += d * d
		}
		sd = math.Sqrt(varSum / float64(n-1))
		df := n - 1
		crit := 1.959964
		if df < len(tCrit95) {
			crit = tCrit95[df]
		}
		ci95 = crit * sd / math.Sqrt(float64(n))
	}

	return SummaryStats{
		N:      n,
		Mean:   mean,
		SD:     sd,
		CI95:   ci95,
		Median: percentileSorted(cp, 0.50),
		P95:    percentileSorted(cp, 0.95),
		P99:    percentileSorted(cp, 0.99),
		Min:    cp[0],
		Max:    cp[n-1],
	}
}

func normalizeExperiments(raw []string) ([]string, error) {
	if len(raw) == 0 {
		return []string{"E1", "E2", "E3", "E5"}, nil
	}
	seen := make(map[string]bool)
	out := make([]string, 0, len(raw))
	for _, item := range raw {
		for _, tok := range strings.Split(item, ",") {
			e := strings.ToUpper(strings.TrimSpace(tok))
			if e == "" {
				continue
			}
			switch e {
			case "E1", "E2", "E3", "E5":
				if !seen[e] {
					seen[e] = true
					out = append(out, e)
				}
			default:
				return nil, fmt.Errorf("bench: unsupported automated experiment %q (expected E1, E2, E3, E5)", e)
			}
		}
	}
	if len(out) == 0 {
		return nil, errors.New("bench: no valid experiments selected")
	}
	return out, nil
}

func writeCSVFile(path string, header []string, rows [][]string) error {
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()

	w := csv.NewWriter(f)
	if err := w.Write(header); err != nil {
		return err
	}
	for _, row := range rows {
		if err := w.Write(row); err != nil {
			return err
		}
	}
	w.Flush()
	return w.Error()
}

// Run executes the configured benchmark suite and writes canonical CSV datasets + bench_manifest.json.
func Run(cfg Config) (RunManifest, error) {
	exps, err := normalizeExperiments(cfg.Experiments)
	if err != nil {
		return RunManifest{}, err
	}
	if strings.TrimSpace(cfg.OutDir) == "" {
		return RunManifest{}, errors.New("bench: OutDir must not be empty")
	}
	if err := os.MkdirAll(cfg.OutDir, 0o755); err != nil {
		return RunManifest{}, fmt.Errorf("bench: create %s: %w", cfg.OutDir, err)
	}

	probe := strings.TrimSpace(cfg.Probe)
	if probe == "" {
		probe = "pico_universal"
	}
	targets := cfg.Targets
	if len(targets) == 0 {
		targets = []string{"stm32f407", "rp2040", "esp32", "ch32v003"}
	}
	payloads := cfg.PayloadsKiB
	if len(payloads) == 0 {
		payloads = []int{4, 16, 64, 256}
	}
	trialsE1 := cfg.TrialsE1
	if trialsE1 <= 0 {
		trialsE1 = 30
	}
	trialsE2 := cfg.TrialsE2
	if trialsE2 <= 0 {
		trialsE2 = 100
	}
	trialsE3 := cfg.TrialsE3
	if trialsE3 <= 0 {
		trialsE3 = 30
	}
	hilCycles := cfg.HILCycles
	if hilCycles <= 0 {
		hilCycles = 1000
	}

	exec := cfg.Executor
	if exec == nil {
		if !cfg.DryRun {
			return RunManifest{}, errors.New("bench: live lab executor not attached; pass --dry-run for schema self-test or attach physical HIL bench harness")
		}
		exec = DryRunExecutor{}
	}

	nowUTC := time.Now().UTC().Format(time.RFC3339)
	var written []string

	for _, exp := range exps {
		switch exp {
		case "E1":
			var rows [][]string
			for _, tgt := range targets {
				for _, kb := range payloads {
					// CH32V003 has 16 KiB flash maximum.
					if tgt == "ch32v003" && kb > 16 {
						continue
					}
					for tr := 0; tr < trialsE1; tr++ {
						elapsedMS, verified, runErr := exec.FlashAndVerify(probe, tgt, kb, tr)
						if runErr != nil {
							return RunManifest{}, fmt.Errorf("bench E1 (%s/%dKiB/trial %d): %w", tgt, kb, tr, runErr)
						}
						if elapsedMS <= 0 {
							return RunManifest{}, fmt.Errorf("bench E1 invalid non-positive elapsedMS=%.3f", elapsedMS)
						}
						throughput := float64(kb) / (elapsedMS / 1000.0)
						rows = append(rows, []string{
							nowUTC,
							probe,
							tgt,
							strconv.Itoa(kb),
							strconv.Itoa(tr),
							fmt.Sprintf("%.3f", elapsedMS),
							fmt.Sprintf("%.3f", throughput),
							strconv.FormatBool(verified),
						})
					}
				}
			}
			p := filepath.Join(cfg.OutDir, "e1_throughput.csv")
			if err := writeCSVFile(p, []string{
				"timestamp_utc", "probe", "target", "payload_kib", "trial", "elapsed_ms", "throughput_kibs", "verified",
			}, rows); err != nil {
				return RunManifest{}, err
			}
			written = append(written, p)

		case "E2":
			ops := []string{"halt", "read_regs", "single_step", "breakpoint", "detach_resume"}
			var rows [][]string
			for _, tgt := range targets {
				for _, op := range ops {
					for tr := 0; tr < trialsE2; tr++ {
						latMS, runErr := exec.DebugOp(probe, tgt, op, tr)
						ok := runErr == nil
						rows = append(rows, []string{
							nowUTC,
							probe,
							tgt,
							op,
							strconv.Itoa(tr),
							fmt.Sprintf("%.3f", latMS),
							strconv.FormatBool(ok),
						})
					}
				}
			}
			p := filepath.Join(cfg.OutDir, "e2_debug_latency.csv")
			if err := writeCSVFile(p, []string{
				"timestamp_utc", "probe", "target", "operation", "trial", "latency_ms", "ok",
			}, rows); err != nil {
				return RunManifest{}, err
			}
			written = append(written, p)

		case "E3":
			pairs := [][2]string{
				{"stm32f407", "esp32"},
				{"esp32", "ch32v003"},
				{"ch32v003", "rp2040"},
				{"rp2040", "stm32f407"},
			}
			bauds := []int{0, 115200, 2000000}
			var rows [][]string
			for _, pair := range pairs {
				for _, baud := range bauds {
					for tr := 0; tr < trialsE3; tr++ {
						swMS, drops, rate, runErr := exec.SwitchProtocol(probe, pair[0], pair[1], baud, tr)
						if runErr != nil {
							return RunManifest{}, fmt.Errorf("bench E3 (%s->%s/%dbps): %w", pair[0], pair[1], baud, runErr)
						}
						rows = append(rows, []string{
							nowUTC,
							probe,
							pair[0],
							pair[1],
							strconv.Itoa(baud),
							strconv.Itoa(tr),
							fmt.Sprintf("%.3f", swMS),
							strconv.Itoa(drops),
							fmt.Sprintf("%.3f", rate),
						})
					}
				}
			}
			p := filepath.Join(cfg.OutDir, "e3_switching_uart.csv")
			if err := writeCSVFile(p, []string{
				"timestamp_utc", "probe", "from_family", "to_family", "uart_baud", "trial", "switch_ms", "uart_rx_drops", "throughput_kibs",
			}, rows); err != nil {
				return RunManifest{}, err
			}
			written = append(written, p)

		case "E5":
			classes := []struct {
				name   string
				trials int
			}{
				{"dap_malformed_fuzz", 50000},
				{"flash_journal_powercut", 4353},
				{"hil_endurance_cycles", hilCycles},
				{"mid_flash_usb_disconnect", 50},
				{"ch32_pd1_powercycle_unbrick", 50},
			}
			var rows [][]string
			for _, c := range classes {
				rec, runErr := exec.InjectFault(probe, c.name, c.trials)
				if runErr != nil {
					return RunManifest{}, fmt.Errorf("bench E5 (%s): %w", c.name, runErr)
				}
				pct := (float64(rec) / float64(c.trials)) * 100.0
				rows = append(rows, []string{
					nowUTC,
					probe,
					c.name,
					strconv.Itoa(c.trials),
					strconv.Itoa(rec),
					fmt.Sprintf("%.2f", pct),
				})
			}
			p := filepath.Join(cfg.OutDir, "e5_fault_injection.csv")
			if err := writeCSVFile(p, []string{
				"timestamp_utc", "probe", "fault_class", "trials_total", "trials_recovered", "recovery_rate_pct",
			}, rows); err != nil {
				return RunManifest{}, err
			}
			written = append(written, p)
		}
	}

	mode := "physical"
	if cfg.DryRun {
		mode = "dry-run"
	}
	manifest := RunManifest{
		SchemaVersion: "1.0.0",
		TimestampUTC:  nowUTC,
		Mode:          mode,
		Probe:         probe,
		Targets:       targets,
		Experiments:   exps,
		FilesWritten:  written,
	}
	manifestPath := filepath.Join(cfg.OutDir, "bench_manifest.json")
	raw, err := json.MarshalIndent(manifest, "", "  ")
	if err != nil {
		return RunManifest{}, err
	}
	if err := os.WriteFile(manifestPath, append(raw, '\n'), 0o644); err != nil {
		return RunManifest{}, err
	}
	manifest.FilesWritten = append(manifest.FilesWritten, manifestPath)
	return manifest, nil
}
