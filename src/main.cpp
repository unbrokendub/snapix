#include <Arduino.h>
#include <EInkDisplay.h>
#include <Epub.h>
#include <GfxRenderer.h>
#include <InputManager.h>
#include <LittleFS.h>  // Must be before SdFat includes to avoid FILE_READ/FILE_WRITE redefinition
#include <SDCardManager.h>
#include <SPI.h>
#include <builtinFonts/reader_2b.h>
#include <builtinFonts/reader_bold_2b.h>
#include <builtinFonts/reader_italic_2b.h>
// XSmall font (12pt)
#include <builtinFonts/reader_xsmall_bold_2b.h>
#include <builtinFonts/reader_xsmall_italic_2b.h>
#include <builtinFonts/reader_xsmall_regular_2b.h>
#include <driver/gpio.h>
#include <esp_system.h>
// Medium font (16pt)
#include <builtinFonts/reader_medium_2b.h>
#include <builtinFonts/reader_medium_bold_2b.h>
#include <builtinFonts/reader_medium_italic_2b.h>
// Large font (18pt)
#include <Logging.h>
#include <builtinFonts/reader_large_2b.h>
#include <builtinFonts/reader_large_bold_2b.h>
#include <builtinFonts/reader_large_italic_2b.h>
#include <builtinFonts/small14.h>
#include <builtinFonts/ui_12.h>
#include <builtinFonts/ui_bold_12.h>

#include "Battery.h"
#include "FontManager.h"
#include "MappedInputManager.h"
#include "ThemeManager.h"
#include "config.h"
#include "content/ContentTypes.h"
#include "ui/Elements.h"

#define TAG "MAIN"

// New refactored core system
#include "core/BootMode.h"
#include "core/CrashDebug.h"
#include "core/Core.h"
#include "core/StateMachine.h"
#include "images/SnapixLogo.h"
#include "states/AppLauncherState.h"
#include "states/CalibreSyncState.h"
#include "states/ErrorState.h"
#include "states/FileListState.h"
#include "states/HomeState.h"
#include "states/NetworkState.h"
#include "states/ReaderState.h"
#include "states/SettingsState.h"
#include "states/SleepState.h"
#include "states/StartupState.h"
#include "ui/views/BootSleepViews.h"

#define SPI_FQ 40000000
// Display SPI pins (custom pins for XteinkX4, not hardware SPI defaults)
#define EPD_SCLK 8   // SPI Clock
#define EPD_MOSI 10  // SPI MOSI (Master Out Slave In)
#define EPD_CS 21    // Chip Select
#define EPD_DC 4     // Data/Command
#define EPD_RST 5    // Reset
#define EPD_BUSY 6   // Busy

#define UART0_RXD 20  // Used for USB connection detection

#define SD_SPI_MISO 7

#define SERIAL_INIT_DELAY_MS 10
// Не держим загрузку ради подключения host-side terminal: CDC может открыться и позже,
// а длинное ожидание здесь напрямую ухудшает boot-to-UI latency.
#define SERIAL_READY_TIMEOUT_MS 150

EInkDisplay einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY);
InputManager inputManager;
MappedInputManager mappedInputManager(inputManager);
GfxRenderer renderer(einkDisplay);

// Extern references for driver wrappers
EInkDisplay& display = einkDisplay;
MappedInputManager& mappedInput = mappedInputManager;

// Core system
namespace snapix {
Core core;
}

static void serviceInputDuringDisplayWait() {
  inputManager.update();
  snapix::core.input.poll();
}

// State instances (pre-allocated, no heap per transition)
static snapix::StartupState startupState;
static snapix::HomeState homeState(renderer);
static snapix::FileListState fileListState(renderer);
static snapix::ReaderState readerState(renderer);
static snapix::SettingsState settingsState(renderer);
static snapix::NetworkState networkState(renderer);
static snapix::CalibreSyncState calibreSyncState(renderer);
static snapix::AppLauncherState appLauncherState(renderer);
static snapix::SleepState sleepState(renderer);
static snapix::ErrorState errorState(renderer);
static snapix::StateMachine stateMachine;

