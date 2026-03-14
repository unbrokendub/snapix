#pragma once

#include <cstdint>

#include "../ui/views/AppLauncherViews.h"
#include "State.h"

class GfxRenderer;

namespace papyrix {

class AppLauncherState : public State {
 public:
  explicit AppLauncherState(GfxRenderer& renderer);
  ~AppLauncherState() override;

  void enter(Core& core) override;
  void exit(Core& core) override;
  StateTransition update(Core& core) override;
  void render(Core& core) override;
  StateId id() const override { return StateId::AppLauncher; }

 private:
  enum class Mode : uint8_t { Menu, App, Overlay };

  GfxRenderer& renderer_;
  Mode mode_;
  int8_t activeApp_;
  bool needsRender_;
  bool goNetwork_;

  ui::AppMenuView menuView_;

  void launchApp(Core& core);
  void stopApp(Core& core);
  void showOverlay();
  void hideOverlay();
};

}  // namespace papyrix
