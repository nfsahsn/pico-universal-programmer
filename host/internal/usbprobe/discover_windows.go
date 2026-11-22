//go:build windows

package usbprobe

import (
	"fmt"
	"strings"
	"syscall"
	"unsafe"
)

var (
	modSetupAPI = syscall.NewLazyDLL("setupapi.dll")
	modCfgMgr32 = syscall.NewLazyDLL("cfgmgr32.dll")

	procSetupDiGetClassDevsW              = modSetupAPI.NewProc("SetupDiGetClassDevsW")
	procSetupDiEnumDeviceInfo             = modSetupAPI.NewProc("SetupDiEnumDeviceInfo")
	procSetupDiDestroyDeviceInfoList      = modSetupAPI.NewProc("SetupDiDestroyDeviceInfoList")
	procSetupDiGetDeviceRegistryPropertyW = modSetupAPI.NewProc("SetupDiGetDeviceRegistryPropertyW")
	procSetupDiGetDevicePropertyW         = modSetupAPI.NewProc("SetupDiGetDevicePropertyW")
	procSetupDiOpenDevRegKey              = modSetupAPI.NewProc("SetupDiOpenDevRegKey")

	procCMGetParent    = modCfgMgr32.NewProc("CM_Get_Parent")
	procCMGetDeviceIDW = modCfgMgr32.NewProc("CM_Get_Device_IDW")
)

const (
	digcfPresent    = 0x00000002
	digcfAllClasses = 0x00000004

	spdrpDeviceDesc   = 0x00000000
	spdrpService      = 0x00000004
	spdrpFriendlyName = 0x0000000C

	dicsFlagGlobal = 0x00000001
	diregDev       = 0x00000001
	devpropTypeStr = 0x00000012
	crSuccess      = 0x00000000
)

type spDevInfoData struct {
	cbSize    uint32
	classGUID syscall.GUID
	devInst   uint32
	reserved  uintptr
}

type devPropKey struct {
	fmtid syscall.GUID
	pid   uint32
}

// DEVPKEY_Device_BusReportedDeviceDesc: {540b947e-8b40-45bc-a8a2-6a0b894cbda2}, 4
var devpkeyBusReportedDeviceDesc = devPropKey{
	fmtid: syscall.GUID{
		Data1: 0x540b947e,
		Data2: 0x8b40,
		Data3: 0x45bc,
		Data4: [8]byte{0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2},
	},
	pid: 4,
}

func collectOSInterfaceNodes(vid, _ uint16) ([]RawInterfaceNode, error) {
	enumPtr, err := syscall.UTF16PtrFromString("USB")
	if err != nil {
		return nil, err
	}
	hDevInfo, _, callErr := procSetupDiGetClassDevsW.Call(
		0,
		uintptr(unsafe.Pointer(enumPtr)),
		0,
		uintptr(digcfPresent|digcfAllClasses),
	)
	if syscall.Handle(hDevInfo) == syscall.InvalidHandle {
		return nil, fmt.Errorf("usbprobe: SetupDiGetClassDevsW failed: %w", callErr)
	}
	defer procSetupDiDestroyDeviceInfoList.Call(hDevInfo)

	vidPrefix := fmt.Sprintf(`USB\VID_%04X`, vid)
	var records []WindowsDevRecord

	for idx := uint32(0); ; idx++ {
		var devInfo spDevInfoData
		devInfo.cbSize = uint32(unsafe.Sizeof(devInfo))
		r1, _, _ := procSetupDiEnumDeviceInfo.Call(
			hDevInfo,
			uintptr(idx),
			uintptr(unsafe.Pointer(&devInfo)),
		)
		if r1 == 0 {
			break
		}

		instanceID := cmGetDeviceID(devInfo.devInst)
		if instanceID == "" || !strings.HasPrefix(strings.ToUpper(instanceID), vidPrefix) {
			continue
		}

		var parentInstID string
		var parentDevInst uint32
		if cr, _, _ := procCMGetParent.Call(uintptr(unsafe.Pointer(&parentDevInst)), uintptr(devInfo.devInst), 0); cr == crSuccess {
			parentInstID = cmGetDeviceID(parentDevInst)
		}

		records = append(records, WindowsDevRecord{
			InstanceID:       instanceID,
			ParentInstanceID: parentInstID,
			Product:          queryBusReportedDesc(hDevInfo, &devInfo),
			DeviceDesc:       queryDeviceRegString(hDevInfo, &devInfo, spdrpDeviceDesc),
			FriendlyName:     queryDeviceRegString(hDevInfo, &devInfo, spdrpFriendlyName),
			Service:          queryDeviceRegString(hDevInfo, &devInfo, spdrpService),
			PortName:         queryDevRegPortName(hDevInfo, &devInfo),
		})
	}

	return ParseWindowsDeviceRecords(records)
}

