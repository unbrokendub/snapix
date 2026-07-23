/**
 * Fb2.cpp
 *
 * FictionBook 2.0 XML e-book handler implementation for Snapix Reader
 */

#include "Fb2.h"

#include "Base64JpegPump.h"

#include <LittleFS.h>  // Internal-flash image cache (v2.0.53+) — see flashImageCacheDir below

#include <CacheFs.h>
#include <CoverHelpers.h>
#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>

#if SNAPIX_SMOL_JPEG
#include <SmolJpeg.h>
#endif

#define TAG "FB2"
#include <SDCardManager.h>
#include <Serialization.h>
#include <SharedSpiLock.h>

#include <algorithm>
#include <cstring>
#include <functional>  // v2.0.60: std::function for recursive rmTree lambda in clearCache
#include <new>         // v2.0.197: std::bad_alloc for parseXmlStream try/catch
#include <stdexcept>   // v2.0.197: std::length_error, std::exception
#include <memory>

namespace {
constexpr uint8_t kMetaCacheVersion = 5;  // v5: adds binary index for inline images
constexpr char kMetaCacheFile[] = "/meta.bin";

void closeFileProtected(FsFile& file) {
  if (!file) {
    return;
  }
  snapix::spi::SharedBusLock lk;
  file.close();
}

#if SNAPIX_SMOL_JPEG
// ---------------------------------------------------------------------------
// Base64JpegPump → SmolJpeg::InputStream adapter.  Bridges the forward-only
// base64-decoder pump used by Fb2::decodeImageDirect onto the random-access
// interface SmolJpeg expects.
//
// SmolJpeg reads mostly forward (HeaderReader's sliding window scans
// markers in order; decodeBaselineStream's BitReader walks the
// entropy-coded scan from `scanStart` linearly).  Backward seeks happen
// only when SmolJpeg rewinds for restart markers — vanishingly rare on
// the EPUB / FB2 corpus.  We handle them by rewinding the underlying
// pump and replaying past bytes through `produceBytes` into a throwaway
// stack buffer.
//
// v2.0.95 (Phase 4d — FB2 image decode through SmolJpeg): same
// motivation as the v2.0.89 EPUB inline-image fix.  The legacy FB2
// path went `Fb2::decodeImageDirect` → `JpegToBmpConverter::jpegStreamToBmp`
// → JPEGDEC's pre-allocated ~25 KB workspace + per-decode ~12-15 KB
// scratch arena.  On a tight heap (chapter parse interleaved with UI
// render) those two contiguous blocks couldn't co-exist and the decode
// reported `JPEGDEC: OOM allocating shared workspace` or `JPEG OOM
// allocating decode scratch`.  Reroute to SmolJpeg (BSS-pooled
// JpegState + ~15 KB transient yRow only) so the JPEGDEC two-tier
// allocation pressure goes away.
// ---------------------------------------------------------------------------
class Base64PumpInputStream : public snapix::smoljpeg::InputStream {
 public:
  explicit Base64PumpInputStream(Base64JpegPump& p) : pump_(p) {}

  uint32_t length() const override { return pump_.logicalSize(); }

  int read(uint32_t offset, uint8_t* buf, size_t len) override {
    if (offset < pump_.logicalPos()) {
      // Backward seek — rewind the pump and replay forward.  Rare in
      // practice (SmolJpeg's HeaderReader / BitReader almost never
      // seek backward past their internal 256-byte window).
      pump_.rewind();
    }
    // Forward-skip the gap between current pos and requested offset.
    uint8_t throwaway[128];
    while (pump_.logicalPos() < offset) {
      const size_t want = std::min<size_t>(sizeof(throwaway),
                                            offset - pump_.logicalPos());
      const size_t got = pump_.produceBytes(throwaway, want);
      if (got == 0) return -1;  // EOS while skipping
    }
    if (len == 0) return 0;
    return static_cast<int>(pump_.produceBytes(buf, len));
  }

 private:
  Base64JpegPump& pump_;
};

class PrintOutputStream : public snapix::smoljpeg::OutputStream {
 public:
  explicit PrintOutputStream(Print& p) : print_(p) {}
  bool write(const uint8_t* data, size_t len) override {
    return print_.write(data, len) == len;
  }

 private:
  Print& print_;
};
#endif  // SNAPIX_SMOL_JPEG

// Base64 alphabet decode table.  Returns -1 for non-base64 / whitespace,
// -2 for the '=' padding character, [0..63] for valid base64 chars.
// Used by streamDecodeBase64ToFile to decode FB2 <binary> blocks without
// holding the entire encoded string in RAM.
int8_t base64DecodeChar(uint8_t c) {
  if (c >= 'A' && c <= 'Z') return static_cast<int8_t>(c - 'A');
  if (c >= 'a' && c <= 'z') return static_cast<int8_t>(c - 'a' + 26);
  if (c >= '0' && c <= '9') return static_cast<int8_t>(c - '0' + 52);
  if (c == '+') return 62;
  if (c == '/') return 63;
  if (c == '=') return -2;
  return -1;
}

// Stream-decode a base64 chunk inside an FB2 file directly to an output JPEG
// file.  Ranges from `offset` (start of `<binary` opening tag) for `length`
// bytes (up to start of `</binary>`).  The opening-tag prefix is detected
// and skipped — first '>' encountered marks the start of base64 content.
//
// v2.0.49 perf rework: per-chunk lock granularity used to dominate wall-time.
// Old code held the SharedBusLock for ~256 B at a time → 800 lock acquisitions
// for a 200 KB block → ~9 s total under display-refresh contention.  Bumping
// to 4 KB (one SD cluster) gives us ~50 acquires → ~600 ms total.  Hold the
// lock around the read+write+sync so the in-flight decode buffers fully drain
// to SD as one batch, not interleaved with display SPI ops.
//
// We also DROP shouldAbort propagation here.  Previously a mid-stream abort
// would leave the binary in a "deferred" limbo that needed a retry path AND
// caused ImageBlocks to vanish from page cache (the parser's makePages flush
// dropped the placeholder block).  At the new ~600 ms speed it's no longer
// worth interrupting — the worker just commits to finishing the stream.
bool streamDecodeBase64ToJpegFile(const std::string& srcPath, uint32_t offset, uint32_t length,
                                  const std::string& dstPath) {
  FsFile src;
  if (!SdMan.openFileForRead("FB2", srcPath, src)) return false;
  FsFile dst;
  if (!SdMan.openFileForWrite("FB2", dstPath, dst)) {
    src.close();
    return false;
  }

  {
    snapix::spi::SharedBusLock lk;
    if (!src.seek(offset)) {
      src.close();
      dst.close();
      return false;
    }
  }

  // 4 KB IO buffers — match SD cluster size for write coalescing.  out is
  // sized at 3 KB so a full inBuf of base64 (~4096 chars / 4 × 3 = 3072
  // decoded bytes) fits exactly without mid-buffer flush.
  static constexpr size_t kInBuf = 4096;
  static constexpr size_t kOutBuf = 3072;
  std::unique_ptr<uint8_t[]> inBuf(new (std::nothrow) uint8_t[kInBuf]);
  std::unique_ptr<uint8_t[]> outBuf(new (std::nothrow) uint8_t[kOutBuf]);
  if (!inBuf || !outBuf) {
    src.close();
    dst.close();
    return false;
  }

  size_t outIdx = 0;
  uint32_t accum = 0;
  int accumBits = 0;
  bool foundOpenTagEnd = false;
  uint32_t remaining = length;
  bool ok = true;

  while (remaining > 0) {
    int toRead = static_cast<int>(std::min<uint32_t>(kInBuf, remaining));
    int got = 0;
    {
      snapix::spi::SharedBusLock lk;
      got = src.read(inBuf.get(), toRead);
    }
    if (got <= 0) {
      ok = (got == 0);  // genuine EOF is fine; negative is an SD read error
      break;
    }
    remaining -= static_cast<uint32_t>(got);

    for (int i = 0; i < got; i++) {
      const uint8_t c = inBuf[i];
      if (!foundOpenTagEnd) {
        if (c == '>') foundOpenTagEnd = true;
        continue;
      }
      const int8_t v = base64DecodeChar(c);
      if (v < 0) continue;  // skip whitespace, '=' padding, anything non-base64
      accum = (accum << 6) | static_cast<uint8_t>(v);
      accumBits += 6;
      if (accumBits >= 8) {
        accumBits -= 8;
        outBuf[outIdx++] = static_cast<uint8_t>((accum >> accumBits) & 0xFF);
        if (outIdx >= kOutBuf) {
          snapix::spi::SharedBusLock lk;
          dst.write(outBuf.get(), outIdx);
          outIdx = 0;
        }
      }
    }
  }
  if (outIdx > 0) {
    snapix::spi::SharedBusLock lk;
    dst.write(outBuf.get(), outIdx);
  }
  {
    snapix::spi::SharedBusLock lk;
    dst.sync();
    dst.close();
    src.close();
  }
  return ok;
}

// ============================================================================
// LittleFS image cache (v2.0.53)
//
// Inline FB2 image BMPs live on the internal flash partition instead of the
// SD card.  Three reasons:
//
// 1) Internal flash uses a separate SPI bus from the SD-card / e-paper
//    display pair, so image reads during render don't fight with display
//    refresh (no more 100-200 ms post-write-recovery latency cliff per
//    SdMan.openFileForRead).
//
// 2) LittleFS reads have stable ~5-10 ms latency for our typical 30 KB BMP
//    regardless of recent write activity.  This kills the "first render
//    after decode is 1.5 s" pathology of v2.0.51-52.
//
// 3) The in-memory ImageRenderCache that v2.0.50-52 used to dodge SD
//    contention pinned 30+ KB of contiguous heap and tipped the BG
//    worker's `isHeapCritical` watchdog into a permanent deadlock.  With
//    flash reads being fast, we no longer need that cache at all — saves
//    32 KB of pinned heap budget.
//
// Capacity: the LittleFS partition is 3.4 MB total.  Existing usage is
// effectively read-only (font files only), leaving ~2 MB free for image
// cache.  At ~30 KB per BMP that's room for ~60-70 cached images, more
// than any single book typically references.
//
// Wear: industry-standard 100 K erase cycles per 4 KB block × 870 blocks
// gives ~10 M total cache writes.  Realistic intensive reading patterns
// would take ~1000 years to wear this out.  See conversation context for
// the full math.
// ============================================================================

constexpr char kFlashImageCacheRoot[] = "/img";

// Compute the per-book LittleFS image cache directory.  Keyed by the book's
// existing FB2 cache hash so books on the same device get isolated cache
// spaces (and clearing one book's cache doesn't trash others).
std::string flashImageCacheDir(const std::string& cachePath) {
  // cachePath is "<cacheDir>/fb2_<hash>"; we want just "fb2_<hash>".
  size_t lastSlash = cachePath.find_last_of('/');
  const std::string bookDir = (lastSlash == std::string::npos) ? cachePath : cachePath.substr(lastSlash + 1);
  return std::string(kFlashImageCacheRoot) + "/" + bookDir;
}

// Recursively create LittleFS directory tree (Arduino LittleFS::mkdir is
// non-recursive — it fails if a parent doesn't exist).  Returns true if
// the directory exists or was created successfully.
bool ensureFlashDir(const std::string& path) {
  if (path.empty() || path == "/") return true;
  if (LittleFS.exists(path.c_str())) return true;

  size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos && lastSlash > 0) {
    if (!ensureFlashDir(path.substr(0, lastSlash))) return false;
  }
  return LittleFS.mkdir(path.c_str());
}

