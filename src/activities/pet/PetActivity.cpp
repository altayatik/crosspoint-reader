#include "PetActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr const char* STATE_PATH = "/.crosspoint/pet.json";

// Per calendar day away. Tuned so a week of neglect is visible but not fatal:
// this is a companion, not a guilt machine.
constexpr int HUNGER_PER_DAY = 18;
constexpr int HAPPINESS_PER_DAY = 12;
constexpr int ENERGY_PER_DAY = 8;

constexpr int FEED_AMOUNT = 35;
constexpr int PLAY_HAPPINESS = 25;
constexpr int PLAY_ENERGY_COST = 12;

int clamp8(const int value) { return std::max(0, std::min(100, value)); }

}  // namespace

PetActivity::PetActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Pet", renderer, mappedInput) {}

bool PetActivity::isLeapYear(const int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

uint32_t PetActivity::ordinalDay(const int year, const int month, const int day) {
  static constexpr int CUMULATIVE[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
  if (year < 2000 || month < 1 || month > 12) return 0;

  const int years = year - 2000;
  // Leap days elapsed in whole years before this one.
  uint32_t days = static_cast<uint32_t>(years) * 365 + (years + 3) / 4 - (years + 99) / 100 + (years + 399) / 400;
  days += CUMULATIVE[month - 1];
  if (month > 2 && isLeapYear(year)) days += 1;
  days += static_cast<uint32_t>(day - 1);
  return days;
}

void PetActivity::onEnter() {
  Activity::onEnter();
  Storage.mkdir("/.crosspoint");

  const bool existed = load();

  Rtc::DateTime now;
  haveClock = halClock.isAvailable() && halClock.getDateTime(now);

  if (haveClock) {
    const uint32_t today = ordinalDay(now.year, now.month, now.day);
    todayOrdinal = today;
    if (!existed || state.lastDay == 0) {
      // First meeting. Start it fresh rather than applying decades of decay
      // against a zeroed lastDay.
      state.lastDay = today;
      state.streak = 1;
      save();
    } else if (today > state.lastDay) {
      // A visit the very next day extends the streak; any longer gap ends it.
      state.streak = (today - state.lastDay == 1) ? static_cast<uint16_t>(std::min(state.streak + 1, 9999)) : 0;
      advanceTo(today);
      save();
    }
  } else if (!existed) {
    save();
  }

  requestUpdate();
}

void PetActivity::advanceTo(const uint32_t today) {
  // Cap the catch-up so a device that sat in a drawer for a year does not
  // arrive at a pet that is simply dead on open.
  const uint32_t elapsed = std::min<uint32_t>(today - state.lastDay, 30);

  state.hunger = static_cast<uint8_t>(clamp8(state.hunger + static_cast<int>(elapsed) * HUNGER_PER_DAY));
  state.happiness = static_cast<uint8_t>(clamp8(state.happiness - static_cast<int>(elapsed) * HAPPINESS_PER_DAY));
  state.energy = static_cast<uint8_t>(clamp8(state.energy - static_cast<int>(elapsed) * ENERGY_PER_DAY));
  state.ageDays = static_cast<uint16_t>(std::min<uint32_t>(state.ageDays + (today - state.lastDay), 9999));
  state.lastDay = today;

  LOG_INF("PET", "Advanced %u day(s): hunger %u happy %u energy %u", static_cast<unsigned>(elapsed),
          state.hunger, state.happiness, state.energy);
}

bool PetActivity::load() {
  HalFile file;
  if (!Storage.openFileForRead("PET", STATE_PATH, file)) return false;

  const size_t size = file.size();
  if (size == 0 || size > 512) {
    file.close();
    return false;
  }

  std::string json;
  json.resize(size);
  const int read = file.read(&json[0], size);
  file.close();
  if (read <= 0) return false;
  json.resize(static_cast<size_t>(read));

  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    LOG_ERR("PET", "State file corrupt; starting over");
    return false;
  }

  state.hunger = static_cast<uint8_t>(clamp8(doc["hunger"] | 20));
  state.happiness = static_cast<uint8_t>(clamp8(doc["happy"] | 80));
  state.energy = static_cast<uint8_t>(clamp8(doc["energy"] | 80));
  state.ageDays = doc["age"] | 0;
  state.lastDay = doc["day"] | 0;
  state.lastRestDay = doc["rest"] | 0;
  state.streak = doc["streak"] | 0;
  const char* saved = doc["name"] | "";
  strncpy(state.name, saved, sizeof(state.name) - 1);
  state.name[sizeof(state.name) - 1] = '\0';
  return true;
}

bool PetActivity::save() const {
  JsonDocument doc;
  doc["hunger"] = state.hunger;
  doc["happy"] = state.happiness;
  doc["energy"] = state.energy;
  doc["age"] = state.ageDays;
  doc["day"] = state.lastDay;
  doc["rest"] = state.lastRestDay;
  doc["streak"] = state.streak;
  doc["name"] = state.name;

  std::string json;
  serializeJson(doc, json);

  HalFile file;
  if (!Storage.openFileForWrite("PET", STATE_PATH, file)) {
    LOG_ERR("PET", "Could not save state");
    return false;
  }
  file.write(reinterpret_cast<const uint8_t*>(json.data()), json.size());
  file.close();
  return true;
}

PetActivity::Mood PetActivity::mood() const {
  if (state.energy < 20) return Mood::Asleep;
  if (state.hunger > 70) return Mood::Hungry;
  if (state.happiness < 30) return Mood::Sad;
  if (state.happiness > 70 && state.hunger < 40) return Mood::Happy;
  return Mood::Content;
}

const char* PetActivity::moodLabel() const {
  switch (mood()) {
    case Mood::Happy:
      return tr(STR_PET_HAPPY);
    case Mood::Hungry:
      return tr(STR_PET_HUNGRY);
    case Mood::Sad:
      return tr(STR_PET_SAD);
    case Mood::Asleep:
      return tr(STR_PET_SLEEPY);
    case Mood::Content:
    default:
      return tr(STR_PET_CONTENT);
  }
}

PetActivity::Stage PetActivity::stage() const {
  if (state.ageDays < 2) return Stage::Hatchling;
  if (state.ageDays < 7) return Stage::Pup;
  if (state.ageDays < 21) return Stage::Junior;
  if (state.ageDays < 60) return Stage::Grown;
  return Stage::Elder;
}

const char* PetActivity::stageLabel() const {
  switch (stage()) {
    case Stage::Hatchling:
      return tr(STR_PET_STAGE_HATCHLING);
    case Stage::Pup:
      return tr(STR_PET_STAGE_PUP);
    case Stage::Junior:
      return tr(STR_PET_STAGE_JUNIOR);
    case Stage::Grown:
      return tr(STR_PET_STAGE_GROWN);
    case Stage::Elder:
    default:
      return tr(STR_PET_STAGE_ELDER);
  }
}

const char* PetActivity::needsLabel() const {
  // Ranked by what actually blocks interaction: a tired pet cannot play, a
  // starving one is the most urgent, and boredom is the slow one.
  if (state.hunger > 60) return tr(STR_PET_NEEDS_FOOD);
  if (state.energy < 25) return tr(STR_PET_NEEDS_REST);
  if (state.happiness < 45) return tr(STR_PET_NEEDS_PLAY);
  return tr(STR_PET_NEEDS_NOTHING);
}

const char* PetActivity::displayName() const { return state.name[0] != '\0' ? state.name : stageLabel(); }

void PetActivity::rename() {
  auto handler = [this](const ActivityResult& result) {
    if (result.isCancelled) return;
    const auto& kb = std::get<KeyboardResult>(result.data);
    strncpy(state.name, kb.text.c_str(), sizeof(state.name) - 1);
    state.name[sizeof(state.name) - 1] = '\0';
    save();
    requestUpdate();
  };
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_PET_NAME),
                                                                 std::string(state.name), sizeof(state.name) - 1,
                                                                 InputType::Text),
                         handler);
}

