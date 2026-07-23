#include "SnapixSettings.h"

#include <Logging.h>
#include <SDCardManager.h>
#include <SdFat.h>
#include <Serialization.h>

#include <string>

#include "../FontManager.h"
#include "../Theme.h"
#include "../config.h"
#include "../drivers/Storage.h"

#define TAG "SETTINGS"

namespace snapix {

namespace {
// Magic signature to identify Snapix settings files ("PPXS" in little-endian)
constexpr uint32_t SETTINGS_MAGIC = 0x53585050;
// Minimum version we can read (allows backward compatibility)
constexpr uint8_t MIN_SETTINGS_VERSION = 3;
// Version 9: Moved frontButtonLayout from Theme to Settings
constexpr uint8_t SETTINGS_FILE_VERSION = 12;
// Increment this when adding new persisted settings fields
constexpr uint8_t SETTINGS_COUNT = 30;
constexpr uint16_t SETTINGS_FIELD_SIZES[SETTINGS_COUNT] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    32, 256, 1, 1, 1, 256, 128, 2, 1, 1, 1, 1, 1, 1,
};

constexpr uint32_t settingsSerializedSize(const uint8_t fieldCount) {
  uint32_t size = sizeof(SETTINGS_MAGIC) + sizeof(SETTINGS_FILE_VERSION) +
                  sizeof(SETTINGS_COUNT);
  for (uint8_t i = 0; i < fieldCount && i < SETTINGS_COUNT; ++i) {
    size += SETTINGS_FIELD_SIZES[i];
  }
  return size;
}

constexpr uint32_t SETTINGS_SERIALIZED_SIZE = settingsSerializedSize(SETTINGS_COUNT);
static_assert(SETTINGS_SERIALIZED_SIZE == 705, "Unexpected settings layout");

bool recoverDirectSettingsFile() {
  if (SdMan.exists(SNAPIX_SETTINGS_FILE)) return true;

  const std::string backupPath = std::string(SNAPIX_SETTINGS_FILE) + ".bak";
  const std::string tmpPath = std::string(SNAPIX_SETTINGS_FILE) + ".tmp";
  if (SdMan.exists(backupPath.c_str()) &&
      SdMan.rename(backupPath.c_str(), SNAPIX_SETTINGS_FILE)) {
    return true;
  }
  if (SdMan.exists(tmpPath.c_str()) &&
      SdMan.rename(tmpPath.c_str(), SNAPIX_SETTINGS_FILE)) {
    return true;
  }
  return false;
}

bool replaceDirectSettingsFile(const std::string& tmpPath) {
  const std::string backupPath = std::string(SNAPIX_SETTINGS_FILE) + ".bak";
  const bool hadDestination = SdMan.exists(SNAPIX_SETTINGS_FILE);
  bool hasBackup = SdMan.exists(backupPath.c_str());
  if (hadDestination) {
    if (hasBackup && !SdMan.remove(backupPath.c_str())) return false;
    if (!SdMan.rename(SNAPIX_SETTINGS_FILE, backupPath.c_str())) return false;
    hasBackup = true;
  }
  if (SdMan.rename(tmpPath.c_str(), SNAPIX_SETTINGS_FILE)) {
    if (hasBackup) SdMan.remove(backupPath.c_str());
    return true;
  }
  if (hasBackup && !SdMan.exists(SNAPIX_SETTINGS_FILE)) {
    SdMan.rename(backupPath.c_str(), SNAPIX_SETTINGS_FILE);
  }
  return false;
}
}  // namespace

