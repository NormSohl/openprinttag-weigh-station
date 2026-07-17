# Development & flashing

This is a [PlatformIO](https://platformio.org/) project. The toolchain, the
ESP32 platform, and every library install themselves on the first build — you
don't hand-install any of that. The same build runs in CI on every push, so a
green local build should match CI.

## 1. Install the tools

**Recommended — VS Code + PlatformIO:**

1. Install [VS Code](https://code.visualstudio.com/).
2. Install [Git](https://git-scm.com/downloads).
3. In VS Code → Extensions → search **"PlatformIO IDE"** → Install. It bundles
   its own Python and the `pio` CLI — nothing else to set up.

**Or CLI-only:** install
[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html)
(`pip install platformio`, or the standalone installer).

## 2. Get the code

```bash
git clone https://github.com/NormSohl/Filliment-Scale-with-OpenPrintTag-RFID-tracking-and-Spoolman-integration.git
cd Filliment-Scale-with-OpenPrintTag-RFID-tracking-and-Spoolman-integration
```

## 3. Build

The first build downloads everything (the `espressif32` platform, the GCC
toolchain, and all `lib_deps`: PN5180, NAU7802, TFT_eSPI, ArduinoJson,
WiFiManager, AsyncTCP, ESPAsyncWebServer, tinycbor, …).

- **VS Code:** open the folder; PlatformIO auto-detects `platformio.ini`. Click
  the **✓ (Build)** button in the bottom status bar.
- **CLI:** `pio run`

## 4. Connect the board and flash

The target is a **SparkFun Thing Plus ESP32-S3** (`sparkfun_esp32s3_thing_plus`),
flashed over USB-C.

1. Plug the board into USB-C.
2. Upload: the VS Code **→ (Upload)** button, or `pio run -t upload`.
   - A normal upload writes the **bootloader + partition table + app**
     together, so the custom `partitions.csv` (single 3 MB app + 4.88 MB
     LittleFS, no OTA) takes effect on the first flash automatically.
   - If auto-reset doesn't catch (common on the very first flash): hold
     **BOOT**, tap **RESET**, release **BOOT**, then upload again.
   - Optional clean slate the first time: `pio run -t erase` before uploading.

**No filesystem image to upload.** Config tables seed themselves at first boot
and the web UI is embedded in the firmware, so there is no `uploadfs` step — a
plain upload is complete.

## 5. Serial monitor (optional)

```bash
pio device monitor -b 115200
```

For boot logs and the capacity/test harness (`SEED <spools> <events>`, `DUMP`,
`LOGSTATS`, `REBUILD`, …). WiFi setup is shown on the TFT and
calibration/onboarding are done in the browser, so the serial console is **not**
required for bring-up.

If the monitor is blank after a successful upload, it's the ESP32-S3 native-USB
CDC quirk: add `-DARDUINO_USB_CDC_ON_BOOT=1` to `build_flags` in
`platformio.ini` (depends on the board's USB wiring).

## First-boot bring-up (no PC needed after flashing)

1. **WiFi:** on first boot the TFT shows `WiFi Setup` — join the
   `WeighStation-Setup` network and enter the lab WiFi. After that it runs in
   plain station mode at `http://weighstation.local` (no access point). If it
   can't reach the network it falls back to the `WeighStation` SoftAP.
2. **Find the UI:** the idle screen shows the web address.
3. **Calibrate:** open the web UI → **Calibrate** → *Zero* (empty scale) then
   *Set calibration* (known weight + its grams). The "not calibrated" banner
   clears when done. (Serial `ZERO` / `CAL <grams>` still work as a fallback.)
4. **Onboard a spool:** place a blank tag → 5-second confirm countdown → a stub
   is created → fill in the details at **Onboard**.

## Verifying the flash size

The partition map assumes **8 MB** flash (ESP32-S3-MINI-1-N8R2). Confirm before
relying on it:

```bash
pio pkg exec -- esptool.py flash_id      # look for "Detected flash size: 8MB"
```

## Project layout

| Path | What |
|---|---|
| `src/main.cpp` | task setup + shared globals |
| `src/nfc_task.cpp` | PN5180 / OPT tag read + write |
| `src/scale_task.cpp` | NAU7802 load cell + calibration |
| `src/sync_task.cpp` | state machine, WiFi/AP, tag ⇄ store reconcile |
| `src/web_app.cpp` | built-in web UI (inventory, onboard, reorder, calibrate, history, backup) |
| `src/store.*` | append-only event log + indices (LittleFS) |
| `src/config_store.*` | onboarding catalog tables |
| `src/display_task.cpp` | TFT + buzzer + NeoPixel |
| `include/User_Setup.h` | TFT_eSPI config (force-included via `platformio.ini`) |
| `partitions.csv` | flash layout |
| `docs/design/` | architecture + implementation plan (+ post-bringup backlog) |
