# Spotify CYD — คู่มือฉบับละเอียด

🇬🇧 [English](SETUP.md)

เอกสารนี้อธิบายทุกส่วนของระบบ: สถาปัตยกรรม, ความสัมพันธ์ระหว่าง server กับบอร์ด, network, การตั้งค่า Spotify Developer Dashboard, กลไก auth, ขั้นตอนติดตั้งตั้งแต่ศูนย์, และการแก้ปัญหา

---

## 1. ภาพรวมสถาปัตยกรรม

ระบบมี 3 ส่วน ทำงานร่วมกันแบบนี้:

```
┌─────────────────┐   HTTPS + OAuth    ┌──────────────────────┐   HTTP ธรรมดา (LAN)   ┌─────────────────┐
│  Spotify Cloud  │ ←───────────────→  │  Mac mini (server/)  │ ←──────────────────→  │  บอร์ด CYD       │
│  Web API        │                    │  Docker: spotify-cyd │                       │  (firmware/)     │
│                 │                    │  Flask พอร์ต 8080     │                       │  ESP32-2432S028R │
└─────────────────┘                    └──────────────────────┘                       └─────────────────┘
     ต้องมี token                        ทำงานหนักทั้งหมด:                                ทำงานเบา:
     ต้องใช้ TLS                          - เก็บ/ต่ออายุ token                             - ดึง JSON ทุก 2 วิ
                                          - poll เพลงปัจจุบัน                              - วาดจอ + รับทัช
                                          - ย่อปก + แปลง RGB565                           - ส่งคำสั่งเพลง
                                          - คำนวณสีธีมจากปก
```

**ทำไมต้องมี server ตรงกลาง — เหตุผล 3 ข้อ:**

1. **กฎ OAuth ใหม่ของ Spotify (บังคับ พ.ย. 2025):** redirect URI ต้องเป็น HTTPS หรือ loopback (`http://127.0.0.1:...`) เท่านั้น → ESP32 ทำ login flow เองบนตัวบอร์ดไม่ได้อีกแล้ว ต้องมีเครื่องที่เปิด browser ได้ทำ auth แทน
2. **RAM ของบอร์ดจำกัด (ไม่มี PSRAM):** การต่อ TLS ไป Spotify + decode JPEG ปกอัลบั้มบนบอร์ดกินแรม/ซับซ้อน server จึงย่อรูปและแปลงเป็น **raw RGB565** ให้บอร์ดเอาไปวาดตรง ๆ โดยไม่ต้อง decode อะไรเลย
3. **ลด rate limit:** server cache ผล API และรูปไว้ บอร์ด poll กี่รอบก็ไม่กระทบโควต้า Spotify

---

## 2. ความสัมพันธ์ server ↔ board (data flow)

### 2.1 วงจรหลัก (ทุก 2 วินาที)

```
บอร์ด                                 server                              Spotify
  │  GET /now                           │                                    │
  │ ───────────────────────────────→    │  (ถ้า cache เก่ากว่า 1.5 วิ)          │
  │                                     │  GET /me/player ────────────────→  │
  │                                     │  ←──────────────── playback JSON   │
  │  ←─────────────── JSON:             │                                    │
  │  {title, artists, album,            │                                    │
  │   is_playing, progress_ms,          │                                    │
  │   duration_ms, art_id, theme565,    │                                    │
  │   volume, shuffle, repeat, ok}      │                                    │
```

- บอร์ดเทียบ `art_id` กับปกที่โหลดไว้ ถ้าเปลี่ยน → ขอปกใหม่ (ข้อ 2.2)
- `theme565` = สีเด่นของปกในรูปแบบ RGB565 (16-bit int) — บอร์ดใช้ทำสีปุ่ม กรอบ แถบโปรเกรส และย้อมพื้นหลัง
- `volume` (0–100 หรือ -1 = ไม่รู้), `shuffle` (bool), `repeat` (0=ปิด 1=ทั้งลิสต์ 2=เพลงเดียว) — บอร์ดใช้แสดงสถานะไอคอนและคำนวณ volume overlay
- ระหว่างรอ poll รอบถัดไป บอร์ด interpolate `progress_ms` เองในเครื่อง แถบโปรเกรสจึงเดินลื่นทุกวินาทีโดยไม่ต้องยิง network ถี่
- โพลรอบแรกหลังเพลงเปลี่ยน `theme565` อาจเป็น 0 (server ยังคำนวณไม่เสร็จ — ทำใน background thread) รอบถัดไปจะได้ค่าจริง

