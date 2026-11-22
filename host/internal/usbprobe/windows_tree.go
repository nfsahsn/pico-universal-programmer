package usbprobe

import (
	"fmt"
	"strconv"
	"strings"
)

const (
	// CMSISDAPv2WinUSBGUID is the standard ARM CMSIS-DAP v2 DeviceInterfaceGUID (IF 0).
	CMSISDAPv2WinUSBGUID = "{cdb3b5ad-293b-4663-aa36-1aae46463776}"
	// ControlWinUSBGUID is the Pico Universal Control/Telemetry DeviceInterfaceGUID (IF 5).
	ControlWinUSBGUID = "{50554354-2e8a-4d53-9a05-1aae46463777}"
)

// WindowsDevRecord represents a Windows SetupAPI / CfgMgr32 USB PnP device node.
// Kept OS-independent so Windows composite correlation is unit-tested on all platforms.
type WindowsDevRecord struct {
	InstanceID       string // e.g. `USB\VID_2E8A&PID_1050\E660583883278628` or `USB\VID_2E8A&PID_1050&MI_01\7&1a2b3c4d&0&0001`
	ParentInstanceID string // e.g. `USB\VID_2E8A&PID_1050\E660583883278628`
	ParentPrefix     string // optional legacy WDM ParentIdPrefix (e.g. `7&1a2b3c4d&0`)
	Product          string // bus-reported USB product string (DEVPKEY_Device_BusReportedDeviceDesc)
	DeviceDesc       string // OS registry class description (SPDRP_DEVICEDESC), kept separate from Product
	FriendlyName     string // e.g. "USB Serial Device (COM7)"
	PortName         string // e.g. "COM7"
	DevicePath       string // WinUSB symbolic path if queried directly
	Service          string // e.g. "WINUSB", "usbser", "usbccgp"
}

type winParentInfo struct {
	vid     uint16
	pid     uint16
	serial  string
	product string
}

// ParseWindowsDeviceRecords correlates Windows USB composite parent nodes and MI_xx child
// interface nodes into RawInterfaceNode records.
func ParseWindowsDeviceRecords(records []WindowsDevRecord) ([]RawInterfaceNode, error) {
	byInstance := make(map[string]winParentInfo)
	byPrefix := make(map[string]winParentInfo)

	// Pass 1: index composite parent nodes (USB\VID_xxxx&PID_yyyy\<HardwareSerial>).
	for _, r := range records {
		vid, pid, mi, tail, ok := parseWindowsUSBInstanceID(r.InstanceID)
		if !ok || mi >= 0 {
			continue
		}
		// Windows synthesizes '&'-delimited instance tails when a device has no USB serial descriptor.
		if tail == "" || strings.Contains(tail, "&") {
			continue
		}
		parentProd := strings.TrimSpace(r.Product)
		if parentProd == "" && !isGenericWindowsClassDesc(r.DeviceDesc) {
			parentProd = strings.TrimSpace(r.DeviceDesc)
		}
		info := winParentInfo{
			vid:     vid,
			pid:     pid,
			serial:  tail,
			product: parentProd,
		}
		byInstance[strings.ToUpper(strings.TrimSpace(r.InstanceID))] = info
		if pfx := strings.ToUpper(strings.TrimSpace(r.ParentPrefix)); pfx != "" {
			byPrefix[pfx] = info
		}
	}

	// Pass 2: correlate MI_xx interface nodes back to their parent probe's hardware serial.
	var nodes []RawInterfaceNode
	for _, r := range records {
		vid, pid, mi, tail, ok := parseWindowsUSBInstanceID(r.InstanceID)
		if !ok || mi < 0 {
			continue
		}

		var parent winParentInfo
		var foundParent bool

		if parentKey := strings.ToUpper(strings.TrimSpace(r.ParentInstanceID)); parentKey != "" {
			if p, exists := byInstance[parentKey]; exists {
				parent = p
				foundParent = true
			} else if pVID, pPID, pMI, pTail, pOK := parseWindowsUSBInstanceID(r.ParentInstanceID); pOK && pMI < 0 && pTail != "" && !strings.Contains(pTail, "&") {
				parent = winParentInfo{vid: pVID, pid: pPID, serial: pTail}
				foundParent = true
			}
		}

		if !foundParent && tail != "" {
			// WDM child instance tail has form "<ParentPrefix>&<4hexIfNum>", e.g. "7&99887766&0&0001".
			if lastAmp := strings.LastIndexByte(tail, '&'); lastAmp > 0 {
				candidatePrefix := strings.ToUpper(tail[:lastAmp])
				if p, exists := byPrefix[candidatePrefix]; exists && p.vid == vid && p.pid == pid {
					parent = p
					foundParent = true
				}
			}
		}

		if !foundParent || parent.serial == "" {
			continue
		}

		product := resolveWindowsChildProduct(r.Product, r.DeviceDesc, parent.product)

		devPath := resolveWindowsDevPath(r, vid, pid, mi, tail)
		nodes = append(nodes, RawInterfaceNode{
			VID:          vid,
			PID:          pid,
			Serial:       parent.serial,
			Product:      product,
			InterfaceNum: mi,
			DevPath:      devPath,
		})
	}

	return nodes, nil
}

