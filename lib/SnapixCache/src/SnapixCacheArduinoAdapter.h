#pragma once

// =============================================================================
// SnapixCache Arduino-side adapters — bridge SdFat `FsFile&` and Arduino
// `fs::File&` (LittleFS / SPIFFS) onto the SnapixCache BlobReader /
// BlobSeekableWriter interfaces.
//
// Header-only — pulls Arduino + SdFat / FS headers only when the file is
// included.  The core SnapixCache library stays platform-pure so the host
// unit tests can link without Arduino headers; firmware code includes
// this header explicitly at the integration site.
//
// Intended caller: PageCache once it migrates to the unified .bin layout
// (Phase 5b).  Until then, this header sits unused in the lib but its
// existence keeps the integration target one #include + ~5 lines away.
// =============================================================================

#include "SnapixBookIO.h"

// We only define the adapters when the host environment provides the
// underlying file types.  __has_include keeps the header benign on bare
// host compiles even though it's normally only #include'd from firmware.
#if defined(__has_include)
#  if __has_include(<SdFat.h>)
#    include <SdFat.h>
#    define SNAPIX_CACHE_ARDUINO_SDFAT 1
#  endif
#  if __has_include(<FS.h>)
#    include <FS.h>
#    define SNAPIX_CACHE_ARDUINO_FS 1
#  endif
#endif

namespace snapix::cache {

#ifdef SNAPIX_CACHE_ARDUINO_SDFAT
// SdFat random-access read adapter.  Reads bytes from `file` at the given
// absolute offset via seekSet + read.  `file` must outlive the adapter.
class FsFileBlobReader : public BlobReader {
 public:
  explicit FsFileBlobReader(FsFile& file) : file_(file) {}
  uint32_t size() const override {
    return static_cast<uint32_t>(file_.fileSize());
  }
  int read(uint32_t offset, uint8_t* buf, uint32_t len) override {
    if (!file_.seekSet(static_cast<uint64_t>(offset))) return -1;
    return file_.read(buf, len);
  }
 private:
  FsFile& file_;
};

// SdFat seekable-write adapter.  Writes are appended at the current
// position; `seek()` repositions for the header / tables backfill in
// `SnapixBookStreamWriter::finalize()`.
class FsFileBlobSeekableWriter : public BlobSeekableWriter {
 public:
  explicit FsFileBlobSeekableWriter(FsFile& file) : file_(file) {}
  bool write(const uint8_t* data, uint32_t len) override {
    return file_.write(data, len) == len;
  }
  bool seek(uint32_t offset) override {
    return file_.seekSet(static_cast<uint64_t>(offset));
  }
  uint32_t position() const override {
    return static_cast<uint32_t>(file_.curPosition());
  }
 private:
  FsFile& file_;
};
#endif  // SNAPIX_CACHE_ARDUINO_SDFAT

#ifdef SNAPIX_CACHE_ARDUINO_FS
// Arduino fs::File random-access read adapter (LittleFS / SPIFFS).
class FlashFileBlobReader : public BlobReader {
 public:
  explicit FlashFileBlobReader(fs::File& file) : file_(file) {}
  uint32_t size() const override {
    return static_cast<uint32_t>(file_.size());
  }
  int read(uint32_t offset, uint8_t* buf, uint32_t len) override {
    if (!file_.seek(offset, fs::SeekSet)) return -1;
    const int got = file_.read(buf, len);
    return got < 0 ? 0 : got;
  }
 private:
  fs::File& file_;
};

class FlashFileBlobSeekableWriter : public BlobSeekableWriter {
 public:
  explicit FlashFileBlobSeekableWriter(fs::File& file) : file_(file) {}
  bool write(const uint8_t* data, uint32_t len) override {
    return file_.write(data, len) == len;
  }
  bool seek(uint32_t offset) override {
    return file_.seek(offset, fs::SeekSet);
  }
  uint32_t position() const override {
    return static_cast<uint32_t>(file_.position());
  }
 private:
  fs::File& file_;
};
#endif  // SNAPIX_CACHE_ARDUINO_FS

}  // namespace snapix::cache
