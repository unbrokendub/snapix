#include "ReaderNavigationController.h"

namespace snapix::reader {

void ReaderNavigationController::resetSession() {
  holdNavigated_ = false;
  powerPressActive_ = false;
  powerPressStartedMs_ = 0;
  pressSeenMask_ = 0;
}

}  // namespace snapix::reader
