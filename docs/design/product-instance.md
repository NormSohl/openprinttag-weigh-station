# Design: products and instances

Status: **proposed** — nothing here is built. Written so the data model can be
argued about before code hardens against the current flat one.

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
| 1 | `package_uuid` | the SKU | ✗ |
| 0 | `instance_uuid` | **this spool** | ✓ as `SpoolRecord.uuid` |

And the spec gives a derivation rule that makes this adoptable without vendor
cooperation:

> `material_uuid` — *"If not specified, can be deduced from `brand_uuid` +
> `material_name`."*

So a product identity can be computed from data we already hold. No new tag
field, no dependency on vendors writing UUIDs.

## The four entities

**Brand** → **Product** → **Profile** → **Instance**, where Profile is *not* a
child of Product but a shared table both point into.

| Entity | Holds | Why there |
|---|---|---|
| **Brand** | name | Already just a string; a table only if it grows attributes |
| **Product** | material name, abbreviation, colour (rgba + measured L\*a\*b\*), diameter, print/bed temps, GTIN, default profile | Everything true of *every* spool of this filament |
| **Profile** | tare (`empty_g`), nominal full weight | The physical reel. Shared across products, and independent of them |
| **Instance** | `instance_uuid`, spool number, remaining/used, `needs_ob`, optional tare override | The only things that differ spool to spool |

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

1. `material_uuid` from the tag, if the vendor wrote one;
2. normalised (`brand_name`, `material_name`) — the spec's own derivation, case-
   and whitespace-insensitive;
3. no match → create one.

Step 2 is what makes adoption converge instead of multiply. Without it the
second Prusament PETG Orange creates a second product and the inventory lists
it twice.

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

### New event type

`StoreEv::Product` — an upsert of a product definition, replayed into a
products index like spools are. Products are **definitions, not events**: they
must survive compaction the way `Usage` rows do, so `storeCompact()` has to
carry them into the rewritten log rather than folding them away.

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
product edit says it should be. Aux still records, per spec.

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

## Open questions

1. **Is `package_uuid` (SKU level) worth modelling?** It distinguishes 1 kg from
   5 kg of the same material. Product + Instance may be enough; adding a level
   that is never used costs clarity.
2. **Instance tare override** — worth having, or does per-profile tare plus
   "Capture tare" cover it? Real reels vary a few grams.
3. **Should a tag-derived product be marked provisional** until a human confirms
   it, and should provisional products be excluded from tag write-back?
4. **Measured colour** (`primary_color_lab`, key 59) — add now as three floats
   on the product, or leave the field until there is a spectrometer? The spec
   forbids deriving it from RGB, so it cannot be back-filled by guessing.

## Sequencing

Store first (product entity, index, event type, compaction), then onboarding,
then reorder, then the web pages. Each step is independently shippable because
`product == 0` remains valid throughout.
