# Design: one vocabulary for onboarding

Status: **proposed** (2026-09-28) — nothing built. Written to pin down the
problem before choosing a fix; step 1 of *Sequencing* is useful on its own and
does not depend on the rest.

| step | state |
|---|---|
| Unknown tare is visible; every source feeds the picklists and spool profiles | not started |
| Vocabulary moves into the event log | not started |
| Name normalization (alias table, and option A or C) | not started — wait for data |
| The service reads the vocabulary | not started — depends on `station-service-split.md` |

## The problem

A spool's descriptive data reaches the station from three sources:

1. **A tag that is already written** — a vendor spool, or one tagged elsewhere
   (foreign-tag adoption, `sync_task.cpp` `productFromMain()` /
   `identityFromMain()`).
2. **A pick from the OpenPrintTag database** — the catalog search on `/onboard`
   and `/stock` (`CATALOG_SCRIPT` `applyPick()`, then `handleApiOnboard`'s
   `source=catalog` branch).
3. **Entry by a person** — the manual picklists (`handleApiOnboard`'s manual
   branch).

They describe the same things — vendor, material, spool body — and should be
interchangeable. They are not. Each path writes what it was given verbatim,
nothing normalizes it, and only one of them teaches the station anything for
next time.

## What each source does today

Checked against the code 2026-09-28.

| | vendor | material (display, OPT key 10) | abbreviation (key 52) | tare | nominal |
|---|---|---|---|---|---|
| **Tag** | `brand_name` verbatim, or `"Unknown"` | `material_name` verbatim, or `"Unknown"` | verbatim, may be empty | `empty_container_weight`, **0 if absent** | `nominal_netto_full_weight` |
| **Database pick** | brand `name`, or a prettified slug | the database's `mat.name`, e.g. `"PLA Basic Fire Engine Red"` | `abbreviation`, else `type`, may be empty | container `empty_weight`, **0 if the package has no container**; the tare box overrides | package `nominal_netto_full_weight` |
| **Person** | picklist or typed | composed `<material> <colour>`, e.g. `"PLA Fire Engine Red"` | from the `CfgMaterial` row | from the spool profile (required); the tare box overrides | from the spool profile |
| *"Another spool of X"* | inherited from the product | inherited | inherited | inherited — **the tare box is ignored** | inherited |
| *Stock List row* | picklist, typed, or database | as above | as above | always 0 (nothing is weighed) | typed |

And what each one feeds back:

| | last-used defaults (`last_ob`) | vendor picklist | material / colour picklists | spool profiles |
|---|---|---|---|---|
| Tag | no | no | no | no |
| Database pick | no | **yes** (`cfgVendorAdd()`, `web_app.cpp:2033`, runs on every web onboard) | no | no |
| Person | **yes** — the only caller of `lastOnboardSet()` | yes | only via "+ Add new" | only via "+ Add new" |

## What goes wrong

1. **The display name has a different shape per source.** Hand entry always
   yields `<material> <colour>`; the database yields the vendor's own product
   name, which often carries more words ("Basic", "Matte", "HF"); a tag yields
   whatever its writer chose. `normEq_()` forgives case and whitespace only, so
   the same filament entered two ways becomes two products. Rungs 1–3 of
   `findProduct_()` (package UUID, GTIN, material UUID) usually save a database
   pick and a vendor tag from this — but hand entry carries no identifiers, so
   it always falls to rung 4, the name comparison. This is the same failure
   that broke the Stock List's name probe.
2. **Vendor spelling varies** — "Bambu Lab", "BambuLab", "Bambu" — and nothing
   reconciles them. Database picks leak their spelling into the vendor
   picklist; tags never do.
3. **The last-used defaults see one source in three.** After a foreign spool
   or a database pick, the manual picklists still default to whatever was last
   typed by hand.
4. **The picklists are not a shared vocabulary.** Materials and colours enter
   only by "+ Add new". A material that arrived by tag or database is never
   offered, so the next hand-entered spool of it is typed afresh — and
   possibly differently.
5. **The abbreviation is the weakest field and the most load-bearing one.** It
   keys the consumption rollup (and falls back to the full display name when
   empty, splitting one bucket per colour), and the catalog and
   "another spool" paths use it to look up the material's class, type and
   temperatures. That lookup, `cfgMaterialByName(abbr)`, compares against the
   `CfgMaterial` **name** field, while the manual path looks up by name — so a
   config row named `"PLA Silk"` with abbreviation `"PLA"` is found by one path
   and missed by the other.
