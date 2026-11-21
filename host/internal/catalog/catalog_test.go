package catalog

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func repoRoot(t *testing.T) string {
	t.Helper()
	abs, err := filepath.Abs(filepath.Join("..", "..", ".."))
	if err != nil {
		t.Fatalf("resolve repo root: %v", err)
	}
	return abs
}

func TestLoadFromRepoRootAndCatalogDir(t *testing.T) {
	root := repoRoot(t)
	catDir := filepath.Join(root, "catalog")

	for _, p := range []string{root, catDir} {
		tc, lock, err := Load(p)
		if err != nil {
			t.Fatalf("Load(%q) unexpected error: %v", p, err)
		}
		if tc.SchemaVersion != 1 {
			t.Fatalf("expected catalog schema_version 1, got %d", tc.SchemaVersion)
		}
		if lock.SchemaVersion != 1 {
			t.Fatalf("expected lock schema_version 1, got %d", lock.SchemaVersion)
		}
		for _, family := range []string{"stm32", "rp2040", "esp32", "ch32v"} {
			core, ok := tc.FindCore(family)
			if !ok || core.Package == "" || core.Version == "" {
				t.Fatalf("missing or incomplete core %q: %+v", family, core)
			}
		}

		b, ok := tc.FindBoard("bluepill-f103c8")
		if !ok {
			t.Fatal("expected bluepill-f103c8 board")
		}
		if b.Core != "stm32" || b.Profile != "stm32f1" {
			t.Fatalf("unexpected bluepill spec: %+v", b)
		}

		bByFQBN, ok := tc.FindBoardByFQBN("WCH:ch32v:CH32V00x_EVT:pnum=CH32V003F4")
		if !ok || bByFQBN.ID != "ch32v003f4" {
			t.Fatalf("expected ch32v003f4 by FQBN, got %+v (ok=%v)", bByFQBN, ok)
		}

		if _, ok := tc.FindBoard("does-not-exist"); ok {
			t.Fatal("expected false for unknown board id")
		}

		espProf, ok := tc.FindProfile("esp32")
		if !ok || espProf.Transport != "jtag" || espProf.OpenOCDCfg != "catalog/openocd/pu-esp32.cfg" {
			t.Fatalf("unexpected esp32 profile: %+v (ok=%v)", espProf, ok)
		}
		chProf, ok := tc.FindProfile("ch32v003")
		if !ok || chProf.Transport != "swio" || chProf.Engine != "picorvd" {
			t.Fatalf("unexpected ch32v003 profile: %+v (ok=%v)", chProf, ok)
		}
		if _, ok := tc.FindProfile("missing-profile"); ok {
			t.Fatal("expected false for unknown profile")
		}
	}
}

func copyCatalogFixture(t *testing.T) string {
	t.Helper()
	root := repoRoot(t)
	dst := t.TempDir()
	for _, name := range []string{"targets.json", "dependencies.lock.json"} {
		raw, err := os.ReadFile(filepath.Join(root, "catalog", name))
		if err != nil {
			t.Fatalf("read %s: %v", name, err)
		}
		if err := os.WriteFile(filepath.Join(dst, name), raw, 0o600); err != nil {
			t.Fatalf("write %s: %v", name, err)
		}
	}
	return dst
}

func TestLoadRejectsTamperedTargetsHashMismatch(t *testing.T) {
	dir := copyCatalogFixture(t)
	targetsPath := filepath.Join(dir, "targets.json")
	raw, err := os.ReadFile(targetsPath)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(targetsPath, append(raw, '\n'), 0o600); err != nil {
		t.Fatal(err)
	}

	if _, _, err := Load(dir); err == nil || !strings.Contains(err.Error(), "catalog_sha256") {
		t.Fatalf("expected catalog_sha256 mismatch error, got %v", err)
	}
}

