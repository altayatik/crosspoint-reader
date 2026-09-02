#include "DashboardActivity.h"

#include <Bitmap.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "network/NetService.h"

namespace {

// Download here first, validate, then rename over LIVE_PATH. A half-written
// file can therefore never become the displayed image, and the last good frame
// survives a power loss mid-download.
constexpr const char* TEMP_PATH = "/.crosspoint/dashboard.tmp";
constexpr const char* LIVE_PATH = "/.crosspoint/dashboard.bmp";

// A 528x792 1-bpp BMP is 53,918 bytes; a 480x800 one is 48,062. Anything far
// outside that band is not a panel image, and rejecting it early avoids
// handing a nonsense file to the bitmap parser.
constexpr size_t MIN_PLAUSIBLE_BMP = 4 * 1024;
constexpr size_t MAX_PLAUSIBLE_BMP = 512 * 1024;

// Absolute ceiling on one refresh, independent of the configured timeouts.
// Whatever goes wrong, the device must not sit awake burning battery.
constexpr unsigned long HARD_CYCLE_LIMIT_MS = 90UL * 1000UL;

/** Byte-for-byte compare, used to skip an e-ink refresh when nothing changed. */
bool filesIdentical(const char* a, const char* b) {
  HalFile fileA;
  HalFile fileB;
  if (!Storage.openFileForRead("DSH", a, fileA)) return false;
  if (!Storage.openFileForRead("DSH", b, fileB)) return false;
  if (fileA.size() != fileB.size()) return false;

  uint8_t bufA[512];
  uint8_t bufB[512];
  for (;;) {
    const int readA = fileA.read(bufA, sizeof(bufA));
    const int readB = fileB.read(bufB, sizeof(bufB));
    if (readA != readB) return false;
    if (readA <= 0) return readA == 0;
    if (memcmp(bufA, bufB, static_cast<size_t>(readA)) != 0) return false;
  }
}

}  // namespace

DashboardActivity::DashboardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const bool autoSleep)
    : Activity("Dashboard", renderer, mappedInput), autoSleep(autoSleep) {}

void DashboardActivity::onEnter() {
  Activity::onEnter();
  Storage.mkdir("/.crosspoint");

  // Show the last frame straight away. The panel is already holding it after a
  // timer wake, but coming from the Home menu it is not, and watching a menu
  // sit there through a Wi-Fi association and a 54KB download is what made this
  // feel unresponsive. Painting from the SD cache first costs one refresh and
  // makes the screen look instant.
  if (!autoSleep && Storage.exists(LIVE_PATH)) {
    phase = Phase::Image;
    requestUpdateAndWait();
  }

  busy = true;
  lastOutcome = refresh();
  busy = false;
  refreshed = true;

  LOG_INF("DSH", "Refresh finished: %s", outcomeName(lastOutcome));

  if (!autoSleep) {
    // Development mode: stay awake so the next build can be iterated on
    // without power-cycling. Show what happened, since there is no serial
    // console attached to a device sitting on a shelf.
    if (lastOutcome != Outcome::Displayed && lastOutcome != Outcome::Unchanged) {
      showStatus(outcomeName(lastOutcome));
    }
    return;
  }

  // Production: back to sleep immediately. This call does not return, so
  // loop() and onExit() are never reached -- same pattern main.cpp uses when it
  // sleeps straight out of setup().
  //
  // That means the teardown enterDeepSleep() normally performs has to be
  // repeated here. WiFi is already down (above); the IMU and panel follow.
  // powerDownRailsForSleep() inside startDeepSleep() requires display.deepSleep()
  // to have run first, so the panel controller gets its command while its rail
  // is still up.
  const uint32_t seconds = static_cast<uint32_t>(SETTINGS.getDashboardRefreshMinutes()) * 60U;
  LOG_INF("DSH", "Sleeping for %u minutes", SETTINGS.getDashboardRefreshMinutes());
  halTiltSensor.deepSleep();
  display.deepSleep();
  powerManager.startDeepSleepWithTimer(gpio, seconds);
}

void DashboardActivity::onExit() { Activity::onExit(); }

