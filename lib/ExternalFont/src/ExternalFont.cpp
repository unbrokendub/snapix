#include "ExternalFont.h"

#include <Logging.h>
#include <SharedSpiLock.h>

#if __has_include(<esp_attr.h>)
#include <esp_attr.h>
#endif
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

#define TAG "EXT_FONT"

#include <algorithm>
#include <cstring>
#include <vector>

ExternalFont::~ExternalFont() { unload(); }

void ExternalFont::unload() {
  if (_fontFile) {
    _fontFile.close();
  }
  _isLoaded = false;
  _fontName[0] = '\0';
  _fontSize = 0;
  _charWidth = 0;
  _charHeight = 0;
  _bytesPerRow = 0;
  _bytesPerChar = 0;
  _accessCounter = 0;

  // Free bitmap pool
  if (_bitmapPool) {
    free(_bitmapPool);
    _bitmapPool = nullptr;
  }

  // Clear cache metadata and hash table
  for (int i = 0; i < CACHE_SIZE; i++) {
    _cache[i].codepoint = 0xFFFFFFFF;
    _cache[i].lastUsed = 0;
    _cache[i].notFound = false;
    _hashTable[i] = HASH_EMPTY;
  }
}

bool ExternalFont::parseFilename(const char* filepath) {
  // Extract filename from path
  const char* filename = strrchr(filepath, '/');
  if (filename) {
    filename++;  // Skip '/'
  } else {
    filename = filepath;
  }

  // Parse format: FontName_size_WxH.bin
  // Example: KingHwaOldSong_38_33x39.bin

  char nameCopy[64];
  strncpy(nameCopy, filename, sizeof(nameCopy) - 1);
  nameCopy[sizeof(nameCopy) - 1] = '\0';

  // Remove .bin extension
  char* ext = strstr(nameCopy, ".bin");
  if (!ext) {
    LOG_ERR(TAG, "Invalid filename: no .bin extension");
    return false;
  }
  *ext = '\0';

  // Find _WxH part from the end
  char* lastUnderscore = strrchr(nameCopy, '_');
  if (!lastUnderscore) {
    LOG_ERR(TAG, "Invalid filename format");
    return false;
  }

  // Parse WxH
  int w, h;
  if (sscanf(lastUnderscore + 1, "%dx%d", &w, &h) != 2) {
    LOG_ERR(TAG, "Failed to parse dimensions");
    return false;
  }
  _charWidth = (uint8_t)w;
  _charHeight = (uint8_t)h;

  // Validate dimensions
  static constexpr uint8_t MAX_CHAR_DIM = 64;
  if (_charWidth > MAX_CHAR_DIM || _charHeight > MAX_CHAR_DIM) {
    LOG_ERR(TAG, "Dimensions too large: %dx%d (max %d). Using default font.", _charWidth, _charHeight, MAX_CHAR_DIM);
    return false;
  }

  *lastUnderscore = '\0';

  // Find size (supports both "38" and "px30" notation)
  lastUnderscore = strrchr(nameCopy, '_');
  if (!lastUnderscore) {
    LOG_ERR(TAG, "Invalid filename format: no size");
    return false;
  }

  int size;
  const char* sizeStr = lastUnderscore + 1;
  if (strncmp(sizeStr, "px", 2) == 0) {
    if (sscanf(sizeStr + 2, "%d", &size) != 1) {
      LOG_ERR(TAG, "Failed to parse pixel-height size");
      return false;
    }
  } else if (sscanf(sizeStr, "%d", &size) != 1) {
    LOG_ERR(TAG, "Failed to parse size");
    return false;
  }
  _fontSize = (uint8_t)size;
  *lastUnderscore = '\0';

  // Remaining part is font name
  strncpy(_fontName, nameCopy, sizeof(_fontName) - 1);
  _fontName[sizeof(_fontName) - 1] = '\0';

  // Calculate bytes per char
  _bytesPerRow = (_charWidth + 7) / 8;
  _bytesPerChar = _bytesPerRow * _charHeight;

  if (_bytesPerChar > MAX_GLYPH_BYTES) {
    LOG_ERR(TAG, "Glyph too large: %d bytes (max %d)", _bytesPerChar, MAX_GLYPH_BYTES);
    return false;
  }

  LOG_INF(TAG, "Parsed: name=%s, size=%d, %dx%d, %d bytes/char", _fontName, _fontSize, _charWidth, _charHeight,
          _bytesPerChar);

  return true;
}

