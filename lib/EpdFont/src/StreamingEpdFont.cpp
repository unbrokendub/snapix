#include "StreamingEpdFont.h"

#include <SharedSpiLock.h>
#include <Utf8.h>

#if __has_include(<esp_attr.h>)
#include <esp_attr.h>
#endif
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

#include <algorithm>
#include <cstring>

#include "EpdFontLoader.h"

StreamingEpdFont::StreamingEpdFont() {
  memset(&_fontData, 0, sizeof(_fontData));
  for (int i = 0; i < CACHE_SIZE; i++) {
    _hashTable[i] = HASH_EMPTY;
  }
}

StreamingEpdFont::~StreamingEpdFont() { unload(); }

bool StreamingEpdFont::load(const char* path) {
  unload();

  // Use EpdFontLoader to parse the file
  auto result = EpdFontLoader::loadForStreaming(path);
  if (!result.success) {
    return false;
  }

  // Transfer ownership of allocated data
  _glyphs = result.glyphs;
  _intervals = result.intervals;
  _glyphCount = result.glyphCount;
  _glyphsSize = result.glyphsSize;
  _intervalsSize = result.intervalsSize;
  _bitmapOffset = result.bitmapOffset;

  // Copy font metadata
  _fontData.bitmap = nullptr;  // No bitmap in RAM - we stream it
  _fontData.glyph = _glyphs;
  _fontData.intervals = _intervals;
  _fontData.intervalCount = result.fontData.intervalCount;
  _fontData.advanceY = result.fontData.advanceY;
  _fontData.ascender = result.fontData.ascender;
  _fontData.descender = result.fontData.descender;
  _fontData.is2Bit = result.fontData.is2Bit;

  // Open file and keep it open for streaming
  if (!SdMan.openFileForRead("SFONT", path, _fontFile)) {
    delete[] _glyphs;
    delete[] _intervals;
    _glyphs = nullptr;
    _intervals = nullptr;
    return false;
  }

  // Allocate the bitmap pool — one contiguous 32 KB buffer that stays put
  // for the lifetime of the font.  Each cache slot owns a fixed slice.
  // This eliminates the per-glyph `new[]` calls that previously fragmented
  // the heap during a single page render.
  _pool = new (std::nothrow) uint8_t[POOL_BYTES];
  if (!_pool) {
    snapix::spi::SharedBusLock lk;
    _fontFile.close();
    delete[] _glyphs;
    delete[] _intervals;
    _glyphs = nullptr;
    _intervals = nullptr;
    return false;
  }

  // Reset cache state.  All slots already have INVALID glyphIndex from the
  // default initialiser; only the hash table needs explicit reset.
  for (int i = 0; i < CACHE_SIZE; i++) {
    _hashTable[i] = HASH_EMPTY;
    _cache[i].glyphIndex = INVALID_CODEPOINT;
    _cache[i].bitmapSize = 0;
    _cache[i].lastUsed = 0;
  }
  _accessCounter = 0;
  _totalCacheAllocation = 0;
  _tombstoneCount = 0;
  _cacheHits = 0;
  _cacheMisses = 0;

  _isLoaded = true;
  return true;
}

void StreamingEpdFont::clearBitmapCache() {
  if (!_isLoaded) return;

  // With the pool design there is nothing to free here — the pool is one
  // contiguous allocation that lives until unload().  Resetting metadata
  // simply marks every slot as available so the next render's glyph warm
  // starts fresh (useful on book transitions where the previous book's
  // codepoint set is irrelevant to the new one).
  for (int i = 0; i < CACHE_SIZE; i++) {
    _cache[i].glyphIndex = INVALID_CODEPOINT;
    _cache[i].bitmapSize = 0;
    _cache[i].lastUsed = 0;
    _hashTable[i] = HASH_EMPTY;
  }

  for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
    _glyphCache[i].codepoint = INVALID_CODEPOINT;
    _glyphCache[i].glyph = nullptr;
  }

  _accessCounter = 0;
  _totalCacheAllocation = 0;
  _tombstoneCount = 0;
  _cacheHits = 0;
  _cacheMisses = 0;
}

