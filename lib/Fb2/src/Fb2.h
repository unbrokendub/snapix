/**
 * Fb2.h
 *
 * FictionBook 2.0 XML e-book handler for Snapix Reader
 * Provides EPUB-like interface for FB2 file handling
 */

#pragma once

#include <expat.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/**
 * Fb2 File Handler
 *
 * Handles FB2 file loading, XML parsing, and metadata extraction.
 */
class Fb2 {
 public:
  struct TocItem {
    std::string title;
    int sectionIndex = -1;  // Sequential section number (0-based)
    uint32_t sourceOffset = 0;  // Byte offset of the section start in the FB2 source
    uint8_t depth = 0;  // Nesting level in the FB2 section tree
  };

  struct BinaryEntry {
    uint32_t fileOffset = 0;  // Byte offset where base64 content starts in FB2 source
                              // (i.e. immediately after the opening <binary ...> tag).
    uint32_t byteLength = 0;  // Length of the base64 content in source bytes
                              // (between the opening tag and </binary>).
    uint8_t mimeType = 0;     // 0 = image/jpeg, 1 = image/png, 2 = other
  };

 private:
  std::string filepath;
  std::string cachePath;
  std::string title;
  std::string author;
  std::string coverPath;
  size_t fileSize;
  bool loaded;

  // XML parsing state
  XML_Parser xmlParser_ = nullptr;
  int depth = 0;
  int skipUntilDepth = INT_MAX;  // Skip content inside binary tags

  // Metadata extraction state
  bool inBookTitle = false;
  bool inFirstName = false;
  bool inLastName = false;
  bool inAuthor = false;
  bool inTitleInfo = false;
  bool inCoverPage = false;
  std::string currentAuthorFirst;
  std::string currentAuthorLast;

  // Body tracking (for TOC section counting)
  bool inBody = false;
  int bodyCount_ = 0;

  // TOC extraction state (tocItems_ used only during initial parse, then freed)
  std::vector<TocItem> tocItems_;
  // Lazy TOC: compact LUT of file offsets into meta.bin (4 bytes/entry vs ~68 bytes/entry)
  std::vector<uint32_t> tocLut_;
  uint16_t tocItemCount_ = 0;
  int sectionCounter_ = 0;
  int sectionDepth_ = 0;
  uint32_t currentSectionOffset_ = 0;
  bool inSectionTitle_ = false;
  int sectionTitleDepth_ = 0;
  std::string currentSectionTitle_;

  // Binary (image) index: binary id → file offset / length of base64 data.
  // Built during the initial XML parse and persisted in meta.bin.  Lets the
  // chapter parser jump directly to a referenced <image l:href="#id"/> at
  // render time without re-scanning the whole file.
  //
  // v2.0.179 — replaced std::unordered_map with a sorted std::vector.  For a
  // typical FB2 (~30 embedded images, occasionally up to ~100), the vector
  // drops ~1-4 KB of hash-bucket overhead per book load.  Build phase
  // appends entries unsorted; first lookup sorts once, subsequent lookups
  // are O(log N) via binary_search.  Iteration (during meta.bin save) is
  // order-independent so no sort needed for that path.
  using BinaryIndexEntry = std::pair<std::string, BinaryEntry>;
  // mutable: lazy-sort runs inside const lookup paths (cacheImage,
  // peekImageDims, decodeImageDirect).  The vector's CONTENT is logically
  // immutable after the build phase — only the iteration order changes
  // when the first lookup triggers the one-time sort.
  mutable std::vector<BinaryIndexEntry> binaryIndex_;
  mutable bool binaryIndexSorted_ = true;  // empty vector is trivially sorted

  void binaryIndexInsert(std::string id, const BinaryEntry& entry) {
    binaryIndex_.emplace_back(std::move(id), entry);
    binaryIndexSorted_ = false;
  }
  const BinaryEntry* binaryIndexFind(const std::string& id) const {
    if (!binaryIndexSorted_) {
      std::sort(binaryIndex_.begin(), binaryIndex_.end(),
                [](const BinaryIndexEntry& a, const BinaryIndexEntry& b) { return a.first < b.first; });
      binaryIndexSorted_ = true;
    }
    auto it = std::lower_bound(
        binaryIndex_.begin(), binaryIndex_.end(), id,
        [](const BinaryIndexEntry& entry, const std::string& key) { return entry.first < key; });
    if (it != binaryIndex_.end() && it->first == id) return &it->second;
    return nullptr;
  }

