//go:build !linux && !windows

package usbprobe

func transceiveOSCtrlBulk(_ string, _ []byte) ([]byte, error) {
	return nil, ErrCtrlUnsupported
}
