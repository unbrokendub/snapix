#include "test_utils.h"

#include "AdcButtonDebouncer.h"

int main() {
  TestUtils::TestRunner runner("AdcButtonDebouncerTest");

  // A complete click is represented by physical timestamps even if the UI
  // does not consume it until much later.
  {
    AdcButtonDebouncer line;
    line.reset(-1, 0);

    runner.expectFalse(line.observe(2, 100).changed, "press candidate starts");
    const auto press = line.observe(2, 108);
    runner.expectTrue(press.changed, "press confirms independently of UI");
    runner.expectEq(-1, static_cast<int>(press.previousButton), "press starts from released");
    runner.expectEq(2, static_cast<int>(press.currentButton), "press keeps decoded button");
    runner.expectEq(100U, press.timestampMs, "press keeps first physical observation");

    runner.expectFalse(line.observe(-1, 150).changed, "release candidate starts");
    const auto release = line.observe(-1, 158);
    runner.expectTrue(release.changed, "release confirms independently of UI");
    runner.expectEq(2, static_cast<int>(release.previousButton), "release identifies held button");
    runner.expectEq(-1, static_cast<int>(release.currentButton), "release clears line");
    runner.expectEq(150U, release.timestampMs, "release keeps first physical observation");
  }

  // Noise on one resistor ladder must not restart debounce on the other.
  {
    AdcButtonDebouncer front;
    AdcButtonDebouncer side;
    front.reset(-1, 0);
    side.reset(-1, 0);

    runner.expectFalse(side.observe(1, 200).changed, "side press candidate starts");
    runner.expectFalse(front.observe(3, 202).changed, "front noise starts separately");
    runner.expectFalse(front.observe(-1, 204).changed, "front noise returns separately");
    const auto sidePress = side.observe(1, 208);
    runner.expectTrue(sidePress.changed, "front noise cannot reset side debounce");
    runner.expectEq(200U, sidePress.timestampMs, "independent line keeps timestamp");
  }

  // Contact bounce produces one stable press, not several synthetic clicks.
  {
    AdcButtonDebouncer line;
    line.reset(-1, 0);
    runner.expectFalse(line.observe(0, 300).changed, "bounce initial press");
    runner.expectFalse(line.observe(-1, 302).changed, "bounce release");
    runner.expectFalse(line.observe(0, 304).changed, "bounce press restarts window");
    const auto press = line.observe(0, 312);
    runner.expectTrue(press.changed, "stable post-bounce press accepted");
    runner.expectEq(304U, press.timestampMs, "post-bounce edge timestamp");
    runner.expectFalse(line.observe(0, 320).changed, "stable press not duplicated");
  }

  // Unsigned elapsed arithmetic remains correct across millis() rollover.
  {
    AdcButtonDebouncer line;
    line.reset(-1, 0xFFFFFF00U);
    runner.expectFalse(line.observe(1, 0xFFFFFFFCU).changed, "rollover candidate starts");
    const auto press = line.observe(1, 0x00000008U);
    runner.expectTrue(press.changed, "rollover press accepted after debounce");
    runner.expectEq(0xFFFFFFFCU, press.timestampMs, "rollover timestamp retained");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
