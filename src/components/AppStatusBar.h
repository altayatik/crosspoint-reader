#pragma once

class GfxRenderer;

/**
 * The bar across the top of every app screen: time and date on the left, Wi-Fi
 * and battery on the right.
 *
 * Deliberately not part of UITheme. The theme's drawHeader is built around a
 * reader's title/subtitle and is used by screens this build no longer shows;
 * this is a fixed-height strip that every app draws first and then lays out
 * beneath, which is the only way the apps agree on where their content starts.
 *
 * Everything is drawn from cached state (RTC poll, battery ADC, NetService
 * flag), so calling it costs no I/O beyond one RTC read every ten seconds.
 */
namespace AppStatusBar {

/** Height reserved at the top of the screen. Content must start below this. */
constexpr int HEIGHT = 34;

/**
 * Draw the bar. `title` is optional and centred; pass nullptr on screens whose
 * content already names itself.
 */
void draw(const GfxRenderer& renderer, const char* title = nullptr);

}  // namespace AppStatusBar