RTC_DATA_ATTR uint16_t rtcPowerButtonDurationMs = 400;

// Always-needed fonts (UI, status bar)
EpdFont smallFont(&small14);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui12Font(&ui_12);
EpdFont uiBold12Font(&ui_bold_12);
EpdFontFamily uiFontFamily(&ui12Font, &uiBold12Font);

// Reader font families — lazily constructed via static locals so only the
// active size allocates EpdFont objects (~520 bytes each × 3 per size).
// In READER mode this saves ~4.5KB by not instantiating unused sizes.
static EpdFontFamily& readerFontFamilyXSmall() {
  static EpdFont r(&reader_xsmall_regular_2b), b(&reader_xsmall_bold_2b), i(&reader_xsmall_italic_2b);
  static EpdFontFamily f(&r, &b, &i, &b);
  return f;
}
static EpdFontFamily& readerFontFamilySmall() {
  static EpdFont r(&reader_2b), b(&reader_bold_2b), i(&reader_italic_2b);
  static EpdFontFamily f(&r, &b, &i, &b);
  return f;
}
static EpdFontFamily& readerFontFamilyMedium() {
  static EpdFont r(&reader_medium_2b), b(&reader_medium_bold_2b), i(&reader_medium_italic_2b);
  static EpdFontFamily f(&r, &b, &i, &b);
  return f;
}
static EpdFontFamily& readerFontFamilyLarge() {
  static EpdFont r(&reader_large_2b), b(&reader_large_bold_2b), i(&reader_large_italic_2b);
  static EpdFontFamily f(&r, &b, &i, &b);
  return f;
}

bool isUsbConnected() { return digitalRead(UART0_RXD) == HIGH; }

struct WakeupInfo {
  esp_reset_reason_t resetReason;
  bool isPowerButton;
};

static bool wokeFromPowerButtonSleep = false;

WakeupInfo getWakeupInfo() {
  const bool usbConnected = isUsbConnected();
  const auto wakeupCause = esp_sleep_get_wakeup_cause();
  const auto resetReason = esp_reset_reason();

  // Without USB: power button triggers a full power-on reset (not GPIO wakeup)
  // With USB: power button wakes from deep sleep via GPIO
  const bool isPowerButton =
      (!usbConnected && wakeupCause == ESP_SLEEP_WAKEUP_UNDEFINED && resetReason == ESP_RST_POWERON) ||
      (usbConnected && wakeupCause == ESP_SLEEP_WAKEUP_GPIO && resetReason == ESP_RST_DEEPSLEEP);

  return {resetReason, isPowerButton};
}

// Verify long press on wake-up from deep sleep
void verifyWakeupLongPress(esp_reset_reason_t resetReason) {
  if (resetReason == ESP_RST_SW) {
    LOG_DBG(TAG, "Skipping wakeup verification (software restart)");
    return;
  }

  // Fast path for short press mode - skip verification entirely.
  // Uses settings directly (not RTC variable) so it works even after a full power cycle
  // where RTC memory is lost. Needed because inputManager.isPressed() may take up to
  // ~500ms to return the correct state after wake-up.
  if (snapix::core.settings.shortPwrBtn == snapix::Settings::PowerSleep) {
    LOG_DBG(TAG, "Skipping wakeup verification (short press mode)");
    return;
  }

  // Give the user up to 1000ms to start holding the power button, and must hold for the configured duration
  const auto start = millis();
  bool abort = false;
  const uint16_t requiredPressDuration = snapix::core.settings.getPowerButtonDuration();

  inputManager.update();
  // Verify the user has actually pressed
  while (!inputManager.isPressed(InputManager::BTN_POWER) && millis() - start < 1000) {
    delay(10);  // only wait 10ms each iteration to not delay too much in case of short configured duration.
    inputManager.update();
  }

  if (inputManager.isPressed(InputManager::BTN_POWER)) {
    do {
      delay(10);
      inputManager.update();
    } while (inputManager.isPressed(InputManager::BTN_POWER) && inputManager.getHeldTime() < requiredPressDuration);
    abort = inputManager.getHeldTime() < requiredPressDuration;
  } else {
    abort = true;
  }

  if (abort) {
    // Button released too early. Returning to sleep.
    // IMPORTANT: Re-arm the wakeup trigger before sleeping again
    esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
    disableGpioPullsForSleep();
    esp_deep_sleep_start();
  }
}

