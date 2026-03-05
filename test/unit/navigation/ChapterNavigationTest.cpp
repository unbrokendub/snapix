#include "test_utils.h"

#include <cstdint>

// ============================================================================
// Extract chapter navigation logic from ReaderState for unit testing.
// Tests calcFirstContentSpine() and the navigate-next/prev-chapter decisions.
// ============================================================================

// Inline calcFirstContentSpine (from ReaderState::calcFirstContentSpine)
static int calcFirstContentSpine(bool hasCover, int textStartIndex, size_t spineCount) {
  if (hasCover && textStartIndex == 0 && spineCount > 1) {
    return 1;
  }
  return textStartIndex;
}

// Minimal state struct mirroring ReaderState fields used by chapter navigation
struct ChapterNavState {
  int currentSpineIndex = 0;
  int currentSectionPage = 0;
  size_t spineCount = 10;
  bool hasCover = false;
  int textStartIndex = 0;
  bool needsRender = false;

  // Simulate navigateNextChapter logic from ReaderState
  void navigateNextChapter() {
    if (currentSpineIndex + 1 >= static_cast<int>(spineCount)) return;
    currentSpineIndex++;
    currentSectionPage = 0;
    needsRender = true;
  }

  // Simulate navigatePrevChapter logic from ReaderState
  void navigatePrevChapter() {
    if (currentSectionPage > 0) {
      currentSectionPage = 0;
      needsRender = true;
    } else {
      int firstContentSpine = calcFirstContentSpine(hasCover, textStartIndex, spineCount);
      if (currentSpineIndex <= firstContentSpine) {
        return;
      }
      currentSpineIndex--;
      currentSectionPage = 0;
      needsRender = true;
    }
  }
};