  // In-memory list of binary ids whose `cacheImage[fast]` base64 stream was
  // aborted mid-flight (typically because the user pressed page-turn while
  // the BG worker was still decoding).  Each entry remembers the maxBox
  // dimensions the parser asked for so retryDeferredImages() can reproduce
  // the same scaledFit() the parser would have used originally.  The list
  // is mutable so const cacheImage() can append to it; entries are popped
  // by retryDeferredImages() during the next BG sweep.
  struct DeferredImage {
    std::string binaryId;
    int maxBoxWidth = 0;
    int maxBoxHeight = 0;
  };
  mutable std::vector<DeferredImage> deferredImages_;

  // Cached JPEG source dimensions populated by `peekImageDims` /
  // cacheImage[fast]'s pump-based peek path.  Reading the JPEG SOF marker
  // requires opening the FB2 source and decoding ~1 KB of base64; caching
  // the result lets repeat `peekImageDims` calls (parser sometimes asks for
  // the same binary twice during hot-extend) be a free vector lookup.
  //
  // v2.0.179 — replaced std::unordered_map with linear-scan vector.  This
  // cache is small (~5-15 entries per book, one per uniquely-peeked image),
  // so the linear scan is faster in practice than hashing + bucket walk.
  // Saves the ~40 B/entry hash overhead — modest but free.
  struct PeekedDimsEntry {
    std::string id;
    uint16_t width;
    uint16_t height;
  };
  mutable std::vector<PeekedDimsEntry> peekedSourceDims_;

  // In-memory queue of binary ids whose JPEG BMP has not yet been
  // materialised on disk.  Populated by cacheImage[fast] (as the parser
  // walks the page emitting <image> tags) and drained by decodePendingImages
  // (BG worker calls decodeImageDirect for each id).  v2.0.50: replaces the
  // legacy `pending/<id>.jpg` directory listing that needed a full base64
  // → JPEG file write before the BG worker even saw the image.
  //
  // `retries` is incremented every time decodeImageDirect aborts mid-flight
  // (typically because the user pressed page-turn during a HW /1 full
  // decode that takes ~5-15 s).  After kMaxRetries we give up and mark
  // the binary as .failed — protects against heap-low aborts that would
  // otherwise re-queue infinitely.
  struct PendingJpegDecode {
    std::string binaryId;
    int maxBoxWidth = 0;
    int maxBoxHeight = 0;
    uint8_t retries = 0;
  };
  static constexpr uint8_t kMaxJpegDecodeRetries = 10;
  mutable std::vector<PendingJpegDecode> pendingJpegDecodes_;

  // In-memory tracker for stage decodes that failed mid-flight (most often
  // the heap watchdog firing during a heavy mid/high decode for a
  // small-but-tall source).  Without this we'd retry the same stage on
  // every BG idle wake and the worker would loop forever burning CPU.
  // Cleared on Fb2 destruction so a fresh book load gets a clean slate.
  mutable std::unordered_set<std::string> failedStageKeys_;

