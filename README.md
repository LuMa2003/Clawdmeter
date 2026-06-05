# Clawdmeter (display-only fork)

A small ESP32 dashboard that sits on the desk and shows Claude Code usage.

This is a personal fork of [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter) stripped down to display-only. The splash screen with the Clawd animations, the touch toggle, the side-button HID keystrokes, and the alternative board ports have all been removed. What's left: a Waveshare ESP32-S3-Touch-AMOLED-2.16 showing two utilization bars (current 5h, weekly 7d) updated over BLE by a Windows tray daemon.

![Usage meter](assets/demo.jpeg)

## Hardware

[Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/esp32-s3-touch-amoled-2.16.htm). Only board the firmware supports — the touch controller and side buttons are physically present on the board but the firmware doesn't drive them.

## Prerequisites

- Windows 10/11, macOS, or Linux
- [PlatformIO CLI](https://docs.platformio.org/en/latest/core/installation/index.html)
- Linux: `bluetoothctl` (BlueZ)
- macOS: `python3` (the installer sets up a venv)
- Windows: Python 3.11+ (`pystray`, `bleak`, `httpx`, `pywin32` get installed by the installer)
- Claude Code with an active subscription

## Windows installation (primary platform)

```powershell
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port COM5   # use your device's COM port
```

Pair the device once via **Settings → Bluetooth & devices → Add device → Bluetooth → Clawdmeter**. Pairing isn't strictly required for the data display (the custom GATT service doesn't require encryption) but it makes the device persistent in Windows' known list.

Run the installer:

```powershell
powershell -ExecutionPolicy Bypass -File install-windows.ps1
```

This creates a venv, installs deps from `daemon/requirements-windows.txt`, registers a per-user login-autostart entry (HKCU\Run, no admin), and launches the tray app.

Right-click the tray icon for status / autostart toggle / quit. Logs at `%LOCALAPPDATA%\Clawdmeter\daemon.log`.

## macOS / Linux installation

```bash
./flash-mac.sh                  # macOS, auto-detect /dev/cu.usbmodem*
./flash.sh                      # Linux, default /dev/ttyACM0
./install-mac.sh                # macOS launchd
./install.sh                    # Linux systemd --user
```

## Physical controls

| Button | What it does |
|---|---|
| **PWR** (middle, AXP2101 PKEY) | Short tap → cycle screen brightness (64 → 128 → 200 → 255). Hold ~3s and release → clear BLE bonds + re-advertise (pair gesture). Hold ~7s → manual deep sleep. |
| **BOOT** (left, GPIO 0) | Wake from sleep — no app-level action. Used as an RTC GPIO wake source for deep sleep. |

Touch input and the second side button (GPIO 18) are not used. Pressing them does nothing.

## BLE protocol

The device exposes a single custom GATT data service. No HID, no other services.

|                            | UUID                                   |
| -------------------------- | -------------------------------------- |
| **Data Service**           | `4c41555a-4465-7669-6365-000000000001` |
| RX Characteristic (write)  | `4c41555a-4465-7669-6365-000000000002` |
| TX Characteristic (notify) | `4c41555a-4465-7669-6365-000000000003` |
| REQ Characteristic (notify)| `4c41555a-4465-7669-6365-000000000004` |

JSON payload format (written to RX):

```json
{ "s": 45, "sr": 120, "w": 28, "wr": 7200, "st": "allowed", "ok": true,
  "dow": 3, "hour": 14, "min": 27, "locked": false }
```

| Field | Meaning |
|---|---|
| `s` / `w` | Session (5h) / weekly (7d) usage % — integer 0..100 |
| `sr` / `wr` | Minutes until session / weekly reset |
| `st` | Anthropic rate-limit status ("allowed" / etc.) |
| `ok` | Daemon parse-success flag |
| `dow` | Host day of week (Mon=0 .. Sun=6) — used by the firmware's work-window check |
| `hour` / `min` | Host local time — used for the work-window check and the deep-sleep wake-time calc |
| `locked` | Windows session lock state — triggers immediate light-sleep on the device |

