#include "Network.h"

#include <Arduino.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_wifi.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

#define TAG "NETWORK"

namespace snapix {
namespace drivers {

namespace {
bool isBlankSsid(const char* ssid) {
  if (!ssid) return true;
  while (*ssid) {
    if (!isspace(static_cast<unsigned char>(*ssid))) {
      return false;
    }
    ++ssid;
  }
  return true;
}

void configureCountryChannels() {
  wifi_country_t country = {
      .cc = "EU",
      .schan = 1,
      .nchan = 13,
      .max_tx_power = 20,
      .policy = WIFI_COUNTRY_POLICY_MANUAL,
  };
  esp_err_t err = esp_wifi_set_country(&country);
  if (err != ESP_OK) {
    LOG_ERR(TAG, "Failed to set WiFi country: 0x%x", err);
  }
}

void resetStationForScan() {
  WiFi.scanDelete();
  WiFi.disconnect(true, false, 200);
  WiFi.mode(WIFI_OFF);
  delay(120);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);
  configureCountryChannels();
  delay(180);
}
}  // namespace

Result<void> Network::init() {
  if (initialized_) {
    return Ok();
  }

  WiFi.persistent(false);
  resetStationForScan();

  initialized_ = true;
  connected_ = false;
  apMode_ = false;

  LOG_INF(TAG, "WiFi initialized (STA mode)");
  return Ok();
}

void Network::shutdown() {
  if (connected_) {
    disconnect();
  }

  if (apMode_) {
    stopAP();
  }

  if (initialized_) {
    WiFi.mode(WIFI_OFF);
    initialized_ = false;
    scanInProgress_ = false;
    LOG_INF(TAG, "WiFi shut down");
  }
}

Result<void> Network::connect(const char* ssid, const char* password) {
  if (apMode_) {
    stopAP();
  }

  if (!initialized_) {
    TRY(init());
  }

  LOG_INF(TAG, "Connecting to %s...", ssid);

  WiFi.begin(ssid, password);

  // Wait for connection with timeout
  constexpr uint32_t TIMEOUT_MS = 15000;
  uint32_t startMs = millis();

  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startMs > TIMEOUT_MS) {
      LOG_ERR(TAG, "Connection timeout");
      return ErrVoid(Error::Timeout);
    }
    delay(100);
  }

  esp_wifi_set_ps(WIFI_PS_NONE);

  connected_ = true;
  LOG_INF(TAG, "Connected, IP: %s", WiFi.localIP().toString().c_str());
  return Ok();
}

void Network::disconnect() {
  if (connected_) {
    WiFi.disconnect();
    uint32_t start = millis();
    while (WiFi.status() == WL_CONNECTED && millis() - start < 3000) {
      delay(10);
    }
    connected_ = false;
    LOG_INF(TAG, "Disconnected");
  }
}

int8_t Network::signalStrength() const {
  if (!connected_) {
    return 0;
  }
  return WiFi.RSSI();
}

void Network::getIpAddress(char* buffer, size_t bufferSize) const {
  if (!connected_ || bufferSize == 0) {
    if (bufferSize > 0) buffer[0] = '\0';
    return;
  }

  String ip = WiFi.localIP().toString();
  strncpy(buffer, ip.c_str(), bufferSize - 1);
  buffer[bufferSize - 1] = '\0';
}

Result<void> Network::startScan() {
  if (!initialized_) {
    TRY(init());
  }

  if (apMode_) {
    return ErrVoid(Error::InvalidOperation);
  }

  LOG_INF(TAG, "Starting WiFi scan...");
  resetStationForScan();
  int16_t result = WiFi.scanNetworks(true, true, false, 400);  // Async scan with hidden SSIDs enabled
  if (result == WIFI_SCAN_FAILED) {
    LOG_ERR(TAG, "Failed to start scan");
    return ErrVoid(Error::IOError);
  }
  scanInProgress_ = true;
  return Ok();
}

bool Network::isScanComplete() const {
  if (!scanInProgress_) {
    return true;
  }

  int16_t result = WiFi.scanComplete();
  return result != WIFI_SCAN_RUNNING;
}

