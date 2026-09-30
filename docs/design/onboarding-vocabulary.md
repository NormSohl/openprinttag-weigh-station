# Design: one vocabulary for onboarding

Status: **proposed** (2026-09-28, revised 2026-09-30) — nothing built.
**Station-only:** the hosted service in `station-service-split.md` is on hold
(2026-09-30, on direct user decision), so this plan assumes the station stays
self-contained. What the service would change is kept under *If the service
resumes*, not in the plan.

| step | state |
|---|---|
| 1. `cfgMaterialByName(abbr)` lookup fix | **built** 2026-09-30 (`cfgMaterialByAbbr()`); `4443a99` compiles clean with `pio run`; not yet run on the bench |
| 2. Every tag written the same way (drop the read-only rule for adopted tags), verified on a real Prusament spool | **built** 2026-09-30; compiles clean (`4443a99`), native tests pass; not yet run on the bench. Third-party bench test deferred until a third-party spool is available |
| 3. Tare belongs to the spool | **built** 2026-09-30; compiles clean (`4443a99`), native tests pass; not yet run on the bench |
| 4. Tare required wherever a person or the database supplies it | **built** 2026-09-30, source-only — needs `pio run` and the bench |
| 5. Every source feeds the picklists; spool types replace spool profiles | not started |
| 6. Abbreviation required or inferred | not started |
| 7. Product merge | not started |
| 8. Name normalization (vendor aliases, then option A or C) | not started — wait for data |

## Decisions

All on direct user decision.

- **Tare does not make a product different** (2026-09-28). Sometimes a
  different kind of spool has to be ordered for the same filament. Tare
  belongs to the spool, defaulted from its product.
- **Tare is never unknown** (2026-09-28). It is always supplied — by the tag,
  the database, or a person. Nothing a person or the database creates may be
  stored with tare 0.
- **Every tag is written the same way, whoever made it** (2026-09-30). The
  read-only rule for adopted tags was a guard against format bugs that have
  since been fixed; OPT is public and we follow it. If writing breaks a tag,
  that is a bug in our encoding and gets fixed, not routed around.
- **Tags adopted from a vendor carry a correct tare** (2026-09-30). So the
  weigh keeps taking tare from the tag (`empty_container_weight`), which on
  every tag we write equals the record's.
- **Products are edited only on `/product?id=N`**, as today. Merge is built
  there too.
- **A spool type is identified by spool vendor + type** (2026-09-30), e.g.
  "Seattle Makers Plywood", "eSun Cardboard". See *Spool types*.

## The problem

A spool's descriptive data reaches the station from three sources:

1. **A tag that is already written** — a vendor spool, or one tagged elsewhere
   (adoption, `sync_task.cpp` `productFromMain()` / `identityFromMain()`).
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
| **Tag** | `brand_name` verbatim, or `"Unknown"` | `material_name` verbatim, or `"Unknown"` | verbatim, may be empty | `empty_container_weight` | `nominal_netto_full_weight` |
| **Database pick** | brand `name`, or a prettified slug | the database's `mat.name`, e.g. `"PLA Basic Fire Engine Red"` | `abbreviation`, else `type`, may be empty | container `empty_weight`, **0 if the package has no container**; the tare box overrides | package `nominal_netto_full_weight` |
| **Person** | picklist or typed | composed `<material> <colour>`, e.g. `"PLA Fire Engine Red"` | from the `CfgMaterial` row | from the spool profile (required); the tare box overrides | from the spool profile |
| *"Another spool of X"* | inherited from the product | inherited | inherited | inherited — **the tare box is ignored** | inherited |
| *Stock List row* | picklist, typed, or database | as above | as above | **always 0** (nothing is weighed) | typed |

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
3. **The picklists are not a shared vocabulary.** Materials and colours enter
   only by "+ Add new". A material that arrived by tag or database is never
   offered, so the next hand-entered spool of it is typed afresh — and
   possibly differently.
4. **The abbreviation is the weakest field and the most load-bearing one.** It
   keys the consumption rollup (and falls back to the full display name when
   empty, splitting one bucket per colour), and the catalog and
   "another spool" paths use it to look up the material's class, type and
   temperatures. That lookup, `cfgMaterialByName(abbr)`, compares against the
   `CfgMaterial` **name** field, while the manual path looks up by name — so a
   config row named `"PLA Silk"` with abbreviation `"PLA"` is found by one path
   and missed by the other.
5. **A missing tare is stored as zero, silently.** Remaining is gross minus
   tare, and the weigh (`sync_task.cpp:779`) falls back to gross when tare is
   0 — so the spool body is reported as filament, typically 200–250 g too much,
   and nothing flags it. The sources that can produce it are a database package
   with no container file and a Stock List product.
