#pragma once

#include <SDCardManager.h>

#include <cstdint>

/**
 * External font loader - supports Xteink .bin format
 * Filename format: FontName_size_WxH.bin (e.g. KingHwaOldSong_38_33x39.bin)
 * Also supports pixel-height notation: FontName_px30_WxH.bin
 *
 * Font format:
 * - Direct Unicode codepoint indexing
 * - Offset = codepoint * bytesPerChar
 * - Each char = bytesPerRow * charHeight bytes
 * - 1-bit black/white bitmap, MSB first
 */
class ExternalFont {
 public:
  ExternalFont() {
    for (int i = 0; i < CACHE_SIZE; i++) {
      _hashTable[i] = HASH_EMPTY;
    }
  }
  ~ExternalFont();

  // Disable copy
  ExternalFont(const ExternalFont&) = delete;
  ExternalFont& operator=(const ExternalFont&) = delete;

  /**
   * Load font from .bin file
   * @param filepath Full path on SD card (e.g.
   * "/fonts/KingHwaOldSong_38_33x39.bin")
   * @return true on success
   */
  bool load(const char* filepath);

  /**
   * Get glyph bitmap data (with LRU cache)
   * @param codepoint Unicode codepoint
   * @return Bitmap data pointer, nullptr if char not found
   */
  const uint8_t* getGlyph(uint32_t codepoint);

  /**
   * Preload multiple glyphs at once (optimized for batch SD reads)
   * Call this before rendering a chapter to warm up the cache
   * @param codepoints Array of unicode codepoints to preload
   * @param count Number of codepoints in the array
   */
  void preloadGlyphs(const uint32_t* codepoints, size_t count);

  // Font properties
  uint8_t getCharWidth() const { return _charWidth; }
  uint8_t getCharHeight() const { return _charHeight; }
  uint8_t getBytesPerRow() const { return _bytesPerRow; }
  uint16_t getBytesPerChar() const { return _bytesPerChar; }
  const char* getFontName() const { return _fontName; }
  uint8_t getFontSize() const { return _fontSize; }

  bool isLoaded() const { return _isLoaded; }
  void unload();

  /**
   * Get cached metrics for a glyph.
   * Must call getGlyph() first to ensure it's loaded!
   * @param cp Unicode codepoint
   * @param outMinX Minimum X offset (left bearing)
   * @param outAdvanceX Advance width for cursor positioning
   * @return true if metrics found in cache, false otherwise
   */
  bool getGlyphMetrics(uint32_t cp, uint8_t* outMinX, uint8_t* outAdvanceX);

  /**
   * Log cache statistics for debugging
   */
  void logCacheStats() const;

  /**
   * Get the configured cache size (compile-time constant).
   * Useful for memory profiling.
   */
  static constexpr int getCacheSize() { return CACHE_SIZE; }

  /**
   * Get the approximate cache memory usage in bytes for the current font.
   * Returns metadata (always allocated) + bitmap pool (allocated after load).
   */
  size_t getCacheMemorySize() const {
    // Metadata: CacheEntry array + hash table (always present)
    constexpr size_t metaSize = CACHE_SIZE * (sizeof(CacheEntry) + sizeof(int16_t));
    // Bitmap pool: allocated after load() with actual _bytesPerChar
    const size_t poolSize = _bitmapPool ? static_cast<size_t>(CACHE_SIZE) * _bytesPerChar : 0;
    return metaSize + poolSize;
  }

 private:
  // Font file handle (keep open to avoid repeated open/close)
  FsFile _fontFile;
  bool _isLoaded = false;

  // Properties parsed from filename
  char _fontName[32] = {0};
  uint8_t _fontSize = 0;
  uint8_t _charWidth = 0;
  uint8_t _charHeight = 0;
  uint8_t _bytesPerRow = 0;
  uint16_t _bytesPerChar = 0;