void PetActivity::rest() {
  // Once per calendar day. Without the guard this is just an energy button, and
  // energy is the only thing that gates playing.
  if (haveClock && todayOrdinal != 0 && state.lastRestDay == todayOrdinal) {
    toast = tr(STR_PET_ALREADY_RESTED);
    return;
  }
  state.energy = static_cast<uint8_t>(clamp8(state.energy + 40));
  state.hunger = static_cast<uint8_t>(clamp8(state.hunger + 10));
  state.lastRestDay = todayOrdinal;
  toast = tr(STR_PET_RESTED);
  save();
}

void PetActivity::feed() {
  state.hunger = static_cast<uint8_t>(clamp8(state.hunger - FEED_AMOUNT));
  state.energy = static_cast<uint8_t>(clamp8(state.energy + 8));
  toast = tr(STR_PET_FED);
  save();
}

void PetActivity::play() {
  if (state.energy < 15) {
    toast = tr(STR_PET_TOO_TIRED);
    return;
  }
  state.happiness = static_cast<uint8_t>(clamp8(state.happiness + PLAY_HAPPINESS));
  state.energy = static_cast<uint8_t>(clamp8(state.energy - PLAY_ENERGY_COST));
  state.hunger = static_cast<uint8_t>(clamp8(state.hunger + 6));
  toast = tr(STR_PET_PLAYED);
  save();
}

