#pragma once

#include <GfxRenderer.h>
#include <Theme.h>

#include <cstdint>

#include "../Elements.h"

namespace papyrix {
struct MiniApp;
}

namespace ui {

struct AppMenuView {
  static constexpr const char* const EXTRA_ITEMS[] = {"WiFi Transfer", "Calibre Sync"};
  static constexpr int EXTRA_COUNT = 2;

  ButtonBar buttons{"Back", "Open", "", ""};
  int8_t selected = 0;
  int8_t appCount = 0;
  int8_t itemCount = 0;
  bool needsRender = true;

  void moveUp() {
    if (itemCount == 0) return;
    selected = (selected == 0) ? itemCount - 1 : selected - 1;
    needsRender = true;
  }

  void moveDown() {
    if (itemCount == 0) return;
    selected = (selected + 1) % itemCount;
    needsRender = true;
  }
};

void render(const GfxRenderer& r, const Theme& t, const AppMenuView& v, const papyrix::MiniApp* apps);

}  // namespace ui