void StreamingEpdFont::unload() {
  if (_fontFile) {
    snapix::spi::SharedBusLock lk;
    _fontFile.close();
  }

  delete[] _glyphs;
  delete[] _intervals;

  // Free pool and scratch buffer (single allocations).
  delete[] _pool;
  _pool = nullptr;
  delete[] _scratch;
  _scratch = nullptr;

  // Reset cache slots metadata
  for (int i = 0; i < CACHE_SIZE; i++) {
    _cache[i].glyphIndex = INVALID_CODEPOINT;
    _cache[i].bitmapSize = 0;
    _cache[i].lastUsed = 0;
    _hashTable[i] = HASH_EMPTY;
  }

  // Clear glyph lookup cache
  for (int i = 0; i < GLYPH_CACHE_SIZE; i++) {
    _glyphCache[i].codepoint = INVALID_CODEPOINT;
    _glyphCache[i].glyph = nullptr;
  }

  _glyphs = nullptr;
  _intervals = nullptr;
  _glyphCount = 0;
  _glyphsSize = 0;
  _intervalsSize = 0;
  _bitmapOffset = 0;
  _isLoaded = false;
  _accessCounter = 0;
  _totalCacheAllocation = 0;
  _tombstoneCount = 0;
  _cacheHits = 0;
  _cacheMisses = 0;

  memset(&_fontData, 0, sizeof(_fontData));
}

IRAM_ATTR const EpdGlyph* StreamingEpdFont::lookupGlyph(uint32_t cp) const {
  // Check glyph cache first (O(1) for hot glyphs)
  const int cacheIdx = cp % GLYPH_CACHE_SIZE;
  if (_glyphCache[cacheIdx].codepoint == cp) {
    return _glyphCache[cacheIdx].glyph;
  }

  // Binary search in intervals (O(log n))
  const int count = _fontData.intervalCount;
  if (count == 0) return nullptr;

  int left = 0;
  int right = count - 1;

  while (left <= right) {
    const int mid = left + (right - left) / 2;
    const EpdUnicodeInterval* interval = &_intervals[mid];

    if (cp < interval->first) {
      right = mid - 1;
    } else if (cp > interval->last) {
      left = mid + 1;
    } else {
      // Found: cp >= interval->first && cp <= interval->last
      const uint32_t glyphIdx = interval->offset + (cp - interval->first);
      if (glyphIdx >= _glyphCount) {
        return nullptr;  // Corrupted font data - index out of bounds
      }
      const EpdGlyph* glyph = &_glyphs[glyphIdx];
      // Store in cache
      _glyphCache[cacheIdx].codepoint = cp;
      _glyphCache[cacheIdx].glyph = glyph;
      return glyph;
    }
  }

  return nullptr;
}

IRAM_ATTR const EpdGlyph* StreamingEpdFont::getGlyph(uint32_t cp) {
  if (!_isLoaded) return nullptr;
  return lookupGlyph(cp);
}

int StreamingEpdFont::findInBitmapCache(uint32_t glyphIndex) {
  // O(1) hash table lookup with linear probing
  int hash = hashIndex(glyphIndex);
  for (int i = 0; i < CACHE_SIZE; i++) {
    int idx = (hash + i) % CACHE_SIZE;
    int16_t cacheIdx = _hashTable[idx];
    if (cacheIdx == HASH_EMPTY) {
      return -1;
    }
    if (cacheIdx == HASH_TOMBSTONE) {
      continue;
    }
    if (_cache[cacheIdx].glyphIndex == glyphIndex) {
      return cacheIdx;
    }
  }
  return -1;
}

int StreamingEpdFont::getLruSlot() {
  int lruIndex = 0;
  uint32_t minUsed = _cache[0].lastUsed;

  for (int i = 1; i < CACHE_SIZE; i++) {
    // Prefer unused slots
    if (_cache[i].glyphIndex == INVALID_CODEPOINT) {
      return i;
    }
    if (_cache[i].lastUsed < minUsed) {
      minUsed = _cache[i].lastUsed;
      lruIndex = i;
    }
  }
  return lruIndex;
}

