#include "EpubRenderWorkspace.h"

#include <cstring>

namespace snapix::workspace {

namespace {

// The single BSS-allocated workspace instance.  Anonymous-namespace static
// keeps it private to this TU; the accessor below is the only published
// reference.  Zero-init guaranteed by C++ BSS rules.
EpubRenderWorkspace gWorkspace;

}  // namespace

EpubRenderWorkspace& workspace() {
  return gWorkspace;
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------
void EpubRenderWorkspace::resetForChapter() {
  anchorCount = 0;
  arenaPos    = 0;
  // Note: we deliberately do NOT memset the backing buffers — bytes are
  // overwritten on next use and zeroing would burn ~4 KB of writes per
  // chapter for no functional benefit.
}

// ---------------------------------------------------------------------------
// String arena.  Bump-up-from-zero, no individual free.
// ---------------------------------------------------------------------------
uint8_t* EpubRenderWorkspace::arenaAlloc(const uint16_t n) {
  if (n == 0) return stringArenaBuf;  // valid zero-byte allocation
  if (static_cast<uint32_t>(arenaPos) + n > kStringArenaBytes) {
    return nullptr;  // OOM
  }
  uint8_t* p = stringArenaBuf + arenaPos;
  arenaPos = static_cast<uint16_t>(arenaPos + n);
  return p;
}

const char* EpubRenderWorkspace::arenaCopyStr(const char* s) {
  if (s == nullptr) return nullptr;
  const size_t n = std::strlen(s) + 1;  // include null terminator
  if (n > 0xFFFFu) return nullptr;       // pointer offsets are uint16_t
  uint8_t* dst = arenaAlloc(static_cast<uint16_t>(n));
  if (dst == nullptr) return nullptr;
  std::memcpy(dst, s, n);
  return reinterpret_cast<const char*>(dst);
}

// ---------------------------------------------------------------------------
// Anchor map.
// ---------------------------------------------------------------------------
bool EpubRenderWorkspace::addAnchor(const char* id, const uint16_t pageNum) {
  if (id == nullptr || *id == '\0') return false;
  if (anchorCount >= kAnchorSlotsCount) return false;  // slot overflow

  // Capture current arena pos BEFORE the copy so we can rewind on failure.
  const uint16_t savedPos = arenaPos;
  const char* copied = arenaCopyStr(id);
  if (copied == nullptr) {
    arenaPos = savedPos;  // belt + suspenders (arenaCopyStr already bailed)
    return false;
  }
  const uint16_t offset = static_cast<uint16_t>(
      reinterpret_cast<const uint8_t*>(copied) - stringArenaBuf);

  anchors[anchorCount].idArenaOffset = offset;
  anchors[anchorCount].pageNum       = pageNum;
  ++anchorCount;
  return true;
}

const char* EpubRenderWorkspace::anchorId(const uint16_t index) const {
  if (index >= anchorCount) return nullptr;
  const uint16_t off = anchors[index].idArenaOffset;
  if (off >= kStringArenaBytes) return nullptr;
  return reinterpret_cast<const char*>(stringArenaBuf + off);
}

uint16_t EpubRenderWorkspace::anchorPage(const char* id) const {
  if (id == nullptr || *id == '\0') return kAnchorNotFound;
  for (uint16_t i = 0; i < anchorCount; ++i) {
    const char* cand = anchorId(i);
    if (cand && std::strcmp(cand, id) == 0) return anchors[i].pageNum;
  }
  return kAnchorNotFound;
}

}  // namespace snapix::workspace
