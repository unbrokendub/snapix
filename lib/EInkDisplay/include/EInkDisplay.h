#pragma once
#include <Arduino.h>
#include <SPI.h>

class EInkDisplay {
 public:
  using WaitHook = void (*)();

  // Constructor with pin configuration
  EInkDisplay(int8_t sclk, int8_t mosi, int8_t cs, int8_t dc, int8_t rst, int8_t busy);

  // Destructor
  ~EInkDisplay() = default;

 // Refresh modes (guarded to avoid redefinition in test builds)
  enum RefreshMode {
    FULL_REFRESH,  // Full refresh with complete waveform
    HALF_REFRESH,  // Half refresh (1720ms) - balanced quality and speed
    FAST_REFRESH   // Fast refresh using custom LUT
  };

  enum class ControllerState : uint8_t {
    Uninitialized,
    Ready,
    WarmPreserved,
    Recovering,
    Faulted,
  };

  enum class PowerState : uint8_t {
    Off,
    On,
  };

  enum class RenderState : uint8_t {
    Bw,
    Grayscale,
  };

  // Initialize the display hardware and driver
  void begin();
  void beginPreservingPanelState();

  // Display dimensions
  static constexpr uint16_t DISPLAY_WIDTH = 800;
  static constexpr uint16_t DISPLAY_HEIGHT = 480;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  // Frame buffer operations
  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 bool fromProgmem = false) const;

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  void swapBuffers();
  void syncDrawBufferWithCurrent();
  void syncCurrentFrameAsPrevious();
#endif
  void setFramebuffer(const uint8_t* bwBuffer) const;

  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);
#endif

  // turnOffScreen: Power down display after refresh. Used for sunlight fading fix
  // on SSD1677 displays without resin protection (XTEINK X4).
  void displayBuffer(RefreshMode mode = FAST_REFRESH, bool turnOffScreen = false);
  // Force-drive all pixels to their correct state without the black flash of
  // FULL/HALF_REFRESH.  Writes the bitwise-inverse of the framebuffer into RED
  // RAM so the SSD1677 differential engine sees every pixel as "changed" and
  // drives it with the fast-mode waveform (~same speed as FAST_REFRESH).
  void displayBufferDriveAll(bool turnOffScreen = false);
  // EXPERIMENTAL: Windowed update - display only a rectangular region
  void displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen = false);
  void displayGrayBuffer(bool turnOffScreen = false);

  void refreshDisplay(RefreshMode mode = FAST_REFRESH, bool turnOffScreen = false);
  void setWaitHook(WaitHook hook) { waitHook_ = hook; }
  void armFastRefreshFromPowerOffOnce() { fastRefreshFromPowerOffArmed_ = true; }

  // debug function
  void grayscaleRevert();

  // LUT control
  void setCustomLUT(bool enabled, const unsigned char* lutData = nullptr);

  // Power management
  void deepSleep();

  // Access to frame buffer
  uint8_t* getFrameBuffer() const { return frameBuffer; }

  // Save the current framebuffer to a PBM file (desktop/test builds only)
  void saveFrameBufferAsPBM(const char* filename);

 private:
  struct RuntimeState {
    ControllerState controller = ControllerState::Uninitialized;
    PowerState power = PowerState::Off;
    RenderState render = RenderState::Bw;
    bool differentialBaselineValid = false;
    bool customLutActive = false;
  };

  // Pin configuration
  int8_t _sclk, _mosi, _cs, _dc, _rst, _busy;

  // Frame buffer (statically allocated)
  uint8_t frameBuffer0[BUFFER_SIZE];
  uint8_t* frameBuffer;
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  uint8_t frameBuffer1[BUFFER_SIZE];
  uint8_t* frameBufferActive;
#endif

  // SPI settings
  SPISettings spiSettings;

  // State
  RuntimeState state_;
  uint16_t busyTimeoutRecoveries_ = 0;
  bool fastRefreshFromPowerOffArmed_ = false;
  WaitHook waitHook_ = nullptr;

  // Low-level display control
  void initTransport();
  void resetDisplay();
  void IRAM_ATTR sendCommand(uint8_t command);
  void IRAM_ATTR sendData(uint8_t data);
  void IRAM_ATTR sendData(const uint8_t* data, uint16_t length);
  void IRAM_ATTR sendCommandWithData(uint8_t command, const uint8_t* data, uint16_t length);
  bool waitWhileBusy(const char* comment = nullptr);
  bool initDisplayController();
  bool recoverFromBusyTimeout(const char* comment);
  bool canUseFastRefresh() const;
  void markBaselineKnown(bool valid);

  // Low-level display operations
  void IRAM_ATTR setRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
  void IRAM_ATTR writeRamBuffer(uint8_t ramBuffer, const uint8_t* data, uint32_t size);
  void writeRamBufferInverted(uint8_t ramBuffer, const uint8_t* data, uint32_t size);
};
