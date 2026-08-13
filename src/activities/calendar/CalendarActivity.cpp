#include "CalendarActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "components/UITheme.h"
#include "fontIds.h"

namespace {

// Sunday-first, matching Rtc::DateTime::weekday. Three letters rather than
// single initials: "S M T W T F S" has two ambiguous pairs, and a 70px column
// has room for the real abbreviation.
constexpr const char* WEEKDAY_ABBR[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

constexpr const char* MONTH_NAMES[12] = {"January", "February", "March",     "April",   "May",      "June",
                                         "July",    "August",   "September", "October", "November", "December"};

constexpr const char* MONTH_ABBR[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

constexpr int MIN_YEAR = 2000;  // DS3231 century range
constexpr int MAX_YEAR = 2099;

int wrap(const int value, const int lo, const int hi) {
  const int span = hi - lo + 1;
  int v = (value - lo) % span;
  if (v < 0) v += span;
  return v + lo;
}

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
  year = std::max(MIN_YEAR, std::min(MAX_YEAR, year));
}

void CalendarActivity::shiftYear(const int delta) {
  year = std::max(MIN_YEAR, std::min(MAX_YEAR, year + delta));
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

// ---------------------------------------------------------------------------

void CalendarActivity::openGoTo() {
  // Seed from today when the clock is set, otherwise from whatever is on
  // screen. Either way Confirm-Confirm is a one-gesture "take me home".
  pickYear = haveToday ? todayYear : year;
  pickMonth = haveToday ? todayMonth : month;
  pickDay = haveToday ? todayDay : 1;
  field = Field::Year;
  mode = Mode::GoTo;
}

void CalendarActivity::adjustField(const int delta) {
  switch (field) {
    case Field::Year:
      pickYear = wrap(pickYear + delta, MIN_YEAR, MAX_YEAR);
      break;
    case Field::Month:
      pickMonth = wrap(pickMonth + delta, 1, 12);
      break;
    case Field::Day:
      pickDay = wrap(pickDay + delta, 1, daysInMonth(pickYear, pickMonth));
      break;
  }
  // Changing year or month can strand the day past the end of a shorter month
  // (31 January -> February). Clamp rather than silently rolling into March.
  pickDay = std::min(pickDay, daysInMonth(pickYear, pickMonth));
}

void CalendarActivity::applyGoTo() {
  year = pickYear;
  month = pickMonth;
  mode = Mode::Browse;
}

// ---------------------------------------------------------------------------

void CalendarActivity::loop() {
  Activity::loop();

  if (mode == Mode::GoTo) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      mode = Mode::Browse;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      applyGoTo();
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      adjustField(-1);
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      adjustField(1);
      requestUpdate();
      return;
    }
    // Side buttons move between fields inside the overlay.
    if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      field = field == Field::Year ? Field::Day : static_cast<Field>(static_cast<uint8_t>(field) - 1);
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      field = field == Field::Day ? Field::Year : static_cast<Field>(static_cast<uint8_t>(field) + 1);
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openGoTo();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    shiftMonth(-1);
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    shiftMonth(1);
    requestUpdate();
    return;
  }
  // Side buttons page a whole year, so any date in the RTC's range is a handful
  // of presses away without needing a text entry screen.
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    shiftYear(-1);
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    shiftYear(1);
    requestUpdate();
  }
}

// ---------------------------------------------------------------------------

