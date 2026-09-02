#include "NewsActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "CrossPointSettings.h"
#include "components/AppStatusBar.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/NetService.h"

namespace {

constexpr const char* CACHE_PATH = "/.crosspoint/news.json";

}  // namespace

NewsActivity::NewsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("News", renderer, mappedInput) {}

void NewsActivity::onEnter() {
  Activity::onEnter();
  Storage.mkdir("/.crosspoint");

  // Cache first so something is on screen immediately, then always go and get
  // fresh headlines. Stale news is the one thing a news app must not serve, and
  // the server caches for 15 minutes so this costs one small request.
  loadCache();
  requestUpdate();
  refresh();
}

void NewsActivity::onExit() {
  // The radio stays up; NetService owns it.
  Activity::onExit();
}

void NewsActivity::refresh() {
  busy = true;
  statusLine = tr(STR_NEWS_FETCHING);
  // A whole new list of headlines is a full-screen change, so the differential
  // waveform would leave the old ones showing through.
  refresh_.markDirty();
  requestUpdateAndWait();

  statusLine = fetchNow() ? nullptr : tr(STR_NEWS_OFFLINE);
  busy = false;
  refresh_.markDirty();
  requestUpdate();
}

// ---------------------------------------------------------------------------

std::string NewsActivity::buildUrl() const { return NetService::apiUrl("x3-news.json"); }

bool NewsActivity::parseInto(const std::string& json, std::vector<Item>& out) const {
  JsonDocument doc;
  const auto error = deserializeJson(doc, json);
  if (error) {
    LOG_ERR("NWS", "JSON parse failed: %s", error.c_str());
    return false;
  }
  if (!doc["ok"].as<bool>()) {
    LOG_ERR("NWS", "Server reported news unavailable");
    return false;
  }

  JsonArrayConst array = doc["items"].as<JsonArrayConst>();
  if (array.isNull()) return false;

  out.clear();
  // reserve before the loop: each vector growth is an alloc, a copy and a free,
  // and this runs while a 52KB framebuffer is already resident.
  out.reserve(MAX_ITEMS);

  for (JsonObjectConst entry : array) {
    if (static_cast<int>(out.size()) >= MAX_ITEMS) break;
    const char* title = entry["t"] | "";
    if (title[0] == '\0') continue;
    Item item;
    item.title = title;
    item.source = entry["s"] | "";
    item.summary = entry["d"] | "";
    out.push_back(std::move(item));
  }

  return !out.empty();
}

bool NewsActivity::loadCache() {
  HalFile file;
  if (!Storage.openFileForRead("NWS", CACHE_PATH, file)) return false;

  const size_t size = file.size();
  if (size == 0 || size > JSON_CAPACITY * 2) {
    LOG_ERR("NWS", "Cache size implausible: %u", static_cast<unsigned>(size));
    return false;
  }

  std::string json;
  json.resize(size);
  const int read = file.read(&json[0], size);
  file.close();
  if (read <= 0) return false;
  json.resize(static_cast<size_t>(read));

  if (!parseInto(json, items)) return false;
  // millis() resets across a power cycle, so a loaded cache is always "old
  // enough to refresh" -- better than claiming yesterday's headlines are fresh.
  cacheWasStale = true;
  LOG_INF("NWS", "Loaded %u cached headlines", static_cast<unsigned>(items.size()));
  return true;
}

bool NewsActivity::fetchNow() {
  const std::string url = buildUrl();
  if (url.empty()) {
    LOG_ERR("NWS", "No URL configured");
    return false;
  }

  HalPowerManager::Lock powerLock;

  if (!NET.ensureConnected(std::max<uint32_t>(5, SETTINGS.dashboardWifiTimeoutSeconds) * 1000UL)) {
    LOG_ERR("NWS", NET.hasCredentials() ? "No network" : "No saved WiFi");
    return false;
  }

  std::string body;
  if (!HttpDownloader::fetchUrl(url, body)) {
    LOG_ERR("NWS", "Fetch failed");
    return false;
  }
  if (body.size() > JSON_CAPACITY * 2) {
    LOG_ERR("NWS", "Response too large: %u bytes", static_cast<unsigned>(body.size()));
    return false;
  }

  std::vector<Item> fresh;
  if (!parseInto(body, fresh)) return false;

  items = std::move(fresh);
  selected = 0;
  pageTop = 0;
  cacheWasStale = false;

  // Only persist what parsed: a truncated body must never replace a good cache.
  HalFile out;
  if (Storage.openFileForWrite("NWS", CACHE_PATH, out)) {
    out.write(reinterpret_cast<const uint8_t*>(body.data()), body.size());
    out.close();
  } else {
    LOG_ERR("NWS", "Could not write cache");
  }

  LOG_INF("NWS", "Loaded %u headlines", static_cast<unsigned>(items.size()));
  return true;
}

// ---------------------------------------------------------------------------

void NewsActivity::loop() {
  Activity::loop();
  if (busy) return;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (detail) {
      detail = false;
      refresh_.markDirty();
      requestUpdate();
    } else {
      activityManager.goHome();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (items.empty()) {
      refresh();
    } else {
      detail = !detail;
      refresh_.markDirty();
      requestUpdate();
    }
    return;
  }

  // Refresh lives on the side button so Confirm can open a headline.
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    refresh();
    return;
  }

  if (items.empty()) return;

  const int count = static_cast<int>(items.size());
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    selected = (selected + count - 1) % count;
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    selected = (selected + 1) % count;
    requestUpdate();
  }
}

// ---------------------------------------------------------------------------

