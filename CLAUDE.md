# Project context

ESP32-S3 firmware for a desk-side Claude Code usage monitor. **This is a
display-only personal fork** of the original Clawdmeter — touch input,
splash animations, BLE HID keystrokes, and the alternative board ports
were all stripped out. What's left is a single board showing two
utilization bars updated over BLE.

Read this first; future Claude Code sessions should boot from here.

## Hardware (only supported board)

**Waveshare ESP32-S3-Touch-AMOLED-2.16** — CO5300 480×480 square AMOLED, AXP2101 PMU, QMI8658 IMU. CST9220 touch and the right side button (GPIO 18) are physically present on the board but the firmware doesn't drive them.

Pins:
- **Display:** CO5300 over QSPI (CS=12, SCLK=38, SDIO0..3=4..7, RST=2)
- **I2C bus:** SDA=15, SCL=14 (AXP2101 @ 0x34, QMI8658 @ 0x6B)
- **PWR button:** routed to AXP2101 PKEY; events read via I2C polling
- **BOOT button:** GPIO 0 (also the wake-from-deep-sleep RTC GPIO)

## Architecture

```text
firmware/src/
  hal/                      — runtime abstractions (kept around even though only one board now)
    board_caps.h            — runtime BoardCaps struct (W, H, has_battery, etc.)
    display_hal.h           — init / begin / set_brightness / draw_bitmap / tick / round_area / enter_sleep / exit_sleep
    input_hal.h             — primary-button (BOOT) is_held query for wake-on-press
    power_hal.h             — AXP2101 polling + enter_deep_sleep with EXT1 wake config
    imu_hal.h               — QMI8658 init only (rotation logic is dead code now)
  boards/waveshare_amoled_216/   — the one and only board folder
    board.h                 — pins, I2C addresses
    board_init.cpp          — Wire.begin
    display.cpp             — Arduino_CO5300 + software rotation for partial render
    power.cpp               — AXP2101 driver, AXP_POWEROFF_10S threshold, low-power polling mode
    imu.cpp                 — QMI8658 init (rotation disabled — see imu_hal_tick)
    input.cpp               — BOOT button polling
    caps.cpp                — the single BoardCaps instance
  main.cpp                  — setup() + loop(): LVGL bring-up, BLE polling, button polling, pair_tick, deep_sleep_tick
  ui.{h,cpp}                — single usage view: logo, "Usage" title, battery, two panels (Current/Weekly), pair hint when disconnected
  ble.{h,cpp}               — NimBLE peripheral, single connection slot, custom GATT data service only (no HID)
  idle.{h,cpp}              — state machine: AWAKE → FADING_OUT → ASLEEP or LIGHT_SLEEP_IDLE → FADING_IN. Lock + data-stall + scheduled-deep-sleep + manual-deep-sleep entry points. Pure helper idle_seconds_to_next_work_window.
  idle_cfg.h                — timing constants: 30-min user-input timeout (unused-but-kept), 20-min data-stall, 07:00-18:00 Mon-Fri work window
  brightness.{h,cpp}        — 4-step ramp persisted to NVS via Arduino Preferences
  data.h                    — UsageData struct (s, w, sr, wr, st, ok, dow, hour, min, host_locked)
  logo.h, icons.h           — RGB565[A8] image data for brand logo + battery glyphs
  font_*.c                  — pre-compiled LVGL 9 bitmap fonts (Tiempos 56, Styrene 48/28/24/20/16/14/12)
```

## Build / flash

```bash
pio run -d firmware -e waveshare_amoled_216                                     # build
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port COM3        # flash on Windows
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/ttyACM0    # Linux
pio run -d firmware -e waveshare_amoled_216 -t upload --upload-port /dev/cu.usbmodem101   # macOS
```

If `pio` isn't on PATH: try `~/.platformio/penv/bin/pio` (Linux/macOS install) or `pip install --user platformio` on Windows.

Recovery: `recovery/restore.ps1` reflashes the newest `*.factory.bin` snapshot in that folder via esptool. Each significant build gets snapshotted there for rollback.

## QA your own UI changes

The firmware ships a `screenshot` serial command that dumps the LVGL framebuffer. Read directly via PowerShell:

```powershell
$port = New-Object System.IO.Ports.SerialPort COM3, 115200, None, 8, One
$port.ReadTimeout = 10000; $port.Open()
$port.WriteLine("screenshot")
# read framebuffer (480×480 RGB565 = 460800 bytes) + convert via ffmpeg
$port.Close()
```

There's no `.sh` for Windows — adapt the snippet. Then `ffmpeg -y -f rawvideo -pixel_format rgb565le -video_size 480x480 -i out.raw -update 1 -frames:v 1 out.png` produces a PNG. Read the PNG with the Read tool to verify visual changes.

The device boots straight to the usage view (no splash anymore), so a fresh flash + screenshot will show the usage view directly.

## Critical gotchas

