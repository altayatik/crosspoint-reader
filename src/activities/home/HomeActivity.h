#pragma once
#include <functional>
#include <vector>

#include "./FileBrowserActivity.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

struct RecentBook;
struct Rect;

class HomeActivity final : public Activity {
  ButtonNavigator buttonNavigator;
  int selectorIndex = 0;
  bool recentsLoading = false;
  bool recentsLoaded = false;
  bool firstRenderDone = false;
  bool hasOpdsServers = false;
  bool coverRendered = false;      // Track if cover has been rendered once
  bool coverBufferStored = false;  // Track if cover buffer is stored
  // Home can be entered while Back is still held (e.g. leaving Settings with
  // Back): ignore that stale release until a fresh press is seen here.
  bool backPressSeen = false;
  uint8_t* coverBuffer = nullptr;  // HomeActivity's own buffer for cover image
  size_t coverBufferSize = 0;      // Bytes allocated to coverBuffer
  // Logical rect last passed to drawRecentBookCover. The cover snapshot only
  // needs to cover this region, not the entire framebuffer, so we cache the
  // tile instead of all 48 KB. Set in render() before the call.
  int coverRectX = 0;
  int coverRectY = 0;
  int coverRectW = 0;
  int coverRectH = 0;
  std::vector<RecentBook> recentBooks;
  // Index of the first menu row drawn. The menu is taller than the panel once
  // the app entries are added, so render() windows it around selectorIndex.
  // Owned by render() rather than loop() because only render() knows how many
  // rows actually fit.
  mutable int menuScroll = 0;
  const HomeMenuItem initialMenuItem;

  // The menu, in order. No file browser and no OPDS: this device is a
  // dashboard, and neither entry leads anywhere without the reader.
  static constexpr HomeMenuItem MENU_ORDER[] = {
      HomeMenuItem::FILE_TRANSFER, HomeMenuItem::SETTINGS_MENU, HomeMenuItem::DASHBOARD,
      HomeMenuItem::CALENDAR,      HomeMenuItem::WORLDCLOCK,    HomeMenuItem::TIMER,
      HomeMenuItem::WEATHER,       HomeMenuItem::NEWS,          HomeMenuItem::PET};
  static constexpr int MENU_COUNT = sizeof(MENU_ORDER) / sizeof(MENU_ORDER[0]);

  static int menuItemToIndex(HomeMenuItem item, bool) {
    for (int i = 0; i < MENU_COUNT; ++i) {
      if (MENU_ORDER[i] == item) return i;
    }
    return 0;
  }

  static HomeMenuItem indexToMenuItem(int idx, bool) {
    return (idx >= 0 && idx < MENU_COUNT) ? MENU_ORDER[idx] : HomeMenuItem::NONE;
  }
  void onSelectBook(const std::string& path);
  void onSettingsOpen();
  void onDashboardOpen();
  void onCalendarOpen();
  void onWorldClockOpen();
  void onTimerOpen();
  void onWeatherOpen();
  void onNewsOpen();
  void onPetOpen();
  void onFileTransferOpen();

  int getMenuItemCount() const;
  bool storeCoverBuffer();    // Store frame buffer for cover image
  bool restoreCoverBuffer();  // Restore frame buffer from stored cover
  void freeCoverBuffer();     // Free the stored cover buffer
  void loadRecentBooks(int maxBooks);
  void loadRecentCovers(int coverHeight);

 public:
  explicit HomeActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                        HomeMenuItem initialMenuItemValue = HomeMenuItem::NONE)
      : Activity("Home", renderer, mappedInput), initialMenuItem(initialMenuItemValue) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isHomeActivity() const override { return true; }
};
