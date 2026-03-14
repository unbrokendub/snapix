#include "test_utils.h"

#include <cstdint>

// Inline AppMenuView to avoid firmware/graphics dependencies
struct AppMenuView {
  int8_t selected = 0;
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

int main() {
  TestUtils::TestRunner runner("AppMenuViewTest");

  // --- moveDown wraps around ---
  {
    AppMenuView view;
    view.itemCount = 3;
    view.selected = 0;

    view.moveDown();
    runner.expectEq(int8_t(1), view.selected, "moveDown 0 -> 1");
    view.moveDown();
    runner.expectEq(int8_t(2), view.selected, "moveDown 1 -> 2");
    view.moveDown();
    runner.expectEq(int8_t(0), view.selected, "moveDown wraps to 0");
  }

  // --- moveUp wraps around ---
  {
    AppMenuView view;
    view.itemCount = 3;
    view.selected = 0;

    view.moveUp();
    runner.expectEq(int8_t(2), view.selected, "moveUp wraps to last");
    view.moveUp();
    runner.expectEq(int8_t(1), view.selected, "moveUp 2 -> 1");
    view.moveUp();
    runner.expectEq(int8_t(0), view.selected, "moveUp 1 -> 0");
  }

  // --- empty list does nothing ---
  {
    AppMenuView view;
    view.itemCount = 0;
    view.selected = 0;
    view.needsRender = false;

    view.moveDown();
    runner.expectEq(int8_t(0), view.selected, "moveDown on empty is no-op");
    runner.expectFalse(view.needsRender, "moveDown on empty doesn't set needsRender");

    view.moveUp();
    runner.expectEq(int8_t(0), view.selected, "moveUp on empty is no-op");
    runner.expectFalse(view.needsRender, "moveUp on empty doesn't set needsRender");
  }

  // --- single app stays at 0 ---
  {
    AppMenuView view;
    view.itemCount = 1;
    view.selected = 0;

    view.moveDown();
    runner.expectEq(int8_t(0), view.selected, "moveDown single app stays at 0");
    view.moveUp();
    runner.expectEq(int8_t(0), view.selected, "moveUp single app stays at 0");
  }

  // --- sets needsRender on move ---
  {
    AppMenuView view;
    view.itemCount = 2;
    view.needsRender = false;

    view.moveDown();
    runner.expectTrue(view.needsRender, "moveDown sets needsRender");

    view.needsRender = false;
    view.moveUp();
    runner.expectTrue(view.needsRender, "moveUp sets needsRender");
  }

  // --- moveUp from middle ---
  {
    AppMenuView view;
    view.itemCount = 4;
    view.selected = 2;

    view.moveUp();
    runner.expectEq(int8_t(1), view.selected, "moveUp 2 -> 1");
    view.moveUp();
    runner.expectEq(int8_t(0), view.selected, "moveUp 1 -> 0");
    view.moveUp();
    runner.expectEq(int8_t(3), view.selected, "moveUp wraps to last");
  }

  runner.printSummary();
  return runner.allPassed() ? 0 : 1;
}
