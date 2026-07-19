#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include <esp_task_wdt.h>
#include <time.h>
#include "OpenFontRender.h"
#include "kanit_font.h"

#define FW_VERSION "1.4.0"

#define TOUCH_CS 33
#define TOUCH_IRQ 36
#define TOUCH_CLK 25
#define TOUCH_MISO 39
#define TOUCH_MOSI 32

#define TZ_OFFSET_SEC (7 * 3600)
#define POLL_INTERVAL_MS 2000UL
#define DIM_POLL_INTERVAL_MS 10000UL // gentler polling while the screen is dimmed
#define SERVER_PORT 8080
#define DEFAULT_SERVER "192.168.1.195"
#define STALE_REBOOT_MS 3600000UL

#define BL_PIN 21
#define BL_CH 7
#define DAY_DUTY 255
#define MIN_DUTY 25
#define LDR_PIN 34          // CYD onboard photoresistor: bright room = low raw
#define LDR_BRIGHT_RAW 300  // smoothed raw at/below this = full brightness
#define LDR_DARK_RAW 2800   // smoothed raw at/above this = MIN_DUTY
#define TOUCH_WAKE_MS 300000UL // touch while dimmed = full brightness for 5 min

#define ART_X 16
#define ART_Y 16
#define ART_SIZE 130
#define ART_BYTES (ART_SIZE * ART_SIZE * 2)
#define TXT_X 164
#define TXT_W 144

// server-rendered text strips (/text): packed 4-bit grayscale
#define STRIP_MAX_H 40      // sanity cap for single-line strip height
#define STRIP_MAX_BYTES 40000
#define TXT_TITLE_Y 22
#define TXT_ARTIST_Y 78
#define TXT_ALBUM_Y 108
#define MARQUEE_GAP 36      // blank px between end and wrapped-around start
#define MARQUEE_MS 40       // scroll tick
#define MARQUEE_STEP 2      // px per tick
#define MARQUEE_HOLD_MS 1800
#define DEV_MAX 5
#define LONGPRESS_MS 650

TFT_eSPI tft = TFT_eSPI();
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
TFT_eSPI *gfx = &tft;
OpenFontRender ofr;
WebServer httpSrv(80);
Preferences prefs;

uint16_t C_BG, C_TEXT, C_SUB, C_DIM, C_LINE, C_GREEN, C_RED;

String serverHost = DEFAULT_SERVER;
int screenRot = 1; // 1 or 3; TN panel has one good viewing direction - flip via /flip

char trTitle[160] = "", trArtists[160] = "", trAlbum[160] = "";
char trArtId[72] = "", artLoadedId[72] = "";
bool trPlaying = false, hasTrack = false, serverOk = false;
uint16_t themeColor = 0;
long progressMs = 0, durationMs = 0;
unsigned long progressAtMs = 0;
bool trShuffle = false;
int trRepeat = 0;  // 0=off 1=context 2=track
int trVolume = -1; // 0-100, -1 = unknown/no device
int pollFails = 0;
unsigned long lastDataMs = 0;

static uint8_t artBuf[ART_BYTES];
volatile bool artReady = false;

volatile bool trackDirty = false;
volatile bool playDirty = false;
volatile bool artDirty = false;
volatile bool stateDirty = false;
volatile bool otaActive = false;
volatile int pendingCmd = 0; // 1=playpause 2=next 3=prev 4=shuffle 5=repeat 6=seek
                             // 7=volume 8=device list 9=transfer 10=track detail
volatile long pendingArg = 0;
SemaphoreHandle_t stateMux;

// server-rendered text strips (packed 4-bit grayscale from /text)
struct TextStrip { uint8_t *data = nullptr; uint16_t w = 0, h = 0; };
TextStrip stTitle, stArtist, stAlbum; // now-playing lines (marquee-capable)
TextStrip dtStrip[3];                 // tap-to-detail wrapped title/artists/album
bool stripsOk = false;          // strips match the current track: draw them
bool textUnsupported = false;   // server has no /text (404) - stop asking
volatile bool stripsDirty = false;
volatile bool dtDirty = false;
char stripsFor[352] = "";       // title|artists|album the strips were fetched for

struct Marq { int off = 0; unsigned long holdUntil = 0; };
Marq mqTitle, mqArtist, mqAlbum;
unsigned long lastMarqMs = 0;

// Spotify Connect device list (long-press to open)
char devName[DEV_MAX][44];
char devId[DEV_MAX][48];
char devTypeStr[DEV_MAX][12];
bool devActive[DEV_MAX];
int devCount = 0;
volatile bool devDirty = false;
bool devShowing = false;
bool dtShowing = false;
unsigned long devUntil = 0, dtUntil = 0;

enum Mode { BOOT, PLAYING, IDLE, OFFLINE, DEVICES, DETAIL };
Mode mode = BOOT;
unsigned long lastTouchMs = 0, lastTickMs = 0;
unsigned long wakeUntil = 0;
bool nightDimmed = false;
int ldrRaw = -1, blDuty = DAY_DUTY;
unsigned long volOverlayUntil = 0;
void updateBacklight();

uint16_t accent() { return themeColor ? themeColor : C_GREEN; }

uint16_t C_BGNOW; // now-playing background: album theme darkened to ~20%

