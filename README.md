# Filliment Scale

Firmware for the Seattle Makers filament weigh station — a **self-contained**
NFC + load-cell scale for the 3D-printing lab.

Place a spool on the scale: it reads the OpenPrintTag (OPT) NFC tag, weighs the
spool, and records the remaining filament **locally on the device** — no external
server. A built-in web app handles inventory, onboarding new spools, reordering,
backup/restore, and scale calibration; usage history is kept as an append-only
event log in on-device flash.

- **Hardware:** SparkFun Thing Plus ESP32-S3, PN5180 NFC (ISO15693), NAU7802
  load-cell ADC, 3.5" ILI9488 SPI TFT (480×320), passive piezo buzzer, onboard
  WS2812 NeoPixel.
- **Firmware:** PlatformIO + Arduino, FreeRTOS tasks (NFC / scale / display /
  sync + web app). Local storage on LittleFS + NVS.

## Docs

- [`DEVELOPMENT.md`](DEVELOPMENT.md) — set up, build, and flash
- [`docs/user-manual.md`](docs/user-manual.md) — operation (members + admins)
- [`hardware/netlist.md`](hardware/netlist.md) — wiring diagram + pinout
- [`docs/design/sd-local-ecosystem.md`](docs/design/sd-local-ecosystem.md) — the
  local-storage architecture

> **History:** this project originally synced to Spoolman (and kept history in
> Prometheus). It is now standalone — Spoolman and Prometheus have been removed
> and replaced by on-device storage and the built-in web app. The repo name still
> reflects the original design.