### 2.2 การโหลดปกอัลบั้ม

```
บอร์ด: GET /art/<art_id>?size=130
server: ดาวน์โหลดปก 300px จาก Spotify CDN (ครั้งแรกครั้งเดียว, cache 8 รูปล่าสุด)
        → ย่อเหลือ 130×130 (LANCZOS)
        → Floyd-Steinberg dithering (พรางรอยขั้นสีบนจอ 65,536 สี)
        → แปลงเป็น raw RGB565 big-endian = 130×130×2 = 33,800 bytes เป๊ะ
บอร์ด: stream ลง buffer ในแรม → pushImage ขึ้นจอทันที
```

**เรื่อง byte order (สำคัญ — เคยเป็นบั๊ก):** server ส่ง big-endian ซึ่ง*ตรงกับลำดับที่ SPI ต้องส่งขึ้นสายอยู่แล้ว* ดังนั้นฝั่งบอร์ดต้องวาดด้วย `setSwapBytes(false)` — ห้ามเปิด swap เด็ดขาด (ต่างจากภาพที่ฝังเป็น PROGMEM array แบบ btc-ticker ที่เก็บเป็นค่าตัวเลข native ซึ่งต้องใช้ `setSwapBytes(true)`) ถ้าเปิดผิดจะเป็นการสลับ byte ซ้ำสองรอบ → ปกกลายเป็นลายรุ้งทั้งใบ

### 2.3 คำสั่งควบคุมเพลง (จากทัช)

```
ทัชบนจอ → บอร์ดตั้ง pendingCmd → netTask ส่ง POST ไป server:
  แถวปุ่ม 5 โซน → /shuffle | /previous | /playpause | /next | /repeat
  แตะแถบ progress → /seek?ms=<ตำแหน่ง>
  ปัดขึ้น/ลง → /volume?delta=±10
→ server เรียก Spotify API แทน (พร้อม token)
→ server รอ 0.3 วิ ให้สถานะนิ่ง แล้ว refresh
→ ตอบ JSON สถานะใหม่กลับ (โครงเดียวกับ /now) → บอร์ดวาดจอทันที
```

หมายเหตุ: คำสั่งเหล่านี้ต้องมี **Spotify Premium** และต้องมี "active device" (มีเครื่องที่กำลังเล่นเพลงอยู่) ไม่งั้น Spotify ตอบ 403/404 ซึ่ง server จะกลืน error ไว้ให้ (log อย่างเดียว) — การสั่ง volume โดนปฏิเสธบนอุปกรณ์บางชนิด (เช่น iPhone เป็นตัวเล่น)

### 2.4 การแบ่งงานสองคอร์ในบอร์ด

```
Core 0 (netTask)                       Core 1 (loop)
- poll /now ทุก 2 วิ                    - วาดจอทั้งหมด (คอร์เดียวเท่านั้น — TFT ไม่ thread-safe)
- โหลด /art เมื่อปกเปลี่ยน               - อ่านทัช (press/release + swipe) → ตั้ง pendingCmd
- ส่ง POST คำสั่งเพลง                    - progress tick ทุก 1 วิ + อัปเดต backlight ตามแสง
- อัปเดต state + ตั้ง dirty flags        - เช็ค dirty flags → วาดเฉพาะส่วนที่เปลี่ยน
        └──── สื่อสารผ่าน volatile flags + mutex (stateMux) ────┘
```

---

## 3. Network

### 3.1 ผังเครือข่าย

| เครื่อง | ที่อยู่ | หมายเหตุ |
|---|---|---|
| Mac mini (server) | `192.168.1.195` พอร์ต **8080** | ค่านี้ฝังเป็น default ใน firmware |
| บอร์ด CYD | DHCP พอร์ต **80** | เปลี่ยนได้เมื่อ router แจกใหม่ — ควรตั้ง reservation |
| mDNS ของบอร์ด | `spotify-cyd.local` | ใช้ได้บ้างไม่ได้บ้าง (ดู 3.3) |

