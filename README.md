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

> [!IMPORTANT]
> ## ⚠️ Cache moved to internal flash (v2.0.60+)
>
> Starting with **v2.0.60**, the OTA-update partition was dropped and the
> freed space was given to LittleFS, expanding it from **3.4 MB → 9.6 MB**.
> The book cache (paginated page layouts, decoded image BMPs, TOC, anchors,
> meta) now lives on the **internal SPI flash** instead of the SD card.
> Reading progress and bookmarks stay on SD next to your books.
>
> **Why:** the SD card and the e-paper display share an SPI bus, so every
> cache read serialized with display refresh.  That bus contention caused
> random ~100-300 ms stalls and the rare SDFat "cache directory vanished"
> crash under memory pressure.  LittleFS lives on a separate flash bus,
> so cache reads no longer fight with the display — page-load went from
> ~12-19 ms (SD with bus contention) to ~5-10 ms (LittleFS).
>
> **Trade-offs:**
>
> | | Before (v2.0.59) | After (v2.0.60+) |
> |---|---|---|
> | Page-load latency | 12-19 ms | 5-10 ms |
> | Cache survives SD swap | yes | no — rebuilt on first open |
> | Cache survives Settings → "Clear device storage" | optional | wiped |
> | OTA firmware updates | supported | **not supported** — USB flash only |
>
> **Flash endurance.** The W25Q128 NOR chip in the X4 is rated for ~100 000
> erase/write cycles per sector.  With LittleFS wear-leveling across the
> ~2 460 sectors of the 9.6 MB cache partition, the theoretical write
> budget is ~250 million sector-writes (≈ **1 TB lifetime**).  Realistic
> with LittleFS metadata overhead and ~70-80% leveling efficiency:
> **≈ 700 GB**.
>
> Typical reading generates **1-2 MB** of cache writes per session
> (one or two cold page-cache rebuilds + a few new image decodes + meta
> updates).  Even at heavy usage of 5 sessions/day × 1.5 MB ≈ 2.7 GB/year,
> the flash budget covers **~250 years**.  Battery, buttons, and SD
> connector will all fail long before the flash wears out.
>
> **One-time migration.**  First boot of v2.0.60+ forces a `LittleFS.format()`
> because the partition table changed (the freed OTA-slot region got
> absorbed into LittleFS).  Your books, progress, and bookmarks on the SD
> card are untouched.  The orphaned `/.snapix/cache/` directory on SD can
> be wiped via **Settings → Cleanup → Clear book cache** (it's already
> unused after v2.0.60) or deleted manually.

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
- **Bionic Reading** — bolds the first half of each word for faster scanning
- **Fake Bold** (3 levels: Off / Bold / Extra Bold) — synthetic weight without loading a bold font
- 4 screen orientations

### Customization
- Themes from SD card (`/config/themes/`) — full control over colors, margins, item heights, fonts
- Fonts from SD card (`/config/fonts/`, .epdfont format)
- **Custom status-bar font** per theme via `status_font = <family>` in the `[fonts]` section
- **Adjustable status bar vertical offset** via `status_bar_offset_y = <0-20>` in the `[layout]` section
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

Or download `snapix-flasher` for your OS from the same release page and flash the full image directly:

```bash
snapix-flasher flash snapix-*-full.bin
```

### OTA / SD update (Papyrix and Snapix < v2.0.60 only)

Older builds supported a "drop `firmware.bin` into `/update/` on the SD
card and reboot" update path.  **v2.0.60+ removes this** — the OTA app
slot was repurposed to expand LittleFS for the cache (see the migration
notice above), so SD-card-triggered reflash is no longer possible.
Future updates require a USB cable and `esptool` / `snapix-flasher` per
the steps above.

