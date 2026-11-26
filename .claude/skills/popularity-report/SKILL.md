---
name: popularity-report
description: Make the lab's standard monthly filament popularity report from a weigh-station backup (event log + config), formatted as Discord messages: every item hot to not in g/month and $/month, Most popular 15 / Middle / Least popular 15 with subtotals, not-in-stock list, grand total. Use when asked for a popularity report, usage report, "what's selling", or the monthly filament numbers.
---

# Popularity report

The format was set on 2026-11-25 and is the standard: keep it unless the user
asks for a change. The tool is `tools/report/popularity.py`; its README has the
details and the meaning of every number.

## Steps

1. **Get the two backups from the user** (open reads on the station; a cloud
   session can't reach the lab network):
   - `http://weighstation.local/export` → `weighstation-log.ndjson`, ~225 KB+.
   - `http://weighstation.local/config/export` → `weighstation-config.json`.
   On an iPad, Safari's address suggestions often turn `/export` into
   `/config/export`: if two config files arrive, say so and ask them to type the
   address in full (or the numeric IP) and tap Go. If the upload refuses
   `.ndjson`, renaming it to `.txt` works.
2. **Copy them to the scratchpad**, never into the repo.
3. **Check prices.** List spools whose cost / nominal weight is over $30/kg
   (a mistyped price, e.g. $170 for $17: spools #50 and #53 in Nov 2026). Ask
   the user what the right price is and pass `--price SPOOL=DOLLARS`; remind
   them the station still needs the fix (Restock → Change this spool's
   details, with the spool on the scale).
4. **Run it** with `--now` set to the export's last event time, so the window
   matches what they downloaded:
   ```bash
   python3 tools/report/popularity.py LOG CONFIG --now <last ts> [--price N=$]
   ```
   If they also sent `/api/stock`, add `--check api-stock.json`; any mismatch
   means the station's rule changed and the script must be updated first.
5. **Give the user each message** in a four-backtick fenced block so the inner
   ``` lines survive copying, with its character count. Then, briefly:
   - items marked `*` (audit Close inflation) and what the rate would be
     without that spool;
   - popular items with little on hand, and the bottom six (drop candidates);
   - products not on the Stock List that rank high;
   - the change from last month's total if the Claude Hub row has it.
6. Offer to record the totals on the Claude Hub row (Next step / Last touched)
   so next month can compare.

## Don't

- Don't hand-edit numbers in the output. If something is wrong, fix the data
  (`--price`) or the script, then rerun.
- Don't commit exports or report output: they are lab data.
