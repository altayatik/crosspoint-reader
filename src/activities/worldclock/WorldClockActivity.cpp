#include "WorldClockActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "components/AppStatusBar.h"
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

  // The RTC holds the user's own local time. Anchor everything to that and to
  // ZONES[0] as the home city, rather than to SETTINGS.clockUtcOffsetQ.
  //
  // That setting defaults to UTC+0 and there is no reason for anyone to have
  // touched it -- the status-bar clock works without it. Trusting it meant the
  // home city itself was reported a day behind, which is how Chicago ended up
  // marked -1 while sitting in Chicago.
  const int localMinutes = static_cast<int>(hour) * 60 + static_cast<int>(minute);
  const int minutes = localMinutes - ZONES[0].offsetMinutes;
  return ((minutes % 1440) + 1440) % 1440;
}

int WorldClockActivity::dayOffsetFor(const int zoneOffsetMinutes) const {
  // Home local time is 0..1439 by construction, so its own day index is 0 and
  // the comparison reduces to which day the other zone has rolled into.
  const int homeLocal = ((utcMinutes + ZONES[0].offsetMinutes) % 1440 + 1440) % 1440;
  const int zoneLocal = homeLocal + (zoneOffsetMinutes - ZONES[0].offsetMinutes);

  // floorDiv, not integer division: a zone behind home across midnight gives a
  // negative numerator, and C++ truncation toward zero would report 0 instead
  // of -1 for the whole of the previous day.
  if (zoneLocal < 0) return -(((-zoneLocal) + 1439) / 1440);
  return zoneLocal / 1440;
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
  AppStatusBar::draw(renderer, tr(STR_WORLDCLOCK_TITLE));

  if (!haveClock) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_CAL_NO_CLOCK));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DASHBOARD_REFRESH_BTN), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(refresh_.next());
    return;
  }

  const int listTop = AppStatusBar::HEIGHT + 18;
  const int rowH = (pageHeight - listTop - 56) / ZONE_COUNT;

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

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DASHBOARD_REFRESH_BTN), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(refresh_.next());
}