void PetActivity::loop() {
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    feed();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    play();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left)) {
    rest();
    requestUpdate();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    rename();
  }
}

// ---------------------------------------------------------------------------

void PetActivity::drawPet(const int cx, const int cy, const int size) const {
  const int r = size / 2;
  const Mood m = mood();

  // Body: a filled blob with a knocked-out face. Solid shapes are the only
  // thing that reads well at this size on 1-bit e-ink -- outlines at 100px+
  // look like wireframes rather than a creature.
  renderer.fillRect(cx - r, cy - r + size / 6, size, size - size / 6, true);
  // Rounded top, approximated with a couple of steps. Cheaper than a real
  // circle routine and indistinguishable at this scale.
  renderer.fillRect(cx - r + size / 8, cy - r, size - size / 4, size / 6, true);

  // Ears. They lengthen as the pet grows, which is the cheapest silhouette cue
  // available -- the outline changes even at a glance from across the room.
  static constexpr int EAR_SCALE[5] = {4, 6, 8, 10, 9};
  const int earH = size * EAR_SCALE[static_cast<int>(stage())] / 80;
  renderer.fillRect(cx - r + size / 8, cy - r - earH, size / 6, earH, true);
  renderer.fillRect(cx + r - size / 8 - size / 6, cy - r - earH, size / 6, earH, true);

  if (stage() == Stage::Grown) {
    // Collar: a knocked-out band across the neck.
    renderer.fillRect(cx - r + size / 6, cy + size / 4, size - size / 3, 4, false);
  }

  if (stage() == Stage::Elder) {
    // Whiskers: two knocked-out bars on the cheeks.
    renderer.fillRect(cx - r + size / 12, cy + size / 8, size / 5, 3, false);
    renderer.fillRect(cx + r - size / 12 - size / 5, cy + size / 8, size / 5, 3, false);
  }

  const int eyeY = cy - size / 10;
  const int eyeDx = size / 5;
  const int eyeR = std::max(3, size / 14);

  if (m == Mood::Asleep) {
    // Closed eyes: two bars.
    renderer.fillRect(cx - eyeDx - eyeR, eyeY, eyeR * 2, 3, false);
    renderer.fillRect(cx + eyeDx - eyeR, eyeY, eyeR * 2, 3, false);
  } else {
    renderer.fillRect(cx - eyeDx - eyeR / 2, eyeY - eyeR, eyeR, eyeR * 2, false);
    renderer.fillRect(cx + eyeDx - eyeR / 2, eyeY - eyeR, eyeR, eyeR * 2, false);
  }

  // Mouth: the whole mood is carried here, since eyes are two rectangles.
  const int mouthY = cy + size / 6;
  const int mouthW = size / 3;
  switch (m) {
    case Mood::Happy:
      // Open smile.
      renderer.fillRect(cx - mouthW / 2, mouthY, mouthW, size / 12, false);
      renderer.fillRect(cx - mouthW / 2 - 3, mouthY - size / 20, 3, size / 20, false);
      renderer.fillRect(cx + mouthW / 2, mouthY - size / 20, 3, size / 20, false);
      break;
    case Mood::Hungry:
      // Small round mouth.
      renderer.fillRect(cx - size / 14, mouthY, size / 7, size / 10, false);
      break;
    case Mood::Sad:
      // Frown: bar with the ends lifted.
      renderer.fillRect(cx - mouthW / 2, mouthY + size / 20, mouthW, 3, false);
      renderer.fillRect(cx - mouthW / 2 - 3, mouthY, 3, size / 20, false);
      renderer.fillRect(cx + mouthW / 2, mouthY, 3, size / 20, false);
      break;
    case Mood::Asleep:
      renderer.fillRect(cx - size / 16, mouthY, size / 8, size / 16, false);
      break;
    case Mood::Content:
    default:
      renderer.fillRect(cx - mouthW / 2, mouthY, mouthW, 3, false);
      break;
  }
}

