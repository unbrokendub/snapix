#pragma once

// =============================================================================
// SnapixBookIO — minimal byte-stream interfaces used by SnapixBookReader /
// SnapixBookWriter.  Symmetric with snapix::smoljpeg::InputStream / OutputStream
// but kept independent so this lib has no cross-lib deps.
//
// On Arduino: wrap an FsFile / fs::File in a thin adapter at the call
// site.  For host tests: implement with std::vector.
// =============================================================================

#include <cstddef>
#include <cstdint>

namespace snapix::cache {

// Random-access read source.  Used by SnapixBookReader to seek into the
// .bin file.
class BlobReader {
 public:
  virtual ~BlobReader() = default;

  // Total readable size in bytes.
  virtual uint32_t size() const = 0;

  // Read up to `len` bytes starting at absolute `offset`.  Returns the
  // number of bytes copied into `buf` (may be short at EOF).  Returns
  // -1 on hard I/O error.  Implementations should NOT block other
  // operations; the caller already serialises access.
  virtual int read(uint32_t offset, uint8_t* buf, uint32_t len) = 0;
};

// Sequential write sink.  Used by SnapixBookWriter to emit the final
// .bin.  Does NOT require seek — writes are append-only.
class BlobWriter {
 public:
  virtual ~BlobWriter() = default;

  // Write `len` bytes.  Returns true on full success, false on error
  // (any short write counts as failure).
  virtual bool write(const uint8_t* data, uint32_t len) = 0;
};

// Seek-capable write sink, used by `SnapixBookStreamWriter` for true
// streaming book construction.  The header + tables area is reserved at
// the start of the file, blobs stream past it, and the writer seeks back
// to fill in tables once final offsets are known.
//
// Both FsFile (SdFat) and fs::File (Arduino FS / LittleFS) support seek;
// the adapter pattern on the Arduino side wraps either onto this
// interface.
class BlobSeekableWriter : public BlobWriter {
 public:
  // Seek to `offset` from the start of the file.  Subsequent `write()`
  // calls append from this position.  Returns true on success.
  virtual bool seek(uint32_t offset) = 0;

  // Current write position (bytes from start).
  virtual uint32_t position() const = 0;
};

}  // namespace snapix::cache
