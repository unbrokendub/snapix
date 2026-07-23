#include "Epub.h"

#include <FS.h>          // Arduino base File (v2.0.73 LittleFS migration)
#include <LittleFS.h>
#include <CacheFs.h>
#include <CoverHelpers.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <ImageConverter.h>
#include <Logging.h>
#include <SDCardManager.h>
#include <ZipFile.h>

#define TAG "EPUB"

#include "../../src/config.h"
#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/TocNavParser.h"
#include "Epub/parsers/TocNcxParser.h"

#include <algorithm>

extern GfxRenderer renderer;

namespace {
uint8_t* sharedZipDictBuffer() {
  // Reuse the already-static display framebuffer as uzlib's history buffer
  // during EPUB metadata extraction, instead of requesting a fresh 32KB heap
  // block after a fragmented FB2 session.
  return renderer.getFrameBuffer();
}

std::string directoryForItemPath(const std::string& itemPath) {
  const size_t slashPos = itemPath.find_last_of('/');
  if (slashPos == std::string::npos) {
    return {};
  }
  return itemPath.substr(0, slashPos + 1);
}

std::string canonicalizeHref(const std::string& href) {
  if (href.empty()) return {};

  std::string normalized = FsHelpers::normalisePath(href);
  while (!normalized.empty() && normalized.front() == '/') {
    normalized.erase(normalized.begin());
  }
  return normalized;
}

bool hrefsEquivalent(const std::string& lhs, const std::string& rhs) {
  if (lhs.empty() || rhs.empty()) return false;
  if (lhs == rhs) return true;

  if (lhs.size() > rhs.size() && lhs.compare(lhs.size() - rhs.size(), rhs.size(), rhs) == 0) {
    return lhs[lhs.size() - rhs.size() - 1] == '/';
  }

  if (rhs.size() > lhs.size() && rhs.compare(rhs.size() - lhs.size(), lhs.size(), lhs) == 0) {
    return rhs[rhs.size() - lhs.size() - 1] == '/';
  }

  return false;
}
}  // namespace

bool Epub::findContentOpfFile(std::string* contentOpfFile) const {
  const auto containerPath = "META-INF/container.xml";
  size_t containerSize;

  // Get file size without loading it all into heap
  if (!getItemSize(containerPath, &containerSize)) {
    LOG_ERR(TAG, "Could not find or size META-INF/container.xml");
    return false;
  }

  ContainerParser containerParser(containerSize);

  if (!containerParser.setup()) {
    return false;
  }

  // Stream read (reusing your existing stream logic)
  if (!readItemContentsToStream(containerPath, containerParser, 512, sharedZipDictBuffer())) {
    LOG_ERR(TAG, "Could not read META-INF/container.xml");
    return false;
  }

  // Extract the result
  if (containerParser.fullPath.empty()) {
    LOG_ERR(TAG, "Could not find valid rootfile in container.xml");
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);
  return true;
}

bool Epub::parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata) {
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    LOG_ERR(TAG, "Could not find content.opf in zip");
    return false;
  }

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  LOG_INF(TAG, "Parsing content.opf: %s", contentOpfFilePath.c_str());

  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    LOG_ERR(TAG, "Could not get size of content.opf");
    return false;
  }

  ContentOpfParser opfParser(getCachePath(), getBasePath(), contentOpfSize, bookMetadataCache.get());
  if (!opfParser.setup()) {
    LOG_ERR(TAG, "Could not setup content.opf parser");
    return false;
  }

  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024, sharedZipDictBuffer())) {
    LOG_ERR(TAG, "Could not read content.opf");
    return false;
  }

  // Grab data from opfParser into epub
  bookMetadata.title = opfParser.title;
  bookMetadata.author = opfParser.author;
  bookMetadata.language = opfParser.language;
  bookMetadata.coverItemHref = opfParser.coverItemHref;
  bookMetadata.textReferenceHref = opfParser.textReferenceHref;

  if (!opfParser.tocNcxPath.empty()) {
    tocNcxItem = opfParser.tocNcxPath;
  }

  if (!opfParser.tocNavPath.empty()) {
    tocNavItem = opfParser.tocNavPath;
  }

  // Capture CSS files from manifest
  cssFiles_ = opfParser.getCssFiles();
  LOG_DBG(TAG, "Found %d CSS files in manifest", static_cast<int>(cssFiles_.size()));

  LOG_INF(TAG, "Successfully parsed content.opf");
  return true;
}

