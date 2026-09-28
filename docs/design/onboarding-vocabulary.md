# Design: one vocabulary for onboarding

Status: **proposed** (2026-09-28) — nothing built. Written to pin down the
problem before choosing a fix; step 1 of *Sequencing* is useful on its own and
does not depend on the rest.

Decided 2026-09-28, on direct user decision:

- **The service creates products.** New products are created there, ordered,
  then received and onboarded at the station. See *Products created by the
  service*.
- **Tare does not make a product different.** Sometimes a different kind of
  spool has to be ordered for the same filament. Tare belongs to the spool,
  defaulted from its product. See *Tare belongs to the spool*.
- **Tare is never unknown.** It is always supplied, by the database or by a
  person. Nothing may store a spool or product without one.
- **The service edits and merges products; the station only creates them.**
  Until the service exists, the station's `/product?id=N` stays the editor
  and gets merge first. See *Who edits a product* and *Duplicate products*.

| step | state |
|---|---|
| Tare required and per spool; every source feeds the picklists and spool profiles | not started |
| Vocabulary moves into the event log | not started |
| Name normalization (alias table, and option A or C) | not started — wait for data |
| Product merge (on the station first) | not started |
| The service reads the vocabulary, creates, edits and merges products | not started — depends on `station-service-split.md` |

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
8. **Tare is treated as a product fact, but it is a spool fact.** The same
   filament can arrive on a different spool body. Today:
   - "Another spool of X" inherits the product's tare and **ignores the tare
     box** (`handleApiOnboard`), so a spool on a different body cannot be
     onboarded correctly by that path at all.
   - A product edit rewrites **every** spool's tare with the product's
     (`storePropagateProduct()`, `store.cpp:977`). Cost is snapshotted and
     re-asserted there for exactly this reason; tare is not.
   - A tag whose tare differs from its product's is reported as a
     *disagreement* (`productDiffers_()`, `store.cpp:1004`), when a different
     spool body is a legitimate reason for it.
9. **Products can be minted with no tare.** The Stock List creates a product
   with `empty_g` 0, because nothing is weighed there.

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
- **Tare is always supplied.** Every path that creates a spool or a product
  must end with a real tare, from the database or from a person — never 0.
  See *Tare belongs to the spool* for where it is enforced.
- **The abbreviation is required on every path, or inferred.** Inference can
  come from the vocabulary (a known vendor + product) or the database's `type`;
  failing both, the form asks.
- **The last-used defaults record the resolved values, whatever the path.**
  After a database pick or a foreign spool, the manual picklists default to
  that vendor and material. The spool profile stays out of it: it starts
  neutral on purpose (2026-09-27), and tare is the one field where a
  remembered default silently sticks to the wrong spool.

## Tare belongs to the spool

*Decided:* tare does not distinguish products. The product carries a
**default** tare; each spool carries its **own**, which starts as the default
and may differ.

- **Onboarding always shows the tare, on every path**, prefilled from the
  product's default (or the spool body picked), and a changed value wins —
  including on "another spool of X", which today ignores it. The spool profile
  picker becomes the list of known spool bodies to pick from.
- **A product edit does not touch a spool's tare.** `storePropagateProduct()`
  snapshots and re-asserts it, exactly as it already does for cost. Changing
  the product's default affects spools onboarded afterwards.
- **A tag's tare differing from its product's default is not a
  disagreement.** It is that spool's tare. `productDiffers_()` stops comparing
  it (reversing the fix that added it — which was right while tare was a
  product fact, and is the reason to keep a native test covering this).
- **Never unknown.** Enforced where each source enters:
  - *Hand entry:* already refused (400) without a spool profile; the rule
    becomes "without a tare".
  - *Database pick:* if the package has no container weight, the form
    requires one before saving.
  - *Service-created product:* the Stock List form requires a default tare,
    from the database or typed. It no longer mints `empty_g` 0.
  - *Foreign tag without `empty_container_weight`:* adopted, but marked
    `needs_ob` so it lands on the Onboard page for a tare, rather than being
    weighed as if the spool body were filament.
- **Existing zero tares** (from before this rule) are listed for fixing, not
  guessed at.

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
2. **Who creates products for filament nobody has onboarded.** *Decided: the
   service.* See the next section.

## Products created by the service

The lifecycle is: a product is created on the service, put on the stock list,
ordered, received, and onboarded at the station as "another spool of X". So
the station must know products it did not create — which reverses the split
doc's "the service reads products, it does not write them."

What that requires:

- **A downward path.** The station pushes and nothing can reach in, so the
  station **pulls** new products — most simply in the response to each upload,
  or a periodic fetch on the same connection. Onboarding still works offline;
  a product created on the service since the last contact just is not offered
  yet, and "a new product" remains available.
- **Identity that cannot collide.** Today a product id is a small integer from
  the station's NVS counter. The service cannot draw from that counter. A
  product needs a globally unique identity — the OPT `package_uuid` where the
  database supplies one, otherwise one minted by whichever side creates the
  product — with the station's integer kept as a local index only.
- **Matching still runs.** A service-created product arriving at the station
  goes through the same ladder as any other source, so a product created on
  both sides converges instead of doubling.
- **A default tare is required** at creation (see above).

## Who edits a product

