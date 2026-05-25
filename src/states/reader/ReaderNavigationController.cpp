#include "ReaderNavigationController.h"

namespace snapix::reader {

void ReaderNavigationController::resetSession() {
  holdNavigated_ = false;
  powerPressStartedMs_ = 0;
}

}  // namespace snapix::reader