func cmGetDeviceID(devInst uint32) string {
	var buf [512]uint16
	cr, _, _ := procCMGetDeviceIDW.Call(
		uintptr(devInst),
		uintptr(unsafe.Pointer(&buf[0])),
		uintptr(len(buf)),
		0,
	)
	if cr != crSuccess {
		return ""
	}
	return syscall.UTF16ToString(buf[:])
}

func queryBusReportedDesc(hDevInfo uintptr, devInfo *spDevInfoData) string {
	var propType uint32
	var reqSize uint32
	var buf [256]uint16
	r1, _, _ := procSetupDiGetDevicePropertyW.Call(
		hDevInfo,
		uintptr(unsafe.Pointer(devInfo)),
		uintptr(unsafe.Pointer(&devpkeyBusReportedDeviceDesc)),
		uintptr(unsafe.Pointer(&propType)),
		uintptr(unsafe.Pointer(&buf[0])),
		uintptr(len(buf)*2),
		uintptr(unsafe.Pointer(&reqSize)),
		0,
	)
	if r1 == 0 || propType != devpropTypeStr {
		return ""
	}
	return syscall.UTF16ToString(buf[:])
}

func queryDeviceRegString(hDevInfo uintptr, devInfo *spDevInfoData, prop uint32) string {
	var regType uint32
	var reqSize uint32
	var buf [256]uint16
	r1, _, _ := procSetupDiGetDeviceRegistryPropertyW.Call(
		hDevInfo,
		uintptr(unsafe.Pointer(devInfo)),
		uintptr(prop),
		uintptr(unsafe.Pointer(&regType)),
		uintptr(unsafe.Pointer(&buf[0])),
		uintptr(len(buf)*2),
		uintptr(unsafe.Pointer(&reqSize)),
	)
	if r1 == 0 {
		return ""
	}
	return syscall.UTF16ToString(buf[:])
}

func queryDevRegPortName(hDevInfo uintptr, devInfo *spDevInfoData) string {
	hKeyRaw, _, _ := procSetupDiOpenDevRegKey.Call(
		hDevInfo,
		uintptr(unsafe.Pointer(devInfo)),
		uintptr(dicsFlagGlobal),
		0,
		uintptr(diregDev),
		uintptr(syscall.KEY_READ),
	)
	if syscall.Handle(hKeyRaw) == syscall.InvalidHandle || hKeyRaw == 0 {
		return ""
	}
	hKey := syscall.Handle(hKeyRaw)
	defer syscall.RegCloseKey(hKey)

	valName, err := syscall.UTF16PtrFromString("PortName")
	if err != nil {
		return ""
	}
	var valType uint32
	var buf [64]uint16
	bufLen := uint32(len(buf) * 2)
	if err := syscall.RegQueryValueEx(hKey, valName, nil, &valType, (*byte)(unsafe.Pointer(&buf[0])), &bufLen); err != nil {
		return ""
	}
	return syscall.UTF16ToString(buf[:])
}
