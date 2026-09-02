#include "AppStatusBar.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalPowerManager.h>
#include <Rtc.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "fontIds.h"
#include "network/NetService.h"

namespace {

constexpr const char* MONTH_ABBR[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

/**
 * Wi-Fi arcs, drawn as stacked filled trapezoids.
 *
 * The renderer has no arc primitive, and a stepped circle at 16px reads as a
 * smudge on a 1-bit panel. Three widening bars over a dot is the shape everyone
 * already recognises and it survives the resolution.
 */
void drawWifi(const GfxRenderer& renderer, const int x, const int y, const bool connected, const bool connecting) {
  constexpr int BAR_W[3] = {4, 9, 14};
  constexpr int BAR_Y[3] = {10, 6, 2};

  for (int i = 0; i < 3; ++i) {
    const int w = BAR_W[i];
    const int bx = x + 7 - w / 2;
    // Connecting shows only the base arc, so a half-drawn icon is visibly
    // "working on it" rather than looking like a weak signal.
    const bool on = connected || (connecting && i == 0);
    if (on) {
      renderer.fillRect(bx, y + BAR_Y[i], w, 2, true);
    } else {
      renderer.drawRect(bx, y + BAR_Y[i], w, 2);
    }
  }

  // The dot at the base is always solid: it is what makes the shape read as an
  // antenna rather than three loose dashes.
  renderer.fillRect(x + 6, y + 14, 3, 3, true);

  if (!connected && !connecting) {
    // Offline: a slash through it. Cheaper to read than an absent icon, which
    // is indistinguishable from a rendering bug.
    renderer.drawLine(x, y + 16, x + 15, y, 2, true);
  }
}

void drawBattery(const GfxRenderer& renderer, const int x, const int y, const int percent) {
  constexpr int W = 26;
  constexpr int H = 14;

  renderer.drawRect(x, y, W, H, 1, true);
  // Terminal nub on the right.
  renderer.fillRect(x + W, y + 4, 2, H - 8, true);

  const int clamped = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
  const int fill = (W - 4) * clamped / 100;
  if (fill > 0) renderer.fillRect(x + 2, y + 2, fill, H - 4, true);
}

}  // namespace

namespace AppStatusBar {

void draw(const GfxRenderer& renderer, const char* title) {
  const int pageWidth = renderer.getScreenWidth();
  const int margin = 14;
  const int textY = 6;

  // --- Left: time, then date ---------------------------------------------
  int x = margin;
  Rtc::DateTime now;
  const bool haveClock = halClock.isAvailable() && halClock.getDateTime(now);

  if (haveClock) {
    const bool pm = now.hour >= 12;
    int hour12 = now.hour % 12;
    if (hour12 == 0) hour12 = 12;

    char clock[12];
    snprintf(clock, sizeof(clock), "%d:%02d %s", hour12, now.minute, pm ? "PM" : "AM");
    renderer.drawText(UI_10_FONT_ID, x, textY, clock, true, EpdFontFamily::BOLD);
    x += renderer.getTextWidth(UI_10_FONT_ID, clock, EpdFontFamily::BOLD) + 12;

    char date[16];
    const int monthIndex = (now.month >= 1 && now.month <= 12) ? now.month - 1 : 0;
    snprintf(date, sizeof(date), "%s %d", MONTH_ABBR[monthIndex], now.day);
    renderer.drawText(SMALL_FONT_ID, x, textY + 2, date, true);
  } else {
    // Says why the calendar and pet are not working, in the one place the user
    // will already be looking.
    renderer.drawText(SMALL_FONT_ID, x, textY + 2, "--:--", true);
  }

  // --- Right: battery, then Wi-Fi ----------------------------------------
  int right = pageWidth - margin - 28;
  drawBattery(renderer, right, textY + 2, static_cast<int>(powerManager.getBatteryPercentage()));
  right -= 26;
  drawWifi(renderer, right, textY, NET.isConnected(), NET.isConnecting());

  // --- Centre: optional screen name --------------------------------------
  if (title != nullptr && title[0] != '\0') {
    const int w = renderer.getTextWidth(UI_10_FONT_ID, title, EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, (pageWidth - w) / 2, textY, title, true, EpdFontFamily::BOLD);
  }

  renderer.drawLine(0, HEIGHT - 2, pageWidth, HEIGHT - 2, 1, true);
}

}  // namespace AppStatusBar