uint16_t themeBg(uint16_t a) {
  int r = ((a >> 11) & 0x1F) << 3;
  int g = ((a >> 5) & 0x3F) << 2;
  int b = (a & 0x1F) << 3;
  int mx = max(r, max(g, b));
  if (mx < 8) return C_BG;
  // normalize so the brightest channel hits ~76: theme hue always visible,
  // still dark enough behind white text
  r = r * 76 / mx;
  g = g * 76 / mx;
  b = b * 76 / mx;
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// ---------- text helpers (OpenFontRender, UTF-8 aware) ----------

int u8CharLen(const char *s) {
  uint8_t c = (uint8_t)*s;
  if (c < 0x80) return 1;
  if ((c >> 5) == 0x6) return 2;
  if ((c >> 4) == 0xE) return 3;
  if ((c >> 3) == 0x1E) return 4;
  return 1;
}

void ofrText(int x, int y, float size, uint16_t fg, uint16_t bg, const char *str) {
  ofr.setFontSize(size);
  ofr.setFontColor(fg, bg);
  ofr.setCursor(x, y);
  ofr.printf("%s", str);
}

int textWidth(float size, const char *str) {
  ofr.setFontSize(size);
  return ofr.getTextWidth("%s", str);
}

// copy src into out, truncated with an ellipsis so it fits maxW at font size
void fitText(const char *src, float size, int maxW, char *out, size_t outSz) {
  if (textWidth(size, src) <= maxW) {
    strlcpy(out, src, outSz);
    return;
  }
  char buf[192];
  size_t n = 0;
  const char *p = src;
  out[0] = 0;
  while (*p && n < sizeof(buf) - 8) {
    int cl = u8CharLen(p);
    memcpy(buf + n, p, cl);
    buf[n + cl] = 0;
    char probe[200];
    snprintf(probe, sizeof probe, "%s…", buf);
    if (textWidth(size, probe) > maxW) break;
    n += cl;
    p += cl;
    strlcpy(out, probe, outSz);
  }
  if (!out[0]) strlcpy(out, "…", outSz);
}

// split into two lines, breaking at a space when possible (Thai has none: break anywhere)
void splitTwoLines(const char *src, float size, int maxW,
                   char *l1, size_t s1, char *l2, size_t s2) {
  l1[0] = l2[0] = 0;
  if (textWidth(size, src) <= maxW) {
    strlcpy(l1, src, s1);
    return;
  }
  char buf[192];
  size_t n = 0, lastSpace = 0;
  const char *p = src;
  while (*p && n < sizeof(buf) - 5) {
    int cl = u8CharLen(p);
    memcpy(buf + n, p, cl);
    buf[n + cl] = 0;
    if (textWidth(size, buf) > maxW) break;
    n += cl;
    if (*p == ' ') lastSpace = n;
    p += cl;
  }
  size_t cut = (lastSpace > n / 2) ? lastSpace : n;
  memcpy(l1, buf, cut);
  l1[cut] = 0;
  const char *rest = src + cut;
  while (*rest == ' ') rest++;
  fitText(rest, size, maxW, l2, s2);
}

void fmtTime(long ms, char *out, size_t sz) {
  long s = ms / 1000;
  snprintf(out, sz, "%ld:%02ld", s / 60, s % 60);
}

// ---------- drawing ----------

void drawArtFrame() {
  gfx->drawRoundRect(ART_X - 2, ART_Y - 2, ART_SIZE + 4, ART_SIZE + 4, 6, accent());
  gfx->drawRoundRect(ART_X - 3, ART_Y - 3, ART_SIZE + 6, ART_SIZE + 6, 7, C_LINE);
}

void drawArtPlaceholder() {
  int cx = ART_X + ART_SIZE / 2, cy = ART_Y + ART_SIZE / 2;
  gfx->fillRect(ART_X, ART_Y, ART_SIZE, ART_SIZE, gfx->color565(28, 28, 32));
  gfx->fillCircle(cx, cy, 44, gfx->color565(18, 18, 22));
  gfx->drawCircle(cx, cy, 44, C_LINE);
  gfx->drawCircle(cx, cy, 30, C_LINE);
  gfx->fillCircle(cx, cy, 12, accent());
  gfx->fillCircle(cx, cy, 4, gfx->color565(18, 18, 22));
  drawArtFrame();
}

void drawArt() {
  if (artReady && !strcmp(trArtId, artLoadedId) && strlen(artLoadedId)) {
    if (gfx == &tft) {
      // artBuf bytes are already in SPI wire order (big-endian) - no swap
      gfx->pushImage(ART_X, ART_Y, ART_SIZE, ART_SIZE, (uint16_t *)artBuf);
    } else {
      // sprite path (/screen): 8-bit sprite pushImage ignores swapBytes
      for (int y = 0; y < ART_SIZE; y++)
        for (int x = 0; x < ART_SIZE; x++) {
          int i = (y * ART_SIZE + x) * 2;
          gfx->drawPixel(ART_X + x, ART_Y + y, (artBuf[i] << 8) | artBuf[i + 1]);
        }
    }
    drawArtFrame();
  } else {
    drawArtPlaceholder();
  }
}

// ---------- server-rendered text strips ----------

static uint16_t winBuf[TXT_W * STRIP_MAX_H]; // marquee window / detail row buffer

// 16-entry blend LUT between bg and fg for the 4-bit alpha strips
void buildStripLut(uint16_t fg, uint16_t bg, uint16_t *lut) {
  int fr = (fg >> 11) << 3, fg8 = ((fg >> 5) & 0x3F) << 2, fb = (fg & 0x1F) << 3;
  int br = (bg >> 11) << 3, bg8 = ((bg >> 5) & 0x3F) << 2, bb = (bg & 0x1F) << 3;
  for (int a = 0; a < 16; a++) {
    int r = (br * (15 - a) + fr * a) / 15;
    int g = (bg8 * (15 - a) + fg8 * a) / 15;
    int b = (bb * (15 - a) + fb * a) / 15;
    lut[a] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  }
}

// draw a window of a strip at (x,y); off scrolls through the virtual loop of
// width strip.w + MARQUEE_GAP (positions past the strip render as background)
void drawStripWindow(TextStrip &s, int x, int y, uint16_t fg, int off) {
  if (!s.data) return;
  int h = min((int)s.h, STRIP_MAX_H);
  int stride = (s.w + 1) / 2;
  int span = s.w + MARQUEE_GAP;
  int winW = min((int)s.w, TXT_W);
  uint16_t lut[16];
  buildStripLut(fg, C_BGNOW, lut);
  bool direct = (gfx == &tft);
  for (int yy = 0; yy < h; yy++) {
    const uint8_t *row = s.data + yy * stride;
    for (int xx = 0; xx < winW; xx++) {
      int sx = off + xx;
      if (sx >= span) sx -= span;
      uint16_t c;
      if (sx >= (int)s.w) {
        c = lut[0];
      } else {
        uint8_t b = row[sx >> 1];
        c = lut[(sx & 1) ? (b & 0x0F) : (b >> 4)];
      }
      if (direct) winBuf[yy * winW + xx] = __builtin_bswap16(c);
      else gfx->drawPixel(x + xx, y + yy, c); // sprite path (/screen)
    }
  }
  if (direct) gfx->pushImage(x, y, winW, h, winBuf);
}

void drawStripLine(TextStrip &s, int y, uint16_t fg, Marq &mq) {
  mq.off = 0;
  mq.holdUntil = millis() + MARQUEE_HOLD_MS;
  gfx->fillRect(TXT_X, y, TXT_W + 2, min((int)s.h, STRIP_MAX_H), C_BGNOW);
  drawStripWindow(s, TXT_X, y, fg, 0);
}

// full-width strip (tap-to-detail), row-by-row through winBuf
void drawStripFull(TextStrip &s, int x, int y, uint16_t fg) {
  if (!s.data) return;
  int stride = (s.w + 1) / 2;
  int w = min((int)s.w, 320 - x);
  uint16_t lut[16];
  buildStripLut(fg, C_BGNOW, lut);
  bool direct = (gfx == &tft);
  for (int yy = 0; yy < s.h && y + yy < 240; yy++) {
    const uint8_t *row = s.data + yy * stride;
    for (int xx = 0; xx < w; xx++) {
      uint8_t b = row[xx >> 1];
      uint16_t c = lut[(xx & 1) ? (b & 0x0F) : (b >> 4)];
      if (direct) winBuf[xx] = __builtin_bswap16(c);
      else gfx->drawPixel(x + xx, y + yy, c);
    }
    if (direct) gfx->pushImage(x, y + yy, w, 1, winBuf);
  }
}

void drawTrackText() {
  gfx->fillRect(TXT_X, 10, 320 - TXT_X, 138, C_BGNOW);
  if (stripsOk) { // server-rendered lines (CJK-capable, marquee when too wide)
    xSemaphoreTake(stateMux, portMAX_DELAY);
    drawStripLine(stTitle, TXT_TITLE_Y, C_TEXT, mqTitle);
    drawStripLine(stArtist, TXT_ARTIST_Y, C_SUB, mqArtist);
    drawStripLine(stAlbum, TXT_ALBUM_Y, C_DIM, mqAlbum);
    xSemaphoreGive(stateMux);
    return;
  }
  char l1[96], l2[96], fit[96];
  splitTwoLines(trTitle, 21, TXT_W, l1, sizeof l1, l2, sizeof l2);
  ofrText(TXT_X, 16, 21, C_TEXT, C_BGNOW, l1);
  if (l2[0]) ofrText(TXT_X, 43, 21, C_TEXT, C_BGNOW, l2);
  fitText(trArtists, 16, TXT_W, fit, sizeof fit);
  ofrText(TXT_X, 78, 16, C_SUB, C_BGNOW, fit);
  fitText(trAlbum, 13, TXT_W, fit, sizeof fit);
  ofrText(TXT_X, 104, 13, C_DIM, C_BGNOW, fit);
}

void drawShuffleIcon(int cx, int cy) {
  uint16_t c = trShuffle ? accent() : C_DIM;
  gfx->drawLine(cx - 12, cy + 7, cx + 7, cy - 7, c);
  gfx->drawLine(cx - 12, cy + 8, cx + 7, cy - 6, c);
  gfx->drawLine(cx - 12, cy - 7, cx + 7, cy + 7, c);
  gfx->drawLine(cx - 12, cy - 6, cx + 7, cy + 8, c);
  gfx->fillTriangle(cx + 7, cy - 11, cx + 7, cy - 3, cx + 13, cy - 7, c);
  gfx->fillTriangle(cx + 7, cy + 11, cx + 7, cy + 3, cx + 13, cy + 7, c);
}

void drawRepeatIcon(int cx, int cy) {
  uint16_t c = trRepeat ? accent() : C_DIM;
  gfx->drawCircle(cx, cy, 9, c);
  gfx->drawCircle(cx, cy, 10, c);
  gfx->fillRect(cx + 1, cy - 13, 9, 8, C_BGNOW); // gap for the arrowhead
  gfx->fillTriangle(cx + 2, cy - 14, cx + 2, cy - 5, cx + 10, cy - 9, c);
  if (trRepeat == 2) ofrText(cx - 3, cy - 7, 11, c, C_BGNOW, "1");
}

void drawControls() {
  const int cy = 181;
  gfx->fillRect(0, 152, 320, 58, C_BGNOW);
  drawShuffleIcon(32, cy);
  // previous
  gfx->fillTriangle(96, cy, 112, cy - 11, 112, cy + 11, C_SUB);
  gfx->fillRect(92, cy - 11, 4, 22, C_SUB);
  // play / pause circle
  gfx->fillCircle(160, cy, 25, accent());
  uint16_t ic = C_BGNOW;
  if (trPlaying) {
    gfx->fillRoundRect(151, cy - 10, 6, 20, 2, ic);
    gfx->fillRoundRect(163, cy - 10, 6, 20, 2, ic);
  } else {
    gfx->fillTriangle(154, cy - 11, 154, cy + 11, 172, cy, ic);
  }
  // next
  gfx->fillTriangle(224, cy, 208, cy - 11, 208, cy + 11, C_SUB);
  gfx->fillRect(224, cy - 11, 4, 22, C_SUB);
  drawRepeatIcon(288, cy);
}

void drawProgress() {
  const int bx = 58, bw = 204, by = 220, bh = 6;
  long shown = progressMs;
  if (trPlaying) shown += (long)(millis() - progressAtMs);
  if (durationMs > 0 && shown > durationMs) shown = durationMs;
  char buf[12];

  ofr.setFontSize(13);
  fmtTime(shown, buf, sizeof buf);
  gfx->fillRect(10, 212, 44, 20, C_BGNOW);
  ofrText(12, 214, 13, C_SUB, C_BGNOW, buf);

  fmtTime(durationMs, buf, sizeof buf);
  gfx->fillRect(266, 212, 46, 20, C_BGNOW);
  int w = textWidth(13, buf);
  ofrText(308 - w, 214, 13, C_SUB, C_BGNOW, buf);

  int fill = durationMs > 0 ? (int)((int64_t)bw * shown / durationMs) : 0;
  if (fill > bw) fill = bw;
  // knob overhangs the bar ends by 5px - clear its trail on all four sides
  gfx->fillRect(bx - 6, by - 4, bw + 12, 4, C_BGNOW); // above
  gfx->fillRect(bx - 6, by + bh, bw + 12, 4, C_BGNOW); // below
  gfx->fillRect(bx - 6, by, 6, bh, C_BGNOW);           // left of bar
  gfx->fillRect(bx + bw, by, 6, bh, C_BGNOW);          // right of bar
  gfx->fillRoundRect(bx, by, bw, bh, 3, C_LINE);
  if (fill > 2) gfx->fillRoundRect(bx, by, fill, bh, 3, accent());
  gfx->fillCircle(bx + fill, by + bh / 2, 5, C_TEXT);
}

// volume bar drawn in the progress bar's spot for ~1.6s after a swipe
void drawVolume() {
  const int bx = 58, bw = 204, by = 220, bh = 6;
  char buf[12];
  gfx->fillRect(10, 212, 44, 20, C_BGNOW);
  ofrText(12, 214, 13, C_SUB, C_BGNOW, "เสียง");
  gfx->fillRect(266, 212, 46, 20, C_BGNOW);
  int v = trVolume < 0 ? 0 : trVolume;
  snprintf(buf, sizeof buf, "%d%%", v);
  int w = textWidth(13, buf);
  ofrText(308 - w, 214, 13, C_SUB, C_BGNOW, buf);
  gfx->fillRect(bx - 6, by - 4, bw + 12, 4, C_BGNOW);
  gfx->fillRect(bx - 6, by + bh, bw + 12, 4, C_BGNOW);
  gfx->fillRect(bx - 6, by, 6, bh, C_BGNOW);
  gfx->fillRect(bx + bw, by, 6, bh, C_BGNOW);
  gfx->fillRoundRect(bx, by, bw, bh, 3, C_LINE);
  int fill = bw * v / 100;
  if (fill > 2) gfx->fillRoundRect(bx, by, fill, bh, 3, C_TEXT);
}

void drawStatusDot() {
  uint16_t c = (mode == PLAYING || mode == DETAIL) ? C_BGNOW : C_BG;
  if (WiFi.status() != WL_CONNECTED) c = C_RED;
  else if (!serverOk) c = gfx->color565(240, 180, 40);
  gfx->fillCircle(311, 9, 3, c);
}

void drawNowPlaying() {
  gfx->fillScreen(C_BGNOW);
  drawArt();
  drawTrackText();
  drawControls();
  drawProgress();
  drawStatusDot();
}

void drawIdle() {
  gfx->fillScreen(C_BG);
  int cx = 160, cy = 92;
  gfx->fillCircle(cx, cy, 34, accent());
  // three sound arcs, Spotify-ish
  uint16_t d = C_BG;
  gfx->fillRect(cx - 18, cy - 12, 36, 5, d);
  gfx->fillRect(cx - 15, cy - 1, 30, 5, d);
  gfx->fillRect(cx - 12, cy + 10, 24, 5, d);
  struct tm t;
  if (getLocalTime(&t, 10) && t.tm_year > 100) {
    char ts[8];
    strftime(ts, sizeof ts, "%H:%M", &t);
    int w = textWidth(36, ts);
    ofrText(160 - w / 2, 132, 36, C_TEXT, C_BG, ts);
  }
  const char *msg = "ไม่มีเพลงกำลังเล่น";
  int w = textWidth(16, msg);
  ofrText(160 - w / 2, 190, 16, C_DIM, C_BG, msg);
  drawStatusDot();
}

void drawOffline() {
  gfx->fillScreen(C_BG);
  const char *msg = "เชื่อมต่อ server ไม่ได้";
  int w = textWidth(20, msg);
  ofrText(160 - w / 2, 88, 20, C_TEXT, C_BG, msg);
  char buf[80];
  snprintf(buf, sizeof buf, "http://%s:%d", serverHost.c_str(), SERVER_PORT);
  w = textWidth(14, buf);
  ofrText(160 - w / 2, 126, 14, C_SUB, C_BG, buf);
  const char *hint = "เช็คว่า app.py ทำงานอยู่บน Mac";
  w = textWidth(14, hint);
  ofrText(160 - w / 2, 152, 14, C_DIM, C_BG, hint);
  drawStatusDot();
}

// Spotify Connect device list (rows tappable, active device highlighted)
void drawDevices() {
  gfx->fillScreen(C_BG);
  ofrText(16, 8, 19, C_TEXT, C_BG, "เล่นเพลงที่ไหน");
  if (devCount == 0) {
    ofrText(16, 104, 15, C_DIM, C_BG, "ไม่พบอุปกรณ์ — เปิดแอป Spotify ก่อน");
  }
  for (int i = 0; i < devCount; i++) {
    int y = 44 + i * 36;
    gfx->drawRoundRect(8, y, 304, 32, 8, devActive[i] ? accent() : C_LINE);
    if (devActive[i]) gfx->fillCircle(26, y + 16, 5, accent());
    else gfx->drawCircle(26, y + 16, 5, C_DIM);
    char fit[64];
    fitText(devName[i], 15, 195, fit, sizeof fit);
    ofrText(42, y + 6, 15, devActive[i] ? C_TEXT : C_SUB, C_BG, fit);
    int w = textWidth(11, devTypeStr[i]);
    ofrText(302 - w, y + 10, 11, C_DIM, C_BG, devTypeStr[i]);
  }
  drawStatusDot();
}

// tap-to-detail: full track/artist/album text, server-wrapped
void drawDetail() {
  gfx->fillScreen(C_BGNOW);
  xSemaphoreTake(stateMux, portMAX_DELAY);
  uint16_t cols[3] = {C_TEXT, C_SUB, C_DIM};
  int y = 16;
  for (int i = 0; i < 3; i++) {
    if (!dtStrip[i].data) continue;
    drawStripFull(dtStrip[i], 16, y, cols[i]);
    y += dtStrip[i].h + 10;
  }
  xSemaphoreGive(stateMux);
  drawStatusDot();
}

void freeDetail() {
  xSemaphoreTake(stateMux, portMAX_DELAY);
  for (int i = 0; i < 3; i++) {
    free(dtStrip[i].data);
    dtStrip[i].data = nullptr;
    dtStrip[i].w = dtStrip[i].h = 0;
  }
  xSemaphoreGive(stateMux);
}

void drawCurrent() {
  switch (mode) {
    case PLAYING: drawNowPlaying(); break;
    case IDLE: drawIdle(); break;
    case OFFLINE: drawOffline(); break;
    case DEVICES: drawDevices(); break;
    case DETAIL: drawDetail(); break;
    default: break;
  }
}

// ---------- network (core 0) ----------

// one persistent connection for the small/frequent requests (poll + commands):
// keep-alive skips a TCP handshake every 2s, which matters on weak signal
WiFiClient reqClient;
HTTPClient reqHttp;

// find the server via mDNS (_spotify-cyd._tcp, advertised by the host);
// blocks ~2s, so only called when polling already fails. The mDNS responder
// itself is already running courtesy of ArduinoOTA.begin().
bool mdnsFindServer() {
  int n = MDNS.queryService("spotify-cyd", "tcp");
  if (n <= 0) return false;
  String ip = MDNS.IP(0).toString();
  if (ip == "0.0.0.0") return false;
  if (ip != serverHost) {
    xSemaphoreTake(stateMux, portMAX_DELAY);
    serverHost = ip;
    xSemaphoreGive(stateMux);
    prefs.putString("server", ip);
    reqClient.stop(); // old keep-alive socket points at the old host
    Serial.printf("[mdns] server -> %s\n", ip.c_str());
  }
  return true;
}

// active WiFi recovery: setAutoReconnect alone can wedge silently (seen
// 2026-07-19 - red dot for an hour until the stale-data watchdog rebooted)
void wifiWatchdog() {
  static unsigned long downSince = 0, lastKick = 0;
  if (WiFi.status() == WL_CONNECTED) {
    downSince = 0;
    return;
  }
  unsigned long now = millis();
  if (!downSince) {
    downSince = now;
    return;
  }
  unsigned long down = now - downSince;
  if (down > 600000UL) { // 10 min without WiFi beats waiting for the 1h reboot
    Serial.println("[wifi] down 10 min - rebooting");
    delay(100);
    ESP.restart();
  }
  if (down < 15000UL || now - lastKick < 15000UL) return;
  lastKick = now;
  if (down > 180000UL) { // escalate to a full re-association
    Serial.println("[wifi] hard reconnect");
    WiFi.disconnect(false);
    vTaskDelay(pdMS_TO_TICKS(300));
    WiFi.begin(); // stored credentials
  } else {
    Serial.println("[wifi] reconnect");
    WiFi.reconnect();
  }
}

bool serverReq(bool post, const char *path, String &out) {
  if (WiFi.status() != WL_CONNECTED) return false;
  reqHttp.setReuse(true); // end() keeps the socket open for the next request
  reqHttp.setTimeout(4000);
  reqHttp.setConnectTimeout(3000);
  char url[128];
  snprintf(url, sizeof url, "http://%s:%d%s", serverHost.c_str(), SERVER_PORT, path);
  if (!reqHttp.begin(reqClient, url)) return false;
  int code = post ? reqHttp.POST("") : reqHttp.GET();
  bool ok = code == 200;
  if (ok) out = reqHttp.getString();
  reqHttp.end();
  if (code < 0) reqClient.stop(); // wedged keep-alive socket: reconnect next time
  if (!ok) Serial.printf("[http] %d %s\n", code, url);
  return ok;
}

bool fetchArt(const char *artId) {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(8000);
  http.setConnectTimeout(3000);
  char url[160];
  snprintf(url, sizeof url, "http://%s:%d/art/%s?size=%d",
           serverHost.c_str(), SERVER_PORT, artId, ART_SIZE);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) {
    http.end();
    Serial.printf("[art] http %d\n", code);
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  int got = 0;
  unsigned long t0 = millis();
  artReady = false;
  while (got < ART_BYTES && millis() - t0 < 10000) {
    int avail = stream->available();
    if (avail > 0) {
      got += stream->readBytes(artBuf + got, min(avail, ART_BYTES - got));
    } else if (!http.connected()) {
      break;
    } else {
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  http.end();
  Serial.printf("[art] %s got %d/%d heap=%u\n", artId, got, ART_BYTES, ESP.getFreeHeap());
  if (got != ART_BYTES) return false;
  xSemaphoreTake(stateMux, portMAX_DELAY);
  strlcpy(artLoadedId, artId, sizeof artLoadedId);
  artReady = true;
  xSemaphoreGive(stateMux);
  return true;
}

void applyNow(const String &body);

String urlEncode(const char *s) {
  static const char hex[] = "0123456789ABCDEF";
  String out;
  out.reserve(strlen(s) * 3);
  for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
    uint8_t c = *p;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

int readBody(HTTPClient &http, WiFiClient *stream, uint8_t *dst, int len,
             unsigned long tmo) {
  int got = 0;
  unsigned long t0 = millis();
  while (got < len && millis() - t0 < tmo) {
    int avail = stream->available();
    if (avail > 0) got += stream->readBytes(dst + got, min(avail, len - got));
    else if (!http.connected()) break;
    else vTaskDelay(pdMS_TO_TICKS(5));
  }
  return got;
}

// fetch a packed 4-bit text strip from /text; wrapW>0 = multi-line wrap
bool fetchStrip(const char *text, int px, int wrapW, int maxLines, TextStrip &out) {
  if (WiFi.status() != WL_CONNECTED || textUnsupported) return false;
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(6000);
  http.setConnectTimeout(3000);
  String url = String("http://") + serverHost + ":" + SERVER_PORT +
               "/text?px=" + px;
  if (wrapW > 0) url += String("&wrap=") + wrapW + "&lines=" + maxLines;
  url += "&t=" + urlEncode(text);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) {
    http.end();
    if (code == 404) textUnsupported = true; // old server without /text
    Serial.printf("[text] http %d\n", code);
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  uint8_t hdr[4];
  if (readBody(http, stream, hdr, 4, 4000) != 4) {
    http.end();
    return false;
  }
  int w = hdr[0] | (hdr[1] << 8), h = hdr[2] | (hdr[3] << 8);
  int size = ((w + 1) / 2) * h;
  if (w < 1 || w > 1024 || h < 1 || h > 240 || size > STRIP_MAX_BYTES) {
    http.end();
    return false;
  }
  uint8_t *buf = (uint8_t *)malloc(size);
  if (!buf) {
    http.end();
    return false;
  }
  int got = readBody(http, stream, buf, size, 8000);
  http.end();
  if (got != size) {
    free(buf);
    return false;
  }
  xSemaphoreTake(stateMux, portMAX_DELAY);
  free(out.data);
  out.data = buf;
  out.w = w;
  out.h = h;
  xSemaphoreGive(stateMux);
  return true;
}

void fetchDevices() {
  String body;
  if (!serverReq(false, "/devices", body)) return;
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  JsonArray arr = doc["devices"].as<JsonArray>();
  xSemaphoreTake(stateMux, portMAX_DELAY);
  int n = 0;
  for (JsonObject d : arr) {
    if (n >= DEV_MAX) break;
    strlcpy(devName[n], d["name"] | "?", sizeof devName[0]);
    strlcpy(devId[n], d["id"] | "", sizeof devId[0]);
    strlcpy(devTypeStr[n], d["type"] | "", sizeof devTypeStr[0]);
    devActive[n] = d["active"] | false;
    n++;
  }
  devCount = n;
  devDirty = true;
  xSemaphoreGive(stateMux);
}

void transferPlayback(int i) {
  if (i >= 0 && i < devCount && devId[i][0]) {
    char path[64];
    snprintf(path, sizeof path, "/transfer?id=%s", devId[i]);
    String body;
    if (serverReq(true, path, body)) applyNow(body);
  }
  devShowing = false; // loop falls back to PLAYING and redraws
  stateDirty = true;
}

void fetchDetail() {
  struct { const char *txt; int px; int lines; } req[3] = {
      {trTitle, 23, 4}, {trArtists, 16, 2}, {trAlbum, 13, 1}};
  bool ok = true;
  for (int i = 0; i < 3 && ok; i++)
    ok = fetchStrip(req[i].txt, req[i].px, 288, req[i].lines, dtStrip[i]);
  if (ok) dtDirty = true;
}

void applyNow(const String &body) {
  JsonDocument doc;
  if (deserializeJson(doc, body)) return;
  xSemaphoreTake(stateMux, portMAX_DELAY);
  bool wasPlaying = trPlaying;
  bool hadTrack = hasTrack;
  bool wasShuffle = trShuffle;
  int wasRepeat = trRepeat;
  char prevTitle[160];
  strlcpy(prevTitle, trTitle, sizeof prevTitle);
  uint16_t prevTheme = themeColor;

  strlcpy(trTitle, doc["title"] | "", sizeof trTitle);
  strlcpy(trArtists, doc["artists"] | "", sizeof trArtists);
  strlcpy(trAlbum, doc["album"] | "", sizeof trAlbum);
  strlcpy(trArtId, doc["art_id"] | "", sizeof trArtId);
  trPlaying = doc["is_playing"] | false;
  progressMs = doc["progress_ms"] | 0;
  durationMs = doc["duration_ms"] | 0;
  progressAtMs = millis();
  themeColor = (uint16_t)(doc["theme565"] | 0);
  C_BGNOW = themeColor ? themeBg(themeColor) : C_BG;
  trShuffle = doc["shuffle"] | false;
  trRepeat = doc["repeat"] | 0;
  trVolume = doc["volume"] | -1;
  hasTrack = strlen(trTitle) > 0;
  // stale strips belong to the previous track: fall back to local text
  // until netTask fetches fresh ones
  if (strcmp(prevTitle, trTitle)) stripsOk = false;

  if (hasTrack != hadTrack || strcmp(prevTitle, trTitle) || themeColor != prevTheme)
    trackDirty = true;
  else if (trPlaying != wasPlaying || trShuffle != wasShuffle || trRepeat != wasRepeat)
    playDirty = true;
  stateDirty = true;
  xSemaphoreGive(stateMux);
  lastDataMs = millis();
}

void netTask(void *arg) {
  unsigned long lastPoll = 0;
  for (;;) {
    if (!otaActive && WiFi.status() == WL_CONNECTED) {
      int cmd = pendingCmd;
      if (cmd) {
        long arg = pendingArg;
        pendingCmd = 0;
        if (cmd == 8) {
          fetchDevices();
        } else if (cmd == 9) {
          transferPlayback((int)arg);
        } else if (cmd == 10) {
          fetchDetail();
        } else {
          char path[48];
          switch (cmd) {
            case 1: strlcpy(path, "/playpause", sizeof path); break;
            case 2: strlcpy(path, "/next", sizeof path); break;
            case 3: strlcpy(path, "/previous", sizeof path); break;
            case 4: strlcpy(path, "/shuffle", sizeof path); break;
            case 5: strlcpy(path, "/repeat", sizeof path); break;
            case 6: snprintf(path, sizeof path, "/seek?ms=%ld", arg); break;
            default: snprintf(path, sizeof path, "/volume?delta=%ld", arg); break;
          }
          String body;
          if (serverReq(true, path, body)) applyNow(body);
        }
        lastPoll = millis();
      }
      unsigned long now = millis();
      // dimmed = user asleep: poll gently to save airtime on the busy 2.4G band
      unsigned long pollMs = nightDimmed ? DIM_POLL_INTERVAL_MS : POLL_INTERVAL_MS;
      if (now - lastPoll >= pollMs) {
        lastPoll = now;
        String body;
        if (serverReq(false, "/now", body)) {
          pollFails = 0;
          if (!serverOk) { serverOk = true; stateDirty = true; }
          applyNow(body);
        } else if (++pollFails >= 3 && serverOk) {
          serverOk = false;
          stateDirty = true;
        }
      }
      // server unreachable: ask mDNS where it went (DHCP may have moved it,
      // or the stored IP was never right on a fresh setup)
      static unsigned long lastMdnsMs = 0;
      if (pollFails >= 3 && millis() - lastMdnsMs >= 30000) {
        lastMdnsMs = millis();
        if (mdnsFindServer()) lastPoll = 0; // poll again right away
      }
      if (hasTrack && strlen(trArtId) && strcmp(trArtId, artLoadedId)) {
        char id[72];
        strlcpy(id, trArtId, sizeof id);
        if (fetchArt(id)) artDirty = true;
        else vTaskDelay(pdMS_TO_TICKS(2000));
      }
      // refresh the server-rendered text strips when the track text changes
      if (hasTrack && !textUnsupported) {
        char sig[352];
        snprintf(sig, sizeof sig, "%s|%s|%s", trTitle, trArtists, trAlbum);
        if (strcmp(sig, stripsFor) != 0) {
          strlcpy(stripsFor, sig, sizeof stripsFor);
          bool ok = fetchStrip(trTitle, 24, 0, 0, stTitle);
          if (ok) ok = fetchStrip(trArtists, 17, 0, 0, stArtist);
          if (ok) ok = fetchStrip(trAlbum, 14, 0, 0, stAlbum);
          xSemaphoreTake(stateMux, portMAX_DELAY);
          stripsOk = ok;
          stripsDirty = true;
          xSemaphoreGive(stateMux);
          if (!ok && !textUnsupported) {
            stripsFor[0] = 0; // transient failure: retry next round
            vTaskDelay(pdMS_TO_TICKS(2000));
          }
        }
      }
    }
    if (!otaActive) wifiWatchdog();
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ---------- touch ----------

bool tActive = false, tLongFired = false;
int tStartX = 0, tStartY = 0, tLastX = 0, tLastY = 0;
unsigned long tPressMs = 0;
char touchLog[96] = "none"; // last gesture, readable at /touch for calibration

void queueCmd(int cmd, long arg = 0) {
  if (pendingCmd) return;
  pendingArg = arg;
  pendingCmd = cmd;
}

void tapFeedback(int cx) {
  gfx->drawCircle(cx, 181, 28, C_TEXT);
}

void dispatchTap(int x, int y) {
  if (pendingCmd) return;
  if (mode == IDLE) { // no controls drawn: keep the old thirds
    if (x < 107) queueCmd(3);
    else if (x < 214) queueCmd(1);
    else queueCmd(2);
    return;
  }
  if (y >= 208) { // progress bar row: tap to seek
    if (durationMs > 0 && x >= 50 && x <= 270) {
      long ms = (long)((int64_t)durationMs * constrain(x - 58, 0, 204) / 204);
      progressMs = ms;
      progressAtMs = millis();
      queueCmd(6, ms);
      drawProgress();
    }
    return;
  }
  if (y >= 148) { // controls row: shuffle | prev | play | next | repeat
    if (x < 64) { queueCmd(4); tapFeedback(32); }
    else if (x < 128) { queueCmd(3); tapFeedback(104); }
    else if (x < 192) { queueCmd(1); tapFeedback(160); }
    else if (x < 256) { queueCmd(2); tapFeedback(216); }
    else { queueCmd(5); tapFeedback(288); }
    return;
  }
  // art / text area: full track details (server-rendered, CJK-capable)
  if (!textUnsupported) queueCmd(10);
}

void handleTouch() {
  unsigned long now = millis();
  if (touch.tirqTouched() && touch.touched()) {
    TS_Point p = touch.getPoint();
    int x = constrain(map(p.x, 200, 3700, 0, 320), 0, 319);
    int y = constrain(map(p.y, 240, 3800, 0, 240), 0, 239);
    if (!tActive) {
      tActive = true;
      tLongFired = false;
      tPressMs = now;
      tStartX = tLastX = x;
      tStartY = tLastY = y;
    } else {
      tLastX = x;
      tLastY = y;
    }
    // long-press (finger still down, barely moved) = device list
    if (!tLongFired && !nightDimmed && now - tPressMs >= LONGPRESS_MS &&
        abs(tLastX - tStartX) < 24 && abs(tLastY - tStartY) < 24 &&
        (mode == PLAYING || mode == IDLE)) {
      tLongFired = true;
      queueCmd(8);
    }
    return;
  }
  if (!tActive) return; // released: act once per gesture
  tActive = false;
  if (tLongFired) { // the long-press already acted; swallow the release
    tLongFired = false;
    lastTouchMs = now;
    wakeUntil = now + TOUCH_WAKE_MS;
    return;
  }
  if (now - lastTouchMs < 350) return;
  lastTouchMs = now;
  int dy = tLastY - tStartY;
  snprintf(touchLog, sizeof touchLog, "start=%d,%d end=%d,%d dy=%d mode=%d",
           tStartX, tStartY, tLastX, tLastY, dy, (int)mode);
  bool wasDimmed = nightDimmed;
  wakeUntil = now + TOUCH_WAKE_MS;
  if (wasDimmed) {
    // first touch while dimmed only wakes the screen - no accidental skips
    updateBacklight();
    return;
  }
  if (mode == DEVICES) { // tap a row = transfer; tap elsewhere = close
    int idx = (tStartY - 44) / 36;
    if (tStartY >= 44 && idx >= 0 && idx < devCount &&
        (tStartY - 44) % 36 < 32 && !pendingCmd) {
      gfx->drawRoundRect(6, 44 + idx * 36 - 2, 308, 36, 9, C_TEXT);
      queueCmd(9, idx);
    } else {
      devShowing = false;
    }
    return;
  }
  if (mode == DETAIL) { // any tap closes the detail screen
    dtShowing = false;
    return;
  }
  if (mode != PLAYING && mode != IDLE) return;
  if (mode == PLAYING && abs(dy) >= 45) { // vertical swipe = volume, up = louder
    long delta = dy < 0 ? 10 : -10;
    if (trVolume >= 0) trVolume = constrain(trVolume + (int)delta, 0, 100);
    volOverlayUntil = now + 1600;
    queueCmd(7, delta);
    drawVolume();
    return;
  }
  dispatchTap(tStartX, tStartY);
}

// ---------- misc ----------

void updateBacklight() {
  static float ema = -1;
  int raw = analogRead(LDR_PIN);
  ema = ema < 0 ? raw : ema * 0.7f + raw * 0.3f;
  ldrRaw = (int)ema;
  int duty = DAY_DUTY;
  if ((long)(wakeUntil - millis()) <= 0) {
    // bright room = low raw -> high duty; linear in between, clamped
    duty = DAY_DUTY - (int)((ema - LDR_BRIGHT_RAW) * (DAY_DUTY - MIN_DUTY)
                            / (LDR_DARK_RAW - LDR_BRIGHT_RAW));
    duty = constrain(duty, MIN_DUTY, DAY_DUTY);
  }
  // hysteresis so small LDR jitter doesn't pulse the backlight
  if (abs(duty - blDuty) >= 10 || duty == DAY_DUTY || duty == MIN_DUTY) {
    if (duty != blDuty) {
      blDuty = duty;
      ledcWrite(BL_CH, blDuty);
    }
  }
  nightDimmed = blDuty < 100;
}

void drawPortalScreen(WiFiManager *wm) {
  tft.fillScreen(C_BG);
  ofr.setDrawer(tft);
  ofrText(20, 30, 22, accent(), C_BG, "ต้องตั้งค่า WiFi");
  ofrText(20, 80, 15, C_TEXT, C_BG, "1. ต่อ WiFi ชื่อ Spotify-CYD-Setup");
  ofrText(20, 108, 15, C_TEXT, C_BG, "2. เปิด 192.168.4.1");
  ofrText(20, 136, 15, C_TEXT, C_BG, "3. เลือกเครือข่าย + ใส่รหัสผ่าน");
  ofrText(20, 164, 15, C_SUB, C_BG, "ตั้งค่า Server IP ได้ในหน้านี้ด้วย");
}

void freeStrips() {
  // drop the text strips (largest heap users) - netTask refetches them
  // automatically because stripsFor no longer matches the current track
  xSemaphoreTake(stateMux, portMAX_DELAY);
  TextStrip *all[3] = {&stTitle, &stArtist, &stAlbum};
  for (auto *s : all) {
    free(s->data);
    s->data = nullptr;
    s->w = s->h = 0;
  }
  stripsOk = false;
  stripsFor[0] = 0;
  xSemaphoreGive(stateMux);
}

void handleHttpScreen() {
  otaActive = true;
  TFT_eSprite spr(&tft);
  spr.setColorDepth(8);
  bool sprOk = spr.createSprite(320, 240) != nullptr;
  if (!sprOk) {
    // strip mallocs can fragment the heap below the 76.8KB the sprite
    // needs; sacrifice them for this capture (they come right back)
    freeStrips();
    sprOk = spr.createSprite(320, 240) != nullptr;
  }
  if (sprOk) {
    gfx = &spr;
    ofr.setDrawer(spr);
    drawCurrent();
    ofr.setDrawer(tft);
    gfx = &tft;
  }
  httpSrv.setContentLength(320UL * 240UL * 2UL);
  httpSrv.send(200, "application/octet-stream", "");
  static uint8_t row[640];
  for (int y = 0; y < 240; y++) {
    for (int x = 0; x < 320; x++) {
      uint16_t c = sprOk ? spr.readPixel(x, y) : 0;
      row[x * 2] = c >> 8;
      row[x * 2 + 1] = c & 0xFF;
    }
    httpSrv.sendContent((const char *)row, 640);
  }
  if (sprOk) spr.deleteSprite();
  otaActive = false;
}

unsigned long testUntil = 0;

uint16_t testGradient(int x) { // 0..239 rainbow sweep
  int r = x < 120 ? 255 - x * 2 : 0;
  int g = x < 120 ? x * 2 : 255 - (x - 120) * 2;
  int b = x < 120 ? 0 : (x - 120) * 2;
  return tft.color565(r, g, b);
}

void handleHttpTest() {
  tft.fillScreen(TFT_BLACK);
  ofr.setDrawer(tft);
  struct { uint16_t c; const char *n; } bars[4] = {
    {TFT_RED, "RED"}, {TFT_GREEN, "GREEN"}, {TFT_BLUE, "BLUE"}, {TFT_WHITE, "WHITE"}};
  for (int i = 0; i < 4; i++) {
    int x = i * 80;
    tft.fillRect(x + 4, 30, 72, 70, bars[i].c);
    ofrText(x + 8, 6, 15, TFT_WHITE, TFT_BLACK, bars[i].n);
  }
  // same gradient two ways: per-pixel (known good) vs pushImage (album-art path)
  ofrText(8, 108, 14, TFT_WHITE, TFT_BLACK, "A: drawPixel");
  for (int y = 0; y < 30; y++)
    for (int x = 0; x < 240; x++)
      tft.drawPixel(40 + x, 132 + y, testGradient(x));
  ofrText(8, 168, 14, TFT_WHITE, TFT_BLACK, "B: pushImage");
  static uint8_t rowbuf[240 * 2];
  for (int x = 0; x < 240; x++) {
    uint16_t c = testGradient(x);
    rowbuf[x * 2] = c >> 8; // big-endian, exactly like album art from the server
    rowbuf[x * 2 + 1] = c & 0xFF;
  }
  for (int y = 0; y < 30; y++)
    tft.pushImage(40, 192 + y, 240, 1, (uint16_t *)rowbuf);
  ofrText(8, 224, 13, tft.color565(170, 175, 185), TFT_BLACK,
          "A กับ B ต้องเหมือนกัน - สวัสดี");
  testUntil = millis() + 120000;
  httpSrv.send(200, "text/plain", "test pattern shown for 120s");
}

void handleHttpArtBuf() {
  httpSrv.setContentLength(ART_BYTES);
  httpSrv.send(200, "application/octet-stream", "");
  for (int i = 0; i < ART_BYTES; i += 6760)
    httpSrv.sendContent((const char *)(artBuf + i), 6760);
}

void handleHttpFlip() {
  screenRot = screenRot == 1 ? 3 : 1;
  prefs.putInt("rot", screenRot);
  httpSrv.send(200, "text/plain", "rotation=" + String(screenRot) + " rebooting");
  delay(300);
  ESP.restart();
}

void handleHttpServerCfg() {
  String h = httpSrv.arg("h");
  if (!h.length()) {
    httpSrv.send(200, "text/plain", "server=" + serverHost + "\nset with /server?h=IP");
    return;
  }
  prefs.putString("server", h);
  httpSrv.send(200, "text/plain", "server=" + h + " rebooting");
  delay(300);
  ESP.restart();
}

void setup() {
  setCpuFrequencyMhz(160);
  Serial.begin(460800);
  prefs.begin("scyd");
  serverHost = prefs.getString("server", DEFAULT_SERVER);
  screenRot = prefs.getInt("rot", 1);
  if (screenRot != 1 && screenRot != 3) screenRot = 1;
  tft.init();
  tft.setRotation(screenRot);

  C_BG = tft.color565(15, 15, 18);
  C_TEXT = tft.color565(245, 245, 245);
  C_SUB = tft.color565(170, 175, 185);
  C_DIM = tft.color565(110, 115, 125);
  C_LINE = tft.color565(48, 48, 54);
  C_GREEN = tft.color565(30, 215, 96);
  C_RED = tft.color565(239, 83, 80);
  C_BGNOW = C_BG;

  ledcSetup(BL_CH, 5000, 8);
  ledcAttachPin(BL_PIN, BL_CH);
  ledcWrite(BL_CH, DAY_DUTY);

  ofr.setSerial(Serial);
  ofr.setDrawer(tft);
  if (ofr.loadFont(KANIT_TTF, KANIT_TTF_LEN)) {
    Serial.println("font load failed");
  }

  tft.fillScreen(C_BG);
  ofrText(0, 0, 1, C_BG, C_BG, " "); // warm up freetype
  int w = textWidth(26, "Spotify CYD");
  ofrText(160 - w / 2, 88, 26, C_GREEN, C_BG, "Spotify CYD");
  w = textWidth(14, "v" FW_VERSION);
  ofrText(160 - w / 2, 128, 14, C_DIM, C_BG, "v" FW_VERSION);
  w = textWidth(15, "กำลังต่อ WiFi...");
  ofrText(160 - w / 2, 156, 15, C_SUB, C_BG, "กำลังต่อ WiFi...");
  Serial.printf("Spotify CYD v%s\n", FW_VERSION);

  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFiManager wm;
  WiFiManagerParameter pServer("server", "Server IP (Mac mini)", serverHost.c_str(), 40);
  wm.addParameter(&pServer);
  wm.setConnectTimeout(15);
  wm.setConnectRetries(5);
  wm.setConfigPortalTimeout(180);
  wm.setAPCallback(drawPortalScreen);
  if (!wm.autoConnect("Spotify-CYD-Setup")) {
    ESP.restart();
  }
  if (String(pServer.getValue()) != serverHost && strlen(pServer.getValue())) {
    serverHost = pServer.getValue();
    prefs.putString("server", serverHost);
  }
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  configTime(TZ_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");

  ArduinoOTA.setHostname("spotify-cyd");
  ArduinoOTA.onStart([]() {
    otaActive = true;
    tft.fillScreen(C_BG);
    ofrText(100, 110, 20, C_GREEN, C_BG, "OTA update...");
  });
  ArduinoOTA.onError([](ota_error_t err) {
    // a failed transfer can leave WiFi/HTTP wedged - reboot to a clean state
    Serial.printf("[ota] error %d - rebooting\n", (int)err);
    delay(200);
    ESP.restart();
  });
  ArduinoOTA.begin();

  httpSrv.on("/screen", handleHttpScreen);
  httpSrv.on("/artbuf", handleHttpArtBuf);
  httpSrv.on("/test", handleHttpTest);
  httpSrv.on("/flip", handleHttpFlip);
  httpSrv.on("/server", handleHttpServerCfg);
  httpSrv.on("/ldr", []() {
    char buf[48];
    snprintf(buf, sizeof buf, "ldr=%d duty=%d", ldrRaw, blDuty);
    httpSrv.send(200, "text/plain", buf);
  });
  httpSrv.on("/touch", []() { httpSrv.send(200, "text/plain", touchLog); });
  // signal quality for placement/OTA debugging: > -60 dBm good, < -75 weak
  httpSrv.on("/wifi", []() {
    char buf[96];
    snprintf(buf, sizeof buf, "rssi=%d dBm ssid=%s ch=%d ip=%s",
             (int)WiFi.RSSI(), WiFi.SSID().c_str(), (int)WiFi.channel(),
             WiFi.localIP().toString().c_str());
    httpSrv.send(200, "text/plain", buf);
  });
  // board identity + firmware version without waiting for the boot screen
  httpSrv.on("/version", []() {
    httpSrv.send(200, "text/plain", "spotify-cyd v" FW_VERSION);
  });
  httpSrv.begin();

  touchSpi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
  touch.begin(touchSpi);
  touch.setRotation(screenRot);

  stateMux = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(netTask, "net", 16384, NULL, 1, NULL, 0);

  esp_task_wdt_init(180, true);
  esp_task_wdt_add(NULL);
}

void loop() {
  esp_task_wdt_reset();
  unsigned long now = millis();
  ArduinoOTA.handle();
  if (otaActive) {
    delay(10);
    return;
  }
  httpSrv.handleClient();
  if (testUntil) {
    if (now < testUntil) {
      delay(20);
      return;
    }
    testUntil = 0;
    trackDirty = true;
  }
  handleTouch();

  xSemaphoreTake(stateMux, portMAX_DELAY);
  bool doTrack = trackDirty, doPlay = playDirty, doArt = artDirty,
       doStrips = stripsDirty;
  trackDirty = playDirty = artDirty = stripsDirty = false;
  xSemaphoreGive(stateMux);

  if (doTrack) dtShowing = false; // track changed: close the detail screen
  if (devDirty) { devDirty = false; devShowing = true; devUntil = now + 12000; }
  if (dtDirty) { dtDirty = false; dtShowing = true; dtUntil = now + 20000; }
  if (devShowing && (long)(now - devUntil) >= 0) devShowing = false;
  if (dtShowing && (long)(now - dtUntil) >= 0) dtShowing = false;

  Mode want = mode;
  if (!serverOk && pollFails >= 3) want = OFFLINE;
  else if (hasTrack) want = PLAYING;
  else if (serverOk) want = IDLE;
  if (devShowing) want = DEVICES;
  else if (dtShowing) want = DETAIL;

  Mode prevMode = mode;
  bool modeChange = want != mode;
  mode = want;
  if (modeChange && prevMode == DETAIL) freeDetail();

  if (mode == BOOT) {
    delay(20);
    return;
  }

  if (modeChange || doTrack) {
    drawCurrent();
  } else if (mode == PLAYING) {
    if (doArt) drawArt();
    if (doPlay) drawControls();
    if (doStrips) drawTrackText();
  }

  // marquee: scroll strips wider than the text column
  if (mode == PLAYING && stripsOk && now - lastMarqMs >= MARQUEE_MS) {
    lastMarqMs = now;
    struct { TextStrip *s; Marq *m; int y; uint16_t c; } rows[3] = {
        {&stTitle, &mqTitle, TXT_TITLE_Y, C_TEXT},
        {&stArtist, &mqArtist, TXT_ARTIST_Y, C_SUB},
        {&stAlbum, &mqAlbum, TXT_ALBUM_Y, C_DIM},
    };
    xSemaphoreTake(stateMux, portMAX_DELAY);
    for (auto &r : rows) {
      if (!r.s->data || r.s->w <= TXT_W) continue;
      if ((long)(now - r.m->holdUntil) < 0) continue;
      r.m->off += MARQUEE_STEP;
      if (r.m->off >= r.s->w + MARQUEE_GAP) {
        r.m->off = 0;
        r.m->holdUntil = now + MARQUEE_HOLD_MS;
      }
      drawStripWindow(*r.s, TXT_X, r.y, r.c, r.m->off);
    }
    xSemaphoreGive(stateMux);
  }

  if (volOverlayUntil && (long)(now - volOverlayUntil) >= 0) {
    volOverlayUntil = 0;
    if (mode == PLAYING) drawProgress();
  }

  if (now - lastTickMs >= 1000) {
    lastTickMs = now;
    if (mode == PLAYING && !volOverlayUntil) drawProgress();
    if (mode == IDLE) {
      static int lastMin = -1;
      struct tm t;
      if (getLocalTime(&t, 5) && t.tm_min != lastMin) {
        lastMin = t.tm_min;
        drawIdle();
      }
    }
    drawStatusDot();
    updateBacklight();
    // signed diff + fresh millis(): netTask can bump lastDataMs past the
    // 'now' snapshot taken at loop start, and unsigned underflow here used
    // to look like a huge staleness -> spurious reboot
    if (lastDataMs && (long)(millis() - lastDataMs) > (long)STALE_REBOOT_MS) {
      Serial.println("stale data - rebooting");
      delay(100);
      ESP.restart();
    }
  }

  delay(20);
}
