# Weigh Station — User Manual

The Weigh Station is a self-contained filament inventory tool for the Seattle
Makers 3D-printing lab. Place a spool on the scale; it reads the NFC tag, weighs
the spool, and records the remaining filament **on the device**. A built-in web
app (`http://weighstation.local/`) shows inventory and history, onboards new
spools, flags reorders, and takes backups. There is no external server.

---

## For Members: Weighing a Spool

1. **Place the spool on the scale.** The NFC reader and load cell activate
   immediately — no button press needed.
2. **Wait for the display to settle.** It shows the spool number, material, and
   remaining weight for a second or two.
3. **Remove the spool.** The display returns to the idle screen.

That's it — the reading is saved locally, and a weigh entry is added to the
spool's history.

### What the display shows

The 3.5" screen shows one of these, depending on what's happening:

| Display | Meaning |
|---|---|
| <img src="images/lcd/idle.svg" width="300" alt="Idle screen: Seattle Makers, Place spool to begin, Web app URL"> | **Idle, ready** — place a spool to begin |
| <img src="images/lcd/weighing.svg" width="300" alt="Weighing screen showing grams"> | **Weighing** — reading the load cell |
| <img src="images/lcd/present.svg" width="300" alt="Present screen: spool number, material, grams remaining, Saved locally"> | **Done** — spool number, material, and remaining weight recorded locally |
| <img src="images/lcd/registered.svg" width="300" alt="Registered screen prompting to add details in the web app"> | **New spool registered** — fill in the material details in the web app (see *New Spools* below) |
| <img src="images/lcd/updating-tag.svg" width="300" alt="Updating tag screen"> | **Updating tag** — writing updated filament data back to the NFC tag |
| <img src="images/lcd/idle-uncalibrated.svg" width="300" alt="Idle screen with a Scale not calibrated warning"> | **Not calibrated** — shown on idle until the scale is calibrated (weights are unreliable until then) |

> **Opening the web app from your phone.** The idle screen shows a **QR code** on
> the right — scan it with the camera and it opens the app directly. The address
> is also printed as text: `weighstation.local` for typing, with the numeric IP
> underneath as a fallback. Use the numeric one if `.local` doesn't resolve,
> which happens on some Android browsers and across some networks. (The QR
> always encodes the numeric address, for exactly that reason.)
>
> *The screenshots above predate the QR code and show the older single-line
> address.*

### Status light (NeoPixel)

| Color | Meaning |
|---|---|
| Dim green | Idle, ready |
| Blue | Working (WiFi setup, weighing, writing tag) |
| Green | Spool weighed and saved |
| Yellow | Spool registered but needs details entered in the web app |
| Amber (blinking, accelerating) | New-tag countdown — remove the spool to cancel |
| Red | Tag read error — reposition the spool |

---

## For Members: New Spools

When you place a spool with a **blank NFC tag** on the scale:

1. The display shows a **5-second countdown** — "New tag found / Remove to
   cancel / Registering in: 5…"

   <img src="images/lcd/new-tag-countdown.svg" width="300" alt="New tag found screen with a large countdown digit">

2. **Remove the spool within 5 seconds** to cancel, or **leave it in place** to
   proceed.
3. The station formats the tag and creates a placeholder record on the device.
   The display shows `Registered! / Spool #N / Add details in web app`.

   <img src="images/lcd/registered.svg" width="300" alt="Registered screen prompting to add details in the web app">

4. **Open the web app and fill in the material details** for that spool — see
   *Onboarding a spool* below.

Once the details are saved, the NFC tag is written with the real data — either
right away if the spool is still on the scale, or the next time it's placed.

### Onboarding a spool (web app)

1. With the spool on the scale, browse to `http://weighstation.local/` on any
   phone or laptop on the same network.
2. Open **Onboard**. Pick the **vendor**, **material**, **color**, and **spool
   profile** (the profile fills in the empty-spool tare and nominal weight). You
   can also capture the tare from a matching empty spool with **Capture tare**.
3. **Save & write tag.** The full OpenPrintTag data (identity, print temps,
   weights) is written to the tag and the record is saved.

### Applying an NFC tag to a new spool

Use an OpenPrintTag MK1 sticker. Peel and apply to the flat hub face of the
spool — away from the filament windings, centered so it sits over the NFC reader
when placed on the scale. Press firmly for 5 seconds.

**Do not reuse tags.** Peeling a tag risks cracking the foil antenna invisibly.
New tag for every new spool.

### Pre-tagged spools (e.g. Prusament)

