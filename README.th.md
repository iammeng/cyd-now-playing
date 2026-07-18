# Spotify CYD — จอแสดงเพลงที่กำลังเล่น

🇬🇧 [English](README.md) · 📖 [คู่มือฉบับละเอียด](docs/SETUP.th.md) · 📝 [Changelog](CHANGELOG.md)

จอ Now Playing ของ Spotify บนบอร์ด CYD (ESP32-2432S028R) แสดงปกอัลบั้ม ชื่อเพลง/ศิลปิน (รองรับภาษาไทย) พร้อมปุ่มทัชควบคุมเพลงครบชุด โทนสีหน้าจอเปลี่ยนตามสีปกอัลบั้มอัตโนมัติ

```
Spotify API ── Mac mini (server/) ── LAN ── CYD (firmware/)
```

Mac mini รัน server เล็ก ๆ ทำหน้าที่ OAuth + ย่อปกอัลบั้มเป็น RGB565 ให้บอร์ด ตัวบอร์ดจึงไม่ต้องยุ่งกับ TLS/JPEG เลย

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
pio run -t upload                # สาย USB ครั้งแรก
pio run -e cyd_ota -t upload --upload-port <ip บอร์ด>   # รอบถัดไปผ่าน WiFi
```

บูตครั้งแรกบอร์ดเปิด AP ชื่อ **Spotify-CYD-Setup** → ต่อแล้วเปิด `192.168.4.1` เลือก WiFi และตั้ง **Server IP** (ค่า default `192.168.1.195` = Mac mini)

## การใช้งาน

- **แถวปุ่มล่าง**: สลับสุ่ม · เพลงก่อนหน้า · เล่น/หยุด · เพลงถัดไป · เล่นซ้ำ — ไอคอนสุ่ม/ซ้ำติดสีธีมอัลบั้มเมื่อเปิดอยู่
- **แตะแถบ progress** = กระโดดไปตำแหน่งนั้นของเพลง
- **ปัดขึ้น/ลง** ที่ไหนก็ได้ = เพิ่ม/ลดเสียงทีละ 10% (มีแถบ volume โชว์แทนแถบ progress ชั่วครู่)
- **แสงหน้าจอปรับอัตโนมัติ** ตามแสงในห้องด้วยเซ็นเซอร์แสงบนบอร์ด — แตะจอตอนหรี่เพื่อปลุกให้สว่างเต็ม 5 นาที (แตะแรกตอนหรี่ = ปลุกอย่างเดียว ไม่เปลี่ยนเพลง)
- ไม่มีเพลงเล่น → แสดงนาฬิกา (แตะซ้าย/กลาง/ขวา = ก่อนหน้า/เล่น-หยุด/ถัดไป)

หมายเหตุ: คำสั่งควบคุมเพลงต้องมี **Spotify Premium** และมี active device ส่วนการสั่ง volume บางอุปกรณ์ Spotify ไม่อนุญาต (เช่น iPhone เป็นตัวเล่น)

## Endpoints

| ที่ | endpoint | ทำอะไร |
|---|---|---|
| server | `GET /now` | JSON เพลงปัจจุบัน + `theme565` `volume` `shuffle` `repeat` |
| server | `GET /art/<id>?size=130` | ปกอัลบั้มเป็น raw RGB565 big-endian |
| server | `POST /playpause` `/next` `/previous` | สั่งเพลง |
| server | `POST /seek?ms=N` | กระโดดไปตำแหน่งในเพลง |
| server | `POST /volume?delta=N` (หรือ `?set=N`) | ปรับเสียง 0–100 |
| server | `POST /shuffle` / `POST /repeat` | สลับสุ่ม / วนโหมดเล่นซ้ำ |
| บอร์ด | `GET /screen` | screenshot จอ (ใช้ `tools/capture.py`) |
| บอร์ด | `GET /server?h=IP` | เปลี่ยน server IP แล้วรีบูต |
| บอร์ด | `GET /test` | หน้าทดสอบจอ 2 นาที |
| บอร์ด | `GET /flip` | พลิกภาพ 180° แล้วรีบูต |
| บอร์ด | `GET /ldr` | ค่าเซ็นเซอร์แสง + ระดับ backlight ปัจจุบัน |
| บอร์ด | `GET /touch` | ทัชครั้งล่าสุด (ไว้ calibrate) |
| บอร์ด | `GET /artbuf` | dump บัฟเฟอร์ปกอัลบั้มในเครื่อง (debug) |

หมายเหตุ: ถ้าใช้บอร์ด CYD รุ่นมี USB 2 ช่อง (จอ ST7789) ให้ build ด้วย `pio run -e cyd2usb -t upload` แทน

## โครงสร้าง

- `server/` — Flask + spotipy + Pillow (Python)
- `firmware/` — PlatformIO, TFT_eSPI + OpenFontRender (ฟอนต์ Kanit ฝังใน `src/kanit_font.h`)
- `tools/capture.py` — ดึง screenshot จริงจากจอ: `python3 tools/capture.py <ip บอร์ด>`

ฟอนต์: [Kanit](https://github.com/cadsondemak/kanit) (OFL) subset เฉพาะ **Latin + ไทย** (~26KB) — ชื่อเพลงภาษาญี่ปุ่น/เกาหลี/จีนตอนนี้จะแสดงเป็นช่องว่าง (แผนรองรับ CJK ด้วยการ render ข้อความฝั่ง server อยู่ใน roadmap)
