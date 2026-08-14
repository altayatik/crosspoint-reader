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
#include <string>

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
  out.pressure = doc["pressure"] | 0;
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

void WeatherActivity::drawCard(const int x, const int y, const int w, const int h) const {
  renderer.drawRoundedRect(x, y, w, h, 2, 10, true);
}

void WeatherActivity::drawStat(const int x, const int y, const char* label, const char* value) const {
  renderer.drawText(SMALL_FONT_ID, x, y, label, true);
  renderer.drawText(UI_12_FONT_ID, x, y + renderer.getLineHeight(SMALL_FONT_ID) + 4, value, true,
                    EpdFontFamily::BOLD);
}

void WeatherActivity::drawNowCard(const int x, const int y, const int w, const int h) const {
  drawCard(x, y, w, h);

  // Temperature owns the left half; everything else stacks down the right. The
  // previous layout centred the numeral across the full width and then dropped
  // a score badge on top of it, which is where the overlap came from.
  char big[12];
  snprintf(big, sizeof(big), "%d\xC2\xB0", reading.temp);
  const int bigW = renderer.getTextWidth(NOTOSERIF_18_FONT_ID, big, EpdFontFamily::BOLD);
  const int bigH = renderer.getLineHeight(NOTOSERIF_18_FONT_ID);
  const int leftW = w * 44 / 100;
  renderer.drawText(NOTOSERIF_18_FONT_ID, x + (leftW - bigW) / 2, y + (h - bigH) / 2 - 6, big, true,
                    EpdFontFamily::BOLD);

  const int rightX = x + leftW;
  renderer.drawLine(rightX, y + 18, rightX, y + h - 18, 1, true);

  const int textX = rightX + 18;
  const int avail = w - leftW - 34;
  int ty = y + 26;

  // Conditions can be long ("Thunderstorm with slight hail"); two lines at the
  // smaller size beats one clipped line.
  const int condLine = renderer.getLineHeight(UI_10_FONT_ID);
  if (renderer.getTextWidth(UI_10_FONT_ID, reading.condition, EpdFontFamily::BOLD) <= avail) {
    renderer.drawText(UI_10_FONT_ID, textX, ty, reading.condition, true, EpdFontFamily::BOLD);
    ty += condLine + 14;
  } else {
    // Break at the last space that still fits.
    std::string cond(reading.condition);
    size_t cut = cond.size();
    while (cut != std::string::npos) {
      cut = cond.rfind(' ', cut - 1);
      if (cut == std::string::npos) break;
      if (renderer.getTextWidth(UI_10_FONT_ID, cond.substr(0, cut).c_str(), EpdFontFamily::BOLD) <= avail) break;
    }
    if (cut == std::string::npos) {
      renderer.drawText(UI_10_FONT_ID, textX, ty, reading.condition, true, EpdFontFamily::BOLD);
      ty += condLine + 14;
    } else {
      renderer.drawText(UI_10_FONT_ID, textX, ty, cond.substr(0, cut).c_str(), true, EpdFontFamily::BOLD);
      ty += condLine + 2;
      renderer.drawText(UI_10_FONT_ID, textX, ty, cond.substr(cut + 1).c_str(), true, EpdFontFamily::BOLD);
      ty += condLine + 12;
    }
  }

  char line[40];
  snprintf(line, sizeof(line), "%s %d\xC2\xB0", tr(STR_WEATHER_FEELS), reading.feels);
  renderer.drawText(SMALL_FONT_ID, textX, ty, line, true);
  ty += renderer.getLineHeight(SMALL_FONT_ID) + 12;

  snprintf(line, sizeof(line), "%s %d\xC2\xB0", tr(STR_WEATHER_HIGH), reading.high);
  renderer.drawText(UI_10_FONT_ID, textX, ty, line, true);
  snprintf(line, sizeof(line), "%s %d\xC2\xB0", tr(STR_WEATHER_LOW), reading.low);
  renderer.drawText(UI_10_FONT_ID, textX + avail / 2, ty, line, true);
}

