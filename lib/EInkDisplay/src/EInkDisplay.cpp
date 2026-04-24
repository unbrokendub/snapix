#include "EInkDisplay.h"

#include <Logging.h>
#include <SharedSpiLock.h>

#define TAG "DISPLAY"

#include <cstring>
#include <fstream>
#include <vector>

// SSD1677 command definitions
// Initialization and reset
#define CMD_SOFT_RESET 0x12             // Soft reset
#define CMD_BOOSTER_SOFT_START 0x0C     // Booster soft-start control
#define CMD_DRIVER_OUTPUT_CONTROL 0x01  // Driver output control
#define CMD_BORDER_WAVEFORM 0x3C        // Border waveform control
#define CMD_TEMP_SENSOR_CONTROL 0x18    // Temperature sensor control

// RAM and buffer management
#define CMD_DATA_ENTRY_MODE 0x11     // Data entry mode
#define CMD_SET_RAM_X_RANGE 0x44     // Set RAM X address range
#define CMD_SET_RAM_Y_RANGE 0x45     // Set RAM Y address range
#define CMD_SET_RAM_X_COUNTER 0x4E   // Set RAM X address counter
#define CMD_SET_RAM_Y_COUNTER 0x4F   // Set RAM Y address counter
#define CMD_WRITE_RAM_BW 0x24        // Write to BW RAM (current frame)
#define CMD_WRITE_RAM_RED 0x26       // Write to RED RAM (used for fast refresh)
#define CMD_AUTO_WRITE_BW_RAM 0x46   // Auto write BW RAM
#define CMD_AUTO_WRITE_RED_RAM 0x47  // Auto write RED RAM

// Display update and refresh
#define CMD_DISPLAY_UPDATE_CTRL1 0x21  // Display update control 1
#define CMD_DISPLAY_UPDATE_CTRL2 0x22  // Display update control 2
#define CMD_MASTER_ACTIVATION 0x20     // Master activation
#define CTRL1_NORMAL 0x00              // Normal mode - compare RED vs BW for partial
#define CTRL1_BYPASS_RED 0x40          // Bypass RED RAM (treat as 0) - for full refresh

// LUT and voltage settings
#define CMD_WRITE_LUT 0x32       // Write LUT
#define CMD_GATE_VOLTAGE 0x03    // Gate voltage
#define CMD_SOURCE_VOLTAGE 0x04  // Source voltage
#define CMD_WRITE_VCOM 0x2C      // Write VCOM
#define CMD_WRITE_TEMP 0x1A      // Write temperature

// Power management
#define CMD_DEEP_SLEEP 0x10  // Deep sleep

// Custom LUT for fast refresh
const unsigned char lut_grayscale[] PROGMEM = {
    // 00 black/white
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 01 light gray
    0x54, 0x54, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 10 gray
    0xAA, 0xA0, 0xA8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 11 dark gray
    0xA2, 0x22, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // L4 (VCOM)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // TP/RP groups (global timing)
    0x01, 0x01, 0x01, 0x01, 0x00,  // G0: A=1 B=1 C=1 D=1 RP=0 (4 frames)
    0x01, 0x01, 0x01, 0x01, 0x00,  // G1: A=1 B=1 C=1 D=1 RP=0 (4 frames)
    0x01, 0x01, 0x01, 0x01, 0x00,  // G2: A=0 B=0 C=0 D=0 RP=0 (4 frames)
    0x00, 0x00, 0x00, 0x00, 0x00,  // G3: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G4: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G5: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G6: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G7: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G8: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G9: A=0 B=0 C=0 D=0 RP=0

    // Frame rate
    0x8F, 0x8F, 0x8F, 0x8F, 0x8F,

    // Voltages (VGH, VSH1, VSH2, VSL, VCOM)
    0x17, 0x41, 0xA8, 0x32, 0x30,

    // Reserved
    0x00, 0x00};

const unsigned char lut_grayscale_revert[] PROGMEM = {
    // 00 black/white
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 10 gray
    0x54, 0x54, 0x54, 0x54, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 01 light gray
    0xA8, 0xA8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // 11 dark gray
    0xFC, 0xFC, 0xFC, 0xFC, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    // L4 (VCOM)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

    // TP/RP groups (global timing)
    0x01, 0x01, 0x01, 0x01, 0x01,  // G0: A=1 B=1 C=1 D=1 RP=0 (4 frames)
    0x01, 0x01, 0x01, 0x01, 0x01,  // G1: A=1 B=1 C=1 D=1 RP=0 (4 frames)
    0x01, 0x01, 0x01, 0x01, 0x00,  // G2: A=0 B=0 C=0 D=0 RP=0 (4 frames)
    0x01, 0x01, 0x01, 0x01, 0x00,  // G3: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G4: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G5: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G6: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G7: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G8: A=0 B=0 C=0 D=0 RP=0
    0x00, 0x00, 0x00, 0x00, 0x00,  // G9: A=0 B=0 C=0 D=0 RP=0

    // Frame rate
    0x8F, 0x8F, 0x8F, 0x8F, 0x8F,

    // Voltages (VGH, VSH1, VSH2, VSL, VCOM)
    0x17, 0x41, 0xA8, 0x32, 0x30,

    // Reserved
    0x00, 0x00};