6. **A missing tare is stored as zero, silently.** Remaining is gross minus
   tare, and the weigh (`sync_task.cpp:779`, using the tag's own
   `empty_container_weight`) falls back to gross when tare is 0 — so a spool with
   no recorded tare reports its own spool body as filament — typically
   200–250 g too much — and nothing flags it. Two sources can produce it: a
   database package with no container file, and a tag without
   `empty_container_weight`.
7. **Spool bodies are learned from one source.** A real tare from a database
   container or a vendor tag never becomes a spool profile, so the next
   hand-entered spool of the same vendor's body needs one created by hand.
   `CfgProfile` carries no vendor at all — its label is free text.
8. **Tare is not in the product key.** Nominal weight is (1 kg and 5 kg stay
   apart); tare is only compared after the fact, as a disagreement on tag
   adoption (`productDiffers_()`, `store.cpp:1004`), which is logged and left
   alone. So the first tare to arrive wins for every spool of that product.
   Usually right — worth knowing when it isn't.

## Guiding principle

**Identifiers match; the vocabulary fills in and checks.** OPT already
separates the two: UUIDs and GTIN say *which* product, `brand_name` and
`material_name` are for display. Matching should lean on identifiers wherever a
source has them, and the vocabulary tables exist to supply what a source left
blank and to catch a value that disagrees with what is known — not to be a
second, weaker identity.

It follows that every source, not just hand entry, should feed the vocabulary.

## The vocabulary

Four tables, each fed by all three sources:

| table | key | carries | fed by a tag | fed by a database pick |
|---|---|---|---|---|
| **Vendors** | canonical name, plus aliases | `brand_uuid` when known | `brand_name`, `brand_uuid` | brand `name`, `uuid` |
| **Materials** | abbreviation | class, type, diameter, print/bed temps | `material_abbreviation`, temps, class/type | `abbreviation`/`type`, `properties` |
| **Colours** | name | rgba | — (OPT has no colour name) | — |
| **Spool bodies** | vendor + label | tare, nominal | `empty_container_weight`, nominal | container `empty_weight`, package nominal |

- **A source adds, it never overwrites.** Same rule as a tag and a product: a
  tag may create a vocabulary row but not change one, or one odd tag rewrites
  what every later onboarding is offered. A disagreement is reported, the way
  `storeAdoptProduct()`'s `outDiffers` is.
- **Unknown tare is a state, not a number.** Tare 0 reads as *not recorded*
  everywhere it is shown — the same treatment cost already gets on
  `/spool?id=N`. A real spool body does not weigh nothing. When a source
  supplies no tare, onboarding offers the vendor's known spool body instead of
  storing 0.
- **The abbreviation is required on every path, or inferred.** Inference can
  come from the vocabulary (a known vendor + product) or the database's `type`;
  failing both, the form asks.
- **The last-used defaults record the resolved values, whatever the path.**
  After a database pick or a foreign spool, the manual picklists default to
  that vendor and material. The spool profile stays out of it: it starts
  neutral on purpose (2026-09-27), and tare is the one field where a
  remembered default silently sticks to the wrong spool.

## Where the vocabulary lives: the event log

Today the four tables are JSON files under `/config/`, backed up separately
from the log (`/config/export`). **They become event types in the log**, the
way Products already are, and the `/config/` tables become a rebuildable
index.

- **One backup.** `/export` stays the single file that captures everything the
  station knows, which it already claims to be.
- **One upload.** Under `station-service-split.md`, the continuous upload ships
  the event log and nothing else — so today the service would receive products
  but not the vocabulary those products were built from, and would have to
  build Stock List rows against names it has never seen. Putting the
  vocabulary in the log closes that without a second sync channel.
- **Every source feeds it the same way.** A tag or database pick that brings a
  new vendor or spool body appends the same event hand entry does. That is
  what makes the sources interchangeable, and it costs no mechanism beyond the
  event itself.
- **History comes free.** A vocabulary row that was renamed or corrected keeps
  its record, like everything else in the log.

What this costs:

