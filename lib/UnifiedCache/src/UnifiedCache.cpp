#include "UnifiedCache.h"

#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <utility>

#define TAG "UCACHE"

namespace snapix::unifiedcache {

UnifiedCache UnifiedCache::shared(const std::string& bookCachePath) {
  // Return by value: the directory snapshot lives only for the caller's
  // operation, so it cannot pin heap fragments across book/session changes.
  // Parsers pass this object by reference through their marker/index steps,
  // amortising the scan locally without a process-wide registry or mutex.
  return UnifiedCache(bookCachePath);
}

namespace {

constexpr uint8_t kMagic[4] = {'U', 'C', 'A', 'C'};
constexpr size_t kCompactionMinFileBytes = 128 * 1024;
constexpr size_t kCompactionMinGarbageBytes = 64 * 1024;

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

inline uint32_t directoryId(Kind kind, uint16_t key) {
  return (static_cast<uint32_t>(key) << 8) |
         static_cast<uint8_t>(kind);
}

}  // namespace

UnifiedCache::UnifiedCache(std::string bookCachePath)
    : path_(std::move(bookCachePath) + "/streaming.cache") {}

bool UnifiedCache::recoverCompactionSwap() {
  const std::string compactPath = path_ + ".compact";
  const std::string backupPath = path_ + ".bak";
  const std::string healPath = path_ + ".heal";
  const std::string healBackupPath = path_ + ".heal.bak";
  bool hasMain = LittleFS.exists(path_.c_str());

  // Recover a self-heal swap interrupted after original -> .heal.bak.
  if (!hasMain && LittleFS.exists(healBackupPath.c_str())) {
    if (!LittleFS.rename(healBackupPath.c_str(), path_.c_str())) {
      LOG_ERR(TAG, "Self-heal recovery: restore %s -> %s failed",
              healBackupPath.c_str(), path_.c_str());
      return false;
    }
    hasMain = true;
  }
  if (hasMain) {
    LittleFS.remove(healPath.c_str());
    LittleFS.remove(healBackupPath.c_str());
  }

  const bool hasBackup = LittleFS.exists(backupPath.c_str());

  if (!hasMain && hasBackup) {
    // Power loss after original -> .bak but before .compact -> original.
    if (!LittleFS.rename(backupPath.c_str(), path_.c_str())) {
      LOG_ERR(TAG, "Compaction recovery: restore %s -> %s failed",
              backupPath.c_str(), path_.c_str());
      return false;
    }
  } else if (!hasMain && LittleFS.exists(compactPath.c_str())) {
    // No original and no backup: the fully-written compact candidate is the
    // only recoverable copy.
    if (!LittleFS.rename(compactPath.c_str(), path_.c_str())) {
      LOG_ERR(TAG, "Compaction recovery: promote %s -> %s failed",
              compactPath.c_str(), path_.c_str());
      return false;
    }
  }

  // A compact temp beside a valid main is pre-commit debris.  Keep `.bak`
  // until loadDirectory validates the main file below.
  if (LittleFS.exists(path_.c_str()) && LittleFS.exists(compactPath.c_str())) {
    LittleFS.remove(compactPath.c_str());
  }
  return true;
}

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

  if (!recoverCompactionSwap()) return false;

  if (!LittleFS.exists(path_.c_str())) {
    // Fresh cache — header gets written on first appendFrame.
    headerInitialized_ = false;
    loaded_ = true;
    return true;
  }

  File f = LittleFS.open(path_.c_str(), "r");
  if (!f) {
    LOG_ERR(TAG, "Failed to open %s for read", path_.c_str());
    const std::string backupPath = path_ + ".bak";
    if (LittleFS.exists(backupPath.c_str())) {
      LittleFS.remove(path_.c_str());
      if (LittleFS.rename(backupPath.c_str(), path_.c_str())) {
        loaded_ = false;
        return loadDirectory();
      }
    }
    return false;
  }
  fileSize_ = f.size();
  const std::string backupPath = path_ + ".bak";
  auto restoreBackup = [&]() -> bool {
    if (!LittleFS.exists(backupPath.c_str())) return false;
    LittleFS.remove(path_.c_str());
    if (!LittleFS.rename(backupPath.c_str(), path_.c_str())) return false;
    loaded_ = false;
    return loadDirectory();
  };