// Slurp a LittleFS file into a heap buffer.  Returns nullptr on miss / OOM.
// `outSize` is the file size in bytes.  Caller owns the returned buffer.
std::unique_ptr<uint8_t[]> loadBmpFromFlash(const std::string& path, size_t& outSize) {
  outSize = 0;
  if (!LittleFS.exists(path.c_str())) return nullptr;

  File f = LittleFS.open(path.c_str(), "r");
  if (!f) return nullptr;
  const size_t size = f.size();
  // Sanity bounds: BMP must be ≥ 62 B (header) and we never expect to cache
  // anything bigger than ~96 KB on this constrained device.
  if (size < 62 || size > 96 * 1024) {
    f.close();
    return nullptr;
  }

  std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[size]);
  if (!buf) {
    f.close();
    return nullptr;
  }
  const size_t got = f.read(buf.get(), size);
  f.close();
  if (got != size) return nullptr;

  outSize = size;
  return buf;
}

}  // namespace

std::string Fb2::metaCachePath() const { return cachePath + kMetaCacheFile; }

Fb2::Fb2(std::string filepath, const std::string& cacheDir)
    : filepath(std::move(filepath)), fileSize(0), loaded(false) {
  // Create cache key based on filepath (same as Epub/Xtc/Txt)
  cachePath = cacheDir + "/fb2_" + std::to_string(std::hash<std::string>{}(this->filepath));

  // Extract title from filename
  size_t lastSlash = this->filepath.find_last_of('/');
  size_t lastDot = this->filepath.find_last_of('.');

  if (lastSlash == std::string::npos) {
    lastSlash = 0;
  } else {
    lastSlash++;
  }

  if (lastDot == std::string::npos || lastDot <= lastSlash) {
    title = this->filepath.substr(lastSlash);
  } else {
    title = this->filepath.substr(lastSlash, lastDot - lastSlash);
  }
}

Fb2::~Fb2() {
  if (xmlParser_) {
    XML_ParserFree(xmlParser_);
    xmlParser_ = nullptr;
  }
}

// Wipe all stale `*.failed` markers from this book's LittleFS image cache
// directory.  Called once per book load to recover from false-positive
// markers left behind by older firmware bugs (e.g. v2.0.53's
// decodeImageDirect mis-classifying heap-low aborts as permanent failures).
//
// Truly broken binaries will get `.failed` re-written by decodePendingImages
// after kMaxJpegDecodeRetries consecutive failures in this session, so this
// cleanup doesn't cause infinite-retry loops on genuinely unsupported images.
//
// Cost: one LittleFS dir scan + N small file deletes (typically 0-3 entries).
static void clearStaleFailedMarkers(const std::string& flashImagesDir) {
  if (!LittleFS.exists(flashImagesDir.c_str())) return;
  File dir = LittleFS.open(flashImagesDir.c_str(), "r");
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  std::vector<std::string> toRemove;
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) break;
    const char* name = entry.name();
    const size_t nameLen = name ? strlen(name) : 0;
    entry.close();
    if (nameLen >= 7 && strcmp(name + nameLen - 7, ".failed") == 0) {
      toRemove.push_back(flashImagesDir + "/" + name);
    }
  }
  dir.close();
  for (const auto& path : toRemove) {
    LittleFS.remove(path.c_str());
    LOG_INF(TAG, "clearStaleFailedMarkers: removed %s", path.c_str());
  }
}

bool Fb2::load() {
  LOG_INF(TAG, "Loading FB2: %s", filepath.c_str());

  if (!SdMan.exists(filepath.c_str())) {
    LOG_ERR(TAG, "File does not exist");
    return false;
  }

  if (!cache_fs::ensureSourceFingerprint(filepath, cachePath)) {
    LOG_ERR(TAG, "Could not verify source fingerprint");
    return false;
  }

  // v2.0.55: clear any stale `.failed` markers from previous sessions (in
  // case earlier firmware versions wrote false-positive markers).  Per-load
  // cleanup ensures we always retry images at least once per book open.
  clearStaleFailedMarkers(flashImageCacheDir(cachePath));

  // Try loading from metadata cache first
  if (loadMetaCache()) {
    loaded = true;
    LOG_INF(TAG, "Loaded from cache: %s (title: '%s', author: '%s')", filepath.c_str(), title.c_str(), author.c_str());
    return true;
  }

  FsFile file;
  if (!SdMan.openFileForRead("FB2", filepath, file)) {
    LOG_ERR(TAG, "Failed to open file");
    return false;
  }

  {
    snapix::spi::SharedBusLock lk;
    fileSize = file.size();
    file.close();
  }

  // Stream-parse in chunks (file may exceed available RAM)
  if (!parseXmlStream()) {
    LOG_ERR(TAG, "Failed to parse XML");
    return false;
  }

  saveMetaCache();

  // Free TOC strings, rebuild as compact LUT from cache
  std::vector<TocItem>().swap(tocItems_);
  if (!loadMetaCache()) {
    LOG_ERR(TAG, "Failed to reload meta cache for LUT");
  }

  loaded = true;
  LOG_INF(TAG, "Loaded FB2: %s (title: '%s', author: '%s')", filepath.c_str(), title.c_str(), author.c_str());
  return true;
}

void XMLCALL Fb2::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<Fb2*>(userData);

  self->depth++;

  // Prevent stack overflow from deeply nested XML
  if (self->depth >= 100) {
    return;
  }

  // Skip content inside <binary> tags (embedded images)
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // FB2 uses namespaces, strip prefix if present
  const char* tag = strrchr(name, ':');
  if (tag) {
    tag++;
  } else {
    tag = name;
  }

  // <binary id="..." content-type="..."> — capture the offset / length so we
  // can resolve <image l:href="#id"/> references at chapter parse time
  // without a second full-file scan.  We still skip the base64 content
  // here (skipUntilDepth) — we don't want to materialise it through Expat.
  if (strcmp(tag, "binary") == 0) {
    self->skipUntilDepth = self->depth - 1;

    self->currentBinaryId_.clear();
    self->currentBinaryMime_ = 2;  // 2 = "other / unknown"
    self->currentBinaryStart_ = 0;
    if (atts) {
      for (int i = 0; atts[i]; i += 2) {
        const char* aname = atts[i];
        const char* avalue = atts[i + 1];
        const char* an = strrchr(aname, ':');
        an = an ? (an + 1) : aname;
        if (strcmp(an, "id") == 0 && avalue) {
          self->currentBinaryId_ = avalue;
        } else if (strcmp(an, "content-type") == 0 && avalue) {
          if (strcmp(avalue, "image/jpeg") == 0) self->currentBinaryMime_ = 0;
          else if (strcmp(avalue, "image/png") == 0) self->currentBinaryMime_ = 1;
          else self->currentBinaryMime_ = 2;
        }
      }
    }
    if (!self->currentBinaryId_.empty() && self->xmlParser_) {
      const long byteIndex = XML_GetCurrentByteIndex(self->xmlParser_);
      if (byteIndex >= 0) {
        self->currentBinaryStart_ = static_cast<uint32_t>(byteIndex);
      }
    }
    return;
  }

  // Track <title-info> to only collect metadata from it (not <document-info>)
  if (strcmp(tag, "title-info") == 0) {
    self->inTitleInfo = true;
  }

  // Description / Metadata (only from <title-info>)
  if (strcmp(tag, "book-title") == 0 && self->inTitleInfo) {
    self->inBookTitle = true;
    self->title.clear();
  } else if (strcmp(tag, "author") == 0 && self->inTitleInfo) {
    self->inAuthor = true;
    self->currentAuthorFirst.clear();
    self->currentAuthorLast.clear();
  } else if (strcmp(tag, "first-name") == 0 && self->inAuthor) {
    self->inFirstName = true;
  } else if (strcmp(tag, "last-name") == 0 && self->inAuthor) {
    self->inLastName = true;
  } else if (strcmp(tag, "coverpage") == 0) {
    self->inCoverPage = true;
  } else if (strcmp(tag, "image") == 0 && self->inCoverPage) {
    // Look for l:href or href attribute
    if (atts) {
      for (int i = 0; atts[i]; i += 2) {
        const char* attrName = atts[i];
        const char* attrValue = atts[i + 1];

        // Handle both l:href and href
        const char* attr = strrchr(attrName, ':');
        if (attr)
          attr++;
        else
          attr = attrName;

        if ((strcmp(attr, "href") == 0 || strcmp(attrName, "l:href") == 0) && attrValue) {
          // Store the reference (remove # prefix)
          if (attrValue[0] == '#') {
            self->coverPath = attrValue + 1;
          } else {
            self->coverPath = attrValue;
          }
          LOG_INF(TAG, "Found cover reference: %s", self->coverPath.c_str());
          break;
        }
      }
    }
  } else if (strcmp(tag, "body") == 0) {
    self->bodyCount_++;
    self->inBody = (self->bodyCount_ == 1);
  } else if (strcmp(tag, "section") == 0 && self->inBody) {
    self->sectionCounter_++;
    self->sectionDepth_++;
    const long byteIndex = self->xmlParser_ ? XML_GetCurrentByteIndex(self->xmlParser_) : -1;
    self->currentSectionOffset_ = byteIndex >= 0 ? static_cast<uint32_t>(byteIndex) : 0;
  } else if (strcmp(tag, "title") == 0 && self->inBody && self->sectionCounter_ > 0) {
    self->inSectionTitle_ = true;
    self->sectionTitleDepth_ = self->depth;
    self->currentSectionTitle_.clear();
  }
}

