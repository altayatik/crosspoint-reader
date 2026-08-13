#pragma once

#include <Rtc.h>

#include <cstdint>

#include "MappedInputManager.h"
#include "activities/Activity.h"

/**
 * A plain month calendar off the DS3231.
 *
 * No events, no sync, no network -- a wall calendar you can hold. Left/Right
 * page months, Confirm jumps back to today, Back exits.
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
  void goToToday();
  void shiftMonth(int delta);

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
};
