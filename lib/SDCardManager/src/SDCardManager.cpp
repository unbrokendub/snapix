#include "SDCardManager.h"

#include <Logging.h>
#include <SharedSpiLock.h>

#define TAG "SD"

namespace {
constexpr uint8_t SD_CS = 12;
constexpr uint32_t SPI_FQ = 40000000;
}  // namespace

SDCardManager SDCardManager::instance;

SDCardManager::SDCardManager() : sd() {}

bool SDCardManager::begin() {
  if (!sd.begin(SD_CS, SPI_FQ)) {
    LOG_ERR(TAG, "SD card not detected");
    initialized = false;
  } else {
    LOG_INF(TAG, "SD card detected");
    initialized = true;
  }

  return initialized;
}

void SDCardManager::end() {
  if (initialized) {
    sd.end();
    initialized = false;
  }
}

bool SDCardManager::ready() const { return initialized; }

std::vector<String> SDCardManager::listFiles(const char* path, const int maxFiles) {
  std::vector<String> ret;
  if (!initialized) {
    LOG_ERR(TAG, "not initialized, returning empty list");
    return ret;
  }

  snapix::spi::SharedBusLock lk;
  auto root = sd.open(path);
  if (!root) {
    LOG_ERR(TAG, "Failed to open directory");
    return ret;
  }
  if (!root.isDirectory()) {
    LOG_ERR(TAG, "Path is not a directory");
    root.close();
    return ret;
  }

  int count = 0;
  char name[128];
  auto f = root.openNextFile();
  while (f && count < maxFiles) {
    if (f.isDirectory()) {
      f.close();
      f = root.openNextFile();
      continue;
    }
    f.getName(name, sizeof(name));
    ret.emplace_back(name);
    f.close();
    count++;
    f = root.openNextFile();
  }
  if (f) f.close();
  root.close();
  return ret;
}

String SDCardManager::readFile(const char* path) {
  if (!initialized) {
    LOG_ERR(TAG, "not initialized; cannot read file");
    return {""};
  }

  snapix::spi::SharedBusLock lk;
  FsFile f;
  if (!openFileForRead("SD", path, f)) {
    return {""};
  }

  constexpr size_t maxSize = 50000;  // Limit to 50KB
  const size_t fileSize = f.size();
  const size_t toRead = (fileSize < maxSize) ? fileSize : maxSize;

  String content;
  content.reserve(toRead);

  uint8_t buf[256];
  size_t readSize = 0;
  while (f.available() && readSize < toRead) {
    const size_t chunkSize = min(sizeof(buf), toRead - readSize);
    const int n = f.read(buf, chunkSize);
    if (n <= 0) break;
    content.concat(reinterpret_cast<char*>(buf), static_cast<size_t>(n));
    readSize += static_cast<size_t>(n);
  }
  f.close();
  return content;
}

bool SDCardManager::readFileToStream(const char* path, Print& out, const size_t chunkSize) {
  if (!initialized) {
    LOG_ERR(TAG, "SD card not initialized");
    return false;
  }

  snapix::spi::SharedBusLock lk;
  FsFile f;
  if (!openFileForRead("SD", path, f)) {
    return false;
  }

  constexpr size_t localBufSize = 256;
  uint8_t buf[localBufSize];
  const size_t toRead = (chunkSize == 0) ? localBufSize : (chunkSize < localBufSize ? chunkSize : localBufSize);

  while (f.available()) {
    const int r = f.read(buf, toRead);
    if (r > 0) {
      out.write(buf, static_cast<size_t>(r));
    } else {
      break;
    }
  }

  f.close();
  return true;
}

size_t SDCardManager::readFileToBuffer(const char* path, char* buffer, const size_t bufferSize, const size_t maxBytes) {
  if (!buffer || bufferSize == 0) return 0;
  if (!initialized) {
    LOG_ERR(TAG, "SD card not initialized");
    buffer[0] = '\0';
    return 0;
  }

  snapix::spi::SharedBusLock lk;
  FsFile f;
  if (!openFileForRead("SD", path, f)) {
    buffer[0] = '\0';
    return 0;
  }

  const size_t maxToRead = (maxBytes == 0) ? (bufferSize - 1) : min(maxBytes, bufferSize - 1);
  size_t total = 0;

  while (f.available() && total < maxToRead) {
    constexpr size_t chunk = 64;
    const size_t want = maxToRead - total;
    const size_t readLen = (want < chunk) ? want : chunk;
    const int r = f.read(buffer + total, readLen);
    if (r > 0) {
      total += static_cast<size_t>(r);
    } else {
      break;
    }
  }

  buffer[total] = '\0';
  f.close();
  return total;
}

bool SDCardManager::writeFile(const char* path, const String& content) {
  if (!initialized) {
    LOG_ERR(TAG, "SD card not initialized");
    return false;
  }

  snapix::spi::SharedBusLock lk;
  // Remove existing file so we perform an overwrite rather than append
  if (sd.exists(path)) {
    sd.remove(path);
  }

  FsFile f;
  if (!openFileForWrite("SD", path, f)) {
    return false;
  }

  const size_t written = f.print(content);
  f.close();
  return written == content.length();
}

bool SDCardManager::ensureDirectoryExists(const char* path) {
  if (!initialized) {
    LOG_ERR(TAG, "SD card not initialized");
    return false;
  }

  snapix::spi::SharedBusLock lk;
  // Check if directory already exists
  if (sd.exists(path)) {
    FsFile dir = sd.open(path);
    if (dir && dir.isDirectory()) {
      dir.close();
      return true;
    }
    dir.close();
  }

  // Create the directory
  if (sd.mkdir(path)) {
    LOG_INF(TAG, "Created directory: %s", path);
    return true;
  } else {
    LOG_ERR(TAG, "Failed to create directory: %s", path);
    return false;
  }
}

