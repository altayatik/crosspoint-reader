#pragma once

#include <cstdint>

#include "MappedInputManager.h"
#include "activities/Activity.h"

/**
 * A virtual pet that lives on the SD card.
 *
 * Fully offline. The interesting design problem is that this device spends
 * almost all of its life in deep sleep, so there is no process to run a
 * simulation in -- a pet that decays "in real time" would simply not exist
 * between visits.
 *
 * So it decays on *calendar days*, not elapsed milliseconds. Each visit reads
 * the RTC date, works out how many days have passed since the last one, and
 * applies that many days of hunger and boredom at once. The pet therefore ages
 * correctly whether the device was asleep, flat, or in a drawer for a fortnight,
 * and it costs nothing to run.
 *
 * With no RTC the pet simply does not age -- better than guessing and
 * starving it on every boot.
 */
class PetActivity final : public Activity {
 public:
  PetActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Mood : uint8_t { Happy, Content, Sad, Hungry, Asleep };

  // Growth stages, by age in days. The pet visibly changes as it gets older,
  // which is the only long-term reward a device this quiet can offer.
  enum class Stage : uint8_t { Hatchling, Pup, Junior, Grown, Elder };

  struct State {
    // 0..100. Hunger counts *up* toward starving; the other two count down.
    uint8_t hunger = 20;
    uint8_t happiness = 80;
    uint8_t energy = 80;
    uint16_t ageDays = 0;
    // Last visit, as a plain ordinal day number so day arithmetic is trivial
    // and survives month and year boundaries.
    uint32_t lastDay = 0;
    // Ordinal day of the last nap. Resting is once per day, so it cannot be
    // spammed into an infinite energy supply.
    uint32_t lastRestDay = 0;
    char name[16] = "";
  };

  bool load();
  bool save() const;

  /** Days since 2000-01-01. Monotonic, so subtraction gives elapsed days. */
  static uint32_t ordinalDay(int year, int month, int day);
  static bool isLeapYear(int year);

  /** Apply however many days have passed since the last visit. */
  void advanceTo(uint32_t today);

  Mood mood() const;
  const char* moodLabel() const;

  Stage stage() const;
  const char* stageLabel() const;
  /** Display name, falling back to the stage name until one is set. */
  const char* displayName() const;

  void feed();
  void play();
  void rest();
  void rename();

  void drawPet(int cx, int cy, int size) const;
  void drawStat(int x, int y, int w, const char* label, int value) const;

  State state;
  bool haveClock = false;
  uint32_t todayOrdinal = 0;
  const char* toast = nullptr;
};