EInkDisplay::EInkDisplay(int8_t sclk, int8_t mosi, int8_t cs, int8_t dc, int8_t rst, int8_t busy)
    : _sclk(sclk),
      _mosi(mosi),
      _cs(cs),
      _dc(dc),
      _rst(rst),
      _busy(busy) {
  frameBuffer = nullptr;
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  frameBufferActive = nullptr;
#endif
  LOG_INF(TAG, "Constructor called");
  LOG_INF(TAG, "SCLK=%d, MOSI=%d, CS=%d, DC=%d, RST=%d, BUSY=%d", sclk, mosi, cs, dc, rst, busy);
}

void EInkDisplay::begin() {
  LOG_INF(TAG, "begin() called");

  frameBuffer = frameBuffer0;
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  frameBufferActive = frameBuffer1;
#endif

  state_ = {};
  busyTimeoutRecoveries_ = 0;
  fastRefreshFromPowerOffArmed_ = false;

  // Initialize to white
  memset(frameBuffer0, 0xFF, BUFFER_SIZE);
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  LOG_INF(TAG, "Static frame buffer (%lu bytes = 48KB)", BUFFER_SIZE);
#else
  memset(frameBuffer1, 0xFF, BUFFER_SIZE);
  LOG_INF(TAG, "Static frame buffers (2 x %lu bytes = 96KB)", BUFFER_SIZE);
#endif

  LOG_INF(TAG, "Initializing e-ink display driver...");

  initTransport();

  // Reset display
  resetDisplay();

  // Initialize display controller
  if (!initDisplayController()) {
    state_.controller = ControllerState::Faulted;
    LOG_ERR(TAG, "SSD1677 controller init failed");
    return;
  }

  state_.controller = ControllerState::Ready;
  state_.power = PowerState::Off;
  state_.render = RenderState::Bw;
  markBaselineKnown(true);

  LOG_INF(TAG, "E-ink display driver initialized");
}

void EInkDisplay::beginPreservingPanelState() {
  LOG_INF(TAG, "beginPreservingPanelState() called");

  frameBuffer = frameBuffer0;
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  frameBufferActive = frameBuffer1;
#endif

  memset(frameBuffer0, 0xFF, BUFFER_SIZE);
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  memset(frameBuffer1, 0xFF, BUFFER_SIZE);
#endif

  state_ = {};
  state_.controller = ControllerState::WarmPreserved;
  state_.power = PowerState::On;
  state_.render = RenderState::Bw;
  markBaselineKnown(true);
  fastRefreshFromPowerOffArmed_ = false;

  initTransport();

  LOG_INF(TAG, "Warm-started display transport without controller reset");
}

// ============================================================================
// Low-level display control methods
// ============================================================================

void EInkDisplay::initTransport() {
  // Initialize SPI with custom pins
  SPI.begin(_sclk, -1, _mosi, _cs);
  spiSettings = SPISettings(40000000, MSBFIRST, SPI_MODE0);  // MODE0 is standard for SSD1677
  LOG_INF(TAG, "SPI initialized at 40 MHz, Mode 0");

  // Setup GPIO pins
  pinMode(_cs, OUTPUT);
  pinMode(_dc, OUTPUT);
  pinMode(_rst, OUTPUT);
  pinMode(_busy, INPUT);

  digitalWrite(_cs, HIGH);
  digitalWrite(_dc, HIGH);

  LOG_INF(TAG, "GPIO pins configured");
}

void EInkDisplay::resetDisplay() {
  LOG_DBG(TAG, "Resetting display...");
  digitalWrite(_rst, HIGH);
  delay(20);
  digitalWrite(_rst, LOW);
  delay(2);
  digitalWrite(_rst, HIGH);
  delay(20);
  LOG_DBG(TAG, "Display reset complete");
}

void EInkDisplay::sendCommand(uint8_t command) {
  snapix::spi::SharedBusLock busLock;
  if (!busLock) {
    LOG_ERR(TAG, "Shared SPI lock unavailable for command 0x%02X", command);
    return;
  }
  SPI.beginTransaction(spiSettings);
  digitalWrite(_dc, LOW);  // Command mode
  digitalWrite(_cs, LOW);  // Select chip
  SPI.transfer(command);
  digitalWrite(_cs, HIGH);  // Deselect chip
  SPI.endTransaction();
}

void EInkDisplay::sendData(uint8_t data) {
  snapix::spi::SharedBusLock busLock;
  if (!busLock) {
    LOG_ERR(TAG, "Shared SPI lock unavailable for data write");
    return;
  }
  SPI.beginTransaction(spiSettings);
  digitalWrite(_dc, HIGH);  // Data mode
  digitalWrite(_cs, LOW);   // Select chip
  SPI.transfer(data);
  digitalWrite(_cs, HIGH);  // Deselect chip
  SPI.endTransaction();
}