void CalendarActivity::drawGrid(const int pageWidth, const int pageHeight) const {
  // Derived from the panel rather than hardcoded, so this lays out correctly on
  // both the X3 (528x792) and the X4 (480x800).
  const int margin = 16;
  const int gridLeft = margin;
  const int cellW = (pageWidth - margin * 2) / 7;
  const int gridWidth = cellW * 7;

  // --- Title -------------------------------------------------------------
  char title[32];
  snprintf(title, sizeof(title), "%s %d", MONTH_NAMES[month - 1], year);
  renderer.drawCenteredText(UI_12_FONT_ID, 44, title, true, EpdFontFamily::BOLD);

  // --- Weekday header ----------------------------------------------------
  // The rule sits a full line height below the top of the header text. The
  // previous layout put it 6px down, which drew it straight through the
  // letters.
  const int headerY = 84;
  const int headerLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int ruleY = headerY + headerLineHeight + 6;

  for (int i = 0; i < 7; ++i) {
    const int centre = gridLeft + cellW * i + cellW / 2;
    const int w = renderer.getTextWidth(UI_10_FONT_ID, WEEKDAY_ABBR[i], EpdFontFamily::BOLD);
    renderer.drawText(UI_10_FONT_ID, centre - w / 2, headerY, WEEKDAY_ABBR[i], true, EpdFontFamily::BOLD);
  }
  renderer.drawLine(gridLeft, ruleY, gridLeft + gridWidth, ruleY, 2, true);

  // --- Days --------------------------------------------------------------
  const int gridTop = ruleY + 8;
  // Reserve the button-hint band at the bottom.
  const int cellH = (pageHeight - gridTop - 62) / 6;

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
    const int textY = cellY + (cellH - renderer.getLineHeight(UI_12_FONT_ID)) / 2;

    const bool isToday = haveToday && day == todayDay && month == todayMonth && year == todayYear;
    if (isToday) {
      // Knock today out of a filled block: the strongest mark available on a
      // 1-bit panel, and it survives being glanced at from across a room.
      renderer.fillRect(cellX + 2, cellY + 2, cellW - 4, cellH - 4, true);
      renderer.drawText(UI_12_FONT_ID, textX, textY, label, false, EpdFontFamily::BOLD);
    } else {
      // A box per date. On a monochrome panel the outline is what makes the
      // grid read as a grid instead of a field of loose numbers.
      renderer.drawRect(cellX + 2, cellY + 2, cellW - 4, cellH - 4);
      renderer.drawText(UI_12_FONT_ID, textX, textY, label, true);
    }
  }

  if (!haveToday) {
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 66, tr(STR_CAL_NO_CLOCK));
  }
}

void CalendarActivity::drawGoTo(const int pageWidth, const int pageHeight) const {
  const int boxW = pageWidth - 60;
  const int boxH = 190;
  const int boxX = 30;
  const int boxY = (pageHeight - boxH) / 2 - 30;

  // Clear the area under the panel first: this floats over the month grid, and
  // an unfilled overlay on 1-bit e-ink is unreadable.
  renderer.fillRect(boxX, boxY, boxW, boxH, false);
  renderer.drawRect(boxX, boxY, boxW, boxH, 3, true);

  renderer.drawCenteredText(UI_10_FONT_ID, boxY + 16, tr(STR_CAL_GOTO), true, EpdFontFamily::BOLD);

  // Three equal columns: year, month, day. The active one is inverted rather
  // than underlined -- at arm's length a 2px rule is easy to miss.
  char text[3][8];
  snprintf(text[0], sizeof(text[0]), "%d", pickYear);
  snprintf(text[1], sizeof(text[1]), "%s", MONTH_ABBR[pickMonth - 1]);
  snprintf(text[2], sizeof(text[2]), "%d", pickDay);

  const int fieldW = boxW / 3;
  const int fieldY = boxY + 58;
  const int fieldH = 60;

  for (int i = 0; i < 3; ++i) {
    const int fx = boxX + fieldW * i;
    const bool active = static_cast<int>(field) == i;
    if (active) renderer.fillRect(fx + 8, fieldY, fieldW - 16, fieldH, true);

    const int w = renderer.getTextWidth(UI_12_FONT_ID, text[i], EpdFontFamily::BOLD);
    const int ty = fieldY + (fieldH - renderer.getLineHeight(UI_12_FONT_ID)) / 2;
    renderer.drawText(UI_12_FONT_ID, fx + fieldW / 2 - w / 2, ty, text[i], !active, EpdFontFamily::BOLD);
  }

  renderer.drawCenteredText(SMALL_FONT_ID, boxY + boxH - 34, tr(STR_CAL_GOTO_HINT));
}

void CalendarActivity::render(RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  drawGrid(pageWidth, pageHeight);

  if (mode == Mode::GoTo) {
    drawGoTo(pageWidth, pageHeight);
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CAL_JUMP), "-", "+");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    GUI.drawSideButtonHints(renderer, tr(STR_CAL_FIELD), tr(STR_CAL_FIELD));
  } else {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_CAL_GOTO), "<", ">");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    GUI.drawSideButtonHints(renderer, tr(STR_CAL_YEAR_PREV), tr(STR_CAL_YEAR_NEXT));
  }

  // HALF: paging a month is a full-content change, and FAST would ghost the
  // previous month's digits underneath.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
