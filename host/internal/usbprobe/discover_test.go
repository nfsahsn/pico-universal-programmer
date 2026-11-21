package usbprobe

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestCorrelateNodesSortsBySerialAndMapsInterfaces(t *testing.T) {
	// Deliberately out-of-order serials and mixed interface order.
	nodes := []RawInterfaceNode{
		{VID: 0x2E8A, PID: 0x1234, Serial: "E660583883999999", InterfaceNum: 3, DevPath: "/dev/ttyACM3"},
		{VID: 0x2E8A, PID: 0x1234, Serial: "E660583883278628", InterfaceNum: 5, DevPath: "/dev/bus/usb/001/004:1.5"},
		{VID: 0x2E8A, PID: 0x1234, Serial: "E660583883278628", InterfaceNum: 1, DevPath: "/dev/ttyACM0"},
		{VID: 0x2E8A, PID: 0x1234, Serial: "E660583883999999", InterfaceNum: 1, DevPath: "/dev/ttyACM2"},
		{VID: 0x2E8A, PID: 0x1234, Serial: "E660583883278628", InterfaceNum: 3, DevPath: "/dev/ttyACM1"},
		{VID: 0x2E8A, PID: 0x1234, Serial: "E660583883278628", InterfaceNum: 0, DevPath: "/dev/bus/usb/001/004:1.0"},
	}

	probes := CorrelateNodes(nodes, 0x2E8A, 0x1234)
	if len(probes) != 2 {
		t.Fatalf("expected 2 probes, got %d: %+v", len(probes), probes)
	}
	if probes[0].Serial != "E660583883278628" || probes[1].Serial != "E660583883999999" {
		t.Fatalf("probes not sorted by Serial: %+v", probes)
	}
	p0 := probes[0]
	if p0.DAPPath != "/dev/bus/usb/001/004:1.0" ||
		p0.GDBPort != "/dev/ttyACM0" ||
		p0.UARTPort != "/dev/ttyACM1" ||
		p0.CtrlPath != "/dev/bus/usb/001/004:1.5" {
		t.Fatalf("unexpected composite mapping for probe 0: %+v", p0)
	}
	p1 := probes[1]
	if p1.GDBPort != "/dev/ttyACM2" || p1.UARTPort != "/dev/ttyACM3" {
		t.Fatalf("unexpected composite mapping for probe 1: %+v", p1)
	}
}

func TestCorrelateNodesWildcardPIDZeroVsStrictPID(t *testing.T) {
	nodes := []RawInterfaceNode{
		// Probe A: dev PID 0x000C with "Pico Universal CMSIS-DAP"
		{VID: 0x2E8A, PID: 0x000C, Serial: "E66000000000000A", Product: "Pico Universal CMSIS-DAP", InterfaceNum: 0, DevPath: "usb0"},
		{VID: 0x2E8A, PID: 0x000C, Serial: "E66000000000000A", Product: "Pico Universal CMSIS-DAP", InterfaceNum: 1, DevPath: "/dev/ttyACM0"},
		// Probe B: approved PID 0x1050 with "Pico Universal Pro"
		{VID: 0x2E8A, PID: 0x1050, Serial: "E66000000000000B", Product: "Pico Universal Pro", InterfaceNum: 0, DevPath: "usb1"},
		// Unrelated RP2040 Arduino sketch on VID 0x2E8A
		{VID: 0x2E8A, PID: 0x000A, Serial: "E66000000000000C", Product: "Raspberry Pi Pico", InterfaceNum: 0, DevPath: "/dev/ttyACM9"},
		// Unrelated vendor spoofing product prefix
		{VID: 0x0483, PID: 0x3748, Serial: "E66000000000000D", Product: "Pico Universal Clone", InterfaceNum: 0, DevPath: "usb2"},
	}

	// pid == 0: matches any "Pico Universal*" on VID 0x2E8A (both Probe A and Probe B)
	wildcard := CorrelateNodes(nodes, 0x2E8A, 0x0000)
	if len(wildcard) != 2 {
		t.Fatalf("expected 2 wildcard Pico Universal probes, got %d: %+v", len(wildcard), wildcard)
	}
	if wildcard[0].Serial != "E66000000000000A" || wildcard[1].Serial != "E66000000000000B" {
		t.Fatalf("unexpected wildcard serials: %+v", wildcard)
	}

	// pid == 0x1050: strict exact VID+PID match (ONLY Probe B, rejecting 0x000C!)
	strict := CorrelateNodes(nodes, 0x2E8A, 0x1050)
	if len(strict) != 1 || strict[0].Serial != "E66000000000000B" || strict[0].PID != 0x1050 {
		t.Fatalf("expected only exact PID 0x1050 probe, got %+v", strict)
	}
}

func writeSysfsFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatal(err)
	}
}

func TestParseLinuxSysfsTreeMultiProbeAndSymlinkDedupe(t *testing.T) {
	root := t.TempDir()
	realDevs := filepath.Join(t.TempDir(), "pci0000:00", "usb1")
	busDevs := filepath.Join(root, "sys", "bus", "usb", "devices")
	if err := os.MkdirAll(busDevs, 0o755); err != nil {
		t.Fatal(err)
	}

	// Build two composite Pico Universal probes + one unrelated USB HID device.
	createSysfsProbe := func(busPort, devNum, serial, ttyGDB, ttyUART string) {
		devDir := filepath.Join(realDevs, busPort)
		writeSysfsFile(t, filepath.Join(devDir, "idVendor"), "2e8a\n")
		writeSysfsFile(t, filepath.Join(devDir, "idProduct"), "1050\n")
		writeSysfsFile(t, filepath.Join(devDir, "serial"), serial+"\n")
		writeSysfsFile(t, filepath.Join(devDir, "product"), "Pico Universal CMSIS-DAP\n")
		writeSysfsFile(t, filepath.Join(devDir, "busnum"), "1\n")
		writeSysfsFile(t, filepath.Join(devDir, "devnum"), devNum+"\n")

		for _, itf := range []struct {
			num string
			hex string
			tty string
		}{
			{"0", "00", ""},
			{"1", "01", ttyGDB},
			{"2", "02", ""},
			{"3", "03", ttyUART},
			{"4", "04", ""},
			{"5", "05", ""},
		} {
			ifName := busPort + ":1." + itf.num
			ifDir := filepath.Join(devDir, ifName)
			writeSysfsFile(t, filepath.Join(ifDir, "bInterfaceNumber"), itf.hex+"\n")
			if itf.tty != "" {
				if err := os.MkdirAll(filepath.Join(ifDir, "tty", itf.tty), 0o755); err != nil {
					t.Fatal(err)
				}
			}
			// Mirror real /sys/bus/usb/devices where both <dev> and <dev>:1.x are top-level symlinks.
			if err := os.Symlink(ifDir, filepath.Join(busDevs, ifName)); err != nil {
				t.Fatal(err)
			}
		}
		if err := os.Symlink(devDir, filepath.Join(busDevs, busPort)); err != nil {
			t.Fatal(err)
		}
	}

	createSysfsProbe("1-4", "7", "E660583883BBBBBB", "ttyACM2", "ttyACM3")
	createSysfsProbe("1-2", "4", "E660583883AAAAAA", "ttyACM0", "ttyACM1")

	// Unrelated USB mouse
	mouseDir := filepath.Join(busDevs, "1-9")
	writeSysfsFile(t, filepath.Join(mouseDir, "idVendor"), "046d\n")
	writeSysfsFile(t, filepath.Join(mouseDir, "idProduct"), "c52b\n")
	writeSysfsFile(t, filepath.Join(mouseDir, "product"), "USB Receiver\n")
	writeSysfsFile(t, filepath.Join(mouseDir, "1-9:1.0", "bInterfaceNumber"), "00\n")

	nodes, err := ParseLinuxSysfsTree(busDevs)
	if err != nil {
		t.Fatalf("ParseLinuxSysfsTree error: %v", err)
	}

	probes := CorrelateNodes(nodes, 0x2E8A, 0x0000)
	if len(probes) != 2 {
		t.Fatalf("expected 2 correlated Linux probes, got %d: %+v", len(probes), probes)
	}
	if probes[0].Serial != "E660583883AAAAAA" ||
		probes[0].GDBPort != "/dev/ttyACM0" ||
		probes[0].UARTPort != "/dev/ttyACM1" ||
		probes[0].DAPPath == "" ||
		probes[0].CtrlPath == "" {
		t.Fatalf("unexpected probe[0]: %+v", probes[0])
	}
	if probes[1].Serial != "E660583883BBBBBB" ||
		probes[1].GDBPort != "/dev/ttyACM2" ||
		probes[1].UARTPort != "/dev/ttyACM3" {
		t.Fatalf("unexpected probe[1]: %+v", probes[1])
	}
}

