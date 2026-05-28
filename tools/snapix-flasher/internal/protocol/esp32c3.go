package protocol

// Flash addresses for the Snapix/Xteink X4 ESP32-C3 layout.
const (
	BootloaderAddress = 0x0000
	PartitionsAddress = 0x8000
	FirmwareAddress   = 0x10000
)

// Image/table magic bytes used for lightweight local validation.
const (
	EspImageMagic        = 0xE9
	PartitionTableMagic0 = 0xAA
	PartitionTableMagic1 = 0x50
)

// Default baud rate
const DefaultBaudRate = 921600
