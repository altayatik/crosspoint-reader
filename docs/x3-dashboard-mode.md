# X3 dashboard mode

Turns an Xteink X3 into a dedicated wireless display for a server-rendered
1-bit BMP. The firmware connects, downloads, paints and sleeps; everything else
— layout, typography, data aggregation, theming — happens on the server.

Server half: `dashboard-data-api`, documented in
`DASHBOARD/docs/x3-dashboard-server.md`. Endpoint:
`https://dashboard-data-api.vercel.app/api/x3.bmp`.

> **Status: not yet built or run on hardware.** Written against CrossPoint
> 1.5.0 source. The `freeink-sdk` submodule was not available in the
> environment this was authored in, so nothing here has been through a
> compiler. Expect to fix build errors on the first `pio run`.

---

## 1. Cycle

```
wake (timer or power button)
  └─ SETTINGS.dashboardEnabled?
       └─ DashboardActivity
            ├─ connect WiFi        (saved credentials, last-used first)
            ├─ NTP sync            (opportunistic, only if never synced)
            ├─ GET dashboardUrl    → /.crosspoint/dashboard.tmp
            ├─ validate            (size, BMP headers)
            ├─ unchanged?          → skip the refresh
            ├─ promote tmp → /.crosspoint/dashboard.bmp
            ├─ drawBitmap + HALF_REFRESH
            ├─ WiFi off
            └─ deep sleep with timer  (or stay awake, in dev mode)
```

## 2. Files

### Added

| Path | Purpose |
|---|---|
| `src/activities/dashboard/DashboardActivity.{h,cpp}` | the whole mode |

### Modified

| Path | Change |
|---|---|
| `lib/hal/HalPowerManager.{h,cpp}` | new `startDeepSleepWithTimer()` |
| `lib/hal/HalGPIO.{h,cpp}` | new `WakeupReason::Timer` |
| `src/CrossPointSettings.{h,cpp}` | six settings, `DASHBOARD_REFRESH` enum, `getDashboardRefreshMinutes()` |
| `src/SettingsList.h` | six `SettingInfo` entries |
| `lib/I18n/translations/english.yaml` | 18 strings |
| `src/main.cpp` | boot routing into dashboard mode |

Everything is additive except three small edits. No existing behaviour changes
while `dashboardEnabled` is 0, which is the default.

## 3. The one real gap that had to be filled

CrossPoint had **no timer wake**. `HalPowerManager::startDeepSleep()` ends in
`freeink::PowerManager::deepSleepUntilPowerButton()`, and there was no
`esp_sleep_enable_timer_wakeup()` anywhere in the tree — the device only ever
woke because a human pressed something.

`startDeepSleepWithTimer(gpio, seconds)` arms the RTC timer and then calls
straight into the existing `startDeepSleep()`. Wake sources on ESP32-C3
accumulate rather than replace, so the power button stays armed and either
source wakes the device. `seconds == 0` degrades to plain `startDeepSleep()`,
and values are clamped to 24 hours.

If `esp_sleep_enable_timer_wakeup()` fails, the code logs and sleeps anyway.
Losing the timer means the dashboard stops refreshing until someone presses
power; staying awake instead would flatten the battery, which is strictly
worse.

`WakeupReason::Timer` is checked *before* the GPIO cases in
`getWakeupReason()`, because dashboard mode arms both sources and the cause
reported is whichever actually fired.

## 4. Settings

| Key | Type | Default | On-device |
|---|---|---|---|
| `dashboardEnabled` | toggle | `0` | System |
| `dashboardRefreshMinutes` | enum: 5 / 10 / 15 / 30 / 60 | `15` | System |
| `dashboardDeepSleep` | toggle | `1` | System |
| `dashboardUrl` | string(128) | the Vercel endpoint | hidden |
| `dashboardWifiTimeoutSeconds` | value 5–120 step 5 | `30` | hidden |
| `dashboardHttpTimeoutSeconds` | value 5–120 step 5 | `20` | hidden |

`CrossPointSettings::toJson`/`fromJson` iterate `getSettingsList()`, so adding
the `SettingInfo` entries gave persistence in `/.crosspoint/settings.json`
*and* the web settings API for free.

The three hidden entries are category-less: persisted and web-editable, but
absent from the on-device Settings screen. Nobody is dialling a 128-character
URL in on four buttons.

**`dashboardRefreshMinutes` is persisted by enum index, not minutes.** Any new
interval must be appended to `DASHBOARD_REFRESH` *and* to the `enumValues`
array in `SettingsList.h`, or existing saves get silently reinterpreted.

