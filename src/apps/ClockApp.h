#pragma once

#include "MiniApp.h"

namespace snapix {
namespace clock_app {

void enter(Core& core);
bool update(Core& core);
void onButton(Core& core, Button btn);
bool render(Core& core);
void exit(Core& core);
void renderMenu(Core& core);
void onMenuButton(Core& core, Button btn);

}  // namespace clock_app
}  // namespace snapix
