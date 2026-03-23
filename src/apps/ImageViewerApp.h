#pragma once

#include "MiniApp.h"

namespace papyrix {
namespace imageviewer_app {

void enter(Core& core);
bool update(Core& core);
void onButton(Core& core, Button btn);
bool render(Core& core);
void exit(Core& core);
void renderMenu(Core& core);
void onMenuButton(Core& core, Button btn);

}  // namespace imageviewer_app
}  // namespace papyrix
