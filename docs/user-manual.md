# Weigh Station — User Manual

![The assembled weigh station on the workbench](images/device-full.jpg)

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

![The idle screen in person: Seattle Makers, Place spool to begin, the web app address, and a QR code](images/device-idle-screen.jpg)

The 3.5" screen shows one of these, depending on what's happening:

| Display | Meaning |
|---|---|
| <img src="images/lcd/idle.svg" width="300" alt="Idle screen: Seattle Makers, Place spool to begin, web app address, and a QR code"> | **Idle, ready** — place a spool to begin |
| <img src="images/lcd/weighing.svg" width="300" alt="Weighing screen showing grams"> | **Weighing** — reading the load cell |
| <img src="images/lcd/present.svg" width="300" alt="Present screen: spool number, material, grams remaining, Weight recorded"> | **Done** — spool number, material, and remaining weight recorded locally |
| <img src="images/lcd/registered.svg" width="300" alt="Registered screen: Registered!, spool number and weight on one line, NEEDS ONBOARDING, Scan QR to add details or visit the Onboard page, both addresses, and a QR code"> | **New spool registered** — scan the QR or visit the **Onboard** page to fill in the material details (see *New Spools* below) |
| <img src="images/lcd/updating-tag.svg" width="300" alt="Updating tag screen"> | **Updating tag** — writing updated filament data back to the NFC tag |
| <img src="images/lcd/idle-uncalibrated.svg" width="300" alt="Idle screen with a Scale not calibrated warning and a QR code"> | **Not calibrated** — shown on idle until the scale is calibrated (weights are unreliable until then) |

> **Opening the web app from your phone.** The idle screen shows a **QR code** on
> the right — scan it with the camera and it opens the app directly. The address
> is also printed as text: `weighstation.local` for typing, with the numeric IP
> underneath as a fallback. Use the numeric one if `.local` doesn't resolve,
> which happens on some Android browsers and across some networks. (The QR
> always encodes the numeric address, for exactly that reason.)

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
   The display shows `Registered!`, the spool number, and **NEEDS ONBOARDING**
   — the spool has no vendor, material or colour yet, so it is not usable for
   inventory until step 4. **Scan the QR code on that screen** to jump straight
   to the form for this spool, or type either address it shows —
   `weighstation.local/onboard` or the numeric one below it. Both go to the
   **Onboard** page, which acts on whichever spool is currently on the scale.

   <img src="images/lcd/registered.svg" width="300" alt="Registered screen: Registered!, spool number and weight on one line, NEEDS ONBOARDING, Scan QR to add details or visit the Onboard page, both addresses, and a QR code">

4. **Open the web app and fill in the material details** for that spool — see
   *Onboarding a spool* below.

Once the details are saved, the NFC tag is written with the real data — either
right away if the spool is still on the scale, or the next time it's placed.

### Onboarding a spool (web app)

1. With the spool on the scale, browse to `http://weighstation.local/` on any
   phone or laptop on the same network.
2. Open **Onboard**. If this is another spool of something already on file,
   pick **another spool of X** — it inherits the vendor, material, colour and
   tare with nothing to retype. For something new, choose **a new product**.
3. **Search the catalog first.** Typing a vendor or material name searches the
   published OpenPrintTag catalog directly; picking a real result fills in the
   brand, material, colour, print temperatures, and the manufacturer's own
   identifiers — the same data a genuine vendor tag carries. If the filament
   isn't in the catalog, expand **Enter details manually** and pick the
   **vendor**, **material**, **color**, and **spool profile** (the profile
   fills in the empty-spool tare and nominal weight) from the lab's own lists.
   Any of those four fields has a **+ Add new** option if it isn't in the list
   yet — no need to visit Settings first to add it. You can also capture the
   tare from a matching empty spool with **Capture tare**.
4. **Save & write tag.** The full OpenPrintTag data (identity, print temps,
   weights) is written to the tag and the record is saved.

### Applying an NFC tag to a new spool

