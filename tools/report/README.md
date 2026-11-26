# tools/report/ — the monthly popularity report

`popularity.py` turns a station backup into the lab's standard filament
popularity report, ready to paste into Discord: every item from most to least
used, in grams and dollars per month, with *Most popular 15* / *Middle* /
*Least popular 15* sections, subtotals, the items not in stock during the
window, and a grand total. First made 2026-11-25; that report is the format.

## Run it

Download the two backups (open reads, they don't interrupt weighing):

- `http://weighstation.local/export` → `weighstation-log.ndjson`
- `http://weighstation.local/config/export` → `weighstation-config.json`

(Administration → Backup has both. On an iPad, type the address in full and
tap Go: Safari's suggestions tend to swap `/export` for `/config/export`.)

```bash
python3 tools/report/popularity.py weighstation-log.ndjson weighstation-config.json
# or, from a computer on the lab network:
python3 tools/report/popularity.py --station http://weighstation.local
```

Output is one or more Discord messages separated by `=====NEXT MESSAGE=====`.
Each is under Discord's 2000-character limit; paste each as its own message,
including the ``` lines (Discord doesn't render Markdown tables, but a code
block keeps the columns aligned on desktop).

Options:

| | |
|---|---|
| `--price 50=17` | Use $17 for spool #50 (repeatable). For a mistyped price not yet fixed on the station. |
| `--now 2026-11-25T23:50:00Z` | End of the 90-day window (default: now). Use the export's time to reproduce a report. |
| `--check api-stock.json` | Compare every Stock List row with the station's `/api/stock` and fail on a difference. |
| `--json rows.json` | Also write the rows as JSON. |

## What the numbers mean

The station's own popularity rule (`storeMaterialPopularity()` in
`src/store.cpp`), extended to every product:

- **Used** = the drop in a spool's remaining weight between weighs, plus what
  a Retire books (an audit Close or a tag erase counts the spool's remaining
  filament as used). A weigh under 50 g is a tag alone on the scale and is
  skipped.
- **g/month** = grams used in the last 90 days ÷ days the item had any filament
  on hand × 30.44. A stock-out doesn't drag an item down.
- **$/month** = g/month × the item's average price per kg: each spool's recorded
  cost ÷ its nominal weight, **$15/kg** for a spool with no price.
- A Stock List item counts its own product and its substitutes. Every other
  product is its own row.
- `*` marks an item where an audit Close booked more than 500 g of one spool as
  used. If that spool went missing rather than being used, the real rate is
  lower.
- The total adds rates that each run over their own in-stock days, so it reads
  a little above a calendar month's actual use.

For a Stock List item, g/month equals `/api/stock`'s `grams_per_week × 30.44 / 7`,
and `--check` verifies exactly that, so if the station's rule ever changes the
report fails loudly instead of quietly disagreeing with Reorder.

Keep real exports out of the repo.
