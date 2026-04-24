#pragma once

#include <cstddef>
#include <cstdint>

namespace snapix {

// Fixed-size WiFi credential (no heap allocation)
struct WifiCredential {
  char ssid[33];      // 32 chars + null (IEEE 802.11 max)
  char password[65];  // 64 chars + null (WPA2 max)
};  // 98 bytes each

// Singleton credential store using fixed arrays (~800 bytes total)
// File: /.snapix/wifi.bin (XOR-obfuscated passwords)
class WifiCredentialStore {
 public:
  static constexpr int MAX_NETWORKS = 8;

  static WifiCredentialStore& getInstance();

  WifiCredentialStore(const WifiCredentialStore&) = delete;
  WifiCredentialStore& operator=(const WifiCredentialStore&) = delete;

  bool saveToFile() const;
  bool loadFromFile();

  bool addCredential(const char* ssid, const char* password);
  bool removeCredential(const char* ssid);
  const WifiCredential* findCredential(const char* ssid) const;
  bool hasSavedCredential(const char* ssid) const;

  const WifiCredential* getCredentials() const { return credentials_; }
  int getCount() const { return count_; }

  void clearAll();

 private:
  WifiCredentialStore() = default;

  void obfuscate(char* data, size_t len) const;

  WifiCredential credentials_[MAX_NETWORKS] = {};  // ~784 bytes
  uint8_t count_ = 0;
};

#define WIFI_STORE WifiCredentialStore::getInstance()

}  // namespace snapix
