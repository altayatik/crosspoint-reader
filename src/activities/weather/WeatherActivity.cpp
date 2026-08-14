#include "WeatherActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "WifiCredentialStore.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

constexpr const char* CACHE_PATH = "/.crosspoint/weather.json";

// The endpoint returns scalars only, so this is a hard ceiling rather than a
// guess that grows with the forecast. 1KB leaves generous headroom over the
// ~300 bytes actually sent.
constexpr size_t JSON_CAPACITY = 1024;

void copyField(char* dest, size_t size, const char* src) {
  if (!src) {
    dest[0] = '\0';
    return;
  }
  strncpy(dest, src, size - 1);
  dest[size - 1] = '\0';
}

}  // namespace

WeatherActivity::WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Weather", renderer, mappedInput) {}

void WeatherActivity::onEnter() {
  Activity::onEnter();
  Storage.mkdir("/.crosspoint");

  // Cache first, always. Paint something the moment the screen opens rather
  // than making the user watch a Wi-Fi association before seeing a number.
  const bool haveCache = loadCache();
  requestUpdate();

  if (!haveCache) {
    busy = true;
    statusLine = tr(STR_WEATHER_FETCHING);
    requestUpdateAndWait();
    if (!fetchNow()) statusLine = tr(STR_WEATHER_OFFLINE);
    busy = false;
    requestUpdate();
  }
}

void WeatherActivity::onExit() {
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }
  Activity::onExit();
}

void WeatherActivity::loop() {
  Activity::loop();
  if (busy) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    busy = true;
    statusLine = tr(STR_WEATHER_FETCHING);
    requestUpdateAndWait();
    statusLine = fetchNow() ? nullptr : tr(STR_WEATHER_OFFLINE);
    busy = false;
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
    }
    requestUpdate();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    editCity();
  }
}

void WeatherActivity::editCity() {
  auto handler = [this](const ActivityResult& result) {
    if (result.isCancelled) return;

    const auto& kb = std::get<KeyboardResult>(result.data);
    strncpy(SETTINGS.weatherCity, kb.text.c_str(), sizeof(SETTINGS.weatherCity) - 1);
    SETTINGS.weatherCity[sizeof(SETTINGS.weatherCity) - 1] = '\0';
    SETTINGS.saveToFile();
    LOG_INF("WX", "City set to '%s'", SETTINGS.weatherCity);

    // The cached reading is for the old city, so it is now actively wrong.
    // Drop it and go straight to the network rather than showing the previous
    // city's numbers under the new name.
    Storage.remove(CACHE_PATH);
    reading = Reading();

    busy = true;
    statusLine = tr(STR_WEATHER_FETCHING);
    requestUpdateAndWait();
    statusLine = fetchNow() ? nullptr : tr(STR_WEATHER_OFFLINE);
    busy = false;
    if (WiFi.getMode() != WIFI_MODE_NULL) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
    }
    requestUpdate();
  };

  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_WEATHER_CITY),
                                                                 std::string(SETTINGS.weatherCity),
                                                                 sizeof(SETTINGS.weatherCity) - 1, InputType::Text),
                         handler);
}

// ---------------------------------------------------------------------------

std::string WeatherActivity::encodeQuery(const char* text) {
  // Cities carry spaces, commas and accents ("Sao Paulo", "Washington, DC").
  // Anything outside the unreserved set goes out percent-encoded so the query
  // survives the trip intact.
  // Not named HEX: Arduino's Print.h defines that as the integer 16, and the
  // macro wins over any local declaration.
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  std::string out;
  if (text == nullptr) return out;
  for (const char* p = text; *p != '\0'; ++p) {
    const unsigned char c = static_cast<unsigned char>(*p);
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += HEX_DIGITS[c >> 4];
      out += HEX_DIGITS[c & 0x0F];
    }
  }
  return out;
}

std::string WeatherActivity::buildUrl() const {
  // Derived from the dashboard URL so there is one host to configure, not two.
  // Replacing the last path segment keeps any custom domain or port intact.
  std::string url(SETTINGS.dashboardUrl);
  if (url.empty()) return url;

  const size_t query = url.find('?');
  if (query != std::string::npos) url.erase(query);

  const size_t slash = url.find_last_of('/');
  if (slash == std::string::npos) return std::string();
  url.erase(slash + 1);
  url += "x3-weather.json";

  // Empty means "use whatever the dashboard is configured for", so the two
  // screens agree until the user deliberately splits them.
  if (SETTINGS.weatherCity[0] != '\0') {
    url += "?city=";
    url += encodeQuery(SETTINGS.weatherCity);
  }
  return url;
}