  if (fileSize_ < kHeaderSize) {
    LOG_ERR(TAG, "File %s too small (%zu bytes) — treating as empty", path_.c_str(), fileSize_);
    f.close();
    if (restoreBackup()) return true;
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
    if (restoreBackup()) return true;
    return false;
  }
  if (std::memcmp(hdr, kMagic, sizeof(kMagic)) != 0) {
    LOG_ERR(TAG, "Bad magic in %s — wiping", path_.c_str());
    f.close();
    if (restoreBackup()) return true;
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
    LittleFS.remove(backupPath.c_str());
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
    if ((flags & kFlagIncomplete) != 0) {
      LOG_INF(TAG, "Incomplete frame at %zu in %s — will self-heal", cursor,
              path_.c_str());
      needsHealAtCursor = true;
      break;
    }
    if (static_cast<size_t>(payloadOffset) + static_cast<size_t>(size) >
        fileSize_) {
      LOG_ERR(TAG, "Frame at %zu has size %u that extends past file (%zu) — will self-heal",
              cursor, static_cast<unsigned>(size), fileSize_);
      needsHealAtCursor = true;
      break;
    }
    if (kindByte < static_cast<uint8_t>(Kind::Markers) ||
        kindByte > static_cast<uint8_t>(Kind::Metrics)) {
      // Defensive: unknown kind, skip.
      cursor += kFrameHeaderSize + size;
      continue;
    }
    const Kind kind = static_cast<Kind>(kindByte);
    DirectoryEntry e{kind, key, payloadOffset, size, false};
    if ((flags & kFlagTombstone) != 0) {
      // Tombstone is distinct from a legitimate empty payload.
      e.size = 0;
      e.tombstone = true;
    }
    upsertDirectoryEntry(e);
    cursor += kFrameHeaderSize + size;
  }
  // A reset can interrupt even the 8-byte frame header.  Without trimming
  // this short tail, the next append starts after it and the scanner never
  // reaches any later frames.
  if (!needsHealAtCursor && cursor != fileSize_) {
    LOG_INF(TAG, "Partial frame header at %zu in %s — will self-heal",
            cursor, path_.c_str());
    needsHealAtCursor = true;
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
  if (needsHealAtCursor && cursor >= kHeaderSize) {
    const size_t goodEnd = cursor;
    const size_t oldFileSize = fileSize_;
    const std::string healPath = path_ + ".heal";
    const std::string healBackupPath = path_ + ".heal.bak";

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
      LittleFS.remove(healBackupPath.c_str());
      const bool backedUp =
          LittleFS.rename(path_.c_str(), healBackupPath.c_str());
      if (backedUp && LittleFS.rename(healPath.c_str(), path_.c_str())) {
        LittleFS.remove(healBackupPath.c_str());
        fileSize_ = goodEnd;
        LOG_INF(TAG, "Self-healed %s: truncated %zu -> %zu bytes, preserved %zu valid entries",
                path_.c_str(), oldFileSize, fileSize_, directory_.size());
      } else {
        LOG_ERR(TAG, "Self-heal: recoverable swap %s -> %s failed",
                healPath.c_str(), path_.c_str());
        if (!LittleFS.exists(path_.c_str()) &&
            LittleFS.exists(healBackupPath.c_str())) {
          LittleFS.rename(healBackupPath.c_str(), path_.c_str());
        }
        LittleFS.remove(healPath.c_str());
        loaded_ = false;
        return false;
      }
    } else {
      LOG_ERR(TAG, "Self-heal: rewrite to %s failed; leaving original corrupt file",
              healPath.c_str());
      LittleFS.remove(healPath.c_str());  // clean up partial heal
      loaded_ = false;
      return false;  // retry later; never append behind an unreachable tail
    }
  }

  headerInitialized_ = true;
  loaded_ = true;
  if (LittleFS.exists(backupPath.c_str())) {
    LittleFS.remove(backupPath.c_str());
  }
  // Operation-local instances still scan once at first use, hence DBG.
  LOG_DBG(TAG, "Loaded %s: fileSize=%zu directoryEntries=%zu", path_.c_str(), fileSize_,
          directory_.size());
  return true;
}

