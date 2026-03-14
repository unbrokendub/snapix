#include "ClockApp.h"
#include "ImageViewerApp.h"
#include "MiniApp.h"

namespace papyrix {

const MiniApp APPS[] = {
    {"Image Viewer", imageviewer_app::enter, imageviewer_app::update, imageviewer_app::onButton,
     imageviewer_app::render, imageviewer_app::exit, imageviewer_app::renderMenu, imageviewer_app::onMenuButton},
    {"Clock", clock_app::enter, clock_app::update, clock_app::onButton, clock_app::render, clock_app::exit,
     clock_app::renderMenu, clock_app::onMenuButton},
};
const uint8_t APP_COUNT = sizeof(APPS) / sizeof(APPS[0]);
const int8_t APP_IMAGEVIEWER = 0;
const int8_t APP_CLOCK = 1;

}  // namespace papyrix
