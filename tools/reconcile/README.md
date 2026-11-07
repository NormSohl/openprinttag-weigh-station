# Reconcile the station's products with the eSUN US catalogue

Gives every eSUN product on the station the name, vendor and identity of the eSUN
US item it really is, merges products that turn out to be the same item, makes the
Stock List labels follow, and removes test data. It runs in the simulator against
a backup; the station gets the corrected backup back through Administration →
Backup → Restore. Nothing here talks to the station itself.

Designed with the lab on 2026-11-06 for the `names-are-labels` firmware, where a
product's name is only a label: renaming is safe, and Usage relabels itself.

## On the day

1. **Back up the station** (Administration → Backup, or
   `curl -o events.ndjson http://STATION/export` and
   `curl -o config.json http://STATION/config/export`). **No weighing from here
   until the restore in step 5**: anything weighed in between is lost.
2. Flash the firmware as usual. Check out the branch it was built from.
3. `tools/reconcile/rehearse.sh events.ndjson config.json`
   builds `docs/reference/product-reconciliation-DATE.xlsx` and dry-runs it.
   Decisions carry over from the newest earlier `product-reconciliation-*.xlsx`
   where the proposal is unchanged; anything new or changed is listed at the end of
   the run and defaults to *Apply*.
   Read it; change Decision to *Skip* or *Change* where needed.
4. `tools/reconcile/rehearse.sh events.ndjson config.json --apply`
   applies the decisions in the simulator, re-runs them (must change nothing),
   checks the result against the spreadsheet (`verify.py`), then exports it and
   restores it into a second, fresh simulator, which must come back identical.
5. Restore `tools/reconcile/work/out/events.ndjson` and `config.json` to the
   station (Administration → Backup → Restore, log and config).

## At the makerspace without the Mac: the station kit

`build_station_kit.py decisions.xlsx [--commit main]` builds a folder for a thumb
drive: `firmware.bin` (from a clean checkout of the commit), `reconcile.js`,
`INSTRUCTIONS.html` and `SHA256.txt`. Any Windows PC with Chrome does the rest:
Espressif's web flasher writes the firmware at `0x10000`, and `reconcile.js`,
pasted into Chrome's console on a station page, applies the decisions in that
page through the station's own API. It is the same logic as `apply.py` plus
`remove_vendor.py` (test vendors are removed by fetching the log, dropping their
lines and loading it back through Restore), with the same starting-point checks,
a check-only pass first, backups downloaded before and after, and a final check.
It refuses to run unless the station reports the kit's firmware build.

Tested 2026-11-06 in the simulator: on the Nov 4 backup it reaches a state
identical to `rehearse.sh --apply`; with weighs, a new spool and a hand-renamed
product added afterwards, it keeps the weighs and the new spool, skips the renamed
product, and applies the rest. `window.RECONCILE_NO_DOWNLOAD = true` before
pasting suppresses the downloads (for tests).

## The rules

0. **Lab corrections** in `overrides.json` win (e.g. #21's PLA+HS barcode, really
   PLA+ Magenta).
1. **The barcode decides.** A product with a barcode is the eSUN item that barcode
   belongs to: on the eSUN US list; else via its eSUN part number from Lieske
   (older or EU packaging); else in the OpenPrintTag database. This overrides a
   name someone typed (PLA+ on a PLA Basic barcode becomes PLA Basic again).
2. **No barcode:** the product's OpenPrintTag package ID, else its current name.
   A product renamed without a barcode keeps its name and gets no barcode added:
   that would turn a guess into a fact.
- **New name** = material + colour (+ packaging if not a standard 1 kg spool:
  "Refill", "3 kg"), vendor `eSUN`. Products with no eSUN US item keep their name;
  other vendors are untouched. The barcode a product already has is never changed.
- **Duplicates** are products on the same eSUN part number. The keeper has more
  spools, then a barcode on the eSUN US list, then more filament. A merge (Repair's)
  moves spools, each keeping its own tare and cost, and the loser's barcode becomes
  an alias that finds the keeper.
- **Test vendors** listed in `overrides.json` (`remove_vendors`) are removed from
  the backup before loading (`remove_vendor.py`): the station has no delete. Their
  physical tags would be re-adopted if placed again; erase or bin them.

## How changes are applied (`apply.py`)

Through the station's own web API, as a person would: Repair's merge, the product
edit (which also renames every spool of the product; tags follow on next
placement), and the Backup page's config import for Stock List labels (the
raw-table save refuses the whole Stock List as too large). Each change first checks
the station still has what the spreadsheet saw, and is skipped and reported if not;
one already done is reported as done. `--dry-run` writes nothing.

Expected side effect: a spool recorded with **no tare** takes its product's tare
when the product is saved, and its remaining weight drops by that much. That
corrects it (the empty spool was being counted as filament) and is not booked as
usage. On the Nov 4 backup this is spool #96, 1200.4 → 1030.4 g.

## Files

| | |
|---|---|
| `rehearse.sh` | The whole sequence above. |
| `remove_vendor.py` | Removes a vendor's products and spools from a backup. |
| `match_products.py` | Applies the rules → `work/recon.json`. |
| `build_decisions.py` | The decision spreadsheet, carrying decisions over. |
| `apply.py` | Applies the decisions to a simulator (or station) URL. |
| `snapshot.py`, `verify.py` | Before/after state and the checks. |
| `overrides.json` | Lab corrections and test vendors. |

Needs `tools/esun-catalogue/make.sh` to have run (`rehearse.sh` runs it if not).
