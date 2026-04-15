#include "FsHelpers.h"

#include <cstring>
#include <vector>

namespace {
// Folders/files to hide from file browsers (UI and web interface)
const char* HIDDEN_FS_ITEMS[] = {"System Volume Information", "LOST.DIR", "$RECYCLE.BIN", "config", "XTCache", "sleep"};
constexpr size_t HIDDEN_FS_ITEMS_COUNT = sizeof(HIDDEN_FS_ITEMS) / sizeof(HIDDEN_FS_ITEMS[0]);
}  // namespace

bool FsHelpers::isHiddenFsItem(const char* name) {
  for (size_t i = 0; i < HIDDEN_FS_ITEMS_COUNT; i++) {
    if (strcmp(name, HIDDEN_FS_ITEMS[i]) == 0) return true;
  }
  return false;
}

std::string FsHelpers::normalisePath(const std::string& path) {
  std::vector<std::string> components;
  std::string component;

  auto pushComponent = [&]() {
    if (component == "..") {
      if (!components.empty()) {
        components.pop_back();
      }
    } else if (component != ".") {
      components.push_back(component);
    }
    component.clear();
  };

  for (const auto c : path) {
    if (c == '/') {
      if (!component.empty()) {
        pushComponent();
      }
    } else {
      component += c;
    }
  }

  if (!component.empty()) {
    pushComponent();
  }

  std::string result;
  for (const auto& c : components) {
    if (!result.empty()) {
      result += "/";
    }
    result += c;
  }

  return result;
}

std::string FsHelpers::percentDecode(const std::string& encoded) {
  std::string decoded;
  decoded.reserve(encoded.size());
  for (size_t i = 0; i < encoded.size(); ++i) {
    if (encoded[i] == '%' && i + 2 < encoded.size()) {
      auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return 10 + c - 'a';
        if (c >= 'A' && c <= 'F') return 10 + c - 'A';
        return -1;
      };
      const int hi = hexVal(encoded[i + 1]);
      const int lo = hexVal(encoded[i + 2]);
      if (hi >= 0 && lo >= 0) {
        decoded += static_cast<char>((hi << 4) | lo);
        i += 2;
        continue;
      }
    }
    decoded += encoded[i];
  }
  return decoded;
}

std::string FsHelpers::stripQueryAndFragment(const std::string& ref) {
  size_t pos = ref.find('#');
  const size_t qpos = ref.find('?');
  if (qpos != std::string::npos && (pos == std::string::npos || qpos < pos)) {
    pos = qpos;
  }
  return pos != std::string::npos ? ref.substr(0, pos) : ref;
}
