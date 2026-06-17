#pragma once

// =============================================================================
// UnifiedCacheChunkProvider — MarkerChunkProvider backed by UnifiedCache Markers
// segments, for ChunkedMarkersReader (lazy/progressive markerize).
//
// Two modes:
//   * Single-section (TXT / MD / no-TOC FB2): the document's markers live in
//     consecutive Markers segments keyed 0,1,2,…  The provider probes upward
//     from key 0 until a segment is missing → that's the current chunk count
//     (grows as the background markerize writes more chunks).
//   * Section (EPUB / FB2-with-TOC): the section's markers are a single segment
//     keyed by sectionIndex → exactly one chunk.  Behaves like the old single-
//     segment reader.
// =============================================================================

#include <ChunkedMarkersReader.h>  // snapix::smolport::MarkerChunkProvider
#include <FS.h>
#include <UnifiedCache.h>

#include <cstdint>
#include <vector>

namespace snapix::pagecache {

class UnifiedCacheChunkProvider : public snapix::smolport::MarkerChunkProvider {
 public:
  // Single-section: probe Markers keys 0..N.  maxChunks caps the probe.
  static UnifiedCacheChunkProvider singleSection(snapix::unifiedcache::UnifiedCache& cache,
                                                 int maxChunks = 4096) {
    UnifiedCacheChunkProvider p(cache);
    for (int k = 0; k < maxChunks; ++k) {
      size_t sz = 0;
      if (!cache.segmentSize(snapix::unifiedcache::Kind::Markers, static_cast<uint16_t>(k), &sz)) break;
      p.keys_.push_back(static_cast<uint16_t>(k));
      p.sizes_.push_back(static_cast<uint32_t>(sz));
    }
    return p;
  }

  // Section: a single chunk = Markers[sectionKey].
  static UnifiedCacheChunkProvider section(snapix::unifiedcache::UnifiedCache& cache, uint16_t sectionKey) {
    UnifiedCacheChunkProvider p(cache);
    size_t sz = 0;
    if (cache.segmentSize(snapix::unifiedcache::Kind::Markers, sectionKey, &sz)) {
      p.keys_.push_back(sectionKey);
      p.sizes_.push_back(static_cast<uint32_t>(sz));
    }
    return p;
  }

  int count() const override { return static_cast<int>(keys_.size()); }
  uint32_t size(int chunk) const override { return sizes_[static_cast<size_t>(chunk)]; }

  bool open(int chunk) override {
    if (chunk < 0 || chunk >= count()) return false;
    if (curOpen_) {
      curFile_.close();
      curOpen_ = false;
    }
    size_t segSize = 0;
    if (!cache_.openSegmentReader(snapix::unifiedcache::Kind::Markers, keys_[static_cast<size_t>(chunk)],
                                  curFile_, &segSize)) {
      return false;
    }
    curBase_ = static_cast<uint32_t>(curFile_.position());  // payload start within streaming.cache
    curSize_ = static_cast<uint32_t>(segSize);
    curPos_ = 0;
    curOpen_ = true;
    return true;
  }

  int read(uint8_t* buf, size_t bufSize) override {
    if (!curOpen_) return -1;
    if (curPos_ >= curSize_) return 0;  // chunk EOF
    const size_t want = std::min(bufSize, static_cast<size_t>(curSize_ - curPos_));
    const int got = curFile_.read(buf, want);
    if (got > 0) curPos_ += static_cast<uint32_t>(got);
    return got;
  }

  bool seekLocal(uint32_t offset) override {
    if (!curOpen_ || offset > curSize_) return false;
    if (!curFile_.seek(curBase_ + offset)) return false;
    curPos_ = offset;
    return true;
  }

  ~UnifiedCacheChunkProvider() override {
    if (curOpen_) curFile_.close();
  }

  // Move-only (holds a File handle).
  UnifiedCacheChunkProvider(UnifiedCacheChunkProvider&&) = default;
  UnifiedCacheChunkProvider& operator=(UnifiedCacheChunkProvider&&) = delete;
  UnifiedCacheChunkProvider(const UnifiedCacheChunkProvider&) = delete;
  UnifiedCacheChunkProvider& operator=(const UnifiedCacheChunkProvider&) = delete;

 private:
  explicit UnifiedCacheChunkProvider(snapix::unifiedcache::UnifiedCache& cache) : cache_(cache) {}

  snapix::unifiedcache::UnifiedCache& cache_;
  std::vector<uint16_t> keys_;
  std::vector<uint32_t> sizes_;
  File curFile_;
  uint32_t curBase_ = 0;
  uint32_t curSize_ = 0;
  uint32_t curPos_ = 0;
  bool curOpen_ = false;
};

}  // namespace snapix::pagecache
