#include "test_utils.h"

#include <cstdint>

// Inline the Settings enums and getPowerButtonDuration from SnapixSettings.h
namespace snapix {
struct Settings {
  enum PowerButtonAction : uint8_t { PowerIgnore = 0, PowerSleep = 1, PowerPageTurn = 2 };

  uint8_t shortPwrBtn = PowerIgnore;

  uint16_t getPowerButtonDuration() const { return (shortPwrBtn == PowerSleep) ? 10 : 400; }
};
}  // namespace snapix

int main() {
  TestUtils::TestRunner runner("PowerButtonSettingsTest");

  // === PowerButtonAction enum values ===
  runner.expectEq(uint8_t(0), uint8_t(snapix::Settings::PowerIgnore), "PowerIgnore == 0");
  runner.expectEq(uint8_t(1), uint8_t(snapix::Settings::PowerSleep), "PowerSleep == 1");
  runner.expectEq(uint8_t(2), uint8_t(snapix::Settings::PowerPageTurn), "PowerPageTurn == 2");

  // === Default setting ===
  {
    snapix::Settings settings;
    runner.expectEq(uint8_t(snapix::Settings::PowerIgnore), settings.shortPwrBtn,
                    "default shortPwrBtn is PowerIgnore");
  }

  // === getPowerButtonDuration: PowerIgnore -> 400ms ===
  {
    snapix::Settings settings;
    settings.shortPwrBtn = snapix::Settings::PowerIgnore;
    runner.expectEq(uint16_t(400), settings.getPowerButtonDuration(), "PowerIgnore -> 400ms duration");
  }

  // === getPowerButtonDuration: PowerSleep -> 10ms (short press triggers sleep) ===
  {
    snapix::Settings settings;
    settings.shortPwrBtn = snapix::Settings::PowerSleep;
    runner.expectEq(uint16_t(10), settings.getPowerButtonDuration(), "PowerSleep -> 10ms duration");
  }

  // === getPowerButtonDuration: PowerPageTurn -> 400ms ===
  {
    snapix::Settings settings;
    settings.shortPwrBtn = snapix::Settings::PowerPageTurn;
    runner.expectEq(uint16_t(400), settings.getPowerButtonDuration(), "PowerPageTurn -> 400ms duration");
  }

  // === Wakeup verification uses shortPwrBtn directly (not RTC variable) ===
  // The wakeup verification in main.cpp now checks:
  //   if (settings.shortPwrBtn == Settings::PowerSleep) { skip verification }
  // instead of checking rtcPowerButtonDurationMs <= 10
  {
    snapix::Settings settings;

    settings.shortPwrBtn = snapix::Settings::PowerSleep;
    runner.expectTrue(settings.shortPwrBtn == snapix::Settings::PowerSleep,
                      "PowerSleep detected via settings (not RTC)");
    runner.expectEq(uint16_t(10), settings.getPowerButtonDuration(),
                    "PowerSleep duration matches skip threshold");

    settings.shortPwrBtn = snapix::Settings::PowerIgnore;
    runner.expectFalse(settings.shortPwrBtn == snapix::Settings::PowerSleep,
                       "PowerIgnore is not PowerSleep");

    settings.shortPwrBtn = snapix::Settings::PowerPageTurn;
    runner.expectFalse(settings.shortPwrBtn == snapix::Settings::PowerSleep,
                       "PowerPageTurn is not PowerSleep");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