bool verifyManagedSleepWakeLongPress() {
  // Always require a deliberate long press to wake from managed sleep,
  // regardless of the short power-button action configured for runtime use.
  const uint16_t requiredPressDuration =
      snapix::core.settings.getPowerButtonDuration() < 400 ? 400 : snapix::core.settings.getPowerButtonDuration();
  const auto start = millis();

  inputManager.update();
  while (!inputManager.isPressed(InputManager::BTN_POWER) && millis() - start < 1000) {
    delay(10);
    inputManager.update();
  }

  if (!inputManager.isPressed(InputManager::BTN_POWER)) {
    return false;
  }

  do {
    delay(10);
    inputManager.update();
  } while (inputManager.isPressed(InputManager::BTN_POWER) && inputManager.getHeldTime() < requiredPressDuration);

  return inputManager.getHeldTime() >= requiredPressDuration;
}

bool waitForPowerRelease(const unsigned long timeoutMs = 0) {
  const unsigned long startedAt = millis();
  inputManager.update();
  while (inputManager.isPressed(InputManager::BTN_POWER)) {
    if (timeoutMs > 0 && millis() - startedAt >= timeoutMs) {
      LOG_INF(TAG, "Timed out waiting for power button release");
      return false;
    }
    delay(50);
    inputManager.update();
  }
  return true;
}

// Register only the reader font for the active size (saves ~4.5KB in READER mode)
void setupReaderFontForSize(snapix::Settings::FontSize fontSize) {
  switch (fontSize) {
    case snapix::Settings::FontXSmall:
      renderer.insertFont(READER_FONT_ID_XSMALL, readerFontFamilyXSmall());
      break;
    case snapix::Settings::FontMedium:
      renderer.insertFont(READER_FONT_ID_MEDIUM, readerFontFamilyMedium());
      break;
    case snapix::Settings::FontLarge:
      renderer.insertFont(READER_FONT_ID_LARGE, readerFontFamilyLarge());
      break;
    default:  // FontSmall
      renderer.insertFont(READER_FONT_ID, readerFontFamilySmall());
      break;
  }
}

void setupDisplayAndFonts(bool allReaderSizes = true, bool preservePanelState = false) {
  if (preservePanelState) {
    einkDisplay.beginPreservingPanelState();
  } else {
    einkDisplay.begin();
  }
  renderer.begin();
  LOG_INF(TAG, "Display initialized");
  if (allReaderSizes) {
    renderer.insertFont(READER_FONT_ID_XSMALL, readerFontFamilyXSmall());
    renderer.insertFont(READER_FONT_ID, readerFontFamilySmall());
    renderer.insertFont(READER_FONT_ID_MEDIUM, readerFontFamilyMedium());
    renderer.insertFont(READER_FONT_ID_LARGE, readerFontFamilyLarge());
  } else {
    setupReaderFontForSize(static_cast<snapix::Settings::FontSize>(snapix::core.settings.fontSize));
  }
  renderer.insertFont(UI_FONT_ID, uiFontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);
  renderer.excludeExternalFont(UI_FONT_ID);
  renderer.excludeExternalFont(SMALL_FONT_ID);
  LOG_INF(TAG, "Fonts setup");
}