  // Binaries for which every progressive stage is either skipped (no HW
  // scale benefit) or marked failed.  These are pinned at whatever lower
  // stage already exists on disk — ImageBlock::render falls back to that
  // stage indefinitely.  Without this set the BG worker would wake up on
  // every post-render and re-enter decodePendingImages, which would
  // re-evaluate all stages, hit the same skips/failures, and return 0 —
  // wasting ~50 ms of dir-scan + state-build on every page-turn.
  mutable std::unordered_set<std::string> givenUpBinaries_;
  // Tracking state during parse — which <binary> block we're currently inside
  // (empty when not in a binary block).
  std::string currentBinaryId_;
  uint8_t currentBinaryMime_ = 0;
  uint32_t currentBinaryStart_ = 0;

  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElement(void* userData, const XML_Char* name);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);

  // v3.1.1 — OOM-guarded wrappers, the ONLY handlers actually registered
  // with Expat.  A bad_alloc thrown inside a raw handler must unwind
  // through Expat's C frames (no unwind tables) to reach parseXmlStream's
  // try/catch — that path hits __cxa_call_terminate → abort() → reboot,
  // which is exactly the hardware crash v2.0.197's outer catch FAILED to
  // stop (observed again on v3.0.x: third-book FB2 load aborted right
  // after "Found cover reference").  The wrappers catch INSIDE the C++
  // callback frame (zero C frames between throw and catch), set
  // metaParseOom_, and XML_StopParser — the parse then fails gracefully.
  static void XMLCALL startElementGuarded(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL endElementGuarded(void* userData, const XML_Char* name);
  static void XMLCALL characterDataGuarded(void* userData, const XML_Char* s, int len);
  bool metaParseOom_ = false;

  // Helper methods
  bool parseXmlStream();
  void postProcessMetadata();
  bool loadMetaCache();
  bool saveMetaCache() const;
  std::string metaCachePath() const;

 public:
  explicit Fb2(std::string filepath, const std::string& cacheDir);
  ~Fb2();

  /**
   * Load FB2 file (verify existence and parse metadata)
   * @return true on success
   */
  bool load();

  /**
   * Clear cached data
   * @return true on success
   */
  bool clearCache() const;

  /**
   * Setup cache directory
   */
  void setupCacheDir() const;

  // Path accessors
  const std::string& getCachePath() const { return cachePath; }
  const std::string& getPath() const { return filepath; }

  // Metadata
  const std::string& getTitle() const { return title; }
  const std::string& getAuthor() const { return author; }
  size_t getFileSize() const { return fileSize; }

  // Cover image support
  std::string getCoverBmpPath() const;
  bool generateCoverBmp(bool use1BitDithering = false) const;
  std::string getThumbBmpPath() const;
  bool generateThumbBmp() const;

  /**
   * Read content from file at specified offset
   * @param buffer Output buffer
   * @param offset Byte offset in file
   * @param length Number of bytes to read
   * @return Number of bytes actually read
   */
  size_t readContent(uint8_t* buffer, size_t offset, size_t length) const;

  /**
   * Find a cover image in the same directory as the FB2 file
   * @return Path to cover image, or empty string if not found
   */
  std::string findCoverImage() const;

  // Check if file is loaded
  bool isLoaded() const { return loaded; }

  // TOC access (lazy: reads from cache file on demand)
  uint16_t tocCount() const { return tocItemCount_; }
  TocItem getTocItem(uint16_t index) const;
  // Read all TOC source offsets while holding one LittleFS handle.  Intended
  // for global page estimates; avoids getTocItem() reopening meta.bin once or
  // twice per section during the first status-bar render.
  bool getTocSourceOffsets(std::vector<uint32_t>& offsets) const;

  // Binary (image) index access — returns nullptr if id not found.
  // v2.0.179 — delegates to the sorted-vector lookup helper.
  const BinaryEntry* findBinary(const std::string& id) const { return binaryIndexFind(id); }
  size_t binaryCount() const { return binaryIndex_.size(); }

  /**
   * Decode a referenced binary block to a cached BMP on SD card.  Idempotent:
   * if the cached BMP already exists with sane dimensions, returns its path
   * without re-decoding.  Used by the chapter parser to materialise inline
   * <image> references during page layout.
   *
   * @param binaryId       The <binary id> referenced by an <image l:href="#id"/>
   * @param outBmpPath     On success: filesystem path to the cached BMP
   * @param outWidth       On success: BMP width (or scaled-fit width in fast mode)
   * @param outHeight      On success: BMP height (or scaled-fit height in fast mode)
   * @param maxBoxWidth    Output BMP scaled to fit at most this many pixels wide
   * @param maxBoxHeight   ...and at most this many pixels tall
   * @param fastMode       If true: stream base64 to a `pending/<id>.<ext>` file
   *                       and peek the JPEG / PNG header for dimensions, but
   *                       skip the (slow) pixel decode.  outBmpPath points at
   *                       the *expected* BMP location — the file does not yet
   *                       exist; ImageBlock::render shows a placeholder until
   *                       decodePendingImages() materialises it.  Default
   *                       false preserves the historical synchronous behavior
   *                       (used by cover / thumbnail paths).
   * @return true on success, false if the binary is missing / cannot be decoded
   */
  bool cacheImage(const std::string& binaryId, std::string& outBmpPath, uint16_t& outWidth, uint16_t& outHeight,
                  int maxBoxWidth, int maxBoxHeight, bool fastMode = false,
                  const std::function<bool()>& shouldAbort = nullptr) const;

  /**
   * BG worker entry point: scan the `<cachePath>/images/pending/` directory and
   * decode each pending JPEG / PNG into the matching `<id>.bmp` next to it.
   * Idempotent and abort-able — call repeatedly from a low-priority task.
   *
   * @param shouldAbort  Polled between images; returning true stops the worker.
   * @return number of images decoded during this invocation.
   */
  int decodePendingImages(const std::function<bool()>& shouldAbort = nullptr) const;

  // Predicate: are there pending decodes left on disk?  Cheap directory check
  // — call from the reader's idle loop to decide whether to spawn a BG worker.
  bool hasPendingImages() const;

  // ====================================================================
  // v2.0.50 direct-stream pipeline
  // ====================================================================
  //
  // peekImageDims: parse the embedded JPEG's SOF marker by streaming JUST
  // ENOUGH bytes (~1 KB of base64 → ~750 bytes of JPEG) from the FB2 source.
  // Replaces the old "stream the whole 200 KB base64 to a pending file then
  // peekDimensions() the pending file" path that v2.0.x used — at ~10 ms
  // total (vs. ~600 ms post-buffer-bump) it can run inline from the
  // parser's <image> handler with no perceivable hitch.  Returns false on:
  //   - binaryId not present in the index
  //   - mimeType isn't JPEG (PNG falls back to legacy pending-file path)
  //   - JPEG is malformed (no SOF in first 1 KB)
  bool peekImageDims(const std::string& binaryId, uint16_t& outSrcW, uint16_t& outSrcH) const;

  // decodeImageDirect: JPEG decode straight from FB2 source through a
  // Base64JpegPump into a 1-bit BMP at `/img/fb2_<hash>/<id>.bmp` ON
  // INTERNAL FLASH (LittleFS).  No intermediate `pending/<id>.jpg` is ever
  // created — the pump feeds JPEGDEC bytes inline, decoded base64 is
  // consumed as fast as it's produced.  Atomic .bmp.tmp + rename.
  //
  // PNG is NOT supported here yet (pngle has no equivalent stream-pump API);
  // PNG binaries fall through to the legacy hasPendingImages / decodePending
  // pipeline.
  //
  // Returns true if a BMP landed on flash; false on any error or abort.
  bool decodeImageDirect(const std::string& binaryId, int targetMaxWidth, int targetMaxHeight,
                         const std::function<bool()>& shouldAbort = nullptr) const;

  // (Public LittleFS read helpers were considered for ImageBlock::render
  // but rejected to avoid the lib/RenderTypes → lib/Fb2 dependency cycle.
  // ImageBlock includes <LittleFS.h> directly instead.)

  /**
   * BG worker entry point: re-run cacheImage[fast] for every binary id whose
   * earlier stream was aborted (see DeferredImage above).  Each successful
   * retry repopulates the pending JPEG file, so the next decodePendingImages
   * sweep can produce a real BMP that replaces the on-page placeholder.
   *
   * @param shouldAbort  Honoured between retries AND inside the base64 stream.
   *                     A re-aborted entry is pushed back onto the deferred
   *                     list so it can be retried on the next BG sweep.
   * @return number of binaries whose pending file was successfully (re)written.
   */
  int retryDeferredImages(const std::function<bool()>& shouldAbort = nullptr) const;

  // Predicate: are there deferred binaries waiting to be retried?  Lets the
  // BG cache planner know to wake up even when the page cache itself is
  // complete and there are no `pending/<id>.{jpg,png}` files yet.
  bool hasDeferredImages() const { return !deferredImages_.empty(); }
};