6. **Spool bodies are learned from one source.** A real tare from a database
   container never becomes a spool profile, so the next hand-entered spool of
   the same body needs one created by hand. `CfgProfile` carries no vendor at
   all — its label is free text, and it bundles the spool's tare with the
   package's nominal weight.
7. **Tare is treated as a product fact, but it is a spool fact.** The same
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
8. **Adopted tags are never written.** *(Fixed by step 2.)* Neither Main nor Aux (`sForeign`,
   `sync_task.cpp:650`, `:795`). So a correction to an adopted spool's record
   never reaches its tag, and our weighings never reach its `consumed_weight`
   — which is what Prusa software reads, and which OPT requires to stay
   writable.
9. **Duplicate products cannot be reconciled.** There is no merge and no
   split. The matcher's own comment (`store.cpp:883`) calls an over-merge
   something "a human can see and split", but no tool exists for either.

## Guiding principle

**Identifiers match; the vocabulary fills in and checks.** OPT already
separates the two: UUIDs and GTIN say *which* product, `brand_name` and
`material_name` are for display. Matching should lean on identifiers wherever a
source has them, and the vocabulary tables exist to supply what a source left
blank and to catch a value that disagrees with what is known — not to be a
second, weaker identity.

It follows that every source, not just hand entry, should feed the vocabulary.

## Every tag is written the same way

The read-only rule for adopted tags (`sForeign`, carried on the record as
`foreign`) dates from 2026-08-14, the day our tags were brought byte-for-byte
into line with the OPT reference layout — a rewrite in our old encoding had
destroyed a Prusa tag while debugging. Everything that made a rewrite
dangerous has been fixed since:

- **Unmodelled fields survive.** `optDecode()` keeps every key we do not model
  in `extra[]` and `optEncodeMain()`/`optEncodeAux()` write them back;
  overflow refuses the rewrite.
- **`write_protection` is respected** (`optMainWritable()`).
- **Writes are bounded by the tag's own declared layout** (`optPayloadExtent()`),
  not the physical size.

So the rule goes: adopted tags get Main reconciled and Aux (`consumed_weight`,
cost) written exactly like ours. The record's `foreign` flag stays as a note of
where the spool came from; it no longer blocks writes.

- **Product edits then reach adopted tags too**, through the normal reconcile
  loop. Products inferred from a tag stay `provisional`, and excluded from
  write-back, until a person confirms them — the guard that matters here.
- **A vendor tag may declare a smaller Aux region than our write needs.** With
  cost, our Aux map is 18 bytes. `writeSection()` refuses such a write cleanly
  rather than overrunning; whether real Prusament tags hit this is one of the
  things the bench test below answers.

**Verify on hardware before relying on it**, with a genuine Prusament spool:

1. `DUMP TAG` before any write.
2. Place it (weigh → Aux write), then edit its record and let it reconcile
   (Main write).
3. `DUMP TAG` again: every field present before is still present and
   unchanged, apart from the ones we meant to write.
4. The tag still reads correctly in Prusa's own app, including the remaining
   weight computed from our `consumed_weight`.

### What step 2 found

Writing vendor tags forced a closer look at what a rewrite actually puts on a
tag, and turned up three gaps that already affected our own tags:

- **Invented fields.** `optEncodeMain()` wrote all four temperatures,
  `material_type`, the weights, the diameter and the abbreviation
  unconditionally, so any tag lacking them came back claiming 0 °C and PLA
  (type 0). `OptMain::present` now records what the tag carried; the encoder
  writes only that plus values a writer actually set.
- **Over-long names.** OPT caps `brand_name` at 31 bytes and
  `material_abbreviation` at 7; our buffers allowed 64 and 16. The encoder now
  clamps (UTF-8 safe), and the reconcile comparison clamps the same way so a
  long name does not rewrite on every placement.
- **Unbounded region writes.** `writeSection()` was bounded only by the end of
  the payload, so a Main longer than its region would overwrite Aux. It is now
  bounded by the tag's own declared region (`optRegionBounds()`).

Also: the store's `"Unknown"` placeholder is no longer written onto tags as a
brand or material name.

## Tare belongs to the spool

*Decided:* tare does not distinguish products. The product carries a
**default** tare; each spool carries its **own**, which starts as the default
and may differ. On the tag it is OPT's `empty_container_weight`, which is
per tag anyway.

- **Onboarding always shows the tare, on every path**, prefilled from the
  product's default (or the spool type picked), and a changed value wins —
  including on "another spool of X", which today ignores it. The spool type
  picker (*Spool types*) is the list to pick from.
- **A product edit does not touch a spool's tare.** `storePropagateProduct()`
  snapshots and re-asserts it, exactly as it already does for cost. Changing
  the product's default affects spools onboarded afterwards.
