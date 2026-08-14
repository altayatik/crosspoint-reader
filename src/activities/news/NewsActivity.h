#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/Activity.h"

/**
 * Headlines from /api/x3-news.json.
 *
 * Same offline-first shape as the weather app: read the SD cache and paint it
 * immediately, only touch the radio when there is no cache or the user asks.
 * Bringing Wi-Fi up is the most expensive thing this device does, so glancing
 * at the news costs an SD read and a repaint.
 *
 * Headlines only, deliberately. Article text would mean far more bandwidth, a
 * much larger parse, and a licensing question -- and the upstream RSS reader
 * this could have been ported from routes bodies through the EPUB engine, which
 * is a lot of machinery to hang off a news button.
 *
 * The server sends titles already folded to ASCII and trimmed to ~110
 * characters, so the firmware only has to wrap them.
 */
class NewsActivity final : public Activity {
 public:
  NewsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool preventAutoSleep() override { return busy; }

 private:
  struct Item {
    std::string title;
    std::string source;
    // The feed's own standfirst, already trimmed to 300 chars server-side.
    // Empty when the feed gave none, or when it just repeated the headline.
    std::string summary;
  };

  bool loadCache();
  bool fetchNow();
  bool parseInto(const std::string& json, std::vector<Item>& out) const;
  std::string buildUrl() const;

  void refresh();
  void shutdownWifi() const;

  /** Split a headline across at most `maxLines` lines of `width` pixels. */
  int wrapTitle(const std::string& title, int width, int maxLines, std::string* lines) const;

  /** Same wrap, but for the body font used on the detail screen. */
  int wrapText(const std::string& text, int fontId, int width, int maxLines, std::string* lines) const;

  static constexpr int MAX_ITEMS = 12;
  // Bounds the parse. The endpoint caps itself at 12 x (110 char title + 300
  // char summary), so this is a hard ceiling rather than a guess that grows
  // with the feed.
  static constexpr size_t JSON_CAPACITY = 8192;
  static constexpr int TITLE_LINES = 3;

  std::vector<Item> items;
  int selected = 0;
  // First item drawn. The list is paged rather than scrolled by one row: on
  // e-ink a whole new page costs the same refresh as shifting one line.
  int pageTop = 0;

  bool busy = false;
  bool cacheWasStale = false;
  bool detail = false;
  const char* statusLine = nullptr;
};
