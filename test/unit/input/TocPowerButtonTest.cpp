#include "test_utils.h"

#include <cstdint>

// ============================================================================
// Inline TOC dispatch logic mirroring ReaderState::handleTocInput()
// Tests power button deferred to ButtonRelease in TOC mode.
// ============================================================================

enum class EventType : uint8_t {
  None = 0,
  ButtonPress,
  ButtonRepeat,
  ButtonRelease,
};

enum class Button : uint8_t {
  Up,
  Down,
  Left,
  Right,
  Center,
  Back,
  Power,
};

struct Event {
  EventType type;
  Button button;

  static Event press(Button b) { return {EventType::ButtonPress, b}; }
  static Event repeat(Button b) { return {EventType::ButtonRepeat, b}; }
  static Event release(Button b) { return {EventType::ButtonRelease, b}; }
};

enum class TocAction {
  None,
  MoveUp,
  MoveDown,
  PageUp,
  PageDown,
  Select,
  Exit,
};

struct TocDispatcher {
  enum PowerAction : uint8_t { PowerIgnore = 0, PowerSleep = 1, PowerPageTurn = 2 };
  uint8_t shortPwrBtn = PowerIgnore;
  uint32_t powerPressStartedMs_ = 0;
  uint16_t powerButtonDuration = 400;

  // Mock time for testing timing guard
  uint32_t currentTimeMs_ = 0;
  uint32_t millis() const { return currentTimeMs_; }

  TocAction lastAction = TocAction::None;

  void processEvent(const Event& e) {
    lastAction = TocAction::None;

    if (e.type != EventType::ButtonPress && e.type != EventType::ButtonRepeat &&
        e.type != EventType::ButtonRelease) {
      return;
    }

    if (e.type == EventType::ButtonRelease) {
      if (e.button == Button::Power && shortPwrBtn == PowerPageTurn && powerPressStartedMs_ != 0) {
        const uint32_t heldMs = millis() - powerPressStartedMs_;
        if (heldMs < powerButtonDuration) {
          lastAction = TocAction::MoveDown;
        }
        powerPressStartedMs_ = 0;
      }
      return;
    }

    switch (e.button) {
      case Button::Up:
        lastAction = TocAction::MoveUp;
        break;
      case Button::Down:
        lastAction = TocAction::MoveDown;
        break;
      case Button::Left:
        lastAction = TocAction::PageUp;
        break;
      case Button::Right:
        lastAction = TocAction::PageDown;
        break;
      case Button::Center:
        lastAction = TocAction::Select;
        break;
      case Button::Back:
        lastAction = TocAction::Exit;
        break;
      case Button::Power:
        if (shortPwrBtn == PowerPageTurn) {
          powerPressStartedMs_ = millis();
        }
        break;
    }
  }
};