func TestParseWindowsDeviceRecordsMultiProbeCorrelation(t *testing.T) {
	records := []WindowsDevRecord{
		// Probe 2 (listed first to verify deterministic Serial sort)
		{
			InstanceID:   `USB\VID_2E8A&PID_1050\E660583883222222`,
			Product:      "Pico Universal CMSIS-DAP",
			ParentPrefix: "7&99887766&0",
			Service:      "usbccgp",
		},
		{
			InstanceID: `USB\VID_2E8A&PID_1050&MI_00\7&99887766&0&0000`,
			Service:    "WINUSB",
		},
		{
			InstanceID:   `USB\VID_2E8A&PID_1050&MI_01\7&99887766&0&0001`,
			FriendlyName: "USB Serial Device (COM11)",
			Service:      "usbser",
		},
		{
			InstanceID: `USB\VID_2E8A&PID_1050&MI_03\7&99887766&0&0003`,
			PortName:   "COM12",
			Service:    "usbser",
		},
		{
			InstanceID: `USB\VID_2E8A&PID_1050&MI_05\7&99887766&0&0005`,
			Service:    "WINUSB",
		},

		// Probe 1 (correlated via ParentInstanceID)
		{
			InstanceID: `USB\VID_2E8A&PID_1050\E660583883111111`,
			Product:    "Pico Universal CMSIS-DAP",
			Service:    "usbccgp",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_00\6&11223344&0&0000`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883111111`,
			DevicePath:       `\\?\usb#vid_2e8a&pid_1050&mi_00#6&11223344&0&0000#{cdb3b5ad-293b-4663-aa36-1aae46463776}`,
			Service:          "WINUSB",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_01\6&11223344&0&0001`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883111111`,
			PortName:         "COM7",
			Service:          "usbser",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_03\6&11223344&0&0003`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883111111`,
			PortName:         "COM8",
			Service:          "usbser",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_05\6&11223344&0&0005`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883111111`,
			Service:          "WINUSB",
		},

		// Unserialized Windows port-generated ID (contains '&' in tail — NOT a hardware serial)
		{
			InstanceID: `USB\VID_2E8A&PID_0003\6&deadbeef&0&3`,
			Product:    "RP2 Boot",
		},
	}

	nodes, err := ParseWindowsDeviceRecords(records)
	if err != nil {
		t.Fatalf("ParseWindowsDeviceRecords error: %v", err)
	}
	probes := CorrelateNodes(nodes, 0x2E8A, 0x1050)
	if len(probes) != 2 {
		t.Fatalf("expected 2 correlated Windows probes, got %d: %+v", len(probes), probes)
	}

	p0 := probes[0]
	if p0.Serial != "E660583883111111" || p0.GDBPort != "COM7" || p0.UARTPort != "COM8" {
		t.Fatalf("unexpected Windows probe[0]: %+v", p0)
	}
	if !strings.Contains(strings.ToLower(p0.DAPPath), "cdb3b5ad-293b-4663-aa36-1aae46463776") {
		t.Fatalf("expected CMSIS-DAP v2 WinUSB GUID in DAPPath, got %q", p0.DAPPath)
	}
	if !strings.Contains(strings.ToLower(p0.CtrlPath), "50554354-2e8a-4d53-9a05-1aae46463777") {
		t.Fatalf("expected Control WinUSB GUID in CtrlPath, got %q", p0.CtrlPath)
	}

	p1 := probes[1]
	if p1.Serial != "E660583883222222" || p1.GDBPort != "COM11" || p1.UARTPort != "COM12" {
		t.Fatalf("unexpected Windows probe[1] (ParentPrefix + FriendlyName fallback): %+v", p1)
	}
}

