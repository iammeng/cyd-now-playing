# Spotify CYD — User Manual

🇹🇭 [อ่านภาษาไทย](MANUAL.th.md) · for installation see the [Setup guide](SETUP.md)

Everything about *using* the display day-to-day. Current as of firmware
**v1.1.1**.

---

## 1. The screens

| Screen | When | What you see |
|---|---|---|
| **Boot** | power-on | logo + firmware version (the only place the version is shown) + WiFi status |
| **Now Playing** | something is playing/paused | art, title, artist, album, controls, progress |
| **Clock** | connected, nothing playing | current time + Spotify logo |
| **Offline** | can't reach the server | the server address it tried, and a hint |
| **WiFi setup** | no known WiFi | instructions for the setup portal (§6) |

**Status dot** (top-right corner, 3 px):
- 🔴 red — WiFi is down (the board will keep retrying)
- 🟡 amber — WiFi is fine but the server isn't answering
- invisible — everything is OK (the dot blends into the background)

---

## 2. Touch controls (Now Playing screen)

```
┌──────────────────────────────────────────┐
│ ┌────────┐   Title (up to 2 lines)       │
│ │ album  │   Artist                      │   ← no touch action (reserved)
│ │  art   │   Album                       │
│ └────────┘                               │
├──────────────────────────────────────────┤
│   🔀      ⏮       ⏯       ⏭       🔁     │   ← control row: 5 tap zones
├──────────────────────────────────────────┤
│ 0:42  ━━━━━●─────────────────────  3:35  │   ← tap anywhere on the bar = seek
└──────────────────────────────────────────┘
        ↕ swipe up/down anywhere = volume
```

| Gesture | Action |
|---|---|
| Tap **shuffle** (bottom-left) | toggle shuffle — icon lights up in the album color when on |
| Tap **previous** | previous track |
| Tap **play/pause** (center circle) | play / pause |
| Tap **next** | next track |
| Tap **repeat** (bottom-right) | cycle repeat: off → whole list → single track (shows "1") |
| Tap **the progress bar** | jump to that position in the track |
| **Swipe up** (anywhere) | volume +10% |
| **Swipe down** (anywhere) | volume −10% |

- A white ring flashes around a control to confirm the tap.
- During a volume change, a **white volume bar** replaces the progress bar
  for about 1.5 s, with the percentage on the right.
- On the **clock screen** the controls are simpler: tap the left / middle /
  right third of the screen for previous / play-pause / next.

### Requirements & quirks

- Playback commands need **Spotify Premium** and an *active device* (some
  app somewhere must already be playing or paused). If nothing happens,
  start playback in any Spotify app first.
- **Volume** cannot be set on some playback devices — notably an iPhone as
  the player. Desktop apps and speakers accept it.
- After a command the screen refreshes within ~1 second with the confirmed
  state from Spotify.

---

## 3. Screen brightness

The backlight follows the room's light automatically using the CYD's onboard
light sensor: bright room → full brightness, dark room → dimmed.

- **Touching a dimmed screen** wakes it to full brightness for **5 minutes**.
- The *first* touch while dimmed **only wakes the screen** — it is never
  treated as a button press, so you can't skip a track by accident in the
  dark. Subsequent touches work normally (and each one extends the 5
  minutes).
- If the screen never dims (or is always dim), the sensor thresholds may not
  match your room — see `GET /ldr` below and the tuning table in the
  [Setup guide §9](SETUP.md#9-code-layout-and-tuning-points).

---

## 4. Useful URLs (open in any browser on your LAN)

Replace `<board>` with the board's IP (or `spotify-cyd.local` if mDNS works
for you).

| URL | What it does |
|---|---|
| `http://<board>/screen` | live screenshot (raw dump — use `tools/capture.py` for a PNG) |
| `http://<board>/flip` | rotate the display 180° (for the TN panel's viewing angle) — reboots |
| `http://<board>/server?h=<ip>` | point the board at a different server — reboots |
| `http://<board>/server` | show which server it's using |
| `http://<board>/test` | show a 2-minute display test pattern |
| `http://<board>/ldr` | current light-sensor reading + backlight level |
| `http://<board>/touch` | last touch gesture (for calibration) |

Taking a proper screenshot from your computer:

```bash
python3 tools/capture.py <board-ip> screenshot.png
```

---

## 5. Updating the firmware

Over WiFi (OTA), no cable needed:

```bash
cd firmware
pio run -e cyd_ota -t upload --upload-port <board-ip>
```

The screen shows "OTA update..." during the flash, then reboots. Check the
new version number on the boot screen. If you run more than one ESP32 board
at home, **verify the IP belongs to this board first** (e.g.
`curl http://<ip>/ldr` — only this project answers it).

---

## 6. WiFi setup / moving to a new network

1. The board remembers WiFi credentials across reboots *and* reflashes.
2. If it can't connect (~1.5 min of retries), it opens an access point:
   - connect your phone to **`Spotify-CYD-Setup`**
   - open `192.168.4.1`
   - choose your WiFi, enter the password
   - optionally set the **Server IP** on the same page → Save
3. The board reboots and connects.

If nobody configures the portal within 3 minutes, the board reboots and
tries again on its own.

---

## 7. Quick problem guide

| What you see | Try |
|---|---|
| "เชื่อมต่อ server ไม่ได้" (can't reach server) screen | is the server machine on? `docker ps` — then check the IP on the screen matches the server |
| Clock screen although music is playing | the Spotify token may need a refresh — `docker logs spotify-cyd`; if it says *run auth.py*, re-run auth (Setup guide §5.4) |
| Buttons don't do anything | start playback in a Spotify app first (needs an active device + Premium) |
| Volume swipe ignored | your playback device rejects volume commands (iPhone does) — switch device |
| Art never appears | check the server logs (`docker logs -f spotify-cyd`) |
| Wrong colors when photographing the screen | camera moiré — not a real defect |
| Display washed out at an angle | it's a TN panel — face it toward you, or open `/flip` |

More depth: [Setup guide §8 — Troubleshooting](SETUP.md#8-troubleshooting-from-real-incidents).