- **A tag's tare differing from its product's default is not a
  disagreement.** It is that spool's tare. `productDiffers_()` stops comparing
  it (reversing the fix that added it — which was right while tare was a
  product fact, and is the reason to keep a native test covering this).
- **The weigh keeps reading the tag.** On every tag we write, the tag holds the
  record's tare; an adopted vendor tag's is taken as correct as it arrives.
- **Never unknown.** Enforced where a person or the database supplies it:
  - *Hand entry:* already refused (400) without a spool profile; the rule
    becomes "without a tare", from a spool type or typed.
  - *Database pick:* if the package has no container weight, the form
    requires one before saving.
  - *Stock List:* requires a default tare, from the database or typed. It no
    longer creates products with `empty_g` 0.
- **Existing zero tares** (from before this rule) are listed for fixing, not
  guessed at.

## The vocabulary

Four tables, each fed by all three sources:

| table | key | carries | fed by a tag | fed by a database pick |
|---|---|---|---|---|
| **Vendors** | canonical name, plus aliases | `brand_uuid` when known | `brand_name`, `brand_uuid` | brand `name`, `uuid` |
| **Materials** | abbreviation | class, type, diameter, print/bed temps | `material_abbreviation`, temps, class/type | `abbreviation`/`type`, `properties` |
| **Colours** | name | rgba | — (OPT has no colour name) | — |
| **Spool types** | spool vendor + type | tare; the database container UUID when known | — (see *Spool types*) | container brand, name, UUID, `empty_weight` |

- **A source adds, it never overwrites.** Same rule as a tag and a product: a
  tag may create a vocabulary row but not change one, or one odd tag rewrites
  what every later onboarding is offered. A disagreement is reported, the way
  `storeAdoptProduct()`'s `outDiffers` is.
- **Junk never gets in.** `"Unknown"`, empty strings and a 0 tare are not
  vocabulary.
- **The abbreviation is required on every path, or inferred.** Inference can
  come from the vocabulary (a known vendor + product) or the database's `type`;
  failing both, the form asks.
- **The tables stay where they are** — JSON under `/config/`, backed up by
  `/config/export`. Moving them into the event log was proposed for the
  service's upload; with the service on hold it is deferred (*If the service
  resumes*).
- **The last-used defaults stay as they are**, fed by hand entry only.
  Considered and dropped (2026-09-30): the defaults only matter to hand entry,
  a database pick or a tagged spool never uses the picklists, and letting them
  write the defaults would overwrite one workflow's memory with another's.

## Spool types

*Decided 2026-09-30:* a spool type is **spool vendor + type** —
"Seattle Makers Plywood", "eSun Cardboard". It replaces the free-text spool
profile, and it is what the onboarding picker offers.

- **The spool vendor is not the filament vendor.** A Seattle Makers plywood
  spool can carry anyone's filament; an eSun cardboard spool usually carries
  eSun's. So the spool type is its own vendor + type pair, never derived from
  the product's vendor.
- **It carries a tare, not a nominal weight.** How much filament a spool holds
  belongs to the product (the package), not the spool body. Today's spool
  profile bundles both ("Prusament 1kg PETG", tare + nominal), and hand entry
  takes its nominal weight from it. Hand entry therefore needs the nominal
  from somewhere else: the product when one is picked, otherwise its own
  field on the form.
- **Sizes that weigh differently are different types.** Where a vendor makes
  the same body in more than one size, the size is part of the type
  ("Prusament Spool 1kg" / "2kg"), because the tares differ.
- **From the database:** checked 2026-09-30, the OpenPrintTag database has 101
  container entries. 51 name a brand (e.g. "Elegoo Cardboard Spool 1kg",
  brand `elegoo`), each with a UUID; only 35 give an `empty_weight`; about 50
  are generic sizes ("1000g") with neither brand nor weight. So:
  - a container with a brand and a weight proposes a spool type — vendor from
    the brand, type from the name, which a person confirms the first time — and
    the type keeps the container UUID so the next pick of it finds it
    directly;
  - a generic or weightless container proposes nothing, and the person picks
    or adds a spool type, which is also where the required tare comes from.
- **From a tag:** the tag's `empty_container_weight` is that spool's tare, as
  today. The Main fields this firmware reads say nothing about which spool
  body it is, so a tag does not create a spool type.
- **Existing spool profiles** have free-text labels and no vendor. They are
  kept, listed for a person to assign a vendor and type, and dropped from the
  picker once converted. They cannot be converted automatically: "Prusament
  1kg PETG" names a product, not a spool body.

## Duplicate products

Duplicates arise whenever the ladder misses: hand entry falls to the name
rung, and names differ in shape by source.

### Merging B into A