ทุกอย่างวิ่งบน LAN เดียวกันด้วย **HTTP ธรรมดา ไม่มี TLS** — ยอมรับได้เพราะเป็นเครือข่ายในบ้าน ข้อมูลที่วิ่งคือชื่อเพลง/รูปปก ไม่มี secret (token อยู่บน Mac mini เท่านั้น ไม่เคยถูกส่งให้บอร์ด)

### 3.2 บอร์ดรู้จัก server ได้ยังไง

ลำดับความสำคัญ:
1. ค่าที่เก็บใน NVS (flash ของบอร์ด) key `server` namespace `scyd`
2. ถ้าไม่เคยตั้ง → default `192.168.1.195` (กำหนดใน `firmware/src/main.cpp` → `DEFAULT_SERVER`)

วิธีเปลี่ยนมี 2 ทาง:
- เปิด `http://<ip บอร์ด>/server?h=<ip ใหม่>` → บันทึกแล้วรีบูตทันที
- ผ่านหน้า WiFiManager portal (ช่อง "Server IP (Mac mini)") ตอน setup WiFi ใหม่

**ควรตั้ง DHCP reservation ให้ Mac mini** ที่ router (ผูก MAC → 192.168.1.195 ถาวร) ไม่งั้นถ้า Mac เปลี่ยน IP บอร์ดจะหา server ไม่เจอ

### 3.3 การหาบอร์ดเจอ (เมื่อ IP เปลี่ยน)

- ลอง `ping spotify-cyd.local` ก่อน (mDNS — บางทีใช้ได้)
- ถ้าไม่ได้ ให้กวาดหา endpoint เฉพาะของบอร์ด:
  ```bash
  for i in $(seq 2 254); do (curl -s --max-time 1 "http://192.168.1.$i/ldr" | grep -q "^ldr=" && echo "บอร์ดอยู่ที่ 192.168.1.$i") & done; wait
  ```
- หรือดูใน ARP: `arp -a | grep -i "e4:65:b8"` (OUI ของ Espressif)
- **ระวัง: ถ้ามีบอร์ด ESP32 หลายตัวในบ้าน** (เช่น btc-ticker) อย่าเชื่อ IP เดิม — เช็คด้วย endpoint เฉพาะ (`/ldr`, `/artbuf`) ก่อน OTA เสมอ
- จะตั้ง DHCP reservation ให้บอร์ดด้วยก็ยิ่งดี — จะได้ OTA ไป IP เดิมได้ตลอด

### 3.4 บอร์ดต่อ WiFi ยังไง (ครั้งแรก / เปลี่ยนบ้าน)

Firmware ใช้ WiFiManager:
1. บูตแล้วพยายามต่อ WiFi ที่เคยจำไว้ (เก็บใน NVS โดย WiFi stack — รอด reflash)
2. ต่อไม่ได้ภายใน ~15 วิ × 5 ครั้ง → เปิด Access Point ชื่อ **`Spotify-CYD-Setup`** พร้อมโชว์คำแนะนำบนจอ
3. เอามือถือต่อ AP นั้น → เปิด `192.168.4.1` → เลือก WiFi บ้าน + ใส่รหัส + (แก้ Server IP ได้ในหน้าเดียวกัน) → Save
4. portal เปิดค้างเกิน 3 นาทีไม่มีใครตั้งค่า → บอร์ดรีบูตแล้วลองใหม่

---

## 4. Spotify Developer Dashboard

### 4.1 สร้าง App (ทำครั้งเดียว)

1. เข้า https://developer.spotify.com/dashboard → login ด้วยบัญชี Spotify ปกติ → **Create app**
2. กรอกฟอร์ม:
   - **App name:** อะไรก็ได้ **แต่ห้ามขึ้นต้นด้วย "Spot"** (กฎของ Spotify) เช่น `cyd-now-playing`
   - **App description:** อะไรก็ได้
   - **Website:** เว้นว่างได้
   - **Redirect URIs:** พิมพ์ `http://127.0.0.1:8888/callback` แล้ว **ต้องกดปุ่ม Add** ก่อน Save (พิมพ์ค้างไว้เฉย ๆ ไม่ถูกบันทึก)
   - **Which API/SDKs:** ติ๊กเฉพาะ **Web API**
