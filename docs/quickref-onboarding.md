# Quick Reference — Onboarding a New Spool

*Print this page and keep it at the station. Full detail: [user-manual.md](user-manual.md).*

Do this once per **new, untagged** spool — a fresh reel that's never touched
the station before.

## Steps

1. **Apply an OpenPrintTag sticker** to the spool's flat hub face — away from
   the windings, centered so it lands over the reader. Press firmly for 5
   seconds.
2. **Place the spool on the scale.**
3. The display shows **"New tag found / Remove to cancel / Registering in:
   2…"** — a 2-second window. In the last second the digit turns **red** and
   the text switches to **"Writing tag now!"**.
   - **Remove the spool** within the window to cancel (e.g. it landed on the
     scale by accident).
   - **Leave it in place** to confirm — the station formats the tag and
     assigns a spool number.
4. Display shows **"Registered! / NEEDS ONBOARDING"** with the spool number
   and a QR code. The spool is now trackable but has no material data yet.
5. **Scan the QR code** (or browse to `weighstation.local/onboard` on any
   device on the network) while the spool is still on the scale.
6. On the **Onboard** page:
   - Already have this exact product on file? Pick **"another spool of X"** —
     it inherits vendor/material/colour/tare with nothing to retype.
   - New product? Choose **"a new product"**, then **search the catalog
     first** — picking a real result fills in brand, material, colour, print
     temps, and vendor identifiers automatically.
   - Not in the catalog? Expand **Enter details manually**. Vendor, material,
     colour, and spool profile each default to whatever you picked last time,
     and each has a **+ Add new** option if it's not in the list yet.
7. Click **Save & write tag.** The full data is written to the tag.

Done — the tag now carries real identity, and the spool is in inventory.

## If it goes wrong

**Tag stuck on "Read Error"** after a registration that failed partway (tag is
half-written — no longer blank, but not valid either): connect over USB
serial (115200 baud) and run:

```
TAGFORMAT
```

Then lift and re-place the spool — it registers from scratch.