void DashboardActivity::loop() {
  Activity::loop();
  if (!refreshed) return;

  // Only reachable with dashboardDeepSleep off. Confirm re-fetches, Back leaves
  // dashboard mode for the normal UI.
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    busy = true;
    lastOutcome = refresh();
    busy = false;
    if (lastOutcome != Outcome::Displayed && lastOutcome != Outcome::Unchanged) {
      showStatus(outcomeName(lastOutcome));
    }
    return;
  }

  // Night-mode toggle. Cycles the override and immediately re-fetches, because
  // the theme is decided server-side -- there is nothing to invert locally.
  if (mappedInput.wasReleased(MappedInputManager::Button::Right)) {
    cycleTheme();
    busy = true;
    lastOutcome = refresh();
    busy = false;
    if (lastOutcome != Outcome::Displayed && lastOutcome != Outcome::Unchanged) {
      showStatus(outcomeName(lastOutcome));
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goHome();
  }
}

// ---------------------------------------------------------------------------

DashboardActivity::Outcome DashboardActivity::refresh() {
  const unsigned long started = millis();
  HalPowerManager::Lock powerLock;  // full clock speed for TLS and SD writes

  showBanner(tr(STR_DASHBOARD_CONNECTING));
  if (!connectWifi()) {
    return NET.hasCredentials() ? Outcome::WifiFailed : Outcome::NoWifi;
  }

  // Opportunistic clock repair. Not syncFromNTP(): that writes UTC, and every
  // date-aware screen in this build reads the RTC as local time. NetService
  // fetches the local reading instead. Failure is ignored -- it is not what we
  // came for.
  if (!NET.clockIsSet()) NET.syncClock();

  if (millis() - started > HARD_CYCLE_LIMIT_MS) {
    LOG_ERR("DSH", "Cycle budget exhausted before download");
    return Outcome::FetchFailed;
  }

  showBanner(tr(STR_DASHBOARD_FETCHING));
  const std::string url = buildUrl();
  if (url.empty()) {
    LOG_ERR("DSH", "No dashboard URL configured");
    return Outcome::FetchFailed;
  }
  if (!download(url)) return Outcome::FetchFailed;

  const Outcome committed = commitDownload();
  if (committed == Outcome::Unchanged) {
    // Identical bytes do not mean the panel is already showing them.
    //
    // Skipping the paint here is only safe on an unattended timer wake: deep
    // sleep is a full reset, but e-ink holds its last image with no power and
    // showBanner() draws nothing in that mode, so the dashboard really is still
    // on the glass and repainting it would waste a refresh.
    //
    // Every other route has drawn over it -- the Home menu we launched from, or
    // the progress banner -- so the image has to go back on. Returning early
    // here is what left "Fetching dashboard" on screen for good.
    if (autoSleep) return Outcome::Unchanged;
    return displayImage() ? Outcome::Unchanged : Outcome::BadImage;
  }
  if (committed != Outcome::Displayed) return committed;

  return displayImage() ? Outcome::Displayed : Outcome::BadImage;
}

// ---------------------------------------------------------------------------

std::string DashboardActivity::buildUrl() const {
  std::string url(SETTINGS.dashboardUrl);
  if (url.empty()) return url;

  // AUTO sends no parameter at all, so the server keeps deciding from the sun.
  // Only an explicit override is worth putting on the wire -- and leaving the
  // URL untouched means the CDN keeps one cache entry instead of three.
  const char* theme = nullptr;
  switch (SETTINGS.dashboardTheme) {
    case CrossPointSettings::DASHBOARD_THEME_LIGHT:
      theme = "light";
      break;
    case CrossPointSettings::DASHBOARD_THEME_DARK:
      theme = "dark";
      break;
    default:
      return url;
  }

  // The URL is user-editable and may already carry a query string.
  url += (url.find('?') == std::string::npos) ? '?' : '&';
  url += "theme=";
  url += theme;
  return url;
}

void DashboardActivity::cycleTheme() {
  // Auto -> Light -> Night -> Auto. Persisted, because an override that reset
  // on every wake would be useless on a device that spends its life asleep.
  SETTINGS.dashboardTheme = (SETTINGS.dashboardTheme + 1) % CrossPointSettings::DASHBOARD_THEME_COUNT;
  SETTINGS.saveToFile();
  LOG_INF("DSH", "Theme override now %u", SETTINGS.dashboardTheme);
}

