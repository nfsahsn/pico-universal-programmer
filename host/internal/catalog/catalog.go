package catalog

import (
	"crypto/sha256"
	_ "embed"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
)

//go:embed embedded/targets.json
var embeddedTargetsJSON []byte

//go:embed embedded/dependencies.lock.json
var embeddedDependenciesLockJSON []byte

//go:embed embedded/openocd/pu-esp32.cfg
var embeddedESP32OpenOCDConfig []byte

// EmbeddedTargetsJSON returns a defensive copy of the embedded catalog/targets.json bytes.
func EmbeddedTargetsJSON() []byte {
	return append([]byte(nil), embeddedTargetsJSON...)
}

// EmbeddedDependenciesLockJSON returns a defensive copy of the embedded catalog/dependencies.lock.json bytes.
func EmbeddedDependenciesLockJSON() []byte {
	return append([]byte(nil), embeddedDependenciesLockJSON...)
}

// EmbeddedESP32OpenOCDConfig returns a defensive copy of the embedded catalog/openocd/pu-esp32.cfg bytes.
func EmbeddedESP32OpenOCDConfig() []byte {
	return append([]byte(nil), embeddedESP32OpenOCDConfig...)
}

// LoadESP32OpenOCDConfig loads pu-esp32.cfg from catalogDir/openocd/pu-esp32.cfg when
// catalogDir is explicitly specified and contains that file, otherwise returning the
// embedded pu-esp32.cfg bytes.
func LoadESP32OpenOCDConfig(catalogDir string) ([]byte, error) {
	if strings.TrimSpace(catalogDir) != "" {
		catDir, err := ResolveCatalogDir(catalogDir)
		if err != nil {
			return nil, err
		}
		candidate := filepath.Join(catDir, "openocd", "pu-esp32.cfg")
		if info, statErr := os.Stat(candidate); statErr == nil && !info.IsDir() {
			raw, readErr := os.ReadFile(candidate)
			if readErr != nil {
				return nil, fmt.Errorf("catalog: read %s: %w", candidate, readErr)
			}
			return raw, nil
		}
	}
	return EmbeddedESP32OpenOCDConfig(), nil
}

// CoreSpec describes a pinned Arduino core in catalog/targets.json.
type CoreSpec struct {
	Package string `json:"package"`
	Version string `json:"version"`
	Index   string `json:"index"`
	Source  string `json:"source"`
}

// ProfileSpec describes an upload/debug transport profile in catalog/targets.json.
type ProfileSpec struct {
	Engine     string `json:"engine"`
	Mode       string `json:"mode"`
	Transport  string `json:"transport"`
	Target     string `json:"target,omitempty"`
	OpenOCDCfg string `json:"openocd_cfg,omitempty"`
	ToolFamily string `json:"tool_family"`
	SpeedKHz   int    `json:"speed_khz,omitempty"`
}

// BoardSpec describes a target MCU board entry in catalog/targets.json.
type BoardSpec struct {
	ID                string `json:"id"`
	Name              string `json:"name"`
	Core              string `json:"core"`
	FQBN              string `json:"fqbn"`
	Profile           string `json:"profile"`
	Phase             string `json:"phase"`
	HardwareQualified bool   `json:"hardware_qualified"`
}

// IDECoreSpec describes the stock Arduino IDE startup dependency core.
type IDECoreSpec struct {
	Package string `json:"package"`
	Version string `json:"version"`
	Index   string `json:"index"`
	Purpose string `json:"purpose"`
}

// IDEDependencies describes built-in Arduino IDE startup dependencies.
type IDEDependencies struct {
	Core      IDECoreSpec       `json:"core"`
	Libraries map[string]string `json:"libraries"`
}

// TargetsCatalog represents catalog/targets.json.
type TargetsCatalog struct {
	SchemaVersion   int                    `json:"schema_version"`
	Cores           map[string]CoreSpec    `json:"cores"`
	Profiles        map[string]ProfileSpec `json:"profiles"`
	Boards          []BoardSpec            `json:"boards"`
	IDEDependencies IDEDependencies        `json:"ide_dependencies"`
}

// FindBoard looks up a board by its canonical catalog ID.
func (c *TargetsCatalog) FindBoard(id string) (BoardSpec, bool) {
	if c == nil {
		return BoardSpec{}, false
	}
	for _, b := range c.Boards {
		if b.ID == id {
			return b, true
		}
	}
	return BoardSpec{}, false
}

// FindBoardByFQBN looks up a board by its exact Arduino FQBN.
func (c *TargetsCatalog) FindBoardByFQBN(fqbn string) (BoardSpec, bool) {
	if c == nil {
		return BoardSpec{}, false
	}
	for _, b := range c.Boards {
		if b.FQBN == fqbn {
			return b, true
		}
	}
	return BoardSpec{}, false
}

