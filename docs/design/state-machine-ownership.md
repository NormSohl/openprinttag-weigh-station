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

- **Phase 1 — observability & plan of record (this doc).** Update `device-states.mermaid`
  to real transitions incl. ownership; land the `setState old->new` trace behind
  `STATE_TRACE` (off by default). No behavior change. **← current**
- **Phase 2 — WiFi/idle group → controller.** syncTask posts WiFi events; nfcTask posts
  `TagGone`; controller owns `WiFiSetupMode`/`Idle`/`IdleNoWiFi` and picks the idle variant.
  *This phase alone removes the bug class we hit.*
- **Phase 3 — detection group → controller** (`TagDetecting`/`*TagFound`/`TagReadError`/
  `BlankTagFound`/`AwaitingFormatConfirm`/`FormattingAndRegistering`).
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