bool DashboardActivity::connectWifi() {
  // NetService associated at boot and holds the link; just wait for it.
  return NET.ensureConnected(std::max<uint32_t>(5, SETTINGS.dashboardWifiTimeoutSeconds) * 1000UL);
}


bool DashboardActivity::download(const std::string& url) {
  Storage.remove(TEMP_PATH);

  // The timeout goes to HttpDownloader, which pushes it into
  // esp_http_client_config_t::timeout_ms and derives a whole-transfer deadline
  // from it.
  //
  // An earlier version tried to enforce this from the progress callback by
  // flipping a cancel flag. That did not work: the callback only fires once
  // bytes are arriving AND a Content-Length was present, so it could never
  // interrupt the case that actually matters -- a server that completes the TLS
  // handshake and then goes quiet. The device sat on the default 60s
  // per-operation timeout with the "Fetching dashboard" banner up, which looked
  // exactly like a hang.
  const uint32_t timeoutMs = std::max<uint32_t>(5, SETTINGS.dashboardHttpTimeoutSeconds) * 1000U;

  const auto error = HttpDownloader::downloadToFile(url, TEMP_PATH, nullptr, nullptr, "", "", timeoutMs);
  if (error != HttpDownloader::OK) {
    LOG_ERR("DSH", "Download failed (%d)", static_cast<int>(error));
    Storage.remove(TEMP_PATH);
    return false;
  }
  return true;
}

DashboardActivity::Outcome DashboardActivity::commitDownload() {
  HalFile temp;
  if (!Storage.openFileForRead("DSH", TEMP_PATH, temp)) {
    LOG_ERR("DSH", "Downloaded file missing");
    return Outcome::FetchFailed;
  }

  const size_t size = temp.size();
  if (size < MIN_PLAUSIBLE_BMP || size > MAX_PLAUSIBLE_BMP) {
    LOG_ERR("DSH", "Implausible image size: %u bytes", static_cast<unsigned>(size));
    temp.close();
    Storage.remove(TEMP_PATH);
    return Outcome::BadImage;
  }

  // Parse before promoting: a 200 response carrying an HTML error page would
  // otherwise overwrite a perfectly good dashboard with garbage.
  Bitmap probe(temp);
  const auto parsed = probe.parseHeaders();
  if (parsed != BmpReaderError::Ok) {
    LOG_ERR("DSH", "Not a usable BMP: %s", Bitmap::errorToString(parsed));
    temp.close();
    Storage.remove(TEMP_PATH);
    return Outcome::BadImage;
  }
  LOG_DBG("DSH", "Image %dx%d, %u bpp", probe.getWidth(), probe.getHeight(), probe.getBpp());
  temp.close();

  // Skip the refresh when the bytes are identical. An e-ink update is the most
  // expensive and most visible thing this activity does, so not doing it is
  // worth the comparison -- especially on a manual re-press.
  if (Storage.exists(LIVE_PATH) && filesIdentical(TEMP_PATH, LIVE_PATH)) {
    LOG_INF("DSH", "Image unchanged, skipping refresh");
    Storage.remove(TEMP_PATH);
    return Outcome::Unchanged;
  }

  Storage.remove(LIVE_PATH);
  if (!Storage.rename(TEMP_PATH, LIVE_PATH)) {
    LOG_ERR("DSH", "Could not promote temp file");
    Storage.remove(TEMP_PATH);
    return Outcome::FetchFailed;
  }
  return Outcome::Displayed;
}

bool DashboardActivity::displayImage() {
  // Validate here, on the calling task, rather than having render() report back
  // through a member. Passing the result across tasks would mean refresh()'s
  // outcome depended on a write cppcheck cannot see, and it flagged the
  // resulting condition as always false -- correctly, from what it could tell.
  //
  // The re-parse costs one header read of a file commitDownload() has already
  // validated, which is cheap next to the e-ink refresh that follows.
  {
    HalFile file;
    if (!Storage.openFileForRead("DSH", LIVE_PATH, file)) {
      LOG_ERR("DSH", "Image missing at display time");
      return false;
    }
    Bitmap probe(file);
    if (probe.parseHeaders() != BmpReaderError::Ok) {
      LOG_ERR("DSH", "Image no longer parses");
      return false;
    }
  }

  // The drawing itself happens in render(), on the render task, under the
  // render lock. Block until it has finished so the caller can shut the radio
  // down and sleep knowing the panel has been written.
  phase = Phase::Image;
  requestUpdateAndWait();
  return true;
}