func isGenericWindowsClassDesc(desc string) bool {
	trimmed := strings.TrimSpace(desc)
	if trimmed == "" {
		return true
	}
	return strings.EqualFold(trimmed, "USB Serial Device") ||
		strings.EqualFold(trimmed, "WinUsb Device") ||
		strings.EqualFold(trimmed, "USB Composite Device")
}

func resolveWindowsChildProduct(childBusDesc, childDevDesc, parentProduct string) string {
	child := strings.TrimSpace(childBusDesc)
	parent := strings.TrimSpace(parentProduct)
	if isGenericWindowsClassDesc(child) {
		if parent != "" {
			return parent
		}
		if !isGenericWindowsClassDesc(childDevDesc) {
			return strings.TrimSpace(childDevDesc)
		}
		return child
	}
	if strings.HasPrefix(parent, ProductPrefix) && !strings.HasPrefix(child, ProductPrefix) {
		return parent
	}
	return child
}

// parseWindowsUSBInstanceID parses `USB\VID_2E8A&PID_1050\SERIAL` (mi = -1)
// or `USB\VID_2E8A&PID_1050&MI_01\7&1a2b3c4d&0&0001` (mi = 1).
func parseWindowsUSBInstanceID(id string) (vid, pid uint16, mi int, tail string, ok bool) {
	trimmed := strings.TrimSpace(id)
	parts := strings.Split(trimmed, `\`)
	if len(parts) != 3 || !strings.EqualFold(parts[0], "USB") {
		return 0, 0, -1, "", false
	}

	mi = -1
	var haveVID, havePID bool
	for _, tok := range strings.Split(parts[1], "&") {
		upper := strings.ToUpper(tok)
		switch {
		case strings.HasPrefix(upper, "VID_") && len(upper) == 8:
			v, err := strconv.ParseUint(upper[4:], 16, 16)
			if err != nil {
				return 0, 0, -1, "", false
			}
			vid = uint16(v)
			haveVID = true
		case strings.HasPrefix(upper, "PID_") && len(upper) == 8:
			v, err := strconv.ParseUint(upper[4:], 16, 16)
			if err != nil {
				return 0, 0, -1, "", false
			}
			pid = uint16(v)
			havePID = true
		case strings.HasPrefix(upper, "MI_") && len(upper) == 5:
			v, err := strconv.ParseUint(upper[3:], 16, 8)
			if err != nil {
				return 0, 0, -1, "", false
			}
			mi = int(v)
		}
	}
	if !haveVID || !havePID {
		return 0, 0, -1, "", false
	}
	return vid, pid, mi, strings.TrimSpace(parts[2]), true
}

func resolveWindowsDevPath(r WindowsDevRecord, vid, pid uint16, mi int, tail string) string {
	switch mi {
	case 0, 5:
		if dp := strings.TrimSpace(r.DevicePath); dp != "" {
			return dp
		}
		if !strings.EqualFold(strings.TrimSpace(r.Service), "WINUSB") {
			return ""
		}
		safeTail := strings.ToLower(strings.ReplaceAll(tail, `\`, "#"))
		if safeTail == "" {
			return ""
		}
		if mi == 0 {
			return fmt.Sprintf(`\\?\usb#vid_%04x&pid_%04x&mi_00#%s#%s`, vid, pid, safeTail, CMSISDAPv2WinUSBGUID)
		}
		return fmt.Sprintf(`\\?\usb#vid_%04x&pid_%04x&mi_05#%s#%s`, vid, pid, safeTail, ControlWinUSBGUID)
	case 1, 2, 3, 4:
		if port := strings.TrimSpace(r.PortName); isCDCSerialDevPath(port) {
			return port
		}
		if com := extractCOMFromFriendlyName(r.FriendlyName); com != "" {
			return com
		}
		return ""
	default:
		return ""
	}
}

func extractCOMFromFriendlyName(friendly string) string {
	upper := strings.ToUpper(friendly)
	idx := strings.LastIndex(upper, "(COM")
	if idx < 0 {
		return ""
	}
	rest := upper[idx+4:]
	end := strings.IndexByte(rest, ')')
	if end <= 0 {
		return ""
	}
	digits := rest[:end]
	for i := 0; i < len(digits); i++ {
		if digits[i] < '0' || digits[i] > '9' {
			return ""
		}
	}
	return "COM" + digits
}
