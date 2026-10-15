#pragma once

#include <BackgroundTask.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// v2.0.125 R3.6 — page-boundary snapshot type for the per-spine
// stream offset cache (SNAPIX_MARKERIZED_RENDER path only).  Empty
// header in default builds where the flag is OFF.
#if defined(SNAPIX_MARKERIZED_RENDER) && SNAPIX_MARKERIZED_RENDER
#include <MarkerizedPageRender.h>
#endif

namespace snapix::unifiedcache {
class UnifiedCache;
}

namespace xtc {
class CoverBmpWriter;
}

#include "../content/BookmarkManager.h"
#include "../content/ReaderNavigation.h"
#include "../core/Types.h"
#include "../rendering/XtcPageRenderer.h"
#include "../ui/views/HomeView.h"
#include "../ui/views/ReaderViews.h"
#include "reader/ReaderAsyncJobsController.h"
#include "reader/ReaderCacheController.h"
#include "reader/ReaderNavigationController.h"
#include "reader/ReaderTypes.h"
#include "State.h"

class ContentParser;
class GfxRenderer;
class PageCache;
class Page;
struct RenderConfig;
struct Theme;

namespace snapix {

// Forward declarations
class Core;
struct Event;
namespace unifiedcache {
class UnifiedCache;
}

// ReaderState - unified reader for all content types
// Uses ContentHandle to abstract Epub/Xtc/Txt/Markdown differences
// Uses PageCache for all formats with partial caching support
// Delegates to: XtcPageRenderer (binary rendering), ProgressManager (persistence),
//               ReaderNavigation (page traversal)
class ReaderState : public State {
 public:
  explicit ReaderState(GfxRenderer& renderer);
  ~ReaderState() override;

  void enter(Core& core) override;
  void exit(Core& core) override;
  StateTransition update(Core& core) override;
  void render(Core& core) override;
  StateId id() const override { return StateId::Reader; }

  // Set content path before entering state
  void setContentPath(const char* path);

  // Reading position
  uint32_t currentPage() const { return currentPage_; }
  void setCurrentPage(uint32_t page) { currentPage_ = page; }

 private:
  using BackgroundCacheWakeReason = reader::BackgroundCacheWakeReason;
  using BackgroundCachePlan = reader::BackgroundCachePlan;
  using Viewport = reader::Viewport;
  using WarmPageSlot = reader::WarmPageSlot;

  GfxRenderer& renderer_;
  XtcPageRenderer xtcRenderer_;
  char contentPath_[256];
  uint32_t currentPage_;
  bool needsRender_;
  bool contentLoaded_;
  bool loadFailed_ = false;  // Track if content loading failed (for error state transition)

  // Reading position (maps to ReaderNavigation::Position)
  int currentSpineIndex_;
  int currentSectionPage_;

  // Last successfully rendered position (for accurate progress saving)
  int lastRenderedSpineIndex_ = 0;
  int lastRenderedSectionPage_ = 0;

#if defined(SNAPIX_MARKERIZED_RENDER) && SNAPIX_MARKERIZED_RENDER
  // v2.0.125 R3.6 — per-spine in-memory cache of page-boundary
  // snapshots captured during streaming render.  Lets subsequent
  // renders within the same chapter session SKIP the MEASURE-only
  // walk through earlier pages: render of page N seeks the
  // markers file to `streamOffsetCache_[N].byteOffset` and tells
  // the paginator to start at `startPage=N` with
  // `styleBits=streamOffsetCache_[N].styleBits`.
  //
  // Cleared on spine change (different chapter = different
  // markers file = different offsets).  Lives only in RAM —
  // R3.6.b follow-up (later) persists to `markers/<spine>.idx`
  // for cross-session reuse.
  //
  // Vector indexed by page number; size grows as new pages are
  // discovered.  ~6 bytes per entry; 100 pages = 600 B per
  // spine.  Fits easily on heap once per active chapter.
  //
  // v2.0.128 R4.a — also persists to a `.idx` sidecar at
  // `<markers path>.idx`.  Loaded on first render of a spine,
  // saved on spine change.  The configHash field detects stale
  // .idx after font/margin/orientation changes.
  int streamOffsetCacheSpine_ = -1;
  std::vector<snapix::smolport::PageBoundarySnapshot> streamOffsetCache_;
  // v2.0.167 — replaced streamOffsetCacheIdxPath_ (str path to *.idx file)
  // with (bookCachePath + key) so idx writes route through UnifiedCache.
  std::string streamOffsetCacheBookPath_;
  int streamOffsetCacheKey_ = -1;
  uint16_t streamOffsetCacheConfigHash_ = 0;
  // v3.11.12 — the page render's UnifiedCache, reused across page turns while
  // UnifiedCache::writeGeneration() is unchanged (each fresh instance rescans
  // the whole streaming.cache: ~140 ms of a ~200 ms page render).  Dropped on
  // book exit so its directory never outlives the book.
  std::unique_ptr<snapix::unifiedcache::UnifiedCache> renderCache_;
  std::string renderCachePath_;
  uint32_t renderCacheGeneration_ = 0;
  snapix::unifiedcache::UnifiedCache& renderCacheFor(const std::string& bookCachePath);
  // v3.11.12 — true when `cache` already stores a current idx for the
  // outgoing spine with at least as many entries as streamOffsetCache_, so the
  // render-side copy must NOT overwrite it (a progressive chapter's parser
  // keeps extending the stored idx after the render loaded its copy).
  bool storedIdxCoversStreamOffsetCache(snapix::unifiedcache::UnifiedCache& cache);
#endif

