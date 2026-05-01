#pragma once

#include <SDCardManager.h>

#include <cstdint>

#include "EpdFontData.h"

/**
 * Streaming font loader for .epdfont files.
 *
 * Unlike EpdFont which loads the entire bitmap into RAM (~50-100KB),
 * StreamingEpdFont keeps the file open and streams glyph bitmaps on demand
 * with an LRU cache (~10-25KB total).
 *
 * Memory comparison for typical 50KB font:
 *   - EpdFont: ~70KB (intervals + glyphs + bitmap all in RAM)
 *   - StreamingEpdFont: ~25KB (intervals + glyphs + cache only)
 *
 * Trade-off: Slightly slower glyph access (SD card reads on cache miss)
 *            but significantly lower RAM usage.
 */
class StreamingEpdFont {
 public:
  StreamingEpdFont();
  ~StreamingEpdFont();

  StreamingEpdFont(const StreamingEpdFont&) = delete;
  StreamingEpdFont& operator=(const StreamingEpdFont&) = delete;

  /**
   * Load font from .epdfont file (streaming mode).
   * Loads intervals and glyph table into RAM, keeps file open for bitmap streaming.
   *
   * @param path Path to .epdfont file on SD card
   * @return true on success
   */
  bool load(const char* path);

  /**
   * Unload font and free all resources.
   */
  void unload();

  /**
   * Check if font is loaded and ready.
   */
  bool isLoaded() const { return _isLoaded; }

  /**
   * Get glyph data for a unicode codepoint.
   * Uses LRU cache - may read from SD card on cache miss.
   *
   * @param cp Unicode codepoint
   * @return Pointer to glyph data, or nullptr if not found
   */
  const EpdGlyph* getGlyph(uint32_t cp);

  /**
   * Get glyph bitmap data for a glyph.
   * This is the actual bitmap pixels, streamed from SD or cache.
   *
   * @param glyph Pointer to glyph obtained from this font's getGlyph() method.
   *              Must not be a glyph pointer from a different font instance.
   * @return Pointer to bitmap data, or nullptr on error
   */
  const uint8_t* getGlyphBitmap(const EpdGlyph* glyph);

  /**
   * Calculate text dimensions without rendering.
   */
  void getTextDimensions(const char* string, int* w, int* h) const;

  /**
   * Check if string contains any printable characters.
   */
  bool hasPrintableChars(const char* string) const;

  /**
   * Get font data structure (for compatibility with EpdFont interface).
   * Note: The bitmap pointer is nullptr in streaming mode.
   */
  const EpdFontData* getData() const { return &_fontData; }

  // Font metrics accessors
  uint8_t getAdvanceY() const { return _fontData.advanceY; }
  int getAscender() const { return _fontData.ascender; }
  int getDescender() const { return _fontData.descender; }
  bool is2Bit() const { return _fontData.is2Bit; }

  /**
   * Get total RAM usage of this font instance.
   * Includes intervals, glyphs, and cache.
   */
  size_t getMemoryUsage() const;

  /**
   * Free all cached glyph bitmaps WITHOUT unloading the font.
   *
   * The intervals + glyph table + open file handle remain in RAM, so the font
   * stays usable.  Frees the up to CACHE_SIZE scattered bitmap allocations
   * that accumulate across book transitions and fragment the heap.
   *
   * Call this on book switch (when the font is preserved via sameFont) to
   * defragment the heap without paying the cold-load penalty of a full
   * unload + reload.
   */
  void clearBitmapCache();

  /**
   * Log cache statistics for debugging.
   */
  void logCacheStats() const;

  /**
   * Get the configured cache size.
   */
  static constexpr int getCacheSize() { return CACHE_SIZE; }

