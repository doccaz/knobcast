# ESP32-C3 Chromecast Physical Remote

A PlatformIO project that turns an **ESP32-C3 + KY-040 rotary encoder + 72×40 OLED**
into a physical volume/playback controller for any Chromecast on your local network.

**Passive / non-invasive:** the device attaches to whatever app is already running
on the Chromecast (Spotify, YouTube, etc.) and controls it without launching a new
receiver or interrupting playback.

---

## Why did I build this?

I wanted a dedicated physical remote for my Chromecast that would allow me to control
volume and playback without having to pick up my phone or use the Google Home app.
I also wanted to be able to see the current volume level and playback status at a
glance, and have the ability to quickly switch between different Chromecasts on my
network.

---

## Features

- **Volume control** via rotary encoder with 5× acceleration on fast spin
- **Playback control** — play/pause, next/previous track, stop, seek
- **Mute/unmute** toggle via long press
- **On-device menu system** navigated entirely with the encoder + back button
- **Multi-device support** — discover and switch between Chromecasts (including groups)
- **72×40 OLED** with HUD, scrolling text, overlays, and configurable screen timeout
- **Web UI** for configuration, control, status, WiFi scanning, and debug logs
- **AP mode** captive portal for first-time WiFi setup (no hardcoded credentials)
- **Persistent config** stored in NVS (survives reboots and reflashes)
- **Auto-reconnect** on connection drop or app change
- **Auto-connect** to last known device on boot (configurable)
- **Scan on boot** — automatic mDNS discovery at startup (configurable)
- **Progress bar modes** — show volume level or elapsed playback time
- **LED status** — solid when connected, pulsing during scan/connect, off when idle
- **mDNS registration** — device accessible at `knobcast.local`

---

## Hardware

| Component | Detail |
|---|---|
| MCU | ESP32-C3 dev board (RISC-V, WiFi) |
| Encoder | KY-040 rotary encoder with push-button |
| Display | 72×40 SSD1306 OLED (I2C, addr 0x3C) — onboard |
| LED | Onboard LED (GPIO 8, active-low) — solid when connected, pulses during scan/connect |

### Wiring

```
KY-040 Pin  →  ESP32-C3 GPIO
─────────────────────────────
CLK (A)     →  2  (internal pull-up)
DT  (B)     →  3  (internal pull-up)
SW          →  4  (internal pull-up, active-low)
+           →  3.3 V
GND         →  GND

Back button →  9  (internal pull-up, active-low)

OLED (I2C)
──────────
SDA         →  5
SCL         →  6
```

> All ESP32-C3 GPIOs support internal pull-ups — no external resistors needed.

---

## Controls

### HUD mode (default)

| Action | Function |
|---|---|
| Rotate CW / CCW | Volume up / down (configurable step, 5× acceleration) |
| Short press (< 0.8 s) | Open menu |
| Long press (≥ 0.8 s) | Mute / Unmute toggle |

### Menu mode

| Action | Function |
|---|---|
| Rotate CW / CCW | Navigate menu items |
| Short press | Select item |
| Back button (GPIO 9) | Go back / exit menu |

### Menu structure

```
Main Menu
├── Actions
│   ├── Play / Pause
│   ├── Previous track
│   ├── Next track
│   ├── Connection info
│   └── Disconnect
├── Devices
│   ├── <discovered devices>
│   ├── Scan network
│   └── Back
├── Scan network
├── Connection info
├── Disconnect
├── About
├── Settings
│   ├── Menu Timeout (5s / 15s / 30s / 60s)
│   ├── Screen Timeout (30s / 1m / 5m / 10m / Never)
│   ├── Progress Bar (Volume / Elapsed time)
│   ├── Scan on boot (on/off)
│   ├── Auto-connect (on/off)
│   └── Back
├── Reboot
└── Exit
```

---

## How it works — Passive Cast Control

```
ESP32-C3  ──── WiFi ────  Chromecast
                 │
            Port 8009
            TLS (self-signed cert → setInsecure())
                 │
        [4-byte BE uint32 length] + [Protobuf CastMessage]
                 │
       Namespaces / channels:
         tp.connection  → CONNECT (to receiver + transport)
         tp.heartbeat   → PING / PONG (every 5 s)
         receiver       → GET_STATUS, SET_VOLUME
         media          → PLAY, PAUSE, NEXT, PREVIOUS, SEEK, STOP
```

The key design choice is **no LAUNCH** — we never start a new receiver app:

1. **CONNECT + GET_STATUS** — on connection, read `RECEIVER_STATUS` to discover
   the currently running app and its `transportId`.
2. **Volume/mute** — sent to `receiver-0` on the receiver namespace (device-level,
   works regardless of which app is running).
3. **Media commands** — sent to the existing `transportId` on the media namespace
   after connecting to the transport and learning the `mediaSessionId`.
4. **Session tracking** — if the app changes (e.g. Spotify → YouTube), the new
   `transportId` is detected in `RECEIVER_STATUS` and reconnected automatically.
5. **Dual command format** — next/previous sends both `NEXT`/`QUEUE_NEXT` and
   `PREVIOUS`/`QUEUE_PREV` for maximum app compatibility.

The protobuf is hand-rolled in `cast_message.h` (no nanopb needed).

