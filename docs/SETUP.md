# Spotify CYD — Full Setup Guide

🇹🇭 [อ่านภาษาไทย](SETUP.th.md)

This document explains every part of the system: the architecture, how the
server and the board talk to each other, networking, the Spotify Developer
Dashboard setup, the auth mechanism, a from-scratch install, and
troubleshooting.

---

## 1. Architecture overview

The system has three parts:

```
┌─────────────────┐   HTTPS + OAuth    ┌──────────────────────┐    plain HTTP (LAN)   ┌─────────────────┐
│  Spotify Cloud  │ ←───────────────→  │  Server machine      │ ←──────────────────→  │  CYD board       │
│  Web API        │                    │  Docker: spotify-cyd │                       │  (firmware/)     │
│                 │                    │  Flask on port 8080  │                       │  ESP32-2432S028R │
└─────────────────┘                    └──────────────────────┘                       └─────────────────┘
   needs a token                         does the heavy lifting:                        stays lightweight:
   needs TLS                             - stores/refreshes tokens                      - polls JSON every 2 s
                                         - polls current playback                       - draws UI + reads touch
                                         - resizes art, converts RGB565                 - sends playback commands
                                         - computes theme color from art
```

**Why a server in the middle — three reasons:**

1. **Spotify's OAuth rules (enforced Nov 2025):** redirect URIs must be HTTPS
   or loopback (`http://127.0.0.1:...`) only → an ESP32 can no longer run the
   login flow on-device; a machine with a browser must do auth on its behalf.
2. **Limited board RAM (no PSRAM):** TLS to Spotify plus JPEG decoding on the
   board is heavy and complex. The server resizes art and converts it to
   **raw RGB565** so the board just draws bytes with zero decoding.
3. **Rate limiting:** the server caches API results and images, so the board
   can poll as often as it likes without touching Spotify quotas.

---

## 2. Server ↔ board data flow

### 2.1 Main loop (every 2 seconds)

```
board                                 server                              Spotify
  │  GET /now                           │                                    │
  │ ───────────────────────────────→    │  (if cache older than 1.5 s)       │
  │                                     │  GET /me/player ────────────────→  │
  │                                     │  ←──────────────── playback JSON   │
  │  ←─────────────── JSON:             │                                    │
  │  {title, artists, album,            │                                    │
  │   is_playing, progress_ms,          │                                    │
  │   duration_ms, art_id, theme565,    │                                    │
  │   volume, shuffle, repeat, ok}      │                                    │
```

- The board compares `art_id` with the loaded art; if it changed it fetches
  new art (§2.2).
- `theme565` = the art's dominant color as an RGB565 16-bit int — used for
  buttons, frames, the progress bar, and the background tint.
- `volume` (0–100, or -1 = unknown), `shuffle` (bool), `repeat` (0=off,
  1=context, 2=track) — drive the icon states and the volume overlay.
- Between polls the board interpolates `progress_ms` locally, so the progress
  bar advances smoothly every second without extra network traffic.
- On the first poll after a track change `theme565` may be 0 (the server
  computes it in a background thread); the next poll returns the real value.

### 2.2 Album art loading

```
board:  GET /art/<art_id>?size=130
server: downloads the 300px art from the Spotify CDN (once; caches last 8)
        → resizes to 130×130 (LANCZOS)
        → Floyd–Steinberg dithering (hides banding on the 65,536-color panel)
        → converts to raw RGB565 big-endian = 130×130×2 = 33,800 bytes exactly
board:  streams it into a RAM buffer → pushImage straight to the display
```

**Byte order (important — was once a bug):** the server sends big-endian,
which is *already the order SPI puts on the wire*, so the board must draw
with `setSwapBytes(false)`. Never enable swapping (unlike PROGMEM image
arrays stored as native values, which need `setSwapBytes(true)`). Getting it
wrong double-swaps the bytes and turns the art into rainbow noise.

### 2.3 Playback commands (from touch)

```
touch on screen → board sets pendingCmd → netTask POSTs to the server:
  5-zone control row → /shuffle | /previous | /playpause | /next | /repeat
  tap on progress bar → /seek?ms=<position>
  vertical swipe → /volume?delta=±10
→ server calls the Spotify API (with the token)
→ server waits 0.3 s for the state to settle, then refreshes
→ replies with fresh state JSON (same shape as /now) → board redraws
```