bool WeatherActivity::parseInto(const std::string& json, Reading& out) const {
  JsonDocument doc;
  const auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("WX", "JSON parse failed: %s", error.c_str());
    return false;
  }
  if (!doc["ok"].as<bool>()) {
    LOG_ERR("WX", "Server reported weather unavailable");
    return false;
  }

  copyField(out.place, sizeof(out.place), doc["place"] | "");
  copyField(out.condition, sizeof(out.condition), doc["cond"] | "");
  copyField(out.sunrise, sizeof(out.sunrise), doc["sunrise"] | "");
  copyField(out.sunset, sizeof(out.sunset), doc["sunset"] | "");
  copyField(out.updated, sizeof(out.updated), doc["updated"] | "");
  out.temp = doc["temp"] | 0;
  out.feels = doc["feels"] | 0;
  out.high = doc["hi"] | 0;
  out.low = doc["lo"] | 0;
  out.humidity = doc["humidity"] | 0;
  out.wind = doc["wind"] | 0;
  out.rain = doc["rain"] | 0;
  out.code = doc["code"] | 0;
  // Null when the server had too little hourly data to score the day; -1 keeps
  // that distinct from a genuine score of 0 ("stay inside").
  out.sunny = doc["sunny"].isNull() ? -1 : (doc["sunny"] | -1);
  copyField(out.sunnyLabel, sizeof(out.sunnyLabel), doc["sunnyLabel"] | "");
  out.isDay = (doc["day"] | 1) != 0;
  out.valid = true;
  return true;
}

bool WeatherActivity::loadCache() {
  HalFile file;
  if (!Storage.openFileForRead("WX", CACHE_PATH, file)) return false;

  const size_t size = file.size();
  if (size == 0 || size > JSON_CAPACITY * 2) {
    LOG_ERR("WX", "Cache size implausible: %u", static_cast<unsigned>(size));
    return false;
  }

  std::string json;
  json.resize(size);
  const int read = file.read(&json[0], size);
  file.close();
  if (read <= 0) return false;
  json.resize(static_cast<size_t>(read));

  if (!parseInto(json, reading)) return false;
  reading.fetchedAtMs = millis();
  // Nothing here tells us the true age across a power cycle -- millis() resets.
  // Treat a loaded cache as "old enough to refresh" so the first Confirm after
  // a boot is honest about being stale rather than silently showing yesterday.
  cacheWasStale = true;
  LOG_INF("WX", "Loaded cached weather");
  return true;
}

bool WeatherActivity::fetchNow() {
  const std::string url = buildUrl();
  if (url.empty()) {
    LOG_ERR("WX", "No URL configured");
    return false;
  }

  HalPowerManager::Lock powerLock;

  if (WiFi.status() != WL_CONNECTED) {
    WIFI_STORE.loadFromFile();
    const size_t count = WIFI_STORE.getCredentialCount();
    if (count == 0) {
      LOG_ERR("WX", "No saved WiFi");
      return false;
    }
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);

    const std::string last = WIFI_STORE.getLastConnectedSsid();
    const auto credential = !last.empty() ? WIFI_STORE.findCredential(last) : WIFI_STORE.getCredentialAt(0);
    if (!credential) return false;

    if (credential->password.empty()) {
      WiFi.begin(credential->ssid.c_str());
    } else {
      WiFi.begin(credential->ssid.c_str(), credential->password.c_str());
    }

    const unsigned long deadline =
        millis() + std::max<unsigned long>(5, SETTINGS.dashboardWifiTimeoutSeconds) * 1000UL;
    while (WiFi.status() != WL_CONNECTED && millis() < deadline) delay(100);
    if (WiFi.status() != WL_CONNECTED) {
      LOG_ERR("WX", "WiFi timeout");
      return false;
    }
  }

  std::string body;
  // Straight into memory: the payload is a few hundred bytes, so a temp file on
  // SD would cost more than it saves.
  if (!HttpDownloader::fetchUrl(url, body)) {
    LOG_ERR("WX", "Fetch failed");
    return false;
  }
  if (body.size() > JSON_CAPACITY * 2) {
    LOG_ERR("WX", "Response too large: %u bytes", static_cast<unsigned>(body.size()));
    return false;
  }

  Reading fresh;
  if (!parseInto(body, fresh)) return false;
  fresh.fetchedAtMs = millis();

  reading = fresh;
  cacheWasStale = false;

  // Only persist what parsed. A half-written or malformed body must never
  // replace a good cache.
  HalFile out;
  if (Storage.openFileForWrite("WX", CACHE_PATH, out)) {
    out.write(reinterpret_cast<const uint8_t*>(body.data()), body.size());
    out.close();
  } else {
    LOG_ERR("WX", "Could not write cache");
  }

  LOG_INF("WX", "Weather updated: %d F, %s", reading.temp, reading.condition);
  return true;
}