If you're upgrading from a pre-v2.0.60 build still running OTA-style
update: drop `snapix-2.0.60-firmware.bin` into `/update/firmware.bin`
once.  After it boots into v2.0.60, all subsequent updates need USB.

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
# Default: portrait 480x800, 4-bit indexed grayscale
# Options: --orientation, --bits, --dither, --fit
```

Copy the output BMP to `/sleep/` directory or as `/sleep.bmp` on the SD card.

### Creating a release

Push a tag matching `v*` to trigger the [GitHub Actions release workflow](.github/workflows/build.yml):

```bash
git tag v1.0.1
git push origin v1.0.1
```

It builds release firmware and publishes a GitHub Release with `snapix-*-full.bin` (one-shot flashable), `snapix-*-firmware.bin` (app-only), `snapix-*-bootloader.bin`, `snapix-*-partitions.bin`, and `snapix-flasher-*` binaries for macOS, Linux, and Windows.

## Data caching (v2.0.60+ split layout)

After v2.0.60 the cache splits across two storage tiers — **user data
on SD** (persistent, survives reflash and clear-device-storage), **book
cache on internal flash** (rebuilt on factory reset / first boot of new
firmware).

**SD card** — `/.snapix/`:

```
.snapix/
├── settings.bin                # global settings
├── state.bin                   # last-opened book, etc.
├── wifi.bin                    # WiFi credentials
├── progress/
│   └── <book_id>.bin           # reading position per book
└── bookmarks/
    ├── <book_id>.bin           # bookmarks per book (up to 20)
    └── <book_id>.txt           # human-readable bookmark export
```

**Internal flash (LittleFS)** — root paths:

```
/cache/<book_id>/               # book cache (paginated layouts + meta)
├── meta.bin                    # title, author, TOC items, binary index
├── pages_<spine_hash>.bin      # serialized Page objects per section
├── pages_<spine_hash>.bin.anchors  # TOC anchor → page-number mapping
└── .cover.failed               # marker if cover generation gave up

/img/<book_id>/                 # decoded image BMPs (1bpp, dithered)
└── <id>.bmp                    # one per <image l:href> in source

/font/                          # user-installed fonts (.epdfont)
```

`<book_id>` = `<type>_<hash>` where `<type>` is one of `fb2`, `epub`,
`txt`, `md`, `html`, `xtc` and `<hash>` is a deterministic 32-bit hash
of the source file path.  Same encoding the pre-v2.0.60 SD layout used.

**Clearing.**  **Settings → Cleanup → Clear book cache** wipes both the
LittleFS `/cache/` + `/img/` trees AND any orphaned SD `/.snapix/cache/`
left over from pre-v2.0.60 builds.  **Clear device storage** runs a full
`LittleFS.format()` which also nukes installed fonts.  See
[docs/file-formats.md](docs/file-formats.md) for binary layouts.

## Credits

Snapix is a fork of **[Papyrix](https://github.com/bigbag/papyrix-reader)** by bigbag, which itself builds on [CrossPoint Reader](https://github.com/daveallie/crosspoint-reader) by Dave Allie.

* X4 hardware insights: [bb_epaper](https://github.com/bitbank2/bb_epaper) by Larry Bank
* Markdown parsing: [MD4C](https://github.com/mity/md4c) by Martin Mitáš
* CSS parser adapted from [microreader](https://github.com/CidVonHighwind/microreader) by CidVonHighwind

**Not affiliated with Xteink or any manufacturer of the X4 hardware.**

## Third-party fonts

The firmware embeds four font families — each in five sizes (4 / 10 / 11 / 12 / 13 pt, regular only, 2-bit grayscale) — selectable via builtin themes. Format conversion (TTF → C array) is permitted under SIL OFL 1.1 §39-42.

| Theme | Font | Designer | License | OFL.txt |
|---|---|---|---|---|
| **JetBrains Mono** | [JetBrains Mono NL](https://github.com/JetBrains/JetBrainsMono) | JetBrains s.r.o. | SIL OFL 1.1 | [`scripts/jetbrains/OFL.txt`](scripts/jetbrains/OFL.txt) |
| **PT Mono** | [PT Mono](https://www.paratype.com/fonts/pt/pt-mono) | ParaType Ltd. | SIL OFL 1.1 | [`scripts/pt-mono/OFL.txt`](scripts/pt-mono/OFL.txt) |
| **IBM Plex Mono** | [IBM Plex Mono](https://github.com/IBM/plex) | IBM Corp. (Mike Abbink, Bold Monday) | SIL OFL 1.1 | [`scripts/ibm-plex-mono/OFL.txt`](scripts/ibm-plex-mono/OFL.txt) |
| **Literata** | [Literata](https://fonts.google.com/specimen/Literata) | Type Network for Google Fonts | SIL OFL 1.1 | [`scripts/literata/OFL.txt`](scripts/literata/OFL.txt) |

Per OFL 1.1 §56-61, every release artifact bundle includes the corresponding `OFL-*.txt` files. They MUST be redistributed alongside any copy of the firmware. The original family names are retained as user-facing display names — we do not claim authorship of any font, only embed and credit them.