Result<void> Settings::save(drivers::Storage& storage) const {
  // Make sure the SD-side settings directory exists.  v2.0.60: page cache
  // moved to LittleFS — PageCache::create ensures the LittleFS cache dir
  // lazily, no upfront mkdir needed here.
  storage.mkdir(SNAPIX_DIR);

  // v2.0.75: atomic write — `.tmp` + rename so a power loss mid-write
  // doesn't corrupt the existing settings.  Pre-2.0.75 wrote 27 fields
  // sequentially to the final path; if power cut between magic+version
  // and the rest, load() saw wrong magic / wrong version → DELETED the
  // file (line 93) and dropped the user back to defaults.
  const std::string tmpPath = std::string(SNAPIX_SETTINGS_FILE) + ".tmp";
  FsFile outputFile;
  auto result = storage.openWrite(tmpPath.c_str(), outputFile);
  if (!result.ok()) {
    return result;
  }

  serialization::writePod(outputFile, SETTINGS_MAGIC);
  serialization::writePod(outputFile, SETTINGS_FILE_VERSION);
  serialization::writePod(outputFile, SETTINGS_COUNT);
  serialization::writePod(outputFile, sleepScreen);
  serialization::writePod(outputFile, textLayout);
  serialization::writePod(outputFile, shortPwrBtn);
  serialization::writePod(outputFile, statusBar);
  serialization::writePod(outputFile, orientation);
  serialization::writePod(outputFile, fontSize);
  serialization::writePod(outputFile, pagesPerRefresh);
  serialization::writePod(outputFile, sideButtonLayout);
  serialization::writePod(outputFile, autoSleepMinutes);
  serialization::writePod(outputFile, paragraphAlignment);
  serialization::writePod(outputFile, hyphenation);
  serialization::writePod(outputFile, textAntiAliasing);
  serialization::writePod(outputFile, showImages);
  serialization::writePod(outputFile, startupBehavior);
  serialization::writePod(outputFile, _reserved);
  serialization::writePod(outputFile, lineSpacing);
  // Write themeName as fixed-length string
  outputFile.write(reinterpret_cast<const uint8_t*>(themeName), sizeof(themeName));
  outputFile.write(reinterpret_cast<const uint8_t*>(lastBookPath), sizeof(lastBookPath));
  serialization::writePod(outputFile, pendingTransition);
  serialization::writePod(outputFile, transitionReturnTo);
  serialization::writePod(outputFile, sunlightFadingFix);
  outputFile.write(reinterpret_cast<const uint8_t*>(fileListDir), sizeof(fileListDir));
  outputFile.write(reinterpret_cast<const uint8_t*>(fileListSelectedName), sizeof(fileListSelectedName));
  serialization::writePod(outputFile, fileListSelectedIndex);
  serialization::writePod(outputFile, frontButtonLayout);
  serialization::writePod(outputFile, transitionFullRefresh);
  serialization::writePod(outputFile, pendingSleepWake);
  serialization::writePod(outputFile, sleepHoldTime);
  serialization::writePod(outputFile, bionicReading);
  serialization::writePod(outputFile, fakeBold);
  outputFile.flush();
  const uint32_t writtenBytes = outputFile.size();
  outputFile.close();
  if (writtenBytes != SETTINGS_SERIALIZED_SIZE) {
    LOG_ERR(TAG, "Short settings write %u/%u", static_cast<unsigned>(writtenBytes),
            static_cast<unsigned>(SETTINGS_SERIALIZED_SIZE));
    storage.remove(tmpPath.c_str());
    return ErrVoid(Error::IOError);
  }

  // Commit through Storage's backup/rollback replace.
  if (!storage.rename(tmpPath.c_str(), SNAPIX_SETTINGS_FILE)) {
    LOG_ERR(TAG, "Settings rename %s -> %s failed; keeping old file", tmpPath.c_str(), SNAPIX_SETTINGS_FILE);
    storage.remove(tmpPath.c_str());
    return ErrVoid(Error::SdCardNotFound);
  }
  LOG_INF(TAG, "Settings saved to file");
  return Ok();
}

