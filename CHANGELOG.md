# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and version numbers follow [Semantic Versioning](https://semver.org/). The
version refers to the firmware (`FW_VERSION` in `firmware/src/main.cpp`, shown
on the boot screen); server changes ship together with the firmware version
that uses them.

## [1.1.1] - 2026-07-18

### Fixed

- White artifact left at the start of the progress bar: the progress knob
  overhangs the bar ends by 5 px, but the redraw only cleared above/below and
  to the right of the bar, so knob pixels drawn at 0:00 stayed on screen after
  the knob moved on. The knob trail is now cleared on all four sides (both in
  the progress bar and in the volume overlay, which shares the same spot).

## [1.1.0] - 2026-07-18

### Added

- **Shuffle and repeat controls** — the control row now has five touch zones
  (shuffle · previous · play/pause · next · repeat) with icons that light up
  in the album accent color when active; repeat cycles off → context → track.
- **Tap-to-seek** — tapping anywhere on the progress bar jumps to that
  position in the track (drawn optimistically before the server confirms).
- **Swipe volume** — swiping up/down on the now-playing screen changes volume
  by ±10%; a volume bar temporarily replaces the progress bar as feedback.
- **Ambient-light auto-brightness** — backlight now follows the CYD's onboard
  photoresistor (GPIO 34, smoothed with an EMA + hysteresis) instead of a
  fixed night schedule. Touching the screen while dimmed still wakes it to
  full brightness for 5 minutes, and the first touch only wakes (never fires
  a playback command).
- Server endpoints: `POST /seek?ms=`, `POST /volume?delta=|set=`,
  `POST /shuffle` (toggle), `POST /repeat` (cycle); `GET /now` now also
  reports `volume`, `shuffle`, and `repeat`.
- Board debug endpoints: `GET /ldr` (light-sensor reading + backlight duty)
  and `GET /touch` (last touch gesture, for remote calibration).

### Changed

- Touch input rewritten from press-triggered 3-zone (x-axis only) to
  gesture-based press/release tracking with both axes: taps dispatch by zone,
  vertical swipes control volume. The idle screen keeps the old
  thirds behavior (previous / play-pause / next).

## [1.0.0] - 2026-07-14

### Added

- Initial working release.
- **Server** (Flask + spotipy + Pillow, Docker on a Mac on the LAN): Spotify
  OAuth with one-time browser login (`auth.py`) to satisfy Spotify's
  loopback-only redirect rule (Nov 2025); polls current playback with
  caching; serves album art resized and converted to raw RGB565 big-endian
  with Floyd–Steinberg dithering; extracts a dominant "theme" color per
  album; playback commands (`/playpause` `/play` `/pause` `/next`
  `/previous`).
- **Firmware** (ESP32-2432S028R "CYD", PlatformIO): now-playing UI with album
  art, Thai text via OpenFontRender + embedded Kanit subset (26 KB), UI
  tinted from the album theme color, smooth locally-interpolated progress
  bar, clock screen when idle, offline screen with hints; touch controls
  (previous / play-pause / next); WiFiManager captive-portal setup with
  configurable server IP; ArduinoOTA; watchdog + stale-data reboot; night
  dimming on a fixed schedule (23:00–07:00) with touch-to-wake; debug
  endpoints `/screen` (live screenshot), `/test` (display test pattern),
  `/flip` (rotate 180°), `/server` (change server IP), `/artbuf` (art buffer
  dump).

Versions 1.0.0 and 1.1.0 predate the public repository (its history starts
at 1.1.1), so only 1.1.1 and later have tags.

[1.1.1]: https://github.com/iammeng/cyd-now-playing/releases/tag/v1.1.1
[1.1.0]: https://github.com/iammeng/cyd-now-playing/blob/main/CHANGELOG.md
[1.0.0]: https://github.com/iammeng/cyd-now-playing/blob/main/CHANGELOG.md