bool StreamingEpdFont::readGlyphBitmap(uint32_t glyphIndex, uint8_t* buf, uint16_t bufLen) {
  if (!_fontFile || glyphIndex >= _glyphCount || !buf) {
    return false;
  }

  const EpdGlyph& glyph = _glyphs[glyphIndex];
  const uint16_t dataLen = glyph.dataLength;

  if (dataLen == 0 || dataLen > MAX_GLYPH_BITMAP_SIZE || dataLen > bufLen) {
    return false;
  }

  // Calculate file position: bitmapOffset + glyph's dataOffset.
  // Retry seek+read on transient SD card failures (file handle stays valid).
  const uint32_t filePos = _bitmapOffset + glyph.dataOffset;
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) delay(50);
    snapix::spi::SharedBusLock lk;
    if (!lk) continue;
    if (!_fontFile.seek(filePos)) continue;
    if (_fontFile.read(buf, dataLen) == dataLen) return true;
  }

  return false;
}

const uint8_t* StreamingEpdFont::getGlyphBitmap(const EpdGlyph* glyph) {
  if (!_isLoaded || !glyph || !_pool) return nullptr;

  // Validate glyph pointer belongs to this font instance (defense against wrong font)
  if (glyph < _glyphs || glyph >= _glyphs + _glyphCount) {
    return nullptr;
  }

  // Calculate glyph index from pointer arithmetic (now safe after validation)
  const uint32_t glyphIndex = glyph - _glyphs;
  const uint16_t dataLen = _glyphs[glyphIndex].dataLength;
  if (dataLen == 0 || dataLen > MAX_GLYPH_BITMAP_SIZE) return nullptr;

  // Hot path: cache hit.  Returns a pointer into the stable pool slice.
  int cacheIndex = findInBitmapCache(glyphIndex);
  if (cacheIndex >= 0) {
    _cache[cacheIndex].lastUsed = ++_accessCounter;
    _cacheHits++;
    return slotBuffer(cacheIndex);
  }

  _cacheMisses++;

  // Glyphs whose bitmap exceeds POOL_SLOT_SIZE bypass the cache and render
  // through a single shared scratch buffer.  No LRU storage, no caching —
  // the renderer reads the returned pointer for one drawText pass and
  // doesn't keep it.  Rare in practice (only large CJK glyphs).
  if (dataLen > POOL_SLOT_SIZE) {
    if (!_scratch) {
      _scratch = new (std::nothrow) uint8_t[MAX_GLYPH_BITMAP_SIZE];
      if (!_scratch) return nullptr;
    }
    if (!readGlyphBitmap(glyphIndex, _scratch, MAX_GLYPH_BITMAP_SIZE)) {
      return nullptr;
    }
    return _scratch;
  }

  // Cache miss within slot capacity — pick LRU slot, overwrite its pool slice.
  int slot = getLruSlot();

  // If we are evicting a live entry, mark its hash entry as tombstone so
  // future lookups on its codepoint correctly miss.
  if (_cache[slot].glyphIndex != INVALID_CODEPOINT) {
    int oldHash = hashIndex(_cache[slot].glyphIndex);
    for (int i = 0; i < CACHE_SIZE; i++) {
      int idx = (oldHash + i) % CACHE_SIZE;
      if (_hashTable[idx] == slot) {
        _hashTable[idx] = HASH_TOMBSTONE;
        _tombstoneCount++;
        break;
      }
    }
    if (_tombstoneCount >= TOMBSTONE_REHASH_THRESHOLD) {
      rehashTable();
    }
  }

  // Read directly into the slot's fixed pool slice.  No allocation, no
  // realloc — the slice was carved out at font load.
  if (!readGlyphBitmap(glyphIndex, slotBuffer(slot), POOL_SLOT_SIZE)) {
    // Read failed.  Mark slot empty so we don't return stale data on next hit.
    _cache[slot].glyphIndex = INVALID_CODEPOINT;
    _cache[slot].bitmapSize = 0;
    return nullptr;
  }

  _cache[slot].glyphIndex = glyphIndex;
  _cache[slot].bitmapSize = dataLen;
  _cache[slot].lastUsed = ++_accessCounter;

  // Live bytes used across all slots — for memory profiling.
  size_t totalAllocation = 0;
  for (int i = 0; i < CACHE_SIZE; i++) {
    totalAllocation += _cache[i].bitmapSize;
  }
  _totalCacheAllocation = totalAllocation;

  // Add to hash table
  int hash = hashIndex(glyphIndex);
  bool inserted = false;
  for (int i = 0; i < CACHE_SIZE; i++) {
    int idx = (hash + i) % CACHE_SIZE;
    if (_hashTable[idx] == HASH_EMPTY || _hashTable[idx] == HASH_TOMBSTONE) {
      _hashTable[idx] = slot;
      inserted = true;
      break;
    }
  }

  // If hash table is full (should not happen with proper LRU eviction), force rehash
  if (!inserted) {
    rehashTable();
    // Re-insert after rehash
    hash = hashIndex(glyphIndex);
    for (int i = 0; i < CACHE_SIZE; i++) {
      int idx = (hash + i) % CACHE_SIZE;
      if (_hashTable[idx] == HASH_EMPTY) {
        _hashTable[idx] = slot;
        break;
      }
    }
  }

  return slotBuffer(slot);
}

