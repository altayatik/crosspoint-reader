#include "NetService.h"

#include <ArduinoJson.h>
#include <HalClock.h>
#include <Logging.h>
#include <Rtc.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"
#include "network/HttpDownloader.h"

namespace {

// Association only -- no TLS on this task. An HTTPS handshake with the CA
// bundle wants 8-10KB and would overflow this; the clock sync therefore runs on
// the main task, which has 8192 and is where every other HTTPS call in this
// firmware already happens.
constexpr uint32_t TASK_STACK_BYTES = 4096;

// Give up after this many clock attempts per boot, so an unreachable server
// does not mean a request every time round the main loop.
constexpr uint8_t MAX_CLOCK_ATTEMPTS = 5;
constexpr unsigned long CLOCK_RETRY_MS = 30000;

// How long to wait for the first association before giving up and retrying.
constexpr uint32_t ASSOCIATE_TIMEOUT_MS = 20000;

// Retry cadence once the first attempt has failed. Long enough that a device
// out of range is not holding the radio on continuously.
constexpr uint32_t RETRY_INTERVAL_MS = 60000;

}  // namespace

NetService& NetService::getInstance() {
  static NetService instance;
  return instance;
}

std::string NetService::apiUrl(const char* leaf) {
  std::string url(SETTINGS.dashboardUrl);
  if (url.empty()) return std::string();

  const size_t query = url.find('?');
  if (query != std::string::npos) url.erase(query);

  const size_t slash = url.find_last_of('/');
  if (slash == std::string::npos) return std::string();
  url.erase(slash + 1);
  url += leaf;
  return url;
}

void NetService::begin() {
  if (taskHandle != nullptr) return;

  WIFI_STORE.loadFromFile();
  credentialCount = static_cast<uint8_t>(std::min<size_t>(WIFI_STORE.getCredentialCount(), 255));

  // Believe the RTC only if it is reporting a year this firmware could have
  // been built in. A dead backup cell reads back as 2000 or 2165.
  Rtc::DateTime now;
  clockValid = halClock.isAvailable() && halClock.getDateTime(now) && plausibleYear(now.year);
  if (!clockValid) LOG_INF("NET", "RTC unset or implausible; will sync when online");

  if (credentialCount == 0) {
    LOG_INF("NET", "No saved WiFi; staying offline");
    return;
  }

  TaskHandle_t handle = nullptr;
  xTaskCreate(&NetService::taskTrampoline, "NetService", TASK_STACK_BYTES, this, 1, &handle);
  taskHandle = handle;
  if (taskHandle == nullptr) LOG_ERR("NET", "Could not start network task");
}

void NetService::taskTrampoline(void* param) { static_cast<NetService*>(param)->taskLoop(); }

