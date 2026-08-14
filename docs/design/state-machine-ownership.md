# State-machine ownership refactor

**Status:** approved, phased implementation in progress (Phase 1).
**Goal:** make `gState` have a single owner. Today two autonomous tasks write it with no arbiter, and every device-state bug so far has lived in that seam.

## The problem

`gState` (defined in `main.cpp`, guarded by `gStateMutex`) is written by **two** tasks,
split by *which task does the work* for each state:

| Task | Domain | States it writes |
|---|---|---|
| `nfcTask` | PN5180 / SPI: detect, read, classify, physical Main/Aux writes | `TagDetecting`, `TagReadError`, `BlankTagFound`, `AwaitingFormatConfirm`, `FormattingAndRegistering`, `ValidTagFound`, `Present` (after a reconcile write), and idle-returns via `idleState()` |
| `syncTask` | WiFi + web + store | `WiFiSetupMode`, `Idle`, `IdleNoWiFi`, `ForeignTagFound`→`RegisteringForeignTag`, `WeighingAndSync`, `Present`, `ReconcilingMainSection` |

`scaleTask` and `displayTask` only **read** `gState` (display is a clean mirror — it
renders whatever it reads). The bug surface is entirely the two writers.

They also hand off through side channels:

- `gWriteMainPending` / `gWriteAuxPending` — syncTask → nfcTask physical-write requests.
- `gTagMain` / `gTagAux` / `gTagMeta` — the decoded tag, `gTagMutex`-guarded, read and written by both.
- `gWeightGrams` — scaleTask → syncTask.
- `gSpoolId` / `gSpoolNeedsOnboarding` — display mirrors.
- `sSnapshot` / `sSpoolId` — syncTask-local; `sSpoolId` mirrors `gSpoolId`.

**Why the split exists (real constraints, not accident):**

1. PN5180 calls can block unboundedly (documented in the gotchas). NFC I/O must be
   isolated so a hung reader cannot freeze WiFi / web / display.
2. Store ops (LittleFS) are heavy and live with syncTask, which also runs the web app.
3. `displayTask` must stay responsive; it and `nfcTask` are the only SPI users.

**The failure mode:** no arbiter. A state one task sets, the other can overwrite. Recent
bugs, all in this seam:

- nfcTask's loop *tail* (meant for `Present`/`WeighingAndSync`/`ReconcilingMainSection`)
  was guarded on `!tagPresent`, not on state, so `Boot`/`WiFiSetupMode` fell into it and
  it drove `gState` to idle ~2×/sec — the "WiFi Setup" screen never appeared (fixed `d220abe`).
- Idle-returns hardcoded `Idle`, ignoring SoftAP mode (fixed, same commit, via `idleState()`).
- `WiFiSetupMode` set at the top of syncTask flashed a portal that wasn't up (fixed earlier, `d16429c`).

Each fix made the two writers *agree* in one more case. The seam remains.

## Target architecture: one owner, workers do I/O behind queues

A new **`controllerTask`** becomes the **sole writer of `gState`** and holds the whole
transition function as one single-threaded switch. The other tasks become I/O workers
that report *facts* and never touch `gState`.

```
 scaleTask ──gWeightGrams──▶ ┌──────────────┐ ──cmd queue──▶ nfcWorker (PN5180/SPI)
                             │ controllerTask│ ◀─reply queue── (POLL/READ/WRITE/FORMAT
 syncTask ──wifi events────▶ │  OWNS gState  │                  → TagPresent/TagRead/
   (WiFi+web+compaction)     │  one switch   │                    WriteResult/TagGone)
                             └──────┬───────┘
                                    │ gState (write)
                                    ▼
                              displayTask (read-only mirror)
```

