# tools/ — UI preview generators

Standalone Python scripts that render the device's UI **without flashing the
board**, for reviewing look/layout/wording on a PC. Pure `python3` stdlib, no
dependencies.

| Script | Renders | Source of truth |
|---|---|---|
| `preview_webapp.py` | The built-in web app (dashboard, spools, spool history + sparkline, onboard, reorder, config, calibrate, backup) | `src/web_app.cpp` |
| `preview_lcd.py` | The 3.5" ILI9488 TFT for every device state (480×320) | `src/display_task.cpp` |

```bash
python3 tools/preview_webapp.py   # -> webapp-preview.html
python3 tools/preview_lcd.py      # -> lcd-preview.html
```
Open the generated HTML in any browser.

> ⚠️ **These duplicate layout/markup from the firmware with sample data** — they
> are *not* built or tested by CI, and they will drift if the web/display code
> changes. Treat them as a convenience preview, and re-run (and tweak) them after
> editing `web_app.cpp` / `display_task.cpp`. The firmware is always authoritative.
