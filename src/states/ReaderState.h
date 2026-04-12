#pragma once

#include <BackgroundTask.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

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

namespace papyrix {

// Forward declarations
class Core;
struct Event;

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

  // Whether book has a valid cover image
  bool hasCover_ = false;

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
  bool directUiTransition_ = false;

  bool& thumbnailDone_;
  bool resumeBackgroundCachingAfterRender_ = false;
  uint32_t& lastIdleBackgroundKickMs_;
  uint32_t& lastReaderInteractionMs_;
  void startBackgroundCaching(Core& core, const char* trigger = "auto");
  bool stopBackgroundCaching();

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
  uint32_t& powerPressStartedMs_;

  // Rendering
  void renderCurrentPage(Core& core);
  void renderCachedPage(Core& core);
  void renderXtcPage(Core& core);
  bool renderCoverPage(Core& core);
  void renderLoadedPage(Core& core, const std::shared_ptr<Page>& page, size_t pageCount, bool cacheIsPartial,
                        const Theme& theme, const Viewport& vp, uint32_t totalStartMs, bool allowPagePrefetch);
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
  };
  struct SectionPageMetric {
    uint16_t pages = 0;
    bool exact = false;
  };
  void invalidateGlobalPageMetrics();
  void initializeGlobalPageMetrics(Core& core, int currentSectionTotalPages, bool currentSectionIsPartial);
  void updateGlobalPageMetrics(Core& core, int currentSectionTotalPages, bool currentSectionIsPartial);
  void recomputeGlobalPageMetricTotal();
  GlobalPageMetrics resolveGlobalPageMetrics(Core& core, int currentSectionTotalPages, bool currentSectionIsPartial);

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
  void renderCenteredStatusMessage(Core& core, const char* message);
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
  int lastTocScrollOffset_ = -1;
  bool tocOverlayRendered_ = false;
  bool& pendingTocJumpActive_;
  bool& pendingTocJumpIndexingShown_;
  int& pendingTocJumpTargetSpine_;
  int& pendingTocJumpTargetPageHint_;
  std::string& pendingTocJumpAnchor_;
  uint8_t& pendingTocJumpRetryCount_;
  uint32_t& pendingTocJumpStartedMs_;
  uint32_t& pendingTocJumpLastDiagMs_;
  bool& pendingEpubPageLoadActive_;
  bool& pendingEpubPageLoadMessageShown_;
  bool& pendingEpubPageLoadRequireComplete_;
  bool& pendingEpubPageLoadUseIndexingMessage_;
  int& pendingEpubPageLoadTargetSpine_;
  int& pendingEpubPageLoadTargetPage_;
  uint8_t& pendingEpubPageLoadRetryCount_;
  uint32_t& pendingEpubPageLoadStartedMs_;
  uint32_t& pendingEpubPageLoadLastDiagMs_;
  bool& pendingBackgroundEpubRefresh_;
  int& pendingBackgroundEpubRefreshSpine_;
  int& pendingBackgroundEpubRefreshPage_;
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

}  // namespace papyrix
