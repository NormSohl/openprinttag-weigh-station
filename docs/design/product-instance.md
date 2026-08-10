# Design: products and instances

Status: **built** (2026-08-10) — pending bench verification.

| step | state |
|---|---|
| Stop destroying unmodelled tag fields | **done** — `OptMain.extra` passthrough |
| Read OPT identity keys 1–4 | **done** — `package_uuid` / `material_uuid` / `brand_uuid` / `gtin` |
| Product entity, index, event type, compaction survival, matching | **done** — `ProductRecord`, `StoreEv::Product`, `storeFindProduct()` / `storeAdoptProduct()` |
| Foreign-tag adoption resolves a product | **done** — `syncTask`, provisional |
| Web onboard/edit form resolves a product | **done** — not provisional |
| Products page + `/api/products` | **done** |
| Onboarding UI ("another spool of X" vs "a new product") | **done** |
| Product editing + propagation to its spools | **done** — `/product?id=N` |
| Reorder against products | **done** — exact match, name fallback |

**The design is built.** What is left is bench verification, and B2 in
`implementation-plan.md` (importing a catalog from 3dfilamentprofiles.com),
which is a separate piece of work.

**Products are created by the two onboarding paths and edited only on
`/product`.** That page is the one place an update is allowed, because it is
the only one that can honestly say what the edit will touch — it names the
spools first, then propagates on save. Neither the spool form nor a tag may
update a product; see *Authority* below. `product == 0` stays valid, so records
that predate products keep working untouched.

## The problem

Every spool record today is a complete, independent copy of its own identity:

```c
struct SpoolRecord {
    uint32_t spool;  char uuid[33];
    char     vendor[64];  material[64];  abbr[16];
    uint8_t  rgba[4];
    float    dia, empty_g, nom_g;
    float    remaining_g, used_g;
    bool     needs_ob;  char last_ts[25];
};
```

Ten spools of the same filament are ten copies of the same vendor, material
name, colour, diameter, tare and nominal weight. That costs three things:

- **Onboarding is retyped every time.** Nine of those ten forms are identical,
  and each is a fresh chance to pick the wrong spool profile — a mistake that
  silently biases every later remaining-weight, because remaining is
  `gross − tare`.
- **A correction has to be made ten times.** Discover the nozzle temperature is
  wrong and there is no single place to fix it.
- **Facts that belong to a product are stored per spool.** A measured colour is
  the clearest case: measure one spool with a spectrometer and you have
  characterised *the product*, not that spool. There is nowhere to put that.

## Guiding insight

**OpenPrintTag already models this, and we are flattening it.** Keys 0–3 are a
four-level identity:

| key | field | level | we store |
|---|---|---|---|
| 3 | `brand_uuid` | brand | ✗ (name only) |
| 2 | `material_uuid` | **the product** | ✗ |
| 1 | `package_uuid` | the SKU | ✗ — **this is our Product**, since a different size is a different product |
| 0 | `instance_uuid` | **this spool** | ✓ as `SpoolRecord.uuid` |

And the spec gives a derivation rule that makes this adoptable without vendor
cooperation:

> `material_uuid` — *"If not specified, can be deduced from `brand_uuid` +
> `material_name`."*

So a product identity can be computed from data we already hold. No new tag
field, no dependency on vendors writing UUIDs.

## Prerequisite: stop destroying fields we do not model — **DONE** (2026-08-08)

`optEncodeMain()` writes **16 of the spec's 61 Main keys**. Rewriting Main on a
compliant vendor tag therefore *destroys* up to 44 fields the vendor wrote —
GTIN, all four UUIDs, manufactured and expiry dates, density, drying temperature
and time, chamber temperatures, viscosities, certifications, RAL reference,
secondary colours. Silently, with no error, and irreversibly.

It is latent only because an adopted foreign tag's record is built *from* the
tag, so the two agree and the reconcile loop never fires. **Products make
rewrites routine** — a product edit sets `gWriteMainPending` for every spool of
it — so this stops being latent exactly when the work above lands.

Three options:

