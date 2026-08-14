#pragma once

#include <cstdint>

#include "MappedInputManager.h"
#include "activities/Activity.h"

/**
 * World clocks, computed from the RTC and fixed UTC offsets.
 *
 * HONEST LIMITATION: the offsets are static, so any zone observing daylight
 * saving is an hour out for part of the year. Doing this properly needs a
 * tzdata table (far too large here) or a server that sends current offsets --
 * the dashboard API could, and that is the upgrade path if it starts to annoy.
 *
 * Each entry therefore carries its standard-time offset and a flag for whether
 * the zone observes DST, so the screen can mark the ones that may be off rather
 * than quietly lying.
 *
 * The device's own clock is assumed to be local time, matching how the reader's
 * status-bar clock treats it (SETTINGS.clockUtcOffsetQ gives the offset back to
 * UTC).
 */
class WorldClockActivity final : public Activity {
 public:
  WorldClockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  struct Zone {
    const char* city;
    int16_t offsetMinutes;  // from UTC, standard time
    bool observesDst;
  };

  static const Zone ZONES[];
  static const int ZONE_COUNT;

  /** Minutes since UTC midnight, or -1 when the RTC is unreadable. */
  int utcMinutesNow() const;

  /** +1 / -1 when a zone's calendar day differs from the home zone's. */
  int dayOffsetFor(int zoneOffsetMinutes) const;

  bool haveClock = false;
  int utcMinutes = 0;
  unsigned long lastPollMs = 0;
};
