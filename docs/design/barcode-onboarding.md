# Design: onboard from the Stock List, matched by barcode

Status: **proposed** (2026-10-29) — nothing built. Written up from a design
discussion on the Mac; the reasoning is kept here so the branch carries it.

| step | state |
|---|---|
| 1. Onboard page: "Which stock item is this?" picker over Stock List rows | not started |
| 2. `GET /api/lookup?gtin=…` — barcode → products / Stock List rows | not started |
| 3. Onboard page: catch scanner input, select the matching row | not started |
| 4. Learn: a picked row's product takes the scanned barcode | not started |
| 5. Repair page: unlinked products vs. Stock List rows with nothing on hand | not started — this is step 7 (*Product merge*) of `onboarding-vocabulary.md` |

## The problem

Reorder compares two lists: the **Stock List** (what we want to keep) and
the **spools** on hand. The comparison itself is exact: `rollUp()`
(`web_app.cpp`) counts spools whose `product` equals the Stock List row's
`product`. What is fragile is getting both sides to point at the *same*
product in the first place.

Both sides get there independently. The Stock List was built by searching
the OpenPrintTag database; onboarding searches it again, separately, by
whatever the box says. `findOrCreateProduct()` then runs the matching
ladder (`findProduct_()`, `store.cpp`): package UUID → GTIN → material UUID
\+ nominal weight → vendor + name + weight. **On a miss it creates a new
product without saying so.** A near-neighbour pick (refill instead of
spooled, another size, a duplicate database entry) or a fallback to manual
entry therefore gives the spool a product no Stock List row points at. The
row reads zero on hand and asks for a reorder; the only place the
duplicate is visible is the Products page.

The names really do differ. eSUN sells the filament as **PLA+**; the
database lists it as **PLA Pro** (`esun-pla-pro-black-1kg`). Searching by
the name on the box misses it.

## Decision: the Stock List is the source of truth

Onboarding stops re-deriving identity from the database. Staff say which
**Stock List row** a spool is, and the spool takes that row's product
directly, so the link is exact by construction. Database search stays as
the fallback for filament we do not stock. This extends the existing
"another spool of X" path, which already does this for products.

## Barcodes as the physical key

The spool itself carries no barcode (only a QR code to the vendor's site),
but the **box** carries an EAN-13, one per colour per size.

Evidence, gathered 2026-10-29 without a box in hand:

- Retailers list them, e.g. alltron.ch: eSUN PLA+ 1.75 mm 1 kg black
  **6922572219021**, white 6922572219038. All eSUN codes seen start 6922572.
- The OpenPrintTag database has them: **352 of 462** eSUN packages carry a
  `gtin`; PLA Pro has 110 of 116 colours. Gaps are mostly specialty lines
  (PLA Silk Magic, Silk Candy, Silk, PLA Lite, some PETG).
- They agree: the retailer's black PLA+ code is exactly the `gtin` on
  `esun-pla-pro-black-1kg`.
- **Two codes are shared** by two colours each in the database
  (6922572209237: TPU-LW grey/white; 6922572222755: PETG Matte Blue Brick /
  Dark Grey) — most likely data-entry errors. So a barcode may match more
  than one row, and the lookup must ask rather than guess.

**Still to confirm on a real box:** that the EAN is printed on the outside.

Products created from a database pick already store its GTIN
(`web_app.cpp`, Stock List catalog path), so Stock List rows built from the
database should mostly match from the first scan.

## Scanner: a keyboard-wedge scanner on the staff PC

Staff onboard from a dedicated PC showing the station's web app. So the
scanner attaches to **that PC**, not to the station — no station hardware,
no firmware radio work.

- A scanner with a **USB 2.4 GHz dongle** (or a corded USB one if the PC is
  beside the scale): it appears as a keyboard, needs no Bluetooth pairing to
  keep alive on Windows. Roughly $20–50 (Eyoyo, NETUM and similar; must read
  1D EAN-13). Default config — digits then Enter — is what we want.
- Considered and set aside:
  - **UART scanner module built into the station** (GM65/GM67 type,
    ~$20–35, free GPIOs and VUSB available). Works for anyone, but needs a
    window and mount in the enclosure. Not needed while onboarding happens
    at one PC.
  - **BLE scanner paired to the ESP32-S3.** The S3 has BLE only (no
    Bluetooth Classic), so it needs a scanner with a BLE HID mode, a BLE HID
    host on the station, and Wi-Fi coexistence testing. Only worth it if
    members onboard from their own phones.

## How it fits together

1. **Picker.** `/onboard` leads with "Which stock item is this?": a
   searchable list of Stock List rows by our own names, with colour swatch.
   Picking one resolves to `CfgStock::product`. "A new product" / database
   search follow for filament we do not stock.
2. **Lookup.** `GET /api/lookup?gtin=N` returns the products with that GTIN
   and the Stock List rows linked to them. Read-only, so open like every
   other read.
3. **Scan capture.** Page-level key listener: 8–14 digits arriving a few
   milliseconds apart and ending in Enter is a scan, not typing. Handled
   wherever focus is, and the Enter must not submit the form. Then:
   - one row → select it, show "Scanned: eSUN PLA+ Black";
   - several → ask which;
   - none → open the picker.
4. **Learn.** When staff pick a row after a scan that matched nothing, the
   row's product takes that GTIN (through `/product`'s propagation path, so
   it is an ordinary product edit in the log). The next box of the same
   filament matches by itself. Never overwrite a product's existing,
   different GTIN silently — ask.
5. **Repair.** Existing mismatches on production predate all of this:
   products with spools on hand but no Stock List row, beside Stock List
   rows reading zero. Link them with the planned product merge
   (`onboarding-vocabulary.md` step 7), which keeps the losing product's
   identifiers as aliases so the matcher learns.

Everything except the scanner is firmware and web app, testable on the
bench by typing a barcode into the page.

## Open questions

- Is the EAN on the outside of eSUN boxes? (One box settles it.)
- How many production Stock List rows are unlinked today? Pull
  `/api/stock`, `/api/products` and `/api/spools` from the makerspace
  network to count before building the repair page.
- Other vendors in stock: check their GTIN coverage in the database the
  same way.
