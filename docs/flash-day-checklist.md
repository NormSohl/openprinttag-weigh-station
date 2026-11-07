# Flash-day checklist

One firmware update and one data fix, both done at the makerspace from the
**studio PC (or any Windows PC) in Chrome**, with nothing to install. The Mac
stays home: it builds a kit that travels on a thumb drive.

| Step | What | Can it be undone? |
|---|---|---|
| Flash | Install `main` (barcode scanning, Restock, names as labels, …) | Yes: rebuild the previous firmware on the Mac, restore the before-flash backup |
| Catalogue | Load the eSUN US catalogue, which Restock and the Stock List search first | Yes: load another file; it changes no product, spool or Stock List item |
| Reconcile | Correct every eSUN product against the eSUN US catalogue, merge duplicates, remove the Cabbage test data | Yes: restore its before-reconcile backup |

They can be the same visit or different ones. The old A/B/C plan is gone: the
Stock List is already linked, so one flash covers both earlier updates.

---

## At home, on the Mac (Claude does this)

- [ ] Decisions are final in `docs/reference/product-reconciliation-*.xlsx`
      (to refresh them on a newer backup, see `tools/reconcile/README.md`).
- [ ] The eSUN catalogue is current: `tools/esun-catalogue/make.sh` (builds
      `station-catalogue.json` with the spreadsheets).
- [ ] Build the kit:
      `tools/esun-catalogue/work/venv/bin/python tools/reconcile/build_station_kit.py docs/reference/product-reconciliation-2026-11-06.xlsx`
      → `tools/reconcile/work/weigh-station-kit-<commit>/` with `firmware.bin`,
      `station-catalogue.json`, `reconcile.js`, `INSTRUCTIONS.html`, `SHA256.txt`.
- [ ] Copy that whole folder to the thumb drive.

The kit's firmware is built from a clean checkout of `main`; `reconcile.js`
refuses to run on any other firmware, so the two always travel together.

## At the makerspace, on the studio PC

Copy the kit folder to the Desktop and open `INSTRUCTIONS.html` in Chrome. It
walks through, with the exact settings and the expected checksum:

1. **Back up** the station (Administration → Backup) and note a few counts.
2. **Flash** with Espressif's web flasher (`https://espressif.github.io/esptool-js/`):
   `firmware.bin` at **`0x10000`**, one row only, never *Erase Flash*. Power-cycle
   the station; check counts, menu, a weigh, Restock.
3. **Load the eSUN US catalogue:** Administration → Station setup → eSUN US
   catalogue → `station-catalogue.json` → Load. Restock's catalog search then lists
   *eSUN — eSUN US catalogue* first, and works without internet.
4. **Reconcile:** on a station page, F12 → Console, paste `reconcile.js`. It
   checks only; `reconcile.apply()` applies, downloading a backup before and after.
   It must end in green *Result checks out*. A few seconds with nobody at the scale.
5. Erase or bin the two Cabbage test tags (spools #44, #45).

Before the first visit, a minute at the studio PC: F12 opens Chrome's developer
tools (not blocked), and the USB cable reaches the station's panel port and
carries data.

## Afterwards

- [ ] Tell Claude the station's firmware (open `http://STATION/api/status` and
      read `build_version`) and that the reconcile is on. It updates
      the Hub row and the manual.
- [ ] Bring the four reconcile backup files from the studio PC's Downloads home.
- [ ] Replace the screen pictures and PDFs in the Notion manual; remove its
      "next update" banner. Reprint the quick references.
- [ ] Staff PC: check `C:\Inventory Backups` got that night's backup.

---

## If it goes wrong

Flashing never touches the station's data (the log, Stock List and picklists
live in a separate part of its memory), so a bad flash is fixed by flashing again.

- **The flasher can't connect / no port:** another cable (some only charge) or
  port. The station keeps its old firmware until a flash completes.
- **Something looks wrong after the flash:** prefer fixing forward with Claude.
  To go back: Claude builds commit `d99c83b` into a kit; flash it, then restore the
  before-flash backup. Anything weighed since the flash is lost. (After the
  reconcile, restore its *before-reconcile* backup first: its merges use a format
  the old firmware doesn't read.)
- **The reconcile prints FAIL or PROBLEMS:** stop; restore the before-reconcile
  files from Downloads (Administration → Backup → Restore, log then config) and
  bring them to Claude.
- **Counts don't match after the flash:** don't touch Repair. Download a backup
  of the current state, bring both backups to Claude.
- **Reboot loop or blank screen:** lift the spool off the scale (a bad tag can do
  this), then power-cycle. If it persists, flash the previous firmware.