void WeatherActivity::drawSunnyCard(const int x, const int y, const int w, const int h) const {
  drawCard(x, y, w, h);

  const int pad = 18;
  renderer.drawText(SMALL_FONT_ID, x + pad, y + 12, tr(STR_WEATHER_SUNNY_TITLE), true);

  char score[8];
  snprintf(score, sizeof(score), "%d", reading.sunny);
  const int scoreW = renderer.getTextWidth(NOTOSERIF_18_FONT_ID, score, EpdFontFamily::BOLD);
  renderer.drawText(NOTOSERIF_18_FONT_ID, x + pad, y + 34, score, true, EpdFontFamily::BOLD);

  if (reading.sunnyLabel[0] != '\0') {
    renderer.drawText(UI_12_FONT_ID, x + pad + scoreW + 16, y + 46, reading.sunnyLabel, true, EpdFontFamily::BOLD);
  }

  // Horizontal gauge along the bottom of the card. A bar reads at a glance on
  // 1-bit e-ink in a way a stepped ring does not.
  const int barX = x + pad;
  const int barW = w - pad * 2;
  const int barY = y + h - 26;
  const int barH = 12;
  renderer.drawRect(barX, barY, barW, barH, 1, true);
  const int fill = (barW - 4) * std::max(0, std::min(100, reading.sunny)) / 100;
  if (fill > 0) renderer.fillRect(barX + 2, barY + 2, fill, barH - 4, true);
}

void WeatherActivity::drawDetailCard(const int x, const int y, const int w, const int h) const {
  drawCard(x, y, w, h);

  char rain[12];
  char humidity[12];
  char wind[12];
  char pressure[12];
  snprintf(rain, sizeof(rain), "%d%%", reading.rain);
  snprintf(humidity, sizeof(humidity), "%d%%", reading.humidity);
  snprintf(wind, sizeof(wind), "%d mph", reading.wind);
  snprintf(pressure, sizeof(pressure), "%d mb", reading.pressure);

  const int colX[2] = {x + 22, x + w / 2 + 10};
  const int rowY[2] = {y + 20, y + h / 2 + 8};

  drawStat(colX[0], rowY[0], tr(STR_WEATHER_RAIN), rain);
  drawStat(colX[1], rowY[0], tr(STR_WEATHER_HUMIDITY), humidity);
  drawStat(colX[0], rowY[1], tr(STR_WEATHER_WIND), wind);
  drawStat(colX[1], rowY[1], tr(STR_WEATHER_PRESSURE), pressure);

  renderer.drawLine(x + 14, y + h / 2, x + w - 14, y + h / 2, 1, true);
  renderer.drawLine(x + w / 2, y + 14, x + w / 2, y + h - 14, 1, true);
}

void WeatherActivity::drawSunCard(const int x, const int y, const int w, const int h) const {
  drawCard(x, y, w, h);
  drawStat(x + 22, y + 16, tr(STR_WEATHER_SUNRISE), reading.sunrise);
  drawStat(x + w / 2 + 10, y + 16, tr(STR_WEATHER_SUNSET), reading.sunset);
  renderer.drawLine(x + w / 2, y + 14, x + w / 2, y + h - 14, 1, true);
}

void WeatherActivity::render(RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int margin = 14;

  renderer.clearScreen();

  if (!reading.valid) {
    renderer.drawCenteredText(UI_12_FONT_ID, 46, tr(STR_WEATHER_MENU), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusLine ? statusLine : tr(STR_WEATHER_NO_DATA));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DASHBOARD_REFRESH_BTN), tr(STR_WEATHER_CITY), "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  renderer.drawCenteredText(UI_12_FONT_ID, 26, reading.place, true, EpdFontFamily::BOLD);

  // Bento stack, sized from the panel so the X4's 480x800 lays out too. Heights
  // are proportions of the space between the header and the footer band rather
  // than fixed pixels, which is what let the old layout collide.
  const int cardX = margin;
  const int cardW = pageWidth - margin * 2;
  const int top = 62;
  const int bottom = pageHeight - 96;  // footer line + button hints
  const int gap = 12;
  const int body = bottom - top - gap * 3;

  const int nowH = body * 36 / 100;
  const int sunnyH = reading.sunny >= 0 ? body * 20 / 100 : 0;
  const int sunH = body * 16 / 100;
  const int detailH = body - nowH - sunnyH - sunH;

  int y = top;
  drawNowCard(cardX, y, cardW, nowH);
  y += nowH + gap;

  if (reading.sunny >= 0) {
    drawSunnyCard(cardX, y, cardW, sunnyH);
    y += sunnyH + gap;
  }

  drawDetailCard(cardX, y, cardW, detailH);
  y += detailH + gap;

  drawSunCard(cardX, y, cardW, sunH);

  const char* footer = statusLine      ? statusLine
                       : cacheWasStale ? tr(STR_WEATHER_CACHED)
                                       : tr(STR_WEATHER_UPDATED_NOW);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 86, footer);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DASHBOARD_REFRESH_BTN), tr(STR_WEATHER_CITY), "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