bool Epub::parseTocNcxFile() const {
  // the ncx file should have been specified in the content.opf file
  if (tocNcxItem.empty()) {
    LOG_DBG(TAG, "No ncx file specified");
    return false;
  }

  LOG_INF(TAG, "Parsing toc ncx file: %s", tocNcxItem.c_str());

  const auto tmpNcxPath = getCachePath() + "/toc.ncx";
  // v2.0.73: temp files for TOC parsing live on LittleFS now.
  File tempNcxFile = LittleFS.open(tmpNcxPath.c_str(), "w");
  if (!tempNcxFile) {
    return false;
  }
  if (!readItemContentsToStream(tocNcxItem, tempNcxFile, 1024, sharedZipDictBuffer())) {
    tempNcxFile.close();
    return false;
  }
  tempNcxFile.close();
  tempNcxFile = LittleFS.open(tmpNcxPath.c_str(), "r");
  if (!tempNcxFile) {
    return false;
  }
  const auto ncxSize = tempNcxFile.size();

  const std::string ncxContentBasePath = directoryForItemPath(tocNcxItem);
  TocNcxParser ncxParser(ncxContentBasePath, ncxSize, bookMetadataCache.get());

  if (!ncxParser.setup()) {
    LOG_ERR(TAG, "Could not setup toc ncx parser");
    tempNcxFile.close();
    return false;
  }

  const auto ncxBuffer = static_cast<uint8_t*>(malloc(1024));
  if (!ncxBuffer) {
    LOG_ERR(TAG, "Could not allocate memory for toc ncx parser");
    tempNcxFile.close();
    return false;
  }

  while (tempNcxFile.available()) {
    const int readResult = tempNcxFile.read(ncxBuffer, 1024);
    if (readResult <= 0) {
      LOG_ERR(TAG, "Could not read toc ncx data");
      free(ncxBuffer);
      tempNcxFile.close();
      return false;
    }
    const size_t readSize = static_cast<size_t>(readResult);
    const auto processedSize = ncxParser.write(ncxBuffer, readSize);

    if (processedSize != readSize) {
      LOG_ERR(TAG, "Could not process all toc ncx data");
      free(ncxBuffer);
      tempNcxFile.close();
      return false;
    }
  }

  free(ncxBuffer);
  tempNcxFile.close();
  LittleFS.remove(tmpNcxPath.c_str());

  LOG_INF(TAG, "Parsed TOC items");
  return true;
}

bool Epub::parseTocNavFile() const {
  // the nav file should have been specified in the content.opf file (EPUB 3)
  if (tocNavItem.empty()) {
    LOG_DBG(TAG, "No nav file specified");
    return false;
  }

  LOG_INF(TAG, "Parsing toc nav file: %s", tocNavItem.c_str());

  const auto tmpNavPath = getCachePath() + "/toc.nav";
  // v2.0.73: temp files for TOC parsing live on LittleFS now.
  File tempNavFile = LittleFS.open(tmpNavPath.c_str(), "w");
  if (!tempNavFile) {
    return false;
  }
  if (!readItemContentsToStream(tocNavItem, tempNavFile, 1024, sharedZipDictBuffer())) {
    tempNavFile.close();
    return false;
  }
  tempNavFile.close();
  tempNavFile = LittleFS.open(tmpNavPath.c_str(), "r");
  if (!tempNavFile) {
    return false;
  }
  const auto navSize = tempNavFile.size();

  // Note: We can't use `contentBasePath` here as the nav file may be in a different folder to the content.opf
  // and the HTMLX nav file will have hrefs relative to itself
  const std::string navContentBasePath = tocNavItem.substr(0, tocNavItem.find_last_of('/') + 1);
  TocNavParser navParser(navContentBasePath, navSize, bookMetadataCache.get());

  if (!navParser.setup()) {
    LOG_ERR(TAG, "Could not setup toc nav parser");
    tempNavFile.close();
    return false;
  }

  const auto navBuffer = static_cast<uint8_t*>(malloc(1024));
  if (!navBuffer) {
    LOG_ERR(TAG, "Could not allocate memory for toc nav parser");
    tempNavFile.close();
    return false;
  }

  while (tempNavFile.available()) {
    const int readResult = tempNavFile.read(navBuffer, 1024);
    if (readResult <= 0) {
      LOG_ERR(TAG, "Could not read toc nav data");
      free(navBuffer);
      tempNavFile.close();
      return false;
    }
    const size_t readSize = static_cast<size_t>(readResult);
    const auto processedSize = navParser.write(navBuffer, readSize);

    if (processedSize != readSize) {
      LOG_ERR(TAG, "Could not process all toc nav data");
      free(navBuffer);
      tempNavFile.close();
      return false;
    }
  }

  free(navBuffer);
  tempNavFile.close();
  LittleFS.remove(tmpNavPath.c_str());

  LOG_INF(TAG, "Parsed TOC nav items");
  return true;
}

bool Epub::parseCssFiles() {
  if (cssFiles_.empty()) {
    LOG_DBG(TAG, "No CSS files to parse");
    return true;
  }

  cssParser_.reset(new CssParser());

  for (const auto& cssHref : cssFiles_) {
    // Extract CSS file to temp location.  v2.0.73: temp lives on LittleFS.
    const auto tmpCssPath = getCachePath() + "/.tmp_css.css";

    File tempCssFile = LittleFS.open(tmpCssPath.c_str(), "w");
    if (!tempCssFile) {
      LOG_ERR(TAG, "Failed to create temp CSS file");
      continue;
    }

    if (!readItemContentsToStream(cssHref, tempCssFile, 1024, sharedZipDictBuffer())) {
      LOG_ERR(TAG, "Failed to extract CSS: %s", cssHref.c_str());
      tempCssFile.close();
      LittleFS.remove(tmpCssPath.c_str());
      continue;
    }
    tempCssFile.close();

    // Parse the CSS file (CssParser::parseFile reopens the path; v2.0.73
    // updated to LittleFS.open internally).
    if (!cssParser_->parseFile(tmpCssPath.c_str())) {
      LOG_ERR(TAG, "Failed to parse CSS: %s", cssHref.c_str());
    }

    LittleFS.remove(tmpCssPath.c_str());
  }

  LOG_INF(TAG, "Parsed CSS files, %d style rules loaded", static_cast<int>(cssParser_->getStyleCount()));
  return true;
}