void XMLCALL Fb2::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<Fb2*>(userData);

  // FB2 uses namespaces, strip prefix if present
  const char* tag = strrchr(name, ':');
  if (tag) {
    tag++;
  } else {
    tag = name;
  }

  if (strcmp(tag, "title-info") == 0) {
    self->inTitleInfo = false;
  }

  if (strcmp(tag, "book-title") == 0) {
    self->inBookTitle = false;
  } else if (strcmp(tag, "first-name") == 0) {
    self->inFirstName = false;
  } else if (strcmp(tag, "last-name") == 0) {
    self->inLastName = false;
  } else if (strcmp(tag, "author") == 0 && self->inAuthor) {
    // Combine first and last name for author
    std::string fullAuthor;
    if (!self->currentAuthorFirst.empty()) {
      fullAuthor = self->currentAuthorFirst;
      if (!self->currentAuthorLast.empty()) {
        fullAuthor += " ";
      }
    }
    fullAuthor += self->currentAuthorLast;

    if (!fullAuthor.empty()) {
      if (!self->author.empty()) {
        self->author += ", ";
      }
      self->author += fullAuthor;
    }

    self->inAuthor = false;
    self->currentAuthorFirst.clear();
    self->currentAuthorLast.clear();
  } else if (strcmp(tag, "coverpage") == 0) {
    self->inCoverPage = false;
  } else if (strcmp(tag, "binary") == 0) {
    // Exit binary tag - stop skipping
    self->skipUntilDepth = INT_MAX;
    if (!self->currentBinaryId_.empty() && self->xmlParser_) {
      const long byteIndex = XML_GetCurrentByteIndex(self->xmlParser_);
      if (byteIndex >= 0 && self->currentBinaryStart_ > 0 &&
          static_cast<uint32_t>(byteIndex) > self->currentBinaryStart_) {
        BinaryEntry entry;
        entry.fileOffset = self->currentBinaryStart_;
        entry.byteLength = static_cast<uint32_t>(byteIndex) - self->currentBinaryStart_;
        entry.mimeType = self->currentBinaryMime_;
        // v2.0.179 — sorted-vector replacement for unordered_map (see Fb2.h).
        self->binaryIndexInsert(self->currentBinaryId_, entry);
      }
    }
    self->currentBinaryId_.clear();
    self->currentBinaryStart_ = 0;
    self->currentBinaryMime_ = 0;
  } else if (strcmp(tag, "body") == 0) {
    self->inBody = false;
  } else if (strcmp(tag, "title") == 0 && self->inSectionTitle_ && self->depth == self->sectionTitleDepth_) {
    self->inSectionTitle_ = false;

    // Trim whitespace and replace newlines with spaces
    std::string& t = self->currentSectionTitle_;
    for (size_t i = 0; i < t.size(); i++) {
      if (t[i] == '\n' || t[i] == '\r') {
        t[i] = ' ';
      }
    }
    // Trim leading whitespace
    size_t start = 0;
    while (start < t.size() && isspace(static_cast<unsigned char>(t[start]))) {
      start++;
    }
    // Trim trailing whitespace
    size_t end = t.size();
    while (end > start && isspace(static_cast<unsigned char>(t[end - 1]))) {
      end--;
    }
    if (start > 0 || end < t.size()) {
      t = t.substr(start, end - start);
    }

    // v3.1.1 — cap the TOC at 512 entries.  The chapters-menu view holds 192
    // anyway; unbounded growth on a pathological many-section FB2 is exactly
    // the vector-doubling that exhausted the heap in the v2.0.196/v3.0.x
    // hardware crashes (push_back → realloc → bad_alloc mid-parse).
    if (!t.empty() && self->tocItems_.size() < 512) {
      TocItem item;
      item.title = t;
      item.sectionIndex = self->sectionCounter_ - 1;
      item.sourceOffset = self->currentSectionOffset_;
      item.depth = self->sectionDepth_ > 0 ? static_cast<uint8_t>(self->sectionDepth_ - 1) : 0;
      self->tocItems_.push_back(std::move(item));
    }
  } else if (strcmp(tag, "section") == 0 && self->inBody && self->sectionDepth_ > 0) {
    self->sectionDepth_--;
  }

  self->depth--;
}

void XMLCALL Fb2::characterData(void* userData, const XML_Char* s, int len) {
  auto* self = static_cast<Fb2*>(userData);

  // Skip if inside binary tags
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  // v3.1.1 — cap every accumulator.  These strings are display-only (TOC
  // titles truncate at 48 chars on screen, book title/author fit one line);
  // a malformed FB2 with megabytes of text inside <book-title> must not be
  // able to grow them until the heap dies.  512 B ≈ 250 Cyrillic chars —
  // far beyond anything renderable.
  constexpr size_t kMetaFieldCap = 512;

  // Collect section title text for TOC
  if (self->inSectionTitle_ && self->currentSectionTitle_.size() < kMetaFieldCap) {
    self->currentSectionTitle_.append(s, len);
  }

  // Extract metadata based on current context
  if (self->inBookTitle) {
    if (self->title.size() < kMetaFieldCap) self->title.append(s, len);
  } else if (self->inFirstName) {
    if (self->currentAuthorFirst.size() < kMetaFieldCap) self->currentAuthorFirst.append(s, len);
  } else if (self->inLastName) {
    if (self->currentAuthorLast.size() < kMetaFieldCap) self->currentAuthorLast.append(s, len);
  }
}

// v3.1.1 — see the header-side comment on the *Guarded trio.  The catch
// must live HERE, inside the C++ callback frame, because an exception
// can NOT reliably unwind through Expat's C frames (no unwind tables →
// __cxa_call_terminate → abort → device reboot; reproduced on hardware
// even with parseXmlStream's outer try/catch present).
void XMLCALL Fb2::startElementGuarded(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<Fb2*>(userData);
  try {
    startElement(userData, name, atts);
  } catch (...) {
    self->metaParseOom_ = true;
    if (self->xmlParser_) XML_StopParser(self->xmlParser_, XML_FALSE);
  }
}

void XMLCALL Fb2::endElementGuarded(void* userData, const XML_Char* name) {
  auto* self = static_cast<Fb2*>(userData);
  try {
    endElement(userData, name);
  } catch (...) {
    self->metaParseOom_ = true;
    if (self->xmlParser_) XML_StopParser(self->xmlParser_, XML_FALSE);
  }
}

void XMLCALL Fb2::characterDataGuarded(void* userData, const XML_Char* s, int len) {
  auto* self = static_cast<Fb2*>(userData);
  try {
    characterData(userData, s, len);
  } catch (...) {
    self->metaParseOom_ = true;
    if (self->xmlParser_) XML_StopParser(self->xmlParser_, XML_FALSE);
  }
}

bool Fb2::parseXmlStream() {
  LOG_INF(TAG, "Starting streaming XML parse");

  FsFile file;
  if (!SdMan.openFileForRead("FB2", filepath, file)) {
    return false;
  }

  xmlParser_ = XML_ParserCreate("UTF-8");
  if (!xmlParser_) {
    LOG_ERR(TAG, "Failed to create XML parser");
    closeFileProtected(file);
    return false;
  }

  XML_SetUserData(xmlParser_, this);
  // v3.1.1 — register the OOM-guarded wrappers (see their definitions
  // above): an alloc failure inside a handler must be caught in the C++
  // callback frame, not after unwinding through Expat's C frames.
  XML_SetElementHandler(xmlParser_, startElementGuarded, endElementGuarded);
  XML_SetCharacterDataHandler(xmlParser_, characterDataGuarded);
  metaParseOom_ = false;

  constexpr size_t kChunkSize = 4096;
  uint8_t buffer[kChunkSize];
  bool success = true;

  // v2.0.197 — wrap the parse loop in try/catch so a heap-OOM during a
  // callback's vector::push_back / std::string append (TocItem accumulation,
  // binary index, currentSectionTitle_ concatenation) becomes a graceful
  // load failure instead of `abort()` via the C++ unwind machinery.
  //
  // Backstory: v2.0.196 hardware repro crashed at:
  //   Fb2::endElement → std::vector<TocItem>::push_back → _M_realloc_append
  //   → __throw_length_error/bad_alloc → propagates through expat's C
  //   callback dispatch → no catch in parseXmlStream → terminate → abort
  // The unwind machinery shows up in the panic backtrace
  // (__cxa_call_terminate, _Unwind_RaiseException_Phase2 etc.).  Local
  // catch here intercepts BEFORE it crosses into expat's C frames again.
  try {
    while (true) {
      int bytesRead = 0;
      int done = 0;
      {
        snapix::spi::SharedBusLock lk;
        if (file.available() <= 0) break;
        bytesRead = file.read(buffer, kChunkSize);
        if (bytesRead <= 0) break;
        done = (file.available() == 0) ? 1 : 0;
      }

      if (XML_Parse(xmlParser_, reinterpret_cast<const char*>(buffer), bytesRead, done) ==
          XML_STATUS_ERROR) {
        LOG_ERR(TAG, "XML parse error: %s", XML_ErrorString(XML_GetErrorCode(xmlParser_)));
        success = false;
        break;
      }
    }
  } catch (const std::bad_alloc& e) {
    LOG_ERR(TAG, "XML parse OOM (heap exhausted growing toc/binary/title): %s", e.what());
    success = false;
  } catch (const std::length_error& e) {
    LOG_ERR(TAG, "XML parse length_error (vector size limit): %s", e.what());
    success = false;
  } catch (const std::exception& e) {
    LOG_ERR(TAG, "XML parse exception: %s", e.what());
    success = false;
  } catch (...) {
    LOG_ERR(TAG, "XML parse: unknown exception");
    success = false;
  }

  // v3.1.1 — a guarded handler caught an alloc failure and stopped the
  // parser.  XML_Parse already returned XML_STATUS_ERROR (ABORTED), but
  // surface the real cause instead of a generic parse-error log.
  if (metaParseOom_) {
    LOG_ERR(TAG, "XML parse aborted: heap exhausted in a metadata callback (graceful, no crash)");
    success = false;
  }

  closeFileProtected(file);

  if (success) {
    postProcessMetadata();
  }

  XML_ParserFree(xmlParser_);
  xmlParser_ = nullptr;
  return success;
}

