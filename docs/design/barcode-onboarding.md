# Design: onboard from the Stock List, matched by barcode

Status: **proposed** (2026-10-29) — nothing built. Written up from a design
discussion on the Mac; the reasoning is kept here so the branch carries it.

| step | state |
|---|---|
| 1. Stock List picker on `/onboard` (no scanner needed) | not started |
| 2. `/api/lookup` + scan capture on `/onboard` | not started |
| 3. Learn barcodes on save; flag a pick whose barcode disagrees | not started |
| 4. Scans on `/stock`; GTIN shown and editable on `/product` | not started |
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

## Integration plan

The key fact: `/onboard` already has the "another spool of X" path, a
`<select name='product' id='prod'>` whose value `handleApiOnboard` resolves
directly (product id → identity, product tare as the default). **The Stock
List picker and the scanner both just set that select.** The onboarding
write path, the tag write and the reorder roll-up stay as they are.

### Phase 1 — Stock List picker (useful with no scanner)

- `/onboard`, above "This spool is…": **"Which stock item is this?"**, a
  search box over Stock List rows, using the row's own name and the linked
  product's colour swatch. Rendered server-side from `cfgStockAt()` +
  `storeGetProduct()`; filtered in the page as you type (~60 rows, no
  server round trip).
- Picking a row sets `#prod` to `CfgStock::product` and fires its
  `onchange`, so `#newprod` hides exactly as a manual pick does. A small
  "✓ eSUN PLA+ Black (Stock List)" line confirms it; "clear" puts `#prod`
  back to "a new product".
- Rows with **no product id**, or one that no longer resolves, are listed
  greyed with "not linked — fix on Stock List", never selectable: picking
  one would have to guess a product, which is the bug being fixed.
- Tare is unchanged: the profile picker / tare box stay visible, product
  tare is the default, a typed tare wins (per `onboarding-vocabulary.md`).
- Also: the `#prod` product list gets the same search box. The bench
  review already noted the "~62-item product list has no search".

### Phase 2 — Barcode lookup and scan capture

- **`GET /api/lookup?gtin=N`** (open read, like every other GET). Matches
  `ProductRecord::gtin` and the Stock List row's own `CfgStock::gtin`, and
  returns `{gtin, products:[{id, vendor, material, nom_g}], stock:[{id,
  name, product}]}`. Comparison is numeric (`uint64`), so a 12-digit UPC-A
  and its 13-digit EAN form (leading 0) match without special cases.
- **Scan capture**, one shared script (`SCAN_SCRIPT`, same pattern as
  `CATALOG_SCRIPT`) so `/onboard` and `/stock` behave identically:
  - A document-level `keydown` listener buffers digits that arrive **< 50 ms
    apart**; a burst of **8, 12, 13 or 14 digits ending in Enter**, with a
    **valid GS1 check digit**, is a scan. Anything else is typing and is
    left alone. The check digit is what stops a fast typist or a pasted
    number from triggering it.
  - Works wherever focus is. If the burst landed in a focused input, that
    input's value is restored to what it was when the burst began, and the
    Enter's default is prevented so the form never submits from a scan.
  - Result on `/onboard`:
    - **one Stock List row** → select it as in phase 1, show "Scanned
      6922572219021 → eSUN PLA+ Black";
    - **one product, not on the Stock List** → select it in `#prod`, with a
      note that it is not a stock item;
    - **several** (the two duplicate eSUN codes) → show just those rows to
      choose from;
    - **none** → open the Stock List picker with "Unknown barcode — pick the
      stock item"; the barcode is kept for phase 3.
- **Scan before the spool is placed.** The page shows "No spool is on the
  scale" until one is, then `onboardWatchScript` reloads it. A scan on that
  page is held in `sessionStorage` (≤ 2 min) and applied after the reload,
  so either order works. Wrapped in try/catch like every storage access.
- Scanner setup: none beyond the default (digits + Enter suffix). No prefix
  character needed thanks to the timing + check-digit rule.

### Phase 3 — Learn barcodes; catch wrong picks

- The page sends the scanned code as a hidden `scan_gtin` field.
- `handleApiOnboard`, **after** all validation (a 4xx must not have changed
  anything — see the gotcha in CLAUDE.md):
  - product resolved and its `gtin` is 0 → set it, via
    `storeUpsertProduct()` + `storePropagateProduct()`, so it is an ordinary
    product edit and the spools' tags pick it up through the reconcile loop;
  - product already has the **same** GTIN → nothing;
  - product has a **different** GTIN → do not overwrite. Record nothing,
    and show "this product's barcode is X, you scanned Y" on the result
    page. Either the pick or the database is wrong; a person decides on
    `/product`.
- **Catalog path cross-check.** When "a new product" is chosen by database
  search after a scan, compare the pick's `cat_gtin` with the scan before
  submit: different → "the database says this package's barcode is X, you
  scanned Y — wrong package?" This catches exactly the near-neighbour picks
  (refill vs spool, another size) that cause the problem today. A pick with
  no GTIN takes the scanned one.

### Phase 4 — Stock List and Products pages

- `/stock` Add/Edit: a scan fills the row's **GTIN** field and, if
  `/api/lookup` knows the product, preselects it. Building the Stock List
  by scanning the boxes on hand is then possible.
- `/product?id=N`: show and edit the GTIN, so a wrong one learned in phase
  3 can be fixed in the one place products are edited.
- Stock List page: a "not linked" marker on rows with no resolvable
  product, as a pointer to step 5 (repair).

### Testing

- `/api/lookup`: `curl` on the bench with codes from a seeded store.
- Scan capture without a scanner: keyboard typing is too slow to count as a
  scan by design, so the script exposes `window.simulateScan("…")` for the
  browser console and accepts `?scan=…` on the URL. Real scanner on the
  staff PC last, on site.
- `pio run` for every phase; the web app has no native harness, so this is
  compile-verified until the bench.

Each phase is useful on its own and can ship separately: phase 1 fixes the
matching without any hardware.

## Open questions

- Is the EAN on the outside of eSUN boxes? (One box settles it.)
- How many production Stock List rows are unlinked today? Pull
  `/api/stock`, `/api/products` and `/api/spools` from the makerspace
  network to count before building the repair page.
- Other vendors in stock: check their GTIN coverage in the database the
  same way.
