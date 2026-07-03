# Weigh Station — User Manual

The Weigh Station is a filament inventory tool for the Seattle Makers 3D printing lab. Place a spool on the scale; it reads the NFC tag, weighs the spool, and updates the remaining filament in Spoolman automatically.

---

## For Members: Weighing a Spool

1. **Place the spool on the scale.** The NFC reader and load cell activate immediately — no button press needed.
2. **Wait for the display to settle.** It will show the spool number, material, and remaining weight for a second or two while it syncs.
3. **Remove the spool.** The display returns to the idle screen.

That's it. Spoolman is updated in the background.

### What the display shows

| Display | Meaning |
|---|---|
| `Seattle Makers / Weigh Station / Place spool to begin` | Idle, ready |
| `Weighing...` | Reading the load cell |
| `Spool #N / [material] / NNNg remaining / Synced` | Done — Spoolman updated |
| `Registered! / Spool #N / Edit in Spoolman / NNNg` | New spool just registered — an admin needs to fill in the material details |
| `Updating tag...` | Writing updated filament data back to the NFC tag |
| `Spoolman offline / NNNg (local) / Will sync later` | Can't reach Spoolman — weight is saved to the tag and will sync on the next successful connection |

### Status light (NeoPixel)

| Color | Meaning |
|---|---|
| Dim green | Idle, ready |
| Blue | Communicating (WiFi setup, weighing, writing tag) |
| Green | Spool weighed and synced |
| Yellow | Spool registered but needs data entry in Spoolman |
| Orange | Spoolman unreachable |
| Amber (blinking, accelerating) | New tag countdown — remove the spool to cancel |
| Red | Tag read error — reposition the spool |

---

## For Members: New Spools

When you place a spool with a **blank NFC tag** on the scale:

1. The display shows a **5-second countdown** — "New tag found / Remove to cancel / Registering in: 5…"
2. **Remove the spool within 5 seconds** to cancel, or **leave it in place** to proceed.
3. The station formats the tag and creates a placeholder record in Spoolman. The display shows `Registered! / Spool #N / Edit in Spoolman`.
4. **An admin needs to open Spoolman and fill in the material details** (brand, material type, color, weight) for that spool number.

Once the details are saved in Spoolman, the next time that spool is placed on the scale the NFC tag is automatically updated with the real data.

### Applying an NFC tag to a new spool

Use an OpenPrintTag MK1 sticker. Peel and apply to the flat hub face of the spool — away from the filament windings, centered so it sits over the NFC reader when placed on the scale. Press firmly for 5 seconds.

**Do not reuse tags.** Peeling a tag risks cracking the foil antenna invisibly. New tag for every new spool.

### Pre-tagged spools (e.g. Prusament)

Some spools arrive with an NFC tag already applied (genuine Prusament, or spools tagged by another maker's setup). The station recognizes these as legitimate and registers them in Spoolman using the data already on the tag — no countdown, no blank-tag flow. It then weighs and syncs normally.

---

## For Administrators: Initial Setup

### 1. Hardware assembly

Wire the components per `docs/wiring.md`. The NAU7802 and SSD1306 connect via a Qwiic cable to the board's Qwiic connector. The PN5180 connects via SPI.

### 2. Flash the firmware

```bash
cd <repo>
pio run --target upload
```

Monitor output:

```bash
pio device monitor --baud 115200
```

### 3. First-time WiFi and Spoolman URL setup

On first boot, the display shows:

```
WiFi Setup
Join AP:
WeighStation-Setup
```

1. On any phone or laptop, join the `WeighStation-Setup` WiFi network.
2. A captive portal page opens automatically (or browse to `192.168.4.1`).
3. Select your WiFi network and enter the password.
4. Set the **Spoolman URL** field (e.g. `http://spoolman.local:7912`).
5. Submit. The device connects and the display changes to the idle screen.

### 4. Scale calibration

The scale auto-tares on first boot (assuming the platform is empty). To calibrate with a known weight:

1. Open a serial terminal at 115200 baud (`pio device monitor`).
2. With **nothing** on the scale, send:
   ```
   ZERO
   ```
3. Place a known reference weight (e.g. a 100 g calibration weight) on the scale platform, then send:
   ```
   CAL 100
   ```
   Replace `100` with the actual mass in grams.
4. Calibration is saved to flash automatically. Repeat any time the load cell is remounted or readings drift.

---

## For Administrators: Ongoing Maintenance

### Changing the Spoolman URL

From any browser on the same network:

1. Go to `http://weighstation.local/`
2. Update the **Spoolman URL** field and click **Save**.

No restart required. The new URL takes effect immediately and is persisted to flash.

### Changing the WiFi network

If the network SSID or password changes:

**Option A — web (device must currently be connected):**

1. Go to `http://weighstation.local/reset`
2. The device reboots and opens the `WeighStation-Setup` AP.
3. Connect and reconfigure as in the initial setup.

**Option B — power cycle (device unreachable or not yet connected):**

Power-cycle the device. If the stored credentials fail, WiFiManager automatically opens the `WeighStation-Setup` AP for **120 seconds**. Connect within that window and reconfigure.

> The Spoolman URL is preserved across a WiFi reset — you only need to re-enter the WiFi credentials.

### Recalibrating the scale

Repeat the calibration procedure in the Setup section. New values overwrite the stored ones immediately.

---

## Troubleshooting

### "Spoolman offline" on the display

The device can't reach the Spoolman server. Possible causes:

- **Wrong URL** — browse to `http://weighstation.local/` and check the Spoolman URL field. It should match the address you use to reach Spoolman in a browser (e.g. `http://spoolman.local:7912`).
- **Spoolman server is down** — check that the machine running Spoolman is on and the service is running.
- **WiFi issue** — see below.

The display shows `Fix SpoolMan URL: / weighstation.local` as a reminder. The weight is still written to the NFC tag and will sync to Spoolman once the connection is restored.

### Device won't connect to WiFi

- Power-cycle the device and connect to `WeighStation-Setup` within 120 seconds to reconfigure.
- If the SSID list in the portal is empty, wait 30 seconds for the scan to complete and refresh the page.

### NFC tag not reading ("Read error")

- Reposition the spool so the tag is centered over the reader.
- Check that the tag sticker is fully adhered with no air bubbles near the center.
- If the error persists, the tag antenna may be damaged — replace the tag.

### Scale reads zero or wildly wrong values

- Ensure the platform is clear and stable, then send `ZERO` over serial.
- If readings are consistently off by a fixed ratio, recalibrate with `CAL <grams>`.
- Check that the load cell wiring hasn't shifted (particularly E+ / E− polarity).

### Display is blank

- Confirm the Qwiic cable is fully seated at both ends.
- Check the serial output for `[display] SSD1306 not found` — if present, the I²C address may be wrong or the cable is faulty.