Note: these commands require **Spotify Premium** and an "active device"
(something currently playing). Otherwise Spotify returns 403/404, which the
server swallows (logs only). Setting the volume is rejected on some target
devices (e.g. an iPhone as the playback device).

### 2.4 Dual-core split on the board

```
Core 0 (netTask)                       Core 1 (loop)
- polls /now every 2 s                 - all drawing (single core only — TFT is not thread-safe)
- fetches /art on art change           - reads touch (press/release + swipe) → sets pendingCmd
- POSTs playback commands              - progress tick every 1 s + ambient-light backlight update
- updates state + dirty flags          - checks dirty flags → redraws only what changed
        └──── communicate via volatile flags + a mutex (stateMux) ────┘
```

---

## 3. Networking

### 3.1 Network map

| Machine | Address | Notes |
|---|---|---|
| Server | `192.168.1.195`, port **8080** | baked into the firmware as the default |
| CYD board | DHCP, port **80** | can change when the router reassigns — set a reservation |
| Board mDNS | `spotify-cyd.local` | flaky (see 3.3) |

Everything runs on the same LAN over **plain HTTP, no TLS** — acceptable for
a home network: the traffic is track names and cover art, no secrets (tokens
live only on the server and are never sent to the board).

### 3.2 How the board finds the server

Priority order:
1. The value stored in NVS (board flash), key `server`, namespace `scyd`
2. Otherwise → default `192.168.1.195` (`DEFAULT_SERVER` in
   `firmware/src/main.cpp`)

Two ways to change it:
- open `http://<board-ip>/server?h=<new-ip>` → saves and reboots immediately
- via the WiFiManager portal ("Server IP" field) during WiFi setup

**Give the server machine a DHCP reservation** on your router (pin its MAC to
a fixed IP); otherwise the board loses the server when the IP changes.

### 3.3 Finding the board (when its IP changes)

- Try `ping spotify-cyd.local` first (mDNS — sometimes works)
- Otherwise sweep for a board-specific endpoint:
  ```bash
  for i in $(seq 2 254); do (curl -s --max-time 1 "http://192.168.1.$i/ldr" | grep -q "^ldr=" && echo "board at 192.168.1.$i") & done; wait
  ```
- Or check ARP: `arp -a | grep -i "e4:65:b8"` (an Espressif OUI)
- **Careful if you have several ESP32 boards on the LAN** — do not trust a
  remembered IP; verify with a board-specific endpoint (`/ldr`, `/artbuf`)
  before OTA-flashing anything.
- A DHCP reservation for the board helps too — OTA then always targets the
  same IP.

### 3.4 How the board joins WiFi (first boot / new network)

The firmware uses WiFiManager:
1. On boot it tries the remembered WiFi (stored in NVS by the WiFi stack —
   survives reflashing).
2. If it can't connect within ~15 s × 5 retries → it opens an access point
   named **`Spotify-CYD-Setup`** and shows instructions on screen.
3. Join that AP with your phone → open `192.168.4.1` → pick your home WiFi +
   password (+ optionally change the Server IP on the same page) → Save.
4. If the portal sits unconfigured for 3 minutes → the board reboots and
   retries.

---

## 4. Spotify Developer Dashboard

### 4.1 Create the app (once)

1. Go to https://developer.spotify.com/dashboard → log in with your normal
   Spotify account → **Create app**
2. Fill the form:
   - **App name:** anything, **but it must not start with "Spot"** (Spotify's
     rule) — e.g. `cyd-now-playing`
   - **App description:** anything
   - **Website:** can be left empty
   - **Redirect URIs:** type `http://127.0.0.1:8888/callback` and **press
     Add** before saving (text left in the field without Add is not saved)
   - **Which API/SDKs:** tick **Web API** only
3. Open the app → **Settings** → copy the **Client ID**, then View client
   secret → copy the **Client Secret**

### 4.2 Redirect-URI rules (why 127.0.0.1)

