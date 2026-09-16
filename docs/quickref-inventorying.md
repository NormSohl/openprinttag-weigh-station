# Quick Reference — Inventorying

*Print this page and keep it at the station. Full detail: [user-manual.md](user-manual.md).*

Two things live under this heading: checking what's on hand day-to-day, and
periodically running a physical shelf audit.

## Checking what's on hand (no action needed)

Browse to `http://weighstation.local/` — every weigh already updates this
automatically.

| Page | What it shows |
|---|---|
| **Inventory** | Remaining filament by material, grouped by vendor. Click a row to see individual spools; click a spool number for its weigh history. |
| **Stock List** (`/stock`) | The curated "what we keep buying" list, each item with a 90-day popularity score. Lowest-popularity and never-in-stock items sort to the top — those are the candidates to reconsider. It's normal for inventory to hold material *not* on this list (phased-out stock). |
| **Reorder** | Stock List items at or below their threshold — download a CSV to place the order. |
| **Usage** | The permanent, all-time consumption record by month/year — CSV download. |

## Running a physical audit (e.g. monthly shelf count)

An audit finds spools that have gone missing without anyone recording it, and
retires them properly instead of leaving the count wrong forever.

1. On **Inventory**, click **Start audit**.
2. **Weigh every spool as you normally would** while doing the count — a
   normal weigh during the audit window marks that spool "found"
   automatically, no extra step. **Leave the Inventory page open while you
   work** — the found count updates itself live as each spool is weighed, no
   manual refresh needed.
3. When every spool has been checked, click **Finish audit**. The page now
   lists every spool that was *not* seen, each with two buttons:
   - **Found** — it's physically there but you didn't weigh it (eyeballed it
     on the shelf). No weight change.
   - **Close** — it's genuinely gone. Its last known weight is recorded as
     consumed and it's marked **retired**.
4. Resolve **every** not-found spool one of those two ways — there's no bulk
   action and no way to leave some undecided. Once the last one is resolved,
   the audit closes itself.
5. **Abandon audit** at any point backs out of the audit itself, without
   undoing any Found/Close already recorded.

**Retired spools aren't deleted** — their history still counts toward Usage
and Stock List popularity, but they're hidden from Inventory by default (a
count + show/hide toggle reveals them). If a "retired" spool turns up and gets
weighed again, it automatically un-retires — no extra step.
