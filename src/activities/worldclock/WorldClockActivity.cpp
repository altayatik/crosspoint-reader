#include "WorldClockActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Mirrors dashboard/shared/world-clock-utils.js, plus the home zone.
// Offsets are STANDARD time; see the DST note in the header.
const WorldClockActivity::Zone WorldClockActivity::ZONES[] = {
    {"Chicago", -360, true},    {"New York", -300, true}, {"Los Angeles", -480, true},
    {"London", 0, true},        {"Istanbul", 180, false}, {"Dubai", 240, false},
    {"Mumbai", 330, false},     {"Tokyo", 540, false},
};
const int WorldClockActivity::ZONE_COUNT = sizeof(ZONES) / sizeof(ZONES[0]);

WorldClockActivity::WorldClockActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("WorldClock", renderer, mappedInput) {}

void WorldClockActivity::onEnter() {
  Activity::onEnter();
  utcMinutes = utcMinutesNow();
  haveClock = utcMinutes >= 0;
  lastPollMs = millis();
  requestUpdate();
}

int WorldClockActivity::utcMinutesNow() const {
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (!halClock.isAvailable() || !halClock.getTime(hour, minute)) {
    LOG_ERR("WCL", "No RTC");
    return -1;
  }

  // The RTC holds local time. clockUtcOffsetQ is biased quarter-hours
  // (48 = UTC+0), the same convention the status-bar clock uses.
  uint8_t biased = SETTINGS.clockUtcOffsetQ;
  if (biased > 104) biased = 48;
  const int localOffsetMinutes = (static_cast<int>(biased) - 48) * 15;
  // Remembered so render() can work out which zones are on a different calendar
  // day from the one the user is standing in.
  homeOffsetMinutes = localOffsetMinutes;

  int minutes = static_cast<int>(hour) * 60 + static_cast<int>(minute) - localOffsetMinutes;
  return ((minutes % 1440) + 1440) % 1440;
}

int WorldClockActivity::dayOffsetFor(const int zoneOffsetMinutes) const {
  // floorDiv, not integer division: a zone that is behind UTC midnight gives a
  // negative numerator, and C++ truncation toward zero would report 0 instead
  // of -1 for the whole of the previous day.
  const auto floorDiv = [](const int value) { return value >= 0 ? value / 1440 : -(((-value) + 1439) / 1440); };
  return floorDiv(utcMinutes + zoneOffsetMinutes) - floorDiv(utcMinutes + homeOffsetMinutes);
}

void WorldClockActivity::loop() {
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    utcMinutes = utcMinutesNow();
    haveClock = utcMinutes >= 0;
    requestUpdate();
    return;
  }

  // Self-refresh on the minute boundary. Polled at 20s so the displayed time is
  // never more than that stale, without repainting e-ink any more than needed.
  const unsigned long now = millis();
  if (haveClock && now - lastPollMs >= 20000UL) {
    lastPollMs = now;
    const int fresh = utcMinutesNow();
    if (fresh >= 0 && fresh != utcMinutes) {
      utcMinutes = fresh;
      requestUpdate();
    }
  }
}

void WorldClockActivity::render(RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int margin = 20;

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 46, tr(STR_WORLDCLOCK_TITLE), true, EpdFontFamily::BOLD);

  if (!haveClock) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_CAL_NO_CLOCK));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DASHBOARD_REFRESH_BTN), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  const int listTop = 84;
  const int rowH = (pageHeight - listTop - 74) / ZONE_COUNT;

  for (int i = 0; i < ZONE_COUNT; ++i) {
    const Zone& zone = ZONES[i];
    const int baseline = listTop + rowH * i + rowH / 2 + 6;

    int minutes = ((utcMinutes + zone.offsetMinutes) % 1440 + 1440) % 1440;
    const int hour24 = minutes / 60;
    const int minute = minutes % 60;
    const bool pm = hour24 >= 12;
    int hour12 = hour24 % 12;
    if (hour12 == 0) hour12 = 12;

    char clock[12];
    snprintf(clock, sizeof(clock), "%d:%02d %s", hour12, minute, pm ? "PM" : "AM");

    renderer.drawText(UI_12_FONT_ID, margin, baseline, zone.city, true, EpdFontFamily::BOLD);

    const int clockW = renderer.getTextWidth(UI_12_FONT_ID, clock);
    renderer.drawText(UI_12_FONT_ID, pageWidth - margin - clockW, baseline, clock);

    // Day rollover relative to where the user is standing. This is the thing
    // that actually catches people out when calling abroad -- far more useful
    // than flagging which zones might be an hour off for DST.
    const int dayDelta = dayOffsetFor(zone.offsetMinutes);
    if (dayDelta != 0) {
      char marker[4];
      snprintf(marker, sizeof(marker), "%+d", dayDelta);
      const int markerW = renderer.getTextWidth(SMALL_FONT_ID, marker);
      renderer.drawText(SMALL_FONT_ID, pageWidth - margin - clockW - markerW - 10, baseline, marker);
    }

    if (i + 1 < ZONE_COUNT) {
      renderer.drawLine(margin, listTop + rowH * (i + 1), pageWidth - margin, listTop + rowH * (i + 1), 1, true);
    }
  }

  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 62, tr(STR_WORLDCLOCK_DST_NOTE));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DASHBOARD_REFRESH_BTN), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
