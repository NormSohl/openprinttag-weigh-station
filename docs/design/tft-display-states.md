# Weigh Station — TFT Display Content by State

The display is a 3.5" ILI9488 480×320 SPI TFT (landscape, `setRotation(1)`),
driven by TFT_eSPI in `display_task.cpp` — **the authoritative source for exact
per-state layout**. Body rows are size-2 text (~28 px pitch, a couple dozen
chars wide); titles are size-3. Colour cues status: green idle, amber warning,
red error, cyan/blue action. The onboard NeoPixel mirrors state as a colour, and
the passive buzzer plays short state tones (boot chime, weigh-done, error, …).

| State | Title | Body | Notes |
|---|---|---|---|
| Boot | `Weigh Station` | `Starting...` | brief splash |
| WiFiSetupMode | `WiFi Setup` | `Join network:` · `WeighStation-Setup` · `Then open browser to` · `192.168.4.1` | captive-portal provisioning |
| Idle | `Seattle Makers` | `Place spool to begin`; `Web app:` · `http://weighstation.local`; amber `Scale not calibrated` · `Calibrate in web app` until calibrated | station mode, ready |
| IdleNoWiFi | `Weigh Station` | `Place spool to weigh`; SoftAP fallback: `Join WiFi:` · `WeighStation` · `http://<ip>`; `Uncalibrated - web app` until calibrated | AP fallback, still functional |
| TagDetecting | *(no change)* | | sub-second debounce — don't render, avoid flicker on false triggers |
| TagReadError | `Read Error` | `Reposition spool` · `or remove tag` | actionable, not a code |
| BlankTagFound → AwaitingFormatConfirm | `New tag found` | `Remove to cancel` · `Registering in:` + big live `5 → … → 0` countdown | NeoPixel blink accelerates alongside |
| FormattingAndRegistering | `Registering...` | `Please wait` | tag format + write |
| ForeignTagFound / RegisteringForeignTag | `New spool found` | decoded brand · decoded material · `Registering spool...` | pre-tagged spool (Prusament, another maker's tooling) not yet known — decode its own data and create a local record |
| WeighingAndSync | `Weighing...` | `<grams> g` | single sample |
| Present (needs onboarding) | `Registered!` | `Spool #N` · `Add details in web app:` · `http://weighstation.local` · `<grams> g` | prompts filling in details via the web form; shows the URL |
| Present (normal) | `Spool #N` | material · `<grams> g remaining` · `Saved locally` | steady display while the spool sits on the scale |
| ReconcilingMainSection | `Updating tag...` | | brief overlay, then resumes Present |

Local storage replaced Spoolman, so there is no "offline/unreachable" state —
the device is self-contained. See `device-states.mermaid` for the full state
graph and `sd-local-ecosystem.md` for the storage design.
