#include "TimerActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>

#include "components/UITheme.h"
#include "fontIds.h"

const uint16_t TimerActivity::PRESET_SECONDS[] = {60, 300, 1500, 3000, 600, 900};
const int TimerActivity::PRESET_COUNT = sizeof(PRESET_SECONDS) / sizeof(PRESET_SECONDS[0]);

TimerActivity::TimerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Timer", renderer, mappedInput) {}

void TimerActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

int TimerActivity::remainingSeconds() const {
  if (state == State::Paused) return pausedRemaining;
  if (state != State::Running) return PRESET_SECONDS[presetIndex];
  const unsigned long now = millis();
  // Unsigned subtraction, so this stays correct across the millis() rollover.
  if (static_cast<long>(now - endsAtMs) >= 0) return 0;
  return static_cast<int>((endsAtMs - now + 999UL) / 1000UL);
}

void TimerActivity::start() {
  const int seconds = (state == State::Paused) ? pausedRemaining : PRESET_SECONDS[presetIndex];
  endsAtMs = millis() + static_cast<unsigned long>(seconds) * 1000UL;
  state = State::Running;
  lastShownSecond = -1;
  LOG_INF("TMR", "Started for %d s", seconds);
}

void TimerActivity::reset() {
  state = State::Idle;
  pausedRemaining = 0;
  lastShownSecond = -1;
}

void TimerActivity::loop() {
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state == State::Idle) {
      activityManager.goHome();
    } else {
      reset();
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    switch (state) {
      case State::Idle:
        start();
        break;
      case State::Running:
        pausedRemaining = remainingSeconds();
        state = State::Paused;
        break;
      case State::Paused:
        start();
        break;
      case State::Finished:
        reset();
        break;
    }
    requestUpdate();
    return;
  }

  // Preset selection, only while stopped.
  if (state == State::Idle) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Up)) {
      presetIndex = (presetIndex + PRESET_COUNT - 1) % PRESET_COUNT;
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
        mappedInput.wasReleased(MappedInputManager::Button::Down)) {
      presetIndex = (presetIndex + 1) % PRESET_COUNT;
      requestUpdate();
      return;
    }
  }

  if (state != State::Running) return;

  const int remaining = remainingSeconds();
  if (remaining <= 0) {
    state = State::Finished;
    lastShownSecond = -1;
    LOG_INF("TMR", "Finished");
    requestUpdate();
    return;
  }

  // Repaint cadence: once a minute normally, every second for the last ten.
  // A per-second repaint for 25 minutes would be 1500 e-ink refreshes.
  const bool finalCountdown = remaining <= 10;
  const int tick = finalCountdown ? remaining : remaining / 60;
  if (tick != lastShownSecond) {
    lastShownSecond = tick;
    requestUpdate();
  }
}

void TimerActivity::render(RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  if (state == State::Finished) {
    // The alarm. No buzzer on this hardware, so the panel itself is the alert:
    // full-screen inverted, which is the loudest thing a 1-bit display can do.
    renderer.clearScreen();
    renderer.fillRect(0, 0, pageWidth, pageHeight, true);
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 10, tr(STR_TIMER_DONE), false, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, tr(STR_TIMER_DISMISS), false);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    return;
  }

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 46, tr(STR_TIMER_TITLE), true, EpdFontFamily::BOLD);

  const int remaining = remainingSeconds();
  char clock[16];
  snprintf(clock, sizeof(clock), "%d:%02d", remaining / 60, remaining % 60);

  // The big numeral. NotoSerif 18 is the largest face the firmware carries;
  // there is no 100px font here the way there is on the server-rendered
  // dashboard, so this is as large as the countdown can honestly get.
  renderer.drawCenteredText(NOTOSERIF_18_FONT_ID, pageHeight / 2, clock, true, EpdFontFamily::BOLD);

  const char* status = state == State::Running  ? tr(STR_TIMER_RUNNING)
                       : state == State::Paused ? tr(STR_TIMER_PAUSED)
                                                : tr(STR_TIMER_READY);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 44, status);

  if (state == State::Idle) {
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 66, tr(STR_TIMER_PICK));
  } else if (state == State::Running && remaining > 10) {
    // Set expectations: a static-looking screen is otherwise indistinguishable
    // from a crashed one.
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 66, tr(STR_TIMER_MINUTE_TICK));
  }

  const char* confirm = state == State::Running ? tr(STR_TIMER_PAUSE) : tr(STR_TIMER_START);
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, state == State::Idle ? "<" : "",
                                            state == State::Idle ? ">" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // FAST for the ticking countdown -- the only thing changing is a couple of
  // digits, and HALF every second would flash badly. HALF elsewhere to clear
  // any ghosting the fast updates leave behind.
  renderer.displayBuffer(state == State::Running ? HalDisplay::FAST_REFRESH : HalDisplay::HALF_REFRESH);
}
