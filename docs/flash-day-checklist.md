# Flash-day checklist

One firmware update and one data fix:

| Step | What | Can it be undone? |
|---|---|---|
| Flash | Install `main` (barcode scanning, Restock, names as labels, and everything merged since) | Yes: restore the before-flash backup on the previous firmware (see *If it goes wrong*) |
| Reconcile | Correct every eSUN product against the eSUN US catalogue, merge duplicates, remove the Cabbage test data | Yes: restore the before-reconcile backup |

They can be the same day or days apart. Do both while no one is weighing: a flash
takes the station down for about a minute, and the reconcile needs **no weighing
from its backup until its restore** (about 15 minutes), or what's weighed in between
is lost.

The old A/B/C plan is gone: the Stock List is already linked (nothing to do on
Repair first), so one flash covers both earlier updates. The data path was rehearsed
in the simulator on 2026-11-06 with the Nov 4 backup.

Commands run in Terminal on the Mac, in the repo folder. If `pio` isn't found, use
`~/.platformio/penv/bin/pio`. `STATION` is the station's fixed IP address (or
`weighstation.local`).

---

## Before you go

- [ ] Mac charged, USB-C cable that fits the station's panel port.
- [ ] `git checkout main && git pull` and note the commit: `git log --oneline -1` ______
- [ ] It builds: `pio run` ends in `[SUCCESS]`.
- [ ] The reconcile tools are ready (sets up Python, fetches the OpenPrintTag
      database, builds the catalogue): `tools/esun-catalogue/make.sh`
- [ ] The simulator builds: `tools/sim/run.sh --build`

## 1. Back up and record what's there now

- [ ] Download both backups to the Mac (not into the repo):
  ```bash
  d=~/WeighStation-backups/$(date +%F)-before-flash; mkdir -p "$d"
  curl -fo "$d/events.ndjson" http://STATION/export
  curl -fo "$d/config.json"   http://STATION/config/export
  ls -l "$d"
  ```
  Both files must be non-empty. (Administration → Backup → Download does the
  same in a browser.)
- [ ] Write down, to compare after the flash:
  - Inventory: number of spools shown ______
  - Administration → Products: "N products on file" ______
  - Stock List: number of items ______
  - Usage: this month's total grams ______
  - One spool you can weigh: spool # ____, remaining ______ g
- [ ] Optional, any time before the reconcile: preview it on this backup.
  `tools/reconcile/rehearse.sh "$d/events.ndjson" "$d/config.json"` builds
  `docs/reference/product-reconciliation-<today>.xlsx` and dry-runs it. Earlier
  decisions carry over; anything new is listed at the end of the run.

## 2. Flash

- [ ] Plug the Mac into the station's USB-C port. `pio device list` shows it.
- [ ] `pio run -t upload` (add `--upload-port /dev/cu.usbmodem…` if it picks the wrong one).
      **Never** `-t uploadfs` or `-t erase`: those wipe the station's data.
