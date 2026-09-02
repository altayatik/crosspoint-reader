#pragma once

#include <cstdint>
#include <string>

/**
 * One owner for the radio and the wall clock.
 *
 * Before this, every app brought Wi-Fi up itself and tore it down on the way
 * out. That is why the weather screen could fail to reach the network while the
 * device was plainly connected: it had just powered the radio off, and
 * re-associating from WIFI_OFF inside one screen's timeout is unreliable.
 *
 * Now a background task associates once at boot and keeps the link up for as
 * long as the device is awake. Apps ask whether they have a network and wait if
 * they need one; none of them touch WiFi directly.
 *
 * The same task fixes the clock. The DS3231 keeps time on its coin cell, but if
 * that dies -- or the device has never been set -- the RTC comes back at some
 * meaningless date and the calendar, world clock and pet all quietly break.
 * So a boot that finds an implausible year fetches the real one.
 *
 * Time comes from /api/x3-time.json, not NTP, because NTP gives UTC and this
 * device has no timezone database. The server already knows where the dashboard
 * is, so it sends local wall-clock fields that go straight into the RTC, with
 * DST handled on the day rather than whenever the firmware was last flashed.
 */
class NetService {
 public:
  static NetService& getInstance();

  /** Start the background task. Safe to call once, from setup(). */
  void begin();

  /** True once associated. Cheap; safe from any task. */
  bool isConnected() const { return connected; }

  /** True while the first association attempt is still running. */
  bool isConnecting() const { return connecting; }

  /** False when there is nothing saved to connect to. */
  bool hasCredentials() const { return credentialCount > 0; }

  /**
   * Block until associated, or until the timeout expires.
   * Returns immediately when already connected.
   */
  bool ensureConnected(uint32_t timeoutMs);

  /** True once the RTC holds a date this firmware is willing to believe. */
  bool clockIsSet() const { return clockValid; }

  /**
   * Fetch local time and write it to the RTC. Requires a connection.
   *
   * MUST be called from a task with a large stack. The TLS handshake needs
   * roughly 8-10KB, which is why this is not run on the network task below:
   * doing so overflowed its stack and panicked the device on every boot with a
   * dead RTC, which is a boot loop.
   */
  bool syncClock();

  /**
   * Main-task hook. Call from loop(); syncs the clock at most a few times,
   * spaced out, and only when there is a link and the RTC is wrong.
   */
  void maybeSyncClock();

  /**
   * Absolute URL for an API path, derived from the configured dashboard URL so
   * there is one host to configure rather than four.
   * Returns empty when dashboardUrl is unusable.
   */
  static std::string apiUrl(const char* leaf);

  /** A year the RTC could plausibly be reporting rather than a cold start. */
  static bool plausibleYear(int year) { return year >= 2025 && year <= 2099; }

 private:
  NetService() = default;

  static void taskTrampoline(void* param);
  void taskLoop();
  bool associate();

  void* taskHandle = nullptr;

  // Written by the network task, read by everyone else. Single writer, and the
  // readers only ever branch on them, so volatile is enough -- no mutex, which
  // would risk blocking the render task behind a socket operation.
  volatile bool connected = false;
  volatile bool connecting = false;
  volatile bool clockValid = false;
  volatile uint8_t credentialCount = 0;

  // Clock-sync attempts, owned by the main task. Capped and spaced so a server
  // that is down cannot turn into a request every loop iteration.
  uint8_t clockAttempts = 0;
  unsigned long nextClockAttemptMs = 0;
};

#define NET NetService::getInstance()
