# Snapix ⚡

Speed-optimized fork of [Papyrix](https://github.com/bigbag/papyrix-reader) — open-source firmware for the **Xteink X4** e-paper reader, targeting the **ESP32-C3** microcontroller.

[![Build firmware](https://github.com/unbrokendub/snapix/actions/workflows/build.yml/badge.svg)](https://github.com/unbrokendub/snapix/actions/workflows/build.yml)
[![Changelog](https://img.shields.io/badge/changelog-CHANGELOG.md-blue)](CHANGELOG.md)
[![User Guide](https://img.shields.io/badge/docs-User_Guide-green)](docs/user_guide.md)
[![Customization](https://img.shields.io/badge/docs-Customization-green)](docs/customization.md)
[![Architecture](https://img.shields.io/badge/docs-Architecture-green)](docs/architecture.md)

![Home screen](./docs/images/device.jpg)

## What's different in Snapix

Snapix is a drop-in replacement for Papyrix with the same features, same SD-card layout¹, but tuned hard for speed and stability on the ESP32-C3:

| Scenario | Papyrix | Snapix |
|---|---|---|
| Cold first-render of a new book (same font as previous) | 15–28 s | **0.2–1 s** (up to **130× faster**) |
| TOC jump to a new chapter | retry-spam, 100–400 ms wasted | instant, no retries |
| EPUB chapter with slow-converting image | stuck in retry loop until reboot | image blacklisted per session, chapter continues |
| SdFat "directory vanished" under memory pressure | possible crash / white screen | auto-recovers directory hierarchy |
| Text rendering hot path | in flash, cache-miss penalties | **~6 KB hot code pinned in IRAM** |
| Compiler flags | `-Os` (size) | **`-O2` + LTO** (speed, +~270 KB flash) |

**Other behavioural changes**

* **Fake Bold** now has three levels: `Off` / `Bold` (x,x+1 shift) / `Extra Bold` (x-1,x,x+1 shift) — adds synthetic weight without loading a bold font.
* **Default "Pages Per Refresh" is `0`** (no periodic full refresh).
* **Default "Transition Refresh" is `Off`** (no clean refresh on state transitions) — trade small residual ghosting for much faster navigation.

¹ SD-card paths changed from `/.papyrix/` to `/.snapix/`, so your first boot will see a clean-install experience. Your books in `/Books/` are untouched.

## Features

### Reading & Format Support
- EPUB 2 and EPUB 3 (nav.xhtml with NCX fallback)
- CSS parsing (text-align, font-style, font-weight, text-indent, margins, direction)
- FB2 with metadata, TOC navigation, metadata caching (no inline images)
- HTML (.html/.htm), XTC/XTCH native, Markdown (.md), plain text
- Saved reading position, bookmarks (up to 20 per book)
- Book covers (JPG/JPEG/PNG/BMP)
- Table of contents navigation
- Inline images in EPUB (baseline JPEG/PNG/BMP, max 2048×3072)

### Text & Display
- Configurable font sizes (XSmall/Small/Normal/Large)
- Paragraph alignment (Justified/Left/Center/Right)
- Text layout presets (Compact/Standard/Large)
- Soft hyphens + Liang-pattern hyphenation (de, en, es, fr, it, ru, uk)
- Native Vietnamese, Thai, Greek, Arabic support
- CJK text layout (book text)
- Thai mark positioning; Arabic shaping, Lam-Alef ligatures, RTL
- Knuth-Plass line breaking (TeX-quality justified text)
- Grayscale text anti-aliasing toggle
- Fake Bold (3 levels: Off / Bold / Extra Bold)
- 4 screen orientations

### Customization
- Themes from SD card (`/config/themes/`)
- Fonts from SD card (`/config/fonts/`, .epdfont format)
- Sleep screens (Dark / Light / Custom / Cover)
- Button remapping, power-button page turn

### Network & Connectivity
- WiFi file transfer (web server)
- Calibre Wireless Device

## Installing

### Flash a fresh device (recommended path)

Download the latest **`snapix-*-full.bin`** from [Releases](https://github.com/unbrokendub/snapix/releases). This single file contains the bootloader, partition table, and firmware.

With Python's `esptool`:

```bash
pip install esptool
esptool.py --chip esp32c3 --port /dev/tty.usbmodem* --baud 921600 write_flash 0x0 snapix-*-full.bin
```

Or with [papyrix-flasher](https://github.com/bigbag/papyrix-flasher) (upstream's CLI tool, works for Snapix too — the Xteink X4 ROM protocol is unchanged):

```bash
papyrix-flasher flash snapix-*-full.bin
```

### OTA update (already running Snapix or Papyrix)

Copy `snapix-*-firmware.bin` to the `/update/` folder on your SD card as `firmware.bin`. The device will flash on the next boot.

### If the device won't boot after flash

Erase flash and retry with the full binary:

```bash
esptool.py --chip esp32c3 erase_flash
esptool.py --chip esp32c3 -p /dev/tty.usbmodem* -b 921600 write_flash 0x0 snapix-*-full.bin
```

## Development

### Prerequisites

* PlatformIO Core (`pio`) or VS Code + PlatformIO IDE
* Python 3.12+ with [uv](https://docs.astral.sh/uv/) (for font conversion scripts)
* Node.js 18+ (for sleep screen and logo scripts)
* USB-C cable, Xteink X4

### Build & flash

```bash
git clone https://github.com/unbrokendub/snapix
cd snapix

# Dev build (verbose serial logs, perf counters on)
pio run

# Release build (no logs, smaller/faster)
pio run -e release

# Flash current build
pio run -t upload
```

See [docs/architecture.md](docs/architecture.md) for internals.

### Converting fonts

```bash
# Basic
uv run scripts/fontconvert.py my-font -r MyFont-Regular.ttf --2bit

# Full family with all reader sizes
uv run scripts/fontconvert.py my-font -r Regular.ttf -b Bold.ttf --2bit --all-sizes -o /tmp/fonts/

# With Thai / Arabic support
uv run scripts/fontconvert.py my-font -r Regular.ttf --2bit --thai -o /tmp/fonts/
uv run scripts/fontconvert.py my-font -r Regular.ttf --2bit --arabic -o /tmp/fonts/
```

See [customization guide](docs/customization.md) for full details.

### Sleep screen images

```bash
make sleep-screen INPUT=photo.jpg OUTPUT=sleep.bmp
# Options: --orientation, --bits, --dither, --fit
```

Copy the output BMP to `/sleep/` directory or as `/sleep.bmp` on the SD card.

### Creating a release

Push a tag matching `v*` to trigger the [GitHub Actions release workflow](.github/workflows/build.yml):

```bash
git tag v1.0.1
git push origin v1.0.1
```

It builds release firmware and publishes a GitHub Release with `snapix-*-full.bin` (one-shot flashable), `snapix-*-firmware.bin` (OTA), `snapix-*-bootloader.bin`, `snapix-*-partitions.bin`.

## Data caching

On first open, each book is cached under `/.snapix/<type>_<hash>/` on the SD card. Subsequent opens use the cache. Structure:

```
.snapix/
├── epub_12471232/            # EPUB cache
│   ├── progress.bin          # reading position
│   ├── bookmarks.bin         # (up to 20 per book)
│   ├── book.bin              # metadata, spine, TOC
│   ├── sections/<N>.bin      # paginated chapter data
│   └── images/<hash>.bmp     # converted inline images
│
├── fb2_55667788/             # FB2 cache (similar layout)
├── txt_98765432/             # TXT cache
├── md_12345678/              # Markdown cache
└── html_12345678/            # HTML cache
```

Clear via **Settings → Cleanup**, or delete `.snapix/` manually. See [docs/file-formats.md](docs/file-formats.md) for binary layouts.

## Credits

Snapix is a fork of **[Papyrix](https://github.com/bigbag/papyrix-reader)** by bigbag, which itself builds on [CrossPoint Reader](https://github.com/daveallie/crosspoint-reader) by Dave Allie.

* X4 hardware insights: [bb_epaper](https://github.com/bitbank2/bb_epaper) by Larry Bank
* Markdown parsing: [MD4C](https://github.com/mity/md4c) by Martin Mitáš
* CSS parser adapted from [microreader](https://github.com/CidVonHighwind/microreader) by CidVonHighwind

**Not affiliated with Xteink or any manufacturer of the X4 hardware.**
