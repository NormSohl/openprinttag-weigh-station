# Flash-day checklist

Two firmware updates, done in order. **A** and **B** happen together.
**C** can be the same day or later. Do the flash steps while no one is
weighing; each flash takes the station down for about a minute.

| Step | What | Can it be undone? |
|---|---|---|
| A | Install the barcode update (`barcode-onboarding`) | Yes, until the first merge on Repair |
| B | Link the Stock List on Administration → Repair | Links can be changed; a merge can be undone on Repair, but don't install older firmware after one |
| C | Install the names-as-labels update (`names-are-labels`) | No: restore a backup instead (see *If it goes wrong*) |

Commands are run in Terminal on the Mac, in the repo folder. If `pio` isn't
found, use `~/.platformio/penv/bin/pio`. `STATION` below is the station's
fixed IP address (or `weighstation.local`).

---

## Before you go

- [ ] Mac charged, USB-C cable that fits the station's panel port.
- [ ] `git fetch` and confirm both branches are current:
      `git log --oneline -1 origin/barcode-onboarding origin/names-are-labels`
- [ ] Both build: `git checkout barcode-onboarding && pio run`, then the same
      for `names-are-labels`. `[SUCCESS]` on each.

## 1. Back up and record what's there now

- [ ] Download both backups to the Mac (not into the repo):
  ```bash
  d=~/WeighStation-backups/$(date +%F)-before-A; mkdir -p "$d"
  curl -fo "$d/events.ndjson" http://STATION/export
  curl -fo "$d/config.json"   http://STATION/config/export
  ls -l "$d"
  ```
  Both files must be non-empty. (Administration → Backup → Download does the
  same in a browser.)
- [ ] Write down, so you can compare afterwards:
  - Inventory: number of spools shown ______
  - Administration → Products: "N products on file" ______
  - Stock List: number of items ______
  - Usage: this month's total grams ______
  - Reorder: the items listed ______
  - One spool you can weigh: spool # ____, remaining ______ g

## 2. Rehearse on the simulator (15 minutes, before flashing)

- [ ] Load the backup into the simulator, on the barcode branch:
  ```bash
  git checkout barcode-onboarding
  tools/sim/run.sh &          # builds, then serves http://localhost:8080
  # wait until it's serving, then:
  tools/sim/load.sh "$d/events.ndjson" "$d/config.json"
  ```
- [ ] Open Administration → Repair. Note which Stock List items it offers
      to link, and whether each ★ choice is right. Note any possible
      duplicate products. **Decide the links here**, where nothing is real.
- [ ] Spot-check Inventory, Reorder and Usage against the numbers in step 1.
- [ ] Optional: do the links in the simulator, stop it, switch to
      `names-are-labels`, restart and reload, and check Reorder shows no
      "not linked" items you didn't expect.

## A. Install the barcode update

- [ ] `git checkout barcode-onboarding && git log --oneline -1` (note the commit: ______)
- [ ] Plug the Mac into the station's USB-C port. `pio device list` shows it.
- [ ] `pio run -t upload` (add `--upload-port /dev/cu.usbmodem…` if it picks the wrong one).
      **Never** `-t uploadfs` or `-t erase`: those wipe the station's data.
- [ ] The display comes back green: "Seattle Makers / Place spool to begin".
      (Orange "Join WiFi" means it didn't rejoin the network; see the manual.)

**Check A:**
- [ ] The counts from step 1 match: spools, products, Stock List items,
      this month's Usage.
- [ ] Weigh the spool you noted. Remaining is the same, give or take a few grams.
- [ ] The menu reads Inventory · Reorder · Restock · Usage · Administration.
- [ ] Administration → Repair opens.
- [ ] Download a fresh backup into `~/WeighStation-backups/$(date +%F)-after-A`.

## B. Link the Stock List

- [ ] Administration → Repair. Choose the links you decided on in step 2,
      press **Link the items chosen**.
- [ ] Merge duplicates only if you're sure. After a merge, don't go back to
      older firmware.
- [ ] Reorder: no "not linked" items left, or only ones you meant to leave.
- [ ] Download a backup into `~/WeighStation-backups/$(date +%F)-before-C`.

**Scanner (if it's here):** set it to trigger-only (NETUM manual). On
Restock, scan a box: "Barcode … will be recorded for …" appears. Then
click into a text box on another station page (the station name on Administration, say)
and scan: the code must not land in the box, and a note appears instead.
(That guard covers the station's own pages only; a scan into an email still types.)

## C. Install the names-as-labels update

Only after B. Before it, unlinked items would drop off Reorder.

- [ ] `git checkout names-are-labels && git log --oneline -1` (note: ______)
- [ ] `pio run -t upload`. Green idle screen again.

**Check C:**
- [ ] Counts from step 1 still match. Usage's total for this month is unchanged.
- [ ] Reorder: same items as after B; any unlinked item is listed as unlinked.
- [ ] Weigh the noted spool once more. Remaining unchanged.
- [ ] Restock → Or enter manually: Save is refused until **This is a new product** is ticked.
- [ ] Download a backup into `~/WeighStation-backups/$(date +%F)-after-C`.

## Afterwards

- [ ] Tell Claude the commits flashed. It merges both branches to main and
      updates the Hub row.
- [ ] Replace the screen pictures and PDFs in the Notion manual; remove its
      "next update" banner. Reprint the quick references.
- [ ] Staff PC: check `C:\Inventory Backups` got that night's backup.

---

## If it goes wrong

Installing firmware never touches the station's data (the log, Stock List
and picklists live in a separate part of its memory). So a bad install is
fixed by installing again.

- **Upload fails / Mac can't see the station:** try another cable or port;
  check `pio device list`. The station keeps running its old firmware until an
  upload completes.
- **After A, something looks wrong and nothing has been merged:** put the
  previous firmware back:
  `git checkout d99c83b && pio run -t upload`. Data is untouched.
- **After A, wrong *and* a merge was done:** undo the merge on Repair first,
  then go back as above. Or fix forward with Claude.
- **After C, something looks wrong:** prefer fixing forward. If you must go
  back: install `barcode-onboarding`, then Administration → Backup →
  Restore the `before-C` backup. Anything weighed since C is lost.
- **Counts don't match after a flash:** don't touch Repair. Download a backup
  of the current state, then bring both backups to Claude.
- **Reboot loop or blank screen:** unplug the spool from the scale (a bad tag
  can do this), then power-cycle. If it persists, reinstall the previous firmware.
