# Xteink X4 Device Specifications

Technical documentation for the Xteink X4 e-reader hardware running Snapix firmware.

---

## Overview

- **Device** — Xteink X4 E-Reader
- **Display** — 4.26" E-Ink (800x480)
- **Processor** — ESP32-C3 (RISC-V)
- **RAM** — ~380 KB usable
- **Flash** — 16 MB
- **Storage** — SD Card (FAT32)
- **Battery** — LiPo (monitored via ADC)

---

## Microcontroller

### ESP32-C3 Specifications

- **Architecture** — RISC-V RV32IMC
- **Clock Speed** — 160 MHz
- **SRAM** — 400 KB (380 KB usable)
- **Flash** — 16 MB external
- **WiFi** — 802.11 b/g/n (2.4 GHz)
- **Bluetooth** — BLE 5.0
- **GPIO** — 22 pins
- **ADC** — 2 channels, 12-bit

### Memory Layout

- **DROM** — `0x3C140020` — ~5 MB — Data ROM (strings, constants)
- **DRAM** — `0x3FC91C00` — ~14 KB — Initialized data
- **IROM** — `0x42000020` — ~1.2 MB — Main executable code
- **IRAM** — `0x40380000` — ~72 KB — Hot path code
- **RTC** — `0x50000000` — 56 bytes — RTC retention memory

### Power Consumption

- **Active (reading)** — ~50 mA
- **WiFi active** — ~150 mA
- **Deep sleep** — ~10 uA

---

## Display

### Panel Specifications

- **Type** — E-Ink (electrophoretic)
- **Model** — GDEQ0426T82
- **Size** — 4.26 inches diagonal
- **Resolution** — 800 x 480 pixels
- **Pixel Density** — ~217 PPI
- **Colors** — Black/White (4-level grayscale)
- **Viewing Angle** — 180 degrees

### Display Controller (SSD1677)

- **Controller IC** — Solomon Systech SSD1677
- **Interface** — 4-wire SPI
- **SPI Clock** — 40 MHz
- **SPI Mode** — Mode 0 (CPOL=0, CPHA=0)
- **Data Order** — MSB First

### Framebuffer

- **Single buffer** — 48 KB — One framebuffer (current default)
- **Dual buffer** — 96 KB — BW + RED RAM for differential updates

**Calculation:** 800 pixels / 8 bits = 100 bytes per row x 480 rows = 48,000 bytes

### Refresh Modes

- **Full refresh** — ~1600 ms — Page turns, complete redraw
- **Fast refresh** — ~600 ms — Quick updates with custom LUT
- **Half refresh** — ~1720 ms — Reduced ghosting, no flash
- **Partial (window)** — ~50-100 ms — Status bar, UI elements

#### Refresh Mode Details

- **Full** — Write BW RAM -> Copy to RED RAM -> Refresh — 48KB write + copy + 1.5s waveform
- **Half** — Write BOTH BW and RED RAM separately -> Refresh — 48KB x 2 writes + 1.5s waveform
- **Fast** — Write BW RAM only -> Short waveform — 48KB write + 0.5s short waveform

**Why Half refresh takes longer than Full:**
- Full refresh writes 48KB to BW RAM, then uses fast internal copy to RED RAM
- Half refresh writes 48KB twice (to each RAM separately) = 96KB total transfer
- The extra ~120ms is the second 48KB SPI transfer
- "Half" refers to reduced voltage swing in the waveform, not time

### Display Pin Mapping

- **SCLK** — GPIO 8 — Output — SPI Clock
- **MOSI** — GPIO 10 — Output — SPI Data Out
- **CS** — GPIO 21 — Output — Chip Select (active LOW)
- **DC** — GPIO 4 — Output — Data/Command select
- **RST** — GPIO 5 — Output — Hardware reset (active LOW)
- **BUSY** — GPIO 6 — Input — Busy status (LOW = busy)

### SSD1677 Commands