- **`StoreEvent` is a union of every event type's fields** and a ~375-byte
  stack object (see *Gotchas* in `CLAUDE.md`). Vocabulary events should reuse
  existing fields — name, abbreviation, vendor, rgba, temps, dia, tare,
  nominal are all already there — rather than add new ones.
- **They are global, `uuid`-less events.** Until compaction is removed, each
  needs re-emission from the live index at `storeCompact()`, exactly like
  Products and audit markers, or it silently vanishes across a fold. Under the
  split's no-compaction design, the finished-spool filter must keep them
  forever — one more line type in a rule that already exists.
- **The Settings page's raw-JSON editors and `/config/import`** replace whole
  tables today. As a log-backed index they would have to emit events for the
  differences instead. `/config/export` shrinks to what is genuinely
  station-local configuration.

## Interaction with the station/service split

`station-service-split.md` says vendors, materials, profiles and colours stay
on the station "because the onboarding picklists need them." That is still
right — onboarding is a presence task — but it left two things unsaid:

1. **How the service gets them.** Answered above: they ride the log.
2. **Who creates products for filament nobody has onboarded.** Today the Stock
   List can mint a product with no spool in hand. The split moves the Stock
   List to the service *and* says the service reads products and never writes
   them. Both cannot hold. Either the service may **propose** a product (a
   provisional one the station adopts on next contact, like a foreign tag's), or
   a Stock List row for never-seen filament stays unlinked until a spool of it
   is onboarded and the name probe finds it. *Open — see Decisions.*

## Normalizing names: two options

Only needed if the data from step 1 shows real mismatch. Both keep the
display name OPT-shaped.

- **A — one normalizing function for every source.** Split any incoming name
  into material / variant / colour and canonicalize the vendor. Cleanest
  result, but parsing free-form vendor product names reliably is hard, and a
  wrong split is a silent mis-merge.
- **C — vendor product names only.** Hand entry stops composing
  `<material> <colour>` and instead picks from the database or from existing
  products, with free typing as the last resort. Names then have one shape
  because they have one origin.

Either way a **vendor alias table** ("BambuLab" → "Bambu Lab") is cheap, low
risk, and fixes the vendor half of the problem outright.

## What must not change

- **A tag never updates a product, and now never updates a vocabulary row
  either.** Create only; disagreements reported.
- **A catalog pick's identifiers still reach the tag** — UUIDs, GTIN, and the
  rule that switching back to manual entry clears them first.
- **The spool profile picker starts neutral**, and a manual onboard without a
  profile is still refused (400) before anything is written.
- **A blank field never erases a recorded value** — the rule cost already
  follows. Learning from a source adds data; it does not blank what a person
  entered.

## Decisions to make

1. **Service-side product creation** — propose-and-adopt, or leave unlinked
   rows until a spool arrives. *Open;* decide together with the split.
2. **Option A or C** — *deferred* until step 1 has run long enough to show how
   often names actually disagree.
3. **Should tare join the product key?** Two spool bodies of one product
   (cardboard vs. plastic) would then be two products. Probably *no* — the
   right unit is the spool body, which this design makes a vocabulary row —
   but worth a deliberate answer.
4. **Unknown tare at weigh time** — keep reporting gross (flagged), or report
   nothing until a tare is known? Flagged gross is more useful on the shelf;
   nothing is more honest in the rollup.

## Migration

- First boot on the new firmware emits one vocabulary event per existing
  `/config/` row, the same way `migrateStockIds_()` backfilled Stock List ids.
  Existing spools and products are untouched.
- Spools already carrying tare 0 are not guessed at. They show as *tare not
  recorded* and are fixed on `/product?id=N` or by re-onboarding.

## Sequencing

1. **Local, standalone:** tare 0 shown as unrecorded; tag and database sources
   feed the picklists and spool profiles (create-only); abbreviation required
   or inferred; last-used defaults learn from every path; the
   `cfgMaterialByName(abbr)` name/abbreviation mismatch fixed. Fixes the
   silently wrong remaining weights and makes the sources interchangeable.
2. **Vocabulary into the log,** with the migration above. Still station-only;
   makes `/export` complete.
3. **Vendor alias table, then A or C,** once step 1 shows the size of the name
   problem.
4. **The service reads vocabulary events,** decided together with service-side
   product creation.
