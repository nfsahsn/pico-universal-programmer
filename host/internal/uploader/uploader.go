package uploader

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"

	"github.com/nfsahsn/pico-universal-programmer/host/internal/catalog"
	"github.com/nfsahsn/pico-universal-programmer/host/internal/puid"
	"github.com/nfsahsn/pico-universal-programmer/host/internal/usbprobe"
)

// EspressifSectorSize is the 4 KiB (0x1000) NOR flash erase sector size on ESP32 targets.
const EspressifSectorSize = 0x1000

// Segment describes a binary flash image mapped to a target flash address.
type Segment struct {
	Addr uint32
	Path string
}

// ParseSegmentSpec parses an "0xADDR:PATH" segment specification (preserving colons
// inside Windows drive-letter paths such as "0x8000:C:\build\partitions.bin").
func ParseSegmentSpec(raw string) (Segment, error) {
	s := strings.TrimSpace(raw)
	parts := strings.SplitN(s, ":", 2)
	if len(parts) != 2 {
		return Segment{}, fmt.Errorf("uploader: invalid segment %q (expected 0xADDR:PATH)", raw)
	}
	addrStr := strings.TrimSpace(parts[0])
	pathStr := strings.TrimSpace(parts[1])
	if addrStr == "" || pathStr == "" {
		return Segment{}, fmt.Errorf("uploader: invalid segment %q (empty address or path)", raw)
	}

	var addr64 uint64
	var err error
	if strings.HasPrefix(addrStr, "0x") || strings.HasPrefix(addrStr, "0X") {
		addr64, err = strconv.ParseUint(addrStr[2:], 16, 32)
	} else {
		addr64, err = strconv.ParseUint(addrStr, 0, 32)
	}
	if err != nil {
		return Segment{}, fmt.Errorf("uploader: invalid segment address %q: %w", addrStr, err)
	}
	return Segment{
		Addr: uint32(addr64),
		Path: pathStr,
	}, nil
}

// CommandSpec describes an external OpenOCD or GDB flasher process invocation.
type CommandSpec struct {
	Name string
	Args []string
	Env  []string
}

// CommandRunner abstracts flasher subprocess execution for offline unit testing.
type CommandRunner interface {
	LookPath(file string) (string, error)
	Run(spec CommandSpec, stdout, stderr io.Writer) error
}

// OSCommandRunner executes host processes when no fake runner is provided.
type OSCommandRunner struct{}

func (OSCommandRunner) LookPath(file string) (string, error) {
	return catalog.ResolveHostTool(file)
}

func (OSCommandRunner) Run(spec CommandSpec, stdout, stderr io.Writer) error {
	c := exec.Command(spec.Name, spec.Args...)
	if len(spec.Env) > 0 {
		c.Env = append(os.Environ(), spec.Env...)
	}
	c.Stdout = stdout
	c.Stderr = stderr
	return c.Run()
}

// UploadRequest configures an atomic snapshot firmware upload.
type UploadRequest struct {
	Family       string
	FilePath     string
	BaseAddr     uint32
	Segments     []Segment
	ESP32CfgPath string
	Verbose      bool
	Stdout       io.Writer
	Stderr       io.Writer
}

func newPrivateTempDir(prefix string) (string, func(), error) {
	dir, err := os.MkdirTemp("", prefix)
	if err != nil {
		return "", nil, fmt.Errorf("uploader: create private temp dir: %w", err)
	}
	if err := os.Chmod(dir, 0o700); err != nil {
		_ = os.RemoveAll(dir)
		return "", nil, fmt.Errorf("uploader: chmod 0700 %s: %w", dir, err)
	}
	var once sync.Once
	cleanup := func() {
		once.Do(func() {
			_ = os.RemoveAll(dir)
		})
	}
	return dir, cleanup, nil
}

