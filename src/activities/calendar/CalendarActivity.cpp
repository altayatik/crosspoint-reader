#include "CalendarActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Sunday-first, matching Rtc::DateTime::weekday.
constexpr const char* WEEKDAY_INITIALS[7] = {"S", "M", "T", "W", "T", "F", "S"};

constexpr const char* MONTH_NAMES[12] = {"January", "February", "March",     "April",   "May",      "June",
                                         "July",    "August",   "September", "October", "November", "December"};

}  // namespace

CalendarActivity::CalendarActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Calendar", renderer, mappedInput) {}

void CalendarActivity::onEnter() {
  Activity::onEnter();
  goToToday();
  requestUpdate();
}

void CalendarActivity::goToToday() {
  Rtc::DateTime now;
  if (halClock.isAvailable() && halClock.getDateTime(now)) {
    haveToday = true;
    todayYear = now.year;
    todayMonth = now.month;
    todayDay = now.day;
    year = now.year;
    month = now.month;
  } else {
    // No RTC, or it never got set. Show something navigable rather than a
    // blank screen, and let render() say why the highlight is missing.
    haveToday = false;
    year = 2026;
    month = 1;
    LOG_ERR("CAL", "No RTC date available");
  }
}

void CalendarActivity::shiftMonth(const int delta) {
  month += delta;
  while (month > 12) {
    month -= 12;
    ++year;
  }
  while (month < 1) {
    month += 12;
    --year;
  }
  // The RTC only spans 2000-2099; keep paging inside that so the weekday maths
  // stays meaningful.
  if (year < 2000) year = 2000;
  if (year > 2099) year = 2099;
}

bool CalendarActivity::isLeapYear(const int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int CalendarActivity::daysInMonth(const int year, const int month) {
  static constexpr int LENGTHS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 30;
  if (month == 2 && isLeapYear(year)) return 29;
  return LENGTHS[month - 1];
}

int CalendarActivity::firstWeekdayOfMonth() const {
  // Sakamoto's algorithm. Valid for any Gregorian date, and short enough that
  // it beats carrying a date library onto a 380KB device.
  static constexpr int OFFSETS[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  int y = year;
  if (month < 3) y -= 1;
  return (y + y / 4 - y / 100 + y / 400 + OFFSETS[month - 1] + 1) % 7;
}

void CalendarActivity::loop() {
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    goToToday();
    requestUpdate();
    return;
  }
  // Side buttons page too: they are the ones that fall under a thumb.
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    shiftMonth(-1);
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    shiftMonth(1);
    requestUpdate();
  }
}

void CalendarActivity::render(RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  // --- Title -------------------------------------------------------------
  char title[32];
  snprintf(title, sizeof(title), "%s %d", MONTH_NAMES[month - 1], year);
  renderer.drawCenteredText(UI_12_FONT_ID, 46, title, true, EpdFontFamily::BOLD);

  // --- Grid geometry -----------------------------------------------------
  // Derived from the panel rather than hardcoded, so this lays out correctly on
  // both the X3 (528x792) and the X4 (480x800).
  const int margin = 16;
  const int gridLeft = margin;
  const int gridWidth = pageWidth - margin * 2;
  const int cellW = gridWidth / 7;
  const int headerY = 78;
  const int gridTop = headerY + 14;
  // Reserve the button-hint band at the bottom.
  const int available = pageHeight - gridTop - 56;
  const int cellH = available / 6;

  for (int i = 0; i < 7; ++i) {
    const int centre = gridLeft + cellW * i + cellW / 2;
    const int w = renderer.getTextWidth(SMALL_FONT_ID, WEEKDAY_INITIALS[i]);
    renderer.drawText(SMALL_FONT_ID, centre - w / 2, headerY, WEEKDAY_INITIALS[i], true, EpdFontFamily::BOLD);
  }
  renderer.drawLine(gridLeft, gridTop - 8, gridLeft + cellW * 7, gridTop - 8, 2, true);

  // --- Days --------------------------------------------------------------
  const int firstWeekday = firstWeekdayOfMonth();
  const int total = daysInMonth(year, month);

  for (int day = 1; day <= total; ++day) {
    const int index = firstWeekday + day - 1;
    const int col = index % 7;
    const int row = index / 7;
    if (row > 5) break;  // a 6-row grid holds any month

    const int cellX = gridLeft + col * cellW;
    const int cellY = gridTop + row * cellH;

    char label[4];
    snprintf(label, sizeof(label), "%d", day);
    const int textW = renderer.getTextWidth(UI_12_FONT_ID, label);
    const int textX = cellX + cellW / 2 - textW / 2;
    const int textY = cellY + cellH / 2 + 8;

    const bool isToday = haveToday && day == todayDay && month == todayMonth && year == todayYear;
    if (isToday) {
      // Knock today out of a filled block: the strongest mark available on a
      // 1-bit panel, and it survives being glanced at from across a room.
      const int boxW = cellW - 6;
      const int boxH = cellH - 6;
      renderer.fillRect(cellX + 3, cellY + 3, boxW, boxH, true);
      renderer.drawText(UI_12_FONT_ID, textX, textY, label, false, EpdFontFamily::BOLD);
    } else {
      renderer.drawText(UI_12_FONT_ID, textX, textY, label, true);
    }
  }

  if (!haveToday) {
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 66, tr(STR_CAL_NO_CLOCK));
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CAL_TODAY), "<", ">");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // HALF: paging a month is a full-content change, and FAST would ghost the
  // previous month's digits underneath.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