// load in the meta data for the epub file
bool Epub::load(const bool buildIfMissing) {
  LOG_INF(TAG, "Loading ePub: %s", filepath.c_str());

  if (!cache_fs::ensureSourceFingerprint(filepath, cachePath)) {
    LOG_ERR(TAG, "Could not verify source fingerprint");
    return false;
  }

  // Initialize spine/TOC cache
  bookMetadataCache.reset(new BookMetadataCache(cachePath));

  // Try to load existing cache first
  if (bookMetadataCache->load()) {
    LOG_INF(TAG, "Loaded ePub: %s", filepath.c_str());
    return true;
  }

  // If we didn't load from cache above and we aren't allowed to build, fail now
  if (!buildIfMissing) {
    return false;
  }

  // Cache doesn't exist or is invalid, build it
  LOG_INF(TAG, "Cache not found, building spine/TOC cache");
  setupCacheDir();

  // Begin building cache - stream entries to disk immediately
  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR(TAG, "Could not begin writing cache");
    return false;
  }

  // OPF Pass
  BookMetadataCache::BookMetadata bookMetadata;
  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR(TAG, "Could not begin writing content.opf pass");
    return false;
  }
  if (!parseContentOpf(bookMetadata)) {
    LOG_ERR(TAG, "Could not parse content.opf");
    return false;
  }
  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR(TAG, "Could not end writing content.opf pass");
    return false;
  }

  // Parse CSS files for styling
  parseCssFiles();
  // Free CSS file paths - no longer needed after rules are parsed into cssParser_
  {
    std::vector<std::string> tmp;
    cssFiles_.swap(tmp);
  }

  // TOC Pass - try EPUB 3 nav first, fall back to NCX
  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR(TAG, "Could not begin writing toc pass");
    return false;
  }

  bool tocParsed = false;

  // Try EPUB 3 nav document first (preferred)
  if (!tocNavItem.empty()) {
    LOG_DBG(TAG, "Attempting to parse EPUB 3 nav document");
    tocParsed = parseTocNavFile();
  }

  // Fall back to NCX if nav parsing failed or wasn't available
  if (!tocParsed && !tocNcxItem.empty()) {
    LOG_DBG(TAG, "Falling back to NCX TOC");
    tocParsed = parseTocNcxFile();
  }

  if (!tocParsed) {
    LOG_ERR(TAG, "Warning: Could not parse any TOC format");
    // Continue anyway - book will work without TOC
  }

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR(TAG, "Could not end writing toc pass");
    return false;
  }

  // Close the cache files
  if (!bookMetadataCache->endWrite()) {
    LOG_ERR(TAG, "Could not end writing cache");
    return false;
  }

  // Build final book.bin
  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    LOG_ERR(TAG, "Could not update mappings and sizes");
    return false;
  }

  if (!bookMetadataCache->cleanupTmpFiles()) {
    LOG_ERR(TAG, "Could not cleanup tmp files - ignoring");
  }

  // Reload the cache from disk so it's in the correct state
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  if (!bookMetadataCache->load()) {
    LOG_ERR(TAG, "Failed to reload cache after writing");
    return false;
  }

  LOG_INF(TAG, "Loaded ePub: %s", filepath.c_str());
  return true;
}

bool Epub::clearCache() const {
  // v2.0.73: cache lives on LittleFS now.
  if (!LittleFS.exists(cachePath.c_str())) {
    LOG_DBG(TAG, "Cache does not exist, no action needed");
    return true;
  }
  if (!cache_fs::rmTree(cachePath)) {
    LOG_ERR(TAG, "Failed to clear cache");
    return false;
  }
  LOG_INF(TAG, "Cache cleared successfully");
  return true;
}