void prepareTransitionFastStartBaseline() {
  const char* message = snapix::getTransitionNotificationMessage();
  if (!message || message[0] == '\0') {
    return;
  }

  snapix::prepareTransitionNotificationFrame(message);
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  einkDisplay.syncCurrentFrameAsPrevious();
#endif
  LOG_INF(TAG, "Prepared transition baseline for fast post-reboot refresh");
}

void prepareLightSleepWakeBaseline() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen(0xFF);
  renderer.drawImage(SnapixLogo, (pageWidth + 128) / 2, (pageHeight - 128) / 2, 128, 128);
  renderer.drawCenteredText(THEME.uiFontId, pageHeight / 2 + 70, "Snapix", true, BOLD);
  renderer.drawCenteredText(THEME.smallFontId, pageHeight / 2 + 110, "SLEEPING", true);
#ifndef EINK_DISPLAY_SINGLE_BUFFER_MODE
  einkDisplay.syncCurrentFrameAsPrevious();
#endif
  einkDisplay.armFastRefreshFromPowerOffOnce();
  LOG_INF(TAG, "Prepared light sleep baseline for fast wake refresh");
}

void applyThemeFonts() {
  Theme& theme = THEME_MANAGER.mutableCurrent();

  // Reset UI font to builtin first in case custom font loading fails
  theme.uiFontId = UI_FONT_ID;
  theme.statusFontId = SMALL_FONT_ID;
  theme.readerFontIdXSmall = READER_FONT_ID_XSMALL;
  theme.readerFontId = READER_FONT_ID;
  theme.readerFontIdMedium = READER_FONT_ID_MEDIUM;
  theme.readerFontIdLarge = READER_FONT_ID_LARGE;

  if (theme.statusFontFamily[0] != '\0') {
    int customStatusFontId = FONT_MANAGER.getStatusFontId(theme.statusFontFamily, SMALL_FONT_ID);
    if (customStatusFontId != SMALL_FONT_ID) {
      theme.statusFontId = customStatusFontId;
      LOG_INF(TAG, "Status font: %s (ID: %d)", theme.statusFontFamily, customStatusFontId);
    } else {
      LOG_ERR(TAG, "Status font unavailable, falling back to builtin: %s", theme.statusFontFamily);
    }
  } else {
    FONT_MANAGER.getStatusFontId(nullptr, SMALL_FONT_ID);
  }

  // Only load the reader font that matches current font size setting
  // This saves ~500KB+ of RAM by not loading all three sizes
  const char* fontFamilyName = nullptr;
  int* targetFontId = nullptr;
  int builtinFontId = 0;

  switch (snapix::core.settings.fontSize) {
    case snapix::Settings::FontXSmall:
      fontFamilyName = theme.readerFontFamilyXSmall;
      targetFontId = &theme.readerFontIdXSmall;
      builtinFontId = READER_FONT_ID_XSMALL;
      break;
    case snapix::Settings::FontMedium:
      fontFamilyName = theme.readerFontFamilyMedium;
      targetFontId = &theme.readerFontIdMedium;
      builtinFontId = READER_FONT_ID_MEDIUM;
      break;
    case snapix::Settings::FontLarge:
      fontFamilyName = theme.readerFontFamilyLarge;
      targetFontId = &theme.readerFontIdLarge;
      builtinFontId = READER_FONT_ID_LARGE;
      break;
    default:  // FontSmall
      fontFamilyName = theme.readerFontFamilySmall;
      targetFontId = &theme.readerFontId;
      builtinFontId = READER_FONT_ID;
      break;
  }

  // Reset to builtin first in case custom font loading fails
  *targetFontId = builtinFontId;

  if (fontFamilyName && fontFamilyName[0] != '\0') {
    int customFontId = FONT_MANAGER.getReaderFontId(fontFamilyName, builtinFontId);
    if (customFontId != builtinFontId) {
      *targetFontId = customFontId;
      LOG_INF(TAG, "Reader font: %s (ID: %d)", fontFamilyName, customFontId);
    } else {
      LOG_ERR(TAG, "Reader font unavailable, falling back to builtin: %s", fontFamilyName);
    }
  }
}

