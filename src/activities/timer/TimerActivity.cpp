#include "TimerActivity.h"

#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>

#include "components/AppStatusBar.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr int MAX_CUSTOM_SECONDS = 23 * 3600 + 59 * 60 + 59;

void formatDuration(char* out, const size_t size, const int seconds) {
  const int hours = seconds / 3600;
  const int minutes = (seconds % 3600) / 60;
  const int secs = seconds % 60;
  if (hours > 0) {
    snprintf(out, size, "%d:%02d:%02d", hours, minutes, secs);
  } else {
    snprintf(out, size, "%d:%02d", minutes, secs);
  }
}

}  // namespace

const uint16_t TimerActivity::PRESET_SECONDS[] = {30, 60, 300, 600, 1800, 3600};
const int TimerActivity::PRESET_COUNT = sizeof(PRESET_SECONDS) / sizeof(PRESET_SECONDS[0]);

TimerActivity::TimerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Timer", renderer, mappedInput) {}

void TimerActivity::onEnter() {
  Activity::onEnter();
  requestUpdate();
}

int TimerActivity::repaintIntervalFor(const int remaining) {
  if (remaining <= 60) return 1;      // under a minute: every second
  if (remaining <= 300) return 5;     // 1-5 min
  if (remaining <= 1800) return 30;   // 5-30 min
  if (remaining <= 3600) return 60;   // 30-60 min
  return 300;                         // over an hour
}

bool TimerActivity::isCustom() const { return presetIndex >= PRESET_COUNT; }

int TimerActivity::selectedSeconds() const {
  return isCustom() ? customSeconds : static_cast<int>(PRESET_SECONDS[presetIndex]);
}

int TimerActivity::remainingSeconds() const {
  if (state == State::Paused) return pausedRemaining;
  if (state != State::Running) return selectedSeconds();
  const unsigned long now = millis();
  // Unsigned subtraction, so this stays correct across the millis() rollover.
  if (static_cast<long>(now - endsAtMs) >= 0) return 0;
  return static_cast<int>((endsAtMs - now + 999UL) / 1000UL);
}

void TimerActivity::adjustCustom(const int delta) {
  int hours = customSeconds / 3600;
  int minutes = (customSeconds % 3600) / 60;
  int seconds = customSeconds % 60;

  // Each field wraps within itself rather than carrying, so holding a direction
  // cannot silently roll hours over while the user is dialling minutes.
  const auto wrapField = [delta](int value, const int limit) {
    value = (value + delta) % limit;
    return value < 0 ? value + limit : value;
  };

  switch (field) {
    case Field::Hours:
      hours = wrapField(hours, 24);
      break;
    case Field::Minutes:
      minutes = wrapField(minutes, 60);
      break;
    case Field::Seconds:
      seconds = wrapField(seconds, 60);
      break;
  }

  customSeconds = std::min(MAX_CUSTOM_SECONDS, hours * 3600 + minutes * 60 + seconds);
}

void TimerActivity::start() {
  const int seconds = (state == State::Paused) ? pausedRemaining : selectedSeconds();
  if (seconds <= 0) return;  // starting a zero-length timer would fire instantly
  endsAtMs = millis() + static_cast<unsigned long>(seconds) * 1000UL;
  state = State::Running;
  lastPaintBucket = -1;
  LOG_INF("TMR", "Started for %d s", seconds);
}

void TimerActivity::reset() {
  state = State::Idle;
  pausedRemaining = 0;
  lastPaintBucket = -1;
}

void TimerActivity::loop() {
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (state != State::Idle) {
      reset();
      requestUpdate();
    } else if (editing) {
      // Back leaves the editor before it leaves the app, so an accidental
      // Custom is one press to undo.
      editing = false;
      requestUpdate();
    } else {
      activityManager.goHome();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    switch (state) {
      case State::Idle:
        if (isCustom() && !editing) {
          // First Confirm on Custom opens the editor; the second starts it.
          editing = true;
        } else {
          start();
        }
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

  // Duration selection, only while stopped.
  if (state == State::Idle) {
    // Custom is the entry after the last preset, so the cycle is one longer.
    const int options = PRESET_COUNT + 1;

    if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
      if (editing) {
        adjustCustom(-1);
      } else {
        presetIndex = (presetIndex + options - 1) % options;
      }
      requestUpdate();
      return;
    }
    if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      if (editing) {
        adjustCustom(1);
      } else {
        presetIndex = (presetIndex + 1) % options;
      }
      requestUpdate();
      return;
    }
    if (editing) {
      if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
        field = field == Field::Hours ? Field::Seconds : static_cast<Field>(static_cast<uint8_t>(field) - 1);
        requestUpdate();
        return;
      }
      if (mappedInput.wasReleased(MappedInputManager::Button::Down)) {
        field = field == Field::Seconds ? Field::Hours : static_cast<Field>(static_cast<uint8_t>(field) + 1);
        requestUpdate();
        return;
      }
    }
  }

  if (state != State::Running) return;

  const int remaining = remainingSeconds();
  if (remaining <= 0) {
    state = State::Finished;
    lastPaintBucket = -1;
    LOG_INF("TMR", "Finished");
    requestUpdate();
    return;
  }

  const int bucket = remaining / repaintIntervalFor(remaining);
  if (bucket != lastPaintBucket) {
    lastPaintBucket = bucket;
    requestUpdate();
  }
}

