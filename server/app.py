"""Companion server for the Spotify CYD display.

Endpoints (plain HTTP on the LAN):
  GET  /now            -> JSON now-playing state for the ESP32
  GET  /art/<art_id>   -> album art as raw RGB565 big-endian (?size=170)
  POST /playpause /next /previous /play /pause
  POST /seek?ms=N      -> jump to position
  POST /volume?delta=N (or ?set=N) -> adjust volume 0-100
  POST /shuffle        -> toggle shuffle
  POST /repeat         -> cycle off -> context -> track
  GET  /text?t=&px=    -> text rendered as packed 4-bit grayscale (CJK-capable);
                          optional &wrap=W&lines=N for multi-line, &maxw= cap
  GET  /devices        -> Spotify Connect device list
  POST /transfer?id=   -> move playback to another device
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
from text_render import init_fonts, render_line_fit, render_wrapped, pack4
from PIL import Image

POLL_MIN_INTERVAL = 1.5  # seconds between real Spotify API calls
DEFAULT_ART_SIZE = 170
ART_CACHE_MAX = 8
META_CACHE_MAX = 64  # art_urls / theme_cache entries (tiny, but bound them)

app = Flask(__name__)
cfg = load_config()
init_fonts()
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

art_urls = OrderedDict()      # art_id -> full Spotify CDN URL
art_cache = OrderedDict()     # (art_id, size) -> rgb565 bytes
raw_cache = OrderedDict()     # art_id -> original jpeg bytes (one CDN hit per art)
theme_cache = OrderedDict()   # art_id -> theme565 int
theme_pending = {}            # art_id -> Event set once the theme is computed
last_art_size = DEFAULT_ART_SIZE  # size the board actually asks /art for
text_cache = OrderedDict()    # (t, px, maxw, wrap, lines) -> packed strip
TEXT_CACHE_MAX = 128          # revisited tracks get their strips instantly


def cache_put(od, key, val, cap):
    od[key] = val
    od.move_to_end(key)
    while len(od) > cap:
        od.popitem(last=False)


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


def fetch_raw(art_id):
    """Original jpeg bytes for an art_id, downloading from the CDN at most once."""
    with lock:
        raw = raw_cache.get(art_id)
        url = art_urls.get(art_id)
    if raw is not None:
        return raw
    if not url:
        return None
    raw = urllib.request.urlopen(url, timeout=10).read()
    with lock:
        cache_put(raw_cache, art_id, raw, ART_CACHE_MAX)
    return raw


def fetch_art(art_id, size):
    """Resize/dither album art to rgb565; also fills theme_cache."""
    import io
    key = (art_id, size)
    with lock:
        if key in art_cache:
            art_cache.move_to_end(key)
            return art_cache[key]
    raw = fetch_raw(art_id)
    if raw is None:
        return None
    img = Image.open(io.BytesIO(raw)).convert("RGB")
    if art_id not in theme_cache:
        theme = rgb_to_565(*dominant_color(img))
        with lock:
            cache_put(theme_cache, art_id, theme, META_CACHE_MAX)
    img = img.resize((size, size), Image.LANCZOS)
    data = rgb565_dithered(img)
    with lock:
        cache_put(art_cache, key, data, ART_CACHE_MAX)
    return data


def prefetch_theme(art_id, evt):
    """Compute the theme color ASAP (so the /now that spotted the track can
    still include it), then warm the dithered art at the board's size."""
    try:
        raw = fetch_raw(art_id)
        if raw is not None:
            import io
            img = Image.open(io.BytesIO(raw)).convert("RGB")
            theme = rgb_to_565(*dominant_color(img))
            with lock:
                cache_put(theme_cache, art_id, theme, META_CACHE_MAX)
        evt.set()  # theme ready: release the waiting /now before the slow dither
        fetch_art(art_id, last_art_size)
    except Exception as e:
        print(f"[prefetch] {art_id}: {e}")
    finally:
        evt.set()
        with lock:
            theme_pending.pop(art_id, None)


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
        # without additional_types, podcast episodes come back as item=null
        pb = sp.current_playback(additional_types="episode")
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
    if item.get("type") == "episode":  # podcast: show name/publisher, episode art
        show = item.get("show") or {}
        artists = show.get("name", "")
        album = show.get("publisher", "")
        images = item.get("images") or show.get("images") or []
    else:
        artists = ", ".join(a["name"] for a in item.get("artists", []))
        album = item.get("album", {}).get("name", "")
        images = item.get("album", {}).get("images", [])
    # images are sorted large->small; index 1 is normally the 300px one
    art_url = images[1]["url"] if len(images) > 1 else (images[0]["url"] if images else "")
    art_id = art_url.rsplit("/", 1)[-1] if art_url else ""

    with lock:
        if art_id:
            cache_put(art_urls, art_id, art_url, META_CACHE_MAX)

    # new art: kick off the theme computation and give it a moment, so the
    # very first /now for a track usually already carries theme565 and the
    # board paints the tinted background once, not one poll later
    if art_id and art_id not in theme_cache:
        with lock:
            evt = theme_pending.get(art_id)
            if evt is None:
                evt = threading.Event()
                theme_pending[art_id] = evt
                threading.Thread(target=prefetch_theme, args=(art_id, evt),
                                 daemon=True).start()
        evt.wait(0.8)  # CDN fetch is typically 0.2-0.4s; board timeout is 4s

    with lock:
        now_state.update({
            "is_playing": bool(pb.get("is_playing")),
            "title": item.get("name", ""),
            "artists": artists,
            "album": album,
            "progress_ms": pb.get("progress_ms") or 0,
            "duration_ms": item.get("duration_ms") or 0,
            "art_id": art_id,
            "theme565": theme_cache.get(art_id, 0),
            "volume": -1 if vol is None else int(vol),
            "shuffle": bool(pb.get("shuffle_state")),
            "repeat": {"off": 0, "context": 1, "track": 2}.get(pb.get("repeat_state"), 0),
            "ok": True,
        })