  // LRU cache configuration for CJK glyph caching
  // Trade-off: larger cache = better performance with CJK text, but more RAM usage
  //
  // Memory usage: CACHE_SIZE * (12 bytes metadata + _bytesPerChar bitmap)
  //   Bitmap pool is allocated once in load() at the actual glyph size, not a
  //   fixed 256-byte maximum.  For a 20x22 font (_bytesPerChar=66) this saves
  //   ~15KB vs the old fixed-slot layout.
  //
  //   - 256 entries, 33x39 font = 256*(12+195) = ~52KB (was ~69KB)
  //   - 80 entries, 33x39 font  = 80*(12+195) = ~16KB (was ~22KB)
  //   - 80 entries, 20x22 font  = 80*(12+66)  = ~6KB  (was ~22KB)
  //
  // To reduce cache slot count, change EXTERNAL_FONT_CACHE_SIZE before including
  // this header, or modify the default below.
#ifndef EXTERNAL_FONT_CACHE_SIZE
#define EXTERNAL_FONT_CACHE_SIZE 80
#endif
  static constexpr int CACHE_SIZE = EXTERNAL_FONT_CACHE_SIZE;
  static constexpr int MAX_GLYPH_BYTES = 256;  // Hard limit on _bytesPerChar (supports up to ~45px height)

  // Cache entry metadata — no bitmap storage here; bitmaps live in a
  // separate contiguous pool (_bitmapPool) sized to the actual _bytesPerChar.
  struct CacheEntry {
    uint32_t codepoint = 0xFFFFFFFF;  // Invalid marker
    uint32_t lastUsed = 0;
    bool notFound = false;  // True if glyph doesn't exist in font
    uint8_t minX = 0;       // Cached rendering metrics
    uint8_t advanceX = 0;   // Cached advance width
  };
  CacheEntry _cache[CACHE_SIZE] = {};
  // Contiguous bitmap pool: CACHE_SIZE * _bytesPerChar bytes.
  // Allocated once in load(), freed in unload().  Slot i's bitmap starts at
  // _bitmapPool + i * _bytesPerChar.
  uint8_t* _bitmapPool = nullptr;
  uint32_t _accessCounter = 0;

  /// Return pointer to bitmap data for a given cache slot.
  uint8_t* slotBitmap(int slot) { return _bitmapPool + slot * _bytesPerChar; }
  const uint8_t* slotBitmap(int slot) const { return _bitmapPool + slot * _bytesPerChar; }

  // Hash table for O(1) cache lookup (codepoint -> cache index)
  // Values: -1 = empty (never used), -2 = tombstone (deleted), >= 0 = cache index
  static constexpr int16_t HASH_EMPTY = -1;
  static constexpr int16_t HASH_TOMBSTONE = -2;
  int16_t _hashTable[CACHE_SIZE] = {};
  static int hashCodepoint(uint32_t cp) { return cp % CACHE_SIZE; }

  /**
   * Read glyph data from SD card
   */
  bool readGlyphFromSD(uint32_t codepoint, uint8_t* buffer);

  /**
   * Read glyph data from SD card at an already computed offset.
   * Reuses sequential file position when possible to avoid redundant seeks.
   */
  bool readGlyphAtOffset(uint32_t offset, uint8_t* buffer);

  /**
   * Parse filename to get font parameters
   * Format: FontName_size_WxH.bin
   */
  bool parseFilename(const char* filename);

  /**
   * Find glyph in cache
   * @return Cache index, -1 if not found
   */
  int findInCache(uint32_t codepoint);

  /**
   * Get LRU cache slot (least recently used)
   */
  int getLruSlot();

  /**
   * Remove a cache slot from the hash table if it is currently indexed there.
   */
  void removeHashEntryForSlot(int slot);

  /**
   * Insert a cache slot into the hash table.
   */
  void insertHashEntry(uint32_t codepoint, int slot);

  /**
   * Populate cache entry metrics and notFound flag after bitmap load.
   */
  void finalizeCacheEntry(uint32_t codepoint, int slot, bool readSuccess);

  /**
   * Load a glyph into a specific cache slot.
   */
  const uint8_t* loadGlyphIntoSlot(uint32_t codepoint, int slot);
};