- **SWRESET** — `0x12` — Software reset
- **DRIVER_OUTPUT** — `0x01` — Gate driver control
- **DATA_ENTRY** — `0x11` — X/Y direction mode
- **RAM_X_ADDR** — `0x44` — Set X address range
- **RAM_Y_ADDR** — `0x45` — Set Y address range
- **RAM_X_CNT** — `0x4E` — Set X counter
- **RAM_Y_CNT** — `0x4F` — Set Y counter
- **WRITE_BW** — `0x24` — Write to BW RAM
- **WRITE_RED** — `0x26` — Write to RED RAM
- **MASTER_ACT** — `0x20` — Activate display update
- **UPDATE_SEQ** — `0x22` — Display update sequence
- **WRITE_LUT** — `0x32` — Write LUT table
- **DEEP_SLEEP** — `0x10` — Enter deep sleep

### Display Refresh Sequence

```
1. Send 0x4E (Set X address) + start column
2. Send 0x4F (Set Y address) + start/end row
3. Send 0x24 (Write BW RAM)
4. Send 48,000 bytes of image data
5. Send 0x26 (Write RED RAM) [for differential refresh]
6. Send 48,000 bytes of previous frame
7. Send 0x20 (Master activation)
8. Send 0x22 (Display refresh trigger)
9. Wait for BUSY pin LOW
```

### LUT Table Structure (111 bytes)

- **0-49** — 50 bytes — VS waveforms (5 groups x 10 bytes)
- **50-99** — 50 bytes — TP/RP timing (10 groups x 5 bytes)
- **100-104** — 5 bytes — Frame rate control
- **105** — 1 byte — VGH (gate high voltage)
- **106** — 1 byte — VSH1 (source high 1)
- **107** — 1 byte — VSH2 (source high 2)
- **108** — 1 byte — VSL (source low)
- **109** — 1 byte — VCOM
- **110** — 1 byte — Reserved

### Voltage Settings

- **VGH** — `0x17` — Gate high (~17-22V)
- **VSH1** — `0x41` — Source high 1 (~15V)
- **VSH2** — `0xA8` — Source high 2
- **VSL** — `0x32` — Source low (negative)
- **VCOM** — `0x30` — Common voltage

---

## Input System

### Button Configuration

The device uses a resistor ladder connected to two ADC pins plus a dedicated power button GPIO.

- **BACK** — Index 0 — ADC1 — ~3512 mV
- **CONFIRM** — Index 1 — ADC1 — ~2694 mV
- **LEFT** — Index 2 — ADC1 — ~1493 mV
- **RIGHT** — Index 3 — ADC1 — ~5 mV
- **UP** — Index 4 — ADC2 — ~2242 mV
- **DOWN** — Index 5 — ADC2 — ~5 mV
- **POWER** — Index 6 — GPIO3 — Digital (active LOW)

### ADC Pin Configuration

- **ADC_PIN_1** — GPIO 1 — Back/Confirm/Left/Right buttons
- **ADC_PIN_2** — GPIO 2 — Up/Down buttons
- **POWER_PIN** — GPIO 3 — Power button (digital, INPUT_PULLUP)

### ADC Voltage Ranges

**ADC Channel 1 (GPIO 1):**

- **3800+ mV** — None
- **2090-3800 mV** — BACK
- **750-2090 mV** — CONFIRM
- **0-750 mV** — LEFT/RIGHT

**ADC Channel 2 (GPIO 2):**

- **1120+ mV** — None
- **0-1120 mV** — UP/DOWN

### Input Settings

- **Debounce delay** — 5 ms
- **ADC attenuation** — 11 dB
- **ADC resolution** — 12-bit
- **Power button** — Active LOW with pullup

---

## Storage

### SD Card Interface

- **Interface** — SPI
- **Filesystem** — FAT32
- **Max size** — 32 GB recommended
- **SPI Clock** — 40 MHz

### SD Card Pin Mapping

- **CS** — GPIO 12 — Output
- **MISO** — GPIO 7 — Input
- **MOSI** — GPIO 10 — Output (shared with display)
- **SCLK** — GPIO 8 — Output (shared with display)

---

## Power Management

### Battery Monitoring

- **ADC Pin** — GPIO 0
- **Voltage range** — 0-3.3V (after divider)
- **Divider ratio** — 2:1
- **Full battery** — ~4.2V
- **Empty battery** — ~3.0V

### Battery Percentage Formula

Uses polynomial fit for LiPo discharge curve:
```
percentage = -144.9390*V^3 + 1655.8629*V^2 - 6158.8520*V + 7501.3202
```
Where V is battery voltage (clamped to 0-100% range).

