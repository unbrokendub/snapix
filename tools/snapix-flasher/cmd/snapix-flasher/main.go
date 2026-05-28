package main

import (
	"fmt"
	"os"

	"github.com/spf13/cobra"

	"github.com/unbrokendub/snapix/tools/snapix-flasher/embedded"
	"github.com/unbrokendub/snapix/tools/snapix-flasher/internal/detect"
	"github.com/unbrokendub/snapix/tools/snapix-flasher/internal/flasher"
	"github.com/unbrokendub/snapix/tools/snapix-flasher/internal/protocol"
	"github.com/unbrokendub/snapix/tools/snapix-flasher/internal/serial"
)

var (
	portFlag         string
	baudFlag         int
	firmwareOnlyFlag bool
	version          = "dev"
	commit           = "none"
	date             = "unknown"
)

func main() {
	rootCmd := &cobra.Command{
		Use:     "snapix-flasher",
		Short:   "Flash Snapix firmware to Xteink X4 (ESP32-C3) devices",
		Version: fmt.Sprintf("%s (%s, %s)", version, commit, date),
	}

	// Flash command
	flashCmd := &cobra.Command{
		Use:   "flash <snapix.bin>",
		Short: "Flash a Snapix full or firmware-only image",
		Args:  cobra.ExactArgs(1),
		RunE:  runFlash,
	}
	flashCmd.Flags().StringVarP(&portFlag, "port", "p", "", "Serial port (auto-detect if not specified)")
	flashCmd.Flags().IntVarP(&baudFlag, "baud", "b", protocol.DefaultBaudRate, "Baud rate")
	flashCmd.Flags().BoolVar(&firmwareOnlyFlag, "firmware-only", false,
		"Force app-only mode: write the image at 0x10000 and skip bundled bootloader/partitions")

	// Info command
	infoCmd := &cobra.Command{
		Use:   "info",
		Short: "Show device info",
		RunE:  runInfo,
	}
	infoCmd.Flags().StringVarP(&portFlag, "port", "p", "", "Serial port (auto-detect if not specified)")
	infoCmd.Flags().IntVarP(&baudFlag, "baud", "b", protocol.DefaultBaudRate, "Baud rate")

	listCmd := &cobra.Command{
		Use:   "list",
		Short: "List serial ports",
		RunE:  runList,
	}

	rootCmd.AddCommand(flashCmd, infoCmd, listCmd)

	if err := rootCmd.Execute(); err != nil {
		os.Exit(1)
	}
}

func runFlash(cmd *cobra.Command, args []string) error {
	firmwarePath := args[0]

	// Read firmware file
	firmware, err := os.ReadFile(firmwarePath)
	if err != nil {
		return fmt.Errorf("failed to read firmware file: %w", err)
	}

	fmt.Printf("Firmware: %s (%d bytes)\n", firmwarePath, len(firmware))
	regions, imageKind, err := flashRegionsForImage(firmware)
	if err != nil {
		return err
	}
	fmt.Printf("Image type: %s\n", imageKind)

	// Find or use specified port
	portName := portFlag
	if portName == "" {
		fmt.Println("Detecting device...")
		result, err := detect.DetectDevice(baudFlag)
		if err != nil {
			return fmt.Errorf("device detection failed: %w", err)
		}
		portName = result.Port
		fmt.Printf("Found %s on %s\n", result.ChipName, result.Port)
	}

	// Open port
	port, err := serial.Open(portName, baudFlag)
	if err != nil {
		return fmt.Errorf("failed to open port: %w", err)
	}
	defer port.Close()

	fmt.Printf("Port: %s @ %d baud\n", portName, baudFlag)

	// Create flasher
	f := flasher.New(port)

	// Connect to bootloader
	fmt.Println("Connecting to bootloader...")
	if err := f.Connect(); err != nil {
		return err
	}
	fmt.Println("Connected!")

	// Flash each region using compressed transfer
	for _, region := range regions {
		fmt.Printf("Flashing %s at 0x%X (%d bytes)...\n", region.Name, region.Address, len(region.Data))
		if err := f.FlashImageCompressed(region.Data, region.Address, false); err != nil {
			return err
		}
	}

	fmt.Println("\nFlash complete!")

	// Reboot
	fmt.Println("Rebooting device...")
	if err := f.Reboot(); err != nil {
		fmt.Printf("Warning: reboot failed: %v\n", err)
		fmt.Println("If the device does not start, press the reset button.")
	}

	fmt.Println("Done!")
	return nil
}

