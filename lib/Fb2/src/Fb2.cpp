/**
 * Fb2.cpp
 *
 * FictionBook 2.0 XML e-book handler implementation for Snapix Reader
 */

#include "Fb2.h"

#include <CoverHelpers.h>
#include <FsHelpers.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>

#define TAG "FB2"
#include <SDCardManager.h>
#include <Serialization.h>
#include <SharedSpiLock.h>

#include <algorithm>
#include <cstring>

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
// Streaming keeps RAM usage bounded to ~(in 256B + out 192B) ≈ 450 B.  This
// matters because the largest <binary> in a Russian FB2 commonly hits 200 KB
// of base64 → 150 KB of JPEG, which won't fit in a single allocation on
// ESP32-C3 under load.
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

  uint8_t inBuf[256];
  uint8_t outBuf[192];
  size_t outIdx = 0;
  uint32_t accum = 0;
  int accumBits = 0;
  bool foundOpenTagEnd = false;
  uint32_t remaining = length;
  bool ok = true;

  while (remaining > 0) {
    int toRead = static_cast<int>(std::min<uint32_t>(sizeof(inBuf), remaining));
    int got = 0;
    {
      snapix::spi::SharedBusLock lk;
      got = src.read(inBuf, toRead);
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
        if (outIdx >= sizeof(outBuf)) {
          snapix::spi::SharedBusLock lk;
          dst.write(outBuf, outIdx);
          outIdx = 0;
        }
      }
    }
  }
  if (outIdx > 0) {
    snapix::spi::SharedBusLock lk;
    dst.write(outBuf, outIdx);
  }
  {
    snapix::spi::SharedBusLock lk;
    dst.sync();
    dst.close();
    src.close();
  }
  return ok;
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

