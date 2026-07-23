#include "Storage.h"

#include <SDCardManager.h>

#include <string>

namespace snapix {
namespace drivers {

Result<void> Storage::init() {
  if (mounted_) {
    return Ok();
  }

  if (!SdMan.begin()) {
    return ErrVoid(Error::SdCardNotFound);
  }

  mounted_ = true;
  return Ok();
}

void Storage::shutdown() {
  // SdFat doesn't have an explicit shutdown
  mounted_ = false;
}

Result<void> Storage::openRead(const char* path, FsFile& out) {
  if (!mounted_) {
    return ErrVoid(Error::SdCardNotFound);
  }

  if (SdMan.openFileForRead("DRV", path, out)) {
    return Ok();
  }

  // Finish or roll back an interrupted recoverable replace.  A backup is
  // always preferred over a temp file: it is the last version known to have
  // been fully committed.  A lone `.tmp` is promoted for first-time saves.
  const std::string backupPath = std::string(path) + ".bak";
  const std::string tmpPath = std::string(path) + ".tmp";
  if (SdMan.exists(backupPath.c_str()) &&
      SdMan.rename(backupPath.c_str(), path) &&
      SdMan.openFileForRead("DRV", path, out)) {
    return Ok();
  }
  if (SdMan.exists(tmpPath.c_str()) &&
      SdMan.rename(tmpPath.c_str(), path) &&
      SdMan.openFileForRead("DRV", path, out)) {
    return Ok();
  }
  return ErrVoid(Error::FileNotFound);
}

Result<void> Storage::openWrite(const char* path, FsFile& out) {
  if (!mounted_) {
    return ErrVoid(Error::SdCardNotFound);
  }

  if (!SdMan.openFileForWrite("DRV", path, out)) {
    return ErrVoid(Error::FileNotFound);
  }

  return Ok();
}

Result<bool> Storage::exists(const char* path) {
  if (!mounted_) {
    return Err<bool>(Error::SdCardNotFound);
  }

  return Ok(SdMan.exists(path));
}

Result<void> Storage::remove(const char* path) {
  if (!mounted_) {
    return ErrVoid(Error::SdCardNotFound);
  }

  if (!SdMan.remove(path)) {
    return ErrVoid(Error::FileNotFound);
  }

  return Ok();
}

Result<void> Storage::mkdir(const char* path) {
  if (!mounted_) {
    return ErrVoid(Error::SdCardNotFound);
  }

  if (!SdMan.mkdir(path)) {
    return ErrVoid(Error::FileNotFound);
  }

  return Ok();
}

Result<void> Storage::rmdir(const char* path) {
  if (!mounted_) {
    return ErrVoid(Error::SdCardNotFound);
  }

  if (!SdMan.removeDir(path)) {
    return ErrVoid(Error::FileNotFound);
  }

  return Ok();
}

bool Storage::rename(const char* fromPath, const char* toPath) {
  if (!mounted_) return false;

  const std::string backupPath = std::string(toPath) + ".bak";
  const bool hadDestination = SdMan.exists(toPath);
  bool hasBackup = SdMan.exists(backupPath.c_str());
  if (hadDestination) {
    if (hasBackup && !SdMan.remove(backupPath.c_str())) return false;
    if (!SdMan.rename(toPath, backupPath.c_str())) return false;
    hasBackup = true;
  }

  if (SdMan.rename(fromPath, toPath)) {
    if (hasBackup) SdMan.remove(backupPath.c_str());
    return true;
  }

  // Best-effort rollback.  If power is lost before this point, openRead()
  // performs the same recovery from `.bak` on the next boot.
  if (hasBackup && !SdMan.exists(toPath)) {
    SdMan.rename(backupPath.c_str(), toPath);
  }
  return false;
}

Result<void> Storage::openDir(const char* path, FsFile& out) {
  if (!mounted_) {
    return ErrVoid(Error::SdCardNotFound);
  }

  out = SdMan.open(path);
  if (!out) {
    return ErrVoid(Error::FileNotFound);
  }

  return Ok();
}

Result<size_t> Storage::readToBuffer(const char* path, char* buffer, size_t bufferSize) {
  if (!mounted_) {
    return Err<size_t>(Error::SdCardNotFound);
  }

  size_t bytesRead = SdMan.readFileToBuffer(path, buffer, bufferSize);
  if (bytesRead == 0) {
    return Err<size_t>(Error::FileNotFound);
  }

  return Ok(bytesRead);
}

}  // namespace drivers
}  // namespace snapix