void EInkDisplay::sendData(const uint8_t* data, uint16_t length) {
  snapix::spi::SharedBusLock busLock;
  if (!busLock) {
    LOG_ERR(TAG, "Shared SPI lock unavailable for bulk data write (%u bytes)", static_cast<unsigned>(length));
    return;
  }
  SPI.beginTransaction(spiSettings);
  digitalWrite(_dc, HIGH);       // Data mode
  digitalWrite(_cs, LOW);        // Select chip
  SPI.writeBytes(data, length);  // Transfer all bytes
  digitalWrite(_cs, HIGH);       // Deselect chip
  SPI.endTransaction();
}

void EInkDisplay::sendCommandWithData(uint8_t command, const uint8_t* data, uint16_t length) {
  snapix::spi::SharedBusLock busLock;
  if (!busLock) {
    LOG_ERR(TAG, "Shared SPI lock unavailable for command+data 0x%02X (%u bytes)", command,
            static_cast<unsigned>(length));
    return;
  }
  SPI.beginTransaction(spiSettings);
  digitalWrite(_cs, LOW);
  digitalWrite(_dc, LOW);
  SPI.transfer(command);
  if (data && length > 0) {
    digitalWrite(_dc, HIGH);
    SPI.writeBytes(data, length);
  }
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();
}

bool EInkDisplay::waitWhileBusy(const char* comment) {
  unsigned long start = millis();
  unsigned long lastHook = start;
  constexpr unsigned long INPUT_POLL_INTERVAL_MS = 8;
  while (digitalRead(_busy) == HIGH) {
    const unsigned long now = millis();
    if (waitHook_ && (now - lastHook >= INPUT_POLL_INTERVAL_MS)) {
      waitHook_();
      lastHook = now;
    }
    delay(1);
    if (now - start > 10000) {
      LOG_ERR(TAG, "Timeout waiting for busy%s", comment ? comment : "");
      return recoverFromBusyTimeout(comment);
    }
  }
  if (comment) {
    LOG_DBG(TAG, "Wait complete: %s (%lu ms)", comment, millis() - start);
  }
  return true;
}

bool EInkDisplay::initDisplayController() {
  LOG_INF(TAG, "Initializing SSD1677 controller...");

  const uint8_t TEMP_SENSOR_INTERNAL = 0x80;

  // Soft reset
  sendCommand(CMD_SOFT_RESET);
  if (!waitWhileBusy(" CMD_SOFT_RESET")) return false;

  // Temperature sensor control (internal)
  sendCommandWithData(CMD_TEMP_SENSOR_CONTROL, &TEMP_SENSOR_INTERNAL, 1);

  // Booster soft-start control (GDEQ0426T82 specific values)
  const uint8_t boosterSoftStart[] = {0xAE, 0xC7, 0xC3, 0xC0, 0x40};
  sendCommandWithData(CMD_BOOSTER_SOFT_START, boosterSoftStart, sizeof(boosterSoftStart));

  // Driver output control: set display height (480) and scan direction
  const uint16_t HEIGHT = 480;
  const uint8_t driverOutputControl[] = {
      static_cast<uint8_t>((HEIGHT - 1) % 256),
      static_cast<uint8_t>((HEIGHT - 1) / 256),
      0x02,
  };
  sendCommandWithData(CMD_DRIVER_OUTPUT_CONTROL, driverOutputControl, sizeof(driverOutputControl));

  // Border waveform control
  const uint8_t borderWaveform = 0x01;
  sendCommandWithData(CMD_BORDER_WAVEFORM, &borderWaveform, 1);

  // Set up full screen RAM area
  setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);

  LOG_DBG(TAG, "Clearing RAM buffers...");
  const uint8_t whitePattern = 0xF7;
  sendCommandWithData(CMD_AUTO_WRITE_BW_RAM, &whitePattern, 1);  // Auto write BW RAM
  if (!waitWhileBusy(" CMD_AUTO_WRITE_BW_RAM")) return false;

  sendCommandWithData(CMD_AUTO_WRITE_RED_RAM, &whitePattern, 1);  // Auto write RED RAM
  if (!waitWhileBusy(" CMD_AUTO_WRITE_RED_RAM")) return false;

  LOG_INF(TAG, "SSD1677 controller initialized");
  return true;
}

void EInkDisplay::setRamArea(const uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  constexpr uint8_t DATA_ENTRY_X_INC_Y_DEC = 0x01;

  // Reverse Y coordinate (gates are reversed on this display)
  y = DISPLAY_HEIGHT - y - h;

  // Set data entry mode (X increment, Y decrement for reversed gates)
  sendCommandWithData(CMD_DATA_ENTRY_MODE, &DATA_ENTRY_X_INC_Y_DEC, 1);

  // Set RAM X address range (start, end) - X is in PIXELS
  const uint8_t xRange[] = {
      static_cast<uint8_t>(x % 256),
      static_cast<uint8_t>(x / 256),
      static_cast<uint8_t>((x + w - 1) % 256),
      static_cast<uint8_t>((x + w - 1) / 256),
  };
  sendCommandWithData(CMD_SET_RAM_X_RANGE, xRange, sizeof(xRange));

  // Set RAM Y address range (start, end) - Y is in PIXELS
  const uint8_t yRange[] = {
      static_cast<uint8_t>((y + h - 1) % 256),
      static_cast<uint8_t>((y + h - 1) / 256),
      static_cast<uint8_t>(y % 256),
      static_cast<uint8_t>(y / 256),
  };
  sendCommandWithData(CMD_SET_RAM_Y_RANGE, yRange, sizeof(yRange));

  // Set RAM X address counter - X is in PIXELS
  const uint8_t xCounter[] = {
      static_cast<uint8_t>(x % 256),
      static_cast<uint8_t>(x / 256),
  };
  sendCommandWithData(CMD_SET_RAM_X_COUNTER, xCounter, sizeof(xCounter));

  // Set RAM Y address counter - Y is in PIXELS
  const uint8_t yCounter[] = {
      static_cast<uint8_t>((y + h - 1) % 256),
      static_cast<uint8_t>((y + h - 1) / 256),
  };
  sendCommandWithData(CMD_SET_RAM_Y_COUNTER, yCounter, sizeof(yCounter));
}

