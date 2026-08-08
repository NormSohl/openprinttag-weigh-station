# Weigh Station — TFT Display Content by State

The display is a 3.5" ILI9488 480×320 SPI TFT (landscape, `setRotation(1)`),
driven by TFT_eSPI in `display_task.cpp` — **the authoritative source for exact
per-state layout**. Body rows are size-2 text (28 px pitch); titles are size-3.
Colour cues status: green idle, amber warning, red error, cyan/blue action. The
onboard NeoPixel mirrors state as a colour, and the passive buzzer plays short
state tones (boot chime, weigh-done, error, …).

The screen is split into a text column and a QR panel on the right, and **the
text budget is 26 characters per row** — the GLCD font is 12 px per character
at size 2, starting at a 12 px margin, and `row()` only clears out to `TEXT_W`.
A longer string is both clipped by the panel and left behind on the next
redraw, so it fails in a way that looks like a display fault rather than a
typo. `weighstation.local/onboard` is exactly 26; so is the longest
`deviceStateName()`. Check new strings against that before adding them.

| State | Title | Body | Notes |
|---|---|---|---|
| Boot | `Weigh Station` | `Starting...` | brief splash |
| WiFiSetupMode | `WiFi Setup` | `Join network:` · `WeighStation-Setup` · `Then open browser to` · `192.168.4.1` | captive-portal provisioning |
| Idle | `Seattle Makers` | `Place spool to begin`; `Web app:` · `weighstation.local` · `or <ip>`; amber `Scale not calibrated` · `Calibrate in web app` until calibrated, or red `STORAGE FULL - not` · `recording weighs!`; QR panel to `http://<ip>/` | station mode, ready. Both address forms deliberately: the name is the one worth typing, the numeric one is the fallback that always resolves — which is also why the QR encodes the numeric address |
| IdleNoWiFi | `Weigh Station` | `Place spool to weigh`; SoftAP fallback: `Join WiFi:` · `WeighStation` · `http://<ip>`; `Uncalibrated - web app` until calibrated | AP fallback, still functional |
| TagDetecting | *(no change)* | | sub-second debounce — don't render, avoid flicker on false triggers |
| TagReadError | `Read Error` | `Reposition spool` · `or remove tag` | actionable, not a code |
| BlankTagFound → AwaitingFormatConfirm | `New tag found` | `Remove to cancel` · `Registering in:` + big live `5 → … → 0` countdown | NeoPixel blink accelerates alongside |
| FormattingAndRegistering | `Registering...` | `Please wait` | tag format + write |
| ForeignTagFound / RegisteringForeignTag | `New spool found` | decoded brand · decoded material · `Registering spool...` | pre-tagged spool (Prusament, another maker's tooling) not yet known — decode its own data and create a local record |
| WeighingAndSync | `Weighing...` | `<grams> g` | single sample |
| Present (needs onboarding) | `Registered!` | `Spool #N  <grams> g` · amber `NEEDS ONBOARDING` · `Scan QR to add details,` · `or visit the Onboard page:` · `weighstation.local/onboard` · `or <ip>/onboard`; QR panel deep-links to `/onboard` | names the missing step rather than just reporting success. Both addresses carry the `/onboard` path — the home page doesn't say which spool it would act on. The `.local` line is suppressed in SoftAP fallback, where mDNS won't resolve |
| ValidTagFound | `Tag read` | `<material_name>` (wrapped) · `<brand_name>` | brief screen between detection and weighing |
| Present (normal) | `Spool #N` | `<material_name>` (wrapped over up to 2 rows) · `<brand_name>` · `<grams> g remaining` · `Saved locally` | steady display while the spool sits on the scale. Shows the OPT **display string** — "PLA Summer Grass", not "PLA" — with the brand beneath it, as the spec asks (`brand_name` + `material_name` together). A 63-char name cannot fit a 26-char row, so `rowWrap()` breaks it at a space; a word longer than a row is cut hard |
| ReconcilingMainSection | `Updating tag...` | | brief overlay, then resumes Present |

Local storage replaced Spoolman, so there is no "offline/unreachable" state —
the device is self-contained. See `device-states.mermaid` for the full state
graph and `sd-local-ecosystem.md` for the storage design.