*Decided 2026-09-28:* **both sides create products; only the service edits
and merges them.** Until the service exists, the station's `/product?id=N`
remains the editor, and merge is built there first.

- **Curating products is not a presence task.** Fixing a product's name or
  folding two entries together needs no spool on the scale, so under the
  split's rule it belongs with the service.
- **The service is the only place duplicates can all be seen.** Once it creates
  products for ordering and the station creates them at onboarding, both will
  create copies of the same filament, and neither can see that alone.
- **One editor means no conflict rule.** Edits and merges travel down the same
  pull path as new products; the station applies them as events, and tags
  follow on next placement through the existing reconcile loop. Offline, they
  simply wait.
- **The station still fixes spools.** A spool attached to the wrong product is
  re-onboarded as "another spool of" the right one. That edits the spool, not
  the product, and needs the spool on the scale.

When the station's editor retires, `/product?id=N` becomes read-only on the
station and links out.

## Duplicate products

Nothing reconciles duplicates today: there is no merge and no split. The
matcher's own comment (`store.cpp:883`) calls an over-merge something "a human
can see and split", but no tool exists for either. Duplicates arise whenever
the ladder misses — hand entry falls to the name rung, names differ in shape
by source, and after the split two sides create products independently.

### Merging B into A

A merge is an **event in the log**, never a rewrite of history.

1. **A person picks the survivor and confirms.** Conflicting fields are shown
   side by side, and the page names the spools that will move, as `/product`
   already does for an edit. No automatic merge: the destructive step takes
   one explicit click, the same rule as erase and audit close.
2. **B's spools are re-pointed to A** — one `Reconcile` each, exactly as
   propagation works. Each keeps its own tare and cost (*Tare belongs to the
   spool*).
3. **B's identifiers become aliases of A**: its package UUID, GTIN, material
   UUID and names. This is the load-bearing step. Without it the next tag or
   database pick carrying B's identity misses A and re-creates B. Every merge
   teaches the matcher; the ladder checks aliases on each rung.
4. **B becomes a redirect, not a deletion.** Anything still holding B's id
   resolves to A — a Stock List row, a service reference, an old event during
   replay. Chains resolve (C → B → A). The same idea as a spool tombstone:
   an identity that once existed keeps answering.
5. **Stock List rows that now point at the same product** are shown to a person
   to combine. Two reorder thresholds cannot be merged automatically.

The monthly usage rollup is unaffected — it groups by vendor + abbreviation,
not product — though vendor spellings still need the alias table. Popularity
follows the redirect, so B's history counts toward A.

Constraints this inherits:

- **Merge and alias events are global and `uuid`-less.** While compaction
  exists they need re-emission from live state like Products; under the split
  the finished-spool filter must keep them.
- **Product identity must be globally unique** before merges can cross
  station and service (*Products created by the service*).

### Finding duplicates

The Products page already exists to answer "is adoption converging?". It
suggests likely duplicates for a person to confirm, never merges them:

- same vendor (after aliases), abbreviation and nominal weight, with a close
  colour;
- names that differ only by added words ("PLA Basic Fire Engine Red" /
  "PLA Fire Engine Red").

### Undoing a merge

Because nothing is rewritten, an unmerge can restore B and its aliases. Which
spools return still takes a person, one spool at a time. Merges should be
rare enough that this is acceptable.

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

1. **Service-side product creation.** *Decided: yes* (2026-09-28).
2. **Who edits a product after creation.** *Decided* (2026-09-28): the
   service edits and merges; the station only creates, and edits only until
   the service exists.
3. **Option A or C** — *deferred* until step 1 has run long enough to show how
   often names actually disagree.
4. **Does tare distinguish products?** *Decided: no* (2026-09-28). Tare is per
   spool, defaulted from the product.
5. **Unknown tare.** *Decided: not allowed* (2026-09-28). Always supplied by
   the database or a person; a foreign tag without one goes to onboarding.

## Migration

- First boot on the new firmware emits one vocabulary event per existing
  `/config/` row, the same way `migrateStockIds_()` backfilled Stock List ids.
  Existing spools and products are untouched.
- Spools and products already carrying tare 0 are not guessed at. They are
  listed for fixing, and a spool with one on the scale is sent to onboarding.

## Sequencing

1. **Local, standalone:** tare required on every path and per spool (the tare
   box honoured on "another spool of X", propagation preserving it, tag tare
   no longer a disagreement, foreign tags without one sent to onboarding);
   tag and database sources feed the picklists and spool profiles
   (create-only); abbreviation required or inferred; last-used defaults learn
   from every path; the `cfgMaterialByName(abbr)` name/abbreviation mismatch
   fixed. Fixes the silently wrong remaining weights and makes the sources
   interchangeable.
2. **Vocabulary into the log,** with the migration above. Still station-only;
   makes `/export` complete.
3. **Product merge on the station:** merge and alias events, redirects, the
   duplicate suggestions on the Products page. Independent of the service,
   and useful now — duplicates already happen.
4. **Vendor alias table, then A or C,** once step 1 shows the size of the name
   problem.
5. **The service reads vocabulary events, creates products, and takes over
   editing and merging,** with the downward product path and globally unique
   product identity. The station's `/product` editor becomes read-only.