bool ExternalFont::load(const char* filepath) {
  unload();

  if (!parseFilename(filepath)) {
    return false;
  }

  {
    papyrix::spi::SharedBusLock busLock;
    if (!SdMan.openFileForRead("EXT_FONT", filepath, _fontFile)) {
      LOG_ERR(TAG, "Failed to open: %s", filepath);
      return false;
    }
  }

  // Validate file size
  static constexpr uint32_t MAX_FONT_FILE_SIZE = 32 * 1024 * 1024;  // 32MB max
  uint32_t fileSize = _fontFile.size();
  if (fileSize == 0 || fileSize > MAX_FONT_FILE_SIZE) {
    LOG_ERR(TAG, "Invalid file size: %u bytes (max 32MB). Using default font.", fileSize);
    _fontFile.close();
    return false;
  }

  // Allocate contiguous bitmap pool sized to actual glyph dimensions.
  // For a 20x22 font this is 80*66 = 5,280 bytes instead of the old
  // 80*256 = 20,480 bytes — saving ~15KB of heap.
  const size_t poolSize = static_cast<size_t>(CACHE_SIZE) * _bytesPerChar;
  _bitmapPool = static_cast<uint8_t*>(malloc(poolSize));
  if (!_bitmapPool) {
    LOG_ERR(TAG, "Failed to allocate glyph bitmap pool (%zu bytes)", poolSize);
    _fontFile.close();
    return false;
  }
  memset(_bitmapPool, 0, poolSize);

  _isLoaded = true;
  LOG_INF(TAG, "Loaded: %s (cache pool %zuB = %d slots x %d B/glyph)", filepath, poolSize, CACHE_SIZE, _bytesPerChar);
  return true;
}

IRAM_ATTR int ExternalFont::findInCache(uint32_t codepoint) {
  // O(1) hash table lookup with linear probing for collisions
  int hash = hashCodepoint(codepoint);
  for (int i = 0; i < CACHE_SIZE; i++) {
    int idx = (hash + i) % CACHE_SIZE;
    int16_t cacheIdx = _hashTable[idx];
    if (cacheIdx == HASH_EMPTY) {
      // Empty slot (never used) - entry not in table
      return -1;
    }
    if (cacheIdx == HASH_TOMBSTONE) {
      // Deleted slot - continue probing
      continue;
    }
    if (_cache[cacheIdx].codepoint == codepoint) {
      return cacheIdx;
    }
  }
  return -1;
}

int ExternalFont::getLruSlot() {
  int lruIndex = 0;
  uint32_t minUsed = _cache[0].lastUsed;

  for (int i = 1; i < CACHE_SIZE; i++) {
    // Prefer unused slots
    if (_cache[i].codepoint == 0xFFFFFFFF) {
      return i;
    }
    if (_cache[i].lastUsed < minUsed) {
      minUsed = _cache[i].lastUsed;
      lruIndex = i;
    }
  }
  return lruIndex;
}

bool ExternalFont::readGlyphFromSD(uint32_t codepoint, uint8_t* buffer) {
  return readGlyphAtOffset(codepoint * _bytesPerChar, buffer);
}

bool ExternalFont::readGlyphAtOffset(uint32_t offset, uint8_t* buffer) {
  if (!_fontFile) {
    return false;
  }

  papyrix::spi::SharedBusLock busLock;
  if (!busLock) {
    return false;
  }

  if (_fontFile.position() != offset && !_fontFile.seek(offset)) {
    return false;
  }

  size_t bytesRead = _fontFile.read(buffer, _bytesPerChar);
  if (bytesRead != _bytesPerChar) {
    // May be end of file or other error, fill with zeros
    memset(buffer, 0, _bytesPerChar);
  }

  return true;
}

void ExternalFont::removeHashEntryForSlot(const int slot) {
  if (_cache[slot].codepoint == 0xFFFFFFFF) {
    return;
  }

  const int oldHash = hashCodepoint(_cache[slot].codepoint);
  for (int i = 0; i < CACHE_SIZE; i++) {
    const int idx = (oldHash + i) % CACHE_SIZE;
    if (_hashTable[idx] == slot) {
      _hashTable[idx] = HASH_TOMBSTONE;
      return;
    }
  }
}

void ExternalFont::insertHashEntry(const uint32_t codepoint, const int slot) {
  const int hash = hashCodepoint(codepoint);
  for (int i = 0; i < CACHE_SIZE; i++) {
    const int idx = (hash + i) % CACHE_SIZE;
    if (_hashTable[idx] == HASH_EMPTY || _hashTable[idx] == HASH_TOMBSTONE) {
      _hashTable[idx] = slot;
      return;
    }
  }
}