void Fb2::postProcessMetadata() {
  // Clean up title (remove newlines and extra whitespace)
  while (!title.empty() && isspace(static_cast<unsigned char>(title.back()))) {
    title.pop_back();
  }
  while (!title.empty() && isspace(static_cast<unsigned char>(title.front()))) {
    title.erase(title.begin());
  }

  // Replace newlines in title with spaces
  for (size_t i = 0; i < title.size(); i++) {
    if (title[i] == '\n' || title[i] == '\r') {
      title[i] = ' ';
    }
  }

  LOG_INF(TAG, "XML parsing complete: title='%s', author='%s'", title.c_str(), author.c_str());
}

bool Fb2::clearCache() const {
  // v2.0.60: cache lives on LittleFS now, not SD.  Recursively walk the
  // book's cache subtree (page-cache section files plus the LittleFS image
  // cache dir if it exists) and remove everything.
  if (!LittleFS.exists(cachePath.c_str())) {
    LOG_INF(TAG, "Cache does not exist, no action needed");
    return true;
  }

  // Recursive rmTree — Arduino LittleFS::rmdir is non-recursive.
  std::function<bool(const std::string&)> rmTree = [&](const std::string& p) -> bool {
    if (!LittleFS.exists(p.c_str())) return true;
    File dir = LittleFS.open(p.c_str(), "r");
    if (!dir) return false;
    if (!dir.isDirectory()) {
      dir.close();
      return LittleFS.remove(p.c_str());
    }
    File entry = dir.openNextFile();
    while (entry) {
      const std::string entryPath = p + "/" + entry.name();
      const bool isDir = entry.isDirectory();
      entry.close();
      if (isDir) {
        if (!rmTree(entryPath)) {
          dir.close();
          return false;
        }
      } else {
        LittleFS.remove(entryPath.c_str());
      }
      entry = dir.openNextFile();
    }
    dir.close();
    return LittleFS.rmdir(p.c_str());
  };

  if (!rmTree(cachePath)) {
    LOG_ERR(TAG, "Failed to clear cache");
    return false;
  }

  // Image cache lives at /img/<book>/.  Wipe that too for symmetry with the
  // pre-LittleFS clear semantics.
  const std::string imagesDir = flashImageCacheDir(cachePath);
  if (LittleFS.exists(imagesDir.c_str())) {
    rmTree(imagesDir);
  }

  LOG_INF(TAG, "Cache cleared successfully");
  return true;
}

void Fb2::setupCacheDir() const {
  // v2.0.60: cache moved to LittleFS.  Recursive mkdir via the same helper
  // used by image cache (ensureFlashDir defined earlier in this file).
  if (!ensureFlashDir(cachePath)) {
    LOG_ERR(TAG, "Failed to create cache dir: %s", cachePath.c_str());
  }

  // Always verify sections/ exists — partial cache clear may have removed it.
  // ensureFlashDir handles the recursive case (LittleFS::mkdir is not
  // recursive, but ensureFlashDir walks parents).
  const auto sectionsDir = cachePath + "/sections";
  if (!ensureFlashDir(sectionsDir)) {
    LOG_ERR(TAG, "Failed to create sections dir: %s", sectionsDir.c_str());
  }

  // v2.0.167 one-shot orphan-cleanup pass for upgraders from <=2.0.165.
  // Removes the legacy `markers/` subtree — markers + idx now live as
  // UnifiedCache segments in `streaming.cache`.  Same rmtree pattern
  // Epub::setupCacheDir uses.
  std::function<bool(const std::string&)> rmTree = [&](const std::string& p) -> bool {
    if (!LittleFS.exists(p.c_str())) return true;
    File dir = LittleFS.open(p.c_str(), "r");
    if (!dir) return false;
    if (!dir.isDirectory()) {
      dir.close();
      return LittleFS.remove(p.c_str());
    }
    File entry = dir.openNextFile();
    while (entry) {
      const std::string entryPath = p + "/" + entry.name();
      const bool isDir = entry.isDirectory();
      entry.close();
      if (isDir) {
        if (!rmTree(entryPath)) {
          dir.close();
          return false;
        }
      } else {
        LittleFS.remove(entryPath.c_str());
      }
      entry = dir.openNextFile();
    }
    dir.close();
    return LittleFS.rmdir(p.c_str());
  };
  const std::string markersDir = cachePath + "/markers";
  if (LittleFS.exists(markersDir.c_str()) && rmTree(markersDir)) {
    LOG_INF(TAG, "[CONTENT][FB2] cleanup: removed legacy markers/ dir from %s (migrated to UnifiedCache)",
            cachePath.c_str());
  }
}

std::string Fb2::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

std::string Fb2::findCoverImage() const {
  // Extract directory path
  size_t lastSlash = filepath.find_last_of('/');
  std::string dirPath = (lastSlash == std::string::npos) ? "/" : filepath.substr(0, lastSlash);
  if (dirPath.empty()) {
    dirPath = "/";
  }

  return CoverHelpers::findCoverImage(dirPath, title);
}