// FindProfile looks up a transport/debug profile by name.
func (c *TargetsCatalog) FindProfile(name string) (ProfileSpec, bool) {
	if c == nil || c.Profiles == nil {
		return ProfileSpec{}, false
	}
	p, ok := c.Profiles[name]
	return p, ok
}

// FindCore looks up a core specification by family key (stm32, rp2040, esp32, ch32v).
func (c *TargetsCatalog) FindCore(name string) (CoreSpec, bool) {
	if c == nil || c.Cores == nil {
		return CoreSpec{}, false
	}
	core, ok := c.Cores[name]
	return core, ok
}

// IndexLock represents a locked package/library index checksum in dependencies.lock.json.
type IndexLock struct {
	URL            string `json:"url"`
	Filename       string `json:"filename"`
	SHA256         string `json:"sha256"`
	UpstreamSHA256 string `json:"upstream_sha256"`
}

// LockedCoreSpec represents a locked core entry in dependencies.lock.json.
type LockedCoreSpec struct {
	Package      string     `json:"package"`
	Version      string     `json:"version"`
	Index        string     `json:"index"`
	Source       string     `json:"source"`
	Artifact     string     `json:"artifact"`
	Dependencies [][]string `json:"dependencies"`
}

// LockedIDECoreSpec represents an IDE startup core in dependencies.lock.json.
type LockedIDECoreSpec struct {
	Package      string     `json:"package"`
	Version      string     `json:"version"`
	Index        string     `json:"index"`
	Purpose      string     `json:"purpose"`
	Artifact     string     `json:"artifact"`
	Dependencies [][]string `json:"dependencies"`
}

// LockedLibrarySpec represents a locked Arduino library in dependencies.lock.json.
type LockedLibrarySpec struct {
	Version  string `json:"version"`
	Artifact string `json:"artifact"`
}

// LockedToolSpec represents a host-platform tool entry in dependencies.lock.json.
type LockedToolSpec struct {
	Packager  string            `json:"packager"`
	Name      string            `json:"name"`
	Version   string            `json:"version"`
	Artifacts map[string]string `json:"artifacts"`
}

// LockedReleaseSpec represents a host release binary (arduino-cli / arduino-ide).
type LockedReleaseSpec struct {
	Version   string            `json:"version"`
	Artifacts map[string]string `json:"artifacts"`
}

// ArtifactLock represents a SHA-256 pinned downloadable archive in dependencies.lock.json.
type ArtifactLock struct {
	URL      string `json:"url"`
	Filename string `json:"filename"`
	SHA256   string `json:"sha256"`
	Size     int64  `json:"size"`
}

// DependenciesLock represents catalog/dependencies.lock.json.
type DependenciesLock struct {
	SchemaVersion int                          `json:"schema_version"`
	CatalogSHA256 string                       `json:"catalog_sha256"`
	Indexes       []IndexLock                  `json:"indexes"`
	Cores         map[string]LockedCoreSpec    `json:"cores"`
	IDECores      map[string]LockedIDECoreSpec `json:"ide_cores"`
	Libraries     map[string]LockedLibrarySpec `json:"libraries"`
	Tools         []LockedToolSpec             `json:"tools"`
	Releases      map[string]LockedReleaseSpec `json:"releases"`
	Artifacts     map[string]ArtifactLock      `json:"artifacts"`
}

// IsValidSHA256Hex returns true iff s is exactly 64 lowercase hexadecimal characters.
func IsValidSHA256Hex(s string) bool {
	if len(s) != 64 {
		return false
	}
	for i := 0; i < len(s); i++ {
		c := s[i]
		if !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) {
			return false
		}
	}
	return true
}

// ComputeFileSHA256 streams path and returns its 64-char lowercase hex SHA-256 digest.
func ComputeFileSHA256(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", fmt.Errorf("catalog: open %s: %w", path, err)
	}
	defer f.Close()

	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", fmt.Errorf("catalog: hash %s: %w", path, err)
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// VerifyFileSHA256 verifies that path matches expectedHex (64 lowercase hex chars).
func VerifyFileSHA256(path, expectedHex string) error {
	if !IsValidSHA256Hex(expectedHex) {
		return fmt.Errorf("catalog: invalid expected SHA-256 hex digest %q (must be 64 lowercase hex chars)", expectedHex)
	}
	actual, err := ComputeFileSHA256(path)
	if err != nil {
		return err
	}
	if actual != expectedHex {
		return fmt.Errorf("catalog: SHA-256 mismatch for %s: got %s, want %s", path, actual, expectedHex)
	}
	return nil
}