Since Nov 2025 Spotify enforces for every app:
- ❌ `http://localhost:8888/callback` — the word "localhost" is banned
- ❌ `http://192.168.x.x/callback` — plain HTTP to arbitrary IPs is banned
  (this is what broke all the older login-on-the-ESP32 projects)
- ✅ `http://127.0.0.1:8888/callback` — only the numeric loopback IP is
  exempt
- ✅ `https://...` — needs a real certificate (overkill for this)

The value must match `server/config.json` (`redirect_uri`) **character for
character** — otherwise you get `INVALID_CLIENT: Invalid redirect URI`.

### 4.3 Development mode

New apps are in development mode: usable by the owner plus up to 25 invited
users. For a personal display nothing more is needed — just use your own
account.

---

## 5. Auth — mechanism and token lifecycle

### 5.1 First-time flow (must run on the server machine)

```
you run server/auth.py on the server machine
  1. spotipy starts a tiny listener on 127.0.0.1:8888 (local only)
  2. it opens the Spotify authorize page in your browser
     (with client_id + scopes + redirect_uri attached)
  3. you click "Agree" in the browser
  4. Spotify redirects the browser to http://127.0.0.1:8888/callback?code=XXXX
     → the browser loops back into the same machine → spotipy receives the code
  5. spotipy exchanges code + client_secret for access_token + refresh_token
  6. both are written to server/.spotify_token_cache
```

Key point: **Spotify never connects to your machine** — it's your own
browser that hits 127.0.0.1, so the loopback rule works behind any NAT. But
it also means **the browser and auth.py must be on the same machine** (don't
run it over ssh and open the browser elsewhere).

### 5.2 Requested scopes

| scope | used for |
|---|---|
| `user-read-currently-playing` | reading the current track |
| `user-read-playback-state` | play/pause state, progress, volume, shuffle, repeat, device |
| `user-modify-playback-state` | play/pause/next/previous/seek/volume/shuffle/repeat |

If you change the scopes later, delete `.spotify_token_cache` and run
`auth.py` again.

### 5.3 Token lifecycle

- The **access_token** lives 1 hour — `app.py` (via spotipy) refreshes it
  automatically using the refresh_token; nothing to do.
