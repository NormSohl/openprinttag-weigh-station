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

> **Windows:** the VS Code route is the path of least resistance — no Python to
> install, no PATH to edit, and no USB driver (see §5). Open the repo folder and
> VS Code will offer the PlatformIO extension automatically (it's recommended in
> `.vscode/extensions.json`). If you use PowerShell directly and `pio` isn't
> found, either use the PlatformIO toolbar buttons instead or add
> `%USERPROFILE%\.platformio\penv\Scripts` to your PATH.

## 2. Get the code

```bash
git clone https://github.com/NormSohl/openprinttag-weigh-station.git
cd openprinttag-weigh-station
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
     together, so the custom `partitions.csv` (single 1.875 MB app + 2 MB
     LittleFS, no OTA) takes effect on the first flash automatically.
   - **Download mode (expect to need this on the very first flash):** the S3's
     native USB won't present a bootloader until it's told to. Hold **BOOT**
     (IO0), tap **RESET** (EN), release **BOOT**, then upload. After one good
     flash the auto-reset works on its own.
   - Optional clean slate the first time: `pio run -t erase` before uploading.

> **Native USB, no bridge chip.** The Thing Plus ESP32-S3 wires USB-C straight
> to the chip's USB — there is **no CP210x/CH340 serial bridge**, so there's no
> vendor driver to install, and the port is **`ttyACM*` / `cu.usbmodem*` /
> `COMx`**, *not* `ttyUSB*`. (If you're hunting for `/dev/ttyUSB0`, that's why
> it's not there.)

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
required for bring-up — but on the *first* power-on it's the single most useful
instrument (see the expected output below). `pio device list` prints the port.

If the monitor is blank after a successful upload, it's the ESP32-S3 native-USB
CDC quirk: add `-DARDUINO_USB_CDC_ON_BOOT=1` to `build_flags` in
`platformio.ini` (depends on the board's USB wiring).

### Serial port access & drivers (per OS)

No driver install — native USB is class-compliant CDC. The only per-OS bit is
**permissions / a couple of well-known gotchas**:

- **Linux** — add yourself to the serial group once, then log out/in:
  ```bash
  sudo usermod -aG dialout $USER      # (or `uucp` on Arch)
  ```
  If the port appears then **vanishes after ~1 s**, the culprit is almost always
  `brltty` (a screen-reader daemon that grabs ACM devices):
  `sudo apt remove brltty`. Port is `/dev/ttyACM0`.
- **macOS** — nothing to install; port is `/dev/cu.usbmodemXXXX`. (Only a
  *bridge* board would need the SiLabs VCP driver — this one doesn't.)
- **Windows** — Win10/11 install the USB-CDC class driver automatically; it
  shows up as `COMx` in Device Manager. No Zadig needed for flashing/monitoring
  (Zadig is only for the separate JTAG interface, which we don't use).

## 6. Editor automation (VS Code)

Committed in `.vscode/` so it works the moment you open the folder:

| Task | What | How |
|---|---|---|
| **Flash & Monitor** | build → upload → serial monitor in one go | **Ctrl+Shift+B** (default build task) |
| Build / Flash / Monitor | the individual steps | Ctrl+Shift+P → *Run Task* |
| List serial ports | `pio device list` — find the COM port | Ctrl+Shift+P → *Run Task* |
| Erase flash | clean slate; **wipes LittleFS** (log, config, WiFi, calibration) | Ctrl+Shift+P → *Run Task* |

Build errors are parsed by the `$gcc` problem matcher, so compiler messages
become clickable file:line links in the Problems panel.

**Crash decoding is on.** `monitor_filters` in `platformio.ini` enables
`esp32_exception_decoder`, which rewrites a panic backtrace from raw addresses
into function + file:line — so a first-boot crash reads as
`nfcTask() at src/nfc_task.cpp:112`, not `Backtrace: 0x42008f3a 0x3ffb2280`.
Lines are also timestamped (`time`).

## First-boot bring-up (no PC needed after flashing)

> **First power-on: leave the microSD slot empty and keep the serial monitor
> open.** SD is the only brand-new, unvalidated wiring — it's fail-safe (boots
> fine with no card), so keeping it out means it can't muddy the first log. Add
> the card once the core is proven.

**What a healthy boot prints.** The subsystems come up as independent tasks, so
even a dead peripheral won't hide the others — read the banner to see who's
alive:

```
[store] ready: 0 spools, 0 log lines, next id #1
[cfg]   ready: N vendors, N materials, N profiles, N colors, N stock
[sd]    no card (backup to SD disabled until inserted)
*wm:AutoConnect ...            <- WiFiManager (portal or join)
```

Then the TFT lights and shows `WiFi Setup` (or the idle screen once joined). A
missing line points straight at the section to drill into — e.g. no `[cfg]`
means LittleFS didn't mount; a hang right after `SPI.begin` points at the shared
PN5180/TFT bus.

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

The partition map targets **4 MB** flash (ESP32-S3-MINI-1-N4R2), verified on the board. Re-check if you swap boards:
relying on it:

```bash
pio pkg exec -- esptool.py flash_id   # expect "Embedded Flash 4MB", "PSRAM 2MB"
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
