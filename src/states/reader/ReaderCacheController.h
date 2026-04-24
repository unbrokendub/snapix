#pragma once

#include <ContentParser.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../../content/ContentTypes.h"
#include "ReaderDocumentResources.h"
#include "ReaderSupport.h"
#include "ReaderTypes.h"

class Page;
class PageCache;
struct RenderConfig;

namespace snapix {
class Core;
}

namespace snapix::reader {

class ReaderCacheController {
 public:
  explicit ReaderCacheController(GfxRenderer& renderer, PositionRefs position);

  void setContentPath(const char* path);
  void resetSession();

  void clearPagePrefetch();
  void clearLookaheadParser();
  void resetBackgroundPrefetchState();
  static const char* backgroundCacheWakeReasonToString(BackgroundCacheWakeReason reason);

  bool promoteLookaheadParser(int targetSpine);
  void prefetchAdjacentPage(Core& core);

  bool hasRenderOverride(int spine, int sectionPage) const;
  WarmPageSlot takeRenderOverride();
  void clearRenderOverride();
  void setRenderOverride(WarmPageSlot slot);
  const WarmPageSlot& warmedNextPage() const { return warmedNextPage_; }
  const WarmPageSlot& warmedNextNextPage() const { return warmedNextNextPage_; }
  void consumeWarmForwardPage();

  bool hasPageCache() const;
  PageCache* pageCache();
  const PageCache* pageCache() const;
  bool pageCachePartial() const;
  size_t pageCacheCount() const;

  bool thumbnailDone() const { return thumbnailDone_; }
  bool& thumbnailDoneRef() { return thumbnailDone_; }
  void setThumbnailDone(bool value) { thumbnailDone_ = value; }
  uint32_t lastIdleBackgroundKickMs() const { return lastIdleBackgroundKickMs_; }
  uint32_t& lastIdleBackgroundKickMsRef() { return lastIdleBackgroundKickMs_; }
  void markIdleBackgroundKick(uint32_t nowMs) { lastIdleBackgroundKickMs_ = nowMs; }
  uint32_t lastReaderInteractionMs() const { return lastReaderInteractionMs_; }
  uint32_t& lastReaderInteractionMsRef() { return lastReaderInteractionMs_; }
  void markReaderInteraction(uint32_t nowMs) { lastReaderInteractionMs_ = nowMs; }

  void loadCacheFromDisk(Core& core, const Viewport& viewport);
  void reloadCacheFromDisk(Core& core, const Viewport& viewport);
  void createOrExtendCache(Core& core, const Viewport& viewport, uint16_t batchSize = kDefaultCacheBatchPages);

  void createOrExtendCacheImpl(ContentParser& parser, const std::string& cachePath, const RenderConfig& config,
                               uint16_t batchSize = kDefaultCacheBatchPages);
  void backgroundCacheImpl(ContentParser& parser, const std::string& cachePath, const RenderConfig& config,
                           int sectionPageHint, const AbortCallback& shouldAbort, bool forceExtendPartial = false);

  bool shouldContinueIdleBackgroundCaching(Core& core);
  BackgroundCachePlan planBackgroundCacheWork(Core& core);
  bool prefetchNextEpubSpineCache(Core& core, const RenderConfig& config, int activeSpineIndex, bool coverExists,
                                  int textStartIndex, bool allowFarSweep, const AbortCallback& shouldAbort);

  static void saveAnchorMap(const ContentParser& parser, const std::string& cachePath);
  static int loadAnchorPage(const std::string& cachePath, const std::string& anchor);
  static std::vector<std::pair<std::string, uint16_t>> loadAnchorMap(const std::string& cachePath);
  const std::vector<std::pair<std::string, uint16_t>>& getCachedAnchorMap(const std::string& cachePath, int spineIndex);
  void invalidateAnchorMapCache();
  static int calcFirstContentSpine(bool hasCover, int textStartIndex, size_t spineCount);

  ReaderDocumentResources& resources() { return resources_; }
  const ReaderDocumentResources& resources() const { return resources_; }
  ReaderDocumentResources::State& resourceState() { return resources_.unsafeState(); }
  const ReaderDocumentResources::State& resourceState() const { return resources_.unsafeState(); }
  WarmPageSlot& warmedNextPageRef() { return warmedNextPage_; }
  WarmPageSlot& warmedNextNextPageRef() { return warmedNextNextPage_; }
  WarmPageSlot& renderOverridePageRef() { return renderOverridePage_; }

  bool withForegroundResources(const char* reason, const std::function<void(ReaderDocumentResources::Session&)>& fn);
  bool withWorkerResources(const char* reason, const std::function<void(ReaderDocumentResources::Session&)>& fn);

 private:
  uint16_t nonEpubBatchSizeFor(ContentType type, const ContentParser& parser) const;

  PositionRefs position_;
  ReaderDocumentResources resources_;
  std::string contentPath_;

  bool thumbnailDone_ = false;
  uint32_t lastIdleBackgroundKickMs_ = 0;
  uint32_t lastReaderInteractionMs_ = 0;
  bool backgroundNearPrefetchComplete_ = false;
  int backgroundPrefetchBlockedSpine_ = -1;
  size_t backgroundPrefetchBlockedFreeBytes_ = 0;
  size_t backgroundPrefetchBlockedLargestBlock_ = 0;
  uint32_t backgroundPrefetchBlockedMs_ = 0;
  int backgroundPrefetchCursorSpine_ = -1;
  bool backgroundPrefetchSweepComplete_ = false;

  std::vector<std::pair<std::string, uint16_t>> cachedAnchorMap_;
  std::string cachedAnchorMapPath_;
  int cachedAnchorMapSpine_ = -1;

  WarmPageSlot warmedNextPage_;
  WarmPageSlot warmedNextNextPage_;
  WarmPageSlot renderOverridePage_;
};

}  // namespace snapix::reader