void Epub::setupCacheDir() const {
  // v2.0.73: cache lives on LittleFS now.  ensureFlashDir walks parents,
  // so we don't need the per-subdir mkdir dance — but the explicit subdirs
  // (sections/images) still need to exist as empty leaves before their
  // respective writers create files inside.
  //
  // v2.0.163: `chapters/` subdir no longer created.  v2.0.159's markerize-
  // from-ZIP eliminated the temp `chapters/N.src.html` files; v2.0.161
  // deleted the legacy prepareChapterHtml that wrote them.  For
  // upgraders coming from <=2.0.160, run a one-shot cleanup that removes
  // any orphaned `chapters/` subtree under this book's cache dir —
  // reclaims ~50-100 KB of LittleFS per book that hadn't been opened
  // since v2.0.161 wiped its in-memory references.
  if (!cache_fs::ensureFlashDir(cachePath)) {
    LOG_ERR(TAG, "Failed to create cache dir: %s", cachePath.c_str());
  }
  if (!cache_fs::ensureFlashDir(cachePath + "/sections")) {
    LOG_ERR(TAG, "Failed to create sections dir");
  }
  if (!cache_fs::ensureFlashDir(cachePath + "/images")) {
    LOG_ERR(TAG, "Failed to create images dir");
  }

  // v2.0.163 + v2.0.167 one-shot orphan-cleanup pass.  Removes:
  //   * <cachePath>/chapters/    — temp HTML files, gone since v2.0.159
  //   * <cachePath>/markers/     — markers + idx, moved to UnifiedCache in v2.0.167
  //   * <cachePath>/metrics.bin  — moved to UnifiedCache::Metrics in v2.0.166
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
  const std::string chaptersDir = cachePath + "/chapters";
  if (LittleFS.exists(chaptersDir.c_str()) && rmTree(chaptersDir)) {
    LOG_INF(TAG, "[CONTENT][EPUB] cleanup: removed orphaned chapters/ dir from %s", cachePath.c_str());
  }
  const std::string markersDir = cachePath + "/markers";
  if (LittleFS.exists(markersDir.c_str()) && rmTree(markersDir)) {
    LOG_INF(TAG, "[CONTENT][EPUB] cleanup: removed legacy markers/ dir from %s (migrated to UnifiedCache)",
            cachePath.c_str());
  }
  const std::string metricsPath = cachePath + "/metrics.bin";
  if (LittleFS.exists(metricsPath.c_str()) && LittleFS.remove(metricsPath.c_str())) {
    LOG_INF(TAG, "[CONTENT][EPUB] cleanup: removed legacy metrics.bin from %s (migrated to UnifiedCache)",
            cachePath.c_str());
  }
}

const std::string& Epub::getCachePath() const { return cachePath; }

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.title;
}

const std::string& Epub::getAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.author;
}

const std::string& Epub::getLanguage() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.language;
}

std::string Epub::getCoverBmpPath() const { return cachePath + "/cover.bmp"; }

std::string Epub::getCoverPreviewBmpPath() const { return cachePath + "/cover_preview.bmp"; }

bool Epub::generateCoverPreviewBmp() const {
  const auto previewPath = getCoverPreviewBmpPath();

  // v2.0.73: cover preview lives on LittleFS now.
  if (LittleFS.exists(previewPath.c_str())) {
    return true;
  }
  if (LittleFS.exists(getCoverBmpPath().c_str())) {
    return false;
  }

  setupCacheDir();

  // Priority 1: External cover file (bookname.jpg, etc.)
  std::string externalCover = findCoverImage();
  if (!externalCover.empty()) {
    LOG_DBG(TAG, "Found external cover for preview: %s", externalCover.c_str());
    ImageConvertConfig config;
    config.quickMode = true;
    config.logTag = "EBP";
    config.outputOnLittleFs = true;
    if (ImageConverterFactory::convertToBmp(externalCover, previewPath, config)) {
      LOG_INF(TAG, "Generated cover preview from external image");
      return true;
    }
    LittleFS.remove(previewPath.c_str());
  }

  // Priority 2: Try common internal cover paths (batch scan - single ZIP pass)
  static const char* const commonCoverPaths[] = {
      "cover.jpg",
      "cover.jpeg",
      "cover.png",
      "images/cover.jpg",
      "images/cover.jpeg",
      "images/cover.png",
      "Images/cover.jpg",
      "Images/cover.jpeg",
      "Images/cover.png",
      "OEBPS/cover.jpg",
      "OEBPS/cover.jpeg",
      "OEBPS/cover.png",
      "OEBPS/images/cover.jpg",
      "OEBPS/images/cover.jpeg",
      "OEBPS/images/cover.png",
      "OEBPS/Images/cover.jpg",
      "OEBPS/Images/cover.jpeg",
      "OEBPS/Images/cover.png",
  };
  constexpr int commonCoverPathsCount = sizeof(commonCoverPaths) / sizeof(commonCoverPaths[0]);

  ZipFile zip(filepath);
  const int foundIndex = zip.findFirstExisting(commonCoverPaths, commonCoverPathsCount);
  if (foundIndex >= 0) {
    const char* path = commonCoverPaths[foundIndex];
    LOG_DBG(TAG, "Found cover for preview via heuristic: %s", path);

    const std::string ext = FsHelpers::isJpegFile(path) ? ".jpg" : ".png";
    const auto coverTempPath = getCachePath() + "/.cover_preview" + ext;

    File coverFile = LittleFS.open(coverTempPath.c_str(), "w");
    if (coverFile) {
      if (readItemContentsToStream(path, coverFile, 1024)) {
        coverFile.close();
        ImageConvertConfig config;
        config.quickMode = true;
        config.logTag = "EBP";
        config.outputOnLittleFs = true;
        if (ImageConverterFactory::convertToBmp(coverTempPath, previewPath, config)) {
          LittleFS.remove(coverTempPath.c_str());
          return true;
        }
        LittleFS.remove(previewPath.c_str());
      } else {
        coverFile.close();
      }
    }
    LittleFS.remove(coverTempPath.c_str());
  }

  LOG_DBG(TAG, "No cover found for preview");
  return false;
}