- [ ] The display comes back green: "Seattle Makers / Place spool to begin".
      (Orange "Join WiFi" means it didn't rejoin the network; see the manual.)

**Check the flash:**
- [ ] The counts from step 1 match: spools, products, Stock List items, this
      month's Usage.
- [ ] The menu reads Inventory · Reorder · Restock · Usage · Administration.
- [ ] Reorder **will differ** from before: it lists an item once it's below half
      its target, by weight, and shows **To order**. No "not linked" items.
- [ ] Weigh the spool you noted. Remaining is the same, give or take a few grams.
- [ ] Restock → Or enter manually: Save is refused until **This is a new product** is ticked.
- [ ] Download a backup into `~/WeighStation-backups/$(date +%F)-after-flash`.

**Scanner (if it's here):** set it to trigger-only (NETUM manual). On Restock, scan
a box: "Barcode … will be recorded for …" appears. Then click into a text box on
another station page (the station name on Administration, say) and scan: the code
must not land in the box, and a note appears instead. (That guard covers the
station's own pages only; a scan into an email still types.)

## 3. Reconcile

**No weighing from the backup below until the restore at the end.**

- [ ] Fresh backup, the one the reconcile works from:
  ```bash
  r=~/WeighStation-backups/$(date +%F)-before-reconcile; mkdir -p "$r"
  curl -fo "$r/events.ndjson" http://STATION/export
  curl -fo "$r/config.json"   http://STATION/config/export
  ```
- [ ] Build and check the decisions:
  `tools/reconcile/rehearse.sh "$r/events.ndjson" "$r/config.json"`
  Open `docs/reference/product-reconciliation-<today>.xlsx`. Anything the run
  listed as new or changed needs a Decision (*Apply*, *Skip*, or *Change* with a
  note). The dry run must end with **0 skipped**.
- [ ] Apply in the simulator:
  `tools/reconcile/rehearse.sh "$r/events.ndjson" "$r/config.json" --apply`
  It must print **0 skipped**, then `0 applied, 0 skipped` on the re-run, **OK**
  from the check, and **round trip: identical**. If not, stop: nothing has
  touched the station yet.
- [ ] Restore the result to the station: Administration → Backup → Restore,
      first the log `tools/reconcile/work/out/events.ndjson`, then the config
      `tools/reconcile/work/out/config.json`.

**Check the reconcile:** the station must now match the simulator's result exactly.
```bash
PY=tools/esun-catalogue/work/venv/bin/python
$PY tools/reconcile/snapshot.py http://STATION "$r/station-after.json"
$PY -c "import json,sys;a,b=(json.load(open(f)) for f in sys.argv[1:]);print('identical' if a==b else [k for k in a if a[k]!=b[k]])" tools/reconcile/work/after.json "$r/station-after.json"
```
- [ ] It prints `identical`.
- [ ] Spot-check: Products shows eSUN names like *PLA Basic Black*, *PETG Black*,
      *PLA Silk Mystic …*; no Cabbage products; Reorder still has no "not linked".
- [ ] Download a backup into `~/WeighStation-backups/$(date +%F)-after-reconcile`.
- [ ] The two Cabbage test spools (#44 PETG Clear Red, #45 PLA Yellow): erase
      their tags (Administration → Erase Tag) or bin them. Placed again, the
      station would adopt them back.
- [ ] Tags catch up by themselves: each spool's tag is rewritten with its new name
      the next time it is placed. Nothing to do.

## Afterwards

- [ ] Tell Claude the commit flashed and that the reconcile is on. It updates the
      Hub row and the manual.
- [ ] Replace the screen pictures and PDFs in the Notion manual; remove its
      "next update" banner. Reprint the quick references.
- [ ] Staff PC: check `C:\Inventory Backups` got that night's backup.

---

## If it goes wrong

Installing firmware never touches the station's data (the log, Stock List and
picklists live in a separate part of its memory). So a bad install is fixed by
installing again.

- **Upload fails / Mac can't see the station:** try another cable or port; check
  `pio device list`. The station keeps running its old firmware until an upload
  completes.
- **Something looks wrong after the flash:** prefer fixing forward with Claude. If
  you must go back: `git checkout d99c83b && pio run -t upload`, then
  Administration → Backup → Restore the `before-flash` backup. Anything weighed
  since the flash is lost. (Don't go back after the reconcile without first
  restoring `before-reconcile`: its merges are a format the old firmware doesn't read.)
- **The reconcile run stops, skips something, or the round trip differs:** nothing
  has reached the station. Bring `tools/reconcile/work/` and the backup to Claude.
- **The station doesn't match after the restore, or something looks wrong:**
  restore the `before-reconcile` backup (log and config). The station is back
  exactly as it was before step 3.
- **Counts don't match after the flash:** don't touch Repair. Download a backup of
  the current state, then bring both backups to Claude.
- **Reboot loop or blank screen:** unplug the spool from the scale (a bad tag can
  do this), then power-cycle. If it persists, reinstall the previous firmware.