3. เข้า app → **Settings** → คัดลอก **Client ID** และกด View client secret → คัดลอก **Client Secret**

### 4.2 กฎ Redirect URI (เหตุผลที่ต้อง 127.0.0.1)

ตั้งแต่ พ.ย. 2025 Spotify บังคับทุก app:
- ❌ `http://localhost:8888/callback` — ห้ามใช้คำว่า localhost
- ❌ `http://192.168.x.x/callback` — ห้าม HTTP ไปยัง IP ทั่วไป (นี่คือสิ่งที่ทำให้โปรเจกต์ ESP32 เก่า ๆ ที่ login บนบอร์ดพังหมด)
- ✅ `http://127.0.0.1:8888/callback` — loopback IP ตัวเลขเท่านั้นที่ยกเว้น
- ✅ `https://...` — ต้องมี cert จริง (ยุ่งยากเกินจำเป็นสำหรับงานนี้)

ค่านี้ต้อง**ตรงเป๊ะทุกตัวอักษร**กับที่อยู่ใน `server/config.json` (`redirect_uri`) — ไม่ตรง = error `INVALID_CLIENT: Invalid redirect URI`

### 4.3 โหมด Development

App ใหม่อยู่ในโหมด development: ใช้ได้กับบัญชีเจ้าของ + ผู้ใช้ที่ add ไว้ (สูงสุด 25 คน) — สำหรับจอส่วนตัวแบบนี้ไม่ต้องทำอะไรเพิ่ม ใช้บัญชีตัวเองได้เลย

---

## 5. Auth — กลไกและวงจรชีวิตของ token

### 5.1 Flow ครั้งแรก (ต้องทำบนเครื่อง server เท่านั้น)

```
คุณรัน server/auth.py บนเครื่อง server
  1. spotipy เปิด listener เล็ก ๆ รอที่ 127.0.0.1:8888 (ในเครื่องเอง)
  2. เปิด browser ไปหน้า authorize ของ Spotify (แนบ client_id + scopes + redirect_uri)
  3. คุณกด "Agree" ในเบราว์เซอร์
  4. Spotify สั่ง browser redirect ไป http://127.0.0.1:8888/callback?code=XXXX
     → browser วิ่งกลับเข้าเครื่องตัวเอง → listener ของ spotipy รับ code
  5. spotipy เอา code + client_secret ไปแลก access_token + refresh_token
  6. เก็บทั้งคู่ลงไฟล์ server/.spotify_token_cache
```

จุดสำคัญ: **Spotify ไม่เคยต้องเชื่อมเข้ามาหาเครื่องเราเลย** — คนที่วิ่งไปที่ 127.0.0.1 คือ browser ของเราเอง กฎ loopback จึงใช้ได้กับเครื่องหลังบ้าน/NAT ทุกกรณี แต่ก็แปลว่า **browser กับ auth.py ต้องอยู่เครื่องเดียวกัน** (ห้าม ssh ไปรันแล้วเปิด browser คนละเครื่อง)

### 5.2 Scopes ที่ขอ

| scope | ใช้ทำอะไร |
|---|---|
| `user-read-currently-playing` | อ่านเพลงที่กำลังเล่น |
| `user-read-playback-state` | อ่านสถานะ play/pause, progress, volume, shuffle, repeat, device |
| `user-modify-playback-state` | สั่ง play/pause/next/previous/seek/volume/shuffle/repeat |

ถ้าแก้ scopes ในภายหลัง ต้องลบ `.spotify_token_cache` แล้วรัน `auth.py` ใหม่

### 5.3 วงจรชีวิต token