- The **refresh_token** never expires unless revoked (at
  https://www.spotify.com/account/apps/ or by rotating the client_secret).
- All tokens live in `server/.spotify_token_cache` (single file,
  permission 600) — **this file is the key to your Spotify account; never
  commit or share it.**
- The Docker container doesn't store tokens itself — it bind-mounts this file
  from the host (§6.3), so re-authing on the host is picked up instantly.

### 5.4 When you need to re-auth

- scopes / client_id / client_secret changed
- access revoked in your Spotify account
- the cache file was lost

Symptom: `docker logs spotify-cyd` shows `no cached token - run auth.py
first` and the board sits on the idle screen. Fix:
`server/.venv/bin/python server/auth.py` on the server machine — no
container restart needed.

---

## 6. Install from scratch (step-by-step)

### 6.1 Server side

```bash
cd spotify-cyd/server

# 1. create a virtualenv (used for auth and dev — production runs in Docker)
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt

# 2. create the config template (first run exits after creating the file)
.venv/bin/python app.py

# 3. edit config.json — fill in client_id / client_secret from the Dashboard (§4)
#    (see config.json.example)

# 4. one-time login (opens a browser)
.venv/bin/python auth.py
# "Logged in as: <name>" = success
```

### 6.2 Test the server before Docker

```bash
.venv/bin/python app.py &
curl http://127.0.0.1:8080/health          # {"ok": true}
# play something in Spotify, then:
curl http://127.0.0.1:8080/now             # should show the track
curl -X POST http://127.0.0.1:8080/next    # should skip (needs Premium)
kill %1
```

### 6.3 Run permanently with Docker

```bash
cd spotify-cyd
docker compose up -d --build
```

What `docker-compose.yml` does:
- builds the image from `server/Dockerfile` (python:3.12-slim + pip deps)
- maps port `8080:8080`
- **bind-mounts two files from the host:** `config.json` (credentials,
  read) and `.spotify_token_cache` (read+write — spotipy refreshes tokens
  and writes them back)
- `restart: unless-stopped` → the container recovers from crashes and
  reboots (enable Docker Desktop's start-at-login)

Maintenance commands:
```bash
docker logs -f spotify-cyd        # live logs
docker compose restart            # restart
docker compose up -d --build      # deploy new code after editing app.py
```

(Non-Docker alternative: a launchd plist is provided at
`server/com.kritsadas.spotify-cyd.plist` — copy it to
`~/Library/LaunchAgents` and `launchctl load` it.)

### 6.4 Flash the board

Requires the PlatformIO CLI (`pio`).

```bash
cd firmware

# first time: over USB
pio run -t upload

# afterwards: over WiFi (OTA) — no cable needed
pio run -e cyd_ota -t upload --upload-port <board-ip>
```

Environments in `platformio.ini`:
| env | when |
|---|---|
| `cyd` (default) | the common 1-USB-port CYD (ILI9341 panel) |
| `cyd_ota` | same as `cyd`, uploads over WiFi |
| `cyd2usb` / `cyd2usb_ota` | the 2-USB-port variant (ST7789 panel, needs BGR + inversion) |

### 6.5 First boot

1. The screen shows "Spotify CYD v_._._ ... connecting to WiFi" (the
   firmware version is shown here).
2. If WiFi was never configured → follow §3.4.
3. Once connected → if something is playing, the Now Playing screen appears
   within ~2–4 s; otherwise the clock screen.

---

## 7. Daily use

- **5-zone control row**: shuffle | previous | play/pause | next | repeat —
  shuffle/repeat icons light up in the theme color when active (track-repeat
  shows a "1")
- **Tap the progress bar** = seek to that position
- **Swipe up/down** = volume ±10% (a volume bar replaces the progress bar
  for ~1.6 s)
- Background + buttons + art frame tint themselves to the album color
- **Auto-dimming follows the room's light** (onboard LDR; tune in
  `main.cpp`: `LDR_BRIGHT_RAW`, `LDR_DARK_RAW`, `MIN_DUTY`)
- **Touching a dimmed screen** → full brightness for 5 minutes
  (`TOUCH_WAKE_MS`), then it dims again — the first touch while dimmed only
  wakes the screen and never fires a playback command (no accidental skips
  in the dark)
- Nothing playing → clock (tap thirds: previous / play-pause / next) |
  server down → an error screen with the IP it tried

### All HTTP endpoints

**Server (:8080)**
| endpoint | method | what it does |
|---|---|---|
| `/now` | GET | current-track JSON (shape in §2.1) |
| `/art/<art_id>?size=N` | GET | art as raw RGB565 big-endian, N×N (32–240, default 170) |
| `/playpause` `/play` `/pause` `/next` `/previous` | POST | playback commands; reply with fresh state |
| `/seek?ms=N` | POST | jump to position N ms |
| `/volume?delta=N` or `/volume?set=N` | POST | relative/absolute volume (clamped 0–100) |
| `/shuffle` | POST | toggle shuffle |
| `/repeat` | POST | cycle repeat off → context → track |
| `/health` | GET | liveness check |

**Board (:80)**
| endpoint | what it does |
|---|---|
| `/screen` | screenshot as an RGB565 dump — use with `tools/capture.py <ip> [out.png]` |
| `/server?h=IP` | change the server IP (saved to NVS) and reboot / without `h` = show current |
| `/flip` | rotate 180° (rotation 1↔3, saved to NVS) — flips the TN panel's good viewing side |
| `/test` | 2-minute test screen: RGBW bars + gradient A (drawPixel) vs B (pushImage) — A must equal B, else byte order is broken |
| `/ldr` | smoothed light-sensor reading + current backlight duty — for calibrating dimming |
| `/touch` | last touch gesture (start/end coords + dy) — for calibrating touch |
| `/artbuf` | dump the raw art buffer in board RAM (debug, compare against the server's bytes) |

---

## 8. Troubleshooting (from real incidents)

| Symptom | Cause | Fix |
|---|---|---|
| Art is rainbow noise but text is fine | double byte swap (`setSwapBytes(true)` on big-endian data) | must be `setSwapBytes(false)` in `drawArt()` — fixed permanently; verify with `/test` (A must equal B) |
| Whole screen washed out / bluish at an angle | TN panel's narrow viewing cone (hardware limit) | angle the display toward you, or `/flip` to swap the good viewing side |
| Photos of the screen show rainbow speckles (fine to the eye) | camera moiré + panel subpixels | not a real problem |
| Art shows color banding | 65,536-color panel | the server already dithers (`rgb565_dithered`) |
| Board shows "can't reach server" | server machine off / container dead / server IP changed | check `docker ps`, set a DHCP reservation, or `/server?h=<new-ip>` |
| `no cached token - run auth.py first` in logs | token cache lost/revoked | re-run `auth.py` on the host (§5.4) |
| Buttons don't change the music | no active device, or not Premium | start playback in a Spotify app first |
| Volume swipe does nothing | target device rejects volume commands (e.g. iPhone) | play on a device that accepts them (desktop/speaker) |
| Screen never dims / always dim | LDR thresholds don't match the room | read the real value at `/ldr`, adjust `LDR_BRIGHT_RAW` / `LDR_DARK_RAW` |
| OTA can't find the board | flaky mDNS / IP changed / another ESP32 took the IP | find the IP per §3.3, **verify it's the right board first**, then `--upload-port <ip>` |
| Art never loads (record placeholder stays) | server can't reach the CDN / incomplete buffer | check the board serial log (`[art] ...got X/33800`) and `docker logs` |
| Japanese/Korean/Chinese titles are blank | embedded font is a Latin+Thai subset | current behavior — the roadmap fixes this with server-side text rendering |

---

## 9. Code layout and tuning points

```
spotify-cyd/
├── server/
│   ├── app.py                # all of Flask: /now /art /commands + dithering + theme color
│   ├── auth.py               # first-time OAuth (host only)
│   ├── spotify_common.py     # shared config + SpotifyOAuth
│   ├── config.json           # client_id/secret (gitignored — see config.json.example)
│   ├── .spotify_token_cache  # tokens (gitignored, never share)
│   ├── Dockerfile
│   └── com.kritsadas.spotify-cyd.plist   # launchd alternative
├── docker-compose.yml
├── firmware/
│   ├── platformio.ini        # pins, SPI 40MHz, min_spiffs partition, envs
│   └── src/
│       ├── main.cpp          # the whole app: WiFi, netTask, UI, touch, endpoints
│       └── kanit_font.h      # Kanit Medium subset, Thai+Latin, 26 KB (PROGMEM)
├── tools/capture.py          # pull a real screenshot from the board
├── CHANGELOG.md
└── docs/                     # SETUP.md (this file) / SETUP.th.md (Thai)
```

Popular tuning points (all in `firmware/src/main.cpp` unless noted):

| To change | Where |
|---|---|
| theme background intensity | `themeBg()` — the constant `76` (0–255, higher = brighter) |
| art size | `ART_SIZE` (≤ 240; adjust the layout too) |
| poll rate | `POLL_INTERVAL_MS` (default 2000) |
| dimming light range / minimum | `LDR_BRIGHT_RAW` `LDR_DARK_RAW` `MIN_DUTY` (read real values from `/ldr`) |
| wake duration on touch while dimmed | `TOUCH_WAKE_MS` (default 300000 = 5 min) |
| volume step per swipe | the `±10` constant in `handleTouch()` |
| dominant-color algorithm | `server/app.py` → `dominant_color()` |
| font | build a new subset with `pyftsubset`, convert to a header replacing `kanit_font.h` |

### Design decisions (don't accidentally revert)

1. `setSwapBytes(false)` for server art (big-endian) — see §2.2
2. Display SPI at 40 MHz — the ILI9341's real spec limit (55 MHz was flaky)
3. Draw only from core 1 (`loop`) — netTask must never touch TFT/OFR
4. Thai rendered by OpenFontRender (FreeType) — vowels/tone marks rely on the
   font's zero-advance metrics, no GPOS, so tall-ascender consonants
   (ป ฝ ฟ) can look slightly off — accepted
5. `/screen` renders into an 8-bit sprite → screenshot colors are coarser
   than the real panel; use it for layout, not color checks
6. The progress knob overhangs the bar ends by ±5 px — any redraw must clear
   all four sides around the bar (the white-speck bug in v1.1.0)
