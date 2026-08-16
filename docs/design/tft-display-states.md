# Weigh Station — TFT Display Content by State

The display is a 3.5" ILI9488 480×320 SPI TFT (landscape, `setRotation(1)`),
driven by TFT_eSPI in `display_task.cpp` — **the authoritative source for exact
per-state layout**. Body rows are size-2 text (28 px pitch); titles are size-3.
**Title colour is a fixed five-way vocabulary — pick from this list, don't invent a new shade:**

| Colour | Meaning | States |
|---|---|---|
| Green | Ready, nothing wrong | Idle |
| Amber `(220,140,0)` | Warning, or a choice pending — not an error | IdleNoWiFi, BlankTagFound, AwaitingFormatConfirm |
| Red | Error | TagReadError |
| Cyan | New information just arrived — a tag was read | ForeignTagFound, RegisteringForeignTag, ValidTagFound |
| Blue `(0,100,220)` | Working silently in the background — nothing needed from you | WiFiSetupMode, FormattingAndRegistering, WeighingAndSync, ReconcilingMainSection |
| Yellow | Succeeded, but incomplete — a next step still needs a human | Present (needs onboarding) |
| White | Neutral / default | Boot, Present (normal — no title; row 0 *is* the header) |

Cyan and blue are both "the station is doing something," but distinctly: cyan says *look, new data*, blue says *stand by*. Yellow is not a softer amber — amber is a warning about station/network state, yellow is "the spool is fine, a person needs to finish something." `ReconcilingMainSection` was yellow until 2026-08-16; it moved to blue because rewriting a tag while a spool sits on the scale is exactly the "working silently" case, not a spool waiting on a human.

The onboard NeoPixel mirrors the same five-way meaning as a colour (not always the identical RGB — see `display_task.cpp` per-state `pixelColor`), and the passive buzzer plays short state tones (boot chime, weigh-done, error, …).

**Every screen carries a corner clock**, bottom-right, silver, local time — `MM/DD/YYYY HH:MM` (24-hour) or `MM/DD/YYYY hh:mm AM/PM` (12-hour, the default), whichever is configured on the Settings page alongside the timezone (see *Display: timezone, clock format, station name* in `CLAUDE.md`). Blank until the clock has synced from NTP — never a fabricated date. Redrawn on every state change and on minute rollover, plus immediately on a live Settings-page save so it never sits stale while the device idles. Below and clear of everything else any screen draws.

The screen is split into a text column and a QR panel on the right. **Rows 0-1
are full width (38 characters); row 2 down is 26** — the GLCD font is 12 px per character
at size 2, starting at a 12 px margin, and `row()` only clears out to `TEXT_W`.
A longer string is both clipped by the panel and left behind on the next
redraw, so it fails in a way that looks like a display fault rather than a
typo. `weighstation.local/onboard` is exactly 26; so is the longest
`deviceStateName()`. Check new strings against that before adding them.

| State | Title | Body | Notes |
|---|---|---|---|
| Boot | `Weigh Station` | `Starting...` | brief splash |
| WiFiSetupMode | `WiFi Setup` | `Join network:` · `WeighStation-Setup` · `Then open browser to` · `192.168.4.1`; QR panel scans straight into the open setup AP (`WIFI:S:WeighStation-Setup;T:nopass;;`); live row 8 `Closes in M:SS` counts down the portal window | captive-portal provisioning. The QR exists because typing an SSID by hand is the friction here — a web-URL QR would be useless on this screen, since `192.168.4.1` is unreachable until you're already on the AP |
| Idle | `<station name>` (green; configurable on the Settings page, default `Seattle Makers` — the one screen that greets by the owning org's name) | `Place spool to begin`; `Web app:` · `weighstation.local` · `or <ip>`; amber `Scale not calibrated` · `Calibrate in web app` until calibrated, or red `STORAGE FULL - not` · `recording weighs!`; QR panel to `http://<ip>/` | station mode, ready. Both address forms deliberately: the name is the one worth typing, the numeric one is the fallback that always resolves — which is also why the QR encodes the numeric address |
| IdleNoWiFi | `Weigh Station` | `Setup timed out`; `Place spool to weigh`; `Weighs & saves locally` (row 3), or `Uncalibrated - web app`/`STORAGE FULL - not saving` in its place; `Join WiFi:` · `WeighStation` · `http://<ip>`; `Change network:` · `<ip>/reset`; QR panel joins the SoftAP | AP fallback, fully functional for weighing — only WiFi-dependent things (remote web access, NTP clock sync) don't work. Row 1 names the cause: the setup portal was offered and closed without new credentials, whether on first join or after a previously-saved network dropped. Row 3 is a three-way priority (storage failure beats miscalibration beats the reassurance line) — `storeAppendEvent()`/nfcTask have no WiFi awareness at all, so without this line the screen reads as more broken than it is |
| TagDetecting | *(no change)* | | sub-second debounce — don't render, avoid flicker on false triggers |
| TagReadError | `Read Error` | `Reposition spool` · `or remove tag` | actionable, not a code |
| BlankTagFound → AwaitingFormatConfirm | `New tag found` | BlankTagFound alone: title only, no body yet (brief flash on the very first redraw); AwaitingFormatConfirm: `Remove to cancel` · `Registering in:` + big live `5 → … → 0` countdown | NeoPixel blink accelerates alongside the countdown |
| FormattingAndRegistering | `Registering...` | `Please wait` | tag format + write |
| ForeignTagFound / RegisteringForeignTag | `New spool found` | decoded brand · decoded material · `Registering spool...` | pre-tagged spool (Prusament, another maker's tooling) not yet known — decode its own data and create a local record |
| WeighingAndSync | `Weighing...` | `<N> grams` | single sample |
| Present (needs onboarding) | `Registered!` | `Spool #N  <N> grams` · amber `NEEDS ONBOARDING` · `Scan QR to add details,` · `or visit the Onboard page:` · `weighstation.local/onboard` · `or <ip>/onboard`; QR panel deep-links to `/onboard` | names the missing step rather than just reporting success. Both addresses carry the `/onboard` path — the home page doesn't say which spool it would act on. The `.local` line is suppressed in SoftAP fallback, where mDNS won't resolve |
| ValidTagFound | `Tag read` | `<brand_name>` · `<material_name>` (wrapped), both white | brief screen between detection and weighing. Brand before material, matching ForeignTagFound and Present's header — was material-first with brand secondary-colored until 2026-08-16, made consistent |
| Present (normal) | *(none — the header line is row 0)* | full-width `Spool #N  <brand_name>  <material_name>` (wraps to row 1 if needed) · `<N> grams remaining` · `Weight recorded`, or red `NOT SAVED - storage full` when the log has stopped accepting writes | steady display while the spool sits on the scale. Rows 0-1 clear to y 68 and the QR panel starts at y 82, so the header band gets the **whole 480 px — 38 characters against 26 below**, which is what lets number, brand and product name share one line. Brand precedes the name, matching OPT's own "Prusament PLA Galaxy Black" ordering, and keeping brand on row 0 with the number when the line wraps. Shows the OPT display string ("PLA Summer Grass", not "PLA") with the brand, as the spec asks. The status line reports the actual write outcome; it used to read "Saved locally" unconditionally, a Spoolman-era phrase that distinguished nothing once there was no server, and that lied outright when a full log had caused the event to be dropped |
| ReconcilingMainSection | `Updating tag...` | | brief overlay, then resumes Present |

Local storage replaced Spoolman, so there is no "offline/unreachable" state —
the device is self-contained. See `device-states.mermaid` for the full state
graph and `sd-local-ecosystem.md` for the storage design.