- **access_token** อายุ 1 ชั่วโมง — `app.py` (ผ่าน spotipy) ต่ออายุให้อัตโนมัติโดยใช้ refresh_token ก่อนหมดทุกครั้ง ไม่ต้องทำอะไร
- **refresh_token** ไม่มีวันหมดอายุ ตราบใดที่ไม่ถูก revoke (ถอนสิทธิ์ที่ https://www.spotify.com/account/apps/ หรือเปลี่ยน client_secret)
- token ทั้งหมดอยู่ในไฟล์ `server/.spotify_token_cache` (ไฟล์เดียว, permission 600) — **ไฟล์นี้คือกุญแจเข้าบัญชี Spotify ห้าม commit / แชร์**
- Docker container ไม่ได้เก็บ token เอง — มัน bind-mount ไฟล์นี้จาก host (ดู 6.3) ดังนั้น auth ใหม่บน host เมื่อไหร่ container ก็เห็นทันที

### 5.4 ต้อง auth ใหม่เมื่อไหร่

- เปลี่ยน scopes / client_id / client_secret
- กด revoke ใน Spotify account
- ไฟล์ cache หาย

อาการ: `docker logs spotify-cyd` ขึ้น `no cached token - run auth.py first` และบอร์ดขึ้นหน้า idle ตลอด วิธีแก้: `server/.venv/bin/python server/auth.py` บนเครื่อง server แล้วจบ ไม่ต้อง restart container

---

## 6. ติดตั้งตั้งแต่ศูนย์ (step-by-step)

### 6.1 เตรียมฝั่ง server

```bash
cd spotify-cyd/server

# 1. สร้าง virtualenv (ใช้ตอน auth และตอน dev — ตัว production รันใน Docker)
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt

# 2. สร้าง config template (รันครั้งแรกจะ exit พร้อมสร้างไฟล์)
.venv/bin/python app.py

# 3. แก้ config.json — ใส่ client_id / client_secret จาก Dashboard (ข้อ 4)
#    (มีตัวอย่างที่ config.json.example)

# 4. login ครั้งเดียว (เปิด browser)
.venv/bin/python auth.py
# เห็น "Logged in as: <ชื่อ>" = สำเร็จ
```

### 6.2 ทดสอบ server ก่อนขึ้น Docker

```bash
.venv/bin/python app.py &
curl http://127.0.0.1:8080/health          # {"ok": true}
# เปิดเพลงใน Spotify แล้ว:
curl http://127.0.0.1:8080/now             # ต้องเห็นชื่อเพลง
curl -X POST http://127.0.0.1:8080/next    # เพลงต้องข้าม (ต้องมี Premium)
kill %1
```

### 6.3 รันถาวรด้วย Docker

```bash
cd spotify-cyd
docker compose up -d --build
```

สิ่งที่ `docker-compose.yml` ทำ:
- build image จาก `server/Dockerfile` (python:3.12-slim + pip deps)
- map พอร์ต `8080:8080`
- **bind-mount 2 ไฟล์จาก host เข้า container:** `config.json` (อ่าน credentials) และ `.spotify_token_cache` (อ่าน+เขียน — spotipy ต่ออายุ token แล้วเขียนกลับลงไฟล์นี้)
- `restart: unless-stopped` → container ฟื้นเองเมื่อ crash หรือหลังรีบูต (Docker Desktop ต้องตั้ง Start at login)

คำสั่งดูแล:
```bash
docker logs -f spotify-cyd        # ดู log สด
docker compose restart            # restart
docker compose up -d --build      # deploy โค้ดใหม่หลังแก้ app.py
```

(ทางเลือกไม่ใช้ Docker: launchd plist อยู่ที่ `server/com.kritsadas.spotify-cyd.plist` — copy ไป `~/Library/LaunchAgents` แล้ว `launchctl load`)

### 6.4 Flash บอร์ด

ต้องมี PlatformIO CLI (`pio`)

```bash
cd firmware

# ครั้งแรก: ผ่านสาย USB
pio run -t upload

# ครั้งถัดไป: ผ่าน WiFi (OTA) — ไม่ต้องเสียบสาย
pio run -e cyd_ota -t upload --upload-port <ip บอร์ด>
```

Environments ใน `platformio.ini`:
| env | ใช้เมื่อไหร่ |
|---|---|
| `cyd` (default) | บอร์ด CYD ปกติ USB 1 ช่อง (จอ ILI9341) |
| `cyd_ota` | เหมือน `cyd` แต่ upload ผ่าน WiFi |
| `cyd2usb` / `cyd2usb_ota` | เผื่อบอร์ดรุ่น USB 2 ช่อง (จอ ST7789 ต้อง BGR + inversion) |

### 6.5 บูตครั้งแรก

1. จอขึ้น "Spotify CYD v_._._ ... กำลังต่อ WiFi" (เลขเวอร์ชัน firmware แสดงตรงนี้)
2. ถ้าไม่เคยตั้ง WiFi → ทำตามข้อ 3.4
3. ต่อได้แล้ว → ถ้ามีเพลงเล่นจะขึ้นหน้า Now Playing ภายใน ~2-4 วิ / ไม่มีเพลง → หน้านาฬิกา

---

## 7. การใช้งานประจำวัน

- **แถวปุ่มล่าง 5 โซน**: สลับสุ่ม | เพลงก่อนหน้า | เล่น/หยุด | เพลงถัดไป | เล่นซ้ำ — ไอคอนสุ่ม/ซ้ำติดสีธีมเมื่อเปิด (เล่นซ้ำเพลงเดียวมีเลข "1")
- **แตะแถบ progress** = seek ไปตำแหน่งนั้น
- **ปัดขึ้น/ลง** = เสียง ±10% (แถบ volume โชว์แทนแถบ progress ~1.6 วิ)
- พื้นหลัง + ปุ่ม + กรอบปก เปลี่ยนสีตามโทนปกอัลบั้มอัตโนมัติ
- **จอหรี่อัตโนมัติตามแสงห้อง** (เซ็นเซอร์ LDR บนบอร์ด, ปรับได้ใน `main.cpp`: `LDR_BRIGHT_RAW`, `LDR_DARK_RAW`, `MIN_DUTY`)
- **แตะจอตอนจอหรี่** → สว่างเต็ม 5 นาที (`TOUCH_WAKE_MS`) แล้วหรี่กลับเอง — การแตะครั้งแรกตอนหรี่จะแค่ปลุกจอ ไม่นับเป็นคำสั่งเพลง (กันแตะพลาดตอนมืด)
- ไม่มีเพลงเล่น → นาฬิกา (แตะซ้าย/กลาง/ขวา = ก่อนหน้า/เล่น-หยุด/ถัดไป) | server ล่ม → ขึ้นข้อความพร้อม IP ที่พยายามต่อ

### HTTP endpoints ทั้งหมด

**Server (:8080)**
| endpoint | method | ทำอะไร |
|---|---|---|
| `/now` | GET | JSON เพลงปัจจุบัน (โครงตาม 2.1) |
| `/art/<art_id>?size=N` | GET | ปกเป็น raw RGB565 big-endian, N×N (32–240, default 170) |
| `/playpause` `/play` `/pause` `/next` `/previous` | POST | สั่งเพลง แล้วตอบสถานะใหม่ |
| `/seek?ms=N` | POST | กระโดดไปตำแหน่ง N มิลลิวินาที |
| `/volume?delta=N` หรือ `/volume?set=N` | POST | ปรับเสียงสัมพัทธ์/สัมบูรณ์ (clamp 0–100) |
| `/shuffle` | POST | toggle สลับสุ่ม |
| `/repeat` | POST | วนโหมดซ้ำ off → context → track |
| `/text?t=&px=N` | GET | render ข้อความเป็น strip 4-bit grayscale (Noto ไทย→Latin→CJK); `&maxw=` จำกัดความกว้างบรรทัดเดียว, `&wrap=W&lines=N` = หลายบรรทัด; header 4 byte (w,h) ตามด้วยข้อมูลทีละแถว stride (w+1)/2 |
| `/devices` | GET | รายชื่ออุปกรณ์ Spotify Connect (id, name, type, active) |
| `/transfer?id=` | POST | ย้ายเพลงไปเล่นที่อุปกรณ์นั้น (force-play) |
| `/health` | GET | เช็คว่า server มีชีวิต |

**บอร์ด (:80)**
| endpoint | ทำอะไร |
|---|---|
| `/version` | เลขเวอร์ชัน firmware — ไว้เช็คตัวตนบอร์ดก่อน OTA |
| `/screen` | screenshot จอเป็น RGB565 dump — ใช้คู่ `tools/capture.py <ip> [ไฟล์.png]`; ถ้าแรมแตกกระจายจนจองภาพ capture ไม่ได้ บอร์ดจะปล่อย text strips ชั่วคราว (กลับมาเองใน ~2 วิ); ภาพ capture อาจมี artifact เล็กน้อยตอน marquee กำลังวิ่ง |
| `/server?h=IP` | เปลี่ยน server IP (เก็บ NVS) แล้วรีบูต / เรียกไม่ใส่ `h` = ดูค่าปัจจุบัน |
| `/flip` | พลิกภาพ 180° (rotation 1↔3, เก็บ NVS) — สลับทิศมุมมองที่ดีของจอ TN |
| `/test` | หน้าทดสอบจอ 2 นาที: แถบ RGBW + gradient A (drawPixel) vs B (pushImage) — A กับ B ต้องเหมือนกันเสมอ ถ้าต่าง = byte order พัง |
| `/ldr` | ค่าเซ็นเซอร์แสง (smoothed) + ระดับ backlight ปัจจุบัน — ไว้ calibrate การหรี่จอ |
| `/touch` | gesture ทัชล่าสุด (พิกัด start/end + dy) — ไว้ calibrate ทัช |
| `/artbuf` | dump บัฟเฟอร์ปกดิบในแรมบอร์ด (debug เทียบกับของ server) |

---

## 8. Troubleshooting (จากเหตุการณ์จริง)

| อาการ | สาเหตุ | วิธีแก้ |
|---|---|---|
| ปกเป็นลายรุ้ง/เม็ดสีเพี้ยน แต่ตัวหนังสือปกติ | byte order สลับซ้ำ (`setSwapBytes(true)` กับข้อมูล big-endian) | ต้องเป็น `setSwapBytes(false)` ใน `drawArt()` — แก้ถาวรแล้ว, เช็คได้ด้วย `/test` (A ต้อง = B) |
| ทั้งจอโทนฟ้า/ซีดเมื่อมองเฉียง | จอ TN มุมมองแคบ (ข้อจำกัดฮาร์ดแวร์) | ตั้งจอเอียงเข้าหาสายตา หรือ `/flip` เพื่อสลับทิศมุมมองที่ดี |
| ถ่ายรูปจอแล้วปกเป็นจุดรุ้ง (แต่ตาเปล่าปกติ) | moiré ของกล้อง + subpixel จอ | ไม่ใช่ปัญหาจริง |
| ปกไล่สีเป็นขั้น ๆ | จอแสดงได้ 65,536 สี | server ทำ dithering ให้แล้ว (`rgb565_dithered`) |
| จอขึ้น "เชื่อมต่อ server ไม่ได้" | Mac ปิด / container ตาย / Mac เปลี่ยน IP | `docker ps` เช็ค, ตั้ง DHCP reservation, หรือ `/server?h=IP ใหม่` |
| `no cached token - run auth.py first` ใน log | token cache หาย/ถูก revoke | รัน `auth.py` ใหม่บน host (ดู 5.4) |
| กดปุ่มแล้วเพลงไม่เปลี่ยน | ไม่มี active device หรือไม่ใช่ Premium | เปิดแอป Spotify ให้เล่นเพลงอยู่ก่อน |
| ปัดปรับเสียงแล้วเสียงไม่เปลี่ยน | อุปกรณ์เป้าหมายไม่รับคำสั่ง volume (เช่น iPhone) | เปลี่ยนไปเล่นบนเครื่องที่รับได้ (desktop/ลำโพง) |
| จอไม่หรี่ตอนมืด / หรี่ตลอด | threshold LDR ไม่ตรงกับห้อง | ดูค่าจริงที่ `/ldr` แล้วปรับ `LDR_BRIGHT_RAW` / `LDR_DARK_RAW` |
| OTA ไม่เจอบอร์ด | mDNS ไม่นิ่ง / IP เปลี่ยน / มีบอร์ด ESP32 ตัวอื่นแย่ง IP | หา IP ตามข้อ 3.3 **แล้วเช็คว่าเป็นบอร์ดตัวถูกก่อน** แล้ว `--upload-port <ip>` |
| ปกโหลดไม่ขึ้น (ค้าง placeholder แผ่นเสียง) | server ดึงรูปจาก CDN ไม่ได้ / buffer ไม่ครบ | ดู serial log บอร์ด (`[art] ...got X/33800`) และ `docker logs` |
| ชื่อเพลงญี่ปุ่น/เกาหลี/จีนเป็นช่องว่าง | ฟอนต์ที่ฝัง subset แค่ Latin+ไทย | พฤติกรรมปัจจุบัน — roadmap จะแก้ด้วย server-side text rendering |

---

## 9. โครงสร้างโค้ดและจุดปรับแต่ง

```
spotify-cyd/
├── server/
│   ├── app.py                # Flask ทั้งหมด: /now /art /คำสั่ง + dithering + theme color
│   ├── auth.py               # OAuth ครั้งแรก (รันบน host เท่านั้น)
│   ├── spotify_common.py     # config + SpotifyOAuth ที่ใช้ร่วมกัน
│   ├── config.json           # client_id/secret (gitignored — ดู config.json.example)
│   ├── .spotify_token_cache  # token (gitignored, ห้ามแชร์)
│   ├── Dockerfile
│   └── com.kritsadas.spotify-cyd.plist   # ทางเลือก launchd
├── docker-compose.yml
├── firmware/
│   ├── platformio.ini        # pins, SPI 40MHz, partition min_spiffs, envs
│   └── src/
│       ├── main.cpp          # ทั้งแอป: WiFi, netTask, UI, touch, endpoints
│       └── kanit_font.h      # ฟอนต์ Kanit Medium subset ไทย+ละติน 26KB (PROGMEM)
├── tools/capture.py          # ดึง screenshot จริงจากบอร์ด
├── CHANGELOG.md
└── docs/                     # SETUP.md (EN) / SETUP.th.md (ไฟล์นี้)
```

จุดปรับแต่งยอดนิยม (ทั้งหมดใน `firmware/src/main.cpp` เว้นแต่ระบุ):

| อยากปรับ | ที่ไหน |
|---|---|
| ความเข้มพื้นหลังธีม | `themeBg()` — ตัวเลข `76` (0–255, มาก = สว่าง) |
| ขนาดปก | `ART_SIZE` (ต้อง ≤ 240 และแก้ layout ตาม) |
| ความถี่ poll | `POLL_INTERVAL_MS` (default 2000) |
| ช่วงแสง/ระดับหรี่จอ | `LDR_BRIGHT_RAW` `LDR_DARK_RAW` `MIN_DUTY` (ดูค่าจริงจาก `/ldr`) |
| ระยะเวลาปลุกจอเมื่อแตะตอนหรี่ | `TOUCH_WAKE_MS` (default 300000 = 5 นาที) |
| step ปรับเสียงต่อ swipe | ค่าคงที่ `±10` ใน `handleTouch()` |
| สีเด่นจากปก (อัลกอริทึม) | `server/app.py` → `dominant_color()` |
| ฟอนต์ | สร้าง subset ใหม่ด้วย `pyftsubset` แล้วแปลงเป็น header แทน `kanit_font.h` |

### ข้อควรรู้เชิงดีไซน์ (อย่าเผลอแก้กลับ)

1. `setSwapBytes(false)` สำหรับปกจาก server (big-endian) — ดูข้อ 2.2
2. SPI จอ 40MHz — ILI9341 สเปคจริงรับแค่นี้ (55MHz เดิมเสี่ยง)
3. วาดจอจาก core 1 (`loop`) เท่านั้น — netTask ห้ามแตะ TFT/OFR
4. ฟอนต์ไทยเรนเดอร์ด้วย OpenFontRender (FreeType) — สระ/วรรณยุกต์ใช้ zero-advance ของฟอนต์ ไม่มี GPOS จึงอาจเพี้ยนเล็กน้อยกับพยัญชนะหางสูง (ป ฝ ฟ) — ยอมรับได้
5. `/screen` วาดลง sprite 8-bit → สีใน screenshot หยาบกว่าจอจริง ใช้ดู layout ไม่ใช่ดูสี
6. หัวกลมแถบ progress ล้นออกนอกแถบ ±5px — เวลาแก้โค้ดวาด ต้อง clear รอบแถบทั้ง 4 ด้าน (เคยเป็นบั๊กติ่งขาวค้าง v1.1.0)