// ---------------------------------------------------------------------------

void TimerActivity::drawCustomFields(const int pageWidth, const int y) const {
  // Three boxed fields, active one inverted. This sits under the big numeral
  // and is what Left/Right act on.
  const char* names[3] = {tr(STR_TIMER_HOURS), tr(STR_TIMER_MINUTES), tr(STR_TIMER_SECONDS)};
  const int values[3] = {customSeconds / 3600, (customSeconds % 3600) / 60, customSeconds % 60};

  const int boxW = 96;
  const int boxH = 54;
  const int gap = 12;
  const int totalW = boxW * 3 + gap * 2;
  const int left = (pageWidth - totalW) / 2;

  for (int i = 0; i < 3; ++i) {
    const int bx = left + i * (boxW + gap);
    const bool active = static_cast<int>(field) == i;

    if (active) {
      renderer.fillRect(bx, y, boxW, boxH, true);
    } else {
      renderer.drawRect(bx, y, boxW, boxH);
    }

    char value[4];
    snprintf(value, sizeof(value), "%02d", values[i]);
    const int vw = renderer.getTextWidth(UI_12_FONT_ID, value, EpdFontFamily::BOLD);
    renderer.drawText(UI_12_FONT_ID, bx + boxW / 2 - vw / 2, y + 8, value, !active, EpdFontFamily::BOLD);

    const int nw = renderer.getTextWidth(SMALL_FONT_ID, names[i]);
    renderer.drawText(SMALL_FONT_ID, bx + boxW / 2 - nw / 2, y + boxH + 6, names[i], true);
  }
}

void TimerActivity::render(RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();

  if (state == State::Finished) {
    // The alarm. No buzzer on this hardware, so the panel itself is the alert:
    // full-screen inverted, which is the loudest thing a 1-bit display can do.
    // No status bar here on purpose: the alarm should be unmistakable, and a
    // strip of normal UI across the top softens it.
    renderer.clearScreen();
    renderer.fillRect(0, 0, pageWidth, pageHeight, true);
    renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 10, tr(STR_TIMER_DONE), false, EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, tr(STR_TIMER_DISMISS), false);
    renderer.displayBuffer(refresh_.next());
    return;
  }

  renderer.clearScreen();
  AppStatusBar::draw(renderer, tr(STR_TIMER_TITLE));

  const int remaining = remainingSeconds();
  char clock[16];
  formatDuration(clock, sizeof(clock), remaining);

  // The big numeral. NotoSerif 18 is the largest face the firmware carries;
  // there is no 100px font here the way there is on the server-rendered
  // dashboard, so this is as large as the countdown can honestly get.
  const int clockY = editing ? pageHeight / 2 - 90 : pageHeight / 2;
  renderer.drawCenteredText(NOTOSERIF_18_FONT_ID, clockY, clock, true, EpdFontFamily::BOLD);

  const char* status = state == State::Running  ? tr(STR_TIMER_RUNNING)
                       : state == State::Paused ? tr(STR_TIMER_PAUSED)
                                                : tr(STR_TIMER_READY);
  renderer.drawCenteredText(UI_10_FONT_ID, clockY + 44, status);

  if (state == State::Idle && editing) {
    drawCustomFields(pageWidth, clockY + 84);
  } else if (state == State::Idle) {
    // Name the selection so Custom is visibly one of the options rather than a
    // mode you have to already know about.
    renderer.drawCenteredText(UI_10_FONT_ID, clockY + 78, isCustom() ? tr(STR_TIMER_CUSTOM) : tr(STR_TIMER_PICK));
  } else if (state == State::Running && remaining > 10) {
    // Set expectations: a static-looking screen is otherwise indistinguishable
    // from a crashed one. Say how often it will actually move.
    char note[48];
    const int interval = repaintIntervalFor(remaining);
    if (interval >= 60) {
      snprintf(note, sizeof(note), "%s %d min", tr(STR_TIMER_UPDATES_EVERY), interval / 60);
    } else {
      snprintf(note, sizeof(note), "%s %d s", tr(STR_TIMER_UPDATES_EVERY), interval);
    }
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 66, note);
  }

  const char* confirm = state == State::Running                  ? tr(STR_TIMER_PAUSE)
                        : (state == State::Idle && isCustom() && !editing) ? tr(STR_TIMER_SET)
                                                                   : tr(STR_TIMER_START);
  const bool adjustable = state == State::Idle;
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), confirm, adjustable ? "-" : "", adjustable ? "+" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  // A note rather than side-button hints: those are drawn rotated down the
  // screen edges at a fixed height and cut across the content.
  if (state == State::Idle && editing) {
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 88, tr(STR_TIMER_FIELD_HINT));
  }

  // FAST for the ticking countdown -- the only thing changing is a couple of
  // digits, and HALF every tick would flash badly. HALF elsewhere to clear any
  // ghosting the fast updates leave behind.
  renderer.displayBuffer(state == State::Running ? HalDisplay::FAST_REFRESH : HalDisplay::HALF_REFRESH);
}