## 5. Boot routing, and the lockout that had to be avoided

The naive version — "if `dashboardEnabled`, show the dashboard and sleep" —
bricks the reader. Press power, the device boots, refreshes, sleeps again
before any button can register, and the library is unreachable short of
editing `settings.json` on the SD card.

So **auto-sleep is keyed on the wake source, not just the setting**:

| Wake | Behaviour |
|---|---|
| Timer | Unattended. Refresh, then straight back to sleep. |
| Power button | A human is holding it. Refresh, then **stay awake** — `Confirm` re-fetches, `Back` goes to the normal home screen. |

`dashboardDeepSleep = 0` forces the awake path for both, for development.

Recovery mode (UP + power) and the panic path are both checked before
dashboard routing, so neither can be shadowed by this feature.

## 6. Buttons

Verified against `MappedInputManager`, not assumed:

- **Confirm** — manual refresh.
- **Back** — leave dashboard mode for home.

Both are logical buttons, so they respect the user's front-button remap and the
live orientation transform. The power button is untouched
(`SETTINGS.shortPwrBtn` owns it), and so is **left side button + power**, which
is the SD-card recovery boot combination.

## 7. Failure handling

An e-ink panel holds its image at zero power, so "do nothing" is a good
fallback. Every failure path leaves the panel untouched, shuts the radio down
and sleeps until the next cycle — the user keeps looking at slightly stale data
instead of an error screen.

| Failure | Result |
|---|---|
| No saved Wi-Fi credentials | `NoWifi`, no display write |
| Association times out on every saved network | `WifiFailed`, no display write |
| HTTP error, timeout, or short body | `FetchFailed`, previous image retained |
| Bytes match the current image | `Unchanged`, **refresh skipped** |
| Not a parseable BMP, or an implausible size | `BadImage`, temp discarded, previous image retained |

Three layers of protection against a runaway cycle:

1. Per-network Wi-Fi timeout (`dashboardWifiTimeoutSeconds`).
2. HTTP deadline, enforced through `HttpDownloader`'s `cancelFlag` — it has no
   timeout parameter, so the progress callback flips the flag once the deadline
   passes.
3. A hard 90-second ceiling on the whole cycle, checked before the download
   starts.

### Download safety

Downloads land on `/.crosspoint/dashboard.tmp` and are only renamed over
`/.crosspoint/dashboard.bmp` after the size and BMP headers check out. A
half-written file can never become the displayed image, and the last good frame
survives a power loss mid-download.

Parsing before promoting matters more than it sounds: a captive portal or an
error page served with HTTP 200 would otherwise overwrite a perfectly good
dashboard with garbage.

### Unchanged-image detection

A byte-for-byte compare of temp against live. An e-ink update is the most
expensive and most visible thing this activity does, so not doing it is worth
the comparison — especially on a manual re-press.

The server also emits an `ETag`. Sending `If-None-Match` for a 304 would be
strictly better (zero bytes transferred as well as zero refresh), but
`HttpDownloader` has no request-header API, so that needs an upstream change.
Noted as future work rather than worked around.

## 8. E-ink refresh policy

`HALF_REFRESH`, matching `SleepActivity::renderBitmapSleepScreen()`. It is the
single-pass clean waveform the OEM firmware itself uses for its sleep image.

- `FAST_REFRESH` is differential and would accumulate artifacts over hundreds
  of unattended updates.
- `FULL_REFRESH` runs the multi-flash GC waveform and visibly blinks.

A periodic full refresh to clear accumulated ghosting is **not implemented
yet** — worth adding once there is real data on how the panel ages under this
duty cycle.

## 9. Power

Normal state is deep sleep. During a cycle the radio is up only for the
association plus the ~54 KB download, and `shutdownWifi()` runs before the
e-ink waveform rather than after — Wi-Fi is the largest current draw on this
device and there is no reason to hold it up while the panel refreshes.

A `HalPowerManager::Lock` is held across the refresh so TLS and SD writes run
at full clock.

## 10. Not done

- **Periodic full refresh** for ghosting (see §8).
- **`If-None-Match` / 304** (see §7).
- **Battery and Wi-Fi indicators.** The server does not know either. If wanted,
  the firmware should overlay a corner glyph after `drawBitmap()` and before
  `displayBuffer()`, rather than the server guessing.
- **A build.** See the status note at the top.
