# Design: station/service split

Status: **on hold** (2026-09-30, on direct user decision) — nothing built.
Proposed 2026-09-28 and written so the boundary could be argued about before
any code moved. The station stays self-contained for now; the onboarding and
data-entry work continues station-only in `onboarding-vocabulary.md`, which
keeps the service-dependent decisions under *If the service resumes*.

| step | state |
|---|---|
| Upload events to a hosted service (additive, nothing removed) | not started |
| Stock list and ordering owned by the service | not started |
| Remove compaction; the station frees space only by dropping finished spools | not started |
| Delete the station's reporting code | not started |

## The problem

Everything the station knows is on the station, and everything you can do with
it happens on the station's own web app — reachable only from the lab LAN.
Three things follow from that:

- **Inventory can only be managed from inside the building.** Refining the
  stock list, checking what needs ordering, and reading usage all require being
  on the makerspace network, even though none of them involve touching a spool.
- **History has to fit in 2 MB of flash, forever.** So the station carries a
  great deal of machinery whose only job is to preserve history across a fold:
  `storeCompact()` (241 lines), the `Usage` rollup, `storeMaterialPopularity()`
  (138 lines), and the popularity retention floor. Several of the hardest bugs
  this project has paid for lived in that machinery.
- **Reporting runs on a microcontroller.** The `/stock` page replays the entire
  event log on every request.

## Guiding principle

**The station owns the physical present. The service owns the past and the
plan.**

A task that needs a spool on the scale stays on the station. A task that
doesn't, moves. That puts every piece of data where the work that uses it
happens, and it means the station keeps working with no network at all for
everything that requires being in the lab.

### Why this is not a return to Spoolman

This project deliberately removed Spoolman and Prometheus in the local-storage
redesign (`sd-local-ecosystem.md`). The reasons were that the station depended
on an external service to do its job, and stopped working without one.

This design keeps that property. The station remains fully self-sufficient for
weighing, onboarding, auditing and tag reuse. The service is **additive**: it
receives a copy of what happened and takes over only the tasks that never
needed the lab. An outage makes reports stale; it never stops a weigh.

## The split

| stays on the station | moves to the service |
|---|---|
| Weighing and the display | Stock list |
| Onboarding | Ordering / reorder |
| Audits (physical inventorying) | Usage, popularity and cost reports |
| Tag reuse | Full per-spool weigh history |
| Calibration, WiFi setup | |
| Live inventory view | |
| Products (onboarding picks from them) | Products (created here, then ordered) |

The station keeps a local web server for exactly the left column. WiFi setup
and calibration **must** stay local no matter what — you need them precisely
when the network is broken.

## Data ownership

Each thing has exactly one authoritative home. Nothing is edited in two places,
which is what lets this avoid two-way sync entirely.

| data | authoritative | why |
|---|---|---|
| Live spools (identity, tare, remaining) | station | the scale is the only thing that observes them |
| Spool cost | station | entered on the Onboard form (`handleApiOnboard`), a presence task; it ships with the spool's events like any other field |
| Events (weighs, onboards, retires, audits) | station originates, service keeps forever | the station produces them; only the service has room to keep them |
| Products | **both create; the service edits and merges** | the service creates products to order them (decided 2026-09-28); the station creates them when onboarding something new, and "another spool of X" picks from them, a presence task. Needs a downward path and globally unique product identity — see `onboarding-vocabulary.md`, *Products created by the service*. Editing and merging duplicates move to the service (decided 2026-09-28); until then the station's `/product` page edits |
| Vendors, materials, profiles, colours | station | the onboarding picklists need them |
| Stock list | **service** | read only by the Stock List's own pages, CSV and API, by `/reorder`, and by config import — all of which move. Checked 2026-09-28: otherwise `cfgStock` appears only in a boot-log count |
| Audit state | station | an audit is walking the shelf |
| Usage, popularity, cost rollup (`usage_dollars`) | service computes | from complete raw history — no fold to survive |

The config catalog therefore **splits**: the stock-items table leaves the
station; the other four tables stay.

Two things this leaves open are taken up in `onboarding-vocabulary.md`: the
upload ships only the event log, so the service would never see the vendors,
materials, profiles and colours at all (proposed: move them into the log); and
products created on the service for ordering must reach the station before the
spool arrives (decided: the service creates products; the station pulls them).

## Upload: continuous, pushed by the station

**Every event ships as it happens.** It is tempting to back a spool up only when
it is finished, but ordering needs live on-hand weight and reporting needs live
consumption. A spool used over three months would otherwise show its
consumption three months late, and ordering would not know what is on the
shelf. So upload is continuous; *finishing* only governs when the station may
forget a spool (below).

**The station pushes.** It is the always-on device inside the network, and the
lab's NAT means a hosted service cannot reach in to pull.

