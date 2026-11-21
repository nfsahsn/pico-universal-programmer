package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"strconv"
	"strings"

	"github.com/nfsahsn/pico-universal-programmer/host/internal/catalog"
	"github.com/nfsahsn/pico-universal-programmer/host/internal/puid"
	"github.com/nfsahsn/pico-universal-programmer/host/internal/uploader"
)

type uploaderRunnerAdapter struct {
	inner commandRunner
}

func (a uploaderRunnerAdapter) LookPath(file string) (string, error) {
	return a.inner.LookPath(file)
}

func (a uploaderRunnerAdapter) Run(spec uploader.CommandSpec, stdout, stderr io.Writer) error {
	return a.inner.Run(commandSpec{
		Name: spec.Name,
		Args: spec.Args,
		Env:  spec.Env,
	}, stdout, stderr)
}

type segmentSliceFlag []uploader.Segment

func (s *segmentSliceFlag) String() string {
	parts := make([]string, 0, len(*s))
	for _, seg := range *s {
		parts = append(parts, fmt.Sprintf("0x%x:%s", seg.Addr, seg.Path))
	}
	return strings.Join(parts, ",")
}

func (s *segmentSliceFlag) Set(val string) error {
	seg, err := uploader.ParseSegmentSpec(val)
	if err != nil {
		return err
	}
	*s = append(*s, seg)
	return nil
}

func resolveUploadFamily(familyFlag, coreFlag, fqbnFlag, boardFlag string) (string, error) {
	if strings.TrimSpace(familyFlag) != "" {
		return puid.NormalizeFamily(familyFlag)
	}

	// Resolve from Arduino FQBN using embedded catalog/targets.json first.
	fqbn := strings.TrimSpace(fqbnFlag)
	if fqbn != "" {
		if tc, _, err := catalog.LoadDefault(); err == nil {
			if b, ok := tc.FindBoardByFQBN(fqbn); ok && b.Profile != "" {
				return puid.NormalizeFamily(b.Profile)
			}
			// Match prefix when Arduino IDE appends extra menu options (`vendor:arch:board:opt1=val,opt2=val`).
			for _, b := range tc.Boards {
				if strings.HasPrefix(fqbn, b.FQBN+",") || strings.HasPrefix(fqbn, b.FQBN+":") {
					return puid.NormalizeFamily(b.Profile)
				}
			}
		}
		lowerFQBN := strings.ToLower(fqbn)
		switch {
		case strings.Contains(lowerFQBN, ":genf1:") || strings.Contains(lowerFQBN, "f103"):
			return "stm32f1", nil
		case strings.Contains(lowerFQBN, ":genf4:") || strings.Contains(lowerFQBN, "f401") || strings.Contains(lowerFQBN, "f411") || strings.Contains(lowerFQBN, "f407") || strings.Contains(lowerFQBN, "f446"):
			return "stm32f4", nil
		case strings.Contains(lowerFQBN, "rpipico2"):
			return "rp2350", nil
		case strings.Contains(lowerFQBN, "rp2040"):
			return "rp2040", nil
		case strings.Contains(lowerFQBN, "esp32s3"):
			return "esp32s3", nil
		case strings.Contains(lowerFQBN, "esp32"):
			return "esp32", nil
		case strings.Contains(lowerFQBN, "ch32v"):
			return "ch32v003", nil
		}
	}

	lowerBoard := strings.ToLower(strings.TrimSpace(boardFlag))
	switch {
	case strings.Contains(lowerBoard, "f1"):
		return "stm32f1", nil
	case strings.Contains(lowerBoard, "f4"):
		return "stm32f4", nil
	case strings.Contains(lowerBoard, "pico2") || strings.Contains(lowerBoard, "rp2350"):
		return "rp2350", nil
	case strings.Contains(lowerBoard, "esp32s3"):
		return "esp32s3", nil
	}

	switch strings.ToLower(strings.TrimSpace(coreFlag)) {
	case "rp2040":
		return "rp2040", nil
	case "esp32":
		return "esp32", nil
	case "ch32v", "ch32v003":
		return "ch32v003", nil
	case "stm32":
		return "", errors.New("ambiguous STM32 sub-family; pass --family stm32f1 or --family stm32f4 (or --fqbn)")
	}

	return "", errors.New("--family is required (stm32f1|stm32f4|rp2040|esp32|ch32v003)")
}

func runUpload(ctx cliContext, args []string) int {
	fs := flag.NewFlagSet("pu upload", flag.ContinueOnError)
	fs.SetOutput(ctx.stderr)

	familyFlag := fs.String("family", "", "Target MCU family (stm32f1|stm32f4|rp2040|rp2350|esp32|esp32s3|ch32v003)")
	fileFlag := fs.String("file", "", "Firmware ELF or BIN file path")
	elfFlag := fs.String("elf", "", "Firmware ELF file path (Arduino IDE 2 recipe alias for --file)")
	addrFlag := fs.String("addr", "", "Optional flash base address in hex for raw .bin upload")
	serialFlag := fs.String("serial", "", "Select specific Pico Universal probe hardware serial number")
	pidFlag := fs.String("pid", "", "Approved USB PID in hex (overrides PU_USB_PID)")
	coreFlag := fs.String("core", "", "Arduino IDE core family key (stm32|rp2040|esp32|ch32v)")
	fqbnFlag := fs.String("fqbn", "", "Arduino IDE board FQBN")
	boardFlag := fs.String("board", "", "Arduino IDE board identifier")
	verboseShort := fs.Bool("v", false, "Verbose flasher output")
	verboseLong := fs.Bool("verbose", false, "Verbose flasher output")

	var segments segmentSliceFlag
	fs.Var(&segments, "segment", "Espressif flash segment 0xADDR:PATH (repeatable)")

	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			return 0
		}
		return 2
	}

	family, err := resolveUploadFamily(*familyFlag, *coreFlag, *fqbnFlag, *boardFlag)
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu upload: %v\n", err)
		return 2
	}

	fwPath := strings.TrimSpace(*fileFlag)
	if fwPath == "" {
		fwPath = strings.TrimSpace(*elfFlag)
	}
	if fwPath == "" && len(segments) == 0 {
		fmt.Fprintln(ctx.stderr, "pu upload: --file (or --segment for esp32) is required")
		return 2
	}

	var baseAddr uint32
	if s := strings.TrimSpace(*addrFlag); s != "" {
		val, parseErr := strconv.ParseUint(strings.TrimPrefix(strings.TrimPrefix(s, "0x"), "0X"), 16, 32)
		if parseErr != nil {
			fmt.Fprintf(ctx.stderr, "pu upload: invalid --addr %q: %v\n", s, parseErr)
			return 2
		}
		baseAddr = uint32(val)
	}

	probe, err := resolveSingleProbe(ctx, *serialFlag, *pidFlag)
	if err != nil {
		fmt.Fprintf(ctx.stderr, "pu upload: %v\n", err)
		return 1
	}

	req := uploader.UploadRequest{
		Family:   family,
		FilePath: fwPath,
		BaseAddr: baseAddr,
		Segments: segments,
		Verbose:  *verboseShort || *verboseLong,
		Stdout:   ctx.stdout,
		Stderr:   ctx.stderr,
	}

	if err := uploader.UploadAtomic(probe, req, uploaderRunnerAdapter{inner: ctx.cmdRunner}); err != nil {
		fmt.Fprintf(ctx.stderr, "pu upload: %v\n", err)
		return 1
	}

	fmt.Fprintf(ctx.stdout, "Uploaded firmware to %s via Pico Universal (%s).\n", family, probe.Serial)
	return 0
}
