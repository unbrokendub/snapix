#include "ReaderDocumentResources.h"

#include <GfxRenderer.h>
#include <Logging.h>

#define TAG "RDR_RES"

namespace papyrix::reader {

ReaderDocumentResources::Session::Session(ReaderDocumentResources& owner, const Owner kind) : owner_(&owner), kind_(kind) {}

ReaderDocumentResources::Session::Session(Session&& other) noexcept : owner_(other.owner_), kind_(other.kind_) {
  other.owner_ = nullptr;
  other.kind_ = Owner::None;
}

ReaderDocumentResources::Session& ReaderDocumentResources::Session::operator=(Session&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  release();
  owner_ = other.owner_;
  kind_ = other.kind_;
  other.owner_ = nullptr;
  other.kind_ = Owner::None;
  return *this;
}

ReaderDocumentResources::Session::~Session() { release(); }

ReaderDocumentResources::State& ReaderDocumentResources::Session::state() { return owner_->state_; }

const ReaderDocumentResources::State& ReaderDocumentResources::Session::state() const { return owner_->state_; }

GfxRenderer& ReaderDocumentResources::Session::renderer() const { return owner_->renderer_; }

void ReaderDocumentResources::Session::release() {
  if (!owner_) {
    return;
  }
  owner_->release(kind_);
  owner_ = nullptr;
  kind_ = Owner::None;
}

ReaderDocumentResources::ReaderDocumentResources(GfxRenderer& renderer) : renderer_(renderer) {}

ReaderDocumentResources::Session ReaderDocumentResources::acquireForeground(const char* reason) {
  return acquire(Owner::Foreground, reason) ? Session(*this, Owner::Foreground) : Session();
}

ReaderDocumentResources::Session ReaderDocumentResources::acquireWorker(const char* reason) {
  return acquire(Owner::Worker, reason) ? Session(*this, Owner::Worker) : Session();
}

bool ReaderDocumentResources::acquire(const Owner kind, const char* reason) {
  Owner expected = Owner::None;
  if (owner_.compare_exchange_strong(expected, kind, std::memory_order_acq_rel, std::memory_order_acquire)) {
    ownerReason_.store(reason, std::memory_order_release);
    return true;
  }

  const char* currentReason = ownerReason_.load(std::memory_order_acquire);
  LOG_ERR(TAG, "[OWNERSHIP] deny request=%d reason=%s current=%d currentReason=%s", static_cast<int>(kind),
          reason ? reason : "-", static_cast<int>(expected), currentReason ? currentReason : "-");
  return false;
}

void ReaderDocumentResources::release(const Owner kind) {
  Owner expected = kind;
  if (!owner_.compare_exchange_strong(expected, Owner::None, std::memory_order_acq_rel, std::memory_order_acquire)) {
    LOG_ERR(TAG, "[OWNERSHIP] release mismatch requested=%d current=%d", static_cast<int>(kind),
            static_cast<int>(expected));
    return;
  }
  ownerReason_.store(nullptr, std::memory_order_release);
}

}  // namespace papyrix::reader