### Protocol

- **A local cursor.** A byte offset into the event log, in NVS, marking what the
  service has confirmed. The upload task POSTs the lines after it; on a 2xx the
  cursor advances to the end of that batch.
- **Idempotency by line content, not by offset.** If an ack is lost, the station
  resends and the service drops lines it already holds, keyed on a hash of the
  line. Every line already carries a CRC, and a byte-identical line is by
  definition the same event: replaying it twice yields the same state, and the
  consumption delta between two identical weighs is zero. So the wire protocol
  needs no sequence numbers and no server-side high-water mark.
- **Order comes from the log.** Within one station the file order is a total
  order, and batches arrive in that order. The one-second `ts` resolution does
  not matter for a single station. It would for merging several, which is
  deliberately out of scope here (see *Open questions*).
- **Initial import ships the whole existing log**, including any `Checkpoint`
  and `Usage` lines from past compactions — they are the only surviving record
  of what they summarise, so the service takes them as its baseline. After
  that, no checkpoint is ever written again: the station no longer compacts.
- **Pre-NTP events** are stamped in 1970. `periodOf_()` files those as
  `unknown`; the service must do the same rather than invent a month.

### Device side

- **A dedicated upload task, with its own stack.** This is where the change can
  go wrong. A TLS handshake needs several KB of stack and ~40 KB of heap, and
  stack overflow is this project's signature failure — silent reboot loop, no
  useful backtrace. syncTask (8192) already carries WiFi, the reconcile loop and
  the web server's startup. A separate task isolates it, can block on the
  network freely, and gets measured by `STACK` once its handle is added. The
  2 MB of PSRAM covers the heap.
- **There is no HTTP client on the station today** — the Spoolman one was
  deleted, not disabled. This is new code, on top of the ~65% of flash the last
  recorded build used.
- **Certificate validation.** Pin the service's root CA or use the ESP-IDF CA
  bundle. `setInsecure()` is the tempting shortcut, and it defeats the point of
  sending a token over TLS.
- **Configuration on the Settings page**, stored in NVS in the same shape as
  the API key: service URL, per-station token.
- **Visible status.** Last successful upload and backlog in bytes, on Settings
  and `/api/status`. A stalled uploader otherwise looks exactly like a working
  one.

## No compaction: the station forgets only finished spools

**Decided (2026-09-28): the station does not compact.** Its log stops being the
permanent record — the service holds history — so there is nothing left to
preserve across a fold, and no fold. The station frees flash in exactly one way:
once a finished spool's events have all shipped, a pass rewrites the log
**without that spool's lines**.

This is a filter, not a fold, and the difference is the point. Compaction
*synthesises* lines — checkpoints, re-emitted products and audit markers,
`Usage` rows — and every one of those had to be exactly right: products had to
be re-emitted from the live index, popularity lost in-window history across a
fold, and uuid-less audit markers vanished across one (the bug found on
hardware 2026-08-15). **A filter writes nothing new.** Its output is a strict
subset of its input, so it cannot get a synthesised line wrong. Products and
audit markers carry no `uuid`, so a filter that removes lines by spool can never
touch them.

What it still is, honestly: a whole-log rewrite, under the store lock, through a
staging file promoted by rename — the same crash-safe mechanism compaction
uses, and like compaction it runs only while the scale is idle. It is just a
trivial one: copy every line except the dropped spools'.

**The upload rule is automatic.** A finished spool's lines precede its `Retire`,
and the `Retire` must have shipped before the spool is eligible, so every line
the filter removes is before the upload cursor. The unshipped tail is copied
verbatim to the end of the new file, so the new cursor is the new file size
minus its length.

**What storage is bounded by now:** the lines of every *live* spool, kept in
full, plus the tombstones below. Not shelf size alone — a spool accumulates
every weigh over its life — but not time either. At 163 B per weigh line about
11,500 lines fit; 100 live spools averaging 50 weighs each is 5,000, and 200 at
50 is 10,000. Whether that is comfortable is an empirical question about this
lab, which is why it is surfaced rather than assumed (*Watching it work*).

### Which spools are finished

`storeRetireSpool()` is called from exactly two places, and they need different
handling:

- **A rediscovered blank** (`sync_task.cpp`). Fires whenever a chip that used to
  carry a spool comes back blank — reuse mode, `TAGFORMAT`, or a third-party NFC
  tool used entirely outside this firmware. The old `instance_uuid` no longer
  exists anywhere physical, and the next onboarding mints a fresh one. Once its
  `Retire` has shipped, **every** line of the spool can go.
- **Audit close** (`web_app.cpp`, `/api/audit/close`). "Not found on the shelf"
  — and it can come back. A genuine reweigh already un-retires a spool, by
  design. If nothing of it remained, its returning tag would be adopted as a
  stranger and **given a new spool number**, breaking the number people look
  spools up by.