bool EInkDisplay::recoverFromBusyTimeout(const char* comment) {
  if (state_.controller == ControllerState::Recovering) {
    state_.controller = ControllerState::Faulted;
    LOG_ERR(TAG, "Nested busy-timeout recovery failed%s", comment ? comment : "");
    return false;
  }

  state_.controller = ControllerState::Recovering;
  state_.power = PowerState::Off;
  state_.render = RenderState::Bw;
  state_.customLutActive = false;
  markBaselineKnown(false);
  ++busyTimeoutRecoveries_;

  LOG_ERR(TAG, "Recovering display after busy timeout%s (count=%u)", comment ? comment : "",
          static_cast<unsigned>(busyTimeoutRecoveries_));

  resetDisplay();
  if (!initDisplayController()) {
    state_.controller = ControllerState::Faulted;
    LOG_ERR(TAG, "Display recovery failed during controller init");
    return false;
  }

  // Re-seed controller RAM with the host's current notion of the frame so the
  // next refresh starts from a coherent baseline instead of stale controller RAM.
  setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  writeRamBuffer(CMD_WRITE_RAM_BW, frameBuffer, BUFFER_SIZE);
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  writeRamBuffer(CMD_WRITE_RAM_RED, frameBuffer, BUFFER_SIZE);
#else
  writeRamBuffer(CMD_WRITE_RAM_RED, frameBuffer, BUFFER_SIZE);
  memcpy(frameBufferActive, frameBuffer, BUFFER_SIZE);
#endif

  state_.controller = ControllerState::Ready;
  state_.power = PowerState::Off;
  state_.render = RenderState::Bw;
  markBaselineKnown(true);
  LOG_INF(TAG, "Display recovery completed");
  return true;
}

bool EInkDisplay::canUseFastRefresh() const {
  return state_.power == PowerState::On && state_.differentialBaselineValid &&
         state_.controller != ControllerState::Faulted;
}

void EInkDisplay::markBaselineKnown(bool valid) { state_.differentialBaselineValid = valid; }

void EInkDisplay::clearScreen(const uint8_t color) const { memset(frameBuffer, color, BUFFER_SIZE); }

void EInkDisplay::drawImage(const uint8_t* imageData, const uint16_t x, const uint16_t y, const uint16_t w,
                            const uint16_t h, const bool fromProgmem) const {
  if (!frameBuffer) {
    LOG_ERR(TAG, "Frame buffer not allocated!");
    return;
  }

  // Calculate bytes per line for the image
  const uint16_t imageWidthBytes = w / 8;

  // Copy image data to frame buffer
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t destY = y + row;
    if (destY >= DISPLAY_HEIGHT) break;

    const uint16_t destOffset = destY * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint16_t srcOffset = row * imageWidthBytes;

    for (uint16_t col = 0; col < imageWidthBytes; col++) {
      if ((x / 8 + col) >= DISPLAY_WIDTH_BYTES) break;

      if (fromProgmem) {
        frameBuffer[destOffset + col] = pgm_read_byte(&imageData[srcOffset + col]);
      } else {
        frameBuffer[destOffset + col] = imageData[srcOffset + col];
      }
    }
  }

  LOG_DBG(TAG, "Image drawn to frame buffer");
}

void EInkDisplay::writeRamBuffer(uint8_t ramBuffer, const uint8_t* data, uint32_t size) {
  const char* bufferName = (ramBuffer == CMD_WRITE_RAM_BW) ? "BW" : "RED";
  const unsigned long startTime = millis();
  LOG_DBG(TAG, "Writing frame buffer to %s RAM (%lu bytes)...", bufferName, size);

  sendCommandWithData(ramBuffer, data, static_cast<uint16_t>(size));

  const unsigned long duration = millis() - startTime;
  LOG_DBG(TAG, "%s RAM write complete (%lu ms)", bufferName, duration);
}

