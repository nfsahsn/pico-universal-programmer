//go:build linux

package usbprobe

import (
	"fmt"
	"runtime"
	"strings"
	"syscall"
	"unsafe"
)

const (
	// _IOR('U', 15, unsigned int) and _IOR('U', 16, unsigned int) from <linux/usbdevice_fs.h>.
	usbdevfsClaimInterface   uintptr = 0x8004550F
	usbdevfsReleaseInterface uintptr = 0x80045510
)

type usbdevfsBulkTransfer struct {
	EP      uint32
	Len     uint32
	Timeout uint32
	Data    *byte
}

func usbdevfsBulkIoctl() uintptr {
	// _IOWR('U', 2, struct usbdevfs_bulktransfer)
	return (uintptr(3) << 30) | (unsafe.Sizeof(usbdevfsBulkTransfer{}) << 16) | (uintptr('U') << 8) | 2
}

func normalizeLinuxUSBDevPath(ctrlPath string) string {
	p := strings.TrimSpace(ctrlPath)
	if colon := strings.LastIndexByte(p, ':'); colon > 0 {
		return p[:colon]
	}
	return p
}

func transceiveOSCtrlBulk(ctrlPath string, req []byte) ([]byte, error) {
	devPath := normalizeLinuxUSBDevPath(ctrlPath)
	fd, err := syscall.Open(devPath, syscall.O_RDWR|syscall.O_CLOEXEC, 0)
	if err != nil {
		return nil, fmt.Errorf("usbprobe: open %s: %w", devPath, err)
	}
	defer syscall.Close(fd)

	ifNum := uint32(CtrlInterfaceNum)
	if _, _, errno := syscall.Syscall(
		syscall.SYS_IOCTL,
		uintptr(fd),
		usbdevfsClaimInterface,
		uintptr(unsafe.Pointer(&ifNum)),
	); errno != 0 {
		return nil, fmt.Errorf("usbprobe: claim IF %d on %s: %w", CtrlInterfaceNum, devPath, errno)
	}
	defer func() {
		relIf := uint32(CtrlInterfaceNum)
		_, _, _ = syscall.Syscall(
			syscall.SYS_IOCTL,
			uintptr(fd),
			usbdevfsReleaseInterface,
			uintptr(unsafe.Pointer(&relIf)),
		)
	}()

	outXfer := usbdevfsBulkTransfer{
		EP:      CtrlEndpointOut,
		Len:     uint32(len(req)),
		Timeout: CtrlTimeoutMS,
		Data:    &req[0],
	}
	written, _, errno := syscall.Syscall(
		syscall.SYS_IOCTL,
		uintptr(fd),
		usbdevfsBulkIoctl(),
		uintptr(unsafe.Pointer(&outXfer)),
	)
	runtime.KeepAlive(req)
	runtime.KeepAlive(&outXfer)
	if errno != 0 {
		return nil, fmt.Errorf("usbprobe: bulk OUT 0x%02x on %s: %w", CtrlEndpointOut, devPath, errno)
	}
	if int(written) != len(req) {
		return nil, fmt.Errorf("usbprobe: short bulk OUT on %s (%d/%d bytes)", devPath, written, len(req))
	}

	var respBuf [CtrlPacketSize]byte
	inXfer := usbdevfsBulkTransfer{
		EP:      CtrlEndpointIn,
		Len:     uint32(len(respBuf)),
		Timeout: CtrlTimeoutMS,
		Data:    &respBuf[0],
	}
	readN, _, errno := syscall.Syscall(
		syscall.SYS_IOCTL,
		uintptr(fd),
		usbdevfsBulkIoctl(),
		uintptr(unsafe.Pointer(&inXfer)),
	)
	runtime.KeepAlive(&respBuf)
	runtime.KeepAlive(&inXfer)
	if errno != 0 {
		return nil, fmt.Errorf("usbprobe: bulk IN 0x%02x on %s: %w", CtrlEndpointIn, devPath, errno)
	}
	if readN > uintptr(len(respBuf)) {
		readN = uintptr(len(respBuf))
	}
	return append([]byte(nil), respBuf[:readN]...), nil
}
