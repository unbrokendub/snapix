#pragma once
#include <Logging.h>

#include <iostream>

// v2.0.60: file-based overloads were FsFile-specific (SdFat).  PageCache
// migrating to LittleFS needs Arduino-File-based versions too.  Solution:
// template the file overloads on FileT — both FsFile (SdFat) and File
// (Arduino FS) have compatible read/write/seekCur signatures, so a single
// templated implementation serves both.  std::ostream/istream stay as
// non-template overloads (they don't share an interface with file types).
namespace serialization {
template <typename T>
static void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename FileT, typename T>
static void writePod(FileT& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
static void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename FileT, typename T>
static void readPod(FileT& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

template <typename FileT, typename T>
[[nodiscard]] static bool readPodChecked(FileT& file, T& value) {
  return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

static void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

template <typename FileT>
static void writeString(FileT& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

[[nodiscard]] static bool readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  if (!is.good()) {
    s.clear();
    return false;
  }
  if (len > 65536) {  // Sanity check: no string should be > 64KB
    s.clear();
    is.setstate(std::ios::failbit);
    return false;
  }
  s.resize(len);
  is.read(&s[0], len);
  return is.good();
}

template <typename FileT>
[[nodiscard]] static bool readString(FileT& file, std::string& s) {
  uint32_t len;
  if (file.read(reinterpret_cast<uint8_t*>(&len), sizeof(len)) != static_cast<int>(sizeof(len))) {
    s.clear();
    return false;
  }
  if (len > 65536) {  // Sanity check: no string should be > 64KB
    LOG_ERR("SERIAL", "String length %u exceeds max, file corrupt", len);
    s.clear();
    return false;
  }
  s.resize(len);
  if (len > 0 && file.read(reinterpret_cast<uint8_t*>(&s[0]), len) != static_cast<int>(len)) {
    s.clear();
    return false;
  }
  return true;
}

template <typename FileT>
[[nodiscard]] static bool skipString(FileT& file) {
  uint32_t len;
  if (file.read(reinterpret_cast<uint8_t*>(&len), sizeof(len)) != static_cast<int>(sizeof(len))) {
    return false;
  }
  if (len > 65536) {
    return false;
  }
  // Both FsFile (SdFat) and Arduino File expose seek(absolute pos) — use
  // current position + offset for a portable forward-skip.  FsFile also
  // has seekCur(rel) but Arduino File does not, so this construct is the
  // common-denominator API.
  return len == 0 || file.seek(file.position() + len);
}

template <typename FileT, typename T>
static void readPodValidated(FileT& file, T& value, T maxValue) {
  T temp;
  file.read(reinterpret_cast<uint8_t*>(&temp), sizeof(T));
  if (temp < maxValue) {
    value = temp;
  }
}
}  // namespace serialization
