"""Server-side text rendering for the CYD (Phase 2).

The board's embedded Kanit subset covers Latin + Thai only. For everything
else (Japanese/Korean/Chinese, symbols) the server rasterizes text with
Pillow and streams a packed 4-bit grayscale strip that the board colorizes
and pushes to the panel. Also used for marquee strips and the tap-to-detail
screen, so long Thai/Latin titles benefit too.

Wire format (what GET /text returns):
  4-byte header: w_lo w_hi h_lo h_hi (two little-endian uint16)
  then rows of packed 4-bit pixels, stride = (w+1)//2 bytes per row,
  high nibble = left pixel, 0 = background .. 15 = full text color.
"""
import os
import threading

from PIL import Image, ImageDraw, ImageFont

try:
    from fontTools.ttLib import TTFont
    HAVE_FONTTOOLS = True
except ImportError:
    HAVE_FONTTOOLS = False

# Order = priority: first font whose cmap covers a codepoint wins.
# Debian paths come from fonts-noto-core / fonts-noto-cjk (see Dockerfile);
# the macOS entry is a dev-machine fallback so the endpoint works outside
# Docker too.
FONT_CANDIDATES = [
    ("/usr/share/fonts/truetype/noto/NotoSansThai-Regular.ttf", 0),
    ("/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", 0),
    ("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", 0),
    ("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", 0),
]

_fonts = []  # {"path", "index", "cmap": set[int], "sized": {px: ImageFont}}
_font_lock = threading.Lock()
_measure = ImageDraw.Draw(Image.new("L", (1, 1)))


def init_fonts():
    for path, idx in FONT_CANDIDATES:
        if not os.path.exists(path):
            continue
        cmap = set()
        if HAVE_FONTTOOLS:
            try:
                tt = TTFont(path, fontNumber=idx, lazy=True)
                cmap = set(tt.getBestCmap().keys())
                tt.close()
            except Exception as e:
                print(f"[text] cmap {path}: {e}")
        _fonts.append({"path": path, "index": idx, "cmap": cmap, "sized": {}})
    print(f"[text] fonts: {[f['path'] for f in _fonts] or 'NONE FOUND'}")


def _font_for(cp):
    for i, f in enumerate(_fonts):
        if f["cmap"] and cp in f["cmap"]:
            return i
    return 0


def _sized(fi, px):
    f = _fonts[fi]
    with _font_lock:
        if px not in f["sized"]:
            f["sized"][px] = ImageFont.truetype(f["path"], px, index=f["index"])
        return f["sized"][px]


def _char_w(ch, px):
    return _measure.textlength(ch, font=_sized(_font_for(ord(ch)), px))


def _segments(text):
    """Split into runs of consecutive chars served by the same physical font."""
    segs = []
    for ch in text:
        fi = _font_for(ord(ch))
        if segs and segs[-1][0] == fi:
            segs[-1][1] += ch
        else:
            segs.append([fi, ch])
    return segs


def render_line(text, px):
    """Render one line to a tight grayscale image (0 = bg, 255 = text)."""
    segs = _segments(text)
    fis = {fi for fi, _ in segs} or {0}
    asc = desc = 0
    for fi in fis:
        a, d = _sized(fi, px).getmetrics()
        asc, desc = max(asc, a), max(desc, d)
    widths = [max(0, round(_measure.textlength(run, font=_sized(fi, px))))
              for fi, run in segs]
    img = Image.new("L", (max(1, sum(widths)), asc + desc), 0)
    draw = ImageDraw.Draw(img)
    x = 0
    for (fi, run), w in zip(segs, widths):
        draw.text((x, asc), run, font=_sized(fi, px), fill=255, anchor="ls")
        x += w
    return img


def render_line_fit(text, px, maxw):
    """One line, ellipsis-truncated to maxw pixels (marquee width cap)."""
    img = render_line(text, px)
    if img.width <= maxw:
        return img
    ell_w = _char_w("…", px)
    total, keep = 0.0, 0
    for i, ch in enumerate(text):
        w = _char_w(ch, px)
        if total + w + ell_w > maxw:
            break
        total += w
        keep = i + 1
    img = render_line(text[:keep].rstrip() + "…", px)
    return img.crop((0, 0, maxw, img.height)) if img.width > maxw else img


def _wrap_lines(text, px, wrap_w, max_lines):
    lines = []
    cur = []  # [(char, width)]
    for ch in text.replace("\r", ""):
        if ch == "\n":
            lines.append("".join(c for c, _ in cur))
            cur = []
            continue
        w = _char_w(ch, px)
        if cur and sum(x for _, x in cur) + w > wrap_w:
            spaces = [i for i, (c, _) in enumerate(cur) if c == " "]
            if spaces and spaces[-1] > 0:  # break at the last space if any
                cut = spaces[-1]
                lines.append("".join(c for c, _ in cur[:cut]))
                cur = cur[cut + 1:]
            else:  # no space (Thai/CJK): break at the character
                lines.append("".join(c for c, _ in cur))
                cur = []
        if ch == " " and not cur:
            continue  # no leading spaces on a fresh line
        cur.append((ch, w))
    if cur:
        lines.append("".join(c for c, _ in cur))
    if not lines:
        lines = [""]
    if len(lines) > max_lines:
        lines = lines[:max_lines]
        lines[-1] += "…"
    return lines


def render_wrapped(text, px, wrap_w, max_lines=4):
    """Multi-line greedy wrap (space-aware, works without spaces too)."""
    imgs = [render_line(l, px) for l in _wrap_lines(text, px, wrap_w, max_lines)]
    gap = max(2, px // 8)
    w = min(wrap_w, max(i.width for i in imgs))
    h = sum(i.height for i in imgs) + gap * (len(imgs) - 1)
    out = Image.new("L", (max(1, w), h), 0)
    y = 0
    for im in imgs:
        out.paste(im.crop((0, 0, min(im.width, w), im.height)), (0, y))
        y += im.height + gap
    return out


def pack4(img):
    """Grayscale PIL image -> wire format (header + packed 4-bit rows)."""
    w, h = img.size
    pix = img.load()
    stride = (w + 1) // 2
    buf = bytearray(4 + stride * h)
    buf[0], buf[1] = w & 0xFF, w >> 8
    buf[2], buf[3] = h & 0xFF, h >> 8
    i = 4
    for y in range(h):
        for xb in range(stride):
            x = xb * 2
            hi = pix[x, y] >> 4
            lo = (pix[x + 1, y] >> 4) if x + 1 < w else 0
            buf[i] = (hi << 4) | lo
            i += 1
    return bytes(buf)
