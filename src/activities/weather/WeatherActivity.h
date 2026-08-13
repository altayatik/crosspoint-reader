#pragma once

#include <cstdint>
#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"

/**
 * Offline-first weather.
 *
 * Reads a flat ~300 byte summary from /api/x3-weather.json, caches it to the SD
 * card, and renders from that cache. Opening the app shows the cached reading
 * immediately and only goes to the network when there is no cache at all, or
 * when you ask it to with Confirm.
 *
 * That ordering matters on a battery device: bringing Wi-Fi up is by far the
 * most expensive thing here, so the common case -- glancing at the weather --
 * costs an SD read and a repaint, and nothing else.
 *
 * The endpoint returns scalars only, so the parse is a fixed-size document.
 * See dashboard-data-api/api/x3-weather.js for why that shape exists.
 */
class WeatherActivity final : public Activity {
 public:
  WeatherActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  bool preventAutoSleep() override { return busy; }

 private:
  struct Reading {
    bool valid = false;
    char place[40] = "";
    char condition[40] = "";
    char sunrise[12] = "";
    char sunset[12] = "";
    int temp = 0;
    int feels = 0;
    int high = 0;
    int low = 0;
    int humidity = 0;
    int wind = 0;
    int rain = 0;
    int code = 0;
    // SunnyDay score, 0-100, or -1 when the server could not compute one. The
    // same figure altayatik.com/sunnyday shows; see lib/x3/sunnyday.js.
    int sunny = -1;
    char sunnyLabel[20] = "";
    bool isDay = true;
    // millis() at the moment of the fetch. Deliberately not wall-clock: the RTC
    // may be unset, and all this needs to answer is "how long ago".
    uint32_t fetchedAtMs = 0;
    // Wall-clock string from the server, shown verbatim when present.
    char updated[26] = "";
  };

  bool loadCache();
  bool fetchNow();
  bool parseInto(const std::string& json, Reading& out) const;
  std::string buildUrl() const;

  /** Open the on-screen keyboard to change the city, then re-fetch. */
  void editCity();

  /** Percent-encode a city name for the query string. */
  static std::string encodeQuery(const char* text);

  void drawSunnyBadge(int x, int y, int size) const;

  Reading reading;
  bool busy = false;
  bool cacheWasStale = false;
  const char* statusLine = nullptr;
};