Result<void> Settings::load(drivers::Storage& storage) {
  FsFile inputFile;
  auto result = storage.openRead(SNAPIX_SETTINGS_FILE, inputFile);
  if (!result.ok()) {
    return result;
  }

  // Check magic signature to detect incompatible settings files (e.g., from Crosspoint firmware)
  uint32_t magic = 0;
  if (!serialization::readPodChecked(inputFile, magic)) {
    inputFile.close();
    return ErrVoid(Error::FileCorrupted);
  }
  if (magic != SETTINGS_MAGIC) {
    LOG_ERR(TAG, "Invalid settings file (wrong magic 0x%08X), deleting", magic);
    inputFile.close();
    storage.remove(SNAPIX_SETTINGS_FILE);
    return ErrVoid(Error::UnsupportedVersion);
  }

  uint8_t version = 0;
  if (!serialization::readPodChecked(inputFile, version)) {
    inputFile.close();
    return ErrVoid(Error::FileCorrupted);
  }
  if (version < MIN_SETTINGS_VERSION || version > SETTINGS_FILE_VERSION) {
    LOG_ERR(TAG, "Deserialization failed: Unknown version %u", version);
    inputFile.close();
    return ErrVoid(Error::UnsupportedVersion);
  }

  uint8_t fileSettingsCount = 0;
  if (!serialization::readPodChecked(inputFile, fileSettingsCount)) {
    inputFile.close();
    return ErrVoid(Error::FileCorrupted);
  }

  // Reject impossible counts and truncated payloads before mutating any
  // settings fields.  This keeps a torn file from producing a half-old,
  // half-default configuration that is then persisted as if it were valid.
  if (fileSettingsCount > SETTINGS_COUNT) {
    LOG_ERR(TAG, "fileSettingsCount %u exceeds max %u", fileSettingsCount, SETTINGS_COUNT);
    inputFile.close();
    return ErrVoid(Error::FileCorrupted);
  }
  if (inputFile.size() < settingsSerializedSize(fileSettingsCount)) {
    LOG_ERR(TAG, "Truncated settings file");
    inputFile.close();
    return ErrVoid(Error::FileCorrupted);
  }

  // Load settings that exist (support older files with fewer fields)
  // readPodValidated keeps default value if read value >= maxValue
  uint8_t settingsRead = 0;
  do {
    serialization::readPodValidated(inputFile, sleepScreen, uint8_t(5));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, textLayout, uint8_t(3));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, shortPwrBtn, uint8_t(3));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, statusBar, uint8_t(3));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, orientation, uint8_t(4));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, fontSize, uint8_t(4));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, pagesPerRefresh, uint8_t(8));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, sideButtonLayout, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, autoSleepMinutes, uint8_t(5));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, paragraphAlignment, uint8_t(4));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, hyphenation, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, textAntiAliasing, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, showImages, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, startupBehavior, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, _reserved, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, lineSpacing, uint8_t(10));
    if (++settingsRead >= fileSettingsCount) break;
    // Read themeName as fixed-length string
    inputFile.read(reinterpret_cast<uint8_t*>(themeName), sizeof(themeName));
    themeName[sizeof(themeName) - 1] = '\0';  // Ensure null-terminated
    if (++settingsRead >= fileSettingsCount) break;
    inputFile.read(reinterpret_cast<uint8_t*>(lastBookPath), sizeof(lastBookPath));
    lastBookPath[sizeof(lastBookPath) - 1] = '\0';
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, pendingTransition, uint8_t(3));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, transitionReturnTo, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, sunlightFadingFix, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    inputFile.read(reinterpret_cast<uint8_t*>(fileListDir), sizeof(fileListDir));
    fileListDir[sizeof(fileListDir) - 1] = '\0';
    if (++settingsRead >= fileSettingsCount) break;
    inputFile.read(reinterpret_cast<uint8_t*>(fileListSelectedName), sizeof(fileListSelectedName));
    fileListSelectedName[sizeof(fileListSelectedName) - 1] = '\0';
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, fileListSelectedIndex);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, frontButtonLayout, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, transitionFullRefresh, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, pendingSleepWake, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, sleepHoldTime, uint8_t(5));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, bionicReading, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, fakeBold, uint8_t(3));
    if (++settingsRead >= fileSettingsCount) break;
  } while (false);

  // Migrate font size from version < 8 (enum values shifted +1 for FontXSmall)
  // Old: FontSmall=0, FontMedium=1, FontLarge=2
  // New: FontXSmall=0, FontSmall=1, FontMedium=2, FontLarge=3
  // TODO: Delete this migration when MIN_SETTINGS_VERSION >= 8 (after version 10)
  if (version < 8) {
    fontSize++;
  }

  inputFile.close();
  LOG_INF(TAG, "Settings loaded from file");
  return Ok();
}

