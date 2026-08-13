#pragma once

#include <cstdint>

#include "MappedInputManager.h"
#include "activities/Activity.h"

/**
 * Countdown timer with pomodoro presets and a manually dialled duration.
 *
 * Two things shape this on an e-ink device:
 *
 * 1. Repainting every second is not an option -- a refresh takes hundreds of
 *    milliseconds and would ghost the panel to death inside one pomodoro. The
 *    repaint interval therefore scales with how much time is left; see
 *    repaintIntervalFor().
 *
 * 2. There is no buzzer. The alarm is the panel itself: a full-screen inverted
 *    "TIME UP", which is about as loud as this hardware gets.
 *
 * Controls while stopped:
 *   Presets  Left/Right pick a preset, side Up switches to Custom.
 *   Custom   Left/Right change the active field, side Up/Down move between
 *            hours, minutes and seconds, Back returns to the presets.
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
  enum class Picker : uint8_t { Preset, Custom };
  enum class Field : uint8_t { Hours, Minutes, Seconds };

  static const uint16_t PRESET_SECONDS[];
  static const int PRESET_COUNT;

  /**
   * Seconds between repaints at a given remaining time.
   *
   *   <= 10s   every second   the part worth watching
   *   <= 1min  every 5s
   *   <= 10min every 30s
   *   <= 1h    every minute
   *   >  1h    every 5min
   *
   * A 25 minute pomodoro costs ~30 refreshes instead of the 1500 a per-second
   * repaint would.
   */
  static int repaintIntervalFor(int remaining);

  /** Whatever the user has dialled in, in seconds. */
  int selectedSeconds() const;

  int remainingSeconds() const;
  void adjustCustom(int delta);
  void start();
  void reset();

  void drawCustomFields(int pageWidth, int y) const;

  State state = State::Idle;
  Picker picker = Picker::Preset;
  Field field = Field::Minutes;

  int presetIndex = 2;      // 25 minutes, the pomodoro default
  int customSeconds = 300;  // 5:00, a sane starting point to dial from

  unsigned long endsAtMs = 0;
  int pausedRemaining = 0;

  // Bucket index of the last painted frame; a repaint happens when it changes.
  int lastPaintBucket = -1;
};