---

## OLED Display

| Screen | When | Content |
|---|---|---|
| Splash | Boot | "KnobCast" + IP address |
| AP Mode | No WiFi config | "AP Mode" + SSID + IP |
| Connecting | WiFi/Cast connect | "Connecting" + target |
| HUD | READY state | Device name, volume bar + %, play state, app name |
| Menu | Encoder press | Title bar + 3 visible items with scroll |
| Overlay | Any action | Large text (e.g. "VOL 42%", "PAUSE") for 1.2 s |

Long device/app names scroll automatically (50 ms speed, 1.5 s pause).

### Screen timeout (burn-in protection)

The display powers off after a configurable timeout (default 10 min). Any encoder
or button input wakes it immediately. Options: 30 s, 1 min, 5 min, 10 min, or never.

---

## Web UI

Runs on port 80 in both AP and STA modes.

| Endpoint | Method | Function |
|---|---|---|
| `/` | GET | Main HTML page (config + control + status) |
| `/save` | POST | Save WiFi/Cast config → reboot |
| `/status` | GET | JSON: volume, muted, playing, app, device, time, config |
| `/control` | POST | Commands: play, pause, stop, next, prev, mute, unmute, volup, voldown, setvol:N, seek:N, disconnect |
| `/scan` | GET | mDNS scan → JSON list of discovered Chromecasts |
| `/connectcast` | POST | Connect to a specific device by IP and port |
| `/wifiscan` | GET | Scan available WiFi networks |
| `/testwifi` | POST | Test WiFi credentials without saving |
| `/reset` | POST | Factory reset (clear NVS) → reboot |
| `/log` | GET | Debug log (JSON array with timestamps) |
| `/debug` | POST | Enable/disable/clear debug logging |

### First-time setup (AP mode)

On first boot (or after factory reset), the device starts as a WiFi AP named
`KnobCast-XXXX` with a captive portal. Connect to it and a config page opens
automatically. Enter your WiFi credentials and optionally a Chromecast IP
(leave blank for mDNS auto-discovery).

---

## Configuration (NVS)

Stored in ESP32 NVS namespace `knobcast`:

| Key | Type | Default | Description |
|---|---|---|---|
| `configured` | bool | false | Has WiFi been saved? |
| `wifiSsid` | string | "" | WiFi SSID |
| `wifiPass` | string | "" | WiFi password |
| `castIp` | string | "" | Chromecast IP (blank = mDNS) |
| `volStep` | float | 0.02 | Volume step per click (1–20 %) |
| `menuTimeout` | int | 15 | Menu auto-close timeout in seconds |
| `screenTimeout` | int | 600 | Display sleep timeout in seconds (0 = never) |
| `scanOnBoot` | bool | true | Run mDNS scan on startup |
| `barMode` | int | 0 | Progress bar: 0 = volume, 1 = elapsed time |
| `autoConnect` | bool | true | Auto-connect to last device on boot |
| `lastDevIp` | string | "" | Last connected device IP |
| `lastDevName` | string | "" | Last connected device name |
| `lastDevPort` | uint16 | 8009 | Last connected device port |

---

## State machine

```
INIT
  │ config.configured?
  ├── no ─→ AP_MODE        WiFi AP + captive portal + web config UI
  │                          (loops until config saved → reboot)
  └── yes
       ▼
WIFI_CONNECTING            Connect to saved SSID (timeout → AP_MODE fallback)
       │ connected
       ▼
WIFI_CONNECTED             mDNS discovery or configured IP
       │ found
       ▼
READY                      loop(): TLS connect, heartbeat, encoder events,
                            menu, web server, OLED, reconnect on drop
```

---

## Build & flash

```bash
# Requires PlatformIO CLI or VS Code with PlatformIO extension

pio run --target upload                              # default env
pio run -e esp32c3dev_oled72x40 --target upload      # OLED variant
pio device monitor                                   # 115200 baud
```

---

## File structure

```
chromecast-esp32/
├── platformio.ini              ← board: esp32-c3-devkitm-1, libs: ArduinoJson, U8g2
└── src/
    ├── main.cpp                ← WiFi/AP setup, state machine, encoder→Cast glue
    ├── config.h                ← NVS-backed persistent configuration
    ├── display.h               ← OLED display manager (HUD, menu, overlays, timeout)
    ├── menu.h                  ← Menu structure and navigation
    ├── web_server.h            ← Web UI + AP mode captive portal
    ├── rotary_encoder.h        ← Interrupt-driven KY-040 + quadrature state machine + acceleration
    ├── chromecast_client.h     ← Cast client public API (passive mode)
    ├── chromecast_client.cpp   ← Cast client implementation (connect, discover, control)
    ├── cast_message.h          ← Hand-rolled protobuf CastMessage encoder/decoder
    └── debug_log.h             ← Ring buffer debug log (60 lines, serial + web)
```

---

## Limitations / future work

- Device authentication (`com.google.cast.tp.deviceauth`) is not verified.
- Some apps (e.g. Spotify) may not expose `mediaSessionId` for media commands —
  volume/mute always works as it's receiver-level.
- Potential additions: OTA firmware updates.
- Also... I'm working on a cool 3D printed case for this remote, so stay tuned!