void EInkDisplay::writeRamBufferInverted(uint8_t ramBuffer, const uint8_t* data, uint32_t size) {
  const char* bufferName = (ramBuffer == CMD_WRITE_RAM_BW) ? "BW" : "RED";
  const unsigned long startTime = millis();
  LOG_DBG(TAG, "Writing inverted buffer to %s RAM (%lu bytes)...", bufferName, size);

  snapix::spi::SharedBusLock busLock;
  if (!busLock) {
    LOG_ERR(TAG, "Shared SPI lock unavailable for inverted write");
    return;
  }

  SPI.beginTransaction(spiSettings);
  digitalWrite(_cs, LOW);
  // Command byte
  digitalWrite(_dc, LOW);
  SPI.transfer(ramBuffer);
  // Inverted data in 256-byte chunks (single SPI transaction, no extra lock churn)
  digitalWrite(_dc, HIGH);
  constexpr uint16_t kChunk = 256;
  uint8_t buf[kChunk];
  for (uint32_t off = 0; off < size; off += kChunk) {
    const uint16_t len = (size - off < kChunk) ? static_cast<uint16_t>(size - off) : kChunk;
    for (uint16_t j = 0; j < len; j++) {
      buf[j] = ~data[off + j];
    }
    SPI.writeBytes(buf, len);
  }
  digitalWrite(_cs, HIGH);
  SPI.endTransaction();

  const unsigned long duration = millis() - startTime;
  LOG_DBG(TAG, "%s RAM inverted write complete (%lu ms)", bufferName, duration);
}

void EInkDisplay::setFramebuffer(const uint8_t* bwBuffer) const { memcpy(frameBuffer, bwBuffer, BUFFER_SIZE); }

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
void EInkDisplay::swapBuffers() {
  uint8_t* temp = frameBuffer;
  frameBuffer = frameBufferActive;
  frameBufferActive = temp;
}

void EInkDisplay::syncDrawBufferWithCurrent() { memcpy(frameBuffer, frameBufferActive, BUFFER_SIZE); }

void EInkDisplay::syncCurrentFrameAsPrevious() { memcpy(frameBufferActive, frameBuffer, BUFFER_SIZE); }
#endif

void EInkDisplay::grayscaleRevert() {
  if (state_.render != RenderState::Grayscale) {
    return;
  }

  // Load the revert LUT
  setCustomLUT(true, lut_grayscale_revert);
  refreshDisplay(FAST_REFRESH);
  setCustomLUT(false);
  if (state_.controller != ControllerState::Faulted && state_.controller != ControllerState::Recovering) {
    state_.render = RenderState::Bw;
  }
}

void EInkDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) {
  setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  writeRamBuffer(CMD_WRITE_RAM_BW, lsbBuffer, BUFFER_SIZE);
}

void EInkDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) {
  setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  writeRamBuffer(CMD_WRITE_RAM_RED, msbBuffer, BUFFER_SIZE);
}

void EInkDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  writeRamBuffer(CMD_WRITE_RAM_BW, lsbBuffer, BUFFER_SIZE);
  writeRamBuffer(CMD_WRITE_RAM_RED, msbBuffer, BUFFER_SIZE);
}

#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
/**
 * In single buffer mode, this should be called with the previously written BW buffer
 * to reconstruct the RED buffer for proper differential fast refreshes following a
 * grayscale display.
 */
void EInkDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) {
  setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
  writeRamBuffer(CMD_WRITE_RAM_BW, bwBuffer, BUFFER_SIZE);
  writeRamBuffer(CMD_WRITE_RAM_RED, bwBuffer, BUFFER_SIZE);
  state_.render = RenderState::Bw;
  if (state_.controller != ControllerState::Faulted && state_.controller != ControllerState::Recovering) {
    markBaselineKnown(true);
  }
}
#endif

void EInkDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {
  const bool allowFastFromPowerOff =
      fastRefreshFromPowerOffArmed_ && state_.power == PowerState::Off && state_.differentialBaselineValid;
  if (mode == FAST_REFRESH && !canUseFastRefresh()) {
    // Force half refresh if screen is off - FAST_REFRESH requires a valid
    // differential baseline. We allow a one-shot override after wake when the
    // caller explicitly re-seeded the previous frame in RAM.
    if (!allowFastFromPowerOff) {
      mode = HALF_REFRESH;
    }
  }
  const bool fastRefresh = (mode == FAST_REFRESH);
  fastRefreshFromPowerOffArmed_ = false;

  // If currently in grayscale mode, revert first to black/white
  grayscaleRevert();

  // Set up full screen RAM area
  setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);

  if (!fastRefresh) {
    // For full refresh, write to both buffers before refresh
    writeRamBuffer(CMD_WRITE_RAM_BW, frameBuffer, BUFFER_SIZE);
    writeRamBuffer(CMD_WRITE_RAM_RED, frameBuffer, BUFFER_SIZE);
  } else {
    // For fast refresh, write to BW buffer only
    writeRamBuffer(CMD_WRITE_RAM_BW, frameBuffer, BUFFER_SIZE);
    // In single buffer mode, the RED RAM should already contain the previous frame
    // In dual buffer mode, we write back frameBufferActive which is the last frame
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
    writeRamBuffer(CMD_WRITE_RAM_RED, frameBufferActive, BUFFER_SIZE);
#endif
  }

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  swapBuffers();
#endif

  // Refresh the display
  refreshDisplay(mode, turnOffScreen);

  // HALF/FULL refresh already wrote the current frame to RED RAM before the
  // update, so only FAST refresh needs an extra post-refresh RED sync to keep
  // the differential baseline aligned with what is now on screen.
  if (fastRefresh) {
    setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
    writeRamBuffer(CMD_WRITE_RAM_RED, frameBuffer, BUFFER_SIZE);
#else
    writeRamBuffer(CMD_WRITE_RAM_RED, frameBufferActive, BUFFER_SIZE);
#endif
  }

  if (state_.controller != ControllerState::Faulted && state_.controller != ControllerState::Recovering) {
    markBaselineKnown(true);
  }
}

