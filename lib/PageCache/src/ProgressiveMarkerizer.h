#pragma once

// Small operation-local adapter used by progressive Markdown/HTML ingestion.
// One stripper instance survives source-chunk boundaries while its output sink
// is switched to the UnifiedCache frame currently being written.  This keeps
// parser state (partial tags/lines, raw-body mode, open styles) continuous, so
// concatenated marker chunks are byte-identical to a single whole-file pass.

#include <FS.h>
#include <HtmlStripper.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace snapix::pagecache {

class SwitchableMarkerSink final : public snapix::smolport::HtmlStripperSink {
 public:
  void begin(File* target, std::function<bool()> shouldAbort = {}) {
    target_ = target;
    shouldAbort_ = std::move(shouldAbort);
    produced_ = 0;
    ok_ = true;
  }

  void detach() {
    target_ = nullptr;
    shouldAbort_ = {};
  }

  void emit(const uint8_t* data, size_t len) override {
    produced_ += len;
    if (target_ != nullptr && len > 0 && (!*target_ || target_->write(data, len) != len)) {
      ok_ = false;
    }
  }

  bool shouldStop() const override {
    return !ok_ || (shouldAbort_ && shouldAbort_());
  }

  size_t produced() const { return produced_; }
  bool ok() const { return ok_; }

 private:
  File* target_ = nullptr;
  std::function<bool()> shouldAbort_;
  size_t produced_ = 0;
  bool ok_ = true;
};

template <typename Stripper>
class StatefulMarkerizer {
 public:
  StatefulMarkerizer() : stripper_(sink_) {}

  void begin(File* target, std::function<bool()> shouldAbort = {}) {
    sink_.begin(target, std::move(shouldAbort));
  }
  void detach() { sink_.detach(); }
  size_t feed(const uint8_t* data, size_t len) { return stripper_.feed(data, len); }
  void finish() { stripper_.finish(); }
  size_t produced() const { return sink_.produced(); }
  bool ok() const { return sink_.ok(); }

 private:
  SwitchableMarkerSink sink_;
  Stripper stripper_;
};

}  // namespace snapix::pagecache
