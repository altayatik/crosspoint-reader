#pragma once

#include <HalDisplay.h>

/**
 * Picks an e-ink waveform per repaint.
 *
 * Every app screen was asking for HALF_REFRESH on every paint, which is a clean
 * single-pass waveform taking well over a second and visibly wiping the panel.
 * Doing that to move one row down a list is why the device felt like it was
 * flashing constantly.
 *
 * FAST is a differential update: it only drives the pixels that changed, so it
 * is quick and quiet, but the residue builds up. So: a clean pass when a screen
 * is first painted, FAST for interaction after that, and another clean pass
 * every so often to sweep the ghosting away.
 *
 * markDirty() forces the next paint clean, for when the whole screen changes at
 * once -- a new month, a new article, a fresh dashboard image -- where a
 * differential update would leave the old content showing through.
 */
class RefreshPolicy {
 public:
  /** Repaints between clean passes. ~10 is where ghosting starts to show. */
  static constexpr int CLEAN_INTERVAL = 10;

  HalDisplay::RefreshMode next() {
    if (sinceClean < 0 || sinceClean >= CLEAN_INTERVAL) {
      sinceClean = 0;
      return HalDisplay::HALF_REFRESH;
    }
    ++sinceClean;
    return HalDisplay::FAST_REFRESH;
  }

  /** Force the next paint to use the clean waveform. */
  void markDirty() { sinceClean = -1; }

 private:
  // -1 means "never painted", which is also the state markDirty() returns to.
  int sinceClean = -1;
};