1. **Never rewrite Main on a tag we did not originate.** Safe, and gives up
   "edit the product, every tag updates itself" for precisely the spools most
   likely to carry rich data.
2. **Preserve unmodelled keys verbatim.** The decoder already walks the Main map
   key by key; record the byte span of each *unrecognised* key/value pair and
   splice those bytes back in on encode, after our own keys and before the map
   closes. No duplication is possible, because a span is only kept for a key the
   switch did not claim.
3. Model all 61 keys. Not realistic, and it would only move the problem to key
   62.

**(2), implemented.** `OptMain.extra[192]` plus `extra_len`; `optDecode()` copies
each unrecognised pair in, `optEncodeMain()` splices them back before closing the
map. 192 is above the largest Main region the layout produces (234 B) less our
own output, so overflow should be unreachable.

The overflow case is answered rather than assumed away: `extra_overflow` is set,
`optMainPreservesAll()` reports it, and `nfcTask` **refuses the rewrite**. Losing
an edit beats losing a vendor's data, and `DUMP TAG` says which happened.

`tools/opt/optfuzz` proves it — a Main map carrying `gtin`, `country_of_origin`
and `density` (uint, text and float, none of which we model) survives decode →
re-encode → decode byte for byte.

## The four entities

**Brand** → **Product** → **Profile** → **Instance**, where Profile is *not* a
child of Product but a shared table both point into.

| Entity | Holds | Why there |
|---|---|---|
| **Brand** | name | Already just a string; a table only if it grows attributes |
| **Product** | material name, abbreviation, colour (rgba + measured L\*a\*b\*), diameter, print/bed temps, GTIN, default profile | Everything true of *every* spool of this filament |
| **Profile** | tare (`empty_g`), nominal full weight | The physical reel. Shared across products, and independent of them |
| **Instance** | `instance_uuid`, spool number, remaining/used, `needs_ob` | The only things that differ spool to spool |

**Profile is separate rather than a product field** because the two vary
independently. Most of the lab is one cardboard reel, so that is one number
maintained once; but a vendor ships different reels for different lines
(eSun 1 kg is 200 g, eSun PLA+ 1 kg is 255 g), and a vendor can change reels
mid-product-life. Folding tare into Product would duplicate the shared case and
still not capture the changing one.

`CfgProfile` and `CfgStock` already exist and are most of Profile and Product
respectively — `CfgStock` has vendor, material, colour, diameter, nominal
weight, SKU, GTIN and pack quantity. This is less new construction than it
looks; it is mostly wiring what is there to spool records.

**Rename the seeded profiles while doing this.** They are named by brand today
— "eSun 1kg", "Overture 1kg" — which made sense when a profile was picked per
spool. As a shared physical-reel table they should be named by the reel:
*"Cardboard 1 kg (200 g)"*. Otherwise the table fills with brand-named entries
that all describe the same reel.

## Identity

**Products get a local auto-increment id**, like spool numbers, backed by the
same NVS counter mechanism. Not a derived string key: a product must survive
being renamed, and `brand|material_name` does not.

**Matching** — when a tag arrives, find its product by, in order:

1. **`package_uuid`** (key 1), if the vendor wrote one;
2. **`gtin`** (key 4), if present;
3. `material_uuid` (key 2) **plus nominal full weight** (key 16);
4. normalised (`brand_name`, `material_name`, nominal full weight);
5. no match → create one.

**`package_uuid` first, not `material_uuid`.** The spec distinguishes them
precisely along the line this design draws:

> key 2 `material_uuid` — *"identifier of the **material**"*, deducible from
> `brand_uuid` + `material_name`
> key 1 `package_uuid` — *"identifier of the **package (product)**"*, deducible
> from `brand_uuid` + **`gtin`**

GTIN is per-SKU: a 1 kg and a 5 kg of the same filament carry different GTINs
and the same `material_name`. Since **a different size is a different product**
here, `package_uuid` *is* our product identity and `material_uuid` is one level
too coarse — matching on it alone would merge the sizes we just decided to keep
apart. Nominal weight is the fallback that recovers the distinction when a tag
carries neither identifier.