bool NetService::associate() {
  const auto last = WIFI_STORE.getLastConnectedSsid();
  const auto credential = !last.empty() ? WIFI_STORE.findCredential(last) : WIFI_STORE.getCredentialAt(0);
  if (!credential) return false;

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  if (credential->password.empty()) {
    WiFi.begin(credential->ssid.c_str());
  } else {
    WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
  }

  const unsigned long deadline = millis() + ASSOCIATE_TIMEOUT_MS;
  while (WiFi.status() != WL_CONNECTED && millis() < deadline) {
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  return WiFi.status() == WL_CONNECTED;
}

void NetService::taskLoop() {
  connecting = true;
  bool ok = associate();
  connected = ok;
  connecting = false;

  if (ok) LOG_INF("NET", "Connected: %s", WiFi.localIP().toString().c_str());

  for (;;) {
    // Stand down while another screen owns the radio.
    //
    // File Transfer brings up an access point, Wi-Fi setup scans and joins, OTA
    // and font download drive their own connections. Those are upstream screens
    // that predate this service, and re-associating underneath them would break
    // both. AP mode means hands off entirely; a radio that is merely off is
    // fair game again after the retry interval, so a screen that switches it
    // off on exit still gets the link back.
    const wifi_mode_t mode = WiFi.getMode();
    if (mode == WIFI_AP || mode == WIFI_AP_STA) {
      connected = false;
      vTaskDelay(pdMS_TO_TICKS(5000));
      continue;
    }

    // Reconnect rather than reboot: the link drops when the router restarts,
    // and a dashboard on a wall should recover on its own.
    if (WiFi.status() != WL_CONNECTED) {
      if (connected) LOG_INF("NET", "Link lost; retrying");
      connected = false;
      connecting = true;
      ok = associate();
      connected = ok;
      connecting = false;
      if (!ok) vTaskDelay(pdMS_TO_TICKS(RETRY_INTERVAL_MS));
      continue;
    }

    connected = true;
    vTaskDelay(pdMS_TO_TICKS(5000));
  }
}

bool NetService::ensureConnected(const uint32_t timeoutMs) {
  if (connected) return true;
  if (credentialCount == 0) return false;

  const unsigned long deadline = millis() + timeoutMs;
  while (!connected && millis() < deadline) {
    delay(100);
  }
  return connected;
}

void NetService::maybeSyncClock() {
  if (clockValid || !connected) return;
  if (clockAttempts >= MAX_CLOCK_ATTEMPTS) return;

  const unsigned long now = millis();
  if (nextClockAttemptMs != 0 && static_cast<long>(now - nextClockAttemptMs) < 0) return;

  ++clockAttempts;
  nextClockAttemptMs = now + CLOCK_RETRY_MS;
  syncClock();
}

bool NetService::syncClock() {
  if (!halClock.isAvailable()) {
    LOG_ERR("CLK", "No RTC on this device");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) return false;

  const std::string url = apiUrl("x3-time.json");
  if (url.empty()) {
    LOG_ERR("CLK", "No dashboard URL to derive the time endpoint from");
    return false;
  }

  std::string body;
  if (!HttpDownloader::fetchUrl(url, body)) {
    LOG_ERR("CLK", "Time fetch failed");
    return false;
  }
  if (body.size() > 512) {
    LOG_ERR("CLK", "Time response implausible: %u bytes", static_cast<unsigned>(body.size()));
    return false;
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    LOG_ERR("CLK", "Time response did not parse");
    return false;
  }
  if (!doc["ok"].as<bool>()) return false;

  Rtc::DateTime dt;
  dt.year = static_cast<uint16_t>(doc["year"] | 0);
  dt.month = static_cast<uint8_t>(doc["month"] | 0);
  dt.day = static_cast<uint8_t>(doc["day"] | 0);
  dt.hour = static_cast<uint8_t>(doc["hour"] | 0);
  dt.minute = static_cast<uint8_t>(doc["minute"] | 0);
  dt.second = static_cast<uint8_t>(doc["second"] | 0);

  // Never write a nonsense date over a working clock.
  if (!plausibleYear(dt.year) || dt.month < 1 || dt.month > 12 || dt.day < 1 || dt.day > 31 || dt.hour > 23 ||
      dt.minute > 59 || dt.second > 59) {
    LOG_ERR("CLK", "Time response out of range: %u-%u-%u", static_cast<unsigned>(dt.year),
            static_cast<unsigned>(dt.month), static_cast<unsigned>(dt.day));
    return false;
  }

  if (!halClock.setDateTime(dt)) {
    LOG_ERR("CLK", "RTC write failed");
    return false;
  }

  // The RTC now holds local time. Record the offset so anything needing UTC can
  // get back to it; biased quarter-hours, 48 = UTC+0, matching the status bar.
  const int offsetMinutes = doc["offset"] | 0;
  const int biased = 48 + offsetMinutes / 15;
  if (biased >= 0 && biased <= 104) SETTINGS.clockUtcOffsetQ = static_cast<uint8_t>(biased);
  SETTINGS.clockHasBeenSynced = 1;
  SETTINGS.saveToFile();

  clockValid = true;
  LOG_INF("CLK", "Clock set to %u-%02u-%02u %02u:%02u local (offset %d min)", static_cast<unsigned>(dt.year),
          static_cast<unsigned>(dt.month), static_cast<unsigned>(dt.day), static_cast<unsigned>(dt.hour),
          static_cast<unsigned>(dt.minute), offsetMinutes);
  return true;
}