void ExternalFont::finalizeCacheEntry(const uint32_t codepoint, const int slot, const bool readSuccess) {
  // Calculate metrics and check if glyph is empty
  uint8_t minX = _charWidth;
  uint8_t maxX = 0;
  bool isEmpty = true;

  const uint8_t* bmp = slotBitmap(slot);
  if (readSuccess && _bytesPerChar > 0) {
    for (int y = 0; y < _charHeight; y++) {
      for (int x = 0; x < _charWidth; x++) {
        const int byteIndex = y * _bytesPerRow + (x / 8);
        const int bitIndex = 7 - (x % 8);
        if ((bmp[byteIndex] >> bitIndex) & 1) {
          isEmpty = false;
          if (x < minX) minX = x;
          if (x > maxX) maxX = x;
        }
      }
    }
  }

  _cache[slot].codepoint = codepoint;
  _cache[slot].lastUsed = ++_accessCounter;

  // Check if this is a whitespace character (U+2000-U+200F: various spaces, U+3000: ideographic space)
  const bool isWhitespace = (codepoint >= 0x2000 && codepoint <= 0x200F) || codepoint == 0x3000;

  // Mark as notFound only if read failed or (empty AND not whitespace)
  // Whitespace characters are expected to be empty but should still be rendered
  // Empty ASCII slots (from --cjk-only fonts) also marked notFound so they fall through to builtin
  _cache[slot].notFound = !readSuccess || (isEmpty && !isWhitespace);

  if (!isEmpty) {
    _cache[slot].minX = minX;
    _cache[slot].advanceX = (maxX - minX + 1) + 2;
  } else {
    _cache[slot].minX = 0;
    if (isWhitespace) {
      if (codepoint == 0x2003) {
        _cache[slot].advanceX = _charWidth;
      } else if (codepoint == 0x2002) {
        _cache[slot].advanceX = _charWidth / 2;
      } else if (codepoint == 0x3000) {
        _cache[slot].advanceX = _charWidth;
      } else {
        _cache[slot].advanceX = _charWidth / 3;
      }
    } else {
      _cache[slot].advanceX = _charWidth / 3;
    }
  }
}

const uint8_t* ExternalFont::loadGlyphIntoSlot(const uint32_t codepoint, const int slot) {
  removeHashEntryForSlot(slot);

  const bool readSuccess = readGlyphFromSD(codepoint, slotBitmap(slot));
  finalizeCacheEntry(codepoint, slot, readSuccess);
  insertHashEntry(codepoint, slot);

  return _cache[slot].notFound ? nullptr : slotBitmap(slot);
}

IRAM_ATTR const uint8_t* ExternalFont::getGlyph(uint32_t codepoint) {
  if (!_isLoaded) {
    return nullptr;
  }

  // First check cache (O(1) with hash table)
  int cacheIndex = findInCache(codepoint);
  if (cacheIndex >= 0) {
    _cache[cacheIndex].lastUsed = ++_accessCounter;
    // Return nullptr if this codepoint was previously marked as not found
    if (_cache[cacheIndex].notFound) {
      return nullptr;
    }
    return slotBitmap(cacheIndex);
  }

  // Cache miss, need to read from SD card
  return loadGlyphIntoSlot(codepoint, getLruSlot());
}

bool ExternalFont::getGlyphMetrics(uint32_t codepoint, uint8_t* outMinX, uint8_t* outAdvanceX) {
  int idx = findInCache(codepoint);
  if (idx >= 0 && !_cache[idx].notFound) {
    if (outMinX) *outMinX = _cache[idx].minX;
    if (outAdvanceX) *outAdvanceX = _cache[idx].advanceX;
    return true;
  }
  return false;
}

void ExternalFont::preloadGlyphs(const uint32_t* codepoints, size_t count) {
  if (!_isLoaded || !codepoints || count == 0) {
    return;
  }

  // Create a sorted copy for sequential SD card access.
  // Sorting first lets us dedupe before applying cache-size limits.
  std::vector<uint32_t> sorted(codepoints, codepoints + count);
  std::sort(sorted.begin(), sorted.end());

  // Remove duplicates
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  if (sorted.size() > static_cast<size_t>(CACHE_SIZE)) {
    sorted.resize(CACHE_SIZE);
  }

  LOG_INF(TAG, "Preloading %zu unique glyphs", sorted.size());
  const unsigned long startTime = millis();

  size_t loaded = 0;
  size_t skipped = 0;

  for (uint32_t cp : sorted) {
    // Skip if already in cache
    if (findInCache(cp) >= 0) {
      skipped++;
      continue;
    }

    const int slot = getLruSlot();
    loadGlyphIntoSlot(cp, slot);
    loaded++;
  }

  LOG_INF(TAG, "Preload done: %zu loaded, %zu already cached, took %lums", loaded, skipped, millis() - startTime);
}

void ExternalFont::logCacheStats() const {
  int used = 0;
  for (int i = 0; i < CACHE_SIZE; i++) {
    if (_cache[i].codepoint != 0xFFFFFFFF) used++;
  }
  const size_t usedBytes = used * (sizeof(CacheEntry) + _bytesPerChar);
  const size_t totalBytes = getCacheMemorySize();
  LOG_DBG(TAG, "Cache: %d/%d slots used (%zuB active / %zuB total, %dB/glyph)",
          used, CACHE_SIZE, usedBytes, totalBytes, _bytesPerChar);
}