// ---------------------------------------------------------------------------

void WeatherActivity::render(RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int margin = 20;

  renderer.clearScreen();

  if (!reading.valid) {
    renderer.drawCenteredText(UI_12_FONT_ID, 46, tr(STR_WEATHER_MENU), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusLine ? statusLine : tr(STR_WEATHER_NO_DATA));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DASHBOARD_REFRESH_BTN), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  renderer.drawCenteredText(SMALL_FONT_ID, 40, reading.place, true, EpdFontFamily::BOLD);

  char big[12];
  snprintf(big, sizeof(big), "%d\xC2\xB0", reading.temp);
  renderer.drawCenteredText(NOTOSERIF_18_FONT_ID, 130, big, true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(UI_12_FONT_ID, 172, reading.condition);

  if (reading.sunny >= 0) {
    drawSunnyBadge(pageWidth - margin - 92, 104, 92);
  }

  char line[48];
  snprintf(line, sizeof(line), "%s %d\xC2\xB0", tr(STR_WEATHER_FEELS), reading.feels);
  renderer.drawCenteredText(UI_10_FONT_ID, 208, line);

  snprintf(line, sizeof(line), "%s %d\xC2\xB0   %s %d\xC2\xB0", tr(STR_WEATHER_HIGH), reading.high,
           tr(STR_WEATHER_LOW), reading.low);
  renderer.drawCenteredText(UI_10_FONT_ID, 240, line);

  // --- Detail rows -------------------------------------------------------
  const int rowTop = 288;
  const int rowH = 34;
  struct Row {
    const char* label;
    char value[16];
  } rows[5];

  rows[0].label = tr(STR_WEATHER_RAIN);
  snprintf(rows[0].value, sizeof(rows[0].value), "%d%%", reading.rain);
  rows[1].label = tr(STR_WEATHER_HUMIDITY);
  snprintf(rows[1].value, sizeof(rows[1].value), "%d%%", reading.humidity);
  rows[2].label = tr(STR_WEATHER_WIND);
  snprintf(rows[2].value, sizeof(rows[2].value), "%d mph", reading.wind);
  rows[3].label = tr(STR_WEATHER_SUNRISE);
  copyField(rows[3].value, sizeof(rows[3].value), reading.sunrise);
  rows[4].label = tr(STR_WEATHER_SUNSET);
  copyField(rows[4].value, sizeof(rows[4].value), reading.sunset);

  for (int i = 0; i < 5; ++i) {
    const int baseline = rowTop + rowH * i;
    renderer.drawText(UI_10_FONT_ID, margin, baseline, rows[i].label);
    const int w = renderer.getTextWidth(UI_10_FONT_ID, rows[i].value);
    renderer.drawText(UI_10_FONT_ID, pageWidth - margin - w, baseline, rows[i].value, true, EpdFontFamily::BOLD);
    renderer.drawLine(margin, baseline + 8, pageWidth - margin, baseline + 8, 1, true);
  }

  const char* footer = statusLine                     ? statusLine
                       : cacheWasStale                ? tr(STR_WEATHER_CACHED)
                                                      : tr(STR_WEATHER_UPDATED_NOW);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 62, footer);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DASHBOARD_REFRESH_BTN), tr(STR_WEATHER_CITY), "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void WeatherActivity::drawSunnyBadge(const int x, const int y, const int size) const {
  // The SunnyDay score as a filled proportion of a box, plus the number. A ring
  // would match the website, but the renderer has no arc primitive and a
  // stepped circle at this size reads as a blob on 1-bit e-ink.
  renderer.drawRect(x, y, size, size, 2, true);

  const int inset = 4;
  const int inner = size - inset * 2;
  const int filled = inner * std::max(0, std::min(100, reading.sunny)) / 100;
  // Fill from the bottom, like a gauge.
  renderer.fillRect(x + inset, y + inset + (inner - filled), inner, filled, true);

  char value[8];
  snprintf(value, sizeof(value), "%d", reading.sunny);
  const int vw = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
  const int vy = y + size / 2 - renderer.getLineHeight(UI_12_FONT_ID) / 2;
  // The numeral sits over the gauge, so its colour has to follow whichever part
  // of the box it lands in. Above the fill line it is black on white.
  const bool overFill = (size / 2) > (inset + inner - filled);
  renderer.drawText(UI_12_FONT_ID, x + size / 2 - vw / 2, vy, value, !overFill, EpdFontFamily::BOLD);

  if (reading.sunnyLabel[0] != '\0') {
    const int lw = renderer.getTextWidth(SMALL_FONT_ID, reading.sunnyLabel);
    renderer.drawText(SMALL_FONT_ID, x + size / 2 - lw / 2, y + size + 6, reading.sunnyLabel, true);
  }
}
