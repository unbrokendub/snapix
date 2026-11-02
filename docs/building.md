# Building Snapix from a clean machine

Everything the build needs besides this repository is downloaded on demand,
so build outputs and toolchains can be deleted at any time to free disk space.

## What gets downloaded (and where)

| What | Size | Location | Re-created by |
|---|---|---|---|
| PlatformIO Core 6.1.x (Python package) | ~70 MB | the venv below | `pip install` |
| pioarduino `platform-espressif32` 55.03.37 (pinned in `platformio.ini`) | ~40 MB | `~/.platformio/platforms/espressif32` | first `pio run` |
| RISC-V GCC toolchain `toolchain-riscv32-esp` 14.2.0 | ~2.3 GB (+ a ~2 GB installer copy in `~/.platformio/tools`) | `~/.platformio/packages` | first `pio run` |
| Arduino-ESP32 core 3.3.x + precompiled ESP-IDF libs | ~1.9 GB | `~/.platformio/packages` | first `pio run` |
| esptool, scons, cppcheck | ~20 MB | `~/.platformio/packages` | first `pio run` |
| Firmware build output | ~180 MB per checkout | `<repo>/.pio/build` | `pio run` |
| Host unit-test build | ~250–330 MB per checkout | `<repo>/test/build` | `test/scripts/build_tests.sh` |

The first firmware build therefore downloads roughly 4–5 GB from GitHub and
takes several minutes; later builds are incremental.  Packages are shared by
all checkouts and git worktrees (`~/.platformio/packages`) — do not set a
project-local `packages_dir` in `platformio.ini` (if a machine really needs
isolation, put it in the untracked `platformio.local.ini`).

## 1. Python + PlatformIO (pinned, independent of Homebrew upgrades)

The pinned platform requires **Python 3.10–3.14** (3.15 is rejected).  Use a
dedicated virtual environment so a system/Homebrew Python upgrade cannot break
the build:

```bash
# macOS (Homebrew): python@3.13 or python@3.14
brew install python@3.13
/opt/homebrew/opt/python@3.13/bin/python3.13 -m venv ~/.snapix-pio
~/.snapix-pio/bin/pip install "platformio==6.1.19"
```

Use `~/.snapix-pio/bin/pio` for every command below (or add `~/.snapix-pio/bin`
to `PATH`).  Linux: any distro Python 3.10–3.14 with `python3 -m venv` works
the same way.

## 2. Build

```bash
cd snapix

# Dev build: serial logs, perf counters, memory checkpoints
~/.snapix-pio/bin/pio run -e default

# Release build: no logs, smaller/faster (what CI publishes)
~/.snapix-pio/bin/pio run -e release
```

The first run downloads the platform and packages listed above.  If a GitHub
download times out, simply run the same command again — finished packages are
kept.

## 3. Flash and watch the log

Connect the Xteink X4 over USB-C (it appears as `/dev/cu.usbmodem*` on macOS,
`/dev/ttyACM*` on Linux; the port is auto-detected):

```bash
~/.snapix-pio/bin/pio run -e default -t upload      # or -e release
~/.snapix-pio/bin/pio device monitor -b 115200
```

Only the `default` env prints logs; the `release` env is silent on serial.

## 4. Host unit tests (no device needed)

Requires CMake and a C++20 compiler (Xcode Command Line Tools on macOS).

```bash
SNAPIX_TEST_JOBS=8 bash test/scripts/build_tests.sh
ctest --test-dir test/build
```

Long property/fuzz runs, e.g. the pagination completeness fuzzer:

```bash
./test/build/bin/PaginationCompletenessFuzzTest 3000
```

## Freeing disk space

All of these are safe to delete and are re-created by the commands above:

```bash
rm -rf .pio test/build                                   # per checkout / worktree
rm -rf ~/.platformio/.cache ~/.platformio/dist           # download caches
rm -rf ~/.platformio/tools ~/.platformio/platforms/espressif32 \
       ~/.platformio/packages/toolchain-riscv32-esp \
       ~/.platformio/packages/framework-arduinoespressif32* \
       ~/.platformio/packages/tool-esp*                 # ESP32 toolchain (re-downloaded, ~4–5 GB)
```