std::string Epub::getThumbBmpPath() const { return cachePath + "/thumb.bmp"; }

std::string Epub::findCoverImage() const {
  size_t lastSlash = filepath.find_last_of('/');
  std::string dirPath = (lastSlash == std::string::npos) ? "/" : filepath.substr(0, lastSlash);
  if (dirPath.empty()) dirPath = "/";

  std::string baseName = filepath.substr(lastSlash + 1);
  size_t lastDot = baseName.find_last_of('.');
  if (lastDot != std::string::npos) {
    baseName = baseName.substr(0, lastDot);
  }

  return CoverHelpers::findCoverImage(dirPath, baseName);
}

bool Epub::generateThumbBmp() const {
  const auto thumbPath = getThumbBmpPath();
  const auto failedMarkerPath = cachePath + "/.thumb.failed";

  // v2.0.73: thumb + failure marker live on LittleFS now.
  if (LittleFS.exists(thumbPath.c_str())) return true;
  if (LittleFS.exists(failedMarkerPath.c_str())) return false;

  setupCacheDir();

  // Priority 1: External cover file (bookname.jpg, etc.)
  std::string externalCover = findCoverImage();
  if (!externalCover.empty()) {
    LOG_DBG(TAG, "Found external cover: %s", externalCover.c_str());
    // Generate full-size cover BMP first (needed for thumbnail scaling)
    const auto coverPath = getCoverBmpPath();
    if (!LittleFS.exists(coverPath.c_str())) {
      if (CoverHelpers::convertImageToBmp(externalCover, coverPath, "EBP", true)) {
        LOG_INF(TAG, "Generated cover BMP from external image");
      }
    }
    // Now generate thumbnail from cover
    if (CoverHelpers::generateThumbFromCover(coverPath, thumbPath, "EBP")) {
      return true;
    }
  }

  // Priority 2: Try to generate from existing cover.bmp (much faster than re-extracting from EPUB)
  const auto coverPath = getCoverBmpPath();
  if (CoverHelpers::generateThumbFromCover(coverPath, thumbPath, "EBP")) {
    return true;
  }

  auto writeFailureMarker = [&]() {
    File marker = LittleFS.open(failedMarkerPath.c_str(), "w");
    if (marker) marker.close();
  };

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR(TAG, "Cannot generate thumb BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_DBG(TAG, "No known cover image for thumbnail");
    writeFailureMarker();
    return false;
  }

  if (!ImageConverterFactory::isSupported(coverImageHref)) {
    LOG_ERR(TAG, "Unsupported cover image format for thumbnail");
    writeFailureMarker();
    return false;
  }

  // Extract cover image to LittleFS temp file, convert to thumbnail.
  LOG_DBG(TAG, "Generating thumb BMP from cover image");
  const std::string ext = FsHelpers::isJpegFile(coverImageHref) ? ".jpg" : ".png";
  const auto coverTempPath = getCachePath() + "/.cover" + ext;
  const auto thumbTempPath = thumbPath + ".tmp";

  File coverFile = LittleFS.open(coverTempPath.c_str(), "w");
  if (!coverFile) {
    return false;
  }
  if (!readItemContentsToStream(coverImageHref, coverFile, 1024)) {
    coverFile.close();
    LittleFS.remove(coverTempPath.c_str());
    return false;
  }
  coverFile.close();

  ImageConvertConfig config;
  config.maxWidth = THUMB_WIDTH;
  config.maxHeight = THUMB_HEIGHT;
  config.oneBit = FsHelpers::isJpegFile(coverImageHref);
  config.logTag = "EBP";
  config.outputOnLittleFs = true;

  const bool success = ImageConverterFactory::convertToBmp(coverTempPath, thumbTempPath, config);
  LittleFS.remove(coverTempPath.c_str());

  if (success) {
    // Atomic rename: readers see either no file or complete file.
    LittleFS.rename(thumbTempPath.c_str(), thumbPath.c_str());
  } else {
    LOG_ERR(TAG, "Failed to generate thumb BMP from cover image");
    LittleFS.remove(thumbTempPath.c_str());
    writeFailureMarker();
  }
  LOG_INF(TAG, "Generated thumb BMP from cover image, success: %s", success ? "yes" : "no");
  return success;
}

