package catalog

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
)

func defaultArduinoDataDirs() []string {
	var dirs []string
	if envDir := strings.TrimSpace(os.Getenv("ARDUINO_DIRECTORIES_DATA")); envDir != "" {
		dirs = append(dirs, envDir)
	}
	if runtime.GOOS == "windows" {
		if localAppData := strings.TrimSpace(os.Getenv("LOCALAPPDATA")); localAppData != "" {
			dirs = append(dirs, filepath.Join(localAppData, "Arduino15"))
		}
	}
	if home, err := os.UserHomeDir(); err == nil && home != "" {
		if runtime.GOOS == "darwin" {
			dirs = append(dirs, filepath.Join(home, "Library", "Arduino15"))
		}
		dirs = append(dirs, filepath.Join(home, ".arduino15"))
	}
	return dirs
}

func exeSuffix() string {
	if runtime.GOOS == "windows" {
		return ".exe"
	}
	return ""
}

func isExecutableFile(p string) bool {
	info, err := os.Stat(p)
	if err != nil || info.IsDir() {
		return false
	}
	if runtime.GOOS == "windows" {
		return true
	}
	return info.Mode()&0o111 != 0
}

func findGlobDescending(baseDir string, relPatterns ...string) (string, bool) {
	for _, pat := range relPatterns {
		matches, err := filepath.Glob(filepath.Join(baseDir, pat))
		if err != nil || len(matches) == 0 {
			continue
		}
		sort.Sort(sort.Reverse(sort.StringSlice(matches)))
		for _, m := range matches {
			if isExecutableFile(m) {
				return m, true
			}
		}
	}
	return "", false
}

// ResolveHostTool resolves external flasher/debugger tools from system PATH first
// (except openocd-esp32 which requires Espressif's Xtensa/program_esp fork before
// generic system openocd), falling back to Arduino IDE 2 packages installed by pu setup.
func ResolveHostTool(file string) (string, error) {
	sfx := exeSuffix()
	arduinoDirs := defaultArduinoDataDirs()

	switch file {
	case "openocd-esp32":
		if p, err := exec.LookPath("openocd-esp32"); err == nil {
			return p, nil
		}
		for _, ad := range arduinoDirs {
			if p, ok := findGlobDescending(ad, filepath.Join("packages", "esp32", "tools", "openocd-esp32", "*", "bin", "openocd"+sfx)); ok {
				return p, nil
			}
		}
		return exec.LookPath("openocd")

	case "openocd-rp2040":
		if p, err := exec.LookPath("openocd"); err == nil {
			return p, nil
		}
		for _, ad := range arduinoDirs {
			if p, ok := findGlobDescending(ad,
				filepath.Join("packages", "rp2040", "tools", "pqt-openocd", "*", "bin", "openocd"+sfx),
				filepath.Join("packages", "STMicroelectronics", "tools", "xpack-openocd", "*", "bin", "openocd"+sfx),
			); ok {
				return p, nil
			}
		}
		return "", fmt.Errorf("executable %q not found in PATH or Arduino15 tools", "openocd")

	case "openocd-stm32", "openocd":
		if p, err := exec.LookPath("openocd"); err == nil {
			return p, nil
		}
		for _, ad := range arduinoDirs {
			if p, ok := findGlobDescending(ad,
				filepath.Join("packages", "STMicroelectronics", "tools", "xpack-openocd", "*", "bin", "openocd"+sfx),
				filepath.Join("packages", "rp2040", "tools", "pqt-openocd", "*", "bin", "openocd"+sfx),
				filepath.Join("packages", "WCH", "tools", "openocd", "*", "bin", "openocd"+sfx),
				filepath.Join("packages", "esp32", "tools", "openocd-esp32", "*", "bin", "openocd"+sfx),
			); ok {
				return p, nil
			}
		}
		return "", fmt.Errorf("executable %q not found in PATH or Arduino15 tools", "openocd")

	case "riscv-none-embed-gdb":
		if p, err := exec.LookPath("riscv-none-embed-gdb"); err == nil {
			return p, nil
		}
		for _, ad := range arduinoDirs {
			if p, ok := findGlobDescending(ad,
				filepath.Join("packages", "WCH", "tools", "riscv-none-embed-gcc", "*", "bin", "riscv-none-embed-gdb"+sfx),
			); ok {
				return p, nil
			}
		}
		return "", fmt.Errorf("executable %q not found in PATH or Arduino15 tools", file)

	case "riscv-none-elf-gdb":
		if p, err := exec.LookPath("riscv-none-elf-gdb"); err == nil {
			return p, nil
		}
		for _, ad := range arduinoDirs {
			if p, ok := findGlobDescending(ad,
				filepath.Join("packages", "WCH", "tools", "*", "*", "bin", "riscv-none-elf-gdb"+sfx),
				filepath.Join("packages", "WCH", "tools", "riscv-none-embed-gcc", "*", "bin", "riscv-none-embed-gdb"+sfx),
				filepath.Join("packages", "rp2040", "tools", "pqt-gcc-riscv", "*", "bin", "riscv32-unknown-elf-gdb"+sfx),
			); ok {
				return p, nil
			}
		}
		return "", fmt.Errorf("executable %q not found in PATH or Arduino15 tools", file)

	case "arduino-cli":
		if p, err := exec.LookPath("arduino-cli"); err == nil {
			return p, nil
		}
		if home, err := os.UserHomeDir(); err == nil && home != "" {
			for _, cand := range []string{
				filepath.Join(home, "bin", "arduino-cli"+sfx),
				filepath.Join(home, ".local", "bin", "arduino-cli"+sfx),
			} {
				if isExecutableFile(cand) {
					return cand, nil
				}
			}
		}
		return "", fmt.Errorf("executable %q not found in PATH", file)

	default:
		return exec.LookPath(file)
	}
}

// OpenOCDLookupNameForFamily returns the family-specific OpenOCD lookup hint.
func OpenOCDLookupNameForFamily(family string) string {
	switch strings.ToLower(strings.TrimSpace(family)) {
	case "esp32", "esp32s3", "esp32-s3":
		return "openocd-esp32"
	case "rp2040", "rp2350":
		return "openocd-rp2040"
	case "stm32f1", "stm32f4":
		return "openocd-stm32"
	default:
		return "openocd"
	}
}
