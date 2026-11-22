//go:build linux

package usbprobe

import (
	"errors"
	"os"
)

const defaultLinuxSysfsUSBRoot = "/sys/bus/usb/devices"

func collectOSInterfaceNodes(_, _ uint16) ([]RawInterfaceNode, error) {
	if _, err := os.Stat(defaultLinuxSysfsUSBRoot); err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, nil
		}
		return nil, err
	}
	return ParseLinuxSysfsTree(defaultLinuxSysfsUSBRoot)
}