bool UnifiedCache::ensureLoaded() {
  if (loaded_) return true;
  return loadDirectory();
}

std::vector<DirectoryEntry>::const_iterator UnifiedCache::findEntry(Kind kind, uint16_t key) const {
  const uint32_t id = directoryId(kind, key);
  const auto it = std::lower_bound(
      directory_.begin(), directory_.end(), id,
      [](const DirectoryEntry& entry, uint32_t wanted) {
        return directoryId(entry.kind, entry.key) < wanted;
      });
  if (it != directory_.end() && directoryId(it->kind, it->key) == id) {
    return it;
  }
  return directory_.end();
}

void UnifiedCache::upsertDirectoryEntry(DirectoryEntry entry) {
  const uint32_t id = directoryId(entry.kind, entry.key);
  auto it = std::lower_bound(
      directory_.begin(), directory_.end(), id,
      [](const DirectoryEntry& current, uint32_t wanted) {
        return directoryId(current.kind, current.key) < wanted;
      });
  if (it != directory_.end() && directoryId(it->kind, it->key) == id) {
    *it = entry;
  } else {
    directory_.insert(it, entry);
  }
}

bool UnifiedCache::segmentSize(Kind kind, uint16_t key, size_t* outSize) {
  // v2.0.173 — mutex removed (see top-of-file rationale).  Instances are
  // per-call scope-bound; no concurrent access on a single instance.
  if (!ensureLoaded()) return false;
  auto it = findEntry(kind, key);
  if (it == directory_.end() || it->tombstone) return false;
  if (outSize) *outSize = it->size;
  return true;
}