void EInkDisplay::displayBufferDriveAll(bool turnOffScreen) {
  // Force-drive all pixels to their correct state without the visible black
  // flash of FULL/HALF_REFRESH.  Technique: write the bitwise-inverse of the
  // framebuffer into RED RAM so every pixel is seen as "changed" by the
  // SSD1677 differential engine.  The fast-mode waveform then drives each
  // pixel to its target state — white pixels get a positive pulse, black
  // pixels a negative pulse — clearing any accumulated ghosting.
  //
  // Speed is comparable to FAST_REFRESH (~450 ms + ~12 ms inverted write).

  if (!canUseFastRefresh()) {
    // No valid baseline — fall back to HALF_REFRESH which initialises one.
    displayBuffer(HALF_REFRESH, turnOffScreen);
    return;
  }

  grayscaleRevert();

  setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);

  // BW RAM: target state (what we want on screen)
  writeRamBuffer(CMD_WRITE_RAM_BW, frameBuffer, BUFFER_SIZE);

  // RED RAM: bitwise-inverse — every pixel now appears "changed" to the
  // differential engine, guaranteeing it receives a driving pulse.
  writeRamBufferInverted(CMD_WRITE_RAM_RED, frameBuffer, BUFFER_SIZE);

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  swapBuffers();
#endif

  // Refresh — controller drives 100% of pixels via the fast-mode waveform.
  refreshDisplay(FAST_REFRESH, turnOffScreen);

  // Post-refresh: sync RED RAM with the real displayed frame so that the
  // next differential FAST_REFRESH has a correct baseline.
  setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  writeRamBuffer(CMD_WRITE_RAM_RED, frameBuffer, BUFFER_SIZE);
#else
  writeRamBuffer(CMD_WRITE_RAM_RED, frameBufferActive, BUFFER_SIZE);
#endif

  if (state_.controller != ControllerState::Faulted && state_.controller != ControllerState::Recovering) {
    markBaselineKnown(true);
  }
}

// EXPERIMENTAL: Windowed update support
// Displays only a rectangular region of the frame buffer, preserving the rest of the screen.
// Requirements: x and w must be byte-aligned (multiples of 8 pixels)
void EInkDisplay::displayWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool turnOffScreen) {
  LOG_DBG(TAG, "Displaying window at (%d,%d) size (%dx%d)", x, y, w, h);

  // Validate bounds
  if (x + w > DISPLAY_WIDTH || y + h > DISPLAY_HEIGHT) {
    LOG_ERR(TAG, "Window bounds exceed display dimensions!");
    return;
  }

  // Validate byte alignment
  if (x % 8 != 0 || w % 8 != 0) {
    LOG_ERR(TAG, "Window x and width must be byte-aligned (multiples of 8)!");
    return;
  }

  if (!frameBuffer) {
    LOG_ERR(TAG, "Frame buffer not allocated!");
    return;
  }

  // displayWindow is not supported while the rest of the screen has grayscale content, revert it
  grayscaleRevert();

  if (state_.controller == ControllerState::Faulted) {
    LOG_ERR(TAG, "Skipping window update while display controller is faulted");
    return;
  }

  // Window updates always use fast refresh which requires a valid differential
  // baseline.  If the display isn't ready (cold boot, after deep-sleep, after
  // power-off), fall back to a full-screen displayBuffer which handles the
  // HALF_REFRESH fallback and establishes the baseline for future fast updates.
  if (!canUseFastRefresh()) {
    LOG_DBG(TAG, "Window update: display not ready for fast refresh, using full displayBuffer");
    displayBuffer(FAST_REFRESH, turnOffScreen);
    return;
  }

  // Calculate window buffer size
  const uint16_t windowWidthBytes = w / 8;
  const uint32_t windowBufferSize = windowWidthBytes * h;

  LOG_DBG(TAG, "Window buffer size: %lu bytes (%d x %d pixels)", windowBufferSize, w, h);

  // Reuse heap buffers across calls to avoid fragmentation and sporadic resets
  static std::vector<uint8_t> windowBuffer;
  static std::vector<uint8_t> previousWindowBuffer;
  if (windowBuffer.size() < windowBufferSize) {
    windowBuffer.resize(windowBufferSize);
  }
  if (previousWindowBuffer.size() < windowBufferSize) {
    previousWindowBuffer.resize(windowBufferSize);
  }

  // Extract window region from frame buffer
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t srcY = y + row;
    const uint16_t srcOffset = srcY * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint16_t dstOffset = row * windowWidthBytes;
    memcpy(&windowBuffer[dstOffset], &frameBuffer[srcOffset], windowWidthBytes);
  }

  // Configure RAM area for window
  setRamArea(x, y, w, h);

  // Write to BW RAM (current frame)
  writeRamBuffer(CMD_WRITE_RAM_BW, windowBuffer.data(), windowBufferSize);

