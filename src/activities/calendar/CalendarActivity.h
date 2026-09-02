#pragma once

#include <Rtc.h>

#include <cstdint>

#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "components/RefreshPolicy.h"

/**
 * A plain month calendar off the DS3231.
 *
 * No events, no sync, no network -- a wall calendar you can hold.
 *
 *   Browse: Left/Right page months, Up/Down page years, Confirm opens Go To.
 *   Go To:  Left/Right change the highlighted field, Up/Down move between
 *           year, month and day, Confirm jumps there, Back cancels.
 *
 * Go To opens on today's date, so Confirm twice is "back to today" and there is
 * no need to spend a scarce button on a dedicated Today action.
 *
 * Works with no RTC too: it falls back to a fixed epoch month so the screen is
 * still navigable rather than blank, and says so.
 */
class CalendarActivity final : public Activity {
 public:
  CalendarActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Mode : uint8_t { Browse, GoTo };
  enum class Field : uint8_t { Year, Month, Day };

  void goToToday();
  void shiftMonth(int delta);
  void shiftYear(int delta);

  /** Apply Left/Right inside the Go To overlay to whichever field is active. */
  void adjustField(int delta);
  void openGoTo();
  void applyGoTo();

  void drawGrid(int pageWidth, int pageHeight) const;
  void drawGoTo(int pageWidth, int pageHeight) const;

  /** Zeller-style weekday for the 1st of the shown month. 0 = Sunday. */
  int firstWeekdayOfMonth() const;
  static int daysInMonth(int year, int month);
  static bool isLeapYear(int year);

  int year = 2026;
  int month = 1;  // 1-12

  // Today, captured once on entry so paging never loses the highlight.
  bool haveToday = false;
  int todayYear = 0;
  int todayMonth = 0;
  int todayDay = 0;

  Mode mode = Mode::Browse;
  Field field = Field::Year;
  int pickYear = 2026;
  int pickMonth = 1;
  int pickDay = 1;

  // Clean waveform on entry, differential for interaction. See RefreshPolicy.
  RefreshPolicy refresh_;
};