void showErrorScreen(const char* message) {
  renderer.clearScreen(false);
  renderer.drawCenteredText(UI_FONT_ID, 100, message, true, BOLD);
  renderer.displayBuffer();
}

// Track current boot mode for loop behavior
static snapix::BootMode currentBootMode = snapix::BootMode::UI;

// Early initialization - common to both boot modes
// Returns false if critical initialization failed
bool earlyInit() {
#if SNAPIX_SERIAL_LOG_ENABLED
  // Only start serial if USB connected
  pinMode(UART0_RXD, INPUT);
  if (isUsbConnected()) {
    Serial.begin(115200);
    delay(SERIAL_INIT_DELAY_MS);  // Allow USB CDC to initialize
    unsigned long start = millis();
    while (!Serial && (millis() - start) < SERIAL_READY_TIMEOUT_MS) {
      delay(SERIAL_INIT_DELAY_MS);
    }
  }
#endif

  inputManager.begin();

  // Initialize SPI and SD card before wakeup verification so settings are available
  SPI.begin(EPD_SCLK, SD_SPI_MISO, EPD_MOSI, EPD_CS);
  if (!SdMan.begin()) {
    LOG_ERR(TAG, "SD card initialization failed");
    setupDisplayAndFonts();
    showErrorScreen("SD card error");
    return false;
  }

  // Load settings before wakeup verification - without this, a full power cycle
  // (no USB) resets RTC memory and the short power button setting is ignored
  snapix::core.settings.loadFromFile();
  rtcPowerButtonDurationMs = snapix::core.settings.getPowerButtonDuration();

  const auto wakeup = getWakeupInfo();
  snapix::crashdebug::logBootInfo(wakeup.resetReason);
  const bool pendingSleepWake = snapix::core.settings.pendingSleepWake != 0;
  const bool resumedFromManagedSleep = pendingSleepWake && wakeup.isPowerButton;

  wokeFromPowerButtonSleep = false;
  snapix::core.wokeFromSleep = false;

  if (resumedFromManagedSleep) {
    if (!verifyManagedSleepWakeLongPress()) {
      LOG_INF(TAG, "Wake press too short, returning to sleep");
      esp_deep_sleep_enable_gpio_wakeup(1ULL << InputManager::POWER_BUTTON_PIN, ESP_GPIO_WAKEUP_GPIO_LOW);
      disableGpioPullsForSleep();
      esp_deep_sleep_start();
    }

    snapix::core.settings.pendingSleepWake = 0;
    if (!snapix::core.settings.saveToFile()) {
      LOG_ERR(TAG, "Failed clearing pending sleep wake flag");
    }

    wokeFromPowerButtonSleep = true;
    snapix::core.wokeFromSleep = true;
  } else {
    if (pendingSleepWake) {
      snapix::core.settings.pendingSleepWake = 0;
      if (!snapix::core.settings.saveToFile()) {
        LOG_ERR(TAG, "Failed clearing stale pending sleep wake flag");
      }
    }
  }

  if (wakeup.isPowerButton && !resumedFromManagedSleep) {
    verifyWakeupLongPress(wakeup.resetReason);
  }

  LOG_INF(TAG, "Starting Snapix version " SNAPIX_VERSION);

  // Initialize battery ADC pin with proper attenuation for 0-3.3V range
  analogSetPinAttenuation(BAT_GPIO0, ADC_11db);

  // Initialize internal flash filesystem for font storage
  if (!LittleFS.begin(false)) {
    LOG_ERR(TAG, "LittleFS mount failed, attempting format");
    if (!LittleFS.format() || !LittleFS.begin(false)) {
      LOG_ERR(TAG, "LittleFS recovery failed");
      showErrorScreen("Internal storage error");
      return false;
    }
    LOG_INF(TAG, "LittleFS formatted and mounted");
  } else {
    LOG_INF(TAG, "LittleFS mounted");
  }

  return true;
}

