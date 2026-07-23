#pragma once

#include <LittleFS.h>

#include <cstdint>
#include <functional>
#include <string>

struct ImageConvertConfig {
  int maxWidth = 0;
  int maxHeight = 0;
  bool quickMode = false;
  bool oneBit = false;
  const char* logTag = nullptr;
  bool outputOnLittleFs = false;
  std::function<bool()> shouldAbort;
};

class ImageConverterFactory {
 public:
  static bool convertToBmp(const std::string&, const std::string& target,
                           const ImageConvertConfig& config) {
    ++conversionCalls;
    if (config.shouldAbort && config.shouldAbort()) return false;
    File out = LittleFS.open(target.c_str(), "w");
    if (!out) return false;
    static constexpr uint8_t kMockBmp[] = {'B', 'M', 0, 0};
    const bool ok = out.write(kMockBmp, sizeof(kMockBmp)) == sizeof(kMockBmp);
    out.close();
    return ok;
  }

  static inline int conversionCalls = 0;
};