int Settings::getReaderFontId(const Theme& theme) const {
  // The theme's pre-set readerFontId* fields are honoured as the fallback
  // when no external SD font is requested.  This lets builtin themes that
  // bake their own embedded-font selection (e.g. the "JetBrains Mono"
  // theme) actually flow through to the renderer — without this, the
  // FONT_MANAGER fallback was always READER_FONT_ID (the existing
  // reader_2b family) and the embedded JetBrains Mono variants were never
  // consulted on book render.
  switch (fontSize) {
    case FontXSmall: {
      const int builtin = (theme.readerFontIdXSmall != 0) ? theme.readerFontIdXSmall : READER_FONT_ID_XSMALL;
      return FONT_MANAGER.getReaderFontId(theme.readerFontFamilyXSmall, builtin);
    }
    case FontMedium: {
      const int builtin = (theme.readerFontIdMedium != 0) ? theme.readerFontIdMedium : READER_FONT_ID_MEDIUM;
      return FONT_MANAGER.getReaderFontId(theme.readerFontFamilyMedium, builtin);
    }
    case FontLarge: {
      const int builtin = (theme.readerFontIdLarge != 0) ? theme.readerFontIdLarge : READER_FONT_ID_LARGE;
      return FONT_MANAGER.getReaderFontId(theme.readerFontFamilyLarge, builtin);
    }
    default: {  // FontSmall
      const int builtin = (theme.readerFontId != 0) ? theme.readerFontId : READER_FONT_ID;
      return FONT_MANAGER.getReaderFontId(theme.readerFontFamilySmall, builtin);
    }
  }
}

const char* Settings::getReaderFontFamily(const Theme& theme) const {
  switch (fontSize) {
    case FontXSmall:
      return theme.readerFontFamilyXSmall;
    case FontMedium:
      return theme.readerFontFamilyMedium;
    case FontLarge:
      return theme.readerFontFamilyLarge;
    default:
      return theme.readerFontFamilySmall;
  }
}

bool Settings::hasExternalReaderFont(const Theme& theme) const {
  const char* family = getReaderFontFamily(theme);
  return family && *family;
}

int Settings::getSuperSubFontId(const Theme& theme) const {
  // v3.6.0 — superscript/subscript render in the built-in XSmall reader font
  // (always present in flash, so no extra font load), regardless of the body
  // size.  When the body is already XSmall there's no shrink, which is fine.
  const int builtin = (theme.readerFontIdXSmall != 0) ? theme.readerFontIdXSmall : READER_FONT_ID_XSMALL;
  return FONT_MANAGER.getReaderFontId(theme.readerFontFamilyXSmall, builtin);
}

RenderConfig Settings::getRenderConfig(const Theme& theme, uint16_t viewportWidth, uint16_t viewportHeight) const {
  return RenderConfig(getReaderFontId(theme), getLineCompression(), getIndentLevel(), getSpacingLevel(),
                      paragraphAlignment, static_cast<bool>(hyphenation), static_cast<bool>(showImages),
                      static_cast<bool>(bionicReading), fakeBold, viewportWidth, viewportHeight,
                      getSuperSubFontId(theme));
}

