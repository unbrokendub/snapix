#pragma once

#include <ContentParser.h>
#include <PageCache.h>

#include <atomic>
#include <memory>

class GfxRenderer;

namespace papyrix::reader {

class ReaderDocumentResources {
 public:
  enum class Owner : uint8_t {
    None,
    Foreground,
    Worker,
  };

  struct State {
    std::unique_ptr<PageCache> pageCache;
    std::unique_ptr<ContentParser> parser;
    int parserSpineIndex = -1;
    std::unique_ptr<ContentParser> lookaheadParser;
    int lookaheadParserSpineIndex = -1;
  };

  class Session {
   public:
    Session() = default;
    Session(ReaderDocumentResources& owner, Owner kind);
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&& other) noexcept;
    Session& operator=(Session&& other) noexcept;
    ~Session();

    explicit operator bool() const { return owner_ != nullptr; }
    State& state();
    const State& state() const;
    GfxRenderer& renderer() const;

   private:
    void release();

    ReaderDocumentResources* owner_ = nullptr;
    Owner kind_ = Owner::None;
  };

  explicit ReaderDocumentResources(GfxRenderer& renderer);

  Session acquireForeground(const char* reason);
  Session acquireWorker(const char* reason);

  bool isWorkerOwned() const { return owner_.load(std::memory_order_acquire) == Owner::Worker; }
  bool isForegroundOwned() const { return owner_.load(std::memory_order_acquire) == Owner::Foreground; }
  bool isIdle() const { return owner_.load(std::memory_order_acquire) == Owner::None; }
  Owner currentOwner() const { return owner_.load(std::memory_order_acquire); }

  State& unsafeState() { return state_; }
  const State& unsafeState() const { return state_; }
  GfxRenderer& renderer() const { return renderer_; }

 private:
  friend class Session;

  bool acquire(Owner kind, const char* reason);
  void release(Owner kind);

  GfxRenderer& renderer_;
  State state_;
  std::atomic<Owner> owner_{Owner::None};
  std::atomic<const char*> ownerReason_{nullptr};
};

}  // namespace papyrix::reader
