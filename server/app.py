"""Companion server for the Spotify CYD display.

Endpoints (plain HTTP on the LAN):
  GET  /now            -> JSON now-playing state for the ESP32
  GET  /art/<art_id>   -> album art as raw RGB565 big-endian (?size=170)
  POST /playpause /next /previous /play /pause
  POST /seek?ms=N      -> jump to position
  POST /volume?delta=N (or ?set=N) -> adjust volume 0-100
  POST /shuffle        -> toggle shuffle
  POST /repeat         -> cycle off -> context -> track
  GET  /health
"""
import colorsys
import threading
import time
import urllib.request
from collections import OrderedDict

import spotipy
from flask import Flask, Response, jsonify, request

from spotify_common import load_config, make_auth_manager
from PIL import Image

POLL_MIN_INTERVAL = 1.5  # seconds between real Spotify API calls
DEFAULT_ART_SIZE = 170
ART_CACHE_MAX = 8

app = Flask(__name__)
cfg = load_config()
auth_manager = make_auth_manager(cfg, open_browser=False)
sp = spotipy.Spotify(auth_manager=auth_manager)


def have_token():
    """True if a usable (or refreshable) token is cached — never prompts."""
    try:
        return auth_manager.validate_token(
            auth_manager.cache_handler.get_cached_token()) is not None
    except Exception:
        return False

lock = threading.Lock()
now_state = {"is_playing": False, "title": "", "artists": "", "album": "",
             "progress_ms": 0, "duration_ms": 0, "art_id": "", "theme565": 0,
             "volume": -1, "shuffle": False, "repeat": 0, "ok": False}
fetched_at = 0.0

art_urls = {}                 # art_id -> full Spotify CDN URL
art_cache = OrderedDict()     # (art_id, size) -> rgb565 bytes
theme_cache = {}              # art_id -> theme565 int
prefetching = set()


def rgb_to_565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def dominant_color(img):
    """Pick a vivid, display-friendly accent color from the album art."""
    small = img.convert("RGB").resize((64, 64))
    pal = small.quantize(colors=8, method=Image.Quantize.FASTOCTREE)
    palette = pal.getpalette()
    best, best_score = (30, 215, 96), -1.0  # fallback: Spotify green
    for count, idx in pal.getcolors():
        r, g, b = palette[idx * 3:idx * 3 + 3]
        h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
        # favor common + saturated colors, penalize near-black/near-white
        score = count * (0.15 + s) * (1.0 - abs(v - 0.55))
        if score > best_score:
            best_score, best = score, (r, g, b)
    # brighten dark picks so they read as an accent on the dark UI
    h, s, v = colorsys.rgb_to_hsv(*(c / 255 for c in best))
    v = max(v, 0.55)
    s = min(s, 0.85)
    r, g, b = (int(c * 255) for c in colorsys.hsv_to_rgb(h, s, v))
    return r, g, b


def rgb565_dithered(img):
    """RGB888 -> RGB565 big-endian with Floyd-Steinberg dithering (hides banding
    on the CYD's 65k-color panel)."""
    w, h = img.size
    pix = img.load()
    buf = bytearray(w * h * 2)
    cur = [[0.0, 0.0, 0.0] for _ in range(w)]
    nxt = [[0.0, 0.0, 0.0] for _ in range(w)]
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b = pix[x, y]
            r = min(255.0, max(0.0, r + cur[x][0]))
            g = min(255.0, max(0.0, g + cur[x][1]))
            b = min(255.0, max(0.0, b + cur[x][2]))
            R, G, B = int(r) >> 3, int(g) >> 2, int(b) >> 3
            # reconstructed 8-bit values the panel will actually show
            er = r - ((R << 3) | (R >> 2))
            eg = g - ((G << 2) | (G >> 4))
            eb = b - ((B << 3) | (B >> 2))
            if x + 1 < w:
                cur[x + 1][0] += er * 7 / 16
                cur[x + 1][1] += eg * 7 / 16
                cur[x + 1][2] += eb * 7 / 16
                nxt[x + 1][0] += er * 1 / 16
                nxt[x + 1][1] += eg * 1 / 16
                nxt[x + 1][2] += eb * 1 / 16
            if x > 0:
                nxt[x - 1][0] += er * 3 / 16
                nxt[x - 1][1] += eg * 3 / 16
                nxt[x - 1][2] += eb * 3 / 16
            nxt[x][0] += er * 5 / 16
            nxt[x][1] += eg * 5 / 16
            nxt[x][2] += eb * 5 / 16
            v = (R << 11) | (G << 5) | B
            buf[i] = v >> 8
            buf[i + 1] = v & 0xFF
            i += 2
        cur = nxt
        nxt = [[0.0, 0.0, 0.0] for _ in range(w)]
    return bytes(buf)


def fetch_art(art_id, size):
    """Download/resize album art; returns rgb565 bytes and fills theme_cache."""
    key = (art_id, size)
    with lock:
        if key in art_cache:
            art_cache.move_to_end(key)
            return art_cache[key]
        url = art_urls.get(art_id)
    if not url:
        return None
    raw = urllib.request.urlopen(url, timeout=10).read()
    import io
    img = Image.open(io.BytesIO(raw)).convert("RGB")
    if art_id not in theme_cache:
        theme_cache[art_id] = rgb_to_565(*dominant_color(img))
    img = img.resize((size, size), Image.LANCZOS)
    data = rgb565_dithered(img)
    with lock:
        art_cache[key] = data
        while len(art_cache) > ART_CACHE_MAX:
            art_cache.popitem(last=False)
    return data