// Initialize UI mode - full state registration, all resources
void initUIMode() {
  LOG_INF(TAG, "Initializing UI mode");
  LOG_DBG(TAG, "[UI mode] Free heap: %lu, Max block: %lu", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  const auto& preInitTransition = snapix::getTransition();
  const bool preservePanelState = !snapix::core.settings.transitionFullRefresh && snapix::hasTransitionNotification();

  // Initialize theme and font managers (full)
  FONT_MANAGER.init(renderer);
  THEME_MANAGER.loadTheme(snapix::core.settings.themeName);
  THEME_MANAGER.createDefaultThemeFiles();
  LOG_INF(TAG, "Theme loaded: %s", THEME_MANAGER.currentThemeName());

  setupDisplayAndFonts(true, preservePanelState);
  applyThemeFonts();
  if (preservePanelState) {
    prepareTransitionFastStartBaseline();
  } else if (wokeFromPowerButtonSleep && snapix::core.settings.sleepScreen == snapix::Settings::SleepLight) {
    prepareLightSleepWakeBaseline();
  }

  // Show boot splash only on cold boot (not mode transition)
  if (!preInitTransition.isValid() && !wokeFromPowerButtonSleep) {
    ui::BootView bootView;
    bootView.setLogo(SnapixLogo, 128, 128);
    bootView.setVersion(SNAPIX_VERSION);
    bootView.setStatus("BOOTING");
    ui::render(renderer, THEME, bootView);
  }

  // Register ALL states for UI mode
  stateMachine.registerState(&startupState);
  stateMachine.registerState(&homeState);
  stateMachine.registerState(&fileListState);
  stateMachine.registerState(&readerState);
  stateMachine.registerState(&settingsState);
  stateMachine.registerState(&networkState);
  stateMachine.registerState(&calibreSyncState);
  stateMachine.registerState(&appLauncherState);
  stateMachine.registerState(&sleepState);
  stateMachine.registerState(&errorState);

  // Initialize core
  auto result = snapix::core.init();
  if (!result.ok()) {
    LOG_ERR(TAG, "Init failed: %s", snapix::errorToString(result.err));
    showErrorScreen("Core init failed");
    return;
  }

  LOG_INF(TAG, "State machine starting (UI mode)");
  mappedInputManager.setSettings(&snapix::core.settings);
  ui::setFrontButtonLayout(snapix::core.settings.frontButtonLayout);
  einkDisplay.setWaitHook(serviceInputDuringDisplayWait);

  // Determine initial state - check for return from reader mode
  snapix::StateId initialState = snapix::StateId::Home;
  const auto& transition = snapix::getTransition();

  if (transition.isValid() && transition.mode == snapix::BootMode::READER) {
    initialState = snapix::StateId::Reader;
    strncpy(snapix::core.buf.path, transition.bookPath, sizeof(snapix::core.buf.path) - 1);
    snapix::core.buf.path[sizeof(snapix::core.buf.path) - 1] = '\0';
    snapix::core.pendingDirectReaderTransition = true;
    snapix::core.pendingReaderReturnState =
        (transition.returnTo == snapix::ReturnTo::FILE_MANAGER) ? snapix::StateId::FileList : snapix::StateId::Home;
    LOG_INF(TAG, "Starting directly in Reader via UI mode: %s", snapix::core.buf.path);
  } else if (transition.returnTo == snapix::ReturnTo::FILE_MANAGER) {
    initialState = snapix::StateId::FileList;
    LOG_INF(TAG, "Returning to FileList from Reader");
  } else {
    LOG_INF(TAG, "Starting at Home");
  }

  stateMachine.init(snapix::core, initialState);

  // Discard button events generated during state entry (e.g. power button
  // release captured during the loading-banner display wait on wake from sleep).
  snapix::core.events.clear();

  // Force initial render
  LOG_DBG(TAG, "Forcing initial render");
  stateMachine.update(snapix::core);

  LOG_DBG(TAG, "[UI mode] After init - Free heap: %lu, Max block: %lu", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

// Initialize Reader mode - minimal state registration, single font size
void initReaderMode() {
  LOG_INF(TAG, "Initializing READER mode");
  LOG_DBG(TAG, "[READER mode] Free heap: %lu, Max block: %lu", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // Detect content type early to decide if we need custom fonts
  // XTC/XTCH files contain pre-rendered bitmaps and don't need fonts for page rendering
  const auto& transition = snapix::getTransition();
  snapix::ContentType contentType = snapix::detectContentType(transition.bookPath);
  bool needsCustomFonts = (contentType != snapix::ContentType::Xtc);
  const bool preservePanelState = !snapix::core.settings.transitionFullRefresh && snapix::hasTransitionNotification();

  // Initialize theme and font managers (minimal - no cache)
  FONT_MANAGER.init(renderer);
  THEME_MANAGER.loadTheme(snapix::core.settings.themeName);
  // Skip createDefaultThemeFiles() - not needed in reader mode
  LOG_INF(TAG, "Theme loaded: %s (reader mode)", THEME_MANAGER.currentThemeName());

  setupDisplayAndFonts(false, preservePanelState);  // Only active reader font size

  if (needsCustomFonts) {
    applyThemeFonts();  // Custom fonts - skip for XTC/XTCH to save ~500KB+ RAM
  } else {
    LOG_DBG(TAG, "Skipping custom fonts for XTC content");
  }
  if (preservePanelState) {
    prepareTransitionFastStartBaseline();
  } else if (wokeFromPowerButtonSleep && snapix::core.settings.sleepScreen == snapix::Settings::SleepLight) {
    prepareLightSleepWakeBaseline();
  }

  // Register ONLY states needed for Reader mode
  stateMachine.registerState(&readerState);
  stateMachine.registerState(&sleepState);
  stateMachine.registerState(&errorState);

  // Initialize core
  auto result = snapix::core.init();
  if (!result.ok()) {
    LOG_ERR(TAG, "Init failed: %s", snapix::errorToString(result.err));
    showErrorScreen("Core init failed");
    return;
  }

  LOG_INF(TAG, "State machine starting (READER mode)");
  mappedInputManager.setSettings(&snapix::core.settings);
  ui::setFrontButtonLayout(snapix::core.settings.frontButtonLayout);
  einkDisplay.setWaitHook(serviceInputDuringDisplayWait);

  if (transition.bookPath[0] != '\0') {
    // Copy path to shared buffer for ReaderState to consume
    strncpy(snapix::core.buf.path, transition.bookPath, sizeof(snapix::core.buf.path) - 1);
    snapix::core.buf.path[sizeof(snapix::core.buf.path) - 1] = '\0';
    LOG_INF(TAG, "Opening book: %s", snapix::core.buf.path);
  } else {
    // No book path - fall back to UI mode to avoid boot loop
    LOG_ERR(TAG, "No book path in transition, falling back to UI");
    initUIMode();
    return;
  }

  stateMachine.init(snapix::core, snapix::StateId::Reader);

  // Discard button events generated during state entry (e.g. power button
  // release captured during the loading-banner display wait on wake from sleep).
  snapix::core.events.clear();

  // Force initial render
  LOG_DBG(TAG, "Forcing initial render");
  stateMachine.update(snapix::core);

  LOG_DBG(TAG, "[READER mode] After init - Free heap: %lu, Max block: %lu", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

void setup() {
  // Early initialization (common to both modes)
  if (!earlyInit()) {
    return;  // Critical failure
  }

  // Detect boot mode from RTC memory or settings
  currentBootMode = snapix::detectBootMode();

  if (currentBootMode == snapix::BootMode::READER && !snapix::core.settings.transitionFullRefresh) {
    initUIMode();
  } else if (currentBootMode == snapix::BootMode::READER) {
    initReaderMode();
  } else {
    initUIMode();
  }

  // Ensure we're not still holding the power button before leaving setup.
  // After deep sleep wake the power pin can stay unstable briefly; do not
  // block forever on boot if the line is stuck low.
  if (wokeFromPowerButtonSleep) {
    waitForPowerRelease(1500);
  } else {
    waitForPowerRelease();
  }
  inputManager.update();
  snapix::core.input.resyncState();
  snapix::core.events.clear();
}

void loop() {
  static unsigned long maxLoopDuration = 0;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

  inputManager.update();

  if (!snapix::core.cpu.isThrottled() && millis() - lastMemPrint >= 10000) {
    LOG_DBG(TAG,
            "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes, EventQ: size=%u high=%u dropped=%lu",
            ESP.getFreeHeap(), ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(),
            static_cast<unsigned>(snapix::core.events.size()), static_cast<unsigned>(snapix::core.events.highWaterMark()),
            static_cast<unsigned long>(snapix::core.events.droppedCount()));
    lastMemPrint = millis();
  }

  // Poll input and push events to queue
  snapix::core.input.poll();

  // Auto-sleep after inactivity
  const auto autoSleepTimeout = snapix::core.settings.getAutoSleepTimeoutMs();
  const bool wifiActive = snapix::core.network.isConnected() || snapix::core.network.isAPMode();
  if (wifiActive) {
    snapix::core.input.resetIdleTimer();
  }
  if (autoSleepTimeout > 0 && snapix::core.input.idleTimeMs() >= autoSleepTimeout) {
    LOG_INF(TAG, "Auto-sleep after %lu ms idle", autoSleepTimeout);
    stateMachine.init(snapix::core, snapix::StateId::Sleep);
    return;
  }

  // Power button sleep check: track held time that excludes long rendering gaps
  // where button state changes could have been missed by inputManager
  {
    static unsigned long powerHeldSinceMs = 0;
    static unsigned long prevPowerCheckMs = 0;
    const unsigned long loopGap = loopStartTime - prevPowerCheckMs;
    prevPowerCheckMs = loopStartTime;

    if (inputManager.isPressed(InputManager::BTN_POWER)) {
      if (powerHeldSinceMs == 0 || loopGap > 100) {
        powerHeldSinceMs = loopStartTime;
      }
      if (loopStartTime - powerHeldSinceMs > snapix::core.settings.getPowerButtonDuration()) {
        stateMachine.init(snapix::core, snapix::StateId::Sleep);
        return;
      }
    } else {
      powerHeldSinceMs = 0;
    }
  }

  // CPU frequency scaling: drop to 10 MHz after idle to save battery,
  // restore full speed on any activity. Must run BEFORE stateMachine.update()
  // so rendering always happens at full CPU/SPI speed after wake.
  // Idea: CrossPoint HalPowerManager by @ngxson (https://github.com/ngxson)
  static constexpr unsigned long kIdlePowerSavingMs = 3000;
  if (currentBootMode == snapix::BootMode::READER) {
    if (snapix::core.input.idleTimeMs() >= kIdlePowerSavingMs) {
      snapix::core.cpu.throttle();
    } else {
      snapix::core.cpu.unthrottle();
    }
  }

  // Update state machine (handles transitions and rendering)
  const unsigned long activityStartTime = millis();
  stateMachine.update(snapix::core);
  const unsigned long activityDuration = millis() - activityStartTime;

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG(TAG, "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // Increase delay after idle to save power (~4x less CPU load)
  // Idea: https://github.com/crosspoint-reader/crosspoint-reader/commit/0991782 by @ngxson (https://github.com/ngxson)
  delay(snapix::core.cpu.loopDelayMs());
}