Steps 3–4 are what make adoption converge rather than multiply: without them the
second Prusament PETG Orange creates a second product and the inventory lists it
twice.

**All four keys are now read and written** — `OptMain` carries `package_uuid`,
`material_uuid`, `brand_uuid` and `gtin`, and `tools/opt/optfuzz` asserts they
survive decode → re-encode. Note the trap that came with the passthrough above:
a decode case without a matching encode is now *worse* than not modelling the
field, because claiming the key removes it from `extra` and it is then dropped
on the next rewrite.

`tools/store/` tests the ladder natively — see that README for what each rung
is protecting against.

## Storage: spool records stay resolved snapshots

The tempting design — move the shared fields out of `SpoolRecord` and resolve
them through the product on every read — is the wrong one here. It breaks
foreign tags (a genuine Prusament has full identity inline and no local
product), and it forces a migration of every existing record.

**Instead: `SpoolRecord` keeps its fields, and gains a product reference.**

```c
uint32_t product;   // 0 = none (foreign tag, or pre-migration)
```

The existing fields become a **cache of the resolved values**. Products are the
place edits *originate*; the spool record is what everything downstream already
reads.

This falls out well:

- `recordDiffersFromMain()`, `overlayRecordOntoMain()`, `rebuildInventory_()`
  and the ~1 Hz reconcile loop need **no changes at all**.
- Editing a product emits one `Reconcile` event per spool of that product,
  updating their cached copies — which is exactly the existing mechanism that
  rewrites tags on next placement. The "fix it once, every tag updates itself"
  behaviour comes free.
- A spool with `product == 0` behaves precisely as it does today.

The cost is one event per spool per product edit, bounded by spools per product.
At lab scale that is single digits.

### New event type — built

`StoreEv::Product` is an upsert of a product definition, replayed into a
products index like spools are. Products are **definitions, not events**: they
must survive compaction the way `Usage` rows do, so `storeCompact()` carries
them into the rewritten log rather than folding them away.

It emits them from the **live** index, not from the folded region — and unlike
the spool checkpoints, that is not merely safe but required. A Product event is
not a delta: replay is last-write-wins on the whole record, so the live index
already holds the newest definition of every product. A product last defined in
the retained tail gets that same definition replayed on top (a no-op); a product
last defined in the discarded region survives only because of this. Nothing can
revert, because the tail cannot contain an *older* definition than the index
built by replaying it.

That argument is the mirror image of the one for checkpoints, where writing from
the live index **is** wrong — see *Consumption rollup* in `CLAUDE.md`. The
difference is that a checkpoint is a baseline the tail measures deltas against,
and a product definition is not.

The product-id counter is reconciled against the log at boot and on import, for
the same reason the spool counter is, and one sharper: a reissued spool number
duplicates a label, but a reissued *product* number makes two definitions
overwrite each other on replay, and every spool pointing at that number follows
whichever won.

### What must not change

**The consumption rollup must keep snapshotting strings at weigh time.** It
currently records vendor and abbreviation as text in `UsageRow`, deliberately
capturing what was true when the filament was consumed. If it referenced a
product instead, renaming a product would silently rewrite history. This is
already correct — the requirement is not to "improve" it.

## The two onboarding paths

This is the user-visible payoff, and it is what the whole change is for.

**Another spool of X.** Pick the product; everything else is inherited. Tare
and nominal come from the product's profile. One control, no retyping, no
opportunity to pick the wrong profile.

**A new product.** The current full form, plus a profile picker that mostly
selects an existing reel. Defining a new profile is rare — a genuinely new reel.

The blank-tag flow reaches the same place: format the tag, create the instance,
then the Onboard page asks *which product*, offering "new product" as the
second option rather than the only one.

## Foreign tags create products

A foreign tag is not a special case in this model — it is the new-product path
with the form pre-filled from the tag. Decode Main, find-or-create the product
by the matching rules above, create the instance, converge.

The second spool of that product then finds the existing product and inherits
it, which is the behaviour that makes an inventory of genuine Prusament roll up
correctly instead of listing four identical rows.

### Authority: create on first sight, never auto-update