int NewsActivity::wrapText(const std::string& text, const int fontId, const int width, const int maxLines,
                           std::string* lines) const {
  int used = 0;
  size_t pos = 0;

  while (pos < text.size() && used < maxLines) {
    std::string line;
    size_t lastFit = pos;

    // Grow the line word by word until it no longer fits, then commit the last
    // width that did. Measuring per word rather than per character keeps this
    // to a few getTextWidth calls per line.
    while (true) {
      const size_t space = text.find(' ', lastFit + 1);
      const size_t end = space == std::string::npos ? text.size() : space;
      std::string candidate = text.substr(pos, end - pos);

      if (renderer.getTextWidth(fontId, candidate.c_str()) > width && !line.empty()) break;

      line = std::move(candidate);
      lastFit = end;
      if (end >= text.size()) break;
    }

    if (line.empty()) break;  // a single word wider than the column
    lines[used++] = line;
    pos = lastFit;
    while (pos < text.size() && text[pos] == ' ') ++pos;
  }

  return used;
}

int NewsActivity::wrapTitle(const std::string& title, const int width, const int maxLines,
                            std::string* lines) const {
  return wrapText(title, UI_10_FONT_ID, width, maxLines, lines);
}

void NewsActivity::render(RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int margin = 20;

  renderer.clearScreen();
  AppStatusBar::draw(renderer, tr(STR_NEWS_MENU));

  if (items.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusLine ? statusLine : tr(STR_NEWS_NO_DATA));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DASHBOARD_REFRESH_BTN), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(refresh_.next());
    return;
  }

  const int contentWidth = pageWidth - margin * 2;
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);

  if (detail) {
    const Item& item = items[selected];

    char counter[24];
    snprintf(counter, sizeof(counter), "%d / %d", selected + 1, static_cast<int>(items.size()));
    renderer.drawCenteredText(SMALL_FONT_ID, AppStatusBar::HEIGHT + 8, counter);

    if (!item.source.empty()) {
      renderer.drawText(UI_10_FONT_ID, margin, AppStatusBar::HEIGHT + 32, item.source.c_str(), true,
                        EpdFontFamily::BOLD);
      renderer.drawLine(margin, AppStatusBar::HEIGHT + 56, pageWidth - margin, AppStatusBar::HEIGHT + 56, 1, true);
    }

    // Headline, in the larger face.
    const int titleLine = renderer.getLineHeight(UI_12_FONT_ID);
    std::string lines[5];
    const int used = wrapText(item.title, UI_12_FONT_ID, contentWidth, 5, lines);
    int y = AppStatusBar::HEIGHT + 78;
    for (int i = 0; i < used; ++i) {
      renderer.drawText(UI_12_FONT_ID, margin, y, lines[i].c_str(), true, EpdFontFamily::BOLD);
      y += titleLine + 6;
    }

    // The feed's standfirst. This is what the server sends as `d`; there is no
    // full article body, and the screen says so rather than looking broken when
    // a feed supplies nothing.
    y += 18;
    if (item.summary.empty()) {
      renderer.drawText(SMALL_FONT_ID, margin, y, tr(STR_NEWS_NO_SUMMARY), true);
    } else {
      std::string body[12];
      const int bodyUsed = wrapText(item.summary, UI_10_FONT_ID, contentWidth, 12, body);
      for (int i = 0; i < bodyUsed; ++i) {
        renderer.drawText(UI_10_FONT_ID, margin, y, body[i].c_str(), true);
        y += lineHeight + 4;
      }
    }

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_NEWS_LIST), "<", ">");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(refresh_.next());
    return;
  }

  // --- List --------------------------------------------------------------
  const int listTop = AppStatusBar::HEIGHT + 10;
  const int listBottom = pageHeight - 96;
  const int rowH = lineHeight * TITLE_LINES + 18;
  const int perPage = std::max(1, (listBottom - listTop) / rowH);

  // Page rather than scroll: on e-ink a whole new page costs the same single
  // refresh as shifting by one row, so there is nothing to gain from the
  // smoother behaviour and a lot of ghosting to lose.
  if (selected < pageTop) pageTop = (selected / perPage) * perPage;
  if (selected >= pageTop + perPage) pageTop = (selected / perPage) * perPage;

  const int count = static_cast<int>(items.size());
  for (int i = 0; i < perPage && pageTop + i < count; ++i) {
    const int index = pageTop + i;
    const Item& item = items[index];
    const int rowY = listTop + i * rowH;
    const bool active = index == selected;

    if (active) {
      renderer.drawRect(margin - 6, rowY - 4, contentWidth + 12, rowH - 6, 2, true);
    }

    std::string lines[TITLE_LINES];
    const int used = wrapTitle(item.title, contentWidth - 12, TITLE_LINES, lines);
    for (int line = 0; line < used; ++line) {
      renderer.drawText(UI_10_FONT_ID, margin, rowY + 2 + line * lineHeight, lines[line].c_str(), true,
                        active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    }

    if (!item.source.empty()) {
      const int sw = renderer.getTextWidth(SMALL_FONT_ID, item.source.c_str());
      renderer.drawText(SMALL_FONT_ID, pageWidth - margin - sw, rowY + 2 + used * lineHeight, item.source.c_str());
    }
  }

  char footer[48];
  if (statusLine) {
    snprintf(footer, sizeof(footer), "%s", statusLine);
  } else {
    snprintf(footer, sizeof(footer), "%d / %d%s", selected + 1, count,
             cacheWasStale ? tr(STR_NEWS_CACHED_SUFFIX) : "");
  }
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 62, footer);

  // Not drawSideButtonHints: on the X3 those are drawn rotated down the screen
  // edges at a fixed height and clip their own text.
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 86, tr(STR_NEWS_REFRESH_HINT));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_NEWS_READ), "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(refresh_.next());
}
