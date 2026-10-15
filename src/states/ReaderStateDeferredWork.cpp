// v3.11.12 — work moved off the UI loop.
//
// Each of these used to run synchronously inside a UI-loop step and stalled
// it for seconds on device:
//   * the reader cover BMP (ZIP extract + JPEG decode, ~5 s on a first EPUB
//     open) → ReaderAsync; the cover page shows a title placeholder meanwhile;
//   * the global page metrics batch pass and every metrics persist (1.6 s in
//     the first FB2 page render) → ReaderAsync; the status bar shows the local
//     fallback meanwhile;
//   * the XTC cover/thumbnail (a 96 KB page buffer that rarely fits, attempted
//     right after the first page) → bounded strips between frames on the UI
//     loop (the XTC parser's SD handle belongs to the UI task).
// See reader::DeferredJobSlot for the hand-off protocol.

#include <FS.h>
#include <JpegToBmpConverter.h>
#include <LittleFS.h>
#include <Logging.h>
#include <SDCardManager.h>
#include <UnifiedCache.h>
#include <XtcCoverHelper.h>

#include <algorithm>
#include <new>

#include "../core/Core.h"
#include "ReaderState.h"
#include "ThemeManager.h"
#include "reader/ReaderStateInternal.h"
#include "reader/ReaderSupport.h"

#define TAG "READER"