**This is the rule that needs to hold.** Product edits propagate to tags. So if
a foreign tag could also *update* a product, reading one tag would rewrite the
tags of every other spool sharing it — one odd or damaged tag could walk a whole
shelf.

A later tag that disagrees with an existing product therefore:

- keeps the product as it is,
- records the discrepancy,
- writes nothing.

Surface it in the web app for a human to adjudicate. This makes the dangerous
direction impossible rather than merely unlikely.

`write_protection` (key 13) is already honoured — `optMainWritable()` gates the
Main write-back — so a protected vendor tag will not be rewritten even when a
product edit says it should be. Aux still records, per spec. `/product` says so
before you save, rather than letting a skipped tag look like a failed edit.

**Built as:** `/product?id=N`. It names the spools the edit will touch before
you press save, then `storeUpsertProduct()` + `storePropagateProduct()` — one
`Reconcile` per spool, which updates their cached copies and leaves the existing
reconcile loop to rewrite their tags on next placement. Saving also clears
`provisional`, which is precisely what that flag was waiting for: a human
looked at the values.

## Migration

No data loss and no flag day:

- Existing records keep working untouched (`product == 0`).
- Products can be derived on demand by grouping existing records on
  (vendor, abbreviation, colour), or created lazily the next time a spool of
  that filament is onboarded.
- The store is currently empty after `WIPE ALL`, so in practice this is moot on
  the bench unit — but the property matters for a deployed device.

## Reorder gets simpler

`rollUp()` matches stock items against spools by comparing **strings** —
`strcmp(r.abbr, s.material)` after today's fix, which was itself a repair for
`material` changing meaning. With products, a stock item references a product id
and matching is an exact lookup. The "keep N spools of X" threshold becomes a
product-level property, which is what it always was.

## Decisions (2026-08-08)

**1. No package/SKU level.** A different size is a different product. Three
levels: Brand → Product → Profile → Instance.

> **Consequence for identity.** If size distinguishes products, the match key
> cannot be (`brand_name`, `material_name`) alone — a 1 kg and a 5 kg of the
> same filament often carry the identical `material_name` and would merge. The
> spec already draws this line: `package_uuid` (key 1) identifies the *package*
> and is deducible from `brand_uuid` + GTIN, which is per-SKU. So
> `package_uuid` is our product identity; nominal full weight is the fallback
> when a tag carries no identifier. See *Identity* above.

**2. No per-instance tare override.** Tare lives on the Profile, reached through
the Product. An Instance is just `instance_uuid`, spool number, remaining/used
and `needs_ob`.

> **Consequence for "Capture tare".** With no instance-level place to put it,
> the button stops meaning "this spool weighs X empty" and becomes "measure this
> reel" — i.e. it creates or corrects a **Profile**. That is arguably what it
> always should have been, and it is the honest workflow: you weigh one empty
> reel and every product on that reel benefits.

**3. Tag-derived products are marked provisional.** A product created by
adopting a foreign tag is flagged until a human confirms it. Provisional
products are excluded from tag write-back, so adopting a spool can never cause
the station to rewrite a vendor's tag from data it inferred.

**4. `primary_color_lab` (key 59) — add the fields, leave them blank.** The
struct, decode and encode go in now; nothing populates them until someone
measures. Absent on the tag means unmeasured, and `optEncodeMain()` omits the
key entirely rather than writing zeroes — a measured black is `L*=0`, so zero
is a legal value and cannot double as "unknown". A separate `has_lab` flag
carries that instead, because zero-initialised structs must read as unmeasured.

Two things to settle before the *first* measurement, since neither is
recoverable afterwards and neither would surface as an error:

- **Illuminant must be D65 / 2°**, which the spec requires. Nix apps offer a
  choice and several default to D50 — valid numbers, wrong field.
- **Pick one surface.** Wound filament and a printed swatch of the same material
  are different colours (finish, layer lines, translucency). Either is
  defensible; mixing them makes the values meaningless.

## Sequencing

Store first (product entity, index, event type, compaction), then onboarding,
then reorder, then the web pages. Each step is independently shippable because
`product == 0` remains valid throughout.
