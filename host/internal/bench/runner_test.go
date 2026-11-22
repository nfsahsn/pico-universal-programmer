package bench

import (
	"encoding/csv"
	"os"
	"path/filepath"
	"testing"
)

func TestComputeStatsProducesCI95AndPercentiles(t *testing.T) {
	vals := make([]float64, 30)
	for i := 0; i < 30; i++ {
		vals[i] = 120.0 + float64(i%5)
	}
	st := ComputeStats(vals)
	if st.N != 30 {
		t.Fatalf("expected N=30, got %d", st.N)
	}
	if st.Mean <= 121.5 || st.Mean >= 122.5 {
		t.Fatalf("expected mean ~122.0, got %.4f", st.Mean)
	}
	if st.CI95 <= 0 {
		t.Fatalf("expected positive CI95, got %.4f", st.CI95)
	}
	if st.P99 < st.P95 || st.P95 < st.Median {
		t.Fatalf("percentiles out of order: med=%.2f p95=%.2f p99=%.2f", st.Median, st.P95, st.P99)
	}
}

func TestRunBenchWritesCanonicalCSVsAndManifest(t *testing.T) {
	outDir := t.TempDir()
	manifest, err := Run(Config{
		Experiments: []string{"E1", "E2", "E3", "E5"},
		Probe:       "pico_universal",
		Targets:     []string{"stm32f407", "ch32v003"},
		PayloadsKiB: []int{4, 16, 64},
		TrialsE1:    5,
		TrialsE2:    10,
		TrialsE3:    3,
		HILCycles:   100,
		OutDir:      outDir,
		DryRun:      true,
	})
	if err != nil {
		t.Fatalf("bench.Run failed: %v", err)
	}
	if manifest.Mode != "dry-run" || len(manifest.FilesWritten) != 5 {
		t.Fatalf("unexpected manifest: %+v", manifest)
	}

	// Verify E1 CSV header and row count:
	// stm32f407 gets 3 payloads (4,16,64) * 5 = 15 rows; ch32v003 skips 64 KiB -> 2 * 5 = 10 rows -> 25 data rows.
	f, err := os.Open(filepath.Join(outDir, "e1_throughput.csv"))
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	rows, err := csv.NewReader(f).ReadAll()
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 26 {
		t.Fatalf("expected 1 header + 25 rows in e1_throughput.csv, got %d", len(rows))
	}
	if rows[0][6] != "throughput_kibs" {
		t.Fatalf("unexpected E1 header: %v", rows[0])
	}
}

func TestRunRejectsInvalidExperimentAndMissingLiveExecutor(t *testing.T) {
	if _, err := Run(Config{Experiments: []string{"E99"}, OutDir: t.TempDir(), DryRun: true}); err == nil {
		t.Fatal("expected invalid experiment E99 to fail")
	}
	if _, err := Run(Config{Experiments: []string{"E1"}, OutDir: t.TempDir(), DryRun: false}); err == nil {
		t.Fatal("expected missing live executor when DryRun=false to fail")
	}
}