  // Whether book has a valid cover image
  bool hasCover_ = false;

  // v3.11.12 — the cover BMP is decoded on ReaderAsync (it blocked the UI loop
  // for ~5 s on a first EPUB open).  Until it exists the cover page shows a
  // title placeholder; a failed generation leaves the cover page.
  reader::DeferredJobSlot coverJob_;
  bool coverPlaceholderShown_ = false;
  bool coverGenerationFailed_ = false;  // this session: no cover page

  // First text content spine index (from EPUB guide, 0 if not specified)
  int textStartIndex_ = 0;

  reader::ReaderCacheController cacheController_;
  reader::ReaderNavigationController navigationController_;
  reader::ReaderAsyncJobsController asyncJobs_;

  // Transitional aliases during the cleanup sprint. Actual ownership/state now
  // lives in dedicated controllers instead of implicit task-timing contracts.
  std::unique_ptr<PageCache>& pageCache_;
  std::unique_ptr<ContentParser>& parser_;
  int& parserSpineIndex_;
  std::unique_ptr<ContentParser>& lookaheadParser_;
  int& lookaheadParserSpineIndex_;
  uint8_t pagesUntilFullRefresh_;
  bool forceCleanRefreshOnNext_ = false;  // drive-all refresh after overlay banner
  bool directUiTransition_ = false;

  bool& thumbnailDone_;
  bool resumeBackgroundCachingAfterRender_ = false;
  uint32_t& lastIdleBackgroundKickMs_;
  uint32_t& lastReaderInteractionMs_;
  void startBackgroundCaching(Core& core, const char* trigger = "auto");
  bool stopBackgroundCaching();
  // v2.0.110 (audit fix #1): non-blocking variant for the render path.
  // Requests worker cancel and waits at most ~500 ms for the honest
  // cooperative-cancel to fire.  If worker doesn't yield by then,
  // log and return false WITHOUT calling ESP.restart() — render proceeds
  // anyway against the on-disk cache state.  Use the full
  // `stopBackgroundCaching()` for teardown paths where parser state
  // must be fully released before mutation.
  bool requestBackgroundCachingPause(const char* siteTag);

  // Navigation helpers (delegates to ReaderNavigation)
  void navigateNext(Core& core);
  void navigatePrev(Core& core);
  void navigateNextChapter(Core& core);
  void navigatePrevChapter(Core& core);
  void applyNavResult(const ReaderNavigation::NavResult& result, Core& core);

  // Track whether a chapter jump already fired during a button hold
  bool& holdNavigated_;

  // Track power press start when short power action is mapped to page turn.
  // This lets us execute page turn only on short release and avoid accidental
  // turns when the same press is held to enter sleep.
  bool& powerPressActive_;
  uint32_t& powerPressStartedMs_;