1. **CO5300 cannot rotate.** Its MADCTL only supports axis flips, not column/row exchange. Rotation (if re-enabled) is done by **CPU pixel remapping inside `display_hal_draw_bitmap`** in `boards/waveshare_amoled_216/display.cpp`. Partial render with 480×40 strips. IMU-driven rotation logic is currently dead code (imu_hal_tick is a no-op).
2. **OPI PSRAM** required: `board_build.arduino.memory_type = qio_opi` in platformio.ini. Without this, `MALLOC_CAP_SPIRAM` returns NULL and the screen is black.
3. **pioarduino platform required.** GFX Library for Arduino needs Arduino Core 3.x (`esp32-hal-periman.h`). Pinned to `pioarduino/platform-espressif32` 55.03.38-1.
4. **LVGL 9 font patching.** `lv_font_conv` outputs LVGL 8 format. Each generated font file must be patched: remove `#if LVGL_VERSION_MAJOR >= 8` guards, drop `.cache`, add `.release_glyph`, `.kerning`, `.static_bitmap`, `.fallback`, `.user_data`.
5. **Even-aligned flush regions.** `display_hal_round_area` enforces this — required on CO5300.
6. **LVGL RGB565A8 is planar.** `w*h` RGB565 pixels followed by `w*h` alpha bytes; `data_size = w*h*3`, `stride = w*2`. Used for the battery icons (alpha over panel background).
7. **NVS is wiped on every factory.bin flash.** The factory image covers offsets 0x0..~1.3MB which crosses the NVS partition at 0x9000-0xE000. Bonds (BLE) and saved brightness reset. Re-pair via Windows after a fresh flash.
8. **AXP force-shutdown at 10s.** PWR-hold layers: 3s → pair-arm, 6s → pair-disarm, 7s → manual deep sleep, 10s → AXP cuts power. The 7s gesture has a 3s tolerance window before the hardware shutdown.
9. **GPIO15 = I2C SDA.** XTAL_32K_P pin is consumed by I2C — no external 32 kHz crystal possible without rerouting. Caps the MCU light-sleep floor at ~3.3 mA. See the abandoned `esp-pm-enable` branch + `recovery/ESP-PM-NOTES.md` for the full investigation.

## Icons

`tools/png_to_lvgl.js <input.png> <symbol> [W_MACRO] [H_MACRO] [--tint=RRGGBB | --no-tint]` converts an alpha PNG to RGB565A8. Default tint is white (`0xFFFFFF`) — necessary for Lucide PNGs. Splice output into `firmware/src/icons.h`. Currently the 5 battery icons use this format.

## User profile / preferences

See `~/.claude/projects/.../memory/` files for persistent context. Always read those memory files at session start.

## Daemon / host side

Python tray daemon under `daemon/`. Windows is the primary target (`daemon/claude_usage_daemon_windows.py` + `daemon/tray_windows.py`). macOS and Linux variants exist in the same folder.

**Windows daemon:**
- `pystray` system-tray app with live status icon (green Connected / amber Scanning / red Error).
- Polls Anthropic every 60s, writes JSON to the BLE RX characteristic.
- `daemon/host_lock_windows.py` listens for `WM_WTSSESSION_CHANGE` (Win+L / unlock) and triggers an immediate BLE refresh so the device knows to light-sleep within ~1s, not 60s.
- Bonded-device fallback in `_paired_address_from_registry` — reads the device's BD_ADDR from the BTHLE PnP entries when BleakScanner can't see the (paired-and-not-advertising) device.

**BLE GATT characteristics on service `4c41555a-...0001`:**

- `...0002` RX — daemon writes JSON usage payload here.
- `...0003` TX — firmware notifies ack/nack (daemon doesn't subscribe).
- `...0004` REQ — firmware fires `0x01` notify in `onSubscribe` if `has_received_data` is false.

**JSON payload schema:**

```json
{ "s": 45, "sr": 120, "w": 28, "wr": 7200,
  "st": "allowed", "ok": true,
  "dow": 3, "hour": 14, "min": 27, "locked": false }
```

`dow` is `datetime.weekday()` (Mon=0..Sun=6); the firmware uses it + `hour` for the 07:00-18:00 Mon-Fri work-window check and for the `idle_seconds_to_next_work_window` deep-sleep timer math.

## Recent session highlights

- **Display-only simplification (2026-06-05).** Stripped touch, BLE HID, splash + animation engine, alternative board ports, and porting docs. ~97,600 LOC delete. Single usage view, single connection slot, single supported board.
- **Light-sleep + deep-sleep + lock-aware idle (2026-06-04).** AMOLED-off via CO5300 SLPIN, 80 MHz CPU cap, BLE conn-interval tuning, scheduled deep sleep outside work hours, BOOT-button RTC GPIO wake, manual 7s PWR-hold gesture, host-lock-driven instant light sleep.
- **Abandoned `esp-pm-enable` branch (2026-06-05).** Tried unlocking `esp_pm` auto light sleep via a custom framework-libs rebuild. `esp_pm_configure` returns ESP_OK but two NO_LIGHT_SLEEP locks (NimBLE + USB-Serial-JTAG) prevent actual engagement. Hardware floor on this board is ~3.3 mA due to missing external 32 kHz crystal (GPIO15 = I2C SDA). See `recovery/ESP-PM-NOTES.md` on that branch.