### Power States

- **Active** — Normal operation
- **Light sleep** — CPU paused, peripherals active (wake: Any interrupt)
- **Deep sleep** — Ultra-low power (~10 uA) (wake: Power button GPIO)

### Deep Sleep Wakeup

- Wake on power button press (GPIO 3)
- Configurable hold duration for power on
- Software reset detection via `ESP_RST_SW`

---

## Build Configuration

### PlatformIO Settings

```ini
[env:release]
platform = espressif32
board = esp32-c3-devkitm-1
framework = arduino
board_build.flash_mode = dio
board_build.flash_size = 16MB
board_build.partitions = partitions.csv
upload_speed = 921600
monitor_speed = 115200
```

### Build Flags

- `-DARDUINO_USB_MODE=1` — USB mode enabled
- `-DARDUINO_USB_CDC_ON_BOOT=1` — USB CDC on boot
- `-DEINK_DISPLAY_SINGLE_BUFFER_MODE=1` — Single framebuffer (48KB)
- `-DUSE_UTF8_LONG_NAMES=1` — UTF-8 filename support

### C++ Standard

C++20 (`-std=gnu++2a`)

---

## Supported File Formats

### Books

- **EPUB** — `.epub` — Standard e-book format
- **XTC** — `.xtc` — Xteink native format (1-bit)
- **XTCH** — `.xtch` — Xteink high-quality (2-bit grayscale)
- **Markdown** — `.md`, `.markdown` — Markdown with basic formatting
- **TXT** — `.txt`, `.text` — Plain text

### Images

- **JPEG** — `.jpg`, `.jpeg` — Max 1024x1024
- **PNG** — `.png` — Max 1024x1024
- **BMP** — `.bmp` — Max 1024x1024
- **GIF** — `.gif` — Max 1024x1024

### Fonts

- **TrueType** — `.ttf` — Standard TrueType
- **OpenType** — `.otf` — OpenType fonts
- **WOFF** — `.woff` — Web Open Font Format
- **WOFF2** — `.woff2` — WOFF 2.0
- **EpdFont** — `.epdfont` — Custom e-ink optimized format

---

## Hardware Libraries

Located in `open-x4-sdk/libs/`:

- **EInkDisplay** — `display/EInkDisplay/` — SSD1677 driver
- **InputManager** — `hardware/InputManager/` — Button handling
- **BatteryMonitor** — `hardware/BatteryMonitor/` — Battery ADC
- **SDCardManager** — `hardware/SDCardManager/` — SD card interface

---

## Pin Summary

- **GPIO 0** — Battery ADC — Input
- **GPIO 1** — Button ADC 1 — Input
- **GPIO 2** — Button ADC 2 — Input
- **GPIO 3** — Power button — Input (pullup)
- **GPIO 4** — Display DC — Output
- **GPIO 5** — Display RST — Output
- **GPIO 6** — Display BUSY — Input
- **GPIO 7** — SD MISO — Input
- **GPIO 8** — SPI SCLK — Output
- **GPIO 10** — SPI MOSI — Output
- **GPIO 12** — SD CS — Output
- **GPIO 21** — Display CS — Output

---

## Filesystem Structure

```
/
├── .snapix/                 # System and cache directory
│   ├── settings.bin          # User settings
│   ├── state.bin             # Application state
│   ├── wifi.bin              # WiFi credentials
│   ├── epub_<hash>/          # EPUB cache per book
│   │   ├── progress.bin      # Reading position
│   │   ├── cover.bmp         # Cached cover
│   │   ├── book.bin          # Metadata
│   │   ├── sections/         # Chapter data
│   │   └── images/           # Cached inline images
│   ├── txt_<hash>/           # TXT file cache
│   └── md_<hash>/            # Markdown file cache
├── config/                   # Configuration files
│   ├── calibre.ini           # Calibre server config
│   ├── themes/               # UI themes
│   └── fonts/                # Custom fonts
└── Books/                    # User books directory
```

---

## References

- [ESP32-C3 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-c3_technical_reference_manual_en.pdf)
- [ESP32-C3 Datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c3_datasheet_en.pdf)
- [SSD1677 Datasheet](https://www.solomon-systech.com/)
