//go:build !linux && !windows

package usbprobe

func collectOSInterfaceNodes(_, _ uint16) ([]RawInterfaceNode, error) {
	return nil, nil
}
