# Design: station/service split

Status: **proposed** (2026-09-28) — nothing built. Written so the boundary can
be argued about before any code moves.

| step | state |
|---|---|
| Upload events to a hosted service (additive, nothing removed) | not started |
| Stock list and ordering owned by the service | not started |
| Finished-spool lifecycle: archive, then drop from the station | not started |
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
| Audits (physical inventorying) | Usage and popularity reports |
| Tag reuse | Full per-spool weigh history |
| Calibration, WiFi setup | |
| Live inventory view | |
| Products (the service reads them) | |

The station keeps a local web server for exactly the left column. WiFi setup
and calibration **must** stay local no matter what — you need them precisely
when the network is broken.

## Data ownership

Each thing has exactly one authoritative home. Nothing is edited in two places,
which is what lets this avoid two-way sync entirely.

| data | authoritative | why |
|---|---|---|
| Live spools (identity, tare, remaining) | station | the scale is the only thing that observes them |
| Events (weighs, onboards, retires, audits) | station originates, service keeps forever | the station produces them; only the service has room to keep them |
| Products | station | "another spool of X" onboarding picks from them, and that is a presence task. Product edits propagate to tags on next placement, which is presence-adjacent too |
| Vendors, materials, profiles, colours | station | the onboarding picklists need them |
| Stock list | **service** | only `/stock` and `/reorder` read it, and both move. Checked: `cfgStock` is otherwise used only for a boot-log count |
| Audit state | station | an audit is walking the shelf |
| Usage, popularity | service computes | from complete raw history — no fold to survive |

The config catalog therefore **splits**: the stock-items table leaves the
station; the other four tables stay.

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
  that, the station never ships a checkpoint again, because compaction only ever
  writes them *before* the cursor (next section).
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

## Compaction: fold state, not history

The station's event log stops being the permanent record and becomes two
things: the current state of live spools, and a queue of events the service has
not yet confirmed. Existing compaction already produces nearly that shape — one
checkpoint per spool, then a verbatim tail — so this simplifies it rather than
replacing it.

**The invariant: compaction never discards a line after the upload cursor.**
The fold boundary becomes the earlier of its normal boundary and the cursor.
Compaction then translates the cursor into the new file: the unshipped lines are
copied verbatim to its end, so the new cursor is the new file size minus their
length.

Two consequences worth stating:

- **An outage does not block compaction.** Everything already shipped can still
  be folded; only the unshipped tail is pinned. That is stronger than the
  current popularity retention floor, which is disabled when the clock is not
  set — the cursor rule does not depend on the clock at all.
- **What gets deleted.** Once the service holds history, the `Usage` fold and
  the popularity retention floor exist for no remaining reason. See *What the
  station loses*.
- **What must stay.** Products and audit state are still station-owned, so
  compaction keeps re-emitting both from the live index exactly as it does
  today. The audit case matters: an audit in progress across a compaction is
  the bug found on hardware 2026-08-15, and moving history to the service does
  nothing to make it safe to drop.

## Finishing a spool

Device storage becomes **bounded by shelf size rather than by time**: it holds
the spools physically in the studio, not every weigh since the station was
installed. That requires letting the station forget finished spools.

"Finished" already has exactly two triggers — `storeRetireSpool()` is called
from precisely two places — and they need different handling:

- **A rediscovered blank** (`sync_task.cpp`). Fires whenever a chip that used to
  carry a spool comes back blank — reuse mode, `TAGFORMAT`, or a third-party NFC
  tool used entirely outside this firmware. In every case the old
  `instance_uuid` no longer exists anywhere physical, and the next onboarding
  mints a fresh one, so it can never be seen again. Once the retire has
  shipped, the spool can be dropped at the next compaction.
- **Audit close** (`web_app.cpp`, `/api/audit/close`). "Not found on the shelf"
  — and it can come back. A genuine reweigh already un-retires a spool; that is
  a documented, deliberate behaviour. If the spool had been dropped, its
  returning tag would be adopted as a stranger and **given a new spool number**,
  breaking the number people look spools up by.

So audit-closed spools need either a **grace period** before being dropped, or a
**tombstone** that survives the drop. See *Decisions to make*.

## What the station loses

| removed | size today | why it can go |
|---|---|---|
| `storeMaterialPopularity()` | 138 lines | computed by the service over complete history |
| Popularity retention floor in `storeCompact()` | | protects history the service now holds |
| `Usage` rollup and its fold | ~45 lines + fold logic | same |
| `/stock`, `/reorder`, `/usage`, `/usage.csv`, `/api/stock`, `/api/usage` | | pages move to the service |
| `cfgStock` table | | owned by the service |
| Much of `storeCompact()`'s complexity | of 241 lines | only state is folded now; product and audit re-emission stay |

And one thing gets **faster**: the station no longer replays the whole log on
every `/stock` request, because it no longer serves that page.

## What must not change

- **Every presence task works with no network.** Weigh, onboard, audit, reuse,
  calibrate. This is the property that separates this design from the Spoolman
  era, and any implementation that breaks it is wrong regardless of how the
  service is doing.
- **Spool numbers are never reissued.** The finished-spool lifecycle must not
  let a returning spool get a new number, and the NVS counter must keep
  following the log exactly as `reconcileIdCounter_()` does today.
- **A tag may never update a product** (see `product-instance.md`). Nothing about
  the service changes that; the service reads products, it does not write them.

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
  service's usage report must agree to the gram.
- **Own the stock list**, and compute ordering from the mirrored live spools
  matched to products.
- **Say how fresh it is.** Every page shows when it last heard from the station.
  Offline, ordering and reporting keep showing last-synced data, which is fine
  as long as it reads "as of 3 hours ago" rather than looking current.

## Failure modes

| failure | effect | bounded by |
|---|---|---|
| Service down | reports stale (and labelled so); station unaffected | the unshipped tail grows in flash |
| Long outage | tail reaches the storage-full threshold, existing banner fires | ~11,500 events: a weigh line is 163 B and ~1.88 MB is usable. At 50 placements/day, about 7 months |
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
2. **Audit-closed spools: tombstone or grace period?** *Recommended: tombstone.*
   A tombstone (`instance_uuid` → spool number, ~50 bytes) survives the drop, so
   a spool that turns up after any length of time gets its number back, and the
   tag itself carries the rest of its identity. A grace period is simpler but
   reintroduces a time-based failure: return one day after it expires and the
   spool comes back renumbered.
3. **Hosting and stack for the service.** Open. The requirements above are the
   only constraints.

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
3. **Finished-spool lifecycle.** Tombstones, and dropping finished spools at
   compaction. This is what bounds the station's storage by shelf size.
4. **Delete the station's reporting code.** The payoff, and safe to do last.