bool Epub::generateCoverBmp(bool use1BitDithering) const {
  const auto coverPath = getCoverBmpPath();
  const auto failedMarkerPath = cachePath + "/.cover.failed";

  // v2.0.73: cover BMP + failure marker live on LittleFS now.
  if (LittleFS.exists(coverPath.c_str())) return true;
  if (LittleFS.exists(failedMarkerPath.c_str())) return false;

  setupCacheDir();

  // Priority 1: External cover file (bookname.jpg, etc.)
  std::string externalCover = findCoverImage();
  if (!externalCover.empty()) {
    LOG_DBG(TAG, "Found external cover: %s", externalCover.c_str());
    if (CoverHelpers::convertImageToBmp(externalCover, coverPath, "EBP", use1BitDithering)) {
      LOG_INF(TAG, "Generated cover BMP from external image");
      return true;
    }
  }

  // Priority 1.5: Try common internal cover paths (batch scan - single ZIP pass)
  static const char* const commonCoverPaths[] = {
      // Root level
      "cover.jpg",
      "cover.jpeg",
      "cover.png",
      // images/ directory (lowercase - most common)
      "images/cover.jpg",
      "images/cover.jpeg",
      "images/cover.png",
      // Images/ directory (capitalized - common in Calibre)
      "Images/cover.jpg",
      "Images/cover.jpeg",
      "Images/cover.png",
      // OEBPS/ root
      "OEBPS/cover.jpg",
      "OEBPS/cover.jpeg",
      "OEBPS/cover.png",
      // OEBPS/images/
      "OEBPS/images/cover.jpg",
      "OEBPS/images/cover.jpeg",
      "OEBPS/images/cover.png",
      // OEBPS/Images/
      "OEBPS/Images/cover.jpg",
      "OEBPS/Images/cover.jpeg",
      "OEBPS/Images/cover.png",
      // OPS/ variants (older EPUB 2)
      "OPS/cover.jpg",
      "OPS/cover.jpeg",
      "OPS/cover.png",
      "OPS/images/cover.jpg",
      "OPS/images/cover.jpeg",
      "OPS/images/cover.png",
      "OPS/Images/cover.jpg",
      "OPS/Images/cover.jpeg",
      "OPS/Images/cover.png",
      // EPUB/ variants (EPUB 3)
      "EPUB/cover.jpg",
      "EPUB/cover.jpeg",
      "EPUB/cover.png",
      "EPUB/images/cover.jpg",
      "EPUB/images/cover.jpeg",
      "EPUB/images/cover.png",
      // EPUB/Images/ (capitalized, for consistency with OEBPS/Images/)
      "EPUB/Images/cover.jpg",
      "EPUB/Images/cover.jpeg",
      "EPUB/Images/cover.png",
  };
  constexpr int commonCoverPathsCount = sizeof(commonCoverPaths) / sizeof(commonCoverPaths[0]);

  // Use single ZipFile instance with batch lookup for efficiency
  ZipFile zip(filepath);
  const int foundIndex = zip.findFirstExisting(commonCoverPaths, commonCoverPathsCount);
  if (foundIndex >= 0) {
    const char* path = commonCoverPaths[foundIndex];
    // Note: No file size check needed - converters have built-in limits:
    // - JPEG: MAX_MCU_ROW_BYTES=64KB limits width to ~4K-8K pixels
    // - PNG: MAX_IMAGE_WIDTH=2048, MAX_IMAGE_HEIGHT=3072
    LOG_DBG(TAG, "Found cover via heuristic: %s", path);

    const std::string ext = FsHelpers::isJpegFile(path) ? ".jpg" : ".png";
    const auto coverTempPath = getCachePath() + "/.cover" + ext;

    File coverFile = LittleFS.open(coverTempPath.c_str(), "w");
    if (coverFile) {
      if (readItemContentsToStream(path, coverFile, 1024)) {
        coverFile.close();
        ImageConvertConfig config;
        config.oneBit = use1BitDithering;
        config.logTag = "EBP";
        config.outputOnLittleFs = true;
        if (ImageConverterFactory::convertToBmp(coverTempPath, coverPath, config)) {
          LittleFS.remove(coverTempPath.c_str());
          return true;
        }
        LittleFS.remove(coverPath.c_str());
      } else {
        coverFile.close();
      }
    }
    LittleFS.remove(coverTempPath.c_str());
  }

  auto writeFailureMarker = [&]() {
    File marker = LittleFS.open(failedMarkerPath.c_str(), "w");
    if (marker) marker.close();
  };

  // Priority 2: Internal EPUB cover via OPF metadata
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR(TAG, "Cannot generate cover BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_DBG(TAG, "No known cover image");
    writeFailureMarker();
    return false;
  }

  if (!ImageConverterFactory::isSupported(coverImageHref)) {
    LOG_ERR(TAG, "Unsupported cover image format");
    writeFailureMarker();
    return false;
  }

  // Extract cover image to LittleFS temp, convert.
  LOG_DBG(TAG, "Generating BMP from cover image");
  const std::string ext = FsHelpers::isJpegFile(coverImageHref) ? ".jpg" : ".png";
  const auto coverTempPath = getCachePath() + "/.cover" + ext;

  File coverFile = LittleFS.open(coverTempPath.c_str(), "w");
  if (!coverFile) {
    return false;
  }
  if (!readItemContentsToStream(coverImageHref, coverFile, 1024)) {
    coverFile.close();
    LittleFS.remove(coverTempPath.c_str());
    return false;
  }
  coverFile.close();

  ImageConvertConfig config;
  config.oneBit = use1BitDithering;
  config.logTag = "EBP";
  config.outputOnLittleFs = true;

  const bool success = ImageConverterFactory::convertToBmp(coverTempPath, coverPath, config);
  LittleFS.remove(coverTempPath.c_str());

  if (!success) {
    LOG_ERR(TAG, "Failed to generate BMP from cover image");
    LittleFS.remove(coverPath.c_str());
    writeFailureMarker();
  }
  LOG_INF(TAG, "Generated BMP from cover image, success: %s", success ? "yes" : "no");
  return success;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, const bool trailingNullByte) const {
  if (itemHref.empty()) {
    LOG_ERR(TAG, "Failed to read item, empty href");
    return nullptr;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);

  const auto content = ZipFile(filepath).readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    LOG_ERR(TAG, "Failed to read item %s", path.c_str());
    return nullptr;
  }

  return content;
}

