#pragma once
#include <cstdint>

class BatteryMonitor {
 public:
  // Optional divider multiplier parameter defaults to 2.0
  explicit BatteryMonitor(uint8_t adcPin, float dividerMultiplier = 2.0f);

  // Read voltage and return percentage (0-100)
  uint16_t readPercentage() const;

  // Read the battery voltage in millivolts (accounts for divider)
  // Cached for 1s — analogReadMilliVolts() takes 1-3ms due to ADC calibration,
  // and battery voltage is a slow-changing signal.  Called multiple times per
  // page render (status bar renders 1-4× with anti-aliasing).
  uint16_t readMillivolts() const;

  // Read raw millivolts from ADC (doesn't account for divider, bypasses cache)
  uint16_t readRawMillivolts() const;

  // Read the battery voltage in volts (accounts for divider)
  double readVolts() const;

  // Percentage (0-100) from a millivolt value
  static uint16_t percentageFromMillivolts(uint16_t millivolts);

  // Calibrate a raw ADC reading and return millivolts
  static uint16_t millivoltsFromRawAdc(uint16_t adc_raw);

 private:
  uint8_t _adcPin;
  float _dividerMultiplier;

  // Cache for readMillivolts() — avoids repeated ADC calibration calls
  // during status-bar renders in the same frame or within a few seconds.
  static constexpr uint32_t CACHE_TTL_MS = 1000;
  mutable uint16_t _cachedMillivolts = 0;
  mutable uint32_t _cachedMillivoltsAtMs = 0;
};