def prefetch_theme(art_id):
    """Warm art + theme in the background so /now can report theme565 next poll."""
    try:
        fetch_art(art_id, DEFAULT_ART_SIZE)
    except Exception as e:
        print(f"[prefetch] {art_id}: {e}")
    finally:
        prefetching.discard(art_id)


def refresh_now(force=False):
    global fetched_at
    with lock:
        if not force and time.time() - fetched_at < POLL_MIN_INTERVAL:
            return
        fetched_at = time.time()
    if not have_token():
        print("[spotify] no cached token - run auth.py first")
        with lock:
            now_state["ok"] = False
        return
    try:
        pb = sp.current_playback()
    except Exception as e:
        print(f"[spotify] {e}")
        with lock:
            now_state["ok"] = False
        return

    if not pb or not pb.get("item"):
        with lock:
            now_state.update({"is_playing": False, "title": "", "artists": "",
                              "album": "", "progress_ms": 0, "duration_ms": 0,
                              "art_id": "", "theme565": 0,
                              "volume": -1, "shuffle": False, "repeat": 0, "ok": True})
        return

    item = pb["item"]
    vol = (pb.get("device") or {}).get("volume_percent")
    images = item.get("album", {}).get("images", [])
    # images are sorted large->small; index 1 is normally the 300px one
    art_url = images[1]["url"] if len(images) > 1 else (images[0]["url"] if images else "")
    art_id = art_url.rsplit("/", 1)[-1] if art_url else ""

    with lock:
        if art_id:
            art_urls[art_id] = art_url
        now_state.update({
            "is_playing": bool(pb.get("is_playing")),
            "title": item.get("name", ""),
            "artists": ", ".join(a["name"] for a in item.get("artists", [])),
            "album": item.get("album", {}).get("name", ""),
            "progress_ms": pb.get("progress_ms") or 0,
            "duration_ms": item.get("duration_ms") or 0,
            "art_id": art_id,
            "theme565": theme_cache.get(art_id, 0),
            "volume": -1 if vol is None else int(vol),
            "shuffle": bool(pb.get("shuffle_state")),
            "repeat": {"off": 0, "context": 1, "track": 2}.get(pb.get("repeat_state"), 0),
            "ok": True,
        })

    if art_id and art_id not in theme_cache and art_id not in prefetching:
        prefetching.add(art_id)
        threading.Thread(target=prefetch_theme, args=(art_id,), daemon=True).start()


@app.get("/now")
def get_now():
    refresh_now()
    with lock:
        return jsonify(now_state)


@app.get("/art/<art_id>")
def get_art(art_id):
    size = max(32, min(240, request.args.get("size", DEFAULT_ART_SIZE, type=int)))
    try:
        data = fetch_art(art_id, size)
    except Exception as e:
        print(f"[art] {art_id}: {e}")
        return Response("art fetch failed", status=502)
    if data is None:
        return Response("unknown art_id", status=404)
    return Response(data, mimetype="application/octet-stream",
                    headers={"X-Art-Size": str(size)})


def spotify_command(fn):
    if not have_token():
        return jsonify({"ok": False, "error": "not authenticated"}), 401
    try:
        fn()
    except spotipy.SpotifyException as e:
        # 403s for "already playing/paused" or restriction violations are harmless
        print(f"[cmd] {e.msg}")
    except Exception as e:
        print(f"[cmd] {e}")
        return jsonify({"ok": False}), 502
    time.sleep(0.3)  # Spotify needs a beat before playback state reflects the command
    refresh_now(force=True)
    with lock:
        return jsonify(now_state)


@app.post("/playpause")
def playpause():
    refresh_now(force=True)
    with lock:
        playing = now_state["is_playing"]
    return spotify_command(sp.pause_playback if playing else sp.start_playback)


@app.post("/play")
def play():
    return spotify_command(sp.start_playback)


@app.post("/pause")
def pause():
    return spotify_command(sp.pause_playback)


@app.post("/next")
def next_track():
    return spotify_command(sp.next_track)


@app.post("/previous")
def previous_track():
    return spotify_command(sp.previous_track)


@app.post("/seek")
def seek():
    ms = request.args.get("ms", type=int)
    if ms is None or ms < 0:
        return jsonify({"ok": False, "error": "ms required"}), 400
    return spotify_command(lambda: sp.seek_track(ms))


@app.post("/volume")
def volume():
    delta = request.args.get("delta", type=int)
    target = request.args.get("set", type=int)
    if target is None:
        if delta is None:
            return jsonify({"ok": False, "error": "delta or set required"}), 400
        refresh_now(force=True)
        with lock:
            cur = now_state["volume"]
        if cur < 0:
            return jsonify({"ok": False, "error": "no active device"}), 409
        target = cur + delta
    target = max(0, min(100, target))
    return spotify_command(lambda: sp.volume(target))


@app.post("/shuffle")
def shuffle():
    refresh_now(force=True)
    with lock:
        cur = now_state["shuffle"]
    return spotify_command(lambda: sp.shuffle(not cur))


@app.post("/repeat")
def repeat_cycle():
    refresh_now(force=True)
    with lock:
        cur = now_state["repeat"]
    return spotify_command(lambda: sp.repeat(["off", "context", "track"][(cur + 1) % 3]))


@app.get("/health")
def health():
    return jsonify({"ok": True})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=cfg.get("port", 8080), threaded=True)