const char* Epub::itemReadResultToString(const ItemReadResult result) {
  switch (result) {
    case ItemReadResult::Success:
      return "success";
    case ItemReadResult::Aborted:
      return "aborted";
    case ItemReadResult::NotFound:
      return "not-found";
    case ItemReadResult::ArchiveError:
      return "archive-error";
    case ItemReadResult::IoError:
      return "io-error";
    case ItemReadResult::WriteError:
      return "write-error";
    case ItemReadResult::OpenFailed:
      return "open-failed";
  }
  return "unknown";
}

Epub::ItemReadResult Epub::readItemContentsToStreamDetailed(const std::string& itemHref, Print& out,
                                                            const size_t chunkSize, uint8_t* dictBuffer,
                                                            const std::function<bool()>& shouldAbort) const {
  if (itemHref.empty()) {
    LOG_ERR(TAG, "Failed to read item, empty href");
    return ItemReadResult::NotFound;
  }

  // v2.0.174 — ARCHITECTURAL FIX for "InflateReader init failed" wall.  When
  // the caller passes dictBuffer=nullptr, uzlib heap-allocates its own 32 KB
  // ring buffer per call.  After even a single "switch books" sequence the
  // heap fragments enough that the 32 KB contiguous request fails (largest
  // free drops to ~30 KB).  Reuse the always-static 48 KB display framebuffer
  // (already idle during inflate on e-paper: panel-side RAM holds the visible
  // image; the framebuffer itself is only touched during render→upload
  // bursts that bracket the inflate window).  Zero added BSS cost.  Existing
  // callers that explicitly passed sharedZipDictBuffer() see no change.
  if (dictBuffer == nullptr) dictBuffer = sharedZipDictBuffer();

  // IRI references from OPF manifest or XHTML content may contain percent-
  // encoded characters and fragment identifiers.  Decode before ZIP lookup.
  const std::string path = FsHelpers::normalisePath(
      FsHelpers::percentDecode(FsHelpers::stripQueryAndFragment(itemHref)));
  switch (ZipFile(filepath).readFileToStreamDetailed(path.c_str(), out, chunkSize, dictBuffer, shouldAbort)) {
    case ZipFile::StreamReadResult::Success:
      return ItemReadResult::Success;
    case ZipFile::StreamReadResult::Aborted:
      return ItemReadResult::Aborted;
    case ZipFile::StreamReadResult::NotFound:
      return ItemReadResult::NotFound;
    case ZipFile::StreamReadResult::WriteError:
      return ItemReadResult::WriteError;
    case ZipFile::StreamReadResult::OpenFailed:
      return ItemReadResult::OpenFailed;
    case ZipFile::StreamReadResult::InvalidOffset:
    case ZipFile::StreamReadResult::UnsupportedMethod:
    case ZipFile::StreamReadResult::DecompressionError:
    case ZipFile::StreamReadResult::SizeMismatch:
      return ItemReadResult::ArchiveError;
    case ZipFile::StreamReadResult::AllocFailed:
    case ZipFile::StreamReadResult::ReadError:
      return ItemReadResult::IoError;
  }
  return ItemReadResult::ArchiveError;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize,
                                    uint8_t* dictBuffer, const std::function<bool()>& shouldAbort) const {
  return readItemContentsToStreamDetailed(itemHref, out, chunkSize, dictBuffer, shouldAbort) ==
         ItemReadResult::Success;
}

std::unique_ptr<ZipItemReader> Epub::openItemStream(const std::string& itemHref, const size_t chunkSize,
                                                    uint8_t* dictBuffer) const {
  if (itemHref.empty()) {
    LOG_ERR(TAG, "openItemStream: empty href");
    return nullptr;
  }
  // v2.0.174 — same nullptr→framebuffer fallback as readItemContentsToStreamDetailed.
  // This is the path EpubChapterParser::tryMarkerizeChapter takes for streaming
  // chapter XHTML through HtmlStripper; pre-fix it allocated 32 KB on the heap
  // per chapter parse and failed with `InflateReader init failed (largest free=
  // 30708)` whenever the second EPUB in a session needed a new chapter parse.
  // See the readItemContentsToStreamDetailed comment for the safety argument.
  if (dictBuffer == nullptr) dictBuffer = sharedZipDictBuffer();
  const std::string path = FsHelpers::normalisePath(
      FsHelpers::percentDecode(FsHelpers::stripQueryAndFragment(itemHref)));
  // ZipFile is a transient — the returned reader holds its own file handle
  // and outlives the ZipFile instance.
  return ZipFile(filepath).openItemStream(path.c_str(), chunkSize, dictBuffer);
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).getInflatedFileSize(path.c_str(), size);
}

