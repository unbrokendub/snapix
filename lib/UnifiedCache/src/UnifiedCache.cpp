#include "UnifiedCache.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <utility>

#define TAG "UCACHE"

namespace snapix::unifiedcache {

std::shared_ptr<UnifiedCache> UnifiedCache::shared(const std::string& bookCachePath) {
  // v2.0.173 — REVERTED v2.0.170/171/172 instance caching.  Each call now
  // creates a fresh instance whose lifetime is bound to the caller's
  // shared_ptr (function scope in practice).  When the caller's scope
  // ends, the instance destructs and ALL its state (directory_ vector,
  // path string, std::mutex's FreeRTOS semaphore handle, shared_ptr
  // control block) is freed.
  //
  // Why reverted: v2.0.170 added a process-wide registry pinning per-book
  // UnifiedCache instances + a per-instance std::mutex (~88 B FreeRTOS
  // queue handle).  v2.0.171 was a cosmetic rename.  v2.0.172 tried to
  // bound the registry to LRU-size-1.  Hardware testing showed both
  // v2.0.171 (largest free=31732) AND v2.0.172 (largest free=30708)
  // could not allocate the 32 KB InflateReader ring buffer for EPUB
  // chapter parses after an FB2 reading session in the same boot.  The
  // LRU-1 fix freed the *previous* book's UnifiedCache state but
  // *immediately* re-allocated similar state for the new book — net
  // heap delta was zero, fragmentation pattern was unchanged.
  //
  // v2.0.168 (the last known-good UnifiedCache version) had no registry
  // and no per-instance mutex.  Each `::shared()` call created a fully
  // independent instance, used it, and let it die at function scope.
  // This release restores that pattern.  The "7-10 redundant directory
  // scans per chapter render" that v2.0.170 set out to fix are back —
  // each scan reads ~71 KB from LittleFS and takes ~50 ms — but a 350-
  // 500 ms total scan overhead per chapter render is dramatically
  // preferable to EPUB books being completely unreadable after the
  // first FB2 read in a session.
  //
  // The v2.0.170 changelog also claimed a "latent concurrency bug" fix:
  // BG worker writes between a reader's create-and-scan and the
  // reader's actual read could leave the reader with a stale directory.
  // Pre-v2.0.170 (and now post-revert) this can't happen because
  // readers create their UnifiedCache instance INSIDE their read call,
  // scan once, then read immediately — there is no scan-then-later-read
  // gap to race against.
  //
  // Future re-optimization, if needed: pass a `UnifiedCache&` through
  // EpubChapterParser / Fb2Parser / ReaderState by reference instead
  // of looking it up via `::shared()` each time.  That gives within-
  // chapter-parse instance sharing (eliminates redundant scans for
  // sequential callers in the same operation) without any global
  // registry, without any mutex, and without keeping state alive
  // across operations.  Out of scope for this hotfix.
  return std::make_shared<UnifiedCache>(bookCachePath);
}

namespace {

constexpr uint8_t kMagic[4] = {'U', 'C', 'A', 'C'};

inline void writeU16LE(uint8_t* dst, uint16_t v) {
  dst[0] = static_cast<uint8_t>(v);
  dst[1] = static_cast<uint8_t>(v >> 8);
}

inline void writeU32LE(uint8_t* dst, uint32_t v) {
  dst[0] = static_cast<uint8_t>(v);
  dst[1] = static_cast<uint8_t>(v >> 8);
  dst[2] = static_cast<uint8_t>(v >> 16);
  dst[3] = static_cast<uint8_t>(v >> 24);
}

inline uint16_t readU16LE(const uint8_t* src) {
  return static_cast<uint16_t>(src[0]) | (static_cast<uint16_t>(src[1]) << 8);
}

inline uint32_t readU32LE(const uint8_t* src) {
  return static_cast<uint32_t>(src[0]) | (static_cast<uint32_t>(src[1]) << 8) |
         (static_cast<uint32_t>(src[2]) << 16) | (static_cast<uint32_t>(src[3]) << 24);
}

}  // namespace

UnifiedCache::UnifiedCache(std::string bookCachePath)
    : path_(std::move(bookCachePath) + "/streaming.cache") {}

bool UnifiedCache::writeFileHeader(File& file) {
  uint8_t hdr[kHeaderSize] = {};
  std::memcpy(hdr, kMagic, sizeof(kMagic));
  writeU16LE(hdr + 4, kFormatVersion);
  writeU16LE(hdr + 6, 0);  // flags
  // remaining 8 bytes are reserved/zero
  if (file.write(hdr, kHeaderSize) != kHeaderSize) {
    LOG_ERR(TAG, "Failed to write file header to %s", path_.c_str());
    return false;
  }
  return true;
}

bool UnifiedCache::loadDirectory() {
  directory_.clear();
  fileSize_ = 0;

  if (!LittleFS.exists(path_.c_str())) {
    // Fresh cache — header gets written on first appendFrame.
    headerInitialized_ = false;
    loaded_ = true;
    return true;
  }

  File f = LittleFS.open(path_.c_str(), "r");
  if (!f) {
    LOG_ERR(TAG, "Failed to open %s for read", path_.c_str());
    return false;
  }
  fileSize_ = f.size();

  if (fileSize_ < kHeaderSize) {
    LOG_ERR(TAG, "File %s too small (%zu bytes) — treating as empty", path_.c_str(), fileSize_);
    f.close();
    LittleFS.remove(path_.c_str());
    fileSize_ = 0;
    headerInitialized_ = false;
    loaded_ = true;
    return true;
  }

  uint8_t hdr[kHeaderSize];
  if (f.read(hdr, kHeaderSize) != static_cast<int>(kHeaderSize)) {
    LOG_ERR(TAG, "Failed to read file header from %s", path_.c_str());
    f.close();
    return false;
  }
  if (std::memcmp(hdr, kMagic, sizeof(kMagic)) != 0) {
    LOG_ERR(TAG, "Bad magic in %s — wiping", path_.c_str());
    f.close();
    LittleFS.remove(path_.c_str());
    fileSize_ = 0;
    headerInitialized_ = false;
    loaded_ = true;
    return true;
  }
  const uint16_t version = readU16LE(hdr + 4);
  if (version != kFormatVersion) {
    LOG_INF(TAG, "Format version mismatch in %s (found %u, want %u) — wiping", path_.c_str(),
            static_cast<unsigned>(version), static_cast<unsigned>(kFormatVersion));
    f.close();
    LittleFS.remove(path_.c_str());
    fileSize_ = 0;
    headerInitialized_ = false;
    loaded_ = true;
    return true;
  }

  // Scan frames sequentially.
  // v2.0.182 — track whether the scan encountered an irrecoverable garbage
  // frame.  If so, after the scan we atomically truncate the file at the
  // last known-good byte (cursor at the time the bad frame was detected),
  // discarding the garbage tail and the unreachable entries that followed
  // it.  Self-heals legacy corruption (typically the v2.0.173 "a"-mode
  // append bug that left unpatched size-0 placeholders + 4 bytes of
  // misplaced size-field garbage at EOF) without losing the valid prefix.
  size_t cursor = kHeaderSize;
  bool needsHealAtCursor = false;
  uint8_t frameHdr[kFrameHeaderSize];
  while (cursor + kFrameHeaderSize <= fileSize_) {
    if (!f.seek(cursor)) break;
    if (f.read(frameHdr, kFrameHeaderSize) != static_cast<int>(kFrameHeaderSize)) {
      LOG_ERR(TAG, "Truncated frame header at offset %zu in %s — will self-heal", cursor,
              path_.c_str());
      needsHealAtCursor = true;
      break;
    }
    const uint8_t kindByte = frameHdr[0];
    const uint8_t flags = frameHdr[1];
    const uint16_t key = readU16LE(frameHdr + 2);
    const uint32_t size = readU32LE(frameHdr + 4);
    const uint32_t payloadOffset = static_cast<uint32_t>(cursor + kFrameHeaderSize);
    if (payloadOffset + size > fileSize_) {
      LOG_ERR(TAG, "Frame at %zu has size %u that extends past file (%zu) — will self-heal",
              cursor, static_cast<unsigned>(size), fileSize_);
      needsHealAtCursor = true;
      break;
    }
    if (kindByte == 0 || kindByte > 255) {
      // Defensive: unknown kind, skip.
      cursor += kFrameHeaderSize + size;
      continue;
    }
    const Kind kind = static_cast<Kind>(kindByte);
    // Always record — supersede happens via findEntry walking backwards.
    DirectoryEntry e{kind, key, payloadOffset, size};
    if ((flags & kFlagTombstone) != 0) {
      // Tombstone: still recorded as an entry but with size=0.  findEntry
      // will return it (as the latest), and readers will see size==0 →
      // segment "doesn't exist".
      e.size = 0;
    }
    directory_.push_back(e);
    cursor += kFrameHeaderSize + size;
  }

  f.close();

  // v2.0.182 — self-heal pass.  Atomically rewrite the file to its
  // valid-prefix length via temp file + rename, then update fileSize_
  // so subsequent writeSegment/appendFrame calls append at the correct
  // post-truncation offset (NOT at the old EOF that had the garbage).
  //
  // Without this fix, the v2.0.173 corruption persisted indefinitely:
  // every loadDirectory hit the bad frame at offset 13708 and stopped,
  // but subsequent writes appended past the garbage so new entries
  // were physically in the file but unreachable via the directory
  // (which still stopped at 13708).  FB2 reads of those unreachable
  // entries returned empty data → render abort downstream.  Hardware
  // repro was the v2.0.181 crash at PC 0x421680f1 on FB2 spine=46
  // page 7 render.
  //
  // Crash-safety: writes the .heal temp file completely BEFORE removing
  // the original and renaming.  If power is cut between rewrite and
  // rename, next-boot loadDirectory still finds the corrupt original
  // and re-triggers the heal — eventually consistent.  An orphaned
  // .heal file is harmless leakage (cleaned by user's eventual book
  // delete or a future scrubber).
  if (needsHealAtCursor && cursor > kHeaderSize) {
    const size_t goodEnd = cursor;
    const size_t oldFileSize = fileSize_;
    const std::string healPath = path_ + ".heal";

    File src = LittleFS.open(path_.c_str(), "r");
    File dst = LittleFS.open(healPath.c_str(), "w");
    bool rewriteOk = false;
    if (src && dst) {
      uint8_t buf[1024];
      size_t remaining = goodEnd;
      bool ioOk = true;
      while (remaining > 0) {
        const size_t want = remaining < sizeof(buf) ? remaining : sizeof(buf);
        const int got = src.read(buf, want);
        if (got <= 0) {
          LOG_ERR(TAG, "Self-heal: short read at %zu (want=%zu got=%d) in %s",
                  goodEnd - remaining, want, got, path_.c_str());
          ioOk = false;
          break;
        }
        const size_t wrote = dst.write(buf, static_cast<size_t>(got));
        if (wrote != static_cast<size_t>(got)) {
          LOG_ERR(TAG, "Self-heal: short write at %zu (want=%d wrote=%zu) in %s",
                  goodEnd - remaining, got, wrote, healPath.c_str());
          ioOk = false;
          break;
        }
        remaining -= static_cast<size_t>(got);
      }
      dst.flush();
      rewriteOk = ioOk && (remaining == 0);
    }
    if (src) src.close();
    if (dst) dst.close();

    if (rewriteOk) {
      LittleFS.remove(path_.c_str());
      if (LittleFS.rename(healPath.c_str(), path_.c_str())) {
        fileSize_ = goodEnd;
        LOG_INF(TAG, "Self-healed %s: truncated %zu -> %zu bytes, preserved %zu valid entries",
                path_.c_str(), oldFileSize, fileSize_, directory_.size());
      } else {
        LOG_ERR(TAG, "Self-heal: rename %s -> %s failed; cache is now empty",
                healPath.c_str(), path_.c_str());
        // Both files may be in inconsistent state; reset to empty so the
        // next write starts fresh.  Some data is lost but reader recovers.
        directory_.clear();
        fileSize_ = 0;
        headerInitialized_ = false;
      }
    } else {
      LOG_ERR(TAG, "Self-heal: rewrite to %s failed; leaving original corrupt file",
              healPath.c_str());
      LittleFS.remove(healPath.c_str());  // clean up partial heal
      // Leave fileSize_/directory_ as-is (valid prefix loaded, garbage
      // tail still on disk — same state as pre-v2.0.182, no regression).
    }
  }

  headerInitialized_ = true;
  loaded_ = true;
  // v2.0.169 — dropped from INF to DBG.  Every caller creates a fresh
  // UnifiedCache instance and triggers a directory scan, so the log fires
  // ~7-10× per chapter render.  Cosmetic noise; the scan cost itself
  // (~71KB read per scan on EPUB streaming.cache) is real but bounded.
  // A future optimization could cache a per-book UnifiedCache instance
  // inside Epub / Fb2 to amortize the scan across all callers in a session.
  LOG_DBG(TAG, "Loaded %s: fileSize=%zu directoryEntries=%zu", path_.c_str(), fileSize_,
          directory_.size());
  return true;
}

bool UnifiedCache::ensureLoaded() {
  if (loaded_) return true;
  return loadDirectory();
}

std::vector<DirectoryEntry>::const_iterator UnifiedCache::findEntry(Kind kind, uint16_t key) const {
  // Walk backwards — latest entry wins.
  for (auto it = directory_.rbegin(); it != directory_.rend(); ++it) {
    if (it->kind == kind && it->key == key) {
      return it.base() - 1;  // forward iterator pointing at same element
    }
  }
  return directory_.end();
}

bool UnifiedCache::segmentSize(Kind kind, uint16_t key, size_t* outSize) {
  // v2.0.173 — mutex removed (see top-of-file rationale).  Instances are
  // per-call scope-bound; no concurrent access on a single instance.
  if (!ensureLoaded()) return false;
  auto it = findEntry(kind, key);
  if (it == directory_.end() || it->size == 0) return false;
  if (outSize) *outSize = it->size;
  return true;
}

bool UnifiedCache::readSegment(Kind kind, uint16_t key, std::vector<uint8_t>& out) {
  // v2.0.173 — mutex removed (see top-of-file rationale).
  uint32_t offset = 0;
  uint32_t size = 0;
  if (!ensureLoaded()) return false;
  auto it = findEntry(kind, key);
  if (it == directory_.end() || it->size == 0) return false;
  offset = it->offset;
  size = it->size;

  File f = LittleFS.open(path_.c_str(), "r");
  if (!f) {
    LOG_ERR(TAG, "Failed to open %s for read", path_.c_str());
    return false;
  }
  if (!f.seek(offset)) {
    f.close();
    LOG_ERR(TAG, "Seek to %u failed in %s", static_cast<unsigned>(offset), path_.c_str());
    return false;
  }
  out.resize(size);
  const int n = f.read(out.data(), size);
  f.close();
  if (n != static_cast<int>(size)) {
    LOG_ERR(TAG, "Short read at %u (got %d want %u) in %s", static_cast<unsigned>(offset), n,
            static_cast<unsigned>(size), path_.c_str());
    out.clear();
    return false;
  }
  return true;
}

bool UnifiedCache::openSegmentReader(Kind kind, uint16_t key, File& outFile, size_t* outSize) {
  // v2.0.173 — mutex removed (see top-of-file rationale).
  uint32_t offset = 0;
  uint32_t size = 0;
  if (!ensureLoaded()) return false;
  auto it = findEntry(kind, key);
  if (it == directory_.end() || it->size == 0) return false;
  offset = it->offset;
  size = it->size;
  outFile = LittleFS.open(path_.c_str(), "r");
  if (!outFile) {
    LOG_ERR(TAG, "Failed to open %s for streaming read", path_.c_str());
    return false;
  }
  if (!outFile.seek(offset)) {
    LOG_ERR(TAG, "Seek to %u failed in %s (streaming)", static_cast<unsigned>(offset), path_.c_str());
    outFile.close();
    return false;
  }
  if (outSize) *outSize = size;
  return true;
}

bool UnifiedCache::appendFrame(Kind kind, uint16_t key, uint8_t flags,
                                 const std::function<bool(File&)>& writePayload,
                                 size_t payloadSize) {
  // v2.0.173 — mutex removed (see top-of-file rationale).
  if (!ensureLoaded()) return false;

  // Open in append mode.  If file doesn't exist, write the header first.
  File f = LittleFS.open(path_.c_str(), headerInitialized_ ? "a" : "w");
  if (!f) {
    LOG_ERR(TAG, "Failed to open %s for append", path_.c_str());
    return false;
  }
  if (!headerInitialized_) {
    if (!writeFileHeader(f)) {
      f.close();
      return false;
    }
    headerInitialized_ = true;
    fileSize_ = kHeaderSize;
  }

  // v2.0.168 — use f.position() instead of f.size().  Arduino LittleFS's
  // File::size() returns the on-disk size, NOT the buffered-write size;
  // calling it right after a write returns the pre-write value because
  // the write hasn't been flushed yet.  f.position() returns the current
  // write cursor, which always reflects "where the next byte goes" —
  // exactly what we want when computing the offset of a new frame.
  const uint32_t payloadOffset = static_cast<uint32_t>(f.position()) + kFrameHeaderSize;
  uint8_t frameHdr[kFrameHeaderSize];
  frameHdr[0] = static_cast<uint8_t>(kind);
  frameHdr[1] = flags;
  writeU16LE(frameHdr + 2, key);
  writeU32LE(frameHdr + 4, static_cast<uint32_t>(payloadSize));
  if (f.write(frameHdr, kFrameHeaderSize) != kFrameHeaderSize) {
    LOG_ERR(TAG, "Failed to write frame header to %s", path_.c_str());
    f.close();
    return false;
  }

  if (writePayload) {
    if (!writePayload(f)) {
      LOG_ERR(TAG, "Payload write callback failed for frame kind=%u key=%u",
              static_cast<unsigned>(kind), static_cast<unsigned>(key));
      f.close();
      return false;
    }
  }

  f.flush();
  fileSize_ = f.size();
  f.close();

  // Update in-memory directory: append new entry.
  directory_.push_back({kind, key, payloadOffset,
                         (flags & kFlagTombstone) ? 0u : static_cast<uint32_t>(payloadSize)});
  return true;
}

bool UnifiedCache::writeSegment(Kind kind, uint16_t key, const uint8_t* data, size_t size) {
  return appendFrame(kind, key, 0,
                     [data, size](File& f) {
                       if (size == 0) return true;
                       return f.write(data, size) == size;
                     },
                     size);
}

bool UnifiedCache::writeSegmentStreamingDeferred(Kind kind, uint16_t key,
                                                  const std::function<bool(File&)>& writeCb) {
  // v2.0.173 — mutex removed (see top-of-file rationale).
  if (!ensureLoaded()) return false;

  // v2.0.174 — open existing files in "r+" mode, NOT "a" mode.  This is the
  // architectural fix for the corruption bug discovered after v2.0.173 was
  // flashed: appending the second markers segment to an FB2 streaming.cache
  // produced a frame header with size=0 (placeholder never patched) plus 4
  // bytes of garbage at the file's end, manifesting as
  //   [ERR] [UCACHE] Frame at <X> has size <huge> that extends past file
  // on every subsequent loadDirectory.
  //
  // Root cause: POSIX "a" mode (O_APPEND) forces EVERY write to be
  // positioned at end-of-file atomically, regardless of any preceding
  // seek().  The deferred-streaming flow opens the file, writes a frame
  // header with placeholder size, writes the payload via writeCb, then
  // seeks back to (frameHdrPos + 4) to patch the size field — but in "a"
  // mode that patch write IGNORES the seek and gets appended to EOF
  // instead, leaving the placeholder bytes untouched and growing the file
  // by an extra 4 bytes.  v2.0.168 fixed an earlier f.size()/f.position()
  // bug but didn't notice that "a" mode breaks seek-back semantics
  // entirely — that earlier fix worked only on fresh files (opened in
  // "w" mode, where seek-back DOES work).
  //
  // Fix: open with "r+" (read/write, no append flag) and manually seek to
  // end-of-file to get the initial append-like behaviour.  Subsequent
  // seek+write to patch the frame size now goes to the right offset.
  File f;
  if (headerInitialized_) {
    f = LittleFS.open(path_.c_str(), "r+");
    if (!f) {
      LOG_ERR(TAG, "Failed to open %s for r+ deferred-streaming", path_.c_str());
      return false;
    }
    if (!f.seek(f.size())) {
      LOG_ERR(TAG, "Failed to seek to end of %s for append-like write", path_.c_str());
      f.close();
      return false;
    }
  } else {
    f = LittleFS.open(path_.c_str(), "w");
    if (!f) {
      LOG_ERR(TAG, "Failed to open %s for w deferred-streaming", path_.c_str());
      return false;
    }
    if (!writeFileHeader(f)) {
      f.close();
      return false;
    }
    headerInitialized_ = true;
    fileSize_ = kHeaderSize;
  }

  // v2.0.168 — use f.position() instead of f.size().  See note in
  // appendFrame.  v2.0.174 — additionally documented: f.position() AFTER
  // an explicit seek(f.size()) on an "r+" handle reliably returns the
  // file's end-of-file offset, which is the correct frameHdrPos for the
  // new frame we're about to write.
  const uint32_t frameHdrPos = static_cast<uint32_t>(f.position());
  uint8_t frameHdr[kFrameHeaderSize] = {};
  frameHdr[0] = static_cast<uint8_t>(kind);
  frameHdr[1] = 0;
  writeU16LE(frameHdr + 2, key);
  writeU32LE(frameHdr + 4, 0);  // placeholder size, patched after writeCb
  if (f.write(frameHdr, kFrameHeaderSize) != kFrameHeaderSize) {
    LOG_ERR(TAG, "Failed to write frame placeholder header");
    f.close();
    return false;
  }
  const uint32_t payloadStart = static_cast<uint32_t>(f.position());

  if (writeCb) {
    if (!writeCb(f)) {
      LOG_ERR(TAG, "Deferred-streaming writeCb failed (kind=%u key=%u)",
              static_cast<unsigned>(kind), static_cast<unsigned>(key));
      f.close();
      return false;
    }
  }

  const uint32_t payloadEnd = static_cast<uint32_t>(f.position());
  const uint32_t payloadSize = payloadEnd - payloadStart;

  // Patch the placeholder size field in the frame header.
  if (!f.seek(frameHdrPos + 4)) {
    LOG_ERR(TAG, "Failed to seek back to patch frame size");
    f.close();
    return false;
  }
  uint8_t sizeBytes[4];
  writeU32LE(sizeBytes, payloadSize);
  if (f.write(sizeBytes, 4) != 4) {
    LOG_ERR(TAG, "Failed to patch frame size");
    f.close();
    return false;
  }
  f.flush();
  fileSize_ = payloadEnd;
  f.close();

  directory_.push_back({kind, key, payloadStart, payloadSize});
  return true;
}

bool UnifiedCache::writeSegmentStreaming(Kind kind, uint16_t key, size_t expectedSize,
                                          const std::function<bool(File&)>& writeCb) {
  const size_t fileBefore = fileSize_;
  if (!appendFrame(kind, key, 0,
                   [&writeCb, expectedSize](File& f) {
                     // v2.0.168 — f.position() not f.size(); see appendFrame
                     // and writeSegmentStreamingDeferred for the rationale.
                     const size_t startPos = f.position();
                     if (!writeCb(f)) return false;
                     const size_t endPos = f.position();
                     if (endPos - startPos != expectedSize) {
                       LOG_ERR(TAG, "Streaming write size mismatch (got %zu want %zu)",
                               endPos - startPos, expectedSize);
                       return false;
                     }
                     return true;
                   },
                   expectedSize)) {
    // Rollback by truncating to size before the append (best-effort —
    // LittleFS doesn't have a direct truncate, so we open w-mode and
    // rewrite up to fileBefore).  For simplicity, log and leave the
    // partial frame; loadDirectory's truncated-frame check will skip
    // it on next open.
    LOG_ERR(TAG, "writeSegmentStreaming rolled back; partial frame may remain (file=%zu before=%zu)",
            fileSize_, fileBefore);
    return false;
  }
  return true;
}

bool UnifiedCache::removeSegment(Kind kind, uint16_t key) {
  return appendFrame(kind, key, kFlagTombstone, {}, 0);
}

size_t UnifiedCache::liveSize() const {
  // v2.0.173 — mutex removed (see top-of-file rationale).
  size_t total = 0;
  for (const auto& e : directory_) {
    total += e.size;
  }
  return total;
}

}  // namespace snapix::unifiedcache
