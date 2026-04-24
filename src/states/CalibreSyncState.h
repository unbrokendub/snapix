#pragma once

#include <cstdint>

#include "../ui/views/CalibreViews.h"
#include "State.h"

extern "C" {
#include "calibre_wireless.h"
}

class GfxRenderer;

namespace snapix {

class CalibreSyncState : public State {
 public:
  explicit CalibreSyncState(GfxRenderer& renderer);
  ~CalibreSyncState() override;

  void enter(Core& core) override;
  void exit(Core& core) override;
  StateTransition update(Core& core) override;
  void render(Core& core) override;
  StateId id() const override { return StateId::CalibreSync; }

 private:
  GfxRenderer& renderer_;
  ui::CalibreView calibreView_ = {};
  bool needsRender_;
  bool goBack_;
  bool restartConn_;  // Flag to restart Calibre connection without WiFi shutdown
  bool syncComplete_;

  // Calibre connection (heap-allocated only when active)
  calibre_conn_t* conn_;
  bool libraryInitialized_;
  int booksReceived_;

  // Callbacks - static to bridge C library to C++ class
  static bool onProgress(void* ctx, uint64_t current, uint64_t total);
  static void onBook(void* ctx, const calibre_book_meta_t* meta, const char* path);
  static void onMessage(const void* ctx, const char* message);
  static bool onDelete(void* ctx, const char* lpath);

  // Handle button input
  void handleInput(Core& core, Button button);

  // Initialize Calibre connection (called from enter() and restartConnection())
  void initializeCalibre(Core& core);

  // Cleanup and prepare for restart
  void cleanup();

  // Restart Calibre connection without shutting down WiFi
  void restartConnection(Core& core);
};

}  // namespace snapix