 private:
  // Cache configuration — pool-allocated to eliminate heap fragmentation.
  //
  // Previous design used CACHE_SIZE separate `new uint8_t[size]` allocations
  // for each cached glyph bitmap.  On Cyrillic / extended-Latin pages with
  // 70-100 unique glyphs, the resulting hundred-odd small allocations
  // pulverised the heap into 100-byte fragments — heap.largest dropped from
  // ~65 KB to ~7 KB during a single first-render, blocking the background
  // cache extender ('skip background cache: heap critical') and stretching
  // page-0 of subsequent books to 10-30 seconds.
  //
  // Current design: one contiguous 32 KB pool allocated per font instance.
  // Each cache slot owns a fixed POOL_SLOT_SIZE-byte slice indexed by
  // (slot_index * POOL_SLOT_SIZE).  LRU eviction overwrites the slice
  // in-place — zero per-glyph allocations, zero heap fragmentation.
  //
  // Glyphs whose bitmap exceeds POOL_SLOT_SIZE (rare: only large CJK) bypass
  // the cache and render through a single scratch buffer.  Rendering still
  // works, but those glyphs read SD on every access.
  static constexpr size_t POOL_BYTES = 32 * 1024;       // 32 KB single contiguous allocation
  static constexpr uint16_t POOL_SLOT_SIZE = 384;       // bytes per slot — covers 2-bit Cyrillic / Latin glyphs
  static constexpr int CACHE_SIZE = POOL_BYTES / POOL_SLOT_SIZE;  // 85 slots
  static constexpr uint32_t INVALID_CODEPOINT = 0xFFFFFFFF;

  // Maximum allowed glyph bitmap size (defense against corrupted font files,
  // also the size of the scratch buffer for rare oversized glyphs).
  static constexpr uint16_t MAX_GLYPH_BITMAP_SIZE = 4096;

  // Hash table markers
  static constexpr int16_t HASH_EMPTY = -1;
  static constexpr int16_t HASH_TOMBSTONE = -2;

  // Rehash when tombstones exceed 25% of table size to maintain O(1) lookup
  static constexpr int TOMBSTONE_REHASH_THRESHOLD = CACHE_SIZE / 4;

  // Font metadata (in RAM)
  EpdFontData _fontData;
  EpdGlyph* _glyphs = nullptr;
  EpdUnicodeInterval* _intervals = nullptr;
  uint32_t _glyphCount = 0;

  // File handle (kept open for streaming)
  FsFile _fontFile;
  uint32_t _bitmapOffset = 0;  // File offset where bitmap data starts
  bool _isLoaded = false;

  // Memory tracking
  size_t _glyphsSize = 0;
  size_t _intervalsSize = 0;

  // Pool storage — single contiguous allocation, lives for the lifetime
  // of the font instance.  Slot N's bitmap data lives at _pool + N*POOL_SLOT_SIZE.
  uint8_t* _pool = nullptr;
  // Scratch buffer for glyphs whose bitmap exceeds POOL_SLOT_SIZE.  These
  // bypass the cache (no LRU storage), but rendering still works.  Allocated
  // lazily on first oversized glyph encounter.
  uint8_t* _scratch = nullptr;

  // LRU bitmap cache — slot metadata only, the bitmap data lives in _pool.
  struct CachedBitmap {
    uint32_t glyphIndex = INVALID_CODEPOINT;  // Which glyph this bitmap belongs to
    uint16_t bitmapSize = 0;                  // Actual bytes used in this slot's pool slice
    uint32_t lastUsed = 0;                    // For LRU eviction
  };
  CachedBitmap _cache[CACHE_SIZE];
  int16_t _hashTable[CACHE_SIZE];
  uint32_t _accessCounter = 0;
  size_t _totalCacheAllocation = 0;  // Live bytes used across cache slots (≤ POOL_BYTES)
  int _tombstoneCount = 0;           // Track tombstones to trigger rehashing

  // Cache statistics
  mutable uint32_t _cacheHits = 0;
  mutable uint32_t _cacheMisses = 0;

  // Glyph lookup cache (codepoint -> glyph pointer, for O(1) repeated lookups)
  static constexpr int GLYPH_CACHE_SIZE = 32;
  struct GlyphCacheEntry {
    uint32_t codepoint = INVALID_CODEPOINT;
    const EpdGlyph* glyph = nullptr;
  };
  mutable GlyphCacheEntry _glyphCache[GLYPH_CACHE_SIZE];

  // Helper methods
  static int hashIndex(uint32_t index) { return index % CACHE_SIZE; }
  int findInBitmapCache(uint32_t glyphIndex);
  int getLruSlot();
  // Read glyph bitmap from SD into the supplied buffer.  bufLen must be ≥ glyph dataLength.
  bool readGlyphBitmap(uint32_t glyphIndex, uint8_t* buf, uint16_t bufLen);
  // Pool slice for a given cache slot — stable for the font's lifetime.
  uint8_t* slotBuffer(int slotIdx) const { return _pool + slotIdx * POOL_SLOT_SIZE; }
  const EpdGlyph* lookupGlyph(uint32_t cp) const;
  void rehashTable();  // Rebuild hash table to clear tombstones
};