// ---------------------------------------------------------------------------

void DashboardActivity::showBanner(const char* text) {
  // Every banner costs a real e-ink refresh. On the unattended path that would
  // mean the panel visibly flashes twice on its way to painting the frame --
  // every fifteen minutes, on a wall, with nobody watching. Progress is only
  // worth showing when a human is actually holding the device.
  if (autoSleep) return;

  phase = Phase::Banner;
  message = text;
  requestUpdateAndWait();
}

void DashboardActivity::showStatus(const char* text) {
  phase = Phase::Status;
  message = text;
  requestUpdateAndWait();
}

const char* DashboardActivity::themeLabel() const {
  switch (SETTINGS.dashboardTheme) {
    case CrossPointSettings::DASHBOARD_THEME_LIGHT:
      return tr(STR_DASHBOARD_THEME_LIGHT);
    case CrossPointSettings::DASHBOARD_THEME_DARK:
      return tr(STR_DASHBOARD_THEME_DARK);
    default:
      return tr(STR_DASHBOARD_THEME_AUTO);
  }
}

void DashboardActivity::render(RenderLock&&) {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  switch (phase) {
    case Phase::Banner: {
      // Nothing to say yet (the unattended path never sets a message): leave
      // the panel showing the previous frame rather than flashing it blank.
      if (message == nullptr) return;
      // The previous dashboard stays visible under the popup, so a failed cycle
      // degrades to "yesterday's numbers with a banner" rather than a blank
      // screen. drawPopup ends in displayBuffer().
      GUI.drawPopup(renderer, message);
      return;
    }

    case Phase::Status: {
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, message == nullptr ? "" : message);
      // Not STR_FORCE_REFRESH ("Refresh Screen") -- it overflows the button
      // hint slot and gets clipped mid-word on the panel.
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DASHBOARD_REFRESH_BTN), "", themeLabel());
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      return;
    }

    case Phase::Image:
    default:
      break;
  }

  HalFile file;
  if (!Storage.openFileForRead("DSH", LIVE_PATH, file)) return;

  Bitmap bitmap(file);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return;

  const int x = std::max(0, (pageWidth - bitmap.getWidth()) / 2);
  const int y = std::max(0, (pageHeight - bitmap.getHeight()) / 2);

  renderer.clearScreen();
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0);

  // Button hints, but only when someone is holding the device. Unattended, the
  // panel should be nothing but dashboard -- hints on a wall display are noise,
  // and drawing them would waste the bottom band the server layout already
  // leaves blank for exactly this purpose.
  if (!autoSleep) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_DASHBOARD_REFRESH_BTN), "", themeLabel());
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  // HALF is the single-pass clean waveform the OEM firmware uses for its sleep
  // image. FAST is differential and would accumulate artifacts over hundreds of
  // unattended updates; FULL runs the multi-flash GC waveform and blinks. See
  // SleepActivity::renderBitmapSleepScreen for the same reasoning.
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

const char* DashboardActivity::outcomeName(const Outcome outcome) {
  switch (outcome) {
    case Outcome::Displayed:
      return tr(STR_DASHBOARD_FETCHING);
    case Outcome::Unchanged:
      return tr(STR_DASHBOARD_UNCHANGED);
    case Outcome::NoWifi:
      return tr(STR_DASHBOARD_NO_WIFI);
    case Outcome::WifiFailed:
      return tr(STR_DASHBOARD_WIFI_FAILED);
    case Outcome::FetchFailed:
      return tr(STR_DASHBOARD_FETCH_FAILED);
    case Outcome::BadImage:
    default:
      return tr(STR_DASHBOARD_BAD_IMAGE);
  }
}