// Legacy methods that use SdMan directly (for early init before Core)
bool Settings::saveToFile() const {
  SdMan.mkdir(SNAPIX_DIR);
  // v2.0.60: page cache lives on LittleFS now; no SD cache dir to mkdir here.

  const std::string tmpPath = std::string(SNAPIX_SETTINGS_FILE) + ".tmp";
  FsFile outputFile;
  if (!SdMan.openFileForWrite("SET", tmpPath.c_str(), outputFile)) {
    return false;
  }

  serialization::writePod(outputFile, SETTINGS_MAGIC);
  serialization::writePod(outputFile, SETTINGS_FILE_VERSION);
  serialization::writePod(outputFile, SETTINGS_COUNT);
  serialization::writePod(outputFile, sleepScreen);
  serialization::writePod(outputFile, textLayout);
  serialization::writePod(outputFile, shortPwrBtn);
  serialization::writePod(outputFile, statusBar);
  serialization::writePod(outputFile, orientation);
  serialization::writePod(outputFile, fontSize);
  serialization::writePod(outputFile, pagesPerRefresh);
  serialization::writePod(outputFile, sideButtonLayout);
  serialization::writePod(outputFile, autoSleepMinutes);
  serialization::writePod(outputFile, paragraphAlignment);
  serialization::writePod(outputFile, hyphenation);
  serialization::writePod(outputFile, textAntiAliasing);
  serialization::writePod(outputFile, showImages);
  serialization::writePod(outputFile, startupBehavior);
  serialization::writePod(outputFile, _reserved);
  serialization::writePod(outputFile, lineSpacing);
  outputFile.write(reinterpret_cast<const uint8_t*>(themeName), sizeof(themeName));
  outputFile.write(reinterpret_cast<const uint8_t*>(lastBookPath), sizeof(lastBookPath));
  serialization::writePod(outputFile, pendingTransition);
  serialization::writePod(outputFile, transitionReturnTo);
  serialization::writePod(outputFile, sunlightFadingFix);
  outputFile.write(reinterpret_cast<const uint8_t*>(fileListDir), sizeof(fileListDir));
  outputFile.write(reinterpret_cast<const uint8_t*>(fileListSelectedName), sizeof(fileListSelectedName));
  serialization::writePod(outputFile, fileListSelectedIndex);
  serialization::writePod(outputFile, frontButtonLayout);
  serialization::writePod(outputFile, transitionFullRefresh);
  serialization::writePod(outputFile, pendingSleepWake);
  serialization::writePod(outputFile, sleepHoldTime);
  serialization::writePod(outputFile, bionicReading);
  serialization::writePod(outputFile, fakeBold);
  outputFile.flush();
  const uint32_t writtenBytes = outputFile.size();
  outputFile.close();

  if (writtenBytes != SETTINGS_SERIALIZED_SIZE) {
    LOG_ERR(TAG, "Short settings write %u/%u", static_cast<unsigned>(writtenBytes),
            static_cast<unsigned>(SETTINGS_SERIALIZED_SIZE));
    SdMan.remove(tmpPath.c_str());
    return false;
  }
  if (!replaceDirectSettingsFile(tmpPath)) {
    LOG_ERR(TAG, "Settings replace failed; previous version restored");
    return false;
  }

  LOG_INF(TAG, "Settings saved to file");
  return true;
}

