#include "Storage.h"

#include <SDCardManager.h>

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

  if (!SdMan.openFileForRead("DRV", path, out)) {
    return ErrVoid(Error::FileNotFound);
  }

  return Ok();
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
  // v2.0.76 hotfix: SDFat `rename(from, to)` REFUSES to overwrite an existing
  // destination — it fails silently and the .tmp file lingers.  v2.0.75
  // assumed atomic overwrite (LittleFS does this), so every Settings /
  // Progress / Bookmark save after the first failed → user saw the same
  // "rename failed; keeping old file" error on every device run.  Remove
  // the destination first.  Not strictly atomic across the two ops, but
  // power loss between them leaves the .tmp file intact for manual recovery;
  // the partial corrupt state (no destination, .tmp present) is detectable
  // and safer than the alternative of failing to update altogether.
  if (SdMan.exists(toPath)) {
    SdMan.remove(toPath);
  }
  return SdMan.rename(fromPath, toPath);
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