- **`nfcTask` → NFC worker.** No longer autonomous. Waits on a command queue
  (`POLL`, `READ`, `WRITE_MAIN`, `WRITE_AUX`, `FORMAT`), runs it on the SPI bus, posts a
  result to a reply queue (`TagPresent(uid)`, `TagGone`, `TagRead(class, main)`,
  `WriteResult(ok)`). The controller waits on the reply **with a timeout**, so a hung
  PN5180 becomes a `TagReadError` instead of a wedged controller. (Residual: a hung call
  still holds `gSpiMutex` and can starve the display — that is the separate "bounded wait
  in a vendored PN5180" item, not solved here, but the controller no longer wedges.)
- **`syncTask` stays** but sheds all tag logic: WiFi lifecycle + web + compaction only. It
  **posts WiFi events** (`PortalUp`, `Joined`, `SoftAP`, `Dropped`) to the controller.
- **Store actions move into the controller** (the store is a self-locking library, safe to
  call directly). The nfc↔sync weigh/reconcile ping-pong becomes straight-line code.
- **`scaleTask` / `displayTask`: unchanged.**

The event/request interface is FreeRTOS queues: one cmd + one reply queue for the NFC
worker, one event queue for WiFi. `gState` is written in exactly one place.

## Phased migration

Each phase compiles, flashes, and is bench-tested before the next. Serial tooling:
`DUMP TAG` prints live `gState`; the `STATE_TRACE` compile flag logs every `old -> new`.

- **Phase 1 — observability & plan of record (this doc). DONE.** `device-states.mermaid`
  redrawn to real transitions incl. ownership; `setState old->new` trace behind
  `STATE_TRACE` (off by default). No behavior change.
- **Phase 2 — WiFi/idle group → controller. DONE, validated on hardware 2026-08-13.**
  A `controllerTask` (its own file) is the sole writer of `WiFiSetupMode`/`Idle`/
  `IdleNoWiFi`. syncTask posts `WifiPortalUp`/`WifiJoined`/`WifiSoftAP`; nfcTask posts
  `ReturnToIdle` (replacing `idleState()`), and the controller picks `Idle` vs
  `IdleNoWiFi` from the WiFi mode it tracks. Verified with `STATE_TRACE`: `[ctrl] boot
  -> idle` on join; placing a spool → `present` with the display updating; removing →
  `[ctrl] -> idle`. The idle/WiFi trio now has exactly one writer. (`Present` is still
  written by both nfc and sync — pre-existing, addressed in Phase 4/5.)
- **Phase 3 — detection group → controller. DONE, validated on hardware 2026-08-13.**
  nfcTask no longer reads `gState` for the tag lifecycle: it runs a local `NfcPhase`
  machine (`Waiting`/`Debouncing`/`BlankDetected`/`AwaitingConfirm`/`Formatting`/`Held`/
  `ErrorHeld`) and reports facts — `TagDetecting`/`TagBlank`/`AwaitConfirm`/
  `FormatConfirmed`/`TagValid`/`TagReadErr` — which the controller turns into the matching
  gState. The old four `gState`-keyed removal checks collapsed into one `Held`-phase check.
  Validated: known-spool weigh (`Debouncing → TagValid → Held → present → ReturnToIdle`);
  blank onboarding (`BlankDetected → AwaitingConfirm` 5 s countdown `→ Formatting → TagValid`,
  UUID minted + stub #9 created). The `Boot`/`WiFiSetupMode` no-poll gate still reads `gState`
  (read-only). **Remaining nfc `gState` write:** the `ReconcilingMainSection → Present` line —
  a weigh state, deliberately left for Phase 4.
- **Phase 4 — weigh/reconcile group → controller. DONE, validated on hardware 2026-08-13.**
  syncTask now drives a local `SyncPhase` machine (`Resolving`/`Foreign`/`Registering`/
  `Weighing`/`Holding`/`Reconciling`) and posts facts — `StubReady`/`BeginWeigh`/
  `SpoolForeign`/`ForeignRegistering`/`Weighed`/`NeedsReconcile` — plus nfcTask posts
  `ReconcileDone`. The controller writes every weigh state. **The local phase was
  load-bearing, not cosmetic:** the old blocks ran back-to-back with no delay and relied
  on synchronous `setState` to advance; posting events while keying on `gState` would let
  syncTask re-enter the weigh block on gState lag and append a DUPLICATE weigh — corrupting
  the consumption rollup (primary data). Advancing `sphase` synchronously prevents it.
  Verified on hardware: a foreign adopt + weigh wrote exactly 3 log lines (one Weigh) and
  the count stayed stable while the spool sat — no re-weighing.

  **`gState` now has exactly one writer: the controller.** nfcTask and syncTask only post
  events and read `gState`; both their `setState()` helpers are deleted. The two-writer
  seam that motivated this whole refactor is closed.
- **Phase 5 — command-driven NFC (optional).** Fold the PN5180 I/O behind the controller's
  own cmd/reply queues with bounded waits, so a hung reader times out instead of holding
  the bus. This is the piece that also addresses the PN5180-hang-freezes-display quirk;
  it is a further cleanup, not required for single-writer (already achieved). **← next**
- **Phase 4 — weigh/reconcile group → controller** (store + weigh-append + reconcile move in).
- **Phase 5 — physical writes**: replace `gWrite*Pending` flags with `WRITE_*` commands +
  `WriteResult` replies; NFC becomes fully command-driven.

## Risks

- Biggest control-flow change in the codebase; real regression risk on hardware — hence
  the phasing and per-phase bench validation.
- Queue hops add ~ms latency — irrelevant for this device.
- Controller must never block on the store while a tag awaits a decision; it is
  single-purpose, and display is a separate task, so latency there is not user-visible.
- Compile + flash + bench-verify happen on the physical board each phase; this is several
  focused sessions, not one.

## Bench checks per phase

Place a blank tag (onboard countdown → stub), a foreign tag (adopt → weigh), a known spool
(weigh), a web edit (reconcile rewrites the tag), and WiFi setup (portal screen holds, join,
green idle). `DUMP TAG` + `STATE_TRACE` confirm each transition fires from the controller.
