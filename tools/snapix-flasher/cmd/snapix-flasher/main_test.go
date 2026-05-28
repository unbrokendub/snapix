package main

import (
	"testing"

	"github.com/unbrokendub/snapix/tools/snapix-flasher/internal/protocol"
)

func TestFlashRegionsForMergedFullImage(t *testing.T) {
	oldFirmwareOnly := firmwareOnlyFlag
	firmwareOnlyFlag = false
	defer func() { firmwareOnlyFlag = oldFirmwareOnly }()

	image := make([]byte, protocol.FirmwareAddress+1)
	image[protocol.BootloaderAddress] = protocol.EspImageMagic
	image[protocol.PartitionsAddress] = protocol.PartitionTableMagic0
	image[protocol.PartitionsAddress+1] = protocol.PartitionTableMagic1
	image[protocol.FirmwareAddress] = protocol.EspImageMagic

	regions, kind, err := flashRegionsForImage(image)
	if err != nil {
		t.Fatalf("flashRegionsForImage returned error: %v", err)
	}
	if kind != "merged full image (bootloader + partitions + firmware)" {
		t.Fatalf("kind = %q", kind)
	}
	if len(regions) != 1 {
		t.Fatalf("region count = %d, want 1", len(regions))
	}
	if regions[0].Address != protocol.BootloaderAddress {
		t.Fatalf("region address = 0x%X, want 0x%X", regions[0].Address, protocol.BootloaderAddress)
	}
	if regions[0].Name != "full image" {
		t.Fatalf("region name = %q", regions[0].Name)
	}
}

func TestFlashRegionsForAppOnlyImage(t *testing.T) {
	oldFirmwareOnly := firmwareOnlyFlag
	firmwareOnlyFlag = true
	defer func() { firmwareOnlyFlag = oldFirmwareOnly }()

	image := []byte{protocol.EspImageMagic, 0x01, 0x02, 0x03}

	regions, kind, err := flashRegionsForImage(image)
	if err != nil {
		t.Fatalf("flashRegionsForImage returned error: %v", err)
	}
	if kind != "app-only firmware image" {
		t.Fatalf("kind = %q", kind)
	}
	if len(regions) != 1 {
		t.Fatalf("region count = %d, want 1", len(regions))
	}
	if regions[0].Address != protocol.FirmwareAddress {
		t.Fatalf("region address = 0x%X, want 0x%X", regions[0].Address, protocol.FirmwareAddress)
	}
}

func TestMergedFullImageRejectsFirmwareOnlyFlag(t *testing.T) {
	oldFirmwareOnly := firmwareOnlyFlag
	firmwareOnlyFlag = true
	defer func() { firmwareOnlyFlag = oldFirmwareOnly }()

	image := make([]byte, protocol.FirmwareAddress+1)
	image[protocol.BootloaderAddress] = protocol.EspImageMagic
	image[protocol.PartitionsAddress] = protocol.PartitionTableMagic0
	image[protocol.PartitionsAddress+1] = protocol.PartitionTableMagic1
	image[protocol.FirmwareAddress] = protocol.EspImageMagic

	if _, _, err := flashRegionsForImage(image); err == nil {
		t.Fatal("expected merged full image to reject --firmware-only")
	}
}

func TestBadImageMagicRejected(t *testing.T) {
	oldFirmwareOnly := firmwareOnlyFlag
	firmwareOnlyFlag = false
	defer func() { firmwareOnlyFlag = oldFirmwareOnly }()

	if _, _, err := flashRegionsForImage([]byte{0x00, 0x01}); err == nil {
		t.Fatal("expected bad image magic to be rejected")
	}
}
