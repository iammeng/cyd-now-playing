# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and version numbers follow [Semantic Versioning](https://semver.org/). The
version refers to the firmware (`FW_VERSION` in `firmware/src/main.cpp`, shown
on the boot screen); server changes ship together with the firmware version
that uses them.

## [1.4.0] - 2026-07-19

(Version 1.3.0 was an internal step flashed the same day and never released;
its changes are included here.)

### Added

- **Podcast support** — the server now requests episode metadata
  (`additional_types="episode"`), so podcasts show the episode title, show
  name, publisher, and episode art instead of falling back to the clock
  screen while an episode is playing.
- **mDNS server discovery** — when the board can't reach its stored server
  IP it looks up the `_spotify-cyd._tcp` service every 30 s and saves the
  address it finds. Survives DHCP reassignment and makes first-time setup
  work without typing an IP. The service is advertised by the server itself
  via zeroconf (new dependency) when running directly on the host; under
  Docker on macOS multicast can't leave the container, so a tiny launchd
  agent (`server/com.kritsadas.spotify-cyd-mdns.plist`, runs the built-in
  `dns-sd -R`) advertises from the host instead.
- **Active WiFi recovery on the board** — if WiFi stays down the board now
  nudges a reconnect after 15 s, escalates to a full re-association after
  3 min, and reboots after 10 min. Previously a silent drop could leave the
  red-dot screen up for as long as an hour until the stale-data watchdog
  fired.
- Board endpoint `GET /wifi` — signal strength (RSSI), SSID, channel, and IP
  for placement and OTA-reliability debugging.

### Changed

- **Theme background appears immediately on track change** — the server now
  computes the cover's theme color before answering the `/now` that first
  reports a new track (waiting up to 0.8 s for the CDN fetch), so the board
  paints the tinted background on the first draw instead of one poll (~2 s)
  later. The cover is also downloaded from Spotify's CDN only once per track
  (raw bytes cached; previously twice — once for the theme, once for the
  board) and pre-dithered at the size the board uses, making `/art`
  effectively instant.
- **The board reuses one HTTP connection** (keep-alive) for polling and
  commands instead of a new TCP handshake every request — snappier controls
  and roughly half the airtime on the congested 2.4 GHz band.
- **Server switched from the Flask dev server to waitress** (new
  dependency) — production-grade, properly supports the keep-alive above.
  A werkzeug-style access log is kept, and the Docker image now sets
  `PYTHONUNBUFFERED` so logs reach `docker logs` immediately.
- The server caches rendered text strips (128-entry LRU), so revisiting a
  recently played track shows its text instantly.
- While the backlight is dimmed (dark room, user asleep) the board polls
  every 10 s instead of 2 s, cutting overnight WiFi traffic ~5x; a touch
  restores the normal rate immediately.

### Fixed

- Server: the album-art URL and theme-color caches grew without bound over
  long uptimes; both are now capped (newest 64 entries kept).

## [1.2.0] - 2026-07-19

### Added

- **CJK-capable text via server-side rendering** — the server rasterizes
  track/artist/album text with Noto Sans (Thai + Latin + CJK fallback chain)
  and streams packed 4-bit grayscale strips (`GET /text`); the board blends
  them over the album-themed background. Japanese/Korean/Chinese titles now
  display instead of rendering as blanks. Falls back to the embedded Kanit
  font if the server is older or unreachable.
- **Marquee scrolling** — title/artist/album lines wider than the text column
  scroll horizontally (with a hold at the start of each loop) instead of
  being ellipsis-truncated.
- **Spotify Connect device switching** — long-press the now-playing or clock
  screen to list available devices; tap one to move playback there (the
  active device is highlighted). Uses `GET /devices` + `POST /transfer?id=`.
- **Tap-to-detail** — tap the album art / track info area to show the full
  title, artists, and album wrapped across the whole screen (also
  server-rendered, so CJK works there too). Tap again to close.
- Board endpoint `GET /version` — firmware version without waiting for the
  boot screen (also handy for identifying the board before OTA).

### Fixed

- A failed OTA transfer could leave the board wedged (UI frozen on the OTA
  screen, HTTP dead) until a power cycle; OTA errors now reboot cleanly.

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

[1.4.0]: https://github.com/iammeng/cyd-now-playing/releases/tag/v1.4.0
[1.2.0]: https://github.com/iammeng/cyd-now-playing/releases/tag/v1.2.0
[1.1.1]: https://github.com/iammeng/cyd-now-playing/releases/tag/v1.1.1
[1.1.0]: https://github.com/iammeng/cyd-now-playing/blob/main/CHANGELOG.md
[1.0.0]: https://github.com/iammeng/cyd-now-playing/blob/main/CHANGELOG.md