#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  // Dual buffer: Extract window from frameBufferActive (previous frame)
  for (uint16_t row = 0; row < h; row++) {
    const uint16_t srcY = y + row;
    const uint16_t srcOffset = srcY * DISPLAY_WIDTH_BYTES + (x / 8);
    const uint16_t dstOffset = row * windowWidthBytes;
    memcpy(&previousWindowBuffer[dstOffset], &frameBufferActive[srcOffset], windowWidthBytes);
  }
  writeRamBuffer(CMD_WRITE_RAM_RED, previousWindowBuffer.data(), windowBufferSize);

  // Keep the differential-refresh buffers in sync exactly like the full-screen path.
  // Without swapping here, the next partial update compares against a stale "previous"
  // frame and the controller starts corrupting static UI chrome.
  swapBuffers();
#endif

  // Perform fast refresh
  refreshDisplay(FAST_REFRESH, turnOffScreen);

  // Post-refresh: sync RED RAM with the full current screen, not just the
  // window. Otherwise pixels outside the updated area keep an older baseline
  // and later window refreshes start leaking stale content into UI chrome.
  setRamArea(0, 0, DISPLAY_WIDTH, DISPLAY_HEIGHT);
#ifdef EINK_DISPLAY_SINGLE_BUFFER_MODE
  writeRamBuffer(CMD_WRITE_RAM_RED, frameBuffer, BUFFER_SIZE);
#else
  writeRamBuffer(CMD_WRITE_RAM_RED, frameBufferActive, BUFFER_SIZE);
#endif
  markBaselineKnown(true);

  LOG_DBG(TAG, "Window display complete");
}

void EInkDisplay::displayGrayBuffer(const bool turnOffScreen) {
  state_.render = RenderState::Grayscale;
  markBaselineKnown(false);

  // activate the custom LUT for grayscale rendering and refresh
  setCustomLUT(true, lut_grayscale);
  refreshDisplay(FAST_REFRESH, turnOffScreen);
  setCustomLUT(false);
}

void EInkDisplay::refreshDisplay(const RefreshMode mode, const bool turnOffScreen) {
  // Configure Display Update Control 1
  const uint8_t ctrl1 = (mode == FAST_REFRESH) ? CTRL1_NORMAL : CTRL1_BYPASS_RED;
  sendCommandWithData(CMD_DISPLAY_UPDATE_CTRL1, &ctrl1, 1);  // Configure buffer comparison mode

  // best guess at display mode bits:
  // bit | hex | name                    | effect
  // ----+-----+--------------------------+-------------------------------------------
  // 7   | 80  | CLOCK_ON                | Start internal oscillator
  // 6   | 40  | ANALOG_ON               | Enable analog power rails (VGH/VGL drivers)
  // 5   | 20  | TEMP_LOAD               | Load temperature (internal or I2C)
  // 4   | 10  | LUT_LOAD                | Load waveform LUT
  // 3   | 08  | MODE_SELECT             | Mode 1/2
  // 2   | 04  | DISPLAY_START           | Run display
  // 1   | 02  | ANALOG_OFF_PHASE        | Shutdown step 1 (undocumented)
  // 0   | 01  | CLOCK_OFF               | Disable internal oscillator

  // Select appropriate display mode based on refresh type
  uint8_t displayMode = 0x00;

  // Enable counter and analog if not already on
  if (state_.power != PowerState::On) {
    state_.power = PowerState::On;
    displayMode |= 0xC0;  // Set CLOCK_ON and ANALOG_ON bits
  }

  // Turn off screen if requested
  if (turnOffScreen) {
    state_.power = PowerState::Off;
    displayMode |= 0x03;  // Set ANALOG_OFF_PHASE and CLOCK_OFF bits
    markBaselineKnown(false);
  }

  if (mode == FULL_REFRESH) {
    displayMode |= 0x34;
  } else if (mode == HALF_REFRESH) {
    // Write high temp to the register for a faster refresh
    const uint8_t warmTemp = 0x5A;
    sendCommandWithData(CMD_WRITE_TEMP, &warmTemp, 1);
    displayMode |= 0xD4;
  } else {  // FAST_REFRESH
    displayMode |= state_.customLutActive ? 0x0C : 0x1C;
  }

  // Power on and refresh display
  const char* refreshType = (mode == FULL_REFRESH) ? "full" : (mode == HALF_REFRESH) ? "half" : "fast";
  LOG_DBG(TAG, "Powering on display 0x%02X (%s refresh)...", displayMode, refreshType);
  sendCommandWithData(CMD_DISPLAY_UPDATE_CTRL2, &displayMode, 1);

  sendCommand(CMD_MASTER_ACTIVATION);

  // Wait for display to finish updating
  LOG_DBG(TAG, "Waiting for display refresh...");
  waitWhileBusy(refreshType);
  if (state_.controller != ControllerState::Faulted && state_.controller != ControllerState::Recovering) {
    if (state_.controller == ControllerState::WarmPreserved) {
      state_.controller = ControllerState::Ready;
    }
    if (!turnOffScreen) {
      markBaselineKnown(true);
    }
  }
}

