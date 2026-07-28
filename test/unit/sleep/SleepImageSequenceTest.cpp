#include "states/SleepImageSequence.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "test_utils.h"

int main() {
  TestUtils::TestRunner runner("Sleep image sequence");

  std::vector<std::string> files = {
      "image10.bmp", "Image2.bmp", "image1.bmp", "image02.bmp", "image20.bmp",
  };
  std::sort(files.begin(), files.end(), snapix::sleepImageFilenameLess);
  const std::vector<std::string> expected = {
      "image1.bmp", "Image2.bmp", "image02.bmp", "image10.bmp", "image20.bmp",
  };
  runner.expectTrue(files == expected, "filenames use deterministic natural order");

  uint32_t nextIndex = 0;
  runner.expectEq(std::size_t(0), snapix::takeNextSleepImageIndex(nextIndex, 3), "first sleep uses first image");
  runner.expectEq(std::size_t(1), snapix::takeNextSleepImageIndex(nextIndex, 3), "second sleep uses second image");
  runner.expectEq(std::size_t(2), snapix::takeNextSleepImageIndex(nextIndex, 3), "third sleep uses third image");
  runner.expectEq(std::size_t(0), snapix::takeNextSleepImageIndex(nextIndex, 3), "sequence wraps after last image");

  uint32_t persistedNextIndex = 0;
  runner.expectEq(std::size_t(0), snapix::takeNextSleepImageIndex(persistedNextIndex, 3),
                  "first unplugged sleep uses first image");
  uint32_t reloadedAfterPowerOn = persistedNextIndex;
  runner.expectEq(std::size_t(1), snapix::takeNextSleepImageIndex(reloadedAfterPowerOn, 3),
                  "cursor restored from settings continues after full power-on");

  nextIndex = 9;
  runner.expectEq(std::size_t(1), snapix::takeNextSleepImageIndex(nextIndex, 4),
                  "cursor is normalized when the file count changes");
  runner.expectEq(uint32_t(2), nextIndex, "normalized cursor advances to following image");

  const uint32_t beforeEmpty = nextIndex;
  runner.expectEq(std::size_t(0), snapix::takeNextSleepImageIndex(nextIndex, 0), "empty directory is safe");
  runner.expectEq(beforeEmpty, nextIndex, "empty directory does not consume sequence position");

  return runner.allPassed() ? 0 : 1;
}