int main() {
  TestUtils::TestRunner runner("ChapterNavigation");

  // ============================================
  // calcFirstContentSpine tests
  // ============================================

  {
    int result = calcFirstContentSpine(false, 0, 10);
    runner.expectEq(0, result, "No cover, textStart=0: first content spine is 0");
  }

  {
    int result = calcFirstContentSpine(true, 0, 10);
    runner.expectEq(1, result, "Has cover, textStart=0, spineCount>1: skips cover to spine 1");
  }

  {
    int result = calcFirstContentSpine(true, 0, 1);
    runner.expectEq(0, result, "Has cover, textStart=0, spineCount=1: cannot skip, returns 0");
  }

  {
    int result = calcFirstContentSpine(true, 3, 10);
    runner.expectEq(3, result, "Has cover, textStart=3: returns textStartIndex directly");
  }

  {
    int result = calcFirstContentSpine(false, 2, 5);
    runner.expectEq(2, result, "No cover, textStart=2: returns textStartIndex");
  }

  // ============================================
  // navigateNextChapter tests
  // ============================================

  // Normal: advance from chapter 2 to chapter 3
  {
    ChapterNavState s;
    s.currentSpineIndex = 2;
    s.currentSectionPage = 5;
    s.spineCount = 10;
    s.navigateNextChapter();
    runner.expectEq(3, s.currentSpineIndex, "Next chapter: spine advances from 2 to 3");
    runner.expectEq(0, s.currentSectionPage, "Next chapter: page resets to 0");
    runner.expectTrue(s.needsRender, "Next chapter: needsRender is true");
  }

  // Boundary: at last chapter, should not advance
  {
    ChapterNavState s;
    s.currentSpineIndex = 9;
    s.spineCount = 10;
    s.navigateNextChapter();
    runner.expectEq(9, s.currentSpineIndex, "Next chapter at last spine: stays at 9");
    runner.expectFalse(s.needsRender, "Next chapter at last spine: needsRender stays false");
  }

  // Boundary: at second-to-last chapter, should advance to last
  {
    ChapterNavState s;
    s.currentSpineIndex = 8;
    s.spineCount = 10;
    s.navigateNextChapter();
    runner.expectEq(9, s.currentSpineIndex, "Next chapter from second-to-last: advances to 9");
    runner.expectTrue(s.needsRender, "Next chapter from second-to-last: needsRender is true");
  }

  // Single-chapter book
  {
    ChapterNavState s;
    s.currentSpineIndex = 0;
    s.spineCount = 1;
    s.navigateNextChapter();
    runner.expectEq(0, s.currentSpineIndex, "Next chapter in single-chapter book: stays at 0");
    runner.expectFalse(s.needsRender, "Next chapter in single-chapter book: needsRender stays false");
  }

  // ============================================
  // navigatePrevChapter tests
  // ============================================

  // Mid-chapter: go to beginning of current chapter
  {
    ChapterNavState s;
    s.currentSpineIndex = 3;
    s.currentSectionPage = 5;
    s.navigatePrevChapter();
    runner.expectEq(3, s.currentSpineIndex, "Prev chapter mid-page: spine unchanged");
    runner.expectEq(0, s.currentSectionPage, "Prev chapter mid-page: page resets to 0");
    runner.expectTrue(s.needsRender, "Prev chapter mid-page: needsRender is true");
  }

  // At page 0: go to previous chapter
  {
    ChapterNavState s;
    s.currentSpineIndex = 3;
    s.currentSectionPage = 0;
    s.spineCount = 10;
    s.navigatePrevChapter();
    runner.expectEq(2, s.currentSpineIndex, "Prev chapter from page 0: spine decrements to 2");
    runner.expectEq(0, s.currentSectionPage, "Prev chapter from page 0: page is 0");
    runner.expectTrue(s.needsRender, "Prev chapter from page 0: needsRender is true");
  }

  // At first content spine (no cover): should not go back
  {
    ChapterNavState s;
    s.currentSpineIndex = 0;
    s.currentSectionPage = 0;
    s.hasCover = false;
    s.spineCount = 10;
    s.navigatePrevChapter();
    runner.expectEq(0, s.currentSpineIndex, "Prev chapter at first spine: stays at 0");
    runner.expectFalse(s.needsRender, "Prev chapter at first spine: needsRender stays false");
  }

  // At first content spine (with cover): first content is spine 1, should not go to cover
  {
    ChapterNavState s;
    s.currentSpineIndex = 1;
    s.currentSectionPage = 0;
    s.hasCover = true;
    s.textStartIndex = 0;
    s.spineCount = 10;
    s.navigatePrevChapter();
    runner.expectEq(1, s.currentSpineIndex, "Prev chapter with cover at spine 1: stays at 1 (won't go to cover)");
    runner.expectFalse(s.needsRender, "Prev chapter with cover at spine 1: needsRender stays false");
  }

  // With cover, at spine 2: should go to spine 1 (first content spine)
  {
    ChapterNavState s;
    s.currentSpineIndex = 2;
    s.currentSectionPage = 0;
    s.hasCover = true;
    s.textStartIndex = 0;
    s.spineCount = 10;
    s.navigatePrevChapter();
    runner.expectEq(1, s.currentSpineIndex, "Prev chapter with cover from spine 2: goes to spine 1");
    runner.expectTrue(s.needsRender, "Prev chapter with cover from spine 2: needsRender is true");
  }

  // With textStartIndex=3: should not go below spine 3
  {
    ChapterNavState s;
    s.currentSpineIndex = 3;
    s.currentSectionPage = 0;
    s.hasCover = true;
    s.textStartIndex = 3;
    s.spineCount = 10;
    s.navigatePrevChapter();
    runner.expectEq(3, s.currentSpineIndex, "Prev chapter at textStartIndex=3: stays at 3");
    runner.expectFalse(s.needsRender, "Prev chapter at textStartIndex=3: needsRender stays false");
  }

  // ============================================
  // holdNavigated_ flag behavior
  // ============================================

  // Simulate: first ButtonRepeat triggers navigation, second does not
  {
    bool holdNavigated = false;
    ChapterNavState s;
    s.currentSpineIndex = 2;
    s.spineCount = 10;

    // First repeat: should navigate
    if (!holdNavigated) {
      s.navigateNextChapter();
      holdNavigated = true;
    }
    runner.expectEq(3, s.currentSpineIndex, "Hold: first repeat navigates to spine 3");
    runner.expectTrue(holdNavigated, "Hold: holdNavigated set after first repeat");

    // Second repeat: should NOT navigate
    s.needsRender = false;
    if (!holdNavigated) {
      s.navigateNextChapter();
      holdNavigated = true;
    }
    runner.expectEq(3, s.currentSpineIndex, "Hold: second repeat stays at spine 3");
    runner.expectFalse(s.needsRender, "Hold: second repeat does not trigger render");

    // ButtonRelease: reset flag
    holdNavigated = false;

    // Next repeat after release: should navigate again
    if (!holdNavigated) {
      s.navigateNextChapter();
      holdNavigated = true;
    }
    runner.expectEq(4, s.currentSpineIndex, "Hold: repeat after release navigates to spine 4");
  }

  return runner.allPassed() ? 0 : 1;
}
