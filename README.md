# Spotify CYD — จอแสดงเพลงที่กำลังเล่น

จอ Now Playing ของ Spotify บนบอร์ด CYD (ESP32-2432S028R) แสดงปกอัลบั้ม ชื่อเพลง/ศิลปิน (รองรับภาษาไทย) พร้อมปุ่มทัชควบคุมเพลง โทนสีหน้าจอเปลี่ยนตามสีปกอัลบั้มอัตโนมัติ

```
Spotify API ── Mac mini (server/) ── LAN ── CYD (firmware/)
```

Mac mini รัน server เล็ก ๆ ทำหน้าที่ OAuth + ย่อปกอัลบั้มเป็น RGB565 ให้บอร์ด ตัวบอร์ดจึงไม่ต้องยุ่งกับ TLS/JPEG เลย

> 📖 **คู่มือฉบับละเอียด** (สถาปัตยกรรม, network, auth, troubleshooting): [docs/SETUP.md](docs/SETUP.md)

## ติดตั้งครั้งแรก

### 1. สร้าง Spotify App

1. ไปที่ https://developer.spotify.com/dashboard → Create app
2. ตั้ง **Redirect URI** เป็น `http://127.0.0.1:8888/callback` (ต้องตรงเป๊ะ)
3. จด Client ID / Client Secret

### 2. ตั้งค่า server (บน Mac mini)

```bash
cd server
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
.venv/bin/python app.py          # รันครั้งแรกเพื่อสร้าง config.json แล้วหยุด
# แก้ config.json ใส่ client_id / client_secret
.venv/bin/python auth.py         # เปิด browser ล็อกอิน Spotify ครั้งเดียว
.venv/bin/python app.py          # เริ่ม server ที่พอร์ต 8080
```

ทดสอบ: เปิดเพลงใน Spotify แล้ว `curl http://127.0.0.1:8080/now`

ให้รันถาวรด้วย Docker (ต้องทำ `auth.py` บน host ก่อนเสมอ เพราะ container เปิด browser ไม่ได้):

```bash
docker compose up -d --build
# ดู log: docker logs -f spotify-cyd
```

Container ตั้ง `restart: unless-stopped` + Docker Desktop เปิด AutoStart ไว้ → กลับมาเองหลังรีบูต Mac (ทางเลือก: ใช้ launchd ผ่าน `server/com.kritsadas.spotify-cyd.plist` ถ้าไม่อยากพึ่ง Docker)

### 3. Flash บอร์ด

```bash
cd firmware
pio run -t upload                # สาย USB (พอร์ต /dev/cu.usbserial-230)
pio run -e cyd_ota -t upload     # รอบถัดไปผ่าน WiFi (spotify-cyd.local)
```

บูตครั้งแรกบอร์ดเปิด AP ชื่อ **Spotify-CYD-Setup** → ต่อแล้วเปิด `192.168.4.1` เลือก WiFi และตั้ง **Server IP** (ค่า default `192.168.1.195` = Mac mini)

## การใช้งาน

- **แตะซ้าย** = เพลงก่อนหน้า, **แตะกลาง** = เล่น/หยุด, **แตะขวา** = เพลงถัดไป
- จอหรี่อัตโนมัติ 23:00–07:00 — แตะจอเพื่อปลุกให้สว่างเต็ม 5 นาที (แตะแรกตอนหรี่ = ปลุกอย่างเดียว ไม่เปลี่ยนเพลง)
- ไม่มีเพลงเล่น → แสดงนาฬิกา

## Endpoints

| ที่ | endpoint | ทำอะไร |
|---|---|---|
| server | `GET /now` | JSON เพลงปัจจุบัน + `theme565` สีธีมจากปก |
| server | `GET /art/<id>?size=130` | ปกอัลบั้มเป็น raw RGB565 big-endian |
| server | `POST /playpause` `/next` `/previous` | สั่งเพลง |
| บอร์ด | `GET /screen` | screenshot จอ (ใช้ `tools/capture.py`) |
| บอร์ด | `GET /server?h=IP` | เปลี่ยน server IP แล้วรีบูต |
| บอร์ด | `GET /test` | หน้าทดสอบจอ 2 นาที (แถบ RGBW + gradient เทียบ drawPixel/pushImage) |
| บอร์ด | `GET /flip` | พลิกภาพ 180° (สลับทิศมุมมองที่ดีของจอ TN) แล้วรีบูต |
| บอร์ด | `GET /artbuf` | dump บัฟเฟอร์ปกอัลบั้มในเครื่อง (debug) |

หมายเหตุ: ถ้าใช้บอร์ด CYD รุ่นมี USB 2 ช่อง (จอ ST7789) ให้ build ด้วย `pio run -e cyd2usb -t upload` แทน

## โครงสร้าง

- `server/` — Flask + spotipy + Pillow (Python)
- `firmware/` — PlatformIO, TFT_eSPI + OpenFontRender (ฟอนต์ Kanit ฝังใน `src/kanit_font.h`)
- `tools/capture.py` — ดึง screenshot จริงจากจอ: `python3 tools/capture.py spotify-cyd.local`

ฟอนต์: [Kanit](https://github.com/cadsondemak/kanit) (OFL) subset เฉพาะ **Latin + ไทย** (~26KB) — ชื่อเพลงภาษาญี่ปุ่น/เกาหลี/จีนจะแสดงเป็นช่องว่าง (ไม่มี glyph ในฟอนต์)
