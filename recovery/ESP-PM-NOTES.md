# ESP-PM Migration — Notes from the Attempted Run

**Status:** abandoned. Not merged to `main`. This branch is the audit trail.

## Goal

Unlock IDF's automatic light-sleep framework (`esp_pm`) on top of the
already-shipped power-optimization work in `main`, to claw back another
~50% of the active-state CPU current (estimated ~10-15 mA at the time).

## What we tried

1. **Rebuild `framework-arduinoespressif32-libs`** with `CONFIG_PM_ENABLE=y`,
   `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`, `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=y`
   baked into `configs/defconfig.common` of `esp32-arduino-lib-builder` running
   in Docker (`espressif/idf:release-v5.5`). Output staged into
   `.local/libs-pm-staging/` (gitignored).
2. **Wire PlatformIO** at the custom libs via
   `platform_packages = framework-arduinoespressif32-libs @ symlink://...`
   (commit `a928d56`). `file:///` URLs hit a Windows path-format bug in PIO.
3. **Call `esp_pm_configure()`** in `setup()` before `setCpuFrequencyMhz(80)`
   (commit `4b0e7f2`). On flash, serial reports `pm_init: err=0` — proves
   the custom libs are linked and PM is configured.

## What didn't work

`esp_pm_dump_locks(stdout)` via the diagnostic `pmlocks` serial command
(commit `04c0cd6`) showed two permanent `NO_LIGHT_SLEEP` locks blocking
the framework from actually engaging light sleep:

```
btLS              NO_LIGHT_SLEEP  active=1   ← NimBLE controller
usb_serial_jtag   NO_LIGHT_SLEEP  active=1   ← USJ driver (host connected)
```

Resolving these would require:

- **`btLS`** — additional sdkconfig flags for BLE modem sleep
  (`CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1`, `CONFIG_BT_CTRL_SLEEP_CLOCK_*`, etc.).
  See Espressif's reference example at
  `examples/bluetooth/nimble/power_save/` for the canonical pattern.
- **`usb_serial_jtag`** — `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION` is correctly
  behaving as documented: it holds the lock *precisely because a host
  is connected*. The only way to release it while USB stays plugged is
  `CONFIG_ESP_CONSOLE_NONE` or routing console to UART
  (`CONFIG_ESP_CONSOLE_UART`). Cost: lose convenient
  `pio device monitor` access; flashing requires the GPIO0+RESET dance.

## Why we stopped: hardware-bound floor

Deep research surfaced that Espressif's published ~230 µA BLE-active idle
for ESP32-S3 **requires an external 32.768 kHz crystal wired to the MCU's
`XTAL_32K_P/N` pins**. Without it, the documented fallback is ~3.3 mA (using
main XTAL as the sleep clock). A clean ESP32-S3-WROOM-1 + crystal + the
textbook NimBLE `power_save` example measured ~15 mA in real-world tests
(esp-idf issue #13073).

**The Waveshare AMOLED-2.16 wires GPIO15 (= ESP32-S3 pin 21 = `XTAL_32K_P`)
to the I2C SDA bus** (`ESP32_SDA / RTC_SDA / TP_SDA` net in the schematic
netlist). The PCF85063 RTC chip has its own crystal on its own pins, not
accessible to the MCU as a sleep clock.

So the MCU light-sleep floor on this board is ~3.3 mA, and the realistic
ceiling under active BLE is more like 5-15 mA. The already-shipped
optimizations on `main` (AMOLED off during idle, 80 MHz CPU cap,
BLE conn-param tuning, low-power PMU polling, scheduled deep sleep)
already put us in that regime — the migration's incremental gain is
~1-5 mA, which doesn't justify the migration cost.

## If anyone revisits this

1. **Don't restart from scratch.** This branch already proves the
   framework-libs override mechanism works. `esp_pm_configure` returns
   `ESP_OK`. The remaining work is BT modem-sleep config + console rework.
2. **Try `arduino-as-component` instead of a full rewrite.** Documented
   middle path — `arduino-esp32 v3.3.9` is compatible with `ESP-IDF v5.5`.
   Implement `app_main()` + `initArduino()`, keep all existing libraries.
   Caveat: `arduino-esp32` issue #8122 reports a Wontfix UART corruption
   bug with `Serial.begin()` + `esp_pm_configure()` — on UART0, not USJ,
   so may not affect this board. Test specifically.
3. **The actual battery-life lever is the AMOLED panel**, not the MCU.
   240 mA active → 17.9 mA modem sleep → 3.3 mA light sleep means the
   biggest win is "screen off." We already do that aggressively via the
   host-lock + data-stall logic on `main`.
4. **Hardware path:** a board variant that wires XTAL_32K_P/N to a
   32.768 kHz crystal (and moves I2C SDA to a different GPIO) would
   shift the floor toward 230 µA and make the software migration
   worth the effort.

## Useful references

- `https://github.com/espressif/esp-idf/blob/master/examples/bluetooth/nimble/power_save/` — canonical pattern.
- `https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-guides/usb-serial-jtag-console.html` — USJ + PM behavior.
- `https://docs.espressif.com/projects/arduino-esp32/en/latest/esp-idf_component.html` — arduino-as-component docs.
- `https://github.com/espressif/esp-idf/issues/13073` — community ~15 mA measurement on a clean DevKit.
- `https://github.com/espressif/arduino-esp32/issues/8122` — Serial.begin + esp_pm Wontfix UART corruption.