func copyAndSyncIntoDir(srcPath, dstDir, dstFilename string) (string, error) {
	if strings.TrimSpace(srcPath) == "" {
		return "", errors.New("uploader: empty source path")
	}
	srcFile, err := os.Open(srcPath)
	if err != nil {
		return "", fmt.Errorf("uploader: open %s: %w", srcPath, err)
	}
	defer srcFile.Close()

	info, err := srcFile.Stat()
	if err != nil {
		return "", fmt.Errorf("uploader: stat %s: %w", srcPath, err)
	}
	if info.IsDir() {
		return "", fmt.Errorf("uploader: %s is a directory, expected firmware file", srcPath)
	}
	if !info.Mode().IsRegular() {
		return "", fmt.Errorf("uploader: %s is not a regular file", srcPath)
	}

	dstPath := filepath.Join(dstDir, dstFilename)
	dstFile, err := os.OpenFile(dstPath, os.O_CREATE|os.O_WRONLY|os.O_EXCL, 0o600)
	if err != nil {
		return "", fmt.Errorf("uploader: create snapshot %s: %w", dstPath, err)
	}

	n, copyErr := io.Copy(dstFile, srcFile)
	syncErr := dstFile.Sync()
	closeErr := dstFile.Close()
	if copyErr != nil {
		return "", fmt.Errorf("uploader: copy %s: %w", srcPath, copyErr)
	}
	if syncErr != nil {
		return "", fmt.Errorf("uploader: sync snapshot %s: %w", dstPath, syncErr)
	}
	if closeErr != nil {
		return "", fmt.Errorf("uploader: close snapshot %s: %w", dstPath, closeErr)
	}
	if n <= 0 {
		return "", fmt.Errorf("uploader: firmware file %s is empty (0 bytes)", srcPath)
	}
	return dstPath, nil
}

// CreatePrivateSnapshot copies srcPath into a private 0700 temporary directory and
// fsyncs the copy so concurrent background IDE rebuilds mutating srcPath never corrupt
// an in-flight target upload.
func CreatePrivateSnapshot(srcPath string) (snapPath string, cleanup func(), err error) {
	dir, cleanupFn, err := newPrivateTempDir("pu-snapshot-*")
	if err != nil {
		return "", nil, err
	}
	base := filepath.Base(srcPath)
	if base == "" || base == "." || base == ".." {
		cleanupFn()
		return "", nil, fmt.Errorf("uploader: invalid source path %q", srcPath)
	}
	dst, err := copyAndSyncIntoDir(srcPath, dir, base)
	if err != nil {
		cleanupFn()
		return "", nil, err
	}
	return dst, cleanupFn, nil
}

type sectorSpan struct {
	seg         Segment
	startByte   uint64
	endByteIncl uint64
	firstSector uint64
	lastSector  uint64
}

// ValidateEspressifSegments verifies that every segment file exists, is a non-empty
// regular file, does not wrap uint32 address space, and that no two segments overlap
// in bytes OR share any 4 KiB (0x1000) flash erase sector.
func ValidateEspressifSegments(segs []Segment) error {
	if len(segs) == 0 {
		return errors.New("uploader: no Espressif flash segments provided")
	}

	spans := make([]sectorSpan, 0, len(segs))
	for i, seg := range segs {
		if strings.TrimSpace(seg.Path) == "" {
			return fmt.Errorf("uploader: segment #%d (0x%08X) has empty path", i, seg.Addr)
		}
		info, err := os.Stat(seg.Path)
		if err != nil {
			return fmt.Errorf("uploader: stat segment 0x%08X (%s): %w", seg.Addr, seg.Path, err)
		}
		if info.IsDir() || !info.Mode().IsRegular() {
			return fmt.Errorf("uploader: segment 0x%08X (%s) is not a regular file", seg.Addr, seg.Path)
		}
		if info.Size() <= 0 {
			return fmt.Errorf("uploader: segment 0x%08X (%s) is empty (0 bytes)", seg.Addr, seg.Path)
		}

		size := uint64(info.Size())
		start := uint64(seg.Addr)
		if start+size > math.MaxUint32 {
			return fmt.Errorf("uploader: segment 0x%08X (%s, %d bytes) wraps 32-bit flash address space", seg.Addr, seg.Path, info.Size())
		}
		endIncl := start + size - 1
		spans = append(spans, sectorSpan{
			seg:         seg,
			startByte:   start,
			endByteIncl: endIncl,
			firstSector: start / EspressifSectorSize,
			lastSector:  endIncl / EspressifSectorSize,
		})
	}

	sort.Slice(spans, func(i, j int) bool {
		if spans[i].firstSector != spans[j].firstSector {
			return spans[i].firstSector < spans[j].firstSector
		}
		return spans[i].startByte < spans[j].startByte
	})

	for i := 1; i < len(spans); i++ {
		prev := spans[i-1]
		cur := spans[i]
		if cur.startByte <= prev.endByteIncl {
			return fmt.Errorf("uploader: Espressif segments overlap in bytes: 0x%08X..0x%08X (%s) vs 0x%08X..0x%08X (%s)",
				prev.startByte, prev.endByteIncl, prev.seg.Path,
				cur.startByte, cur.endByteIncl, cur.seg.Path)
		}
		if cur.firstSector <= prev.lastSector {
			sharedSectorAddr := cur.firstSector * EspressifSectorSize
			return fmt.Errorf("uploader: Espressif segments share 4 KiB flash erase sector 0x%08X: 0x%08X..0x%08X (%s) vs 0x%08X..0x%08X (%s)",
				sharedSectorAddr,
				prev.startByte, prev.endByteIncl, prev.seg.Path,
				cur.startByte, cur.endByteIncl, cur.seg.Path)
		}
	}

	return nil
}

