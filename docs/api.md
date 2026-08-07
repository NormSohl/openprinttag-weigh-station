# HTTP API

The station serves its web app and a small JSON API from the same server on
port 80. Base URL is `http://weighstation.local/` — or the raw IP, which is
worth preferring for anything automated, since mDNS is unreliable across VLANs
and from some Linux hosts. Pin it with a DHCP reservation.

Everything below is stable enough to build against; `state` values come from
`deviceStateName()` in `src/device_state.h`.

---

## Reading

All read endpoints are **open** — no key needed — so dashboards and scrapers
work with no configuration.

| Endpoint | Content-Type | Purpose |
|---|---|---|
| `GET /api/status` | JSON | Everything a dashboard polls: state, spool on the scale, weight, WiFi, storage health |
| `GET /api/spools` | JSON | Every spool: id, uuid, vendor, material, colour, remaining/used grams, onboarding flag |
| `GET /api/usage` | JSON | Consumption per month per vendor+material |
| `GET /usage.csv` | CSV | Same data for spreadsheets and analysis pipelines |
| `GET /api/scale` | JSON | Live load-cell reading + calibration flag |
| `GET /api/storage` | JSON | Log size, free space, compaction due, write-failure flag |
| `GET /export` | NDJSON | The complete raw event log — the full backup artifact |

### `GET /api/status`

```json
{
  "firmware": "1.0.0",
  "state": "present",
  "uptime_s": 84213,
  "heap_free": 142360,
  "scale":   { "weight_g": 1243.5, "calibrated": true },
  "spool":   { "id": 42, "uuid": "e0040108660759df", "vendor": "eSun",
               "material": "PLA+", "color": "1b1b1b",
               "remaining_g": 812.0, "needs_onboarding": false },
  "wifi":    { "mode": "station", "ssid": "Neverhood2",
               "ip": "192.168.50.127", "rssi": -54 },
  "storage": { "spools": 37, "log_bytes": 214880, "log_lines": 1442,
               "usage_rows": 18, "free_bytes": 1680384,
               "compact_due": false, "write_failed": false },
  "auth":    { "required": true }
}
```

`spool` is `null` when nothing is on the scale.

### Polling

`ESPAsyncWebServer` holds few concurrent connections. Poll on the order of
seconds, one client at a time; don't point a multi-worker scraper at it.
`/api/status` exists precisely so a dashboard needs one request, not six.

---

## Writing

These change state and are guarded once an API key is set.

| Endpoint | Effect |
|---|---|
| `POST /api/onboard` | Fill in the spool currently on the scale |
| `POST /api/tare` | Tare for onboarding |
| `POST /api/cal-zero` | Zero the scale |
| `POST /api/cal` | Calibrate against a known weight |
| `POST /api/config` | Replace a config catalog table |
| `POST /api/apikey` | Set or clear the API key (needs the *current* key) |
| `POST /import` | Replace the event log with an uploaded backup |
| `POST /reset` | Erase WiFi credentials and reboot into the setup portal |

`/reset` is POST-only by design. As a GET, any page on the network that merely
linked to the URL could wipe the station's network config just by being loaded
in someone's browser.

---

## Authentication

**No key set means the write endpoints are open.** That is deliberate: the
station lives in a cabinet with no reachable BOOT button, and a device that can
lock out its own operators is worse than one that trusts the LAN. The web app
and the boot log both say so plainly when no key is set.

Set one on the **Config** page, or over serial:

```
APIKEY my-long-random-secret     # set
APIKEY                           # show current
APIKEY none                      # clear — reopens the write endpoints
```

Stored in NVS (namespace `api`), so it survives reflashing the firmware. It is
cleared only by `APIKEY none` or a full flash erase.

Three ways to present it — pick whichever suits the client:

```bash
curl -X POST -H "X-API-Key: SECRET" http://weighstation.local/api/cal-zero
curl -X POST "http://weighstation.local/api/cal-zero?key=SECRET"
curl -X POST -u :SECRET http://weighstation.local/api/cal-zero
```

Basic auth accepts any username; the secret is the password. Browsers prompt
for it natively, which is how the web app's own Save buttons work once a key is
set.

### What this does and does not protect

It stops the realistic failures: a script aimed at the wrong host, someone
poking endpoints out of curiosity, and a browser following a link to a
destructive URL. It is **not** transport security — the key crosses the network
in the clear (Basic auth is base64, which is encoding, not encryption) and
anything that can sniff the LAN can replay it. There is no TLS: an ESP32-S3
serving HTTPS from this much RAM alongside four tasks is not a trade worth
making here. Treat the key as a guard rail, and the LAN as the real security
boundary.

---

## CORS

All responses carry `Access-Control-Allow-Origin: *`, and `OPTIONS` on any path
returns 204, so a browser dashboard on another origin can read the JSON.

The wildcard means browsers **refuse to send cookies or Basic-auth credentials**
cross-origin. That is the intended behaviour: a cross-origin caller that needs
to write must present `X-API-Key` explicitly. Non-browser clients — curl,
Python, Home Assistant, Grafana — are unaffected either way; CORS is enforced
only by browsers.

---

## Examples

Monthly consumption into a spreadsheet, from cron:

```bash
curl -sf "http://192.168.50.127/usage.csv" -o "usage-$(date +%F).csv"
```

Alert if the log stops accepting writes:

```bash
curl -sf http://192.168.50.127/api/status \
  | jq -e '.storage.write_failed | not' >/dev/null \
  || echo "weigh station is not recording events"
```

Full off-device backup — the event log carries current state *and* the
permanent consumption rollup, so this one file is the whole backup:

```bash
curl -sf http://192.168.50.127/export -o events-$(date +%F).ndjson
```