FsFile SDCardManager::open(const char* path, oflag_t oflag) {
  snapix::spi::SharedBusLock lk;
  return sd.open(path, oflag);
}

bool SDCardManager::mkdir(const char* path, bool pFlag) {
  // Retry mkdir on transient failures (FAT directory cluster cache eviction
  // races with the directory walk under memory pressure).  Skip retries when
  // the path now exists — that means a concurrent caller (or a previous
  // attempt that returned false but actually succeeded) created it.
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) {
      delay(50);
      snapix::spi::SharedBusLock lk;
      if (sd.exists(path)) return true;
    }
    {
      snapix::spi::SharedBusLock lk;
      if (sd.mkdir(path, pFlag)) return true;
    }
  }
  return false;
}

bool SDCardManager::exists(const char* path) {
  snapix::spi::SharedBusLock lk;
  return sd.exists(path);
}

bool SDCardManager::remove(const char* path) {
  snapix::spi::SharedBusLock lk;
  return sd.remove(path);
}

bool SDCardManager::rmdir(const char* path) {
  snapix::spi::SharedBusLock lk;
  return sd.rmdir(path);
}

bool SDCardManager::rename(const char* path, const char* newPath) {
  snapix::spi::SharedBusLock lk;
  return sd.rename(path, newPath);
}

bool SDCardManager::openFileForRead(const char* moduleName, const char* path, FsFile& file) {
  // SdFat's directory cache occasionally returns false-negatives for both
  // exists() AND the first open() attempt under memory pressure — observed
  // in the wild moments after a saveMetaCache() returned success.  Drop the
  // redundant exists() precheck (sd.open() does its own lookup) and retry
  // a couple of times with a brief delay to let the FAT cache settle.
  // Release the SPI lock between attempts so the display can use the bus.
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) delay(50);
    {
      snapix::spi::SharedBusLock lk;
      file = sd.open(path, O_RDONLY);
      if (file) {
        return true;
      }
    }
  }
  LOG_DBG(moduleName, "File does not exist: %s", path);
  return false;
}

bool SDCardManager::openFileForRead(const char* moduleName, const std::string& path, FsFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool SDCardManager::openFileForRead(const char* moduleName, const String& path, FsFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool SDCardManager::openFileForWrite(const char* moduleName, const char* path, FsFile& file) {
  // Same defensive retry as openFileForRead — under memory pressure SdFat
  // occasionally fails to allocate a directory entry slot on the first try
  // (cache eviction races with the directory walk).  A short pause and a
  // second attempt usually succeeds.
  for (int attempt = 0; attempt < 3; attempt++) {
    if (attempt > 0) delay(50);
    {
      snapix::spi::SharedBusLock lk;
      file = sd.open(path, O_RDWR | O_CREAT | O_TRUNC);
      if (file) {
        return true;
      }
    }
  }
  LOG_ERR(moduleName, "Failed to open file for writing: %s", path);
  return false;
}

bool SDCardManager::openFileForWrite(const char* moduleName, const std::string& path, FsFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool SDCardManager::openFileForWrite(const char* moduleName, const String& path, FsFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool SDCardManager::removeDir(const char* path) {
  snapix::spi::SharedBusLock lk;
  auto dir = sd.open(path);
  if (!dir) {
    LOG_DBG(TAG, "removeDir: cannot open %s", path);
    return false;
  }
  if (!dir.isDirectory()) {
    dir.close();
    LOG_DBG(TAG, "removeDir: not a directory %s", path);
    return false;
  }

  bool allOk = true;
  auto file = dir.openNextFile();
  char name[128];
  // Safety bound — SdFat's openNextFile() has been observed in the wild
  // returning the same pathological entry repeatedly (empty name, FAT cache
  // staleness under memory pressure).  Without a hard cap this turns into a
  // silent infinite loop; cap at a generous 4096 entries (more than any
  // realistic snapix cache subtree).
  int iterations = 0;
  constexpr int kMaxEntries = 4096;
  while (file && iterations++ < kMaxEntries) {
    file.getName(name, sizeof(name));

    // Skip pathological entries: empty name, "." and "..".  These appear
    // when the FAT directory cache loses entries (memory pressure) or when
    // the filesystem returns synthetic entries.  Without this check, an
    // empty name builds a path like ".snapix/cache/foo/" and sd.remove()
    // tries to delete the directory as if it were a file — fails forever.
    const bool skipEntry = (name[0] == '\0') ||
                           (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')));
    if (skipEntry) {
      LOG_DBG(TAG, "removeDir: skipping synthetic entry '%s' in %s", name, path);
      file.close();
      file = dir.openNextFile();
      continue;
    }

    String filePath = path;
    if (!filePath.endsWith("/")) {
      filePath += "/";
    }
    filePath += name;

    if (file.isDirectory()) {
      file.close();
      if (!removeDir(filePath.c_str())) {
        LOG_ERR(TAG, "removeDir: failed subdir %s", filePath.c_str());
        allOk = false;
      }
    } else {
      file.close();
      if (!sd.remove(filePath.c_str())) {
        LOG_ERR(TAG, "removeDir: failed file %s", filePath.c_str());
        allOk = false;
      }
    }
    file = dir.openNextFile();
  }
  if (iterations >= kMaxEntries) {
    LOG_ERR(TAG, "removeDir: aborted after %d iterations on %s (FAT cache likely stale)", iterations, path);
    allOk = false;
  }

  dir.close();

  if (!sd.rmdir(path)) {
    LOG_ERR(TAG, "removeDir: rmdir failed %s (allOk=%u)", path, static_cast<unsigned>(allOk));
    return false;
  }
  return true;
}