  // Rendering
  void renderCurrentPage(Core& core);
  void renderCachedPage(Core& core);
  void renderXtcPage(Core& core);
  // v3.11.12 — XTCH gray passes run as their own UI-loop step (see
  // XtcPageRenderer deferGray); the cover/thumbnail are built in bounded
  // strips between frames instead of one 96 KB allocation after the first page.
  bool xtcGrayPassDue_ = false;
  std::unique_ptr<xtc::CoverBmpWriter> xtcCoverWriter_;
  enum class XtcCoverPhase : uint8_t { Idle, Cover, Thumb };
  XtcCoverPhase xtcCoverPhase_ = XtcCoverPhase::Idle;
  void beginXtcCoverJob(Core& core);
  void runXtcCoverSlice(Core& core);
  void cancelXtcCoverJob();
  bool renderCoverPage(Core& core);
  void renderLoadedPage(Core& core, const std::shared_ptr<Page>& page, size_t pageCount, bool cacheIsPartial,
                        const Theme& theme, const Viewport& vp, uint32_t totalStartMs, bool allowPagePrefetch,
                        bool pageGlyphsWarm = false);
  bool tryFastNavigateNext(Core& core);
  bool tryFastNavigateWithinCurrentCache(Core& core, int direction);
  void enqueuePendingPageTurn(int direction, const char* reason);
  bool deferPageTurnUntilCacheStops(int direction);

  // Helpers
  void renderPageContents(Core& core, Page& page, int marginTop, int marginRight, int marginBottom, int marginLeft);
  void renderStatusBar(Core& core, int marginRight, int marginBottom, int marginLeft, int totalPages = -1,
                       bool isPartial = false);
  struct GlobalPageMetrics {
    int currentPage = 1;
    int totalPages = 0;
    bool totalIsExact = true;
  };
  struct SectionPageMetric {
    uint16_t pages = 0;
    bool exact = false;
    uint32_t byteSize = 0;  // raw section size in source bytes — kept so the
                            // bytes-per-page calibration can be re-derived as
                            // more sections become exact during a session
  };
  void invalidateGlobalPageMetrics();
  void initializeGlobalPageMetrics(Core& core, int currentSectionTotalPages, bool currentSectionIsPartial);
  void updateGlobalPageMetrics(Core& core, int currentSectionTotalPages, bool currentSectionIsPartial);
  void recalibrateGlobalPageEstimates();
  void recomputeGlobalPageMetricTotal();
  // Persist `globalSectionPageMetrics_` between sessions.  A cache hit is
  // checked before any per-section I/O; a miss is filled on ReaderAsync (one
  // batch metadata pass) instead of probing every page-cache file
  // synchronously during the first visible render.
  GlobalPageMetrics resolveGlobalPageMetrics(Core& core, int currentSectionTotalPages, bool currentSectionIsPartial);

  // v3.11.12 — global metrics I/O off the UI loop.  A miss of the persisted
  // frame is filled on ReaderAsync (one bulk metadata pass + persist; it took
  // 1.6 s inside the first FB2 page render) and every later persist runs
  // there too.  The status bar shows the local fallback until it lands.
  struct GlobalMetricsJobIo {
    bool computeEstimates = false;  // false: only persist `metrics`
    ContentType type = ContentType::None;
    uint32_t configHash = 0;
    int spineCount = 0;
    std::string bookCachePath;
    std::vector<SectionPageMetric> metrics;  // in: snapshot to persist / out: estimates
    const char* source = "";                 // out: legacy | batch
    bool persisted = false;                  // out
  };
  reader::DeferredJobSlot metricsJob_;
  GlobalMetricsJobIo metricsJobIo_;
  bool metricsDiskProbed_ = false;    // the persisted frame was looked up this session
  bool metricsComputeFailed_ = false;
  bool metricsSaveDirty_ = false;     // exact sections changed since the last persist
  std::string metricsBookCachePath_;  // where/with which config the metrics persist (UI-owned)
  uint32_t metricsConfigHash_ = 0;
  int fb2FallbackOffsetSpine_ = -1;   // memo for the FB2 crude-scaling fallback
  uint32_t fb2FallbackOffset_ = 0;
  int fb2SectionMemoSpine_ = -1;      // memo: TOC index → FB2 section (markers key)
  int fb2SectionMemoIndex_ = -1;
  static bool parseGlobalMetricsPayload(const std::vector<uint8_t>& payload, uint32_t configHash, int spineCount,
                                        std::vector<SectionPageMetric>& out);
  static bool writeGlobalMetricsFrame(unifiedcache::UnifiedCache& cache, const std::vector<SectionPageMetric>& metrics,
                                      uint32_t configHash);
  static bool buildGlobalMetricEstimates(Core& core, ContentType type, int spineCount,
                                         std::vector<SectionPageMetric>& out);
  void requestGlobalMetricsCompute(ContentType type, const std::string& bookCachePath, uint32_t configHash,
                                   int spineCount);
  void scheduleGlobalMetricsSave() { metricsSaveDirty_ = true; }
  void flushGlobalMetricsSaveNow(Core& core);
  void resetDeferredJobs();