namespace snapix {

namespace {
using SlotState = reader::DeferredJobSlot::State;
using AuxKind = reader::ReaderAsyncJobsController::AuxJobKind;

// The strip buffer of one XTC cover step.  16 KB = 264 rows of a 480-px page:
// a 480×800 XTCH cover takes 4 steps of one ~96 KB page stream each.
constexpr size_t kXtcCoverStripBytes = 16 * 1024;
// XTC cover strips run only after the reader has been idle this long, so they
// never sit between a page-turn press and its frame.
constexpr uint32_t kXtcCoverIdleMs = 1000;
// Minimum spacing of deferred-job hand-offs from pollDeferredJobs().
constexpr uint32_t kDeferredKickIntervalMs = 400;

const char* slotOutcomeName(const SlotState s) {
  switch (s) {
    case SlotState::Done:
      return "ok";
    case SlotState::Failed:
      return "failed";
    default:
      return "retry";
  }
}
}  // namespace

void ReaderState::resetDeferredJobs() {
  // Only with the worker stopped: no job can be in flight.
  coverJob_.reset();
  coverPlaceholderShown_ = false;
  coverGenerationFailed_ = false;
  metricsJob_.reset();
  metricsJobIo_ = GlobalMetricsJobIo{};
  metricsDiskProbed_ = false;
  metricsComputeFailed_ = false;
  metricsSaveDirty_ = false;
  metricsBookCachePath_.clear();
  metricsConfigHash_ = 0;
  lastDeferredKickMs_ = 0;
  fb2FallbackOffsetSpine_ = -1;
  fb2FallbackOffset_ = 0;
  fb2SectionMemoSpine_ = -1;
  fb2SectionMemoIndex_ = -1;
  xtcGrayPassDue_ = false;
  xtcRenderer_.cancelPendingGray();
  cancelXtcCoverJob();
  tocViewPopulated_ = false;
}

void ReaderState::queueDeferredJobs(Core& core) {
  auto send = [&](reader::DeferredJobSlot& slot, const AuxKind kind) {
    // Mark BEFORE sending: the worker's begin() expects Queued.
    if (!slot.markQueued()) return;
    reader::ReaderAsyncJobsController::AuxRequest request;
    request.kind = kind;
    if (!asyncJobs_.queueAuxWork(request)) slot.unmarkQueued();
  };

  // The cover first: it is what the reader is looking at.  Off the cover page
  // it waits — the next visit to the cover page asks again.
  if (coverJob_.state() == SlotState::Pending && onCoverPage()) {
    send(coverJob_, AuxKind::CoverImage);
  }

  // Metrics: a compute request, or a persist of the latest snapshot.  The I/O
  // block may be rewritten while the slot is Idle or Pending (nobody else
  // reads it then).
  if (metricsSaveDirty_ && globalSectionPageMetricsInitialized_ && !metricsBookCachePath_.empty()) {
    const SlotState st = metricsJob_.state();
    if (st == SlotState::Idle || (st == SlotState::Pending && !metricsJobIo_.computeEstimates)) {
      metricsJobIo_.computeEstimates = false;
      metricsJobIo_.type = core.content.metadata().type;
      metricsJobIo_.bookCachePath = metricsBookCachePath_;
      metricsJobIo_.configHash = metricsConfigHash_;
      metricsJobIo_.spineCount = static_cast<int>(globalSectionPageMetrics_.size());
      metricsJobIo_.metrics = globalSectionPageMetrics_;
      metricsJobIo_.source = "";
      metricsJobIo_.persisted = false;
      if (st == SlotState::Idle) metricsJob_.request();
      metricsSaveDirty_ = false;
    }
  }
  if (metricsJob_.state() == SlotState::Pending) {
    send(metricsJob_, AuxKind::GlobalMetrics);
  }
}

void ReaderState::pollDeferredJobs(Core& core) {
  const bool workerBusy = isWorkerRunning();
  // A worker cancel (page turn, overlay) drops queued commands that never ran.
  coverJob_.recoverDropped(workerBusy);
  metricsJob_.recoverDropped(workerBusy);

  switch (coverJob_.consume()) {
    case SlotState::Done:
      LOG_INF(TAG, "[COVER] cover BMP ready (placeholder shown=%u)", static_cast<unsigned>(coverPlaceholderShown_));
      if (onCoverPage() && coverPlaceholderShown_) {
        needsRender_ = true;
        // The chapter cache waited behind the cover; start it after the frame.
        resumeBackgroundCachingAfterRender_ = true;
      }
      break;
    case SlotState::Failed:
      coverGenerationFailed_ = true;
      LOG_INF(TAG, "[COVER] no usable cover image%s", onCoverPage() ? " — leaving the cover page" : "");
      if (onCoverPage()) leaveCoverPage(core);
      break;
    default:
      break;
  }

  const SlotState metricsState = metricsJob_.state();
  if (metricsState == SlotState::Done || metricsState == SlotState::Failed) {
    const bool computed = metricsJobIo_.computeEstimates;
    (void)metricsJob_.consume();
    if (computed && metricsState == SlotState::Done && !globalSectionPageMetricsInitialized_ &&
        static_cast<int>(metricsJobIo_.metrics.size()) == metricsJobIo_.spineCount &&
        metricsJobIo_.configHash == metricsConfigHash_) {
      globalSectionPageMetrics_ = std::move(metricsJobIo_.metrics);
      metricsJobIo_.metrics.clear();
      globalSectionPageMetricsInitialized_ = true;
      recomputeGlobalPageMetricTotal();
      if (!metricsJobIo_.persisted) scheduleGlobalMetricsSave();
      // The next page render merges the current section (and persists it
      // once it is exact).  No repaint just for the status bar.
      LOG_INF(TAG, "[METRICS] global estimates ready source=%s sections=%u total=%u persisted=%u",
              metricsJobIo_.source, static_cast<unsigned>(globalSectionPageMetrics_.size()),
              static_cast<unsigned>(globalSectionPageMetricTotal_),
              static_cast<unsigned>(metricsJobIo_.persisted));
    } else if (computed && metricsState == SlotState::Failed) {
      metricsComputeFailed_ = true;  // fallback status bar for this session
    }
  }

  // Hand pending work to an idle worker now rather than on the next 1.5 s idle
  // kick (startBackgroundCaching() also queues it, ahead of the cache job).
  // Throttled: a job aborted at once (low heap) must not be re-sent every loop.
  const bool deferredWorkPending = (coverJob_.state() == SlotState::Pending && onCoverPage()) ||
                                   metricsJob_.state() == SlotState::Pending ||
                                   (metricsSaveDirty_ && globalSectionPageMetricsInitialized_);
  const uint32_t nowMs = millis();
  if (deferredWorkPending && !workerBusy && !needsRender_ && !menuMode_ && !tocMode_ && !bookmarkMode_ &&
      !pendingTocJumpActive_ && !pendingEpubPageLoadActive_ &&
      static_cast<uint32_t>(nowMs - lastDeferredKickMs_) >= kDeferredKickIntervalMs) {
    lastDeferredKickMs_ = nowMs;
    queueDeferredJobs(core);
  }
}

void ReaderState::flushGlobalMetricsSaveNow(Core& core) {
  (void)core;
  // Book exit, worker stopped: persist what the session learned.  A save still
  // waiting in the slot carries an older snapshot of the same vector.
  const SlotState st = metricsJob_.state();
  const bool savePending = (st == SlotState::Pending || st == SlotState::Queued) && !metricsJobIo_.computeEstimates;
  if (!(metricsSaveDirty_ || savePending) || !globalSectionPageMetricsInitialized_ ||
      metricsBookCachePath_.empty() || globalSectionPageMetrics_.empty()) {
    return;
  }
  const uint32_t startedMs = reader::perfMsNow();
  auto cache = snapix::unifiedcache::UnifiedCache::shared(metricsBookCachePath_);
  const bool ok = writeGlobalMetricsFrame(cache, globalSectionPageMetrics_, metricsConfigHash_);
  metricsSaveDirty_ = false;
  reader::perfLog(TAG, "global-metrics-save-exit", startedMs, "(ok=%u sections=%u)", static_cast<unsigned>(ok),
                  static_cast<unsigned>(globalSectionPageMetrics_.size()));
}

// ----------------------------------------------------------------------------
// ReaderAsync side
// ----------------------------------------------------------------------------

void ReaderState::runAuxJob(const reader::ReaderAsyncJobsController::AuxRequest& request,
                            const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort) {
  switch (request.kind) {
    case AuxKind::CoverImage:
      runCoverJob(shouldAbort);
      break;
    case AuxKind::GlobalMetrics:
      runGlobalMetricsJob(shouldAbort);
      break;
    case AuxKind::None:
      break;
  }
}

void ReaderState::runCoverJob(const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort) {
  if (!coverJob_.begin()) return;  // cancelled / reset while queued
  Core* core = activeCore_;
  if (!core) {
    coverJob_.finish(SlotState::Pending);
    return;
  }
  const reader::HeapState heapBefore = reader::readHeapState();
  if (reader::isHeapCritical(heapBefore)) {
    // A decode that fails for want of heap would write the permanent
    // ".cover.failed" marker; wait for a better moment instead.
    coverJob_.finish(SlotState::Pending);
    return;
  }
  const uint32_t startedMs = reader::perfMsNow();
  std::string coverPath;
  // Under the worker resource token like every other document job, so the
  // foreground render never runs a parser/cache operation alongside it.
  const bool acquired = cacheController_.withWorkerResources(
      "cover-image", [&](auto&) { coverPath = core->content.generateCover(true, shouldAbort); });
  // Cover-gen via JPEGDEC pins ~25 KB (shared decoder) + the decode arena for
  // the rest of the session; release it before the chapter parser runs (see
  // v2.0.101 in the old synchronous renderCoverPage).  No-op with SmolJpeg.
  JpegToBmpConverter::releaseAllPersistent();

  SlotState outcome = SlotState::Done;
  if (!acquired || (coverPath.empty() && shouldAbort && shouldAbort())) {
    outcome = SlotState::Pending;  // pre-empted: retried on the next cover-page visit
  } else if (coverPath.empty()) {
    outcome = SlotState::Failed;
  }
  const reader::HeapState heapAfter = reader::readHeapState();
  reader::perfLog(TAG, "cover-generate-worker", startedMs, "(result=%s heap=%u->%u largest=%u->%u)",
                  slotOutcomeName(outcome), static_cast<unsigned>(heapBefore.freeBytes),
                  static_cast<unsigned>(heapAfter.freeBytes), static_cast<unsigned>(heapBefore.largestBlock),
                  static_cast<unsigned>(heapAfter.largestBlock));
  coverJob_.finish(outcome);
}

void ReaderState::runGlobalMetricsJob(const reader::ReaderAsyncJobsController::AbortCallback& shouldAbort) {
  if (!metricsJob_.begin()) return;
  Core* core = activeCore_;
  if (!core) {
    metricsJob_.finish(SlotState::Pending);
    return;
  }
  GlobalMetricsJobIo& io = metricsJobIo_;
  const uint32_t startedMs = reader::perfMsNow();
  SlotState outcome = SlotState::Failed;
  const bool acquired = cacheController_.withWorkerResources("global-metrics", [&](auto&) {
    if (shouldAbort && shouldAbort()) {
      outcome = SlotState::Pending;
      return;
    }
    auto cache = snapix::unifiedcache::UnifiedCache::shared(io.bookCachePath);
    if (!io.computeEstimates) {
      io.persisted = writeGlobalMetricsFrame(cache, io.metrics, io.configHash);
      // Writer token busy for ~300 ms (markerize in flight): retry later.
      outcome = io.persisted ? SlotState::Done : SlotState::Pending;
      return;
    }

    std::vector<SectionPageMetric> metrics;
    // The UI looks the frame up once per session; a read that failed there
    // (e.g. the file changed under it) must not replace a good frame with
    // fresh estimates, so look again with this instance's own scan.
    {
      std::vector<uint8_t> payload;
      if (cache.readSegment(snapix::unifiedcache::Kind::Metrics, snapix::unifiedcache::kGlobalKey, payload) &&
          parseGlobalMetricsPayload(payload, io.configHash, io.spineCount, metrics)) {
        io.source = "cache";
        io.persisted = true;  // already on disk
        io.metrics = std::move(metrics);
        outcome = SlotState::Done;
        return;
      }
    }
    // Pre-v2.0.166 installs kept the frame in <book>/metrics.bin.
    const std::string legacyPath = reader::metricsCachePath(io.bookCachePath);
    if (LittleFS.exists(legacyPath.c_str())) {
      File f = LittleFS.open(legacyPath.c_str(), "r");
      if (f) {
        std::vector<uint8_t> payload(f.size());
        const int n = payload.empty() ? 0 : static_cast<int>(f.read(payload.data(), payload.size()));
        f.close();
        if (n == static_cast<int>(payload.size()) &&
            parseGlobalMetricsPayload(payload, io.configHash, io.spineCount, metrics)) {
          io.source = "legacy";
        }
      }
    }
    if (metrics.empty()) {
      if (!buildGlobalMetricEstimates(*core, io.type, io.spineCount, metrics)) {
        outcome = SlotState::Failed;
        return;
      }
      io.source = "batch";
    }
    if (shouldAbort && shouldAbort()) {
      outcome = SlotState::Pending;
      return;
    }
    io.persisted = writeGlobalMetricsFrame(cache, metrics, io.configHash);
    io.metrics = std::move(metrics);
    outcome = SlotState::Done;
  });
  if (!acquired) outcome = SlotState::Pending;
  reader::perfLog(TAG, "global-metrics-worker", startedMs, "(mode=%s source=%s sections=%d result=%s persisted=%u)",
                  io.computeEstimates ? "compute" : "persist", io.source, io.spineCount, slotOutcomeName(outcome),
                  static_cast<unsigned>(io.persisted));
  metricsJob_.finish(outcome);
}

// ----------------------------------------------------------------------------
// Cover page
// ----------------------------------------------------------------------------

void ReaderState::leaveCoverPage(Core& core) {
  // Same landing as renderCurrentPage's "no cover" fall-through.
  hasCover_ = false;
  coverPlaceholderShown_ = false;
  currentSectionPage_ = 0;
  if (textStartIndex_ == 0) {
    auto* provider = core.content.asEpub();
    if (provider && provider->getEpub() && provider->getEpub()->getSpineItemsCount() > 1) {
      currentSpineIndex_ = 1;
    }
  }
  needsRender_ = true;
}

void ReaderState::renderCoverPlaceholder(Core& core) {
  // A plain title page in the cover's place: a double rule, the title, the
  // author.  The reader font when one is installed (Cyrillic/CJK titles), the
  // UI font otherwise — the same rule as the TOC overlay.
  const Theme& theme = THEME_MANAGER.current();
  const auto vp = getReaderViewport(core.settings.statusBar != 0);
  const int fontId =
      core.settings.hasExternalReaderFont(theme) ? core.settings.getReaderFontId(theme) : theme.uiFontId;
  const bool ink = theme.primaryTextBlack;
  const int x = vp.marginLeft;
  const int y = vp.marginTop;
  const int w = vp.width;
  const int h = vp.height;
  if (w > 16 && h > 16) {
    renderer_.drawRect(x, y, w, h, ink);
    renderer_.drawRect(x + 4, y + 4, w - 8, h - 8, ink);
  }

  constexpr int kTextPad = 28;
  const int textWidth = std::max(1, w - 2 * kTextPad);
  const int lineHeight = std::max(1, renderer_.getLineHeight(fontId));
  const char* title = core.content.metadata().title;
  const char* author = core.content.metadata().author;
  const bool hasAuthor = author && author[0] != '\0';
  const auto titleLines =
      renderer_.wrapTextWithHyphenation(fontId, title ? title : "", textWidth, 5, EpdFontFamily::BOLD);
  const int blockHeight = static_cast<int>(titleLines.size()) * lineHeight + (hasAuthor ? 2 * lineHeight : 0);
  int textY = y + std::max(0, (h - blockHeight) / 2 - h / 10);
  for (const auto& line : titleLines) {
    const int lineWidth = renderer_.getTextWidth(fontId, line.c_str(), EpdFontFamily::BOLD);
    renderer_.drawText(fontId, x + (w - lineWidth) / 2, textY, line.c_str(), ink, EpdFontFamily::BOLD);
    textY += lineHeight;
  }
  if (hasAuthor) {
    textY += lineHeight;
    const std::string shown = renderer_.truncatedText(fontId, author, textWidth);
    const int authorWidth = renderer_.getTextWidth(fontId, shown.c_str());
    renderer_.drawText(fontId, x + (w - authorWidth) / 2, textY, shown.c_str(), ink);
  }

  // Same refresh choice as CoverHelpers::renderCoverFromBmp.
  const bool turnOffScreen = core.settings.sunlightFadingFix != 0;
  const int pagesPerRefresh = core.settings.getPagesPerRefreshValue();
  if (pagesPerRefresh == 0 || pagesUntilFullRefresh_ > 1) {
    renderer_.displayBuffer(EInkDisplay::FAST_REFRESH, turnOffScreen);
  } else {
    renderer_.displayBuffer(EInkDisplay::HALF_REFRESH, turnOffScreen);
  }
}

// ----------------------------------------------------------------------------
// XTC cover + thumbnail (UI loop, bounded strips)
// ----------------------------------------------------------------------------

void ReaderState::beginXtcCoverJob(Core& core) {
  cancelXtcCoverJob();
  auto* provider = core.content.asXtc();
  const char* cacheDir = core.content.cacheDir();
  if (!provider || !cacheDir) return;
  const std::string coverPath = provider->getCoverBmpPath();
  const std::string thumbPath = provider->getThumbBmpPath();
  const std::string thumbFailedPath = std::string(cacheDir) + "/.thumb.failed";
  const bool coverExists = LittleFS.exists(coverPath.c_str());
  if (coverExists && (LittleFS.exists(thumbPath.c_str()) || LittleFS.exists(thumbFailedPath.c_str()))) {
    return;  // both done (or the thumbnail is known to be impossible)
  }
  if (coverExists) {
    xtcCoverPhase_ = XtcCoverPhase::Thumb;
    return;
  }
  if (!xtcCoverWriter_) xtcCoverWriter_.reset(new (std::nothrow) xtc::CoverBmpWriter());
  if (!xtcCoverWriter_) return;
  const uint32_t startedMs = reader::perfMsNow();
  if (!xtcCoverWriter_->begin(provider->getParser(), coverPath)) {
    xtcCoverWriter_.reset();
    return;
  }
  xtcCoverPhase_ = XtcCoverPhase::Cover;
  reader::perfLog(TAG, "xtc-cover-begin", startedMs, "(height=%u)", static_cast<unsigned>(xtcCoverWriter_->height()));
}

void ReaderState::runXtcCoverSlice(Core& core) {
  auto* provider = core.content.asXtc();
  if (!provider || xtcCoverPhase_ == XtcCoverPhase::Idle) {
    cancelXtcCoverJob();
    return;
  }
  if (static_cast<uint32_t>(millis() - lastReaderInteractionMs_) < kXtcCoverIdleMs) return;

  const uint32_t startedMs = reader::perfMsNow();
  if (xtcCoverPhase_ == XtcCoverPhase::Cover) {
    if (!xtcCoverWriter_) {
      xtcCoverPhase_ = XtcCoverPhase::Idle;
      return;
    }
    const auto status = xtcCoverWriter_->step(provider->getParser(), kXtcCoverStripBytes);
    reader::perfLog(TAG, "xtc-cover-strip", startedMs, "(rows=%u/%u status=%u)",
                    static_cast<unsigned>(xtcCoverWriter_->rowsWritten()),
                    static_cast<unsigned>(xtcCoverWriter_->height()), static_cast<unsigned>(status));
    if (status == xtc::CoverBmpWriter::Status::Done) {
      xtcCoverWriter_.reset();
      xtcCoverPhase_ = XtcCoverPhase::Thumb;
    } else if (status == xtc::CoverBmpWriter::Status::Failed) {
      // Transient (heap / I/O): no failure marker, the next session retries.
      xtcCoverWriter_.reset();
      xtcCoverPhase_ = XtcCoverPhase::Idle;
    }
    return;
  }

  // Thumbnail: scales the cover BMP (cover exists, so no page decode here).
  const bool ok = provider->generateThumbBmp();
  reader::perfLog(TAG, "xtc-thumb", startedMs, "(ok=%u)", static_cast<unsigned>(ok));
  xtcCoverPhase_ = XtcCoverPhase::Idle;
}

void ReaderState::cancelXtcCoverJob() {
  if (xtcCoverWriter_) {
    xtcCoverWriter_->cancel();
    xtcCoverWriter_.reset();
  }
  xtcCoverPhase_ = XtcCoverPhase::Idle;
}

}  // namespace snapix