func formatGDBSerialTarget(port string) string {
	p := strings.TrimSpace(port)
	upper := strings.ToUpper(p)
	if strings.HasPrefix(upper, "COM") && len(upper) > 4 && !strings.HasPrefix(p, `\\.\`) {
		return `\\.\` + p
	}
	return p
}

func formatOpenOCDBracePath(p string) (string, error) {
	slashed := filepath.ToSlash(p)
	if strings.ContainsAny(slashed, "{}\r\n\x00") {
		return "", fmt.Errorf("uploader: path %q contains characters unsafe for OpenOCD JimTcl braces", p)
	}
	return "{" + slashed + "}", nil
}

func formatGDBQuotedPath(p string) (string, error) {
	slashed := filepath.ToSlash(p)
	if strings.ContainsAny(slashed, "\r\n\x00") {
		return "", fmt.Errorf("uploader: path %q contains control characters unsafe for GDB", p)
	}
	escaped := strings.ReplaceAll(slashed, `\`, `\\`)
	escaped = strings.ReplaceAll(escaped, `"`, `\"`)
	return `"` + escaped + `"`, nil
}

// UploadAtomic locks probe.Serial across processes, snapshots all firmware artifacts
// into a private 0700 temp directory (guaranteed cleanup via defer), validates sector
// safety, flashes, verifies readback, and resets the target.
func UploadAtomic(probe usbprobe.ProbeDevice, req UploadRequest, runner CommandRunner) error {
	family, err := puid.NormalizeFamily(req.Family)
	if err != nil {
		return err
	}
	if strings.TrimSpace(probe.Serial) == "" {
		return errors.New("uploader: probe serial must not be empty")
	}
	if runner == nil {
		runner = OSCommandRunner{}
	}

	hasSegments := len(req.Segments) > 0
	hasFile := strings.TrimSpace(req.FilePath) != ""
	if !hasSegments && !hasFile {
		return errors.New("uploader: missing firmware --file / --elf or --segment inputs")
	}
	if hasSegments && family != "esp32" && family != "esp32s3" {
		return fmt.Errorf("uploader: multi-segment flashing is only supported on esp32/esp32s3 (got %s)", family)
	}
	if (family == "esp32" || family == "esp32s3") && !hasSegments {
		isBin := strings.EqualFold(filepath.Ext(req.FilePath), ".bin")
		if !isBin && req.BaseAddr == 0 {
			return fmt.Errorf(
				"uploader: %s single-ELF --file upload is unsupported without flash segments; pass --segment 0x1000:bootloader.bin --segment 0x8000:partitions.bin --segment 0x10000:app.bin (or a raw .bin with --addr)",
				family,
			)
		}
	}

	lk, err := usbprobe.AcquireSessionLock(probe.Serial)
	if err != nil {
		return err
	}
	defer lk.Close()

	if hasSegments {
		if err := ValidateEspressifSegments(req.Segments); err != nil {
			return err
		}
	}

	snapDir, cleanup, err := newPrivateTempDir("pu-upload-*")
	if err != nil {
		return err
	}
	defer cleanup()

	var snappedFile string
	if hasFile {
		base := filepath.Base(req.FilePath)
		snappedFile, err = copyAndSyncIntoDir(req.FilePath, snapDir, "fw_"+base)
		if err != nil {
			return err
		}
	}

	var snappedSegs []Segment
	if hasSegments {
		snappedSegs = make([]Segment, 0, len(req.Segments))
		for idx, seg := range req.Segments {
			dstName := fmt.Sprintf("seg_%02d_%s", idx, filepath.Base(seg.Path))
			snapSegPath, copyErr := copyAndSyncIntoDir(seg.Path, snapDir, dstName)
			if copyErr != nil {
				return copyErr
			}
			snappedSegs = append(snappedSegs, Segment{
				Addr: seg.Addr,
				Path: snapSegPath,
			})
		}
		// Re-validate frozen snapshots inside private 0700 dir to defeat TOCTOU growth races!
		if err := ValidateEspressifSegments(snappedSegs); err != nil {
			return err
		}
		sort.Slice(snappedSegs, func(i, j int) bool {
			return snappedSegs[i].Addr < snappedSegs[j].Addr
		})
	}

	var capOut, capErr bytes.Buffer
	outWriter := io.Writer(&capOut)
	if req.Verbose && req.Stdout != nil {
		outWriter = io.MultiWriter(req.Stdout, &capOut)
	}
	errWriter := io.Writer(&capErr)
	if req.Verbose && req.Stderr != nil {
		errWriter = io.MultiWriter(req.Stderr, &capErr)
	}

	if family == "ch32v003" {
		if strings.TrimSpace(probe.GDBPort) == "" {
			return fmt.Errorf("uploader: probe %s has no CH32 SWIO GDB CDC port", probe.Serial)
		}
		gdbBin, lookErr := runner.LookPath("riscv-none-elf-gdb")
		if lookErr != nil {
			gdbBin, lookErr = runner.LookPath("riscv-none-embed-gdb")
		}
		if lookErr != nil {
			gdbBin, lookErr = runner.LookPath("gdb-multiarch")
			if lookErr != nil {
				return fmt.Errorf("uploader: riscv-none-elf-gdb not found in PATH: %w", lookErr)
			}
		}

		quotedSnap, qErr := formatGDBQuotedPath(snappedFile)
		if qErr != nil {
			return qErr
		}
		spec := CommandSpec{
			Name: gdbBin,
			Args: []string{
				"-batch",
				"-nx",
				"-ex", "set pagination off",
				"-ex", "set confirm off",
				"-ex", "set remotetimeout 20",
				"-ex", "file " + quotedSnap,
				"-ex", "target remote " + formatGDBSerialTarget(probe.GDBPort),
				"-ex", "monitor reset halt",
				"-ex", "load",
				"-ex", "compare-sections",
				"-ex", "monitor reset",
				"-ex", "detach",
				"-ex", "quit",
			},
		}
		runErr := runner.Run(spec, outWriter, errWriter)
		combined := capOut.String() + "\n" + capErr.String()
		combinedLower := strings.ToLower(combined)
		if runErr != nil {
			return fmt.Errorf("uploader: CH32V003 GDB upload failed: %w (%s)", runErr, strings.TrimSpace(combined))
		}
		gdbErrMarkers := []string{
			"mis-matched",
			"mismatched",
			"error:",
			"load failed",
			"no such file or directory",
			"remote failure reply",
			"cannot access memory",
			"could not open",
			"remote connection closed",
			"ignoring packet error",
			"permission denied",
		}
		for _, marker := range gdbErrMarkers {
			if strings.Contains(combinedLower, marker) {
				return fmt.Errorf("uploader: CH32V003 GDB batch reported failure (%q): %s", marker, strings.TrimSpace(combined))
			}
		}
		if !strings.Contains(combinedLower, "matched") {
			return errors.New("uploader: CH32V003 mandatory compare-sections did not confirm 'matched' readback")
		}
		return nil
	}

	ocdBin, err := runner.LookPath(catalog.OpenOCDLookupNameForFamily(family))
	if err != nil {
		ocdBin, err = runner.LookPath("openocd")
		if err != nil {
			return fmt.Errorf("uploader: openocd not found in PATH: %w", err)
		}
	}

	var args []string
	switch family {
	case "stm32f1", "stm32f4", "rp2040", "rp2350":
		targetCfg := map[string]string{
			"stm32f1": "target/stm32f1x.cfg",
			"stm32f4": "target/stm32f4x.cfg",
			"rp2040":  "target/rp2040.cfg",
			"rp2350":  "target/rp2350.cfg",
		}[family]

		bracedSnap, bErr := formatOpenOCDBracePath(snappedFile)
		if bErr != nil {
			return bErr
		}
		progCmd := fmt.Sprintf(`program %s verify reset exit`, bracedSnap)
		isBin := strings.EqualFold(filepath.Ext(snappedFile), ".bin")
		baseAddr := req.BaseAddr
		if isBin && baseAddr == 0 {
			if family == "rp2040" || family == "rp2350" {
				baseAddr = 0x10000000
			} else {
				baseAddr = 0x08000000
			}
		}
		if baseAddr != 0 {
			progCmd = fmt.Sprintf(`program %s 0x%08x verify reset exit`, bracedSnap, baseAddr)
		}

		swdSpeed := "4000"
		if family == "stm32f1" || family == "stm32f4" {
			swdSpeed = "2000"
		}
		args = []string{
			"-f", "interface/cmsis-dap.cfg",
			"-c", "adapter serial " + probe.Serial,
			"-c", "transport select swd",
			"-c", "adapter speed " + swdSpeed,
			"-f", targetCfg,
		}
		if family == "stm32f4" {
			args = append(args, "-c", `stm32f4x.cpu configure -event reset-init "[stm32f4x.cpu cget -event reset-init]; adapter speed 2000"`)
		}
		args = append(args, "-c", progCmd)

	case "esp32", "esp32s3":
		if family == "esp32" {
			var cfgBytes []byte
			if strings.TrimSpace(req.ESP32CfgPath) != "" {
				cfgBytes, err = os.ReadFile(req.ESP32CfgPath)
				if err != nil {
					return fmt.Errorf("uploader: read %s: %w", req.ESP32CfgPath, err)
				}
			} else {
				cfgBytes = catalog.EmbeddedESP32OpenOCDConfig()
			}
			stagedCfg := filepath.Join(snapDir, "pu-esp32.cfg")
			if err := os.WriteFile(stagedCfg, cfgBytes, 0o600); err != nil {
				return fmt.Errorf("uploader: stage pu-esp32.cfg: %w", err)
			}
			args = []string{
				"-c", "adapter serial " + probe.Serial,
				"-f", filepath.ToSlash(stagedCfg),
			}
		} else {
			args = []string{
				"-f", "interface/cmsis-dap.cfg",
				"-c", "adapter serial " + probe.Serial,
				"-c", "transport select jtag",
				"-c", "adapter speed 5000",
				"-f", "target/esp32s3.cfg",
			}
		}

		if hasSegments {
			cmds := make([]string, 0, len(snappedSegs)+3)
			cmds = append(cmds, "init", "reset halt")
			for _, seg := range snappedSegs {
				bracedSeg, bErr := formatOpenOCDBracePath(seg.Path)
				if bErr != nil {
					return bErr
				}
				cmds = append(cmds, fmt.Sprintf(`program_esp %s 0x%08x verify`, bracedSeg, seg.Addr))
			}
			cmds = append(cmds, "reset run", "exit")
			args = append(args, "-c", strings.Join(cmds, "; "))
		} else {
			bracedSnap, bErr := formatOpenOCDBracePath(snappedFile)
			if bErr != nil {
				return bErr
			}
			addr := req.BaseAddr
			if addr == 0 {
				addr = 0x10000
			}
			args = append(args, "-c", fmt.Sprintf(`program_esp %s 0x%08x verify reset exit`, bracedSnap, addr))
		}
	}

	if err := runner.Run(CommandSpec{Name: ocdBin, Args: args}, outWriter, errWriter); err != nil {
		return fmt.Errorf("uploader: openocd %s upload failed: %w (%s)", family, err, strings.TrimSpace(capErr.String()))
	}
	return nil
}