func TestLoadRejectsMalformedSHA256InLockfile(t *testing.T) {
	cases := []struct {
		name    string
		replace func(string) string
	}{
		{
			name: "uppercase_artifact_digest",
			replace: func(s string) string {
				return strings.Replace(s,
					"8d1ff26959c4cef55996f25b8403b029234264dc962793cff832e9ddc27e7544",
					"8D1FF26959C4CEF55996F25B8403B029234264DC962793CFF832E9DDC27E7544", 1)
			},
		},
		{
			name: "short_index_digest",
			replace: func(s string) string {
				return strings.Replace(s,
					"f8a5dd560af85b5122b5bd2fb011c83ab7c846a53db01bd6573ed1711e0cde4e",
					"f8a5dd560af85b5122b5bd2fb011c83ab7c846a53db01bd6573ed1711e0cde4", 1)
			},
		},
		{
			name: "non_hex_upstream_digest",
			replace: func(s string) string {
				return strings.Replace(s,
					"3cb9691dc81453f0ad429c721af4aaf87ff18d9431408f4cd75c8bb479419c3e",
					"gcb9691dc81453f0ad429c721af4aaf87ff18d9431408f4cd75c8bb479419c3e", 1)
			},
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			dir := copyCatalogFixture(t)
			lockPath := filepath.Join(dir, "dependencies.lock.json")
			raw, err := os.ReadFile(lockPath)
			if err != nil {
				t.Fatal(err)
			}
			mutated := tc.replace(string(raw))
			if mutated == string(raw) {
				t.Fatal("fixture replacement did not change lockfile")
			}
			if err := os.WriteFile(lockPath, []byte(mutated), 0o600); err != nil {
				t.Fatal(err)
			}
			if _, _, err := Load(dir); err == nil {
				t.Fatalf("expected Load to fail for %s, got nil", tc.name)
			}
		})
	}
}

func TestVerifyFileSHA256(t *testing.T) {
	dir := t.TempDir()
	sample := filepath.Join(dir, "payload.bin")
	payload := []byte("pico-universal-sha256-verification-test")
	if err := os.WriteFile(sample, payload, 0o600); err != nil {
		t.Fatal(err)
	}
	sum := sha256.Sum256(payload)
	validHex := hex.EncodeToString(sum[:])

	if err := VerifyFileSHA256(sample, validHex); err != nil {
		t.Fatalf("expected valid hash to pass, got %v", err)
	}
	if err := VerifyFileSHA256(sample, strings.ToUpper(validHex)); err == nil {
		t.Fatal("expected uppercase digest to be rejected")
	}
	if err := VerifyFileSHA256(sample, "deadbeef"); err == nil {
		t.Fatal("expected short digest to be rejected")
	}
	wrong := strings.Repeat("0", 64)
	if err := VerifyFileSHA256(sample, wrong); err == nil {
		t.Fatal("expected mismatched digest to fail")
	}
	if err := VerifyFileSHA256(filepath.Join(dir, "missing.bin"), validHex); err == nil {
		t.Fatal("expected missing file to fail")
	}
}

func TestEmbeddedCatalogAssetsMatchRepoFilesByteForByte(t *testing.T) {
	root := repoRoot(t)

	checks := []struct {
		relPath  string
		embedded []byte
	}{
		{filepath.Join("catalog", "targets.json"), EmbeddedTargetsJSON()},
		{filepath.Join("catalog", "dependencies.lock.json"), EmbeddedDependenciesLockJSON()},
		{filepath.Join("catalog", "openocd", "pu-esp32.cfg"), EmbeddedESP32OpenOCDConfig()},
	}

	for _, c := range checks {
		want, err := os.ReadFile(filepath.Join(root, c.relPath))
		if err != nil {
			t.Fatalf("read %s: %v", c.relPath, err)
		}
		if string(c.embedded) != string(want) {
			t.Fatalf("embedded asset drifted from repo %s (embedded=%d bytes, repo=%d bytes)", c.relPath, len(c.embedded), len(want))
		}
	}
}

func TestLoadDefaultWorksFromIsolatedWorkingDirectory(t *testing.T) {
	origWD, err := os.Getwd()
	if err != nil {
		t.Fatal(err)
	}
	isolated := t.TempDir()
	if err := os.Chdir(isolated); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = os.Chdir(origWD) })

	tc, lock, err := LoadDefault()
	if err != nil {
		t.Fatalf("LoadDefault() failed in isolated cwd: %v", err)
	}
	if len(tc.Cores) != 4 || len(tc.Profiles) != 7 || len(lock.Artifacts) == 0 {
		t.Fatalf("unexpected embedded catalog contents: cores=%d profiles=%d artifacts=%d", len(tc.Cores), len(tc.Profiles), len(lock.Artifacts))
	}

	tc2, lock2, err := Load("")
	if err != nil {
		t.Fatalf("Load(\"\") failed in isolated cwd: %v", err)
	}
	if lock2.CatalogSHA256 != lock.CatalogSHA256 || len(tc2.Boards) != len(tc.Boards) {
		t.Fatal("Load(\"\") did not match LoadDefault()")
	}
}

