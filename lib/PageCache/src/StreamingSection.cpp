#include "StreamingSection.h"

#include <FS.h>
#include <Logging.h>
#include <UnifiedCache.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#define TAG "STREAM_SEC"

#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER
#include <ChunkedMarkersReader.h>
#include <GfxRenderer.h>
#include <GfxRendererPaginatorAdapter.h>
#include <MarkerizedPageRender.h>
#include <StreamingPaginator.h>

#include "UnifiedCacheChunkProvider.h"
#endif

namespace snapix::pagecache {

#if defined(SNAPIX_MARKERIZER) && SNAPIX_MARKERIZER

namespace {

// Build the paginator config for a single-section streamed doc.  MUST match the
// cfg ReaderState::renderPageContents builds for TXT/MD (same fields, same
// helpers) so computePageIndexConfigHash() agrees between MEASURE and DRAW.
snapix::smolport::StreamingPaginatorConfig makeSectionConfig(GfxRenderer& renderer,
                                                            const RenderConfig& config, int mT,
                                                            int mB, int mL, int mR,
                                                            const char* hyphenLang) {
  const uint16_t bodyLineH =
      static_cast<uint16_t>(renderer.getLineHeight(config.fontId) * config.lineCompression);
  snapix::smolport::StreamingPaginatorConfig cfg{};
  cfg.pageWidth = static_cast<uint16_t>(renderer.getScreenWidth());
  cfg.pageHeight = static_cast<uint16_t>(renderer.getScreenHeight());
  cfg.marginTop = static_cast<uint16_t>(mT);
  cfg.marginBottom = static_cast<uint16_t>(mB);
  cfg.marginLeft = static_cast<uint16_t>(mL);
  cfg.marginRight = static_cast<uint16_t>(mR);
  cfg.bodyLineHeight = bodyLineH > 0 ? bodyLineH : 24;
  cfg.headingLineHeight = static_cast<uint16_t>(cfg.bodyLineHeight * 3 / 2);
  cfg.paragraphSpacing =
      snapix::smolport::paragraphSpacingForLevel(config.spacingLevel, cfg.bodyLineHeight);
  cfg.firstLineIndent =
      snapix::smolport::firstLineIndentForLevel(config.indentLevel, cfg.bodyLineHeight);
  cfg.hyphenate = (hyphenLang != nullptr && hyphenLang[0] != '\0');
  std::strncpy(cfg.hyphenLang, hyphenLang ? hyphenLang : "", sizeof(cfg.hyphenLang) - 1);
  cfg.hyphenLang[sizeof(cfg.hyphenLang) - 1] = '\0';
  return cfg;
}

// Read the idx header (12 bytes) for `key`.  Returns true and fills outVersion /
// outConfigHash / outBoundaryCount if a segment exists and is >= 12 bytes.
bool readIdxHeader(snapix::unifiedcache::UnifiedCache& cache, uint16_t key, uint16_t& outVersion,
                   uint16_t& outConfigHash, uint16_t& outBoundaryCount) {
  File idxF;
  size_t idxSize = 0;
  if (!cache.openSegmentReader(snapix::unifiedcache::Kind::Idx, key, idxF, &idxSize)) return false;
  uint8_t h[12];
  bool ok = false;
  if (idxSize >= sizeof(h) && idxF.read(h, sizeof(h)) == static_cast<int>(sizeof(h))) {
    const bool magicOk = h[0] == 0x53 && h[1] == 0x50 && h[2] == 0x49 && h[3] == 0x58;  // "SPIX"
    if (magicOk) {
      outVersion = static_cast<uint16_t>(h[4]) | (static_cast<uint16_t>(h[5]) << 8);
      outConfigHash = static_cast<uint16_t>(h[6]) | (static_cast<uint16_t>(h[7]) << 8);
      outBoundaryCount = static_cast<uint16_t>(h[8]) | (static_cast<uint16_t>(h[9]) << 8);
      ok = true;
    }
  }
  idxF.close();
  return ok;
}

}  // namespace

uint16_t ensureStreamingSectionIdx(const std::string& bookCachePath, uint16_t sectionKey,
                                   GfxRenderer& renderer, const RenderConfig& config, int mT, int mB,
                                   int mL, int mR, const char* hyphenLang) {
  auto cache = snapix::unifiedcache::UnifiedCache::shared(bookCachePath);

  size_t markersSize = 0;
  if (!cache.segmentSize(snapix::unifiedcache::Kind::Markers, sectionKey, &markersSize) ||
      markersSize == 0) {
    return 0;  // markerize hasn't run / failed
  }

  const snapix::smolport::StreamingPaginatorConfig cfg =
      makeSectionConfig(renderer, config, mT, mB, mL, mR, hyphenLang);
  const uint16_t configHash =
      snapix::smolport::computePageIndexConfigHash(cfg, config.fontId, config.fakeBold);

  // Fast path: a current (version + configHash) idx already exists — return its
  // count without re-walking.  (A stale idx is rebuilt below; the append-log
  // UnifiedCache keeps the newest frame, so the write supersedes it.)
  {
    uint16_t ver = 0, hash = 0, bcount = 0;
    if (readIdxHeader(cache, sectionKey, ver, hash, bcount) &&
        ver == snapix::smolport::kPageIndexVersion && hash == configHash) {
      return static_cast<uint16_t>(bcount + 1);
    }
  }

  // Build the idx via a MEASURE-only walk over the Markers segment.
  File mf;
  size_t markersStreamSize = 0;
  if (!cache.openSegmentReader(snapix::unifiedcache::Kind::Markers, sectionKey, mf,
                               &markersStreamSize)) {
    return 0;
  }

  // TXT/MD carry no inline images — the resolver always returns empty.
  auto resolveImage = [](const uint8_t*, size_t) -> std::string { return {}; };
  snapix::smolport::GfxRendererPaginatorAdapter adapter(renderer, config.fontId, config.fontId,
                                                        /*black=*/true, resolveImage, config.fakeBold,
                                                        config.superSubFontId);
  snapix::smolport::StreamingPaginator paginator(cfg, adapter);

  constexpr size_t kChunkBufBytes = 4096;
  uint8_t chunkBuf[kChunkBufBytes];
  size_t markersRemaining = markersStreamSize;
  auto readFn = [&mf, &markersRemaining](uint8_t* buf, size_t bufSize) -> int {
    if (!mf || markersRemaining == 0) return markersRemaining == 0 ? 0 : -1;
    const size_t want = std::min(bufSize, markersRemaining);
    const int got = mf.read(buf, want);
    if (got > 0) markersRemaining -= static_cast<size_t>(got);
    return got;
  };

  std::vector<snapix::smolport::PageBoundarySnapshot> captured;
  auto captureFn = [&captured](const snapix::smolport::PageBoundarySnapshot& s) {
    captured.push_back(s);
  };

  snapix::smolport::MarkerizedRenderStats stats{};
  (void)snapix::smolport::renderMarkerizedPage(paginator, readFn, chunkBuf, sizeof(chunkBuf),
                                               UINT16_MAX, {}, &stats, {}, captureFn);
  mf.close();

  if (captured.empty() && stats.bytesConsumed == 0) {
    LOG_ERR(TAG, "[STREAM] idx build read 0 bytes key=%u (markers empty/corrupt)",
            static_cast<unsigned>(sectionKey));
    return 0;
  }

  const uint16_t totalPages = static_cast<uint16_t>(captured.size() + 1);

  // A single TXT/MD section can run to thousands of pages (e.g. a 1.8 MB novel),
  // so the idx may exceed the 4 KB EPUB/FB2 stack buffer.  Persisting the FULL
  // idx is REQUIRED for correct navigation: if it isn't persisted, the render
  // path cold-walks and saves only a partial (prefix) idx on exit, which a later
  // open then mistakes for the complete page count — pagination gets stuck a few
  // pages in.  Allocate exactly what's needed on the heap (nothrow → degrade,
  // never reboot).
  const size_t needed =
      snapix::smolport::kPageIndexHeaderBytes + captured.size() * snapix::smolport::kPageIndexEntryBytes;
  std::unique_ptr<uint8_t[]> serdebuf(new (std::nothrow) uint8_t[needed]);
  if (!serdebuf) {
    LOG_ERR(TAG, "[STREAM] idx alloc failed key=%u bytes=%zu pages=%u",
            static_cast<unsigned>(sectionKey), needed, static_cast<unsigned>(totalPages));
    return totalPages;  // count is known; render falls back to cold-walk
  }
  const size_t wrote = snapix::smolport::serializePageIndex(captured.data(), captured.size(),
                                                            configHash, serdebuf.get(), needed);
  if (wrote == 0) return totalPages;

  if (!cache.writeSegment(snapix::unifiedcache::Kind::Idx, sectionKey, serdebuf.get(), wrote)) {
    LOG_ERR(TAG, "[STREAM] idx write failed key=%u", static_cast<unsigned>(sectionKey));
  } else {
    LOG_INF(TAG, "[STREAM] idx built key=%u pages=%u bytes=%zu", static_cast<unsigned>(sectionKey),
            static_cast<unsigned>(totalPages), wrote);
  }
  return totalPages;
}

uint16_t extendChunkedSectionIdx(ChunkedIdxState& st, const std::string& bookCachePath,
                                 bool sourceExhausted, GfxRenderer& renderer,
                                 const RenderConfig& config, int mT, int mB, int mL, int mR,
                                 const char* hyphenLang) {
  auto cache = snapix::unifiedcache::UnifiedCache::shared(bookCachePath);

  const snapix::smolport::StreamingPaginatorConfig cfg =
      makeSectionConfig(renderer, config, mT, mB, mL, mR, hyphenLang);
  st.configHash = snapix::smolport::computePageIndexConfigHash(cfg, config.fontId, config.fakeBold);
  st.configHashValid = true;

  // Present Markers keys 0..N as one logical stream so the resume walk spans the
  // chunk boundaries.  At least chunk 0 must exist by the time we're called.
  auto provider = UnifiedCacheChunkProvider::singleSection(cache);
  snapix::smolport::ChunkedMarkersReader reader(provider);
  if (reader.chunkCount() <= 0 || reader.totalSize() == 0) {
    LOG_ERR(TAG, "[STREAM] extend: no marker chunks present");
    return 0;
  }

  // Resume from the last (still-incomplete) page boundary so we only measure the
  // pages that the newest chunk made available.  A fresh idx (no boundaries yet)
  // walks from page 0 / offset 0.
  snapix::smolport::MarkerizedRenderResume resume{};
  if (!st.boundaries.empty()) {
    const auto& last = st.boundaries.back();
    resume.startPage = last.pageIndex;
    resume.byteOffset = last.byteOffset;
    resume.styleBits = last.styleBits;
    resume.atParagraphStart = last.atParagraphStart;
    if (!reader.seekGlobal(last.byteOffset)) {
      LOG_ERR(TAG, "[STREAM] extend: seek to resume offset %u failed",
              static_cast<unsigned>(last.byteOffset));
      return 0;
    }
  }

  auto resolveImage = [](const uint8_t*, size_t) -> std::string { return {}; };
  snapix::smolport::GfxRendererPaginatorAdapter adapter(renderer, config.fontId, config.fontId,
                                                        /*black=*/true, resolveImage, config.fakeBold,
                                                        config.superSubFontId);
  snapix::smolport::StreamingPaginator paginator(cfg, adapter);

  constexpr size_t kChunkBufBytes = 4096;
  uint8_t chunkBuf[kChunkBufBytes];
  auto readFn = [&reader](uint8_t* buf, size_t bufSize) -> int { return reader.read(buf, bufSize); };

  // Capture only the boundaries this walk discovers (pages startPage+1 onward);
  // appended to st.boundaries below — never duplicates the resume boundary.
  std::vector<snapix::smolport::PageBoundarySnapshot> fresh;
  auto captureFn = [&fresh](const snapix::smolport::PageBoundarySnapshot& s) { fresh.push_back(s); };

  snapix::smolport::MarkerizedRenderStats stats{};
  (void)snapix::smolport::renderMarkerizedPage(paginator, readFn, chunkBuf, sizeof(chunkBuf),
                                               UINT16_MAX, {}, &stats, resume, captureFn);

  for (const auto& b : fresh) st.boundaries.push_back(b);

  const uint16_t totalPages =
      static_cast<uint16_t>(st.boundaries.size() + (sourceExhausted ? 1 : 0));

  // Persist the full idx (heap-sized, nothrow → degrade not reboot).  A later
  // open re-uses it; if the write fails the count is still returned and the
  // render path cold-walks.
  const size_t needed = snapix::smolport::kPageIndexHeaderBytes +
                        st.boundaries.size() * snapix::smolport::kPageIndexEntryBytes;
  std::unique_ptr<uint8_t[]> serdebuf(new (std::nothrow) uint8_t[needed]);
  if (!serdebuf) {
    LOG_ERR(TAG, "[STREAM] extend idx alloc failed bytes=%zu pages=%u", needed,
            static_cast<unsigned>(totalPages));
    return totalPages;
  }
  const size_t wrote = snapix::smolport::serializePageIndex(st.boundaries.data(), st.boundaries.size(),
                                                            st.configHash, serdebuf.get(), needed);
  if (wrote > 0 && !cache.writeSegment(snapix::unifiedcache::Kind::Idx, 0, serdebuf.get(), wrote)) {
    LOG_ERR(TAG, "[STREAM] extend idx write failed pages=%u", static_cast<unsigned>(totalPages));
  } else {
    LOG_INF(TAG, "[STREAM] idx extend pages=%u (+%u) exhausted=%u", static_cast<unsigned>(totalPages),
            static_cast<unsigned>(fresh.size()), static_cast<unsigned>(sourceExhausted));
  }
  return totalPages;
}

uint16_t loadChunkedSectionIdx(ChunkedIdxState& st, const std::string& bookCachePath,
                               GfxRenderer& renderer, const RenderConfig& config, int mT, int mB,
                               int mL, int mR, const char* hyphenLang) {
  auto cache = snapix::unifiedcache::UnifiedCache::shared(bookCachePath);
  const snapix::smolport::StreamingPaginatorConfig cfg =
      makeSectionConfig(renderer, config, mT, mB, mL, mR, hyphenLang);
  const uint16_t hash = snapix::smolport::computePageIndexConfigHash(cfg, config.fontId, config.fakeBold);

  std::vector<uint8_t> bytes;
  if (!cache.readSegment(snapix::unifiedcache::Kind::Idx, 0, bytes) || bytes.empty()) return 0;
  if (!snapix::smolport::deserializePageIndex(bytes.data(), bytes.size(), hash, st.boundaries)) {
    st.boundaries.clear();
    return 0;  // stale / wrong version / wrong config → caller re-measures
  }
  st.configHash = hash;
  st.configHashValid = true;
  return static_cast<uint16_t>(st.boundaries.size());
}

#else  // !SNAPIX_MARKERIZER

uint16_t ensureStreamingSectionIdx(const std::string&, uint16_t, GfxRenderer&, const RenderConfig&,
                                   int, int, int, int, const char*) {
  return 0;  // markerizer compiled out — TXT/MD produce no pages (build-test env)
}

uint16_t extendChunkedSectionIdx(ChunkedIdxState&, const std::string&, bool, GfxRenderer&,
                                 const RenderConfig&, int, int, int, int, const char*) {
  return 0;  // markerizer compiled out
}

uint16_t loadChunkedSectionIdx(ChunkedIdxState&, const std::string&, GfxRenderer&,
                               const RenderConfig&, int, int, int, int, const char*) {
  return 0;  // markerizer compiled out
}

#endif

}  // namespace snapix::pagecache
