#pragma once

#include <cstdint>

#include "MappedInputManager.h"
#include "activities/Activity.h"

/**
 * Countdown timer with pomodoro presets.
 *
 * Two things shape this on an e-ink device:
 *
 * 1. Repainting every second is not an option -- a refresh takes hundreds of
 *    milliseconds and would ghost the panel to death inside one pomodoro. The
 *    display therefore updates once a minute while more than a minute remains,
 *    then every second for the final ten, where the countdown is worth watching.
 *
 * 2. There is no buzzer. The alarm is the panel itself: a full-screen inverted
 *    "TIME UP", which is about as loud as this hardware gets.
 */
class TimerActivity final : public Activity {
 public:
  TimerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

  // A running timer must outlive the idle-sleep timeout, or the device sleeps
  // mid-countdown and the alarm never fires.
  bool preventAutoSleep() override { return state == State::Running; }

 private:
  enum class State : uint8_t { Idle, Running, Paused, Finished };

  static const uint16_t PRESET_SECONDS[];
  static const int PRESET_COUNT;

  int remainingSeconds() const;
  void start();
  void reset();

  State state = State::Idle;
  int presetIndex = 2;  // 25 minutes, the pomodoro default

  unsigned long endsAtMs = 0;
  int pausedRemaining = 0;

  // Drives the repaint cadence described above.
  int lastShownSecond = -1;
};
