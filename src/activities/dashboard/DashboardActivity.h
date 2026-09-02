#pragma once

#include <cstdint>
#include <string>

#include "MappedInputManager.h"
#include "activities/Activity.h"

/**
 * X3 dashboard mode: turn the device into a wireless display for a
 * server-rendered 1-bit BMP.
 *
 *   wake -> connect WiFi -> GET the image -> validate -> display -> WiFi off
 *   -> deep sleep with a timer -> wake again
 *
 * The firmware is deliberately dumb. All layout, typography, data aggregation
 * and computation happen on the server; this activity downloads a bitmap that
 * is already exactly panel-sized and paints it. See
 * docs/x3-dashboard-mode.md.
 *
 * Failure philosophy: an e-ink panel holds its image with zero power, so
 * "do nothing" is a perfectly good fallback. Every failure path leaves the
 * panel untouched, shuts the radio down and sleeps until the next cycle. The
 * user keeps looking at slightly stale data instead of an error screen, which
 * is the better outcome for a wall dashboard.
 */
class DashboardActivity final : public Activity {
 public:
  DashboardActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, bool autoSleep);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

  // The refresh does its own blocking work and must not be interrupted by the
  // idle-sleep timer part way through a download.
  bool preventAutoSleep() override { return busy; }

 private:
  enum class Outcome : uint8_t {
    Displayed,   // new image fetched and painted
    Unchanged,   // server said 304, or bytes matched the cached copy
    NoWifi,      // no saved credentials at all
    WifiFailed,  // association timed out
    FetchFailed, // HTTP error, timeout or short body
    BadImage     // downloaded, but not a usable panel-sized BMP
  };

  /** One complete wake-connect-fetch-display cycle. Never throws, never hangs. */
  Outcome refresh();

  /** dashboardUrl with the theme override appended, if one is set. */
  std::string buildUrl() const;

  /** Advance the persisted theme override: Auto -> Light -> Night -> Auto. */
  void cycleTheme();

  /** Wait for NetService's link. */
  bool connectWifi();

  /** Download to the temp path. Returns false on any HTTP-layer failure. */
  bool download(const std::string& url);

  /** Validate the temp file and, if good, promote it over the live copy. */
  Outcome commitDownload();

  /** Paint /.crosspoint/dashboard.bmp. Returns false if it will not parse. */
  bool displayImage();

  void showStatus(const char* message);

  /** Progress banner while the panel is still showing the previous frame. */
  void showBanner(const char* message);

  /** Label for the theme button: the mode currently in force. */
  const char* themeLabel() const;

  static const char* outcomeName(Outcome outcome);

  // What render() should paint. Everything reaching the panel has to go through
  // render(), because the render task holds the render lock and will re-push
  // whatever the framebuffer contains whenever it is notified. Painting from
  // the main task instead meant a stale progress banner could land on top of an
  // already-drawn dashboard, which looked exactly like a hang at "Fetching".
  enum class Phase : uint8_t {
    Banner,  // popup over whatever the panel already shows
    Image,   // the downloaded bitmap
    Status   // full-screen message
  };

  Phase phase = Phase::Banner;
  const char* message = nullptr;

  bool autoSleep = true;
  bool busy = false;
  // Set once the first refresh has run, so loop() can sleep or idle afterwards.
  bool refreshed = false;
  Outcome lastOutcome = Outcome::FetchFailed;
};