void EInkDisplay::setCustomLUT(const bool enabled, const unsigned char* lutData) {
  if (enabled) {
    LOG_DBG(TAG, "Loading custom LUT...");

    // Load custom LUT (first 105 bytes: VS + TP/RP + frame rate)
    uint8_t lutDataBuffer[105];
    for (uint16_t i = 0; i < 105; i++) {
      lutDataBuffer[i] = pgm_read_byte(&lutData[i]);
    }
    sendCommandWithData(CMD_WRITE_LUT, lutDataBuffer, sizeof(lutDataBuffer));

    // Set voltage values from bytes 105-109
    const uint8_t gateVoltage = pgm_read_byte(&lutData[105]);
    sendCommandWithData(CMD_GATE_VOLTAGE, &gateVoltage, 1);  // VGH

    const uint8_t sourceVoltages[] = {
        pgm_read_byte(&lutData[106]),
        pgm_read_byte(&lutData[107]),
        pgm_read_byte(&lutData[108]),
    };
    sendCommandWithData(CMD_SOURCE_VOLTAGE, sourceVoltages, sizeof(sourceVoltages));  // VSH1, VSH2, VSL

    const uint8_t vcom = pgm_read_byte(&lutData[109]);
    sendCommandWithData(CMD_WRITE_VCOM, &vcom, 1);  // VCOM

    state_.customLutActive = true;
    LOG_DBG(TAG, "Custom LUT loaded");
  } else {
    state_.customLutActive = false;
    LOG_DBG(TAG, "Custom LUT disabled");
  }
}

void EInkDisplay::deepSleep() {
  LOG_INF(TAG, "Preparing display for deep sleep...");

  // First, power down the display properly
  // This shuts down the analog power rails and clock
  if (state_.power == PowerState::On) {
    const uint8_t bypassRed = CTRL1_BYPASS_RED;
    sendCommandWithData(CMD_DISPLAY_UPDATE_CTRL1, &bypassRed, 1);  // Normal mode

    const uint8_t powerDownSequence = 0x03;
    sendCommandWithData(CMD_DISPLAY_UPDATE_CTRL2, &powerDownSequence, 1);  // Set ANALOG_OFF_PHASE and CLOCK_OFF

    sendCommand(CMD_MASTER_ACTIVATION);

    // Wait for the power-down sequence to complete
    waitWhileBusy(" display power-down");

    state_.power = PowerState::Off;
    markBaselineKnown(false);
  }

  // Now enter deep sleep mode
  LOG_INF(TAG, "Entering deep sleep mode...");
  sendCommand(CMD_DEEP_SLEEP);
  sendData(0x01);  // Enter deep sleep
  state_.controller = ControllerState::Uninitialized;
  fastRefreshFromPowerOffArmed_ = false;
}

void EInkDisplay::saveFrameBufferAsPBM(const char* filename) {
#ifndef ARDUINO
  const uint8_t* buffer = getFrameBuffer();

  std::ofstream file(filename, std::ios::binary);
  if (!file) {
    LOG_ERR(TAG, "Failed to open %s for writing", filename);
    return;
  }

  // Rotate the image 90 degrees counterclockwise when saving
  // Original buffer: 800x480 (landscape)
  // Output image: 480x800 (portrait)
  const int DISPLAY_WIDTH_LOCAL = DISPLAY_WIDTH;    // 800
  const int DISPLAY_HEIGHT_LOCAL = DISPLAY_HEIGHT;  // 480
  const int DISPLAY_WIDTH_BYTES_LOCAL = DISPLAY_WIDTH_LOCAL / 8;

  file << "P4\n";  // Binary PBM
  file << DISPLAY_HEIGHT_LOCAL << " " << DISPLAY_WIDTH_LOCAL << "\n";

  // Create rotated buffer
  std::vector<uint8_t> rotatedBuffer((DISPLAY_HEIGHT_LOCAL / 8) * DISPLAY_WIDTH_LOCAL, 0);

  for (int outY = 0; outY < DISPLAY_WIDTH_LOCAL; outY++) {
    for (int outX = 0; outX < DISPLAY_HEIGHT_LOCAL; outX++) {
      int inX = outY;
      int inY = DISPLAY_HEIGHT_LOCAL - 1 - outX;

      int inByteIndex = inY * DISPLAY_WIDTH_BYTES_LOCAL + (inX / 8);
      int inBitPosition = 7 - (inX % 8);
      bool isWhite = (buffer[inByteIndex] >> inBitPosition) & 1;

      int outByteIndex = outY * (DISPLAY_HEIGHT_LOCAL / 8) + (outX / 8);
      int outBitPosition = 7 - (outX % 8);
      if (!isWhite) {  // Invert: e-ink white=1 -> PBM black=1
        rotatedBuffer[outByteIndex] |= (1 << outBitPosition);
      }
    }
  }

  file.write(reinterpret_cast<const char*>(rotatedBuffer.data()), rotatedBuffer.size());
  file.close();
  LOG_INF(TAG, "Saved framebuffer to %s", filename);
#else
  (void)filename;
  LOG_ERR(TAG, "saveFrameBufferAsPBM is not supported on Arduino builds.");
#endif
}