Some spools arrive with an NFC tag already applied (genuine Prusament, or spools
tagged by another maker's setup). The station recognizes these as legitimate and
creates a local record from the data already on the tag — no countdown, no
blank-tag flow. It then weighs and records normally.

---

## For Administrators: Initial Setup

Full build/flash steps are in [`DEVELOPMENT.md`](../DEVELOPMENT.md); a short
version follows.

### 1. Hardware assembly

Wire the components per [`hardware/netlist.md`](../hardware/netlist.md). The
NAU7802 connects via a Qwiic cable; the PN5180 NFC module and the ILI9488 TFT
display share the SPI bus.

### 2. Flash the firmware

```bash
pio run --target upload
```

### 3. First-time WiFi setup

On first boot the display shows the WiFi setup screen:

<img src="images/lcd/wifi-setup.svg" width="300" alt="WiFi Setup screen: Join network WeighStation-Setup, then open browser to 192.168.4.1">

1. On any phone or laptop, join the `WeighStation-Setup` WiFi network.
2. A captive-portal page opens automatically (or browse to `192.168.4.1`).
3. Select your WiFi network and enter the password.
4. Submit. The device connects to the lab network and the display shows the idle
   screen with its address (`http://weighstation.local/`).

If no network is configured (or it can't be reached), the station falls back to
its own `WeighStation` access point so the web app stays reachable — the idle
screen shows the SSID and address to use.

### 4. Scale calibration (web)

The scale reads raw counts until calibrated; the idle screen shows **"Scale not
calibrated"** until you do this:

<img src="images/lcd/idle-uncalibrated.svg" width="300" alt="Idle screen with a Scale not calibrated warning">

1. With the spool on/off as directed, browse to `http://weighstation.local/` and
   open **Calibrate**.
2. **Step 1 — Zero:** clear the scale, then click **Zero (tare)**.
3. **Step 2 — Calibrate:** place a known weight, type its exact mass in grams,
   then **Set calibration**.

The banner clears when it takes. (Serial `ZERO` / `CAL <grams>` at 115200 baud
still work as a fallback.)

---

## For Administrators: Ongoing Maintenance

### The web app

`http://weighstation.local/` provides:

- **Inventory** — remaining filament by material; the spool currently on the scale.
- **Spools** — the full list; click a spool for its weigh history + a remaining-over-time sparkline (with CSV export).
- **Onboard** — fill in details for a new/blank spool.
- **Usage** — how much filament the lab actually consumes, per material and per month; CSV download.
- **Reorder** — standard-stock items at or below threshold; download a CSV to place the order.
- **Config** — edit the vendor/material/color/spool-profile/stock-item tables.
- **Backup** — download the event log; restore from an uploaded backup.
- **Calibrate** — the scale calibration workflow above.

### Changing the WiFi network

**Option A — web (device currently connected):** browse to
`http://weighstation.local/reset` and press **Reset WiFi**. The device reboots
and opens the `WeighStation-Setup` AP; connect and reconfigure as in initial
setup. (Visiting the page does nothing on its own — the button is what triggers
it, so a stray link can't wipe the network config. If an API key is set, you'll
be asked for it.)

**Option B — power cycle (device unreachable):** power-cycle it. If stored
credentials fail, the captive portal opens for **2 minutes** — connect within
that window and reconfigure. **Once your phone joins the AP the countdown
stops**, so there's no time pressure while typing the password. The portal does
close after **10 minutes** no matter what, so an abandoned session can't leave
it open forever — just power-cycle to reopen it. (Holding the BOOT button for 3 s at power-on also
clears WiFi credentials.)

### Recalibrating / backups

Recalibrate any time via the **Calibrate** page.

**Back up by downloading the event log** from the **Backup** page — that one
file is the whole backup. It carries current spool state *and* the permanent
consumption totals, and **Restore** puts it back. There is no SD card: the
ESP32-S3 ran out of SPI peripherals, so the slot on the display board is not
wired and nothing is saved to it.

Download a copy whenever you'd mind losing the data — before a firmware
update, before a device swap, or on a monthly reminder. It can be automated;
see the API section below.

**About storage limits.** The log is append-only and the device has about 2 MB
for it. Long before that fills, the station compacts the log on its own while
nothing is on the scale: each spool's history collapses to a single checkpoint
and recent events are kept as-is. **Monthly consumption totals are kept forever
and are never affected.** What compaction gives up is individual weigh readings
older than the retained tail — so if you want the complete weigh-by-weigh
record long-term, download the log periodically. The **Backup** page shows the
current log size and warns you if writes ever start failing.

---

## For Administrators: HTTP API

Other programs on the network can read from the station directly — a dashboard,
a spreadsheet job, a monitoring script. Everything is plain HTTP on the same
address as the web app. Full reference: [`api.md`](./api.md).

### Reading data (no setup needed)

Read-only endpoints are open, so nothing needs configuring first:

| URL | What you get |
|---|---|
| `/usage.csv` | Consumption per month per vendor + material — a spreadsheet file |
| `/export` | The complete event log (the same file the Backup page downloads) |
| `/api/status` | Everything at a glance: what the station is doing, what's on the scale, WiFi, storage health |
| `/api/spools` | Every spool and its remaining filament, as JSON |
| `/api/usage` | The same data as `/usage.csv`, as JSON |

Automated monthly consumption pull:

```bash
curl -sf "http://weighstation.local/usage.csv" -o "usage-$(date +%F).csv"
```

Nightly off-device backup, which is the same thing the Backup page gives you:

```bash
curl -sf "http://weighstation.local/export" -o "events-$(date +%F).ndjson"
```

> **Use the IP address, not `weighstation.local`, for anything scheduled.**
> The `.local` name relies on mDNS, which is unreliable from some machines and
> across network segments. Give the station a fixed address in your router's
> DHCP reservations and use that.

> **Poll gently** — every few seconds at most, one program at a time. The
> station is a microcontroller and handles only a handful of connections.
> `/api/status` exists so a dashboard can get everything in one request.

### Protecting the write endpoints

By default **anything on the network can change the station** — onboard spools,
edit the config tables, recalibrate the scale, replace the event log, reset
WiFi. On a trusted lab network that's usually fine and keeps things simple.

To require a password for those actions, set an **API key** on the **Config**
page (bottom of the page, *API access*). Reading stays open; only changes need
the key. The web app will prompt you for it the first time you save something.

Scripts pass it in whichever way suits them:

```bash
curl -X POST -H "X-API-Key: YOURKEY" http://weighstation.local/api/cal-zero
curl -X POST -u :YOURKEY http://weighstation.local/api/cal-zero
```

**If you lose the key,** connect a USB cable, open a serial monitor at 115200
baud, and type:

```
APIKEY none
```

That clears it and reopens the write endpoints. It is the only way back —
changing the key from the web app requires the current one — so the USB port is
worth keeping accessible. `APIKEY` on its own shows the current key.

**What the key is and isn't.** It stops mistakes and casual poking: a script
aimed at the wrong machine, someone exploring endpoints, a browser following a
link to something destructive. It is **not** encryption — the key travels over
the network in the clear, so anyone able to watch lab traffic could capture it.
Treat the network itself as the real boundary, and don't put the station
somewhere a guest network can reach it.

---

## Troubleshooting

### Device won't connect to WiFi

- Power-cycle the device and connect to `WeighStation-Setup` within 2 minutes
  (the countdown pauses as soon as you join, so take your time on the form;
  the portal closes after 10 minutes regardless) to reconfigure.
- If the SSID list in the portal is empty, wait ~30 seconds for the scan and
  refresh the page.

### Tag stuck on "Read error" after a failed registration

If registering a new tag failed partway, the tag is left half-written: it is no
longer blank, but it isn't valid OpenPrintTag data either, so the station shows
**Read Error** every time and there is no way back through the normal flow.

Recover it over USB serial (115200 baud):

```
TAGFORMAT
```

Then lift the spool and place it again. The station rewrites the tag from
scratch and registers it normally.

This is deliberately a manual command rather than something automatic — a tag
the station can't read might be a legitimate tag in a format it doesn't
understand, and reformatting is irreversible.

### NFC tag not reading ("Read error")

- Reposition the spool so the tag is centered over the reader.
- Check that the tag sticker is fully adhered with no air bubbles near the center.
- If the error persists, the tag antenna may be damaged — replace the tag.

### Scale reads zero or wildly wrong values

- Recalibrate: open **Calibrate**, **Zero** with the platform clear, then
  **Set calibration** with a known weight.
- Check that the load cell wiring hasn't shifted (particularly E+ / E− polarity).

### Display is blank

- Check the TFT's SPI wiring — CS (GPIO 15), DC (GPIO 16), RST (GPIO 17), and the
  shared MOSI/SCK.
- Make sure the backlight (`LED` pin) is powered (tied to 3.3 V) — the panel
  shows nothing without it.