bool Fb2::generateCoverBmp(bool use1BitDithering) const {
  const auto coverPath = getCoverBmpPath();
  // v2.0.72: cover BMP and failure marker BOTH on LittleFS (cachePath is
  // /cache/fb2_<hash>).  Pre-v2.0.72 the cover.exists check ran against SD,
  // which always returned false for the LittleFS-rooted path, so the cover
  // got re-generated on every entry — and the regenerated file ALSO landed
  // on SD via SdMan in the converter.  Now both checks use LittleFS and the
  // generated BMP lands on LittleFS via outputOnLittleFs=true in
  // CoverHelpers::convertImageToBmp.
  const auto failedMarkerPath = cachePath + "/.cover.failed";

  // Already generated
  if (LittleFS.exists(coverPath.c_str())) {
    return true;
  }

  // Previously failed, don't retry
  if (LittleFS.exists(failedMarkerPath.c_str())) {
    return false;
  }

  // Find a cover image
  std::string coverImagePath = findCoverImage();
  if (coverImagePath.empty()) {
    LOG_INF(TAG, "No cover image found");
    // Create failure marker on LittleFS.  Make sure parent dir exists first
    // (cache dir might not have been created if the book has no images).
    setupCacheDir();
    File marker = LittleFS.open(failedMarkerPath.c_str(), "w");
    if (marker) {
      marker.close();
    }
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  // Convert to BMP using shared helper
  const bool success = CoverHelpers::convertImageToBmp(coverImagePath, coverPath, "FB2", use1BitDithering);
  if (!success) {
    File marker = LittleFS.open(failedMarkerPath.c_str(), "w");
    if (marker) {
      marker.close();
    }
  }
  return success;
}

namespace {
// Compute the scaled-fit output dimensions that a real decode would produce —
// has to mirror what JpegToBmpConverter / PngToBmpConverter do internally so
// the placeholder layout matches the eventual rendered image.
void scaledFit(int srcW, int srcH, int maxW, int maxH, int& outW, int& outH) {
  if (srcW <= 0 || srcH <= 0) {
    outW = 0;
    outH = 0;
    return;
  }
  if (maxW <= 0 || maxH <= 0 || (srcW <= maxW && srcH <= maxH)) {
    outW = srcW;
    outH = srcH;
    return;
  }
  const double sx = static_cast<double>(maxW) / srcW;
  const double sy = static_cast<double>(maxH) / srcH;
  const double scale = sx < sy ? sx : sy;
  outW = static_cast<int>(srcW * scale);
  outH = static_cast<int>(srcH * scale);
  if (outW < 1) outW = 1;
  if (outH < 1) outH = 1;
}

// Read BMP header at offsets 18/22 with absolute-value handling for the
// negative-height (top-down DIB) flag JpegToBmpConverter / PngToBmpConverter
// emit.  Returns true and fills outW/outH on success.
bool readBmpDimensions(const std::string& bmpPath, uint16_t& outW, uint16_t& outH) {
  FsFile bf;
  if (!SdMan.openFileForRead("FB2", bmpPath, bf)) return false;
  uint8_t hdr[26];
  int got = 0;
  {
    snapix::spi::SharedBusLock lk;
    got = bf.read(hdr, sizeof(hdr));
  }
  bf.close();
  if (got != static_cast<int>(sizeof(hdr)) || hdr[0] != 'B' || hdr[1] != 'M') return false;
  const int32_t w32 = static_cast<int32_t>(hdr[18]) | (static_cast<int32_t>(hdr[19]) << 8) |
                      (static_cast<int32_t>(hdr[20]) << 16) | (static_cast<int32_t>(hdr[21]) << 24);
  const int32_t h32 = static_cast<int32_t>(hdr[22]) | (static_cast<int32_t>(hdr[23]) << 8) |
                      (static_cast<int32_t>(hdr[24]) << 16) | (static_cast<int32_t>(hdr[25]) << 24);
  const int32_t aw = w32 < 0 ? -w32 : w32;
  const int32_t ah = h32 < 0 ? -h32 : h32;
  if (aw <= 0 || aw > 0xFFFF || ah <= 0 || ah > 0xFFFF) return false;
  outW = static_cast<uint16_t>(aw);
  outH = static_cast<uint16_t>(ah);
  return true;
}

// Fully decode a pending JPEG / PNG file into the matching BMP.  Common path
// shared between the synchronous fastMode=false and the BG worker.
//
// ATOMIC WRITE: decode into <bmpPath>.tmp, rename to <bmpPath> only after
// the converter signals success.  Without this, an aborted / failed decode
// leaves a partially-written file on disk that ImageBlock::render then
// opens, fails to parse ("NotBMP (missing 'BM')"), and falls back to the
// "[Image]" placeholder forever — even after a subsequent successful
// decode would have replaced it, because cacheImage's "BMP already exists"
// fast-path short-circuits before re-decoding.  Rename is atomic on FAT,
// so the reader thread either sees a complete valid BMP or no file at all.
bool decodeOnePending(const std::string& pendingPath, bool isPng, const std::string& bmpPath, int maxBoxWidth,
                      int maxBoxHeight, const std::function<bool()>& shouldAbort) {
  // v2.0.53: BMP output target is on LittleFS (internal flash) instead of
  // SD.  Source pending file (PNG / legacy JPEG) is still on SD because
  // pngle / SdFat work with FsFile.  Atomic write via .tmp + rename is
  // honoured on LittleFS.
  FsFile srcFile;
  if (!SdMan.openFileForRead("FB2", pendingPath, srcFile)) return false;
  const std::string tmpBmpPath = bmpPath + ".tmp";
  if (LittleFS.exists(tmpBmpPath.c_str())) LittleFS.remove(tmpBmpPath.c_str());

  File bmpFile = LittleFS.open(tmpBmpPath.c_str(), "w");
  if (!bmpFile) {
    srcFile.close();
    return false;
  }
  const bool ok =
      isPng ? PngToBmpConverter::pngFileToBmpStreamQuick(srcFile, bmpFile, maxBoxWidth, maxBoxHeight, shouldAbort)
            : JpegToBmpConverter::jpegFileToBmpStreamQuick(srcFile, bmpFile, maxBoxWidth, maxBoxHeight, shouldAbort);
  srcFile.close();
  bmpFile.close();
  if (!ok) {
    LittleFS.remove(tmpBmpPath.c_str());
    return false;
  }
  if (LittleFS.exists(bmpPath.c_str())) LittleFS.remove(bmpPath.c_str());
  if (!LittleFS.rename(tmpBmpPath.c_str(), bmpPath.c_str())) {
    LOG_ERR(TAG, "decodeOnePending: failed to promote %s -> %s", tmpBmpPath.c_str(), bmpPath.c_str());
    LittleFS.remove(tmpBmpPath.c_str());
    return false;
  }
  return true;
}
}  // namespace

bool Fb2::cacheImage(const std::string& binaryId, std::string& outBmpPath, uint16_t& outWidth, uint16_t& outHeight,
                     int maxBoxWidth, int maxBoxHeight, bool fastMode,
                     const std::function<bool()>& shouldAbort) const {
  if (binaryId.empty()) return false;
  // v2.0.179 — sorted-vector lookup (was unordered_map.find).
  const BinaryEntry* entryPtr = binaryIndexFind(binaryId);
  if (entryPtr == nullptr) {
    LOG_DBG(TAG, "cacheImage: binary id not in index: %s", binaryId.c_str());
    return false;
  }
  const BinaryEntry& entry = *entryPtr;
  // mimeType: 0 = JPEG, 1 = PNG, 2+ = unsupported.  Anything else is rejected.
  if (entry.byteLength == 0 || entry.mimeType >= 2) {
    return false;
  }
  const bool isPng = (entry.mimeType == 1);
  const char* extDot = isPng ? ".png" : ".jpg";

  // v2.0.53: BMP cache lives on internal flash (LittleFS), not SD.  See
  // the long comment above flashImageCacheDir for the rationale.  PNG
  // pending files are still on SD because pngle requires a real seekable
  // file (no pump path for PNG yet).
  const std::string imagesDir = flashImageCacheDir(cachePath);
  const std::string pendingDir = cachePath + "/images/pending";
  const std::string bmpPath = imagesDir + "/" + binaryId + ".bmp";
  const std::string failPath = imagesDir + "/" + binaryId + ".failed";
  const std::string pendingPath = pendingDir + "/" + binaryId + extDot;

  // Already fully decoded (idempotent) — fast path for both modes.  Probe
  // LittleFS, then peek BMP dims via a small flash read.
  if (LittleFS.exists(bmpPath.c_str())) {
    File hf = LittleFS.open(bmpPath.c_str(), "r");
    if (hf) {
      uint8_t hdr[26];
      const size_t got = hf.read(hdr, sizeof(hdr));
      hf.close();
      if (got == sizeof(hdr) && hdr[0] == 'B' && hdr[1] == 'M') {
        const int32_t w32 = static_cast<int32_t>(hdr[18]) | (static_cast<int32_t>(hdr[19]) << 8) |
                            (static_cast<int32_t>(hdr[20]) << 16) | (static_cast<int32_t>(hdr[21]) << 24);
        const int32_t h32 = static_cast<int32_t>(hdr[22]) | (static_cast<int32_t>(hdr[23]) << 8) |
                            (static_cast<int32_t>(hdr[24]) << 16) | (static_cast<int32_t>(hdr[25]) << 24);
        const int32_t aw = w32 < 0 ? -w32 : w32;
        const int32_t ah = h32 < 0 ? -h32 : h32;
        if (aw > 0 && aw <= 0xFFFF && ah > 0 && ah <= 0xFFFF) {
          outWidth = static_cast<uint16_t>(aw);
          outHeight = static_cast<uint16_t>(ah);
          outBmpPath = bmpPath;
          return true;
        }
      }
    }
    // Header read failed / absurd dimensions — drop the broken BMP.
    LOG_INF(TAG, "cacheImage: removing corrupt %s (no 'BM' header / bad dims)", bmpPath.c_str());
    LittleFS.remove(bmpPath.c_str());
  }

  // Previous attempt failed sentinel — don't retry the same decode every
  // page render in case the source is malformed / OOM-bait.
  if (LittleFS.exists(failPath.c_str())) {
    return false;
  }

  // Ensure flash images dir exists (cheap if it does).  ensureFlashDir is
  // recursive — Arduino LittleFS::mkdir bails on missing parents.  Pending
  // dir on SD is only needed for PNG / synchronous paths; created below.
  if (!ensureFlashDir(imagesDir)) {
    LOG_ERR(TAG, "cacheImage: failed to create flash images dir: %s", imagesDir.c_str());
    return false;
  }

  // v2.0.50 fast-mode JPEG path skips Step 1 entirely — `peekImageDims` and
  // `decodeImageDirect` stream base64 from FB2 directly through the
  // Base64JpegPump.  Only PNG fastMode and synchronous (non-fastMode) paths
  // still need the pending JPEG/PNG on SD.
  const bool needPendingFile = !fastMode || isPng;
  if (needPendingFile) {
    if (!SdMan.exists(pendingDir.c_str())) {
      if (!SdMan.mkdir(pendingDir.c_str())) {
        LOG_ERR(TAG, "cacheImage: failed to create pending dir: %s", pendingDir.c_str());
        return false;
      }
    }
    if (!SdMan.exists(pendingPath.c_str())) {
      if (!streamDecodeBase64ToJpegFile(filepath, entry.fileOffset, entry.byteLength, pendingPath)) {
        LOG_ERR(TAG, "cacheImage: base64 decode failed for id=%s", binaryId.c_str());
        SdMan.remove(pendingPath.c_str());
        File m = LittleFS.open(failPath.c_str(), "w");
        if (m) m.close();
        return false;
      }
    }
  }
  (void)shouldAbort;  // Retained in signature for callers' convenience; ignored here.

  if (fastMode) {
    // Step 2a (fast): peek dims from the source header.  v2.0.50: peek runs
    // BEFORE we ever write a pending file — in fact for JPEGs we don't write
    // one at all on the fast path.  Base64JpegPump streams just enough bytes
    // (~1 KB) directly from the FB2 source to find the JPEG SOF marker.
    int srcW = 0, srcH = 0;
    if (isPng) {
      // PNG: pending file was already streamed to disk by the outer
      // needPendingFile block.  Peek with pngle.
      FsFile srcFile;
      if (!SdMan.openFileForRead("FB2", pendingPath, srcFile)) return false;
      const bool peekOk = PngToBmpConverter::peekDimensions(srcFile, srcW, srcH);
      srcFile.close();
      if (!peekOk) {
        LOG_ERR(TAG, "cacheImage: PNG header peek failed for id=%s", binaryId.c_str());
        SdMan.remove(pendingPath.c_str());
        File m = LittleFS.open(failPath.c_str(), "w");
        if (m) m.close();
        return false;
      }
    } else {
      // JPEG: pump-based peek directly from FB2.  No pending file needed.
      uint16_t pw = 0, ph = 0;
      if (!peekImageDims(binaryId, pw, ph)) {
        LOG_ERR(TAG, "cacheImage: JPEG SOF peek failed for id=%s", binaryId.c_str());
        File m = LittleFS.open(failPath.c_str(), "w");
        if (m) m.close();
        return false;
      }
      srcW = pw;
      srcH = ph;
    }

    int outW = 0, outH = 0;
    scaledFit(srcW, srcH, maxBoxWidth, maxBoxHeight, outW, outH);
    if (outW <= 0 || outH <= 0 || outW > 0xFFFF || outH > 0xFFFF) return false;
    outWidth = static_cast<uint16_t>(outW);
    outHeight = static_cast<uint16_t>(outH);
    outBmpPath = bmpPath;  // Will exist later, after decodeImageDirect() / decodePendingImages() runs.

    // Queue the binary for BG decode.  For JPEGs the BG worker will use
    // decodeImageDirect — no pending file ever lands on SD.  PNGs are
    // already on disk (we wrote pending/<id>.png above) and the BG worker's
    // legacy pending-dir scan will pick them up; we still queue them here
    // so a single drain loop covers both.
    if (!isPng) {
      bool already = false;
      for (const auto& q : pendingJpegDecodes_) {
        if (q.binaryId == binaryId) {
          already = true;
          break;
        }
      }
      if (!already) {
        pendingJpegDecodes_.push_back({binaryId, maxBoxWidth, maxBoxHeight});
      }
    }
    LOG_INF(TAG, "cacheImage[fast]: registered %s src=%dx%d -> %dx%d (BMP pending)", binaryId.c_str(), srcW, srcH, outW,
            outH);
    return true;
  }

  // Step 2b (sync): full pixel decode.  Output BMP lands on LittleFS
  // (decodeOnePending writes via Arduino File API now).
  if (!decodeOnePending(pendingPath, isPng, bmpPath, maxBoxWidth, maxBoxHeight, nullptr)) {
    LOG_ERR(TAG, "cacheImage: %s decode failed for id=%s", isPng ? "PNG" : "JPEG", binaryId.c_str());
    LittleFS.remove(bmpPath.c_str());
    SdMan.remove(pendingPath.c_str());
    File m = LittleFS.open(failPath.c_str(), "w");
    if (m) m.close();
    return false;
  }
  SdMan.remove(pendingPath.c_str());

  // Step 3: read back BMP dimensions for the caller (ImageBlock needs w/h).
  // Peek BMP header from LittleFS (mirrors the cached-fast-path logic).
  {
    File hf = LittleFS.open(bmpPath.c_str(), "r");
    if (hf) {
      uint8_t hdr[26];
      const size_t got = hf.read(hdr, sizeof(hdr));
      hf.close();
      if (got == sizeof(hdr) && hdr[0] == 'B' && hdr[1] == 'M') {
        const int32_t w32 = static_cast<int32_t>(hdr[18]) | (static_cast<int32_t>(hdr[19]) << 8) |
                            (static_cast<int32_t>(hdr[20]) << 16) | (static_cast<int32_t>(hdr[21]) << 24);
        const int32_t h32 = static_cast<int32_t>(hdr[22]) | (static_cast<int32_t>(hdr[23]) << 8) |
                            (static_cast<int32_t>(hdr[24]) << 16) | (static_cast<int32_t>(hdr[25]) << 24);
        const int32_t aw = w32 < 0 ? -w32 : w32;
        const int32_t ah = h32 < 0 ? -h32 : h32;
        if (aw > 0 && aw <= 0xFFFF && ah > 0 && ah <= 0xFFFF) {
          outWidth = static_cast<uint16_t>(aw);
          outHeight = static_cast<uint16_t>(ah);
          outBmpPath = bmpPath;
          LOG_INF(TAG, "cacheImage: decoded %s -> %s (%ux%u)", binaryId.c_str(), bmpPath.c_str(),
                  static_cast<unsigned>(outWidth), static_cast<unsigned>(outHeight));
          return true;
        }
      }
    }
  }

  // BMP exists but unreadable header — give up and mark failed.
  LOG_ERR(TAG, "cacheImage: BMP %s has unreadable / absurd dimensions", bmpPath.c_str());
  LittleFS.remove(bmpPath.c_str());
  File m = LittleFS.open(failPath.c_str(), "w");
  if (m) m.close();
  return false;
}

bool Fb2::hasPendingImages() const {
  // v2.0.50: queued JPEGs (no pending file on SD) AND legacy pending dir
  // (PNGs + leftover JPEGs from older versions) both count as "has work".
  if (!pendingJpegDecodes_.empty()) return true;
  const std::string pendingDir = cachePath + "/images/pending";
  if (!SdMan.exists(pendingDir.c_str())) return false;
  // Open the directory and check for any entry — directories with no entries
  // are treated as "no pending decodes".
  FsFile dir;
  if (!SdMan.openFileForRead("FB2", pendingDir, dir)) return false;
  if (!dir.isDirectory()) {
    dir.close();
    return false;
  }
  FsFile entry;
  bool found = false;
  {
    snapix::spi::SharedBusLock lk;
    while (entry.openNext(&dir, O_RDONLY)) {
      char nameBuf[64];
      const size_t n = entry.getName(nameBuf, sizeof(nameBuf));
      entry.close();
      if (n == 0) continue;
      // Skip "." and ".." entries (some FAT layers expose them).
      if (nameBuf[0] == '.' && (nameBuf[1] == '\0' || (nameBuf[1] == '.' && nameBuf[2] == '\0'))) continue;
      // Match either ".jpg" or ".png" suffix.
      const size_t len = strnlen(nameBuf, sizeof(nameBuf));
      if (len < 4) continue;
      const char* ext = nameBuf + len - 4;
      if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".png") == 0) {
        found = true;
        break;
      }
    }
  }
  dir.close();
  return found;
}

