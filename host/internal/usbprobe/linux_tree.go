package usbprobe

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

// ParseLinuxSysfsTree walks a Linux sysfs USB device directory (such as /sys/bus/usb/devices)
// and extracts RawInterfaceNode records. Pure Go and OS-independent so Linux sysfs topology
// correlation can be unit-tested on all host platforms.
func ParseLinuxSysfsTree(sysfsRoot string) ([]RawInterfaceNode, error) {
	entries, err := os.ReadDir(sysfsRoot)
	if err != nil {
		return nil, fmt.Errorf("usbprobe: read sysfs root %s: %w", sysfsRoot, err)
	}

	names := make([]string, 0, len(entries))
	for _, e := range entries {
		names = append(names, e.Name())
	}
	sort.Strings(names)

	var nodes []RawInterfaceNode

	for _, devName := range names {
		// Interface symlinks contain ':'; device directories do not.
		if strings.Contains(devName, ":") {
			continue
		}
		devDir := filepath.Join(sysfsRoot, devName)
		info, err := os.Stat(devDir)
		if err != nil || !info.IsDir() {
			continue
		}

		vid, okVID := readSysfsHexUint16(filepath.Join(devDir, "idVendor"))
		pid, okPID := readSysfsHexUint16(filepath.Join(devDir, "idProduct"))
		if !okVID || !okPID {
			continue
		}

		serial := readSysfsTrimmed(filepath.Join(devDir, "serial"))
		product := readSysfsTrimmed(filepath.Join(devDir, "product"))
		busNum, hasBus := readSysfsDecUint(filepath.Join(devDir, "busnum"))
		devNum, hasDev := readSysfsDecUint(filepath.Join(devDir, "devnum"))

		// Collect interface subdirectories from both <sysfsRoot>/<devName>/<devName>:*
		// and top-level <sysfsRoot>/<devName>:*, deduplicating by interface name.
		ifDirs := make(map[string]string)
		if subEntries, subErr := os.ReadDir(devDir); subErr == nil {
			for _, se := range subEntries {
				if strings.HasPrefix(se.Name(), devName+":") {
					candidate := filepath.Join(devDir, se.Name())
					if st, stErr := os.Stat(candidate); stErr == nil && st.IsDir() {
						ifDirs[se.Name()] = candidate
					}
				}
			}
		}
		for _, topName := range names {
			if strings.HasPrefix(topName, devName+":") {
				if _, exists := ifDirs[topName]; !exists {
					candidate := filepath.Join(sysfsRoot, topName)
					if st, stErr := os.Stat(candidate); stErr == nil && st.IsDir() {
						ifDirs[topName] = candidate
					}
				}
			}
		}

		ifNames := make([]string, 0, len(ifDirs))
		for k := range ifDirs {
			ifNames = append(ifNames, k)
		}
		sort.Strings(ifNames)

		for _, ifName := range ifNames {
			ifDir := ifDirs[ifName]
			ifNum, ok := parseSysfsInterfaceNumber(ifDir, ifName)
			if !ok {
				continue
			}

			var devPath string
			switch ifNum {
			case 0, 5:
				if hasBus && hasDev {
					devPath = fmt.Sprintf("/dev/bus/usb/%03d/%03d", busNum, devNum)
				} else {
					devPath = ifDir
				}
			case 1, 2, 3, 4:
				devPath = findSysfsTTYDevPath(ifDir)
			}

			ifProduct := product
			if ifaceDesc := readSysfsTrimmed(filepath.Join(ifDir, "interface")); ifProduct == "" && ifaceDesc != "" {
				ifProduct = ifaceDesc
			}

			nodes = append(nodes, RawInterfaceNode{
				VID:          vid,
				PID:          pid,
				Serial:       serial,
				Product:      ifProduct,
				InterfaceNum: ifNum,
				DevPath:      devPath,
			})
		}
	}

	return nodes, nil
}

func readSysfsTrimmed(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}

func readSysfsHexUint16(path string) (uint16, bool) {
	s := readSysfsTrimmed(path)
	if s == "" {
		return 0, false
	}
	v, err := strconv.ParseUint(s, 16, 16)
	if err != nil {
		return 0, false
	}
	return uint16(v), true
}

func readSysfsDecUint(path string) (uint64, bool) {
	s := readSysfsTrimmed(path)
	if s == "" {
		return 0, false
	}
	v, err := strconv.ParseUint(s, 10, 32)
	if err != nil {
		return 0, false
	}
	return v, true
}

func parseSysfsInterfaceNumber(ifDir, ifName string) (int, bool) {
	if raw := readSysfsTrimmed(filepath.Join(ifDir, "bInterfaceNumber")); raw != "" {
		if v, err := strconv.ParseUint(raw, 16, 8); err == nil {
			return int(v), true
		}
	}
	// Fallback: parse trailing ".<ifnum>" after ":" (e.g. "1-2:1.3" -> 3)
	colon := strings.LastIndexByte(ifName, ':')
	if colon < 0 {
		return 0, false
	}
	dot := strings.LastIndexByte(ifName[colon+1:], '.')
	if dot < 0 {
		return 0, false
	}
	v, err := strconv.Atoi(ifName[colon+1+dot+1:])
	if err != nil || v < 0 {
		return 0, false
	}
	return v, true
}

func findSysfsTTYDevPath(ifDir string) string {
	isCDCTTYName := func(name string) bool {
		return strings.HasPrefix(name, "ttyACM") || strings.HasPrefix(name, "ttyUSB")
	}

	// Standard modern Linux cdc_acm / usbserial layout: <ifDir>/tty/ttyACM0 or ttyUSB0
	if ttyEntries, err := os.ReadDir(filepath.Join(ifDir, "tty")); err == nil {
		var names []string
		for _, e := range ttyEntries {
			if isCDCTTYName(e.Name()) {
				names = append(names, e.Name())
			}
		}
		sort.Strings(names)
		if len(names) > 0 {
			return "/dev/" + names[0]
		}
	}

	// Legacy layout: <ifDir>/ttyACM0 or ttyUSB0
	if entries, err := os.ReadDir(ifDir); err == nil {
		var names []string
		for _, e := range entries {
			if isCDCTTYName(e.Name()) {
				names = append(names, e.Name())
			}
		}
		sort.Strings(names)
		if len(names) > 0 {
			return "/dev/" + names[0]
		}
	}
	return ""
}