## How it works

1. The daemon reads your Claude Code OAuth token from a per-platform store (macOS Keychain, `~/.claude/.credentials.json` on Linux, `%USERPROFILE%\.claude\.credentials.json` on Windows).
2. It calls `api.anthropic.com/v1/messages` once a minute — one Haiku token, basically free.
3. The usage numbers come straight out of the response headers (`anthropic-ratelimit-unified-5h-utilization` and friends).
4. The daemon connects to the device over BLE and writes a JSON payload to the GATT RX characteristic.
5. The firmware parses, updates the LVGL dashboard, and uses the timestamp + lock state to manage idle/sleep.

## Power management

- Screen fades to black after **20 min** of no Claude usage change during work hours (07:00-18:00 Mon-Fri), or **immediately** on Windows lock.
- Outside work hours, the device enters real **deep sleep** until the next workday morning. PWR-hold ~7s triggers deep sleep on demand.
- BOOT button or the scheduled timer wake from deep sleep.
- CPU capped at 80 MHz; BLE connection interval pushed long (~300-600 ms idle) for radio quiet time.

## Recompiling fonts

The `firmware/src/font_*.c` files are pre-compiled LVGL bitmap fonts.

```bash
npm install -g lv_font_conv
```

Generate (one at a time — `lv_font_conv` doesn't like loop-driven invocations) with `--no-compress` (required for LVGL 9):

```bash
# Tiempos Text (Usage title, 56px)
lv_font_conv --font assets/TiemposText-400-Regular.otf -r 0x20-0x7E \
  --size 56 --format lvgl --bpp 4 --no-compress \
  -o firmware/src/font_tiempos_56.c --lv-include "lvgl.h"

# Styrene B (large numbers 48, panel labels 28, small text 24, minimal 20)
for size in 48 28 24 20; do
  lv_font_conv --font assets/StyreneB-Regular.otf -r 0x20-0x7E \
    --size $size --format lvgl --bpp 4 --no-compress \
    -o firmware/src/font_styrene_${size}.c --lv-include "lvgl.h"
done
```

**Important:** `lv_font_conv` v1.5.3 outputs LVGL 8 format. Each generated file must be patched for LVGL 9 compatibility:

1. Remove `#if LVGL_VERSION_MAJOR >= 8` guards around `font_dsc` and the font struct.
2. Remove the `.cache` field from `font_dsc`.
3. Add `.release_glyph = NULL`, `.kerning = 0`, `.static_bitmap = 0`, `.fallback = NULL`, `.user_data = NULL` to the font struct.

Without these patches fonts compile but render as invisible.

## Converting Lucide icons

The UI uses a few [Lucide](https://lucide.dev) icons for the battery states.

```bash
node tools/png_to_lvgl.js assets/icon_battery_full.png icon_battery_full_data ICON_BATTERY_FULL_WIDTH ICON_BATTERY_FULL_HEIGHT
```

Default tint is white (`0xFFFFFF`); Lucide PNGs ship as black-on-transparent. Pass `--no-tint` for pre-coloured artwork like the brand logo. Paste the converter output into `firmware/src/icons.h`.

## Credits

- Forked from [HermannBjorgvin/Clawdmeter](https://github.com/HermannBjorgvin/Clawdmeter). Original concept and design by Hermann Bjorgvin and contributors.
- macOS daemon + LaunchAgent originally ported by [Chris Davidson (@lorddavidson)](https://github.com/lorddavidson).
- Windows daemon originally added by [@kvenanzi](https://github.com/kvenanzi).
- Lucide icon set ([lucide.dev](https://lucide.dev), MIT) for battery glyphs.
- Anthropic brand fonts (Tiempos Text, Styrene B) — see licensing note below.

## Licensing gray area warning

The repo uses Anthropic brand assets (proprietary fonts, brand styling). Inherited from the upstream repo. Be aware if you fork or copy the code.