bool UnifiedCache::readSegment(Kind kind, uint16_t key, std::vector<uint8_t>& out) {
  // v2.0.173 — mutex removed (see top-of-file rationale).
  uint32_t offset = 0;
  uint32_t size = 0;
  if (!ensureLoaded()) return false;
  auto it = findEntry(kind, key);
  if (it == directory_.end() || it->tombstone) return false;
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
  const int n = size == 0 ? 0 : f.read(out.data(), size);
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
  if (it == directory_.end() || it->tombstone) return false;
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

  // Use r+ rather than append mode so the incomplete bit can be cleared only
  // after the full payload is durable.  POSIX O_APPEND ignores seek-back
  // writes and caused the older deferred-frame corruption fixed in v2.0.174.
  File f;
  if (headerInitialized_) {
    f = LittleFS.open(path_.c_str(), "r+");
    if (!f || !f.seek(f.size())) {
      LOG_ERR(TAG, "Failed to open/seek %s for append", path_.c_str());
      if (f) f.close();
      loaded_ = false;
      return false;
    }
  } else {
    f = LittleFS.open(path_.c_str(), "w");
    if (!f) {
      LOG_ERR(TAG, "Failed to create %s", path_.c_str());
      return false;
    }
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
  const uint32_t frameHdrPos = static_cast<uint32_t>(f.position());
  const uint32_t payloadOffset = frameHdrPos + kFrameHeaderSize;
  uint8_t frameHdr[kFrameHeaderSize];
  frameHdr[0] = static_cast<uint8_t>(kind);
  frameHdr[1] = flags | kFlagIncomplete;
  writeU16LE(frameHdr + 2, key);
  writeU32LE(frameHdr + 4, static_cast<uint32_t>(payloadSize));
  if (f.write(frameHdr, kFrameHeaderSize) != kFrameHeaderSize) {
    LOG_ERR(TAG, "Failed to write frame header to %s", path_.c_str());
    f.close();
    loaded_ = false;
    return false;
  }

  if (writePayload) {
    if (!writePayload(f)) {
      LOG_ERR(TAG, "Payload write callback failed for frame kind=%u key=%u",
              static_cast<unsigned>(kind), static_cast<unsigned>(key));
      f.close();
      loaded_ = false;
      return false;
    }
  }

  const size_t payloadEnd = f.position();
  if (payloadEnd != static_cast<size_t>(payloadOffset) + payloadSize) {
    LOG_ERR(TAG, "Frame size mismatch kind=%u key=%u got=%zu want=%zu",
            static_cast<unsigned>(kind), static_cast<unsigned>(key),
            payloadEnd - payloadOffset, payloadSize);
    f.close();
    loaded_ = false;
    return false;
  }
  // Make the complete payload durable while the frame is still explicitly
  // invisible.  Only then clear the incomplete bit and flush the one-byte
  // commit marker.
  f.flush();
  if (!f.seek(frameHdrPos + 1) || f.write(&flags, 1) != 1) {
    LOG_ERR(TAG, "Failed to publish frame kind=%u key=%u",
            static_cast<unsigned>(kind), static_cast<unsigned>(key));
    f.close();
    loaded_ = false;
    return false;
  }
  f.flush();
  fileSize_ = payloadEnd;
  f.close();

  // Update the operation-local latest-entry directory.
  upsertDirectoryEntry(
      {kind, key, payloadOffset,
       (flags & kFlagTombstone) ? 0u : static_cast<uint32_t>(payloadSize),
       (flags & kFlagTombstone) != 0});
  return true;
}

bool UnifiedCache::writeSegment(Kind kind, uint16_t key, const uint8_t* data, size_t size) {
  const bool ok = appendFrame(
      kind, key, 0,
      [data, size](File& f) {
        if (size == 0) return true;
        return f.write(data, size) == size;
      },
      size);
  if (ok) (void)compactIfNeeded();
  return ok;
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
  frameHdr[1] = kFlagIncomplete;
  writeU16LE(frameHdr + 2, key);
  writeU32LE(frameHdr + 4, 0);  // placeholder size, patched after writeCb
  if (f.write(frameHdr, kFrameHeaderSize) != kFrameHeaderSize) {
    LOG_ERR(TAG, "Failed to write frame placeholder header");
    f.close();
    loaded_ = false;
    return false;
  }
  const uint32_t payloadStart = static_cast<uint32_t>(f.position());

  if (writeCb) {
    if (!writeCb(f)) {
      LOG_ERR(TAG, "Deferred-streaming writeCb failed (kind=%u key=%u)",
              static_cast<unsigned>(kind), static_cast<unsigned>(key));
      f.close();
      loaded_ = false;  // next access rescans + truncates the incomplete tail
      return false;
    }
  }

  const uint32_t payloadEnd = static_cast<uint32_t>(f.position());
  const uint32_t payloadSize = payloadEnd - payloadStart;

  // Patch the placeholder size field in the frame header.
  if (!f.seek(frameHdrPos + 4)) {
    LOG_ERR(TAG, "Failed to seek back to patch frame size");
    f.close();
    loaded_ = false;
    return false;
  }
  uint8_t sizeBytes[4];
  writeU32LE(sizeBytes, payloadSize);
  if (f.write(sizeBytes, 4) != 4) {
    LOG_ERR(TAG, "Failed to patch frame size");
    f.close();
    loaded_ = false;
    return false;
  }
  // Commit ordering matters under power loss: persist header + payload + final
  // size while the incomplete bit is still set, then publish with a separate
  // one-byte write and flush.
  f.flush();
  // Publish last: an interrupted frame remains flagged incomplete and is
  // removed by loadDirectory rather than superseding a valid older segment.
  if (!f.seek(frameHdrPos + 1)) {
    LOG_ERR(TAG, "Failed to seek back to publish deferred frame");
    f.close();
    loaded_ = false;
    return false;
  }
  const uint8_t publishedFlags = 0;
  if (f.write(&publishedFlags, 1) != 1) {
    LOG_ERR(TAG, "Failed to publish deferred frame");
    f.close();
    loaded_ = false;
    return false;
  }
  f.flush();
  fileSize_ = payloadEnd;
  f.close();

  upsertDirectoryEntry({kind, key, payloadStart, payloadSize, false});
  (void)compactIfNeeded();
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
    // The frame is intentionally left unpublished.  A later load removes
    // the incomplete tail without allowing it to shadow the older value.
    LOG_ERR(TAG, "Streaming frame was not published; recovery will discard it (file=%zu before=%zu)",
            fileSize_, fileBefore);
    return false;
  }
  (void)compactIfNeeded();
  return true;
}

bool UnifiedCache::removeSegment(Kind kind, uint16_t key) {
  const bool ok = appendFrame(kind, key, kFlagTombstone, {}, 0);
  if (ok) (void)compactIfNeeded();
  return ok;
}