Use an OpenPrintTag MK1 sticker — sourced directly from
[Prusa](https://www.prusa3d.com/), who created and promotes the OpenPrintTag
format itself (see `docs/build-guide.md` if you need to reorder). Peel and
apply to the flat hub face of the spool — away from the filament windings,
centered so it sits over the NFC reader when placed on the scale. Press
firmly for 5 seconds.

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

<img src="images/lcd/idle-uncalibrated.svg" width="300" alt="Idle screen with a Scale not calibrated warning and a QR code">

1. With the spool on/off as directed, browse to `http://weighstation.local/` and
   open **Settings → Calibrate**.
2. **Step 1 — Zero:** clear the scale, then click **Zero (tare)**.
3. **Step 2 — Calibrate:** place a known weight, type its exact mass in grams,
   then **Set calibration**.

The banner clears when it takes. (Serial `ZERO` / `CAL <grams>` at 115200 baud
still work as a fallback.)

---

## For Administrators: Ongoing Maintenance

### The web app

`http://weighstation.local/` provides:

- **Inventory** — remaining filament by material, grouped by vendor. Click a
  row to expand it and see the individual spools, or a spool number to open
  its weigh history + a remaining-over-time sparkline (with CSV export). This
  is also where a physical stock **audit** is started — see below.
- **Onboard** — fill in details for a new/blank spool, or add another spool of
  something already on file.
- **Reorder** — Stock List items at or below threshold; download a CSV to
  place the order.
- **Stock List** (`/stock`) — the curated list of what the lab keeps in stock,
  each with a 90-day popularity score, sorted so the least-popular (or
  never-in-stock) items surface first — see *Deciding what to stock* below.
- **Usage** — the lab's permanent consumption record: how much filament has
  gone through the station, per material, broken down by year and by month,
  all-time; CSV download. (For deciding what belongs on the Stock List, prefer
  the Stock List page's popularity score — it corrects for spools that sold
  out early, which this page's plain totals do not.)
- **Settings** — edit the vendor/material/color/spool-profile/stock-item
  tables, set the WiFi/API-key, choose the display timezone and 12/24-hour
  clock format, rename the station (the idle screen's greeting — "Seattle
  Makers" by default, but any lab running this can make it their own), and
  reach **Calibrate**.
- **Backup** — download the event log; restore from an uploaded backup. Also
  where the separate Settings-table backup lives (see *Recalibrating /
  backups* below).

A product's own edit page (`/product?id=N`) isn't in the top nav — reach it by
clicking a spool's material name on its detail page. Editing there updates
every spool of that product and rewrites their tags automatically the next
time each is placed on the scale.

### Physical inventory audits

Periodically (e.g. a monthly shelf count), do a physical audit: weigh and scan
every spool the lab actually has, and let the station reconcile what wasn't
seen.

1. On the **Inventory** page, click **Start audit**. Spools weighed from this
   point on are marked found automatically — no separate step needed for
   normal weighing during the count.
2. When every spool has been physically checked, click **Finish audit**. The
   page now lists each spool that was never seen during the count, with two
   buttons per row:
   - **Found** — it's on the shelf but wasn't weighed (e.g. you eyeballed it
     without placing it on the scale). No weight change.
   - **Close** — it's genuinely gone. The station records its last known
     weight as consumed (the same as a normal weigh-to-zero) and marks it
     **retired**.
3. Every not-found spool must be resolved one of these two ways before the
   audit finishes — there's no way to leave some undecided. Once the last one
   is resolved, the audit closes itself; there's no separate "finish resolving"
   button.
4. **Abandon audit** drops the audit at any point without undoing anything
   already marked Found or Closed.

**Retired spools are not deleted.** They stay in the inventory history (their
consumption still counts toward Usage and Stock List popularity) but are
hidden from the normal Inventory view by default — a count next to a
**show/hide** toggle reveals them. If a spool marked retired is later found
and weighed again, it's automatically un-retired and rejoins normal tracking
with no extra step.

### Deciding what to stock

The **Stock List** page (`/stock`) is where the lab's day-to-day
"what do we keep buying" decisions happen — it's a curated list, not a
snapshot of everything currently on the shelf (that's Inventory). Each entry
shows a **popularity score**: grams consumed over the trailing 90 days,
corrected so a material that sold out early in that window isn't scored as
if it sat unused the rest of the time. The list sorts lowest-popularity (and
never-in-stock) first, since those are the candidates to reconsider. A
material can be in the lab's inventory without being on the Stock List at
all — that's normal for something being phased out, not an error.

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

Recalibrate any time via **Settings → Calibrate**.

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

**The Settings tables (vendors/materials/colors/spool-profiles/Stock List)
back up separately**, also from the **Backup** page: they download and
restore as their own file, independent of the event log. Nothing in a spool's
record points back into these tables by ID, so the two backups don't need to
be kept in sync — and the Settings-only file is also the quick way to hand a
second station the lab's picklists without dragging along this one's spool
history.

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
| `/config/export` | The Settings tables as one file (the separate backup mentioned above) |
| `/api/status` | Everything at a glance: what the station is doing, what's on the scale, WiFi, storage health |
| `/api/spools` | Every spool and its remaining filament, as JSON |
| `/api/products` | Every product (filament SKU) the station knows about |
| `/api/stock` | The Stock List with each item's 90-day popularity score |
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

To require a password for those actions, set an **API key** on the **Settings**
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

- Recalibrate: open **Settings → Calibrate**, **Zero** with the platform clear,
  then **Set calibration** with a known weight.
- Check that the load cell wiring hasn't shifted (particularly E+ / E− polarity).

### Display is blank

- Check the TFT's SPI wiring — CS (GPIO 15), DC (GPIO 16), RST (GPIO 17), and the
  shared MOSI/SCK.
- Make sure the backlight (`LED` pin) is powered (tied to 3.3 V) — the panel
  shows nothing without it.