int Fb2::retryDeferredImages(const std::function<bool()>& shouldAbort) const {
  // v2.0.49+: deferred-retry path is dead.  No mid-stream abort branch means
  // deferredImages_ is always empty.  Kept as a no-op so existing call sites
  // in runBackgroundCacheJob and ReaderCacheController don't need rewiring.
  (void)shouldAbort;
  return 0;
}

// ============================================================================
// v2.0.50 direct-stream pipeline implementations.
// ============================================================================

bool Fb2::peekImageDims(const std::string& binaryId, uint16_t& outSrcW, uint16_t& outSrcH) const {
  outSrcW = 0;
  outSrcH = 0;
  if (binaryId.empty()) return false;
  // v2.0.179 — sorted-vector lookup (was unordered_map.find).
  const BinaryEntry* entryPtr = binaryIndexFind(binaryId);
  if (entryPtr == nullptr) return false;
  const BinaryEntry& entry = *entryPtr;
  if (entry.byteLength == 0 || entry.mimeType >= 2) return false;
  if (entry.mimeType == 1) return false;  // PNG: peek not implemented (rare for inline FB2 figs)

  // v2.0.179 — linear-scan lookup of small peekedSourceDims_ cache (was map.find).
  // Cache hit?  We already peeked this binary earlier in the session.
  for (const auto& cached : peekedSourceDims_) {
    if (cached.id == binaryId) {
      outSrcW = cached.width;
      outSrcH = cached.height;
      return outSrcW > 0 && outSrcH > 0;
    }
  }

  FsFile src;
  if (!SdMan.openFileForRead("FB2", filepath, src)) return false;

  Base64JpegPump pump;
  pump.init(src, entry.fileOffset, entry.byteLength);

  // Pull just enough JPEG bytes to find SOF.  1 KB is comfortably larger
  // than any baseline JPEG header — typical SOF lives within the first
  // 200-500 bytes after SOI + DQT + APP segments.
  uint8_t header[1024];
  JPEGFILE jpegFile{};
  jpegFile.fHandle = &pump;
  const int32_t got = Base64JpegPump::pfnRead(&jpegFile, header, sizeof(header));
  src.close();

  if (got < 16) return false;

  uint16_t w = 0, h = 0;
  if (!parseJpegSofDims(header, static_cast<size_t>(got), &w, &h)) return false;
  if (w == 0 || h == 0) return false;

  outSrcW = w;
  outSrcH = h;
  // v2.0.179 — append to linear-scan cache (was map.operator[]).
  peekedSourceDims_.push_back({binaryId, w, h});
  return true;
}

bool Fb2::decodeImageDirect(const std::string& binaryId, int targetMaxWidth, int targetMaxHeight,
                            const std::function<bool()>& shouldAbort) const {
  if (binaryId.empty()) return false;
  // v2.0.179 — sorted-vector lookup (was unordered_map.find).
  const BinaryEntry* entryPtr = binaryIndexFind(binaryId);
  if (entryPtr == nullptr) return false;
  const BinaryEntry& entry = *entryPtr;
  if (entry.byteLength == 0 || entry.mimeType >= 2) return false;
  if (entry.mimeType == 1) {
    // PNG fallthrough: pngle has no equivalent stream-pump interface in this
    // codebase.  The legacy pending/<id>.png + decodeOnePending path still
    // works for PNGs.  Caller (decodePendingImages) handles this.
    return false;
  }

  // v2.0.53: BMP cache lives on internal flash via LittleFS.  Idempotent
  // fast path — if the BMP is already on flash, nothing to do.
  const std::string imagesDir = flashImageCacheDir(cachePath);
  const std::string bmpPath = imagesDir + "/" + binaryId + ".bmp";
  const std::string failPath = imagesDir + "/" + binaryId + ".failed";
  if (LittleFS.exists(bmpPath.c_str())) return true;
  if (LittleFS.exists(failPath.c_str())) return false;

  // Ensure target dir exists (cheap if it does).  Recursive mkdir because
  // Arduino LittleFS::mkdir doesn't auto-create parents.
  if (!ensureFlashDir(imagesDir)) {
    LOG_ERR(TAG, "decodeImageDirect: failed to create flash images dir: %s", imagesDir.c_str());
    return false;
  }

  FsFile src;
  if (!SdMan.openFileForRead("FB2", filepath, src)) return false;

  Base64JpegPump pump;
  pump.init(src, entry.fileOffset, entry.byteLength);

  // Atomic write: BMP lands on .tmp on LittleFS, rename on success.
  // Mirrors decodeOnePending's flow so a partial / aborted decode never
  // leaves a half-written .bmp.  Arduino LittleFS::File inherits Print
  // so we can pass it directly to jpegStreamToBmp's bmpOut sink.
  const std::string tmpPath = bmpPath + ".tmp";
  if (LittleFS.exists(tmpPath.c_str())) LittleFS.remove(tmpPath.c_str());

  File dst = LittleFS.open(tmpPath.c_str(), "w");
  if (!dst) {
    src.close();
    return false;
  }

  bool ok = false;

#if SNAPIX_SMOL_JPEG
  // v2.0.108 (one-decoder discipline — JPEGDEC fallback chain removed):
  // SmolJpeg is the only decoder when the flag is on.  Pre-fix, an
  // `invalid-jpeg` from SmolJpeg fell back to JPEGDEC which
  // instantiated a ~25 KB workspace + ~7 KB arena, fragmenting the
  // heap permanently to largest_free ~5-9 KB for the rest of the
  // session and triggering cascading cold-rebuild livelocks observed
  // in v2.0.107 trace.  Rust-firmware "one decoder, fail explicitly"
  // — when SmolJpeg can't handle a JPEG, return failure and let the
  // caller decide (placeholder), don't silently switch to a heavier
  // alternative that wrecks the heap for everyone else.
  //
  // The v2.0.108 SmolJpeg restart-marker fix
  // (SmolJpegDecode.cpp::decodeBaselineStream) eliminates the
  // "bad restart marker=0x00" failures that this fallback existed
  // to mask.  If SmolJpeg fails NOW, it's a genuine unsupported
  // feature (progressive, non-4:2:0 chroma) that JPEGDEC also
  // couldn't usefully render to e-paper.
  {
    Base64PumpInputStream sin(pump);
    PrintOutputStream     sout(dst);
    const auto status = snapix::smoljpeg::decodeTo1BitBmp(
        sin, sout, targetMaxWidth, targetMaxHeight, nullptr /* shouldAbort thunk — TODO */);
    if (status == snapix::smoljpeg::Status::Ok) {
      ok = true;
    } else {
      LOG_INF(TAG, "SmolJpeg(FB2) decode failed (%s) — no fallback (one-decoder discipline)",
              snapix::smoljpeg::statusToString(status));
      // dst has a partial 1-bit BMP header; tail will clean up via
      // LittleFS.remove(tmpPath).  No JPEGDEC instantiation, heap stays clean.
    }
  }
#else
  // SNAPIX_SMOL_JPEG=0 (default env): legacy JPEGDEC path remains for
  // bit-identical compatibility until SmolJpeg lands in default.
  ok = JpegToBmpConverter::jpegStreamToBmp(
      Base64JpegPump::pfnRead, Base64JpegPump::pfnSeek, Base64JpegPump::pfnClose, &pump,
      static_cast<int32_t>(pump.logicalSize()), dst, targetMaxWidth, targetMaxHeight, shouldAbort);
#endif

  dst.close();
  src.close();

  if (!ok) {
    LittleFS.remove(tmpPath.c_str());
    // Treat ALL decode failures as transient — let the caller's retry
    // counter (decodePendingImages) decide when to give up permanently.
    //
    // v2.0.53 bug context: this branch USED to mark the binary as .failed
    // when shouldAbort() returned false at the time of the post-failure
    // check.  But the abort callback fires when free heap drops below 15 KB
    // — the moment JPEGDEC's ~25 KB workspace gets freed (right after the
    // aborted decode returns), heap recovers above the threshold so
    // shouldAbort() now reads "false".  We mis-classified a heap-low
    // transient as a permanent failure and wrote .failed.  Subsequent
    // parses of the same binary saw the marker and silently dropped the
    // ImageBlock entirely, so the user got "no placeholder, no image,
    // just text" pathology (i_001 in the user's v2.0.53 log).
    LOG_INF(TAG, "decodeImageDirect: %s decode failed (transient or aborted) — caller retries", binaryId.c_str());
    return false;
  }

  if (LittleFS.exists(bmpPath.c_str())) LittleFS.remove(bmpPath.c_str());
  if (!LittleFS.rename(tmpPath.c_str(), bmpPath.c_str())) {
    LOG_ERR(TAG, "decodeImageDirect: failed to promote %s -> %s", tmpPath.c_str(), bmpPath.c_str());
    LittleFS.remove(tmpPath.c_str());
    return false;
  }

  // Peek dims from the freshly-written flash BMP for logging.
  {
    File hf = LittleFS.open(bmpPath.c_str(), "r");
    if (hf) {
      uint8_t hdr[26];
      const size_t got = hf.read(hdr, sizeof(hdr));
      hf.close();
      if (got == sizeof(hdr) && hdr[0] == 'B' && hdr[1] == 'M') {
        const int32_t w32 = static_cast<int32_t>(hdr[18]) | (static_cast<int32_t>(hdr[19]) << 8) |
                            (static_cast<int32_t>(hdr[20]) << 16) | (static_cast<int32_t>(hdr[21]) << 24);
        const int32_t h32 = static_cast<int32_t>(hdr[22]) | (static_cast<int32_t>(hdr[23]) << 8) |
                            (static_cast<int32_t>(hdr[24]) << 16) | (static_cast<int32_t>(hdr[25]) << 24);
        const uint32_t aw = static_cast<uint32_t>(w32 < 0 ? -w32 : w32);
        const uint32_t ah = static_cast<uint32_t>(h32 < 0 ? -h32 : h32);
        LOG_INF(TAG, "decodeImageDirect: %s -> %s (%ux%u)", binaryId.c_str(), bmpPath.c_str(),
                static_cast<unsigned>(aw), static_cast<unsigned>(ah));
        return true;
      }
    }
  }
  LOG_INF(TAG, "decodeImageDirect: %s -> %s (dim read failed)", binaryId.c_str(), bmpPath.c_str());
  return true;
}