// ResolveCatalogDir resolves either a direct catalog directory (containing targets.json)
// or a repository root directory (containing catalog/targets.json). When path is empty,
// common relative locations around cwd and the running executable are searched.
func ResolveCatalogDir(path string) (string, error) {
	var candidates []string
	if path != "" {
		candidates = []string{
			path,
			filepath.Join(path, "catalog"),
		}
	} else {
		candidates = []string{
			"catalog",
			filepath.Join("..", "catalog"),
			filepath.Join("..", "..", "catalog"),
		}
		if exe, err := os.Executable(); err == nil {
			exeDir := filepath.Dir(exe)
			candidates = append(candidates,
				filepath.Join(exeDir, "catalog"),
				filepath.Join(exeDir, "..", "catalog"),
				filepath.Join(exeDir, "..", "..", "catalog"),
			)
		}
	}

	for _, c := range candidates {
		info, err := os.Stat(filepath.Join(c, "targets.json"))
		if err == nil && !info.IsDir() {
			abs, absErr := filepath.Abs(c)
			if absErr != nil {
				return c, nil
			}
			return abs, nil
		}
	}
	if path != "" {
		return "", fmt.Errorf("catalog: targets.json not found under %q", path)
	}
	return "", errors.New("catalog: targets.json not found in default search paths; pass --catalog-dir")
}

// LoadDefault parses and validates the embedded targets.json and dependencies.lock.json
// assets so standalone binaries work from any working directory without external files.
func LoadDefault() (*TargetsCatalog, *DependenciesLock, error) {
	return parseAndValidate(embeddedTargetsJSON, embeddedDependenciesLockJSON, "embedded:targets.json", "embedded:dependencies.lock.json")
}

// Load parses and validates targets.json and dependencies.lock.json from path
// (accepting either the catalog directory or repository root). When path is empty,
// Load delegates to LoadDefault() using the embedded catalog assets.
func Load(path string) (*TargetsCatalog, *DependenciesLock, error) {
	if strings.TrimSpace(path) == "" {
		return LoadDefault()
	}

	catDir, err := ResolveCatalogDir(path)
	if err != nil {
		return nil, nil, err
	}

	targetsPath := filepath.Join(catDir, "targets.json")
	lockPath := filepath.Join(catDir, "dependencies.lock.json")

	targetsRaw, err := os.ReadFile(targetsPath)
	if err != nil {
		return nil, nil, fmt.Errorf("catalog: read %s: %w", targetsPath, err)
	}
	lockRaw, err := os.ReadFile(lockPath)
	if err != nil {
		return nil, nil, fmt.Errorf("catalog: read %s: %w", lockPath, err)
	}

	return parseAndValidate(targetsRaw, lockRaw, targetsPath, lockPath)
}

func parseAndValidate(targetsRaw, lockRaw []byte, targetsLabel, lockLabel string) (*TargetsCatalog, *DependenciesLock, error) {
	var tc TargetsCatalog
	if err := json.Unmarshal(targetsRaw, &tc); err != nil {
		return nil, nil, fmt.Errorf("catalog: parse %s: %w", targetsLabel, err)
	}
	if tc.SchemaVersion != 1 {
		return nil, nil, fmt.Errorf("catalog: unsupported targets.json schema_version %d", tc.SchemaVersion)
	}

	var lock DependenciesLock
	if err := json.Unmarshal(lockRaw, &lock); err != nil {
		return nil, nil, fmt.Errorf("catalog: parse %s: %w", lockLabel, err)
	}
	if lock.SchemaVersion != 1 {
		return nil, nil, fmt.Errorf("catalog: unsupported dependencies.lock.json schema_version %d", lock.SchemaVersion)
	}

	if !IsValidSHA256Hex(lock.CatalogSHA256) {
		return nil, nil, fmt.Errorf("catalog: invalid catalog_sha256 %q in lockfile", lock.CatalogSHA256)
	}
	sum := sha256.Sum256(targetsRaw)
	actualCatalogSHA := hex.EncodeToString(sum[:])
	if actualCatalogSHA != lock.CatalogSHA256 {
		return nil, nil, fmt.Errorf("catalog: catalog_sha256 mismatch: targets.json is %s, lockfile expects %s", actualCatalogSHA, lock.CatalogSHA256)
	}

	if err := validateCatalog(&tc); err != nil {
		return nil, nil, err
	}
	if err := validateLock(&lock); err != nil {
		return nil, nil, err
	}
	if err := validateCatalogAgainstLock(&tc, &lock); err != nil {
		return nil, nil, err
	}
	return &tc, &lock, nil
}

