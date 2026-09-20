# Quick Reference — Onboarding a New Spool

*Print this page and keep it at the station. Full detail: [user-manual.md](user-manual.md).*

Mainly for processing new stock as it arrives from a vendor — once per new,
untagged reel.

## Steps

1. **Apply an OpenPrintTag sticker** to the spool's flat hub face — centered
   so it lands over the reader. Press firmly.
2. **Place the spool on the scale.**
3. The display shows a 2-second countdown, then in the last second the digit
   turns **red** and the text switches to **"Writing tag now!"**:

   > New tag found
   > Remove to cancel
   > Registering in: 2…

   - **Remove the spool** within the countdown to cancel (e.g. it landed on
     the scale by accident).
   - **Leave it in place** to confirm — the station formats the tag and
     assigns a spool number.
4. Display shows the spool number and a QR code:

   > Registered!  Spool #58  212 g
   > NEEDS ONBOARDING

   The spool is now trackable but has no material data yet.
5. **Enter data for the new spool from any web browser.** Scan the QR code
   with your phone or tablet, or browse to `weighstation.local/onboard` on
   any device on the same network, while the spool is still on the scale.
6. On the **Onboard** page, the "This spool is…" dropdown defaults to
   **"a new product"**:
   - Already have this exact product on file? **Scroll down to select
     "another spool of X"** — it inherits vendor/material/colour/tare with
     nothing to retype.
   - New product (the default)? **Search the catalog first** — picking a
     real result fills in brand, material, colour, print temps, and vendor
     identifiers automatically.
   - Not in the catalog? Expand **Enter details manually**. Vendor, material,
     colour, and spool profile each default to whatever you picked last time,
     and each has a **+ Add new** option if it's not in the list yet.
7. **Cost for this spool** — optional, near the top. Enter what this reel
   cost if you know it; leave it blank if you don't. It feeds the dollar
   figures on the Usage page. The box starts empty every time, on purpose —
   leaving it blank on a later edit keeps whatever was already recorded,
   it does not erase it. (To clear a price, type `0`.)
8. Click **Save & write tag.** The full data is written to the tag.

Done — the tag now carries real identity, and the spool is in inventory.
**Place the next spool** and the page carries you straight into *its*
onboarding form — no need to click the Onboard nav link again. Handy for
working through a stack of new reels back to back.

## If it goes wrong

**Tag stuck on "Read Error"** after a registration that failed partway (tag
is half-written — no longer blank, but not valid either): open the
**Erase Tag** page, start erase mode, then place the stuck spool — it erases
immediately, same recovery as before, no serial cable needed.

**Stop erase mode** once it's done, then place the spool again — it
registers from scratch.