int Fb2::decodePendingImages(const std::function<bool()>& shouldAbort) const {
  // ===========================================================================
  // v2.0.50 fast path: drain queued JPEG decodes via Base64JpegPump.
  //
  // For every binaryId enqueued by cacheImage[fast], call decodeImageDirect
  // which streams base64 directly from the FB2 source through a JPEGDEC pump,
  // skipping the legacy pending/<id>.jpg round-trip entirely.  We drain
  // ONE id per sweep so a navigation cancel doesn't have to wait for an
  // arbitrary number of pending decodes to finish.
  // ===========================================================================
  if (!pendingJpegDecodes_.empty()) {
    if (shouldAbort && shouldAbort()) return 0;
    PendingJpegDecode q;
    {
      // Pop the first entry (FIFO order matches the parser's emit order, so
      // images visible on the current page get decoded before far prefetched
      // ones).
      q = pendingJpegDecodes_.front();
      pendingJpegDecodes_.erase(pendingJpegDecodes_.begin());
    }
    if (decodeImageDirect(q.binaryId, q.maxBoxWidth, q.maxBoxHeight, shouldAbort)) {
      return 1;
    }
    // ANY failure → bump retry counter and re-queue.  decodeImageDirect no
    // longer distinguishes abort vs permanent error itself (see v2.0.54
    // context comment in decodeImageDirect): the abort callback can fire
    // and clear within microseconds (heap-low transient), so we can't
    // reliably read the abort state post-failure.  Trust the retry
    // counter — after kMaxJpegDecodeRetries consecutive failures the
    // binary is marked .failed permanently.
    ++q.retries;
    if (q.retries < kMaxJpegDecodeRetries) {
      pendingJpegDecodes_.push_back(q);
    } else {
      LOG_INF(TAG, "decodePendingImages: giving up on %s after %u retries", q.binaryId.c_str(),
              static_cast<unsigned>(q.retries));
      const std::string failPath = flashImageCacheDir(cachePath) + "/" + q.binaryId + ".failed";
      File m = LittleFS.open(failPath.c_str(), "w");
      if (m) m.close();
    }
    return 0;
  }

  // Legacy path (PNGs and any leftover pending/ files from older versions):
  // ===========================================================================
  // v2.0.49 simplified pipeline: 2 stages instead of 5.
  //
  // Old design had preview / low / mid / high / full BMPs (~5 separate decode
  // passes per image, 5 BMP files on disk per image).  Each intermediate
  // stage was a full JPEGDEC decode pass at a different hardware scale, costing
  // 1-5 s + ~30 KB heap.  The visible benefit between stages was marginal
  // (jumps from chunky-pixelated to slightly-less-chunky), and it tangled us
  // up in skip-when-useless / skip-when-failed bookkeeping that produced
  // infinite worker wakeup loops on small / tall sources.
  //
  // New design: just preview (chunky 64w via JPEGDEC's HW /8 scale, ~1 s) and
  // full (target dims via the standard converter, ~3 s).  Two BMP files per
  // image max.  ImageBlock::render falls back full → preview → placeholder.
  // No skip rules, no per-stage failure tracking, no givenUp set — the
  // pipeline is small enough that there's nothing to "get stuck" on.
  // ===========================================================================
  // v2.0.53: BMPs land on LittleFS (flash) instead of SD.  Pending source
  // files (PNG / leftover JPEGs from older versions) still on SD because
  // pngle / SdFat work with FsFile.
  const std::string imagesDir = flashImageCacheDir(cachePath);
  const std::string pendingDir = cachePath + "/images/pending";
  if (!SdMan.exists(pendingDir.c_str())) return 0;

  // Reader-side viewport is hard-wired to 452×699 in current themes; the
  // converter below honours scaledFit aspect math, so target output dims are
  // bounded by this box.
  constexpr int kMaxBoxWidth = 452;
  constexpr int kMaxBoxHeight = 699;

  // Snapshot the directory listing first so we don't hold the dir handle open
  // across a multi-second BMP decode (which would block any sibling SD ops).
  std::vector<std::string> pendingFiles;
  pendingFiles.reserve(8);
  {
    FsFile dir;
    if (!SdMan.openFileForRead("FB2", pendingDir, dir)) return 0;
    if (!dir.isDirectory()) {
      dir.close();
      return 0;
    }
    FsFile entry;
    snapix::spi::SharedBusLock lk;
    while (entry.openNext(&dir, O_RDONLY)) {
      char nameBuf[96];
      const size_t n = entry.getName(nameBuf, sizeof(nameBuf));
      entry.close();
      if (n == 0) continue;
      if (nameBuf[0] == '.' && (nameBuf[1] == '\0' || (nameBuf[1] == '.' && nameBuf[2] == '\0'))) continue;
      const size_t len = strnlen(nameBuf, sizeof(nameBuf));
      if (len < 4) continue;
      const char* ext = nameBuf + len - 4;
      if (strcmp(ext, ".jpg") != 0 && strcmp(ext, ".png") != 0) continue;
      pendingFiles.emplace_back(nameBuf);
    }
    dir.close();
  }

  if (pendingFiles.empty()) return 0;

  // Pick the image whose progression is furthest behind: the one missing
  // BOTH preview and full goes first (preview is cheap), then images that
  // have preview but not full advance to full.  This keeps multiple images
  // on a page progressing in lockstep instead of one racing to full while
  // the other sits on placeholder.
  enum Stage { kNone = 0, kPreview = 1, kFull = 2 };

  struct Item {
    std::string name;
    std::string binaryId;
    std::string pendingPath;
    std::string bmpPath;
    std::string previewPath;
    std::string failPath;
    bool isPng = false;
    int currentStage = kNone;
  };

  Item* picked = nullptr;
  std::vector<Item> items;
  items.reserve(pendingFiles.size());
  for (const auto& name : pendingFiles) {
    Item it;
    it.name = name;
    const size_t len = name.size();
    it.isPng = (len >= 4 && name.compare(len - 4, 4, ".png") == 0);
    it.binaryId = name.substr(0, len - 4);
    it.pendingPath = pendingDir + "/" + name;
    it.bmpPath = imagesDir + "/" + it.binaryId + ".bmp";
    it.previewPath = imagesDir + "/" + it.binaryId + ".preview.bmp";
    it.failPath = imagesDir + "/" + it.binaryId + ".failed";

    // Already at full → clean up the now-stale preview (LittleFS) +
    // pending (SD) and skip.
    if (LittleFS.exists(it.bmpPath.c_str())) {
      if (LittleFS.exists(it.previewPath.c_str())) LittleFS.remove(it.previewPath.c_str());
      SdMan.remove(it.pendingPath.c_str());
      continue;
    }

    if (LittleFS.exists(it.previewPath.c_str())) {
      it.currentStage = kPreview;
    }
    items.push_back(std::move(it));
  }
  if (items.empty()) return 0;

  for (auto& it : items) {
    if (!picked || it.currentStage < picked->currentStage) {
      picked = &it;
    }
  }
  if (!picked) return 0;

  if (shouldAbort && shouldAbort()) {
    LOG_INF(TAG, "decodePendingImages: aborted before phase");
    return 0;
  }

  // PNGs skip preview (pngle has no hardware-scale equivalent — it'd cost the
  // same as a full decode) and jump straight to full.
  const int nextStage = picked->isPng ? kFull : (picked->currentStage + 1);

  if (nextStage == kPreview) {
    FsFile srcFile;
    if (!SdMan.openFileForRead("FB2", picked->pendingPath, srcFile)) return 0;
    const std::string tmpPath = picked->previewPath + ".tmp";
    if (LittleFS.exists(tmpPath.c_str())) LittleFS.remove(tmpPath.c_str());
    File dstFile = LittleFS.open(tmpPath.c_str(), "w");
    if (!dstFile) {
      srcFile.close();
      return 0;
    }
    const bool ok = JpegToBmpConverter::jpegFileToBmpStreamPreview(srcFile, dstFile, shouldAbort);
    srcFile.close();
    dstFile.close();
    if (!ok) {
      LittleFS.remove(tmpPath.c_str());
      return 0;
    }
    if (LittleFS.exists(picked->previewPath.c_str())) LittleFS.remove(picked->previewPath.c_str());
    if (!LittleFS.rename(tmpPath.c_str(), picked->previewPath.c_str())) {
      LittleFS.remove(tmpPath.c_str());
      return 0;
    }
    LOG_INF(TAG, "decodePendingImages[preview]: %s -> %s", picked->binaryId.c_str(), picked->previewPath.c_str());
    return 1;
  }

  // kFull: standard atomic .bmp.tmp + rename via decodeOnePending (PNG-aware).
  // decodeOnePending writes BMP to LittleFS now (see its v2.0.53 update).
  if (decodeOnePending(picked->pendingPath, picked->isPng, picked->bmpPath, kMaxBoxWidth, kMaxBoxHeight,
                       shouldAbort)) {
    SdMan.remove(picked->pendingPath.c_str());
    if (LittleFS.exists(picked->previewPath.c_str())) LittleFS.remove(picked->previewPath.c_str());
    LOG_INF(TAG, "decodePendingImages[full]: %s -> %s", picked->binaryId.c_str(), picked->bmpPath.c_str());
    return 1;
  }
  // Genuine failure (or abort).  Only mark .failed for non-aborts so an
  // aborted decode gets retried on the next BG sweep instead of being
  // permanently poisoned.
  if (!shouldAbort || !shouldAbort()) {
    LOG_ERR(TAG, "decodePendingImages: %s decode failed (id=%s)", picked->isPng ? "PNG" : "JPEG",
            picked->binaryId.c_str());
    LittleFS.remove(picked->bmpPath.c_str());
    if (LittleFS.exists(picked->previewPath.c_str())) LittleFS.remove(picked->previewPath.c_str());
    SdMan.remove(picked->pendingPath.c_str());
    File m = LittleFS.open(picked->failPath.c_str(), "w");
    if (m) m.close();
  }
  return 0;
}