void PetActivity::drawStat(const int x, const int y, const int w, const char* label, const int value) const {
  // The bar used to start 6px below the top of the label, i.e. straight through
  // it. It goes below the full line box now, and the label gets the UI face
  // rather than the small one so it is legible at arm's length.
  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  renderer.drawText(UI_10_FONT_ID, x, y, label, true, EpdFontFamily::BOLD);

  // Percentage on the right of the same line. A bare bar makes "nearly full"
  // and "full" indistinguishable, which matters when a stat gates an action.
  char percent[8];
  snprintf(percent, sizeof(percent), "%d%%", clamp8(value));
  const int pw = renderer.getTextWidth(UI_10_FONT_ID, percent);
  renderer.drawText(UI_10_FONT_ID, x + w - pw, y, percent, true);

  const int barY = y + lineHeight + 4;
  const int barH = 16;
  renderer.drawRect(x, barY, w, barH, 1, true);
  const int fill = (w - 4) * clamp8(value) / 100;
  if (fill > 0) renderer.fillRect(x + 2, barY + 2, fill, barH - 4, true);
}

void PetActivity::render(RenderLock&&) {
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const int margin = 24;

  renderer.clearScreen();
  renderer.drawCenteredText(UI_12_FONT_ID, 40, displayName(), true, EpdFontFamily::BOLD);

  char subtitle[64];
  if (state.streak > 1) {
    snprintf(subtitle, sizeof(subtitle), "%s  -  %s %u  -  %s %u", stageLabel(), tr(STR_PET_AGE),
             static_cast<unsigned>(state.ageDays), tr(STR_PET_STREAK), static_cast<unsigned>(state.streak));
  } else {
    snprintf(subtitle, sizeof(subtitle), "%s  -  %s %u", stageLabel(), tr(STR_PET_AGE),
             static_cast<unsigned>(state.ageDays));
  }
  renderer.drawCenteredText(SMALL_FONT_ID, 70, subtitle);

  // The pet grows with its stage, from a small hatchling to a full-size adult.
  const int maxSize = std::min(pageWidth - margin * 4, 210);
  static constexpr int STAGE_SCALE[5] = {55, 70, 85, 100, 95};
  const int petSize = maxSize * STAGE_SCALE[static_cast<int>(stage())] / 100;
  drawPet(pageWidth / 2, 210, petSize);

  // A ground line under the pet. Without it a floating blob reads as a bug
  // rather than a creature, and it gives the smaller stages somewhere to be.
  const int floorY = 210 + maxSize / 2 + 8;
  renderer.drawLine(margin + 30, floorY, pageWidth - margin - 30, floorY, 2, true);
  const int shadowW = petSize * 3 / 4;
  renderer.fillRect(pageWidth / 2 - shadowW / 2, floorY + 4, shadowW, 3, true);

  renderer.drawCenteredText(UI_12_FONT_ID, 330, moodLabel(), true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, 358, needsLabel());

  const int statW = pageWidth - margin * 2;
  // Hunger is inverted for display: a full bar should always mean "good".
  drawStat(margin, 382, statW, tr(STR_PET_FULLNESS), 100 - state.hunger);
  drawStat(margin, 440, statW, tr(STR_PET_HAPPINESS), state.happiness);
  drawStat(margin, 498, statW, tr(STR_PET_ENERGY), state.energy);

  if (toast) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - 140, toast, true, EpdFontFamily::BOLD);
  }
  if (!haveClock) {
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 112, tr(STR_PET_NO_CLOCK));
  }

  // A bottom note, not drawSideButtonHints: those draw rotated down the screen
  // edges at a fixed height and clip their own text.
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 86, tr(STR_PET_NAME_HINT));

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_PET_FEED), tr(STR_PET_REST), tr(STR_PET_PLAY));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}
