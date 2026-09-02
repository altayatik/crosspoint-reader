#pragma once

#include <Arduino.h>
#include <Rtc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable Rtc _sdkRtc;
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

  // The RTC sits on I2C and is now read from the render task (the status bar
  // draws the clock on every screen) while the main task can be writing it
  // during a clock sync. Two tasks driving the same bus without a guard is a
  // corrupt read at best and a wedged bus at worst.
  mutable SemaphoreHandle_t _mutex = nullptr;

  /** RAII guard. Degrades to a no-op if the mutex could not be created. */
  class Lock {
    SemaphoreHandle_t _handle;

   public:
    explicit Lock(SemaphoreHandle_t handle) : _handle(handle) {
      if (_handle) xSemaphoreTake(_handle, portMAX_DELAY);
    }
    ~Lock() {
      if (_handle) xSemaphoreGive(_handle);
    }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
  };

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // True if an RTC is present on this device
  bool isAvailable() const { return _available; }

  // Get current hour (0-23) and minute (0-59).
  // Returns false if RTC is not available.
  bool getTime(uint8_t& hour, uint8_t& minute) const;

  // Full wall-clock reading, including the date. Returns false when no RTC is
  // present or the read fails. Bypasses the getTime() cache.
  bool getDateTime(Rtc::DateTime& out) const;

  // Format time into a caller-provided buffer.
  // 24h mode produces "HH:MM" (needs >=6 bytes); 12h mode produces "H:MM AM"/"HH:MM PM" (needs >=9 bytes).
  // utcOffsetQuarterHoursBiased: biased quarter-hour offset (48 = UTC+0, 0 = UTC-12, 104 = UTC+14).
  // use12Hour: when true, format as 12-hour clock with AM/PM suffix.
  // Returns false if RTC is not available.
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;

  // Write a wall-clock reading to the RTC. The caller is responsible for the
  // value being sane and in whatever zone the rest of the firmware expects
  // (local, in this build -- see NetService::syncClock).
  bool setDateTime(const Rtc::DateTime& dt);

  // Sync the RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();
};