func flashRegionsForImage(image []byte) ([]flasher.FlashRegion, string, error) {
	if isMergedFullImage(image) {
		if firmwareOnlyFlag {
			return nil, "", fmt.Errorf("input is a merged full image; remove --firmware-only to flash it at 0x0")
		}
		return []flasher.FlashRegion{{
			Address: protocol.BootloaderAddress,
			Data:    image,
			Name:    "full image",
		}}, "merged full image (bootloader + partitions + firmware)", nil
	}

	if !looksLikeEspImage(image, 0) {
		return nil, "", fmt.Errorf("input does not look like an ESP32-C3 image (bad magic byte)")
	}

	regions := make([]flasher.FlashRegion, 0, 3)
	if !firmwareOnlyFlag {
		regions = append(regions,
			flasher.FlashRegion{
				Address: protocol.BootloaderAddress,
				Data:    embedded.Bootloader(),
				Name:    "bootloader",
			},
			flasher.FlashRegion{
				Address: protocol.PartitionsAddress,
				Data:    embedded.Partitions(),
				Name:    "partitions",
			},
		)
	}

	regions = append(regions, flasher.FlashRegion{
		Address: protocol.FirmwareAddress,
		Data:    image,
		Name:    "firmware",
	})

	if firmwareOnlyFlag {
		return regions, "app-only firmware image", nil
	}
	return regions, "app-only firmware image plus bundled Snapix bootloader/partitions", nil
}

func isMergedFullImage(image []byte) bool {
	return looksLikeEspImage(image, protocol.BootloaderAddress) &&
		looksLikePartitionTable(image, protocol.PartitionsAddress) &&
		looksLikeEspImage(image, protocol.FirmwareAddress)
}

func looksLikeEspImage(image []byte, offset uint32) bool {
	return len(image) > int(offset) && image[offset] == protocol.EspImageMagic
}

func looksLikePartitionTable(image []byte, offset uint32) bool {
	i := int(offset)
	return len(image) > i+1 && image[i] == protocol.PartitionTableMagic0 && image[i+1] == protocol.PartitionTableMagic1
}

func runInfo(cmd *cobra.Command, args []string) error {
	if portFlag != "" {
		// Check specific port
		result, err := detect.DetectOnPort(portFlag, baudFlag)
		if err != nil {
			return fmt.Errorf("failed to detect device on %s: %w", portFlag, err)
		}
		printDeviceInfo(result)
		return nil
	}

	// Auto-detect
	fmt.Println("Scanning for ESP32 devices...")
	devices, err := detect.ListDevices(baudFlag)
	if err != nil {
		return err
	}

	if len(devices) == 0 {
		fmt.Println("No ESP32 devices found")
		return nil
	}

	fmt.Printf("Found %d device(s):\n\n", len(devices))
	for i, d := range devices {
		fmt.Printf("Device %d:\n", i+1)
		printDeviceInfo(&d)
		fmt.Println()
	}

	return nil
}

func runList(cmd *cobra.Command, args []string) error {
	ports, err := serial.ListPorts()
	if err != nil {
		return err
	}
	if len(ports) == 0 {
		fmt.Println("No serial ports found")
		return nil
	}
	for _, port := range ports {
		fmt.Println(port)
	}
	return nil
}

func printDeviceInfo(d *detect.Result) {
	fmt.Printf("  Port:     %s\n", d.Port)
	fmt.Printf("  Chip:     %s\n", d.ChipName)
	if d.ChipID != 0 {
		fmt.Printf("  Chip ID:  0x%02X\n", d.ChipID)
	}
}