  // v3.11.12 — deferred (aux) work hand-off: queue pending jobs ahead of the
  // cache job, and consume their results on the UI loop.
  void queueDeferredJobs(Core& core);
  uint32_t lastDeferredKickMs_ = 0;
  void pollDeferredJobs(Core& core);
  void runAuxJob(const reader::ReaderAsyncJobsController::AuxRequest& request,
                 const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort);
  void runCoverJob(const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort);
  void runGlobalMetricsJob(const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort);
  bool onCoverPage() const { return currentSpineIndex_ == 0 && currentSectionPage_ == -1; }
  void leaveCoverPage(Core& core);
  void renderCoverPlaceholder(Core& core);

  // Cache management
  static constexpr uint16_t kDefaultCacheBatchPages = 5;
  bool ensurePageCached(Core& core, uint16_t pageNum);
  void loadCacheFromDisk(Core& core);
  void reloadCacheFromDisk(Core& core);
  void createOrExtendCache(Core& core, uint16_t batchSize = kDefaultCacheBatchPages);
  void clearPagePrefetch();
  void prefetchAdjacentPage(Core& core);
  void armPendingEpubPageLoad(Core& core, int targetSpine, int targetPage, bool requireComplete,
                              bool useIndexingMessage);
  void clearPendingEpubPageLoad();
  void processPendingEpubPageLoad(Core& core);
  void startPendingEpubPageLoadBackgroundWork(Core& core);

  void createOrExtendCacheImpl(ContentParser& parser, const std::string& cachePath, const RenderConfig& config,
                               uint16_t batchSize = kDefaultCacheBatchPages);
  bool prefetchNextEpubSpineCache(Core& core, const RenderConfig& config, int activeSpineIndex, bool coverExists,
                                  int textStartIndex, bool allowFarSweep, const std::function<bool()>& shouldAbort);
  void clearLookaheadParser();
  bool promoteLookaheadParser(int targetSpine);
  void resetBackgroundPrefetchState();
  BackgroundCachePlan planBackgroundCacheWork(Core& core);
  static const char* backgroundCacheWakeReasonToString(BackgroundCacheWakeReason reason);
  bool shouldContinueIdleBackgroundCaching(Core& core);

  // Display helpers
  void displayWithRefresh(Core& core);
  // v3.11.12 — returns false (nothing drawn) while a background inflate is
  // using the framebuffer as its dictionary; callers retry on a later tick.
  bool renderCenteredStatusMessage(Core& core, const char* message, int fontIdOverride = 0,
                                   const char* detail = nullptr);
  bool renderLoadingStatusMessage(Core& core);

  // v2.0.205 — the loading/indexing overlay is a single STATIC banner drawn
  // by renderCenteredStatusMessage over the existing screen content.  The
  // animated spinner (v2.0.196-204) was removed: the ESP32-C3 cannot repaint
  // it without starving the FB2/EPUB parse worker that shares the SPI bus, so
  // it made cold loads dramatically slower for no real UX gain.
  Viewport getReaderViewport(bool showStatusBar) const;
  bool isWorkerRunning() const;
  BackgroundTask::State workerState() const;
  void requestWorkerCancel();
  bool waitWorkerIdle(uint32_t maxWaitMs = 0);
  void runBackgroundCacheJob(const reader::ReaderAsyncJobsController::BackgroundCacheRequest& request,
                             const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort);
  void runTocJumpJob(const reader::ReaderAsyncJobsController::TocJumpRequest& request,
                     const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort);
  void runPageFillJob(const reader::ReaderAsyncJobsController::PageFillRequest& request,
                      const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort);

  // Get first content spine index (skips cover document when appropriate)
  static int calcFirstContentSpine(bool hasCover, int textStartIndex, size_t spineCount);