### Tombstones, without writing anything new

For an audit-closed spool, the filter drops every line **except its `Retire`**.
That line already carries the `uuid` and the spool number, so it *is* the
tombstone, and the filter still writes nothing new. About 150 bytes each.

**The hazard, and the rule it needs.** A record rebuilt from a `Retire` alone has
a number and no identity — no vendor, material or tare. Today's known-spool path
pushes the station's record onto the tag whenever the two differ
(`overlayRecordOntoMain()` / `recordDiffersFromMain()` in `sync_task.cpp`). For a
returning tombstoned spool that would overwrite the tag's Main section with
blanks. So: **a spool restored from a tombstone takes its identity from the
tag** — the foreign-adoption path already does exactly this, via
`identityFromMain()` and `storeAdoptProduct()` — keeps its old spool number, and
must never push its empty record onto the tag. A record with no identity is
detectable, since only a tombstone produces one.

## Watching it work

Since there is no compaction to fall back on, the station should make its
storage trajectory visible rather than leaving it to the storage-full banner.
On `LOGSTATS`, the Backup page and `/api/status`:

- live spools, tombstones, and total lines;
- lines per live spool — average and maximum, since one heavily weighed spool
  is what would dominate;
- bytes free, and **days until full at the trailing 30-day rate**.

That last number is the one to watch. It turns "does no-compaction fit this
lab?" from a guess into something read off the Backup page.

## What the station loses

| removed | size today | why it can go |
|---|---|---|
| `storeMaterialPopularity()` | 138 lines | computed by the service over complete history |
| `Usage` rollup and its fold | ~45 lines + fold logic | same |
| `/stock`, `/reorder`, `/usage`, `/usage.csv`, `/api/stock`, `/api/usage` | | pages move to the service |
| `cfgStock` table | | owned by the service |
| `storeCompact()`, entirely — checkpoints, the fold, the retention floor, product and audit re-emission | 241 lines | replaced by the finished-spool filter, which writes nothing new |
| `STORE_LOG_COMPACT_BYTES`, `STORE_LOG_KEEP_EVENTS`, the idle compaction trigger, `COMPACT` | | same |

And one thing gets **faster**: the station no longer replays the whole log on
every `/stock` request, because it no longer serves that page.

**What stays: decoding old lines.** A log written before this change may hold
`Checkpoint` and `Usage` lines. The station keeps *reading* both so an existing
log still replays; it just never writes them again.

## What must not change

- **Every presence task works with no network.** Weigh, onboard, audit, reuse,
  calibrate. This is the property that separates this design from the Spoolman
  era, and any implementation that breaks it is wrong regardless of how the
  service is doing.
- **Spool numbers are never reissued.** The finished-spool lifecycle must not
  let a returning spool get a new number, and the NVS counter must keep
  following the log exactly as `reconcileIdCounter_()` does today.
- **The station never pushes an empty record onto a tag.** A spool restored
  from a tombstone takes its identity from the tag (*Tombstones*).
- **A tag may never update a product** (see `product-instance.md`). Nothing about
  the service changes that. The service creates products (for ordering), and
  it alone edits and merges them once it exists.

## The service

Deliberately not specified in detail — hosting and stack are open. What it must
do:

- **Ingest**: an authenticated endpoint that appends a station's lines, deduped
  by content hash, and acknowledges only after they are durably stored. Scope
  the per-station token to *ingest only* — a leaked token from a cabinet-mounted
  device should be able to append that station's events and nothing else.
- **Replay**: rebuild spool state, consumption and popularity from raw events.
  This is the one place the design introduces **a second implementation of the
  same semantics**, and drift between them would be silent. The rules that
  matter: consumption is the drop in remaining weight between two weighs,
  attributed to vendor + **abbreviation** (not the display name); `Retire` is a
  weigh to zero; popularity excludes stockout days. **Conformance test:** feed
  the same log to `tools/store` and to the service; `DUMP usage` and the
  service's usage report must agree to the gram, and the dollars rollup to the
  cent — `tools/store` already covers that sum.
- **Own the stock list**, and compute ordering from the mirrored live spools
  matched to products.
- **Say how fresh it is.** Every page shows when it last heard from the station.
  Offline, ordering and reporting keep showing last-synced data, which is fine
  as long as it reads "as of 3 hours ago" rather than looking current.

## Failure modes