func TestDiscoverProbesRunsCleanlyOnHost(t *testing.T) {
	t.Setenv("PU_USB_PID", "")
	probes, err := DiscoverProbes("NONEXISTENT_SERIAL_FILTER")
	if err != nil {
		t.Fatalf("DiscoverProbes returned unexpected error: %v", err)
	}
	if len(probes) != 0 {
		t.Fatalf("expected 0 probes for nonexistent serial filter, got %+v", probes)
	}

	t.Setenv("PU_USB_PID", "0x1050")
	probes, err = DiscoverProbes("NONEXISTENT_SERIAL_FILTER")
	if err != nil {
		t.Fatalf("DiscoverProbes with PU_USB_PID=0x1050 returned unexpected error: %v", err)
	}
	if len(probes) != 0 {
		t.Fatalf("expected 0 probes for nonexistent serial filter, got %+v", probes)
	}

	t.Setenv("PU_USB_PID", "0xZZZZ")
	if _, err := DiscoverProbes("NONEXISTENT_SERIAL_FILTER"); err == nil {
		t.Fatal("expected DiscoverProbes to fail on invalid PU_USB_PID")
	}

	probes, err = DiscoverProbesWithID("NONEXISTENT_SERIAL_FILTER", 0x2E8A, 0x0000)
	if err != nil {
		t.Fatalf("DiscoverProbesWithID returned unexpected error: %v", err)
	}
	if len(probes) != 0 {
		t.Fatalf("expected 0 probes for nonexistent serial filter, got %+v", probes)
	}
}

func TestWindowsWildcardPIDZeroWithGenericChildProductFallback(t *testing.T) {
	records := []WindowsDevRecord{
		{
			InstanceID: `USB\VID_2E8A&PID_1050\E660583883111111`,
			Product:    "Pico Universal CMSIS-DAP",
			DeviceDesc: "USB Composite Device",
			Service:    "usbccgp",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_00\6&11223344&0&0000`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883111111`,
			Product:          "WinUsb Device",
			DeviceDesc:       "WinUsb Device",
			Service:          "WINUSB",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_01\6&11223344&0&0001`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883111111`,
			Product:          "USB Serial Device",
			DeviceDesc:       "USB Serial Device",
			PortName:         "COM7",
			Service:          "usbser",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_03\6&11223344&0&0003`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883111111`,
			Product:          "USB Serial Device",
			DeviceDesc:       "USB Serial Device",
			PortName:         "COM8",
			Service:          "usbser",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_05\6&11223344&0&0005`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883111111`,
			Product:          "",
			DeviceDesc:       "WinUsb Device",
			Service:          "WINUSB",
		},
	}

	nodes, err := ParseWindowsDeviceRecords(records)
	if err != nil {
		t.Fatalf("ParseWindowsDeviceRecords error: %v", err)
	}
	probes := CorrelateNodes(nodes, 0x2E8A, 0x0000)
	if len(probes) != 1 {
		t.Fatalf("expected 1 wildcard Windows probe when child Product is generic OS fallback, got %d: %+v", len(probes), probes)
	}
	p := probes[0]
	if p.Serial != "E660583883111111" || p.Product != "Pico Universal CMSIS-DAP" || p.GDBPort != "COM7" || p.UARTPort != "COM8" {
		t.Fatalf("unexpected correlated Windows probe: %+v", p)
	}
	if p.DAPPath == "" || p.CtrlPath == "" {
		t.Fatalf("expected WinUSB DAPPath and CtrlPath, got %+v", p)
	}
}