  // Anchor-to-page persistence for intra-spine TOC navigation
  static void saveAnchorMap(const ContentParser& parser, const std::string& cachePath);
  static int loadAnchorPage(const std::string& cachePath, const std::string& anchor);
  static std::vector<std::pair<std::string, uint16_t>> loadAnchorMap(const std::string& cachePath);
  const std::vector<std::pair<std::string, uint16_t>>& getCachedAnchorMap(const std::string& cachePath,
                                                                          int spineIndex);
  void invalidateAnchorMapCache();

  // Source state (where reader was opened from)
  StateId sourceState_ = StateId::Home;
  Core* activeCore_ = nullptr;

  // Cached chapter title for StatusChapter mode (avoids SD I/O on every render)
  char cachedChapterTitle_[64] = "";
  int cachedChapterSpine_ = -1;
  int cachedChapterPage_ = -1;
  std::vector<SectionPageMetric> globalSectionPageMetrics_;
  uint32_t globalSectionPageMetricTotal_ = 0;
  bool globalSectionPageMetricsInitialized_ = false;
  WarmPageSlot& warmedNextPage_;
  WarmPageSlot& warmedNextNextPage_;
  WarmPageSlot& renderOverridePage_;

  // TOC overlay mode
  bool tocMode_ = false;
  ui::ChapterListView tocView_;
  bool tocViewPopulated_ = false;  // v3.11.12 — the book's TOC never changes in a session
  int lastTocScrollOffset_ = -1;
  bool tocOverlayRendered_ = false;
  bool& pendingTocJumpActive_;
  bool& pendingTocJumpIndexingShown_;
  bool& pendingTocJumpDeferredDisplay_;  // v2.0.84: skip "Indexing..." banner when spine cache exists
  int& pendingTocJumpTargetSpine_;
  int& pendingTocJumpTargetPageHint_;
  std::string& pendingTocJumpAnchor_;
  uint8_t& pendingTocJumpRetryCount_;
  uint32_t& pendingTocJumpStartedMs_;
  uint32_t& pendingTocJumpLastDiagMs_;
  std::atomic<bool>& pendingTocFirstPageReady_;  // worker→main signal for deferred-display
  bool& pendingEpubPageLoadActive_;
  bool& pendingEpubPageLoadMessageShown_;
  bool& pendingEpubPageLoadRequireComplete_;
  bool& pendingEpubPageLoadUseIndexingMessage_;
  int& pendingEpubPageLoadTargetSpine_;
  int& pendingEpubPageLoadTargetPage_;
  uint8_t& pendingEpubPageLoadRetryCount_;
  uint32_t& pendingEpubPageLoadStartedMs_;
  uint32_t& pendingEpubPageLoadLastDiagMs_;
  uint32_t& pendingEpubPageLoadNextRetryMs_;
  int& queuedPendingEpubTurn_;
  uint32_t& queuedPendingEpubTurnQueuedMs_;
  uint32_t& lastCachePreemptRequestedMs_;

  void enterTocMode(Core& core);
  void exitTocMode();
  void handleTocInput(Core& core, const Event& e);
  void renderTocOverlay(Core& core);
  int tocVisibleCount() const;
  void populateTocView(Core& core);
  int findCurrentTocEntry(Core& core);
  void jumpToTocEntry(Core& core, int tocIndex);
  void processPendingTocJump(Core& core);
  void startPendingTocJumpBackgroundWork(Core& core);

  // Menu overlay mode
  bool menuMode_ = false;
  ui::ReaderMenuView menuView_;
  void enterMenuMode(Core& core);
  void exitMenuMode();
  void handleMenuInput(Core& core, const Event& e);
  void handleMenuAction(Core& core, int action);

  // Bookmark overlay mode
  Bookmark bookmarks_[BookmarkManager::MAX_BOOKMARKS];
  int bookmarkCount_ = 0;
  bool bookmarkMode_ = false;
  ui::BookmarkListView bookmarkView_;
  int lastBookmarkScrollOffset_ = -1;
  bool bookmarkOverlayRendered_ = false;
  void enterBookmarkMode(Core& core);
  void exitBookmarkMode();
  void handleBookmarkInput(Core& core, const Event& e);
  void renderBookmarkOverlay(Core& core);
  void addBookmark(Core& core);
  void deleteBookmark(Core& core, int index);
  void jumpToBookmark(Core& core, int index);
  void saveBookmarks(Core& core);
  void populateBookmarkView();
  int bookmarkVisibleCount() const;

  // Boot mode transition - exit to UI via restart
  StateTransition exitToUI(Core& core);
};

}  // namespace snapix
