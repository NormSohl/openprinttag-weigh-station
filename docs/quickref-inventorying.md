# Quick Reference — Inventorying

*Print this page and keep it at the station. Full detail: [user-manual.md](user-manual.md).*

## Checking what's on hand

Open `http://weighstation.local/`.

| Page | What it shows |
|---|---|
| **Inventory** | Remaining filament by material. Click a row for its spools; click a spool for its history. |
| **Stock List** | What we keep in stock, with a popularity score. Items at the top are candidates to drop. |
| **Reorder** | Stock List items running low. Download, copy or e-mail the list to order. |
| **Usage** | Filament used by month. CSV download. |

## Running a shelf audit

1. On **Inventory**, press **Start audit**. Keep the page open.
2. **Weigh every spool.** Each weigh marks it found.
3. Press **Finish audit.** The page lists spools that weren't weighed.
4. For each one, press:
   - **Found** if it's on the shelf.
   - **Close** if it's gone. It's retired.
5. The audit ends when every spool is resolved.

**Abandon audit** stops the audit. Found and Close already pressed stay.

**Retired spools** are hidden from Inventory (use the show/hide toggle). A
retired spool that's weighed again comes back automatically.