int main() {
  TestUtils::TestRunner runner("TocPowerButton");

  // ============================================
  // Power button: press records time, short release triggers MoveDown
  // ============================================

  {
    TocDispatcher d;
    d.shortPwrBtn = TocDispatcher::PowerPageTurn;
    d.currentTimeMs_ = 100;
    d.processEvent(Event::press(Button::Power));
    runner.expectEq(static_cast<int>(TocAction::None), static_cast<int>(d.lastAction),
                    "Power Press (PageTurn) -> None (deferred to release)");
    runner.expectEq(uint32_t(100), d.powerPressStartedMs_,
                    "Power Press records timestamp");
  }

  // Release without prior press -> None (powerPressStartedMs_ is 0)
  {
    TocDispatcher d;
    d.shortPwrBtn = TocDispatcher::PowerPageTurn;
    d.processEvent(Event::release(Button::Power));
    runner.expectEq(static_cast<int>(TocAction::None), static_cast<int>(d.lastAction),
                    "Power Release without press -> None (no timestamp)");
  }

  {
    TocDispatcher d;
    d.shortPwrBtn = TocDispatcher::PowerIgnore;
    d.processEvent(Event::release(Button::Power));
    runner.expectEq(static_cast<int>(TocAction::None), static_cast<int>(d.lastAction),
                    "Power Release (Ignore) -> None");
  }

  {
    TocDispatcher d;
    d.shortPwrBtn = TocDispatcher::PowerSleep;
    d.processEvent(Event::release(Button::Power));
    runner.expectEq(static_cast<int>(TocAction::None), static_cast<int>(d.lastAction),
                    "Power Release (Sleep) -> None");
  }

  // ============================================
  // Short press -> release (under threshold) -> MoveDown
  // ============================================

  {
    TocDispatcher d;
    d.shortPwrBtn = TocDispatcher::PowerPageTurn;
    d.currentTimeMs_ = 1000;
    d.processEvent(Event::press(Button::Power));
    runner.expectEq(static_cast<int>(TocAction::None), static_cast<int>(d.lastAction),
                    "Power press+release: press -> None");
    d.currentTimeMs_ = 1100;  // 100ms held, under 400ms threshold
    d.processEvent(Event::release(Button::Power));
    runner.expectEq(static_cast<int>(TocAction::MoveDown), static_cast<int>(d.lastAction),
                    "Power press+release (short) -> MoveDown");
    runner.expectEq(uint32_t(0), d.powerPressStartedMs_,
                    "Power release resets timestamp");
  }

  // ============================================
  // Long hold (over threshold) -> None
  // ============================================

  {
    TocDispatcher d;
    d.shortPwrBtn = TocDispatcher::PowerPageTurn;
    d.currentTimeMs_ = 1000;
    d.processEvent(Event::press(Button::Power));
    d.currentTimeMs_ = 1500;  // 500ms held, over 400ms threshold
    d.processEvent(Event::release(Button::Power));
    runner.expectEq(static_cast<int>(TocAction::None), static_cast<int>(d.lastAction),
                    "Power long hold -> None (held past duration)");
  }

  // Hold exactly at boundary -> None
  {
    TocDispatcher d;
    d.shortPwrBtn = TocDispatcher::PowerPageTurn;
    d.currentTimeMs_ = 1000;
    d.processEvent(Event::press(Button::Power));
    d.currentTimeMs_ = 1400;  // exactly 400ms = threshold
    d.processEvent(Event::release(Button::Power));
    runner.expectEq(static_cast<int>(TocAction::None), static_cast<int>(d.lastAction),
                    "Power hold at exact threshold -> None");
  }

  // ============================================
  // Existing buttons still work on Press
  // ============================================

  {
    TocDispatcher d;
    d.processEvent(Event::press(Button::Up));
    runner.expectEq(static_cast<int>(TocAction::MoveUp), static_cast<int>(d.lastAction),
                    "Up Press -> MoveUp");
  }

  {
    TocDispatcher d;
    d.processEvent(Event::press(Button::Down));
    runner.expectEq(static_cast<int>(TocAction::MoveDown), static_cast<int>(d.lastAction),
                    "Down Press -> MoveDown");
  }

  {
    TocDispatcher d;
    d.processEvent(Event::press(Button::Left));
    runner.expectEq(static_cast<int>(TocAction::PageUp), static_cast<int>(d.lastAction),
                    "Left Press -> PageUp");
  }

  {
    TocDispatcher d;
    d.processEvent(Event::press(Button::Right));
    runner.expectEq(static_cast<int>(TocAction::PageDown), static_cast<int>(d.lastAction),
                    "Right Press -> PageDown");
  }

  // ============================================
  // Existing buttons do NOT fire on Release
  // ============================================

  {
    TocDispatcher d;
    d.processEvent(Event::release(Button::Up));
    runner.expectEq(static_cast<int>(TocAction::None), static_cast<int>(d.lastAction),
                    "Up Release -> None");
  }

  {
    TocDispatcher d;
    d.processEvent(Event::release(Button::Down));
    runner.expectEq(static_cast<int>(TocAction::None), static_cast<int>(d.lastAction),
                    "Down Release -> None");
  }

  {
    TocDispatcher d;
    d.processEvent(Event::release(Button::Center));
    runner.expectEq(static_cast<int>(TocAction::None), static_cast<int>(d.lastAction),
                    "Center Release -> None");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