bool Fb2::load() {
  LOG_INF(TAG, "Loading FB2: %s", filepath.c_str());

  if (!SdMan.exists(filepath.c_str())) {
    LOG_ERR(TAG, "File does not exist");
    return false;
  }

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
        self->binaryIndex_[self->currentBinaryId_] = entry;
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

    if (!t.empty()) {
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

  // Collect section title text for TOC
  if (self->inSectionTitle_) {
    self->currentSectionTitle_.append(s, len);
  }

  // Extract metadata based on current context
  if (self->inBookTitle) {
    self->title.append(s, len);
  } else if (self->inFirstName) {
    self->currentAuthorFirst.append(s, len);
  } else if (self->inLastName) {
    self->currentAuthorLast.append(s, len);
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
  XML_SetElementHandler(xmlParser_, startElement, endElement);
  XML_SetCharacterDataHandler(xmlParser_, characterData);

  constexpr size_t kChunkSize = 4096;
  uint8_t buffer[kChunkSize];
  bool success = true;

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
  if (!SdMan.exists(cachePath.c_str())) {
    LOG_INF(TAG, "Cache does not exist, no action needed");
    return true;
  }

  if (!SdMan.removeDir(cachePath.c_str())) {
    LOG_ERR(TAG, "Failed to clear cache");
    return false;
  }

  LOG_INF(TAG, "Cache cleared successfully");
  return true;
}

void Fb2::setupCacheDir() const {
  if (!SdMan.exists(cachePath.c_str())) {
    // Create directories recursively
    for (size_t i = 1; i < cachePath.length(); i++) {
      if (cachePath[i] == '/') {
        SdMan.mkdir(cachePath.substr(0, i).c_str());
      }
    }
    if (!SdMan.mkdir(cachePath.c_str())) {
      LOG_ERR(TAG, "Failed to create cache dir: %s", cachePath.c_str());
    }
  }

  // Always verify sections/ exists — partial cache clear may have removed it
  const auto sectionsDir = cachePath + "/sections";
  if (!SdMan.exists(sectionsDir.c_str())) {
    if (!SdMan.mkdir(sectionsDir.c_str())) {
      LOG_ERR(TAG, "Failed to create sections dir: %s", sectionsDir.c_str());
    }
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
  const auto failedMarkerPath = cachePath + "/.cover.failed";

  // Already generated
  if (SdMan.exists(coverPath.c_str())) {
    return true;
  }

  // Previously failed, don't retry
  if (SdMan.exists(failedMarkerPath.c_str())) {
    return false;
  }

  // Find a cover image
  std::string coverImagePath = findCoverImage();
  if (coverImagePath.empty()) {
    LOG_INF(TAG, "No cover image found");
    // Create failure marker
    FsFile marker;
    if (SdMan.openFileForWrite("FB2", failedMarkerPath, marker)) {
      marker.close();
    }
    return false;
  }

  // Setup cache directory
  setupCacheDir();

  // Convert to BMP using shared helper
  const bool success = CoverHelpers::convertImageToBmp(coverImagePath, coverPath, "FB2", use1BitDithering);
  if (!success) {
    // Create failure marker
    FsFile marker;
    if (SdMan.openFileForWrite("FB2", failedMarkerPath, marker)) {
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
bool decodeOnePending(const std::string& pendingPath, bool isPng, const std::string& bmpPath, int maxBoxWidth,
                      int maxBoxHeight, const std::function<bool()>& shouldAbort) {
  FsFile srcFile;
  if (!SdMan.openFileForRead("FB2", pendingPath, srcFile)) return false;
  FsFile bmpFile;
  if (!SdMan.openFileForWrite("FB2", bmpPath, bmpFile)) {
    srcFile.close();
    return false;
  }
  const bool ok =
      isPng ? PngToBmpConverter::pngFileToBmpStreamQuick(srcFile, bmpFile, maxBoxWidth, maxBoxHeight, shouldAbort)
            : JpegToBmpConverter::jpegFileToBmpStreamQuick(srcFile, bmpFile, maxBoxWidth, maxBoxHeight, shouldAbort);
  srcFile.close();
  bmpFile.close();
  return ok;
}
}  // namespace

bool Fb2::cacheImage(const std::string& binaryId, std::string& outBmpPath, uint16_t& outWidth, uint16_t& outHeight,
                     int maxBoxWidth, int maxBoxHeight, bool fastMode) const {
  if (binaryId.empty()) return false;
  auto it = binaryIndex_.find(binaryId);
  if (it == binaryIndex_.end()) {
    LOG_DBG(TAG, "cacheImage: binary id not in index: %s", binaryId.c_str());
    return false;
  }
  const BinaryEntry& entry = it->second;
  // mimeType: 0 = JPEG, 1 = PNG, 2+ = unsupported.  Anything else is rejected.
  if (entry.byteLength == 0 || entry.mimeType >= 2) {
    return false;
  }
  const bool isPng = (entry.mimeType == 1);
  const char* extDot = isPng ? ".png" : ".jpg";

  const std::string imagesDir = cachePath + "/images";
  const std::string pendingDir = imagesDir + "/pending";
  const std::string bmpPath = imagesDir + "/" + binaryId + ".bmp";
  const std::string failPath = imagesDir + "/" + binaryId + ".failed";
  const std::string pendingPath = pendingDir + "/" + binaryId + extDot;

  // Already fully decoded (idempotent) — fast path for both modes.
  if (SdMan.exists(bmpPath.c_str())) {
    if (readBmpDimensions(bmpPath, outWidth, outHeight)) {
      outBmpPath = bmpPath;
      return true;
    }
    // Header read failed / absurd dimensions — fall through and re-decode.
  }

  // Previous attempt failed sentinel — don't retry the same decode every
  // page render in case the source is malformed / OOM-bait.
  if (SdMan.exists(failPath.c_str())) {
    return false;
  }

  // Ensure target directories exist (cheap if they already do).
  if (!SdMan.exists(imagesDir.c_str())) {
    if (!SdMan.mkdir(imagesDir.c_str())) {
      LOG_ERR(TAG, "cacheImage: failed to create images dir: %s", imagesDir.c_str());
      return false;
    }
  }
  if (!SdMan.exists(pendingDir.c_str())) {
    if (!SdMan.mkdir(pendingDir.c_str())) {
      LOG_ERR(TAG, "cacheImage: failed to create pending dir: %s", pendingDir.c_str());
      return false;
    }
  }

  // Step 1: stream-decode base64 to the pending file (idempotent — if the
  // pending file already exists from a prior fast-mode call, reuse it).
  if (!SdMan.exists(pendingPath.c_str())) {
    if (!streamDecodeBase64ToJpegFile(filepath, entry.fileOffset, entry.byteLength, pendingPath)) {
      LOG_ERR(TAG, "cacheImage: base64 decode failed for id=%s", binaryId.c_str());
      SdMan.remove(pendingPath.c_str());
      FsFile m;
      if (SdMan.openFileForWrite("FB2", failPath, m)) m.close();
      return false;
    }
  }

  if (fastMode) {
    // Step 2a (fast): peek dimensions from the source header without doing
    // pixel decode — the BG worker will pick up the pending file later via
    // decodePendingImages().  ImageBlock::render gracefully shows a "[Image]"
    // placeholder while the BMP is missing.
    FsFile srcFile;
    if (!SdMan.openFileForRead("FB2", pendingPath, srcFile)) {
      LOG_ERR(TAG, "cacheImage: failed to reopen pending %s", pendingPath.c_str());
      return false;
    }
    int srcW = 0, srcH = 0;
    const bool peekOk = isPng ? PngToBmpConverter::peekDimensions(srcFile, srcW, srcH)
                              : JpegToBmpConverter::peekDimensions(srcFile, srcW, srcH);
    srcFile.close();
    if (!peekOk) {
      LOG_ERR(TAG, "cacheImage: %s header peek failed for id=%s", isPng ? "PNG" : "JPEG", binaryId.c_str());
      SdMan.remove(pendingPath.c_str());
      FsFile m;
      if (SdMan.openFileForWrite("FB2", failPath, m)) m.close();
      return false;
    }
    int outW = 0, outH = 0;
    scaledFit(srcW, srcH, maxBoxWidth, maxBoxHeight, outW, outH);
    if (outW <= 0 || outH <= 0 || outW > 0xFFFF || outH > 0xFFFF) {
      SdMan.remove(pendingPath.c_str());
      return false;
    }
    outWidth = static_cast<uint16_t>(outW);
    outHeight = static_cast<uint16_t>(outH);
    outBmpPath = bmpPath;  // Will exist later, after decodePendingImages() runs.
    LOG_INF(TAG, "cacheImage[fast]: registered %s src=%dx%d -> %dx%d (BMP pending)", binaryId.c_str(), srcW, srcH, outW,
            outH);
    return true;
  }

  // Step 2b (sync): full pixel decode.
  if (!decodeOnePending(pendingPath, isPng, bmpPath, maxBoxWidth, maxBoxHeight, nullptr)) {
    LOG_ERR(TAG, "cacheImage: %s decode failed for id=%s", isPng ? "PNG" : "JPEG", binaryId.c_str());
    SdMan.remove(bmpPath.c_str());
    SdMan.remove(pendingPath.c_str());
    FsFile m;
    if (SdMan.openFileForWrite("FB2", failPath, m)) m.close();
    return false;
  }
  SdMan.remove(pendingPath.c_str());

  // Step 3: read back BMP dimensions for the caller (ImageBlock needs w/h).
  if (readBmpDimensions(bmpPath, outWidth, outHeight)) {
    outBmpPath = bmpPath;
    LOG_INF(TAG, "cacheImage: decoded %s -> %s (%ux%u)", binaryId.c_str(), bmpPath.c_str(),
            static_cast<unsigned>(outWidth), static_cast<unsigned>(outHeight));
    return true;
  }

  // BMP exists but unreadable header — give up and mark failed.
  LOG_ERR(TAG, "cacheImage: BMP %s has unreadable / absurd dimensions", bmpPath.c_str());
  SdMan.remove(bmpPath.c_str());
  FsFile m;
  if (SdMan.openFileForWrite("FB2", failPath, m)) m.close();
  return false;
}

bool Fb2::hasPendingImages() const {
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

int Fb2::decodePendingImages(const std::function<bool()>& shouldAbort) const {
  const std::string imagesDir = cachePath + "/images";
  const std::string pendingDir = imagesDir + "/pending";
  if (!SdMan.exists(pendingDir.c_str())) return 0;

  // Mirrors the maxBox used during fast-mode registration.  Reader-side viewport
  // is always 452×699 in current themes; using the same constraint here ensures
  // the eventual BMP matches the placeholder dimensions stored on the page.
  // (If the viewport ever becomes user-configurable, this should pick the value
  // up from the same source as Fb2Parser; for now they're both hard-wired.)
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

  int decoded = 0;
  for (const auto& name : pendingFiles) {
    if (shouldAbort && shouldAbort()) {
      LOG_INF(TAG, "decodePendingImages: aborted after %d image(s)", decoded);
      break;
    }

    const size_t len = name.size();
    const bool isPng = (len >= 4 && name.compare(len - 4, 4, ".png") == 0);
    const std::string binaryId = name.substr(0, len - 4);
    const std::string pendingPath = pendingDir + "/" + name;
    const std::string bmpPath = imagesDir + "/" + binaryId + ".bmp";
    const std::string failPath = imagesDir + "/" + binaryId + ".failed";

    // Skip if BMP somehow already exists (idempotent).
    if (SdMan.exists(bmpPath.c_str())) {
      SdMan.remove(pendingPath.c_str());
      continue;
    }

    if (decodeOnePending(pendingPath, isPng, bmpPath, kMaxBoxWidth, kMaxBoxHeight, shouldAbort)) {
      SdMan.remove(pendingPath.c_str());
      ++decoded;
      uint16_t w = 0, h = 0;
      if (readBmpDimensions(bmpPath, w, h)) {
        LOG_INF(TAG, "decodePendingImages: %s -> %s (%ux%u)", binaryId.c_str(), bmpPath.c_str(),
                static_cast<unsigned>(w), static_cast<unsigned>(h));
      } else {
        LOG_INF(TAG, "decodePendingImages: %s -> %s (dim read failed)", binaryId.c_str(), bmpPath.c_str());
      }
    } else if (shouldAbort && shouldAbort()) {
      // Decoder bailed because the worker is being preempted (e.g. user pressed
      // a button).  Keep the pending file so we resume on the next BG sweep —
      // do NOT write a .failed marker, that would permanently skip this image.
      LOG_INF(TAG, "decodePendingImages: aborted during %s (id=%s) — pending kept", isPng ? "PNG" : "JPEG",
              binaryId.c_str());
      SdMan.remove(bmpPath.c_str());  // partial / truncated BMP, throw away
      break;
    } else {
      LOG_ERR(TAG, "decodePendingImages: %s decode failed (id=%s)", isPng ? "PNG" : "JPEG", binaryId.c_str());
      SdMan.remove(bmpPath.c_str());
      SdMan.remove(pendingPath.c_str());
      FsFile m;
      if (SdMan.openFileForWrite("FB2", failPath, m)) m.close();
    }
  }
  return decoded;
}

std::string Fb2::getThumbBmpPath() const { return cachePath + "/thumb.bmp"; }

bool Fb2::generateThumbBmp() const {
  const auto thumbPath = getThumbBmpPath();
  const auto failedMarkerPath = cachePath + "/.thumb.failed";

  if (SdMan.exists(thumbPath.c_str())) {
    return true;
  }

  // Previously failed, don't retry
  if (SdMan.exists(failedMarkerPath.c_str())) {
    return false;
  }

  if (!SdMan.exists(getCoverBmpPath().c_str()) && !generateCoverBmp(true)) {
    // Create failure marker
    FsFile marker;
    if (SdMan.openFileForWrite("FB2", failedMarkerPath, marker)) {
      marker.close();
    }
    return false;
  }

  setupCacheDir();

  const bool success = CoverHelpers::generateThumbFromCover(getCoverBmpPath(), thumbPath, "FB2");
  if (!success) {
    // Create failure marker
    FsFile marker;
    if (SdMan.openFileForWrite("FB2", failedMarkerPath, marker)) {
      marker.close();
    }
  }
  return success;
}

bool Fb2::loadMetaCache() {
  FsFile file;
  if (!SdMan.openFileForRead("FB2", metaCachePath(), file)) {
    return false;
  }

  snapix::spi::SharedBusLock lk;

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

  // Binary (image) index — added in meta v5.  Read into the in-memory map.
  binaryIndex_.clear();
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
        binaryIndex_.emplace(std::move(id), entry);
      }
    }
  }

  file.close();
  return true;
}

bool Fb2::saveMetaCache() const {
  setupCacheDir();

  FsFile file;
  if (!SdMan.openFileForWrite("FB2", metaCachePath(), file)) {
    LOG_ERR(TAG, "Failed to create meta cache");
    return false;
  }

  snapix::spi::SharedBusLock lk;

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

  // Explicit sync flushes both file data and the parent directory entry to the
  // SD card before close().  Without this, SdFat keeps the directory cluster
  // dirty in its single-sector cache; if the next file operation evicts that
  // sector, the freshly written meta.bin appears to vanish on the next open.
  file.sync();
  file.close();
  LOG_INF(TAG, "Saved meta cache (%u TOC items, %u binaries)", tocItemCount, binaryCount);
  return true;
}

Fb2::TocItem Fb2::getTocItem(uint16_t index) const {
  TocItem item;
  if (index >= tocItemCount_) return item;

  FsFile file;
  if (!SdMan.openFileForRead("FB2", metaCachePath(), file)) return item;

  snapix::spi::SharedBusLock lk;
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