| failure | effect | bounded by |
|---|---|---|
| Service down | reports stale (and labelled so); presence tasks unaffected | nothing ships, so no finished spool can be dropped and the log only grows |
| Long outage | log reaches the storage-full threshold; the existing banner fires and weighs stop being recorded | ~11,500 lines at 163 B each, ~1.88 MB usable. At 50 placements/day, about 7 months. With no compaction this is the terminal state, so *days until full* is surfaced long before it |
| Returning tombstoned spool | would overwrite its tag with an empty identity | the tombstone rule: identity comes from the tag, never pushed to it |
| Ack lost | batch resent | content-hash dedup |
| Clock never set | events filed as `unknown` on both sides | `periodOf_()` rule |
| Token leaked | attacker can append events as that station | token scoped to ingest; revocable on the service |
| Device and service replay disagree | wrong reports, silently | the conformance test |

## Decisions to make

1. **Standalone reporting.** Does a station with no service configured still
   offer usage, popularity and ordering? *Recommended: no.* Keeping it means
   keeping every piece of machinery this design exists to remove. Without a
   service the station still weighs, onboards, audits and reuses tags; it just
   has no reports.
2. **Audit-closed spools: tombstone or grace period?** *Decided: tombstone* —
   the spool's own `Retire` line, kept by the filter (*Tombstones*). A grace
   period would reintroduce a time-based failure: return a day after it expires
   and the spool comes back renumbered. Tombstones grow at ~150 bytes per
   audit-closed spool; at a few hundred a year that is years of headroom, and
   aging the oldest out is a decision for later.
3. **Hosting and stack for the service.** Open. The requirements above are the
   only constraints.
4. **Can a downloaded backup count as "shipped"?** With no compaction, a station
   that never reaches a service has no way to free space except by the filter,
   and the filter waits for spools to ship. Letting a Backup-page `/export`
   download also count — so finished spools covered by it become droppable —
   would keep a service-less station viable indefinitely, which is the "regular
   external backups" idea in its simplest form. The cost is that the station
   then trusts someone to keep the file. *Open.*

## Open questions

- **Multiple stations.** Out of scope for this design, but it shapes two things
  already here: per-station tokens (so a second station is additive), and
  ordering. With more than one station, events from different stations cannot
  be ordered by one-second timestamps alone, and spool numbers from two NVS
  counters collide. Both need answers before a second station exists, not
  before this ships.
- **Per-spool history on the station.** The spool page's sparkline currently
  replays the station's log. After the change it could show only what is still
  local, or link out to the service. Either is fine; worth deciding
  deliberately.

- **The on-device log format.** *Deferred (2026-09-28) until "days until full"
  says space is a problem.* Measured on a realistic synthetic log (5,000 weighs
  over 90 days, random spool order, noisy weights) in today's exact format:

  | format | bytes/event | vs today |
  |---|---|---|
  | Today's NDJSON | 165 | 1.0× |
  | Lean NDJSON — epoch `ts`, short keys, no `uuid` or derived `used_g` on weighs | 69 | 2.4× |
  | Today's NDJSON, deflate in 4 KB sealed segments | 48 | 3.4× |
  | CBOR records | 32 | 5.1× |
  | Packed binary | 19 | 8.7× |

  If it ever matters, **lean NDJSON first**: most of the size is redundancy (the
  32-char `uuid` repeated on every weigh, the ISO timestamp, long keys), so
  trimming it keeps the log readable and greppable and roughly halves boot
  replay. Its one code change is that weighs key on spool number rather than
  `uuid` in `applyInto_`. Binary and compression cost more than their numbers
  suggest: the log stops being a readable backup; CBOR replay would need the
  tag decoder's guarded accessors, because tinycbor aborts on the malformed
  input a torn write leaves; and compression loses a whole block to a torn
  write unless the active tail stays plain text. Binary sizes are exact; deflate
  ratios will shift on real data.

## Migration

- **Ship first, delete later.** Step 1 below is purely additive, so it can be
  validated against the station's own numbers before anything is removed.
- **Capture the existing history.** The initial import ships the whole current
  log. If `LOGSTATS` shows it has never compacted, the service gets complete raw
  history from day one.
- **The deployed station keeps its spool numbers.** Nothing in this design
  renumbers an existing spool.

## Sequencing

Each step is independently shippable, and each leaves the station fully
working.

1. **Upload.** The station ships events; the service ingests and serves
   read-only reports. Nothing removed. Done when the service's usage matches the
   station's own `DUMP usage` for the same log — the conformance test. **This
   alone delivers remote visibility.**
2. **Stock list and ordering move to the service.** Retire the station's
   `/stock`, `/reorder` and `cfgStock`. This is the step that delivers remote
   *management*.
3. **Remove compaction; drop finished spools.** The filter, tombstones, the
   tombstone rule, and *Watching it work*. Compaction's 900 KB trigger has very
   likely never fired on the deployed station — check `LOGSTATS` — in which case
   removing it changes nothing observable for months. It should still land
   with or after step 1, since the filter can only drop spools that have
   shipped.
4. **Delete the station's reporting code.** The payoff, and safe to do last.