bool Epub::getSpineItemSizes(std::vector<size_t>& sizes) const {
  const int spineCount = getSpineItemsCount();
  sizes.assign(spineCount > 0 ? static_cast<size_t>(spineCount) : 0, 0);
  if (spineCount <= 0) return false;

  std::vector<ZipFile::SizeTarget> targets;
  targets.reserve(static_cast<size_t>(spineCount));
  for (int i = 0; i < spineCount; ++i) {
    const auto spineItem = getSpineItem(i);
    const std::string path = FsHelpers::normalisePath(spineItem.href);
    if (path.empty() || path.size() >= 255) continue;
    targets.push_back(
        {ZipFile::fnvHash64(path.data(), path.size()),
         static_cast<uint16_t>(path.size()), static_cast<uint16_t>(i)});
  }
  if (targets.empty()) return false;
  std::sort(targets.begin(), targets.end());

  std::vector<uint32_t> rawSizes(static_cast<size_t>(spineCount), 0);
  const int matched = ZipFile(filepath).fillUncompressedSizes(targets, rawSizes);
  for (int i = 0; i < spineCount; ++i) {
    sizes[static_cast<size_t>(i)] = rawSizes[static_cast<size_t>(i)];
  }
  return matched > 0;
}

int Epub::getSpineItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  return bookMetadataCache->getSpineCount();
}

BookMetadataCache::SpineEntry Epub::getSpineItem(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR(TAG, "getSpineItem called but cache not loaded");
    return {};
  }

  if (spineIndex < 0 || spineIndex >= bookMetadataCache->getSpineCount()) {
    LOG_ERR(TAG, "getSpineItem index:%d is out of range", spineIndex);
    return bookMetadataCache->getSpineEntry(0);
  }

  return bookMetadataCache->getSpineEntry(spineIndex);
}

BookMetadataCache::TocEntry Epub::getTocItem(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR(TAG, "getTocItem called but cache not loaded");
    return {};
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_ERR(TAG, "getTocItem index:%d is out of range", tocIndex);
    return {};
  }

  return bookMetadataCache->getTocEntry(tocIndex);
}

int Epub::getTocItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }

  return bookMetadataCache->getTocCount();
}

int Epub::resolveTocSpineIndex(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR(TAG, "resolveTocSpineIndex called but cache not loaded");
    return -1;
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_ERR(TAG, "resolveTocSpineIndex: tocIndex %d out of range", tocIndex);
    return -1;
  }

  const auto tocEntry = bookMetadataCache->getTocEntry(tocIndex);
  if (tocEntry.spineIndex >= 0 && tocEntry.spineIndex < bookMetadataCache->getSpineCount()) {
    return tocEntry.spineIndex;
  }

  const std::string canonicalHref = canonicalizeHref(tocEntry.href);
  if (canonicalHref.empty()) {
    LOG_ERR(TAG, "resolveTocSpineIndex: empty href for TOC index %d", tocIndex);
    return -1;
  }

  for (int i = 0; i < bookMetadataCache->getSpineCount(); i++) {
    const auto spineEntry = bookMetadataCache->getSpineEntry(i);
    const std::string canonicalSpineHref = canonicalizeHref(spineEntry.href);
    if (hrefsEquivalent(canonicalSpineHref, canonicalHref)) {
      LOG_INF(TAG, "Recovered spine index %d for TOC index %d via href match: %s", i, tocIndex,
              canonicalHref.c_str());
      return i;
    }
  }

  LOG_ERR(TAG, "resolveTocSpineIndex: no spine match for TOC index %d href %s", tocIndex, canonicalHref.c_str());
  return -1;
}

// work out the section index for a toc index
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  const int spineIndex = resolveTocSpineIndex(tocIndex);
  if (spineIndex < 0) {
    LOG_ERR(TAG, "Section not found for TOC index %d", tocIndex);
    return 0;
  }
  return spineIndex;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const { return getSpineItem(spineIndex).tocIndex; }

int Epub::getSpineIndexForTextReference() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR(TAG, "getSpineIndexForTextReference called but cache not loaded");
    return 0;
  }
  LOG_DBG(TAG, "Core Metadata: cover(%d)=%s, textReference(%d)=%s",
          bookMetadataCache->coreMetadata.coverItemHref.size(), bookMetadataCache->coreMetadata.coverItemHref.c_str(),
          bookMetadataCache->coreMetadata.textReferenceHref.size(),
          bookMetadataCache->coreMetadata.textReferenceHref.c_str());

  if (bookMetadataCache->coreMetadata.textReferenceHref.empty()) {
    // there was no textReference in epub, so we return 0 (the first chapter)
    return 0;
  }

  // loop through spine items to get the correct index matching the text href
  for (size_t i = 0; i < getSpineItemsCount(); i++) {
    if (getSpineItem(i).href == bookMetadataCache->coreMetadata.textReferenceHref) {
      LOG_DBG(TAG, "Text reference %s found at index %d", bookMetadataCache->coreMetadata.textReferenceHref.c_str(), i);
      return i;
    }
  }
  // This should not happen, as we checked for empty textReferenceHref earlier
  LOG_ERR(TAG, "Section not found for text reference");
  return 0;
}