@app.get("/now")
def get_now():
    refresh_now()
    with lock:
        return jsonify(now_state)


@app.get("/art/<art_id>")
def get_art(art_id):
    global last_art_size
    size = max(32, min(240, request.args.get("size", DEFAULT_ART_SIZE, type=int)))
    last_art_size = size  # prefetch warms this size so /art hits the cache
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


@app.get("/text")
def text_strip():
    """Rasterized text for the board: 4-byte w/h header + packed 4-bit rows."""
    t = request.args.get("t", "")
    px = max(8, min(48, request.args.get("px", 20, type=int)))
    maxw = max(32, min(1024, request.args.get("maxw", 512, type=int)))
    wrap = request.args.get("wrap", type=int)
    lines = max(1, min(6, request.args.get("lines", 4, type=int)))
    key = (t, px, maxw, wrap or 0, lines)
    with lock:
        if key in text_cache:
            text_cache.move_to_end(key)
            return Response(text_cache[key], mimetype="application/octet-stream")
    try:
        if wrap:
            img = render_wrapped(t, px, max(64, min(320, wrap)), lines)
        else:
            img = render_line_fit(t, px, maxw)
    except Exception as e:
        print(f"[text] {e}")
        return Response("render failed", status=500)
    data = pack4(img)
    with lock:
        cache_put(text_cache, key, data, TEXT_CACHE_MAX)
    return Response(data, mimetype="application/octet-stream")


@app.get("/devices")
def devices():
    if not have_token():
        return jsonify({"ok": False, "error": "not authenticated"}), 401
    try:
        devs = sp.devices().get("devices", [])
    except Exception as e:
        print(f"[devices] {e}")
        return jsonify({"ok": False}), 502
    return jsonify({"devices": [
        {"id": d.get("id") or "", "name": d.get("name", ""),
         "type": d.get("type", ""), "active": bool(d.get("is_active")),
         "volume": d.get("volume_percent")}
        for d in devs]})


@app.post("/transfer")
def transfer():
    dev = request.args.get("id", "")
    if not dev:
        return jsonify({"ok": False, "error": "id required"}), 400
    return spotify_command(lambda: sp.transfer_playback(dev, force_play=True))


@app.get("/health")
def health():
    return jsonify({"ok": True})


def advertise_mdns(port):
    """Advertise _spotify-cyd._tcp so the board finds us even after a DHCP
    change. Inside Docker on macOS multicast can't reach the LAN, so there
    the host advertises instead (com.kritsadas.spotify-cyd-mdns.plist runs
    dns-sd -R) and this is skipped."""
    import os
    if os.path.exists("/.dockerenv"):
        print("[mdns] in Docker: skipping - host's dns-sd LaunchAgent advertises")
        return
    try:
        import socket
        from zeroconf import Zeroconf, ServiceInfo
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))  # no traffic sent; just picks the LAN iface
        ip = s.getsockname()[0]
        s.close()
        info = ServiceInfo("_spotify-cyd._tcp.local.",
                           "server._spotify-cyd._tcp.local.",
                           addresses=[socket.inet_aton(ip)], port=port)
        Zeroconf().register_service(info)
        print(f"[mdns] advertising _spotify-cyd._tcp at {ip}:{port}")
    except Exception as e:
        print(f"[mdns] disabled: {e}")


if __name__ == "__main__":
    port = cfg.get("port", 8080)
    advertise_mdns(port)

    @app.after_request
    def _access_log(resp):  # waitress has no request log; keep werkzeug-style
        print(time.strftime("[%d/%b/%Y %H:%M:%S]"), request.remote_addr,
              request.method, request.path, resp.status_code)
        return resp

    from waitress import serve
    print(f"[serve] waitress on 0.0.0.0:{port}")
    # keep-alive capable, production-grade; the board reuses one connection
    serve(app, host="0.0.0.0", port=port, threads=8)