int Network::getScanResults(WifiNetwork* out, int maxCount) {
  if (!out || maxCount <= 0 || !scanInProgress_) {
    return 0;
  }

  int16_t result = WiFi.scanComplete();
  if (result == WIFI_SCAN_RUNNING) {
    return 0;
  }

  scanInProgress_ = false;

  if (result == WIFI_SCAN_FAILED || result < 0) {
    LOG_ERR(TAG, "Scan failed");
    return 0;
  }

  if (result == 0) {
    LOG_ERR(TAG, "Async scan returned 0 networks, retrying synchronously");
    resetStationForScan();
    result = WiFi.scanNetworks(false, true, false, 500);
    if (result == WIFI_SCAN_FAILED || result < 0) {
      LOG_ERR(TAG, "Synchronous scan fallback failed");
      return 0;
    }
    if (result == 0) {
      wifi_mode_t mode = WIFI_MODE_NULL;
      esp_wifi_get_mode(&mode);
      LOG_ERR(TAG, "Synchronous scan still returned 0 networks (status=%d mode=%d)", static_cast<int>(WiFi.status()),
              static_cast<int>(mode));
    }
  }

  const int rawCount = static_cast<int>(result);
  std::vector<WifiNetwork> visibleNetworks;
  visibleNetworks.reserve(rawCount);

  for (int i = 0; i < rawCount; i++) {
    String ssid = WiFi.SSID(i);
    if (isBlankSsid(ssid.c_str())) {
      continue;
    }

    const int rssi = WiFi.RSSI(i);
    const bool secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;

    int existing = -1;
    for (size_t j = 0; j < visibleNetworks.size(); j++) {
      if (strncmp(visibleNetworks[j].ssid, ssid.c_str(), sizeof(visibleNetworks[j].ssid)) == 0) {
        existing = static_cast<int>(j);
        break;
      }
    }

    if (existing >= 0) {
      if (rssi > visibleNetworks[existing].rssi) {
        visibleNetworks[existing].rssi = rssi;
        visibleNetworks[existing].secured = secured;
      }
      continue;
    }

    WifiNetwork network{};
    strncpy(network.ssid, ssid.c_str(), sizeof(network.ssid) - 1);
    network.ssid[sizeof(network.ssid) - 1] = '\0';
    network.rssi = rssi;
    network.secured = secured;
    visibleNetworks.push_back(network);
  }

  // Sort by signal strength (strongest first)
  std::sort(visibleNetworks.begin(), visibleNetworks.end(),
            [](const WifiNetwork& a, const WifiNetwork& b) { return a.rssi > b.rssi; });

  const int count = std::min(static_cast<int>(visibleNetworks.size()), maxCount);
  for (int i = 0; i < count; i++) {
    out[i] = visibleNetworks[i];
  }

  LOG_INF(TAG, "Scan found %d visible networks (raw=%d unique=%d)", count, rawCount,
          static_cast<int>(visibleNetworks.size()));
  WiFi.scanDelete();
  return count;
}

Result<void> Network::startAP(const char* ssid, const char* password) {
  if (connected_) {
    disconnect();
  }

  LOG_INF(TAG, "Starting AP: %s", ssid);

  WiFi.mode(WIFI_AP);

  bool success;
  if (password && strlen(password) >= 8) {
    success = WiFi.softAP(ssid, password);
  } else {
    success = WiFi.softAP(ssid);
  }

  if (!success) {
    LOG_ERR(TAG, "Failed to start AP");
    return ErrVoid(Error::IOError);
  }

  initialized_ = true;
  apMode_ = true;
  LOG_INF(TAG, "AP started, IP: %s", WiFi.softAPIP().toString().c_str());
  return Ok();
}

void Network::stopAP() {
  if (apMode_) {
    WiFi.softAPdisconnect(true);
    apMode_ = false;
    LOG_INF(TAG, "AP stopped");
  }
}

void Network::getAPIP(char* buffer, size_t bufferSize) const {
  if (!apMode_ || bufferSize == 0) {
    if (bufferSize > 0) buffer[0] = '\0';
    return;
  }

  String ip = WiFi.softAPIP().toString();
  strncpy(buffer, ip.c_str(), bufferSize - 1);
  buffer[bufferSize - 1] = '\0';
}

}  // namespace drivers
}  // namespace snapix
