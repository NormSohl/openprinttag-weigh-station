# tools/sim/ — the web app on your computer

Runs the weigh station's **real** web app on a Mac or Linux PC, so web
changes can be built, clicked through and tested without touching the
station in production. Then flash the station once, when the change is done.

```bash
tools/sim/run.sh        # build, then serve http://localhost:8080/
tools/sim/test.sh       # browser tests against fresh simulators
```

Open **http://localhost:8080/_sim** for the control panel: the web app on the
right, simulated hardware on the left (place a spool, place a blank tag,
remove it, run serial commands like `SEED 12 20`).

## What it is

Not a copy of the pages. `run.sh` compiles the firmware's own sources —
`src/web_app.cpp`, `store.cpp`, `config_store.cpp`, `api_key.cpp`,
`display_tz.cpp`, `station_name.cpp`, `last_onboard.cpp`, `last_stock.cpp`,
`opt_tag.cpp` — against small stand-ins for the ESP32 libraries:

| Stand-in | Replaces | Where |
|---|---|---|
| ESPAsyncWebServer over [cpp-httplib](https://github.com/yhirose/cpp-httplib) (pinned `v0.60.1`) | the device's web server, incl. SSE (`/events`) and uploads | `shim/ESPAsyncWebServer.h`, `sim_server.cpp` |
| LittleFS, Preferences (NVS), String, Serial | flash storage, settings | `../store/shim/` (shared with the store tests) |
| WiFi, ESP, mDNS | radio, reboot, heap | `shim/WiFi.h`, `shim/ESPmDNS.h` |
| scale / NFC / sync / controller tasks | the hardware | `sim_main.cpp` |

The web server stand-in keeps the device's rules where they have bitten
before: route matching is prefix-based and first-registered-wins (so the
`/config` vs `/config/export` ordering gotcha reproduces — a test checks it,
and was seen to fail when the order was swapped), query and POST parameters
stay separate, handlers run one at a time, and a handler that sends no
response is reported instead of hanging.

The simulated hardware makes the same store calls `syncTask` does: placing
a spool weighs it with the store's tare and logs a Weigh event; placing a
blank tag mints a stub exactly like the Resolving phase. A tag write
(`gWriteMainPending`) is acknowledged and logged as `[sim] tag Main written`.

Data lives in `tools/sim/data/` (or `SIM_DATA=…`) and survives restarts, the
way flash survives a power cycle. Delete the folder to start from a fresh
station.

## Production data

Both backups are open reads on the station and do not interrupt weighing:

```bash
curl -o weighstation-log.ndjson  http://weighstation.local/export
curl -o weighstation-config.json http://weighstation.local/config/export
tools/sim/load.sh weighstation-log.ndjson weighstation-config.json
```

`load.sh` goes through the same `/import` and `/config/import` handlers as
the Backup page. Keep production backups out of the repo.

## Tests

`tests/` is pytest + Playwright (pinned 1.48.0, Chromium, installed into
`.deps/venv` by `test.sh` on first run). Each test starts its own simulator
on a free port with an empty data folder. Outside network is blocked, so the
OpenPrintTag catalog search (which fetches from GitHub in the browser) does
not make tests depend on the internet.

`conftest.py`'s `scan(page, code, gap_ms)` types a barcode the way a
keyboard-wedge scanner does — digits a few ms apart, then Enter — for the
barcode work in `docs/design/barcode-onboarding.md`.

## What it cannot tell you

- Anything about the ESP32 itself: heap, stack depth, how many connections
  the real server holds at once (see the SSE gotcha in CLAUDE.md), timing.
- Real tag reads/writes, the load cell, the display.
- `pio run` still has to compile it for the device, and changes that touch
  the hardware tasks still need the bench.