func TestLinuxSysfsUnboundCDCAndIF2IF4Fallback(t *testing.T) {
	busDevs := filepath.Join(t.TempDir(), "sys", "bus", "usb", "devices")
	if err := os.MkdirAll(busDevs, 0o755); err != nil {
		t.Fatal(err)
	}

	// Probe A: IF 1 unbound, IF 2 bound to ttyACM0; IF 3 & IF 4 both unbound.
	devA := filepath.Join(busDevs, "1-1")
	writeSysfsFile(t, filepath.Join(devA, "idVendor"), "2e8a\n")
	writeSysfsFile(t, filepath.Join(devA, "idProduct"), "1050\n")
	writeSysfsFile(t, filepath.Join(devA, "serial"), "E660583883000001\n")
	writeSysfsFile(t, filepath.Join(devA, "product"), "Pico Universal CMSIS-DAP\n")
	writeSysfsFile(t, filepath.Join(devA, "busnum"), "1\n")
	writeSysfsFile(t, filepath.Join(devA, "devnum"), "4\n")
	for _, itf := range []struct {
		num string
		hex string
		tty string
	}{
		{"0", "00", ""},
		{"1", "01", ""},
		{"2", "02", "ttyACM0"},
		{"3", "03", ""},
		{"4", "04", ""},
		{"5", "05", ""},
	} {
		ifDir := filepath.Join(devA, "1-1:1."+itf.num)
		writeSysfsFile(t, filepath.Join(ifDir, "bInterfaceNumber"), itf.hex+"\n")
		if itf.tty != "" {
			if err := os.MkdirAll(filepath.Join(ifDir, "tty", itf.tty), 0o755); err != nil {
				t.Fatal(err)
			}
		}
	}

	// Probe B: IF 1 & IF 2 unbound; IF 3 unbound, IF 4 bound to ttyUSB1.
	devB := filepath.Join(busDevs, "1-2")
	writeSysfsFile(t, filepath.Join(devB, "idVendor"), "2e8a\n")
	writeSysfsFile(t, filepath.Join(devB, "idProduct"), "1050\n")
	writeSysfsFile(t, filepath.Join(devB, "serial"), "E660583883000002\n")
	writeSysfsFile(t, filepath.Join(devB, "product"), "Pico Universal CMSIS-DAP\n")
	writeSysfsFile(t, filepath.Join(devB, "busnum"), "1\n")
	writeSysfsFile(t, filepath.Join(devB, "devnum"), "5\n")
	for _, itf := range []struct {
		num string
		hex string
		tty string
	}{
		{"0", "00", ""},
		{"1", "01", ""},
		{"2", "02", ""},
		{"3", "03", ""},
		{"4", "04", "ttyUSB1"},
		{"5", "05", ""},
	} {
		ifDir := filepath.Join(devB, "1-2:1."+itf.num)
		writeSysfsFile(t, filepath.Join(ifDir, "bInterfaceNumber"), itf.hex+"\n")
		if itf.tty != "" {
			if err := os.MkdirAll(filepath.Join(ifDir, "tty", itf.tty), 0o755); err != nil {
				t.Fatal(err)
			}
		}
	}

	nodes, err := ParseLinuxSysfsTree(busDevs)
	if err != nil {
		t.Fatalf("ParseLinuxSysfsTree error: %v", err)
	}
	probes := CorrelateNodes(nodes, 0x2E8A, 0x0000)
	if len(probes) != 2 {
		t.Fatalf("expected 2 probes, got %d: %+v", len(probes), probes)
	}

	if probes[0].DAPPath != "/dev/bus/usb/001/004" || probes[0].CtrlPath != "/dev/bus/usb/001/004" {
		t.Fatalf("expected bulk /dev/bus/usb fallback on IF 0/5 for probe[0], got %+v", probes[0])
	}
	if probes[0].GDBPort != "/dev/ttyACM0" || probes[0].UARTPort != "" {
		t.Fatalf("expected IF 2 ttyACM0 fallback and empty unbound UARTPort on probe[0], got %+v", probes[0])
	}
	if probes[1].GDBPort != "" || probes[1].UARTPort != "/dev/ttyUSB1" {
		t.Fatalf("expected empty unbound GDBPort and IF 4 ttyUSB1 fallback on probe[1], got %+v", probes[1])
	}
}

