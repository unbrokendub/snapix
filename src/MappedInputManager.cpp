#include "MappedInputManager.h"

#include "core/SnapixSettings.h"

decltype(InputManager::BTN_BACK) MappedInputManager::mapButton(const Button button) const {
  const auto frontLayout = settings_ ? static_cast<snapix::Settings::FrontButtonLayout>(settings_->frontButtonLayout)
                                     : snapix::Settings::FrontBCLR;
  const auto sideLayout = settings_ ? static_cast<snapix::Settings::SideButtonLayout>(settings_->sideButtonLayout)
                                    : snapix::Settings::PrevNext;

  switch (button) {
    case Button::Back:
      switch (frontLayout) {
        case snapix::Settings::FrontLRBC:
          return InputManager::BTN_LEFT;
        case snapix::Settings::FrontBCLR:
        default:
          return InputManager::BTN_BACK;
      }
    case Button::Confirm:
      switch (frontLayout) {
        case snapix::Settings::FrontLRBC:
          return InputManager::BTN_RIGHT;
        case snapix::Settings::FrontBCLR:
        default:
          return InputManager::BTN_CONFIRM;
      }
    case Button::Left:
      switch (frontLayout) {
        case snapix::Settings::FrontLRBC:
          return InputManager::BTN_BACK;
        case snapix::Settings::FrontBCLR:
        default:
          return InputManager::BTN_LEFT;
      }
    case Button::Right:
      switch (frontLayout) {
        case snapix::Settings::FrontLRBC:
          return InputManager::BTN_CONFIRM;
        case snapix::Settings::FrontBCLR:
        default:
          return InputManager::BTN_RIGHT;
      }
    case Button::Up:
      switch (sideLayout) {
        case snapix::Settings::NextPrev:
          return InputManager::BTN_DOWN;
        case snapix::Settings::PrevNext:
        default:
          return InputManager::BTN_UP;
      }
    case Button::Down:
      switch (sideLayout) {
        case snapix::Settings::NextPrev:
          return InputManager::BTN_UP;
        case snapix::Settings::PrevNext:
        default:
          return InputManager::BTN_DOWN;
      }
    case Button::Power:
      return InputManager::BTN_POWER;
    case Button::PageBack:
      switch (sideLayout) {
        case snapix::Settings::NextPrev:
          return InputManager::BTN_DOWN;
        case snapix::Settings::PrevNext:
        default:
          return InputManager::BTN_UP;
      }
    case Button::PageForward:
      switch (sideLayout) {
        case snapix::Settings::NextPrev:
          return InputManager::BTN_UP;
        case snapix::Settings::PrevNext:
        default:
          return InputManager::BTN_DOWN;
      }
  }

  return InputManager::BTN_BACK;
}

bool MappedInputManager::wasPressed(const Button button) const { return inputManager.wasPressed(mapButton(button)); }

bool MappedInputManager::wasReleased(const Button button) const { return inputManager.wasReleased(mapButton(button)); }

bool MappedInputManager::isPressed(const Button button) const { return inputManager.isPressed(mapButton(button)); }

bool MappedInputManager::wasAnyPressed() const { return inputManager.wasAnyPressed(); }

bool MappedInputManager::wasAnyReleased() const { return inputManager.wasAnyReleased(); }

unsigned long MappedInputManager::getHeldTime() const { return inputManager.getHeldTime(); }