void StreamingEpdFont::rehashTable() {
  // Clear the hash table
  for (int i = 0; i < CACHE_SIZE; i++) {
    _hashTable[i] = HASH_EMPTY;
  }
  _tombstoneCount = 0;

  // Re-insert all valid cache entries
  for (int slot = 0; slot < CACHE_SIZE; slot++) {
    if (_cache[slot].glyphIndex != INVALID_CODEPOINT) {
      int hash = hashIndex(_cache[slot].glyphIndex);
      for (int i = 0; i < CACHE_SIZE; i++) {
        int idx = (hash + i) % CACHE_SIZE;
        if (_hashTable[idx] == HASH_EMPTY) {
          _hashTable[idx] = slot;
          break;
        }
      }
    }
  }
}

void StreamingEpdFont::getTextDimensions(const char* string, int* w, int* h) const {
  int minX = 0, minY = 0, maxX = 0, maxY = 0;
  int cursorX = 0;
  const int cursorY = 0;
  int lastBaseX = 0;
  int lastBaseAdvance = 0;

  if (!string || *string == '\0') {
    *w = 0;
    *h = 0;
    return;
  }

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&string)))) {
    const EpdGlyph* glyph = lookupGlyph(cp);
    if (!glyph) {
      glyph = lookupGlyph('?');
    }
    if (!glyph) {
      continue;
    }

    if (utf8IsCombiningMark(cp)) {
      const int centerX = lastBaseX + lastBaseAdvance / 2 - glyph->width / 2;
      minX = std::min(minX, centerX + glyph->left);
      maxX = std::max(maxX, centerX + glyph->left + glyph->width);
      minY = std::min(minY, cursorY + glyph->top - glyph->height);
      maxY = std::max(maxY, cursorY + glyph->top);
    } else {
      minX = std::min(minX, cursorX + glyph->left);
      maxX = std::max(maxX, cursorX + glyph->left + glyph->width);
      minY = std::min(minY, cursorY + glyph->top - glyph->height);
      maxY = std::max(maxY, cursorY + glyph->top);
      lastBaseX = cursorX;
      lastBaseAdvance = glyph->advanceX;
      cursorX += glyph->advanceX;
    }
  }

  *w = maxX - minX;
  *h = maxY - minY;
}

bool StreamingEpdFont::hasPrintableChars(const char* string) const {
  int w = 0, h = 0;
  getTextDimensions(string, &w, &h);
  return w > 0 || h > 0;
}

size_t StreamingEpdFont::getMemoryUsage() const {
  size_t usage = sizeof(StreamingEpdFont);
  usage += _glyphsSize;
  usage += _intervalsSize;
  // Pool is one fixed-size allocation that lives for the font's lifetime.
  if (_pool) usage += POOL_BYTES;
  if (_scratch) usage += MAX_GLYPH_BITMAP_SIZE;
  return usage;
}

void StreamingEpdFont::logCacheStats() const {
  // No-op: debug logging removed
}