func TestWindowsUnboundCDCAndIF2IF4Fallback(t *testing.T) {
	records := []WindowsDevRecord{
		// Probe A: MI_01 unbound, MI_02 = COM15; MI_03 & MI_04 unbound
		{
			InstanceID: `USB\VID_2E8A&PID_1050\E660583883AAAA01`,
			Product:    "Pico Universal CMSIS-DAP",
			Service:    "usbccgp",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_00\6&aaaa&0&0000`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883AAAA01`,
			Service:          "WINUSB",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_01\6&aaaa&0&0001`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883AAAA01`,
			FriendlyName:     "Pico Universal CDC Control",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_02\6&aaaa&0&0002`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883AAAA01`,
			PortName:         "COM15",
			Service:          "usbser",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_03\6&aaaa&0&0003`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883AAAA01`,
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_04\6&aaaa&0&0004`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883AAAA01`,
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_05\6&aaaa&0&0005`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883AAAA01`,
			Service:          "WINUSB",
		},

		// Probe B: MI_01 & MI_02 unbound; MI_03 unbound, MI_04 = COM16 via FriendlyName
		{
			InstanceID: `USB\VID_2E8A&PID_1050\E660583883AAAA02`,
			Product:    "Pico Universal CMSIS-DAP",
			Service:    "usbccgp",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_01\6&bbbb&0&0001`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883AAAA02`,
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_02\6&bbbb&0&0002`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883AAAA02`,
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_03\6&bbbb&0&0003`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883AAAA02`,
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_04\6&bbbb&0&0004`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883AAAA02`,
			FriendlyName:     "USB Serial Device (COM16)",
			Service:          "usbser",
		},
	}

	nodes, err := ParseWindowsDeviceRecords(records)
	if err != nil {
		t.Fatalf("ParseWindowsDeviceRecords error: %v", err)
	}
	probes := CorrelateNodes(nodes, 0x2E8A, 0x0000)
	if len(probes) != 2 {
		t.Fatalf("expected 2 probes, got %d: %+v", len(probes), probes)
	}
	if probes[0].GDBPort != "COM15" || probes[0].UARTPort != "" {
		t.Fatalf("expected MI_02 COM15 fallback and empty unbound UARTPort on probe[0], got %+v", probes[0])
	}
	if probes[1].GDBPort != "" || probes[1].UARTPort != "COM16" {
		t.Fatalf("expected empty unbound GDBPort and MI_04 COM16 fallback on probe[1], got %+v", probes[1])
	}
}

func TestWindowsNonWinUSBServiceRejectedOnBulkInterfaces(t *testing.T) {
	records := []WindowsDevRecord{
		{
			InstanceID: `USB\VID_2E8A&PID_1050\E660583883NONWIN`,
			Product:    "Pico Universal CMSIS-DAP",
			Service:    "usbccgp",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_00\6&cccc&0&0000`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883NONWIN`,
			Service:          "libusbK",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_01\6&cccc&0&0001`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883NONWIN`,
			PortName:         "COM7",
			Service:          "usbser",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_05\6&cccc&0&0005`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883NONWIN`,
			Service:          "",
		},

		// Second probe: case-insensitive "WinUsb" on MI_00 and explicit DevicePath on MI_05
		{
			InstanceID: `USB\VID_2E8A&PID_1050\E660583883WINOK1`,
			Product:    "Pico Universal CMSIS-DAP",
			Service:    "usbccgp",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_00\6&dddd&0&0000`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883WINOK1`,
			Service:          "WinUsb",
		},
		{
			InstanceID:       `USB\VID_2E8A&PID_1050&MI_05\6&dddd&0&0005`,
			ParentInstanceID: `USB\VID_2E8A&PID_1050\E660583883WINOK1`,
			DevicePath:       `\\?\usb#vid_2e8a&pid_1050&mi_05#explicit#{50554354-2e8a-4d53-9a05-1aae46463777}`,
			Service:          "",
		},
	}

	nodes, err := ParseWindowsDeviceRecords(records)
	if err != nil {
		t.Fatalf("ParseWindowsDeviceRecords error: %v", err)
	}
	probes := CorrelateNodes(nodes, 0x2E8A, 0x0000)
	if len(probes) != 2 {
		t.Fatalf("expected 2 probes, got %d: %+v", len(probes), probes)
	}
	if probes[0].DAPPath != "" || probes[0].CtrlPath != "" {
		t.Fatalf("expected non-WinUSB MI_00/MI_05 to leave DAPPath and CtrlPath empty, got %+v", probes[0])
	}
	if probes[1].DAPPath == "" || probes[1].CtrlPath != `\\?\usb#vid_2e8a&pid_1050&mi_05#explicit#{50554354-2e8a-4d53-9a05-1aae46463777}` {
		t.Fatalf("expected WinUsb MI_00 and explicit DevicePath MI_05 to succeed, got %+v", probes[1])
	}
}
