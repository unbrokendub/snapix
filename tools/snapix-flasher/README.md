# Snapix Flasher

Cross-platform command-line flasher for Snapix firmware on Xteink X4
ESP32-C3 devices.

## What It Flashes

The normal release artifact is `snapix-*-full.bin`. It is a merged image
containing:

- bootloader at `0x0000`
- partition table at `0x8000`
- Snapix firmware at `0x10000`

`snapix-flasher` detects this full image and writes it at `0x0`, matching
the `esptool.py write_flash 0x0 snapix-*-full.bin` path. App-only images
are still supported for advanced/manual use.

## Install

Download the matching archive from the Snapix release page:

- `snapix-flasher-*-darwin-arm64.tar.gz` for Apple Silicon Macs
- `snapix-flasher-*-darwin-amd64.tar.gz` for Intel Macs
- `snapix-flasher-*-linux-amd64.tar.gz`
- `snapix-flasher-*-linux-arm64.tar.gz`
- `snapix-flasher-*-windows-amd64.zip`

## Usage

Flash the recommended full image:

```bash
snapix-flasher flash snapix-v3.0.1-full.bin
```

Pick a port explicitly:

```bash
snapix-flasher flash -p /dev/cu.usbmodem101 snapix-v3.0.1-full.bin
snapix-flasher flash -p /dev/ttyUSB0 snapix-v3.0.1-full.bin
snapix-flasher flash -p COM3 snapix-v3.0.1-full.bin
```

Flash an app-only image at `0x10000`:

```bash
snapix-flasher flash --firmware-only snapix-v3.0.1-firmware.bin
```

Show connected device info:

```bash
snapix-flasher info
snapix-flasher info -p /dev/cu.usbmodem101
```

List serial ports:

```bash
snapix-flasher list
```

Show version:

```bash
snapix-flasher --version
```

## Snapix Flash Layout

The current 16 MB X4 layout is:

- `0x000000-0x009000`: bootloader
- `0x009000-0x00F000`: NVS
- `0x00F000-0x010000`: alignment gap
- `0x010000-0x650000`: factory app
- `0x650000-0xFF0000`: LittleFS cache partition
- `0xFF0000-0x1000000`: coredump

There is no OTA data partition in current Snapix. The old `0xE000` erase
step from upstream flashers is intentionally not used because that address
now belongs to NVS.

## Build

```bash
make build
make test
make release VERSION=v3.0.1
```

Refresh embedded bootloader/partition binaries after building firmware:

```bash
pio run -e release
make update-embedded
```

## License

MIT License. See [LICENSE](LICENSE).