size_t UnifiedCache::liveSize() const {
  size_t total = 0;
  for (const auto& entry : directory_) {
    if (!entry.tombstone) total += entry.size;
  }
  return total;
}

size_t UnifiedCache::compactedFileSize() const {
  size_t total = kHeaderSize;
  for (const auto& entry : directory_) {
    if (!entry.tombstone) total += kFrameHeaderSize + entry.size;
  }
  return total;
}

bool UnifiedCache::compactIfNeeded() {
  if (!ensureLoaded() || fileSize_ < kCompactionMinFileBytes) return true;
  const size_t compactBytes = compactedFileSize();
  if (compactBytes >= fileSize_) return true;
  const size_t garbageBytes = fileSize_ - compactBytes;
  if (garbageBytes < kCompactionMinGarbageBytes ||
      garbageBytes * 3 < fileSize_) {
    return true;
  }
  return compact();
}

bool UnifiedCache::compact() {
  if (!ensureLoaded() || !headerInitialized_ || !LittleFS.exists(path_.c_str())) {
    return true;
  }

  const std::string compactPath = path_ + ".compact";
  const std::string backupPath = path_ + ".bak";
  LittleFS.remove(compactPath.c_str());

  File src = LittleFS.open(path_.c_str(), "r");
  File dst = LittleFS.open(compactPath.c_str(), "w");
  if (!src || !dst || !writeFileHeader(dst)) {
    if (src) src.close();
    if (dst) dst.close();
    LittleFS.remove(compactPath.c_str());
    return false;
  }

  bool ioOk = true;
  uint8_t copyBuf[1024];
  for (const auto& entry : directory_) {
    if (entry.tombstone) continue;

    uint8_t frameHdr[kFrameHeaderSize] = {};
    frameHdr[0] = static_cast<uint8_t>(entry.kind);
    writeU16LE(frameHdr + 2, entry.key);
    writeU32LE(frameHdr + 4, entry.size);
    if (dst.write(frameHdr, sizeof(frameHdr)) != sizeof(frameHdr) ||
        !src.seek(entry.offset)) {
      ioOk = false;
      break;
    }

    size_t remaining = entry.size;
    while (remaining > 0) {
      const size_t want = std::min(remaining, sizeof(copyBuf));
      const int got = src.read(copyBuf, want);
      if (got <= 0 ||
          dst.write(copyBuf, static_cast<size_t>(got)) !=
              static_cast<size_t>(got)) {
        ioOk = false;
        break;
      }
      remaining -= static_cast<size_t>(got);
    }
    if (!ioOk) break;
  }

  dst.flush();
  const size_t compactBytes = dst.size();
  src.close();
  dst.close();
  if (!ioOk || compactBytes != compactedFileSize()) {
    LOG_ERR(TAG, "Compaction write failed for %s (got=%zu want=%zu)",
            path_.c_str(), compactBytes, compactedFileSize());
    LittleFS.remove(compactPath.c_str());
    return false;
  }

  LittleFS.remove(backupPath.c_str());
  if (!LittleFS.rename(path_.c_str(), backupPath.c_str())) {
    LOG_ERR(TAG, "Compaction could not back up %s", path_.c_str());
    LittleFS.remove(compactPath.c_str());
    return false;
  }
  if (!LittleFS.rename(compactPath.c_str(), path_.c_str())) {
    LOG_ERR(TAG, "Compaction could not publish %s", compactPath.c_str());
    LittleFS.rename(backupPath.c_str(), path_.c_str());
    LittleFS.remove(compactPath.c_str());
    loaded_ = false;
    return false;
  }

  const size_t oldBytes = fileSize_;
  loaded_ = false;
  if (!loadDirectory()) {
    LittleFS.remove(path_.c_str());
    if (LittleFS.rename(backupPath.c_str(), path_.c_str())) {
      loaded_ = false;
      (void)loadDirectory();
    }
    return false;
  }
  LOG_INF(TAG, "Compacted %s: %zu -> %zu bytes, live=%zu",
          path_.c_str(), oldBytes, fileSize_, liveSize());
  return true;
}

}  // namespace snapix::unifiedcache
