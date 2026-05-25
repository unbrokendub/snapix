#pragma once
#include <SdFat.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class ZipFile;

// v2.0.159 — streaming reader for a single ZIP entry.  Returned by
// `ZipFile::openItemStream()`.  Self-contained: owns its own FsFile handle
// (seeked to the entry's data offset), uzlib InflateReader (with a 32 KB
// dict), and an 8 KB SD read buffer.  Outlives the ZipFile that created it.
//
// Use this instead of `readFileToStream{Detailed}` when the consumer wants
// to pull bytes lazily — eliminates the need for a temp file to hold the
// fully-extracted content.  Primary use case: markerize-from-ZIP (see
// EpubChapterParser::tryMarkerizeChapter) which avoids the 50 KB transient
// heap peak + ~10 s wall time of the old extract-to-LittleFS path.
//
// Heap cost: 32 KB dict (or caller-provided buffer) + 8 KB SD read buf,
// freed when the reader is destroyed.  No I/O until `read()` is called.
class ZipItemReader {
 public:
  ~ZipItemReader();

  ZipItemReader(const ZipItemReader&) = delete;
  ZipItemReader& operator=(const ZipItemReader&) = delete;
  ZipItemReader(ZipItemReader&&) noexcept;
  ZipItemReader& operator=(ZipItemReader&&) noexcept;

  // Read up to `maxLen` bytes of decompressed content into `buf`.  Returns:
  //   * positive N → N bytes written to buf[0..N)
  //   * zero        → EOF reached (clean end of stream)
  //   * negative    → I/O or decompression error (reader is invalidated)
  int read(uint8_t* buf, size_t maxLen);

  // Total uncompressed size of the entry (from the ZIP central directory).
  // Available immediately after construction, before any read() call.
  uint32_t totalUncompressedSize() const;

  // Bytes produced so far across all read() calls.
  uint32_t bytesProduced() const;

  // True once read() has reached the clean end of the stream.
  bool isEof() const;

  // True if a previous read() returned an error.
  bool hasError() const;

 private:
  friend class ZipFile;
  ZipItemReader();

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class ZipFile {
 public:
  enum class StreamReadResult : uint8_t {
    Success,
    OpenFailed,
    NotFound,
    InvalidOffset,
    UnsupportedMethod,
    AllocFailed,
    Aborted,
    ReadError,
    WriteError,
    DecompressionError,
    SizeMismatch,
  };

  struct FileStatSlim {
    uint16_t method;             // Compression method
    uint32_t compressedSize;     // Compressed size
    uint32_t uncompressedSize;   // Uncompressed size
    uint32_t localHeaderOffset;  // Offset of local file header
  };

  struct ZipDetails {
    uint32_t centralDirOffset;
    uint16_t totalEntries;
    bool isSet;
  };

  struct SizeTarget {
    uint64_t hash;   // FNV-1a 64-bit hash of normalized path
    uint16_t len;    // Length for collision reduction
    uint16_t index;  // Caller's index (e.g. spine index)

    bool operator<(const SizeTarget& other) const {
      return hash < other.hash || (hash == other.hash && len < other.len);
    }
  };

  // FNV-1a 64-bit hash (no std::string allocation)
  // Combined with 16-bit length provides ~80 bits of entropy;
  // collision probability negligible for typical EPUB file counts
  static uint64_t fnvHash64(const char* s, size_t len) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < len; i++) {
      hash ^= static_cast<uint8_t>(s[i]);
      hash *= 1099511628211ull;
    }
    return hash;
  }

 private:
  const std::string& filePath;
  FsFile file;
  ZipDetails zipDetails = {0, 0, false};
  std::unordered_map<std::string, FileStatSlim> fileStatSlimCache;

  bool loadFileStatSlim(const char* filename, FileStatSlim* fileStat);
  long getDataOffset(const FileStatSlim& fileStat);
  bool loadZipDetails();

 public:
  explicit ZipFile(const std::string& filePath) : filePath(filePath) {}
  ~ZipFile() = default;
  // Zip file can be opened and closed by hand in order to allow for quick calculation of inflated file size
  // It is NOT recommended to pre-open it for any kind of inflation due to memory constraints
  bool isOpen() const { return !!file; }
  bool open();
  bool close();
  bool loadAllFileStatSlims();
  const std::unordered_map<std::string, FileStatSlim>& getFileStatSlimCache() const { return fileStatSlimCache; }
  uint16_t getTotalEntries();
  bool getInflatedFileSize(const char* filename, size_t* size);
  // Batch lookup: scan ZIP central dir once and fill sizes for matching targets.
  // targets must be sorted by (hash, len). sizes[target.index] receives uncompressedSize.
  // Returns number of targets matched.
  int fillUncompressedSizes(std::vector<SizeTarget>& targets, std::vector<uint32_t>& sizes);
  // Find first existing file from a list of paths. Returns index into paths array, or -1 if none found.
  // More efficient than calling getInflatedFileSize() for each path individually.
  int findFirstExisting(const char* const* paths, int pathCount);
  // Due to the memory required to run each of these, it is recommended to not preopen the zip file for multiple
  // These functions will open and close the zip as needed
  uint8_t* readFileToMemory(const char* filename, size_t* size = nullptr, bool trailingNullByte = false);
  static const char* streamReadResultToString(StreamReadResult result);
  StreamReadResult readFileToStreamDetailed(const char* filename, Print& out, size_t chunkSize,
                                            uint8_t* dictBuffer = nullptr,
                                            const std::function<bool()>& shouldAbort = nullptr);
  bool readFileToStream(const char* filename, Print& out, size_t chunkSize, uint8_t* dictBuffer = nullptr,
                        const std::function<bool()>& shouldAbort = nullptr);

  // v2.0.159 — open a ZIP entry for lazy streaming reads.  See ZipItemReader.
  // `chunkSize` is the SD read buffer the reader uses for compressed-data
  // pulls (8 KB matches `LARGE_ZIP_STREAM_CHUNK` in EpubChapterParser).
  // `dictBuffer` is optional; nullptr → InflateReader heap-allocates 32 KB.
  // Returns a non-null unique_ptr on success, nullptr on any failure
  // (open, lookup, allocation).  The returned reader OUTLIVES this ZipFile
  // — it opens its own FsFile handle to the same path during construction.
  std::unique_ptr<ZipItemReader> openItemStream(const char* filename, size_t chunkSize = 8192,
                                                uint8_t* dictBuffer = nullptr);
};