bool Settings::loadFromFile() {
  recoverDirectSettingsFile();
  FsFile inputFile;
  if (!SdMan.openFileForRead("SET", SNAPIX_SETTINGS_FILE, inputFile)) {
    return false;
  }

  // Check magic signature to detect incompatible settings files (e.g., from Crosspoint firmware)
  uint32_t magic = 0;
  if (!serialization::readPodChecked(inputFile, magic)) {
    inputFile.close();
    return false;
  }
  if (magic != SETTINGS_MAGIC) {
    LOG_ERR(TAG, "Invalid settings file (wrong magic 0x%08X), deleting", magic);
    inputFile.close();
    SdMan.remove(SNAPIX_SETTINGS_FILE);
    return false;
  }

  uint8_t version = 0;
  if (!serialization::readPodChecked(inputFile, version)) {
    inputFile.close();
    return false;
  }
  if (version < MIN_SETTINGS_VERSION || version > SETTINGS_FILE_VERSION) {
    LOG_ERR(TAG, "Deserialization failed: Unknown version %u", version);
    inputFile.close();
    return false;
  }

  uint8_t fileSettingsCount = 0;
  if (!serialization::readPodChecked(inputFile, fileSettingsCount)) {
    inputFile.close();
    return false;
  }

  if (fileSettingsCount > SETTINGS_COUNT) {
    LOG_ERR(TAG, "fileSettingsCount %u exceeds max %u", fileSettingsCount, SETTINGS_COUNT);
    inputFile.close();
    return false;
  }
  if (inputFile.size() < settingsSerializedSize(fileSettingsCount)) {
    LOG_ERR(TAG, "Truncated settings file");
    inputFile.close();
    return false;
  }

  uint8_t settingsRead = 0;
  do {
    serialization::readPodValidated(inputFile, sleepScreen, uint8_t(5));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, textLayout, uint8_t(3));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, shortPwrBtn, uint8_t(3));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, statusBar, uint8_t(3));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, orientation, uint8_t(4));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, fontSize, uint8_t(4));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, pagesPerRefresh, uint8_t(8));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, sideButtonLayout, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, autoSleepMinutes, uint8_t(5));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, paragraphAlignment, uint8_t(4));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, hyphenation, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, textAntiAliasing, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, showImages, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, startupBehavior, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, _reserved, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, lineSpacing, uint8_t(10));
    if (++settingsRead >= fileSettingsCount) break;
    inputFile.read(reinterpret_cast<uint8_t*>(themeName), sizeof(themeName));
    themeName[sizeof(themeName) - 1] = '\0';
    if (++settingsRead >= fileSettingsCount) break;
    inputFile.read(reinterpret_cast<uint8_t*>(lastBookPath), sizeof(lastBookPath));
    lastBookPath[sizeof(lastBookPath) - 1] = '\0';
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, pendingTransition, uint8_t(3));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, transitionReturnTo, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, sunlightFadingFix, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    inputFile.read(reinterpret_cast<uint8_t*>(fileListDir), sizeof(fileListDir));
    fileListDir[sizeof(fileListDir) - 1] = '\0';
    if (++settingsRead >= fileSettingsCount) break;
    inputFile.read(reinterpret_cast<uint8_t*>(fileListSelectedName), sizeof(fileListSelectedName));
    fileListSelectedName[sizeof(fileListSelectedName) - 1] = '\0';
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPod(inputFile, fileListSelectedIndex);
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, frontButtonLayout, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, transitionFullRefresh, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, pendingSleepWake, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, sleepHoldTime, uint8_t(5));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, bionicReading, uint8_t(2));
    if (++settingsRead >= fileSettingsCount) break;
    serialization::readPodValidated(inputFile, fakeBold, uint8_t(3));
    if (++settingsRead >= fileSettingsCount) break;
  } while (false);

  // Migrate font size from version < 8 (enum values shifted +1 for FontXSmall)
  // Old: FontSmall=0, FontMedium=1, FontLarge=2
  // New: FontXSmall=0, FontSmall=1, FontMedium=2, FontLarge=3
  // TODO: Delete this migration when MIN_SETTINGS_VERSION >= 8 (after version 10)
  if (version < 8) {
    fontSize++;
  }

  inputFile.close();
  LOG_INF(TAG, "Settings loaded from file");
  return true;
}

}  // namespace snapix