A merge is an **event in the log**, never a rewrite of history. Done on
`/product?id=N`, the only place products are edited.

1. **A person picks the survivor and confirms.** Conflicting fields are shown
   side by side, and the page names the spools that will move, as `/product`
   already does for an edit. No automatic merge: the destructive step takes
   one explicit click, the same rule as erase and audit close.
2. **B's spools are re-pointed to A** — one `Reconcile` each, exactly as
   propagation works. Each keeps its own tare and cost. Their tags follow on
   next placement.
3. **B's identifiers become aliases of A**: its package UUID, GTIN, material
   UUID and names. This is the load-bearing step. Without it the next tag or
   database pick carrying B's identity misses A and re-creates B. Every merge
   teaches the matcher; the ladder checks aliases on each rung.
4. **B becomes a redirect, not a deletion.** Anything still holding B's id
   resolves to A — a Stock List row, an old event during replay. Chains
   resolve (C → B → A). The same idea as a spool tombstone: an identity that
   once existed keeps answering.
5. **Stock List rows that now point at the same product** are shown to a person
   to combine. Two reorder thresholds cannot be merged automatically.

The monthly usage rollup is unaffected — it groups by vendor + abbreviation,
not product — though vendor spellings still need the alias table. Popularity
follows the redirect, so B's history counts toward A.

**Merge and alias events are global and `uuid`-less**, so `storeCompact()`
must re-emit them from live state, exactly as it does Products and audit
markers — or they silently vanish across a fold (see the *Gotchas* in
`CLAUDE.md`). `tools/store/` should cover a merge surviving compaction.

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

Only needed if the data from the earlier steps shows real mismatch. Both keep
the display name OPT-shaped.

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

- **Every tag we write stays OPT-compliant**, so Prusa software and other
  OPT readers can read it. A write that would break that is refused, never
  approximated.
- **A tag never updates a product, and never updates a vocabulary row
  either.** Create only; disagreements reported.
- **A catalog pick's identifiers still reach the tag** — UUIDs, GTIN, and the
  rule that switching back to manual entry clears them first.
- **The spool type picker starts neutral**, and a manual onboard without a
  tare is refused (400) before anything is written.
- **A blank field never erases a recorded value** — the rule cost already
  follows. Learning from a source adds data; it does not blank what a person
  entered.

## Questions

1. **Option A or C** — *deferred* until the earlier steps have run long enough
   to show how often names actually disagree.

## Migration

- Spools and products already carrying tare 0 are not guessed at. They are
  listed for fixing (the Products page, and a spool's own page).
- Records with `foreign` set need nothing: the flag simply stops blocking
  writes. Their tags are brought up to date on next placement.

## Sequencing

Each step is independently shippable.

1. **The `cfgMaterialByName(abbr)` fix.** A few lines; the catalog and
   "another spool" paths look up by abbreviation against the name field.
2. **Every tag written the same way**, then the Prusament bench test above
   before relying on it.
3. **Tare belongs to the spool:** "another spool of X" honours the tare box;
   `storePropagateProduct()` preserves each spool's tare; `productDiffers_()`
   stops comparing tare. Native test in `tools/store/` for the propagation.
4. **Tare required** on hand entry, on database picks with no container
   weight, and on the Stock List; existing zero tares listed.
5. **Every source feeds the picklists and spool types**, create-only, with
   the junk guard. Spool types replace spool profiles (*Spool types*), so this
   step also gives hand entry its own nominal-weight field and lists existing
   profiles for conversion.
6. **Abbreviation required or inferred.**
7. **Product merge** on `/product`, with aliases, redirects, duplicate
   suggestions and compaction survival.
8. **Vendor alias table, then A or C**, once there is data.

Steps 1–3 are the recommended first change set: small, self-contained, and
they fix remaining weights and tags that are wrong today.

## If the service resumes

Decided 2026-09-28 for the hosted service, and parked with it:

- **The service creates products** — created, ordered, then received and
  onboarded at the station. That needs a downward path (the station pulls,
  since nothing can reach in) and a product identity that cannot collide:
  the OPT `package_uuid` where the database supplies one, otherwise one minted
  by whichever side creates it, with the station's integer kept as a local
  index.
- **The service edits and merges products; the station only creates them.**
  Curating products is not a presence task, and only the service would see
  duplicates created on both sides. `/product?id=N` would become read-only on
  the station.
- **The vocabulary moves into the event log**, so the one upload carries it
  and `/export` is complete. Costs recorded when this was proposed: vocabulary
  events should reuse existing `StoreEvent` fields (the ~375-byte union), are
  `uuid`-less and so need re-emission through compaction or keeping by the
  finished-spool filter, and the Settings raw-JSON editors and `/config/import`
  would have to emit events for their differences instead of replacing
  tables.
