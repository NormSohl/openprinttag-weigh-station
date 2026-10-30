# Design: product names are labels, not keys

Status: **built** on branch `names-are-labels` (2026-10-30), off
`barcode-onboarding`; not on the station. Items 1–4 below are done:
Reorder counts linked products only (`rollUp()`), popularity and Usage key
on the product (following merges; Usage rows gain `"prod"`, a format
change), and typed entries match by identifiers only, with "This is a new
product" required. **Flash it only after the real Stock List is linked on
`/repair`** (from the barcode branch), or its unlinked items drop off Reorder. Follows
`barcode-onboarding.md` (to be on the station first) and settles step 8 of
`onboarding-vocabulary.md` in favour of option C.

## Why

The descriptive text from the OpenPrintTag database often doesn't match what
the vendor's website (or the box) calls the product, and staff want to rename
products to match. Today a rename can quietly break things, because some
parts of the station still match by name. Found 2026-10-29, after a member
renamed several products without authorization: nothing was lost, but
unlinked Stock List items could read 0 on hand, the Usage report split, and
the next hand-typed onboarding with the old spelling made a duplicate.

Goal: a product's vendor and name are display text only. Renaming one
changes what is shown, and on the next placement what is written to its
tags, and nothing else.

## Already keyed by id (safe to rename today)

- Stock List items linked to a product: `rollUp()` counts by
  `CfgStock::product`.
- Catalog picks: `findProduct_()` matches `pkg_uuid` first.
- Barcodes (`barcode-onboarding.md`): `ProductRecord::gtin`.
- "Another spool of X" and the Stock List picker on Onboard: a product id.

## Still keyed by name, and the change for each

1. **Unlinked Stock List items.** `rollUp()` falls back to a name probe
   (vendor + material + colour), then vendor + abbreviation.
   *Change:* link every item first (`/repair`, which links them in bulk),
   then remove both fallbacks. An unlinked item shows "not linked", never
   a guessed count.
2. **Stock List popularity.** `storeMaterialPopularity()` keys on vendor +
   material display strings.
   *Change:* key on product id (spool records already carry it; resolve
   merges through `storeGetProduct()`).
3. **Usage report.** Usage rows group by vendor + abbreviation strings
   (`usageAdd_()`), so a vendor rename splits it.
   *Change:* record the product id on Usage rows from now on, and show
   current names. Rows written before the change keep their stored names.
   This is a store format change (like `merged_into`): no firmware
   downgrade afterwards.
4. **The matcher's last rung** (`findProduct_()` step 4: vendor + material
   + nominal weight), used when a source carries no identifier:
   - *Hand-typed onboarding:* today finds an existing product by name.
     *Change:* hand entry must pick an existing product (Stock List item, or
     "another spool of X") or say explicitly that this is a new product.
     Typing a name never quietly matches. (Option C.)
   - *Third-party tags with no catalog ids:* keep the name rung here only,
     and only ever to create a **provisional** product, which a person
     confirms or merges on `/repair`. Nothing a person types is matched by
     name.

## What stays controlled

- **Type abbreviation** (PLA, PETG…): it looks up print/bed temperatures
  (`cfgMaterialByAbbr()`) and groups the Usage report, so it is picked from
  the materials list, not typed freely.
- Vendor and product name: free text, display only.

## Costs

- **More duplicates at first.** Matches the name rung used to make silently
  (a second hand-typed spool of a known product) become duplicates if staff
  don't pick the existing product. The Stock List picker and the scanner make
  picking the normal path; `/repair` catches the rest.
- **Renames reach tags** on next placement. Correct: OPT's `material_name`
  and `brand_name` are display fields.
- **Usage rows gain a field** (format change, above).

## Order

1. Barcode branch on the station; link the real Stock List on `/repair`
   (rehearse on the simulator with the production backups first).
2. Items 1 and 2: small, no format change.
3. Item 4: Onboard hand entry changes; the matcher's name rung limited to
   provisional tag adoption.
4. Item 3: the Usage format change.

Each step can be built and tested in the simulator (`tools/sim/`) and the
native store tests (`tools/store/`) before it goes on the station.

## Related: protecting product edits

The renames were possible because writes are guarded only once an API key
is set; an unset key leaves them open on purpose, so the cabinet-mounted
station can never lock its operators out (`CLAUDE.md`, *Writes are
guarded*). Set one, or change it if it was known, on the Settings page or
with `APIKEY <secret>` over serial, before relying on names staying put.