std::string Fb2::getThumbBmpPath() const { return cachePath + "/thumb.bmp"; }

bool Fb2::generateThumbBmp() const {
  const auto thumbPath = getThumbBmpPath();
  const auto failedMarkerPath = cachePath + "/.thumb.failed";

  // v2.0.72: thumb.bmp + failure marker on LittleFS.  Same migration
  // rationale as generateCoverBmp above.
  if (LittleFS.exists(thumbPath.c_str())) {
    return true;
  }

  // Previously failed, don't retry
  if (LittleFS.exists(failedMarkerPath.c_str())) {
    return false;
  }

  if (!LittleFS.exists(getCoverBmpPath().c_str()) && !generateCoverBmp(true)) {
    File marker = LittleFS.open(failedMarkerPath.c_str(), "w");
    if (marker) marker.close();
    return false;
  }

  setupCacheDir();

  const bool success = CoverHelpers::generateThumbFromCover(getCoverBmpPath(), thumbPath, "FB2");
  if (!success) {
    File marker = LittleFS.open(failedMarkerPath.c_str(), "w");
    if (marker) marker.close();
  }
  return success;
}

bool Fb2::loadMetaCache() {
  // v2.0.61: meta cache lives on LittleFS alongside page cache (TOC info
  // is part of "cache", not user data).  No SharedBusLock needed —
  // LittleFS is on a separate SPI bus from SD/display.
  File file = LittleFS.open(metaCachePath().c_str(), "r");
  if (!file) {
    return false;
  }

  uint8_t version;
  if (!serialization::readPodChecked(file, version) || version != kMetaCacheVersion) {
    LOG_ERR(TAG, "Meta cache version mismatch");
    file.close();
    return false;
  }

  if (!serialization::readString(file, title) || !serialization::readString(file, author) ||
      !serialization::readString(file, coverPath)) {
    LOG_ERR(TAG, "Failed to read meta cache strings");
    file.close();
    return false;
  }

  uint32_t cachedFileSize;
  if (!serialization::readPodChecked(file, cachedFileSize)) {
    file.close();
    return false;
  }
  fileSize = cachedFileSize;

  uint16_t sectionCount;
  if (!serialization::readPodChecked(file, sectionCount)) {
    file.close();
    return false;
  }
  sectionCounter_ = sectionCount;

  uint16_t tocItemCount;
  if (!serialization::readPodChecked(file, tocItemCount)) {
    file.close();
    return false;
  }

  // Build compact LUT: record file offset for each TOC entry, skip the actual data
  tocItemCount_ = tocItemCount;
  // Release old capacity before reserving new (swap idiom clears capacity from previous load)
  std::vector<uint32_t>().swap(tocLut_);
  tocLut_.reserve(tocItemCount);
  for (uint16_t i = 0; i < tocItemCount; i++) {
    tocLut_.push_back(static_cast<uint32_t>(file.position()));
    int16_t dummyIdx;
    uint32_t dummyOffset;
    uint8_t dummyDepth;
    if (!serialization::skipString(file) || !serialization::readPodChecked(file, dummyIdx) ||
        !serialization::readPodChecked(file, dummyOffset) || !serialization::readPodChecked(file, dummyDepth)) {
      tocLut_.clear();
      tocItemCount_ = 0;
      file.close();
      return false;
    }
  }

  // Binary (image) index — added in meta v5.  Read into the in-memory vector.
  // v2.0.179 — vector replaces unordered_map.  emplace_back is O(1); the
  // first lookup in binaryIndexFind() sorts the whole vector once.
  binaryIndex_.clear();
  binaryIndexSorted_ = true;  // empty is sorted
  uint16_t binaryCount = 0;
  if (serialization::readPodChecked(file, binaryCount)) {
    binaryIndex_.reserve(binaryCount);
    for (uint16_t i = 0; i < binaryCount; i++) {
      std::string id;
      BinaryEntry entry;
      if (!serialization::readString(file, id) || !serialization::readPodChecked(file, entry.fileOffset) ||
          !serialization::readPodChecked(file, entry.byteLength) ||
          !serialization::readPodChecked(file, entry.mimeType)) {
        // Truncated index — leave whatever we managed to read; not fatal.
        LOG_DBG(TAG, "Binary index truncated at %u/%u entries", i, binaryCount);
        break;
      }
      if (!id.empty()) {
        binaryIndex_.emplace_back(std::move(id), entry);
        binaryIndexSorted_ = false;
      }
    }
  }

  file.close();
  return true;
}

bool Fb2::saveMetaCache() const {
  setupCacheDir();

  File file = LittleFS.open(metaCachePath().c_str(), "w");
  if (!file) {
    LOG_ERR(TAG, "Failed to create meta cache");
    return false;
  }

  serialization::writePod(file, kMetaCacheVersion);
  serialization::writeString(file, title);
  serialization::writeString(file, author);
  serialization::writeString(file, coverPath);

  const uint32_t size32 = static_cast<uint32_t>(fileSize);
  serialization::writePod(file, size32);

  const uint16_t sectionCount = static_cast<uint16_t>(sectionCounter_);
  serialization::writePod(file, sectionCount);

  const uint16_t tocItemCount = static_cast<uint16_t>(tocItems_.size());
  serialization::writePod(file, tocItemCount);

  for (const auto& item : tocItems_) {
    serialization::writeString(file, item.title);
    const int16_t idx = static_cast<int16_t>(item.sectionIndex);
    serialization::writePod(file, idx);
    serialization::writePod(file, item.sourceOffset);
    serialization::writePod(file, item.depth);
  }

  // Binary (image) index — meta v5.  Written immediately after the TOC so
  // the on-disk layout matches loadMetaCache's read order.
  // v2.0.179 — iterate over the vector directly (order doesn't matter, the
  // load-side rebuilds the vector and lazily sorts on first lookup).
  const uint16_t binaryCount = static_cast<uint16_t>(std::min<size_t>(binaryIndex_.size(), 0xFFFFu));
  serialization::writePod(file, binaryCount);
  uint16_t written = 0;
  for (const auto& kv : binaryIndex_) {
    if (written >= binaryCount) break;
    serialization::writeString(file, kv.first);
    serialization::writePod(file, kv.second.fileOffset);
    serialization::writePod(file, kv.second.byteLength);
    serialization::writePod(file, kv.second.mimeType);
    ++written;
  }

  // v2.0.61: meta cache moved to LittleFS — flush() is the Arduino File
  // equivalent of SdFat's sync().  No SD-cache-eviction concerns either.
  file.flush();
  file.close();
  LOG_INF(TAG, "Saved meta cache (%u TOC items, %u binaries)", tocItemCount, binaryCount);
  return true;
}

Fb2::TocItem Fb2::getTocItem(uint16_t index) const {
  TocItem item;
  if (index >= tocItemCount_) return item;

  File file = LittleFS.open(metaCachePath().c_str(), "r");
  if (!file) return item;
  file.seek(tocLut_[index]);
  if (!serialization::readString(file, item.title)) {
    file.close();
    return TocItem{};
  }
  int16_t idx;
  if (serialization::readPodChecked(file, idx)) {
    item.sectionIndex = idx;
  }
  if (!serialization::readPodChecked(file, item.sourceOffset)) {
    item.sourceOffset = 0;
  }
  if (!serialization::readPodChecked(file, item.depth)) {
    item.depth = 0;
  }
  file.close();
  return item;
}

bool Fb2::getTocSourceOffsets(std::vector<uint32_t>& offsets) const {
  offsets.assign(tocItemCount_, 0);
  if (tocItemCount_ == 0 || tocLut_.size() < tocItemCount_) return false;

  File file = LittleFS.open(metaCachePath().c_str(), "r");
  if (!file) return false;

  bool ok = true;
  for (uint16_t i = 0; i < tocItemCount_; ++i) {
    int16_t sectionIndex = -1;
    uint32_t sourceOffset = 0;
    if (!file.seek(tocLut_[i]) || !serialization::skipString(file) ||
        !serialization::readPodChecked(file, sectionIndex) ||
        !serialization::readPodChecked(file, sourceOffset)) {
      ok = false;
      break;
    }
    offsets[i] = sourceOffset;
  }
  file.close();
  if (!ok) offsets.clear();
  return ok;
}

size_t Fb2::readContent(uint8_t* buffer, size_t offset, size_t length) const {
  if (!loaded) {
    return 0;
  }

  FsFile file;
  if (!SdMan.openFileForRead("FB2", filepath, file)) {
    return 0;
  }

  if (offset > 0) {
    snapix::spi::SharedBusLock lk;
    file.seek(offset);
  }

  int readResult = 0;
  {
    snapix::spi::SharedBusLock lk;
    readResult = file.read(buffer, length);
    file.close();
  }

  return readResult > 0 ? static_cast<size_t>(readResult) : 0;
}
