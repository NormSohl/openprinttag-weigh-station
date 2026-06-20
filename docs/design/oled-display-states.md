# Weigh Station — OLED Content by State

Assumes the existing 8x16 font (`qw_fnt_8x16`) on the 128x64 SSD1306 — roughly 16 characters × 4 lines. Lines kept tight to that budget below.

| State | Line 1 | Line 2 | Line 3 | Line 4 | Notes |
|---|---|---|---|---|---|
| Boot | `Weigh Station` | `Starting...` | | | Brief splash, ~1-2s |
| WiFiSetupMode | `WiFi Setup` | `Join AP:` | `WeighStation-Setup` | | Matches existing captive portal flow |
| Idle | `Seattle Makers` | `Weigh Station` | `Place spool` | `to begin` | Calm baseline, WiFi connected |
| IdleNoWiFi | `No WiFi` | `Working offline` | `Place spool` | `to weigh` | Degraded but still functional |
| TagDetecting | *(no change)* | | | | Sub-second debounce — don't render anything; avoid flicker on false triggers |
| TagReadError | `Read error` | `Reposition spool` | `or remove tag` | | Actionable, not just an error code |
| AwaitingFormatConfirm | `New tag found` | `Remove to cancel` | `Registering in:` | **`5`→`4`→`3`→`2`→`1`→`0`** | Big live countdown digit on line 4; LED blink accelerates alongside |
| FormattingAndRegistering | `Registering...` | `Please wait` | | | ~1-2s: tag write + Spoolman create call |
| ForeignTagFound / RegisteringForeignTag | `New spool found` | `Prusament` *(decoded vendor)* | `PETG Black` *(decoded material)* | `Adding to database` | Legitimate pre-tagged spool (Prusament, or another maker's tooling) not yet in Spoolman — decode its own real data and find-or-create, same as the original onboarding logic |
| WeighingAndSync | `Weighing...` | | | | Very brief — single sample, no need for more |
| Present (synced spool) | `Spool #142` | `PETG Black` | `743g remaining` | `✓ synced` | Steady display while spool sits on scale |
| Present (freshly registered) | `Registered!` | `Spool #142` | `Edit in Spoolman` | `743g` | Distinct variant — prompts the team member to go fill in real data |
| ReconcilingMainSection | `Updating tag...` | | | | Brief overlay on top of Present, then resumes normal Present content |
| SpoolmanUnreachable | `Spoolman offline` | `743g (local)` | `Will sync later` | | Same degraded-but-functional tone as IdleNoWiFi |