func validateCatalog(tc *TargetsCatalog) error {
	if len(tc.Cores) == 0 {
		return errors.New("catalog: targets.json contains no cores")
	}
	if len(tc.Profiles) == 0 {
		return errors.New("catalog: targets.json contains no profiles")
	}
	if len(tc.Boards) == 0 {
		return errors.New("catalog: targets.json contains no boards")
	}
	for _, b := range tc.Boards {
		if b.ID == "" || b.FQBN == "" {
			return fmt.Errorf("catalog: board %+v missing id or fqbn", b)
		}
		if _, ok := tc.Cores[b.Core]; !ok {
			return fmt.Errorf("catalog: board %q references unknown core %q", b.ID, b.Core)
		}
		if _, ok := tc.Profiles[b.Profile]; !ok {
			return fmt.Errorf("catalog: board %q references unknown profile %q", b.ID, b.Profile)
		}
	}
	return nil
}

func isSafeLockFilename(fn string) bool {
	if fn == "" || fn == "." || fn == ".." {
		return false
	}
	if strings.TrimSpace(fn) != fn {
		return false
	}
	if strings.Contains(fn, "..") || strings.ContainsAny(fn, `/\`) {
		return false
	}
	return true
}

func validateCatalogAgainstLock(tc *TargetsCatalog, lock *DependenciesLock) error {
	coreKeys := make([]string, 0, len(tc.Cores))
	for k := range tc.Cores {
		coreKeys = append(coreKeys, k)
	}
	sort.Strings(coreKeys)

	for _, key := range coreKeys {
		targetCore := tc.Cores[key]
		lockedCore, ok := lock.Cores[key]
		if !ok {
			return fmt.Errorf("catalog: core %q in targets.json is missing from dependencies.lock.json", key)
		}
		if lockedCore.Package != targetCore.Package || lockedCore.Version != targetCore.Version {
			return fmt.Errorf("catalog: core %q lock mismatch: targets.json has %s@%s, lockfile has %s@%s",
				key, targetCore.Package, targetCore.Version, lockedCore.Package, lockedCore.Version)
		}
	}
	return nil
}

func validateLock(lock *DependenciesLock) error {
	for i, idx := range lock.Indexes {
		if !isSafeLockFilename(idx.Filename) {
			return fmt.Errorf("catalog: unsafe or empty filename %q for index #%d", idx.Filename, i)
		}
		if !IsValidSHA256Hex(idx.SHA256) {
			return fmt.Errorf("catalog: invalid sha256 %q for index %q", idx.SHA256, idx.Filename)
		}
		if idx.UpstreamSHA256 != "" && !IsValidSHA256Hex(idx.UpstreamSHA256) {
			return fmt.Errorf("catalog: invalid upstream_sha256 %q for index %q", idx.UpstreamSHA256, idx.Filename)
		}
	}
	if len(lock.Artifacts) == 0 {
		return errors.New("catalog: dependencies.lock.json contains no artifacts")
	}
	artKeys := make([]string, 0, len(lock.Artifacts))
	for id := range lock.Artifacts {
		artKeys = append(artKeys, id)
	}
	sort.Strings(artKeys)

	seenFilenameDigest := make(map[string]string, len(artKeys))
	for _, id := range artKeys {
		art := lock.Artifacts[id]
		if !isSafeLockFilename(art.Filename) {
			return fmt.Errorf("catalog: unsafe or empty filename %q for artifact %q", art.Filename, id)
		}
		if !IsValidSHA256Hex(art.SHA256) {
			return fmt.Errorf("catalog: invalid sha256 %q for artifact %q", art.SHA256, id)
		}
		if art.Size <= 0 {
			return fmt.Errorf("catalog: non-positive size %d for artifact %q", art.Size, id)
		}
		if prevSHA, exists := seenFilenameDigest[art.Filename]; exists && prevSHA != art.SHA256 {
			return fmt.Errorf("catalog: conflicting sha256 digests for artifact filename %q", art.Filename)
		}
		seenFilenameDigest[art.Filename] = art.SHA256
	}
	requireArtifact := func(ref, owner string) error {
		if _, ok := lock.Artifacts[ref]; !ok {
			return fmt.Errorf("catalog: %s references missing artifact %q", owner, ref)
		}
		return nil
	}
	for name, core := range lock.Cores {
		if err := requireArtifact(core.Artifact, "core "+name); err != nil {
			return err
		}
	}
	for name, core := range lock.IDECores {
		if err := requireArtifact(core.Artifact, "ide_core "+name); err != nil {
			return err
		}
	}
	for name, lib := range lock.Libraries {
		if err := requireArtifact(lib.Artifact, "library "+name); err != nil {
			return err
		}
	}
	for _, tool := range lock.Tools {
		for host, ref := range tool.Artifacts {
			if err := requireArtifact(ref, fmt.Sprintf("tool %s/%s (%s)", tool.Packager, tool.Name, host)); err != nil {
				return err
			}
		}
	}
	for name, rel := range lock.Releases {
		for host, ref := range rel.Artifacts {
			if err := requireArtifact(ref, fmt.Sprintf("release %s (%s)", name, host)); err != nil {
				return err
			}
		}
	}
	return nil
}
