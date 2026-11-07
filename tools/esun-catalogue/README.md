# eSUN catalogue

eSUN US (esun3d.com) is who the lab orders from, so its list is the master copy of
what we can stock. This folder turns that list into a spreadsheet the lab can use,
adds the barcodes eSUN doesn't publish, and says which items the OpenPrintTag
database (what Restock searches today) is missing.

Outputs, in `docs/reference/`:

| File | What |
|---|---|
| `esun-us-catalogue-DATE.xlsx` | **The master list.** Filaments only, one unit per row: material, colour, eSUN part number, Filament ID, barcode, size, packaging, in the OpenPrintTag database?, orderable on eSUN US, price. Orderable items on the main tab; the rest kept on a grey tab. |
| `esun-products-DATE.xlsx` | Lieske Elektronik (an EU distributor, the barcode source) compared with eSUN US, plus both raw lists. |

## Refreshing it

1. **eSUN US** (needs a person at a browser: the site refuses scripted reads).
   In this folder run `python3 receive.py`, open https://www.esun3d.com/ in a
   normal browser, open the developer console, paste `scrape_esun_us.js`, press
   Enter. About two minutes later the tab says *saved …* and
   `data/esun-us-YYYY-MM-DD.json` exists.
2. **Lieske** (scripted, about seven minutes, one page a second):
   `DATE=YYYY-MM-DD python3 crawl_lieske.py` → `data/lieske-YYYY-MM-DD.json`.
   Use the same DATE as step 1.
3. `./make.sh` builds both spreadsheets from the newest data. It creates its own
   Python environment and fetches the OpenPrintTag database into `work/` (git
   ignores it). `OPT_REF=<full commit hash>` pins the database (the 2026-11-06 build used `120b321f1b690acca543459344fe35415a895b19`); `DATE=` picks older data.

4. Load `work/station-catalogue.json` on the station (Administration → eSUN US
   catalogue), or ask Claude for a new station kit, which includes it.

Keep the old `data/` files: they are the record of what was orderable when, and let
any spreadsheet be rebuilt exactly.

## How it is built

`make.sh` runs, in order (each reads and writes `work/`):

| Step | Does |
|---|---|
| `catalogue.py` | eSUN US scrape → one row per filament variant: material, colour, part number, Filament ID, size, packaging, salable, price. |
| `compare_lieske.py` | Matches Lieske's items to eSUN US's (different part-number schemes for one product). |
| `barcodes.py` | Barcodes from the OpenPrintTag database and Lieske, where the packaging matches exactly. |
| `optmatch.py` | Is each variant in the OpenPrintTag database: this package, the colour only, or not at all. |
| `build_catalogue.py` | The master spreadsheet, and `work/catalogue_full.json` for `tools/reconcile`. |
| `build_comparison.py` | The Lieske comparison spreadsheet. |
| `station_catalogue.py` | `work/station-catalogue.json`: the station's own copy (orderable items only), loaded on Administration → eSUN US catalogue. Every row carries its final values (name, list label, short code, material type, temperatures, colour, barcode, OpenPrintTag IDs); the station copies them and computes nothing. IDs are the database's where it has the item, else derived as the spec does (UUIDv5 from brand + barcode / brand + material name), so a later database entry for the same item carries the same IDs. An item without a barcode gets its package ID from brand + eSUN part number, so every item has its own (a spool and its refill must stay two products); a database package ID shared by two packages is not used. |

`esunlib.py` holds what they share.

## Decisions baked in (2026-11-06)

- **Filaments only, single units only.** Resins, accessories, multi-packs and
  bundles (CMYK sets, `HJ-` bundles, the PLA-Basic 250 g x8 mini pack) are left out.
- **Part numbers** are family + diameter + colour code + size + packaging
  (`PLA+ 175 B 1 P1`). **Filament ID** drops size and packaging (`PLA+175B`), so one
  filament links across spool, refill, refill set, 3 kg and 5 kg. Colour codes only
  mean something within a family (`BR` is Brick Red in PLA+, Black Red in Silk
  Magic), so the ID keeps the family.
- **Packaging codes:** P1 paper spool, R1 refill, RS1 one refill sold with an
  eSpool+, B1 plastic spool; S1, Y1, TZ1, P4 are spool types eSUN doesn't name
  (P4 is *not* a 4-pack). One eSUN listing is wrong and corrected:
  `PLA+175T1OR1` is PETG Refilament Clear (`esunlib.FID_FIXES`).
- **Material** is eSUN's product name without packaging words, hyphen shown as a
  space (`PLA-Basic` → PLA Basic) except technical grades (`PLA-CF`, `TPU-95A`).
- **Barcodes** (eSUN publishes none) come from the OpenPrintTag database and
  Lieske, only where the packaging matches; where both have one they agree. A
  barcode the database gives to two different products is dropped. None is yet
  confirmed by scanning a box bought in the US.
- **OpenPrintTag match** compares names as sets of words, with the database's
  wording mapped across (Carbon Fiber PLA = PLA-CF, Super Tough PLA = PLA-ST,
  PLA Silk Metal = PLA Silk, Transparent = Translucent, Solid/Opaque ignored).
- **Orderable** is eSUN's own "salable" flag for its US store. eSUN doesn't say
  whether *not orderable* is out of stock for now or not sold in the US.