func TestValidateLockRejectsEmptyOrPathTraversalFilenames(t *testing.T) {
	badFilenames := []struct {
		name string
		orig string
		bad  string
	}{
		{"empty_artifact_filename", `"filename": "STM32-2.12.0.tar.bz2"`, `"filename": ""`},
		{"dotdot_artifact_filename", `"filename": "STM32-2.12.0.tar.bz2"`, `"filename": "../STM32-2.12.0.tar.bz2"`},
		{"slash_artifact_filename", `"filename": "STM32-2.12.0.tar.bz2"`, `"filename": "sub/STM32-2.12.0.tar.bz2"`},
		{"backslash_artifact_filename", `"filename": "STM32-2.12.0.tar.bz2"`, `"filename": "sub\\STM32-2.12.0.tar.bz2"`},
		{"empty_index_filename", `"filename": "package_index.json"`, `"filename": ""`},
		{"traversal_index_filename", `"filename": "package_index.json"`, `"filename": "../package_index.json"`},
	}

	for _, tc := range badFilenames {
		t.Run(tc.name, func(t *testing.T) {
			dir := copyCatalogFixture(t)
			lockPath := filepath.Join(dir, "dependencies.lock.json")
			raw, err := os.ReadFile(lockPath)
			if err != nil {
				t.Fatal(err)
			}
			mutated := strings.Replace(string(raw), tc.orig, tc.bad, 1)
			if mutated == string(raw) {
				t.Fatalf("failed to mutate fixture for %s", tc.name)
			}
			if err := os.WriteFile(lockPath, []byte(mutated), 0o600); err != nil {
				t.Fatal(err)
			}
			if _, _, err := Load(dir); err == nil {
				t.Fatalf("expected Load to reject %s, got nil", tc.name)
			}
		})
	}
}

func TestLoadRejectsTargetsCoreMissingFromLockfile(t *testing.T) {
	dir := copyCatalogFixture(t)
	lockPath := filepath.Join(dir, "dependencies.lock.json")
	raw, err := os.ReadFile(lockPath)
	if err != nil {
		t.Fatal(err)
	}
	mutated := strings.Replace(string(raw), `"ch32v": {`, `"unlisted_core": {`, 1)
	if mutated == string(raw) {
		t.Fatal("failed to mutate ch32v core key in lockfile")
	}
	if err := os.WriteFile(lockPath, []byte(mutated), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, _, err := Load(dir); err == nil || !strings.Contains(err.Error(), "ch32v") {
		t.Fatalf("expected missing lock core error mentioning ch32v, got %v", err)
	}
}

func TestValidateLockRejectsConflictingDigestsForSameFilename(t *testing.T) {
	dir := copyCatalogFixture(t)
	lockPath := filepath.Join(dir, "dependencies.lock.json")
	raw, err := os.ReadFile(lockPath)
	if err != nil {
		t.Fatal(err)
	}
	// CMSIS-6.2.0.tar.bz2 appears twice (linux-x86_64 and windows-x86_64); mutate only the first SHA-256.
	mutated := strings.Replace(string(raw),
		"e9dcf458a333cda8d4332bb3a4058e8c3de1626c4a567250c60151b590a1d8ce",
		"1111111111111111111111111111111111111111111111111111111111111111", 1)
	if mutated == string(raw) {
		t.Fatal("failed to mutate first CMSIS digest")
	}
	if err := os.WriteFile(lockPath, []byte(mutated), 0o600); err != nil {
		t.Fatal(err)
	}
	if _, _, err := Load(dir); err == nil || !strings.Contains(err.Error(), "CMSIS-6.2.0.tar.bz2") {
		t.Fatalf("expected conflicting filename digest error, got %v", err)
	}
}
