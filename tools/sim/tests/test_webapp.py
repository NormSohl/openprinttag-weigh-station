# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""End-to-end tests of the real web app (src/web_app.cpp) in the simulator."""
import collections
import json
import time

import pytest

PAGES = ["/", "/onboard", "/reorder", "/stock", "/usage", "/erase", "/config",
         "/backup", "/products", "/calibrate"]


@pytest.mark.parametrize("path", PAGES)
def test_page_loads_without_script_errors(sim, page, path):
    resp = page.goto(sim.url(path))
    assert resp.status == 200
    # Not "networkidle": pages with live updates hold an SSE stream open
    # for as long as they are shown, so the network never goes idle.
    page.wait_for_load_state("load")
    page.wait_for_timeout(300)   # let page scripts run
    assert page.js_errors == []


def test_config_export_reaches_its_own_handler(sim):
    # Route ORDER matters on the device (CLAUDE.md, "ESPAsyncWebServer's
    # plain-string route matching is prefix-based"): registered after
    # /config, this would return the Config page's HTML.
    body = sim.get("/config/export")
    assert body.lstrip().startswith("{")


def test_blank_tag_appears_on_open_onboard_page(sim, page):
    """The Onboard page reloads itself over SSE when a stub is registered."""
    page.goto(sim.url("/onboard"))
    assert "No spool is on the scale" in page.content()
    sim.blank(gross=1220)
    page.wait_for_selector("select#profile", timeout=5000)
    assert page.js_errors == []


def test_manual_onboarding_writes_the_spool(sim, page):
    sim.blank(gross=1220)
    page.goto(sim.url("/onboard"))
    # Seeded catalog defaults fill vendor/material/colour; a profile sets the
    # nominal weight and tare, and is required on the manual path.
    page.select_option("select#profile", index=1)
    page.check("#confirm-new")          # a typed entry is a new product
    page.click("button[type=submit]")
    page.wait_for_url("**/spool?id=*")
    spools = sim.get_json("/api/spools")
    assert len(spools) == 1
    assert spools[0]["needs_onboarding"] is False
    assert spools[0]["product"] is not None
    # The simulated NFC task picks the write up on its next tick.
    deadline = time.time() + 3
    while "tag Main written" not in sim.log() and time.time() < deadline:
        time.sleep(0.1)
    assert "tag Main written" in sim.log()


def test_another_spool_of_x_converges_on_one_product(sim, page):
    sim.blank(gross=1220)
    page.goto(sim.url("/onboard"))
    page.select_option("select#profile", index=1)
    page.check("#confirm-new")          # a typed entry is a new product
    page.click("button[type=submit]")
    page.wait_for_url("**/spool?id=*")
    first = sim.get_json("/api/spools")[0]

    sim.remove()
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    page.click(f"#stk-list .stk[data-prod='{first['product']}']")
    page.click("button[type=submit]")
    page.wait_for_url("**/spool?id=*")

    spools = sim.get_json("/api/spools")
    assert len(spools) == 2
    assert {s["product"] for s in spools} == {first["product"]}
    assert len(sim.get_json("/api/products")) == 1


def test_seeded_store_rolls_up_on_reorder(sim, page):
    assert sim.cmd("SEED 6 20").startswith("ok")
    page.goto(sim.url("/reorder"))
    assert page.locator("table").count() >= 1
    assert page.js_errors == []


# ── Stock List picker (docs/design/barcode-onboarding.md, phase 1) ────────────

def _onboard_manually(sim, page, gross=1220):
    sim.blank(gross=gross)
    page.goto(sim.url("/onboard"))
    page.select_option("select#profile", index=1)
    page.check("#confirm-new")          # a typed entry is a new product
    page.click("button[type=submit]")
    page.wait_for_url("**/spool?id=*")
    return sim.get_json("/api/spools")[-1]


def _linked_stock_row(sim, page):
    """A product (from one onboarded spool) and a Stock List row linked to it."""
    first = _onboard_manually(sim, page)
    sim._post("/api/stock/add", product=first["product"], min_spools=2, pack_qty=1)
    sim.remove()
    return first["product"]


def test_stock_pick_onboards_as_that_rows_product(sim, page):
    pid = _linked_stock_row(sim, page)
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    row = page.locator(f"#stk-list .stk[data-prod='{pid}']")
    assert row.count() == 1
    row.click()
    assert page.input_value("select#prod") == str(pid)
    assert page.locator("#newprod").is_hidden()
    assert page.locator("#stk-ok").is_visible()
    page.click("button[type=submit]")
    page.wait_for_url("**/spool?id=*")
    spools = sim.get_json("/api/spools")
    assert [s["product"] for s in spools] == [pid, pid]
    assert page.js_errors == []


def test_stock_search_enter_picks_the_only_match_without_submitting(sim, page):
    pid = _linked_stock_row(sim, page)
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    name = page.locator(f"#stk-list .stk[data-prod='{pid}']").get_attribute("data-name")
    page.fill("#stk-q", name)
    page.press("#stk-q", "Enter")
    page.wait_for_timeout(200)
    assert "/onboard" in page.url          # the form did not submit
    assert page.input_value("select#prod") == str(pid)


def test_unlinked_stock_rows_cannot_be_picked(sim, page):
    # The seeded Stock List rows were never linked to a product; a spool
    # existing gives the page a #prod to (not) set.
    _onboard_manually(sim, page)
    sim.remove()
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    off = page.locator("#stk-list .stk.off")
    assert off.count() >= 1
    assert "not linked" in off.first.inner_text()
    off.first.click(position={"x": 5, "y": 5})
    assert page.input_value("select#prod") == "0"
    assert page.locator("#stk-ok").is_hidden()


def test_clear_and_manual_change_drop_the_stock_pick(sim, page):
    pid = _linked_stock_row(sim, page)
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    page.click(f"#stk-list .stk[data-prod='{pid}']")
    page.click("#stk-clear")                        # "change": back to choosing
    assert page.input_value("select#prod") == "0"
    assert page.locator("#stk-ok").is_hidden()
    assert page.locator("#stk-list").is_visible()
    assert page.locator("#newprod").is_hidden()     # nothing chosen yet
    page.click("#stk-list .stk.new")                # New product...
    assert page.input_value("select#prod") == "0"
    assert page.locator("#newprod").is_visible()
    assert page.inner_text("#stk-ok b") == "New product"


def test_one_list_holds_stock_items_other_products_and_new(sim, page):
    pid = _linked_stock_row(sim, page)               # product 1, on the Stock List
    _catalog_onboard(sim, EAN, material="PETG Blue") # product 2, not on it
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    heads = page.locator("#stk-list .stk-h").all_inner_texts()
    assert heads[:2] == ["Stock List", "Other products"]
    other = page.locator("#stk-list .stk[data-prod]:not([data-sid]):not(.new)")
    assert other.count() == 1 and "PETG Blue" in other.inner_text()
    assert page.locator("#newprod").is_hidden()      # nothing chosen yet
    page.fill("#stk-q", "zzz-no-such-product")
    assert page.locator("#stk-list .stk-h:visible").count() == 0
    page.press("#stk-q", "Enter")
    assert "/onboard" in page.url                    # Enter never submits
    page.fill("#stk-q", "petg")
    page.press("#stk-q", "Enter")                    # the only match: picked
    assert page.input_value("select#prod") == other.get_attribute("data-prod")
    assert page.locator("#newprod").is_hidden()
    assert page.js_errors == []


# ── Barcode lookup and scan capture (barcode-onboarding.md, phase 2) ──────────

from conftest import scan


def gs1(body):
    """`body` plus its GS1 check digit."""
    s = sum(int(d) * (3 if i % 2 == 0 else 1) for i, d in enumerate(reversed(body)))
    return body + str((10 - s % 10) % 10)


UPC = gs1("01234567890")     # 12-digit UPC-A
EAN = "0" + UPC              # the same code as EAN-13


def _catalog_onboard(sim, gtin, gross=1220, vendor="eSUN", material="PLA+ Black"):
    """Onboard the spool on the scale as a catalog pick carrying `gtin`;
    returns the product id."""
    sim.blank(gross=gross)
    spool = sim.get_json("/_sim/state")["spool"]
    sim._post("/api/onboard", id=spool, source="catalog",
              cat_vendor=vendor, cat_material=material, cat_abbr="PLA",
              cat_nom_g=1000, cat_empty_g=220, cat_dia=1.75, cat_gtin=gtin)
    sim.remove()
    return sim.get_json("/api/spools")[-1]["product"]


def test_lookup_matches_upc_and_ean_forms(sim):
    pid = _catalog_onboard(sim, EAN)
    for code in (UPC, EAN):
        j = sim.get_json("/api/lookup?gtin=" + code)
        assert [p["id"] for p in j["products"]] == [pid]
    assert sim.get_json("/api/lookup?gtin=" + gs1("400000000000"))["products"] == []


def test_lookup_reports_stock_rows_linked_or_not(sim):
    pid = _catalog_onboard(sim, EAN)
    sim._post("/api/stock/add", product=pid, min_spools=1, pack_qty=1)
    # Adding a row on the Stock List always links it now, so an unlinked row
    # only comes from older config: give a seeded (unlinked) row a barcode
    # through a config backup round trip.
    other = gs1("400638133393")
    cfg = sim.get_json("/config/export")
    cfg["stock-items"][0]["gtin"] = other
    sim.upload("/config/import", json.dumps(cfg).encode())
    linked = sim.get_json("/api/lookup?gtin=" + UPC)["stock"]
    assert [s["product"] for s in linked] == [pid]
    unlinked = sim.get_json("/api/lookup?gtin=" + other)["stock"]
    assert len(unlinked) == 1 and unlinked[0]["product"] == 0


def test_scan_picks_the_stock_row_and_restores_the_focused_box(sim, page):
    pid = _catalog_onboard(sim, EAN)
    sim._post("/api/stock/add", product=pid, min_spools=1, pack_qty=1)
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    page.fill("#tare", "12")
    page.focus("#tare")
    scan(page, EAN)
    page.wait_for_selector("#scan-msg:not([hidden])")
    page.wait_for_function("document.getElementById('prod').value==%r" % str(pid))
    assert "/onboard" in page.url                   # Enter did not submit
    assert page.input_value("#tare") == "12"        # the digits were taken back
    assert page.locator("#stk-ok").is_visible()
    assert EAN in page.inner_text("#scan-msg")
    assert page.js_errors == []


def test_scan_of_a_product_not_on_the_stock_list(sim, page):
    pid = _catalog_onboard(sim, EAN)
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    scan(page, UPC)
    page.wait_for_function("document.getElementById('prod').value==%r" % str(pid))
    page.wait_for_selector("#scan-msg:has-text('not on the Stock List')")


def test_unknown_barcode_says_so(sim, page):
    _catalog_onboard(sim, EAN)
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    scan(page, gs1("400000000000"))
    page.wait_for_selector("#scan-msg:has-text('Unknown barcode')")
    assert page.input_value("select#prod") == "0"


def test_slow_typing_is_typing_and_a_bad_check_digit_is_ignored(sim, page):
    _catalog_onboard(sim, EAN)
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    page.focus("#stk-q")
    scan(page, EAN, gap_ms=120)                     # a person typing
    page.wait_for_timeout(200)
    assert page.input_value("#stk-q") == EAN        # left alone
    page.fill("#stk-q", "")
    bad = EAN[:-1] + str((int(EAN[-1]) + 1) % 10)
    scan(page, bad)                                 # a scanner, but no product barcode
    page.wait_for_selector("#scan-note:has-text('not a product barcode')")
    assert page.input_value("#stk-q") == ""         # taken back out
    assert page.locator("#scan-msg").is_hidden()
    assert "/onboard" in page.url


def test_scan_before_the_spool_is_placed_is_applied_after(sim, page):
    pid = _catalog_onboard(sim, EAN)
    sim._post("/api/stock/add", product=pid, min_spools=1, pack_qty=1)
    page.goto(sim.url("/onboard"))
    assert "No spool is on the scale" in page.content()
    scan(page, EAN)
    page.wait_for_selector("#scan-msg:has-text('place the spool')")
    sim.blank(gross=1100)                           # the page reloads over SSE
    page.wait_for_selector("select#prod", state="attached")
    page.wait_for_function("document.getElementById('prod').value==%r" % str(pid))
    assert page.locator("#stk-ok").is_visible()


def test_scan_of_an_unlinked_stock_row_says_to_fix_it(sim, page):
    _catalog_onboard(sim, EAN)
    other = gs1("400638133393")
    cfg = sim.get_json("/config/export")
    cfg["stock-items"][0]["gtin"] = other
    sim.upload("/config/import", json.dumps(cfg).encode())
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    scan(page, other)
    page.wait_for_selector("#scan-msg:has-text('not linked')")
    assert page.input_value("select#prod") == "0"
    assert page.locator("#stk-list .stk:visible").count() == 1


# ── Learn barcodes; catch wrong picks (barcode-onboarding.md, phase 3) ────────

def _product(sim, pid):
    return next(p for p in sim.get_json("/api/products") if p["id"] == pid)


def test_scan_then_pick_teaches_the_product_its_barcode(sim, page):
    pid = _linked_stock_row(sim, page)               # a product with no barcode
    code = gs1("400000000000")
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    scan(page, code)
    page.wait_for_selector("#scan-msg:has-text('Unknown barcode')")
    page.click(f"#stk-list .stk[data-prod='{pid}']")
    assert "will be recorded for" in page.inner_text("#scan-learn")
    # Picked by hand after the scan, so it asks first.
    msgs = []
    page.once("dialog", lambda d: (msgs.append(d.message), d.accept()))
    page.click("button[type=submit]")
    page.wait_for_url("**/spool?id=*learned=1*")
    assert msgs and "scanned before the product was chosen" in msgs[0]
    assert "now recorded for this product" in page.content()
    assert [p["id"] for p in sim.get_json("/api/lookup?gtin=" + code)["products"]] == [pid]

    # The next box of it is recognised straight away.
    sim.remove()
    sim.blank(gross=1050)
    page.goto(sim.url("/onboard"))
    scan(page, code)
    page.wait_for_function("document.getElementById('prod').value==%r" % str(pid))
    assert page.locator("#stk-ok").is_visible()


def test_a_scan_never_overwrites_a_different_barcode(sim, page):
    pid = _catalog_onboard(sim, EAN)
    other = gs1("400000000000")
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    scan(page, other)
    page.wait_for_selector("#scan-msg:has-text('Unknown barcode')")
    page.click(f"#stk-list .stk[data-prod='{pid}']")

    # Cancel keeps the page; OK saves, but the product keeps its barcode.
    page.once("dialog", lambda d: d.dismiss())
    page.click("button[type=submit]")
    page.wait_for_timeout(300)
    assert "/onboard" in page.url
    msgs = []
    page.once("dialog", lambda d: (msgs.append(d.message), d.accept()))
    page.click("button[type=submit]")
    page.wait_for_url("**/spool?id=*")
    assert "barcode is " + EAN.lstrip("0") in msgs[0] or "barcode is " + EAN in msgs[0]
    assert page.locator("#scan-conflict").is_visible()
    assert str(_product(sim, pid)["gtin"]).lstrip("0") == EAN.lstrip("0")


def test_catalog_pick_with_a_different_barcode_asks(sim, page):
    _onboard_manually(sim, page)                     # so the page has #prod
    sim.remove()
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    scan(page, UPC)
    page.wait_for_selector("#scan-msg:not([hidden])")
    # Stand in for a catalog pick (the catalog itself is fetched from GitHub,
    # which the tests block): CATALOG_SCRIPT fills these same fields.
    page.evaluate("""() => {
        const f = (k, v) => document.getElementById('cat-f-' + k).value = v;
        document.getElementById('cat-source').value = 'catalog';
        f('cat_vendor', 'eSUN'); f('cat_material', 'PLA+ Red'); f('cat_abbr', 'PLA');
        f('cat_nom_g', '1000'); f('cat_empty_g', '220'); f('cat_dia', '1.75');
        f('cat_gtin', '%s');
    }""" % gs1("690000000000"))
    msgs = []
    page.once("dialog", lambda d: (msgs.append(d.message), d.dismiss()))
    page.click("button[type=submit]")
    page.wait_for_timeout(300)
    assert msgs and "database says" in msgs[0]
    assert "/onboard" in page.url


def test_server_ignores_a_scan_with_a_bad_check_digit(sim, page):
    pid = _linked_stock_row(sim, page)
    sim.blank(gross=1100)
    spool = sim.get_json("/_sim/state")["spool"]
    bad = UPC[:-1] + str((int(UPC[-1]) + 1) % 10)
    sim._post("/api/onboard", id=spool, product=pid, scan_gtin=bad)
    assert "gtin" not in _product(sim, pid)


# ── Stock List and Products pages (barcode-onboarding.md, phase 4) ───────────

def test_product_page_edits_the_barcode(sim, page):
    pid = _linked_stock_row(sim, page)
    page.goto(sim.url(f"/product?id={pid}"))
    scan(page, UPC)
    assert page.input_value("#gtin") == UPC
    page.click("button[type=submit]")
    page.wait_for_url(f"**/product?id={pid}")
    assert _product(sim, pid)["gtin"] == str(int(UPC))

    bad = UPC[:-1] + str((int(UPC[-1]) + 1) % 10)
    page.fill("#gtin", bad)
    page.click("button[type=submit]")
    assert "not a valid barcode" in page.content()
    assert _product(sim, pid)["gtin"] == str(int(UPC))   # unchanged

    page.goto(sim.url(f"/product?id={pid}"))
    page.fill("#gtin", "")
    page.click("button[type=submit]")
    page.wait_for_url(f"**/product?id={pid}")
    assert "gtin" not in _product(sim, pid)


def test_stock_list_scan_picks_the_product_and_fills_the_barcode(sim, page):
    pid = _catalog_onboard(sim, EAN)
    page.goto(sim.url("/stock"))
    scan(page, UPC)
    page.wait_for_function("document.getElementById('sprod').value==%r" % str(pid))
    assert page.input_value("#sgtin") == UPC
    assert page.locator("#stocknew").is_hidden()
    page.click("#stockform ~ .card button[type=submit]")
    page.wait_for_url("**/stock")
    rows = [r for r in sim.get_json("/api/stock")["items"] if r["gtin"] == UPC]
    assert len(rows) == 1
    found = sim.get_json("/api/lookup?gtin=" + UPC)["stock"]
    assert [(r["id"], r["product"]) for r in found] == [(rows[0]["id"], pid)]

    # Scanning the same box again says it is already there.
    page.goto(sim.url("/stock"))
    scan(page, EAN)
    page.wait_for_selector("#scan-msg:has-text('Already on the Stock List')")
    assert page.js_errors == []


def test_stock_list_marks_unlinked_rows(sim, page):
    pid = _linked_stock_row(sim, page)
    page.goto(sim.url("/stock"))
    seeded = len(sim.get_json("/api/stock")["items"]) - 1
    assert page.locator(".unlinked").count() == seeded
    assert page.locator("#unlinked-note").is_visible()


# ── Repair page (barcode-onboarding.md step 5; product merge) ─────────────────

def test_repair_is_under_settings_not_a_tab(sim, page):
    page.goto(sim.url("/config"))
    page.click("#repair-link")
    page.wait_for_url("**/repair")
    assert page.locator("nav a[href='/repair']").count() == 0
    assert page.js_errors == []


def test_repair_links_an_unlinked_row_to_its_likely_product(sim, page):
    # The default Stock List has an unlinked "eSun" "PLA+" "Black" row; the
    # catalog's name for it carries an extra word. ("eSun PLA Matte Black"
    # must not be offered: PLA is not PLA+.)
    pid = _catalog_onboard(sim, EAN, vendor="eSUN", material="PLA+ Basic Black")
    page.goto(sim.url("/repair"))
    sels = page.locator("select.link-sel")
    before = sels.count()
    chosen = [s for s in sels.evaluate_all("ss => ss.map(s => s.value)") if s != "0"]
    assert chosen == [str(pid)]          # the one likely match, chosen for us
    page.click("#unlinked ~ form button[type=submit]")
    page.wait_for_url("**/repair?linked=1")
    assert page.locator("select.link-sel").count() == before - 1
    # Onboard can offer it now.
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    assert page.locator(f"#stk-list .stk[data-prod='{pid}']").count() == 1


def test_duplicates_are_suggested_merged_and_undone(sim, page):
    a = _catalog_onboard(sim, EAN, vendor="eSUN", material="PLA+ Black")
    other = gs1("400000000000")
    b = _catalog_onboard(sim, other, gross=1100, vendor="eSun", material="PLA Plus Black")
    assert a != b
    sim._post("/api/stock/add", product=b, min_spools=1, pack_qty=1)

    page.goto(sim.url("/repair"))
    dup = page.locator("tr.dup")
    assert dup.count() == 1 and "written differently" in dup.inner_text()
    dup.locator("a").click()
    page.wait_for_url("**/repair/merge?*")          # not swallowed by /repair
    assert page.locator("tr.diff").count() >= 1
    page.check(f"input[name=into][value='{a}']")
    page.once("dialog", lambda d: d.accept())
    page.click("form[action='/api/repair/merge'] button[type=submit]")
    page.wait_for_url("**/repair?merged=*")
    assert "moved" in page.inner_text("#repair-done")

    assert [p["id"] for p in sim.get_json("/api/products")] == [a]
    assert {s["product"] for s in sim.get_json("/api/spools")} == {a}
    # B's barcode now leads to A, and the Stock List row followed the merge.
    assert [p["id"] for p in sim.get_json("/api/lookup?gtin=" + other)["products"]] == [a]
    assert [s["product"] for s in sim.get_json("/api/lookup?gtin=" + EAN)["stock"]] == [a]
    # B's old page shows A.
    page.goto(sim.url(f"/product?id={b}"))
    page.wait_for_url(f"**/product?id={a}&merged={b}")

    page.goto(sim.url("/repair#merged"))
    page.once("dialog", lambda d: d.accept())
    page.click("form[action='/api/repair/unmerge'] button")
    page.wait_for_url("**/repair?unmerged=*")
    assert sorted(p["id"] for p in sim.get_json("/api/products")) == sorted([a, b])
    assert {s["product"] for s in sim.get_json("/api/spools")} == {a}   # spools stay
    assert page.js_errors == []


def test_merge_refuses_bad_requests_without_changing_anything(sim):
    a = _catalog_onboard(sim, EAN)
    import urllib.error
    for params in ({"a": a, "b": a, "into": a}, {"a": a, "b": 999, "into": a}):
        try:
            sim._post("/api/repair/merge", **params)
            assert False, "accepted"
        except urllib.error.HTTPError as e:
            assert e.code == 400
    assert len(sim.get_json("/api/products")) == 1


# ── Stray scans (any page) ─────────────────────────────────────────────────────

def test_a_stray_qr_scan_never_submits_the_onboard_form(sim, page):
    _onboard_manually(sim, page)
    sim.remove()
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    page.focus("input[name=cost]")
    # The QR code on an eSUN spool: a URL, typed at scanner speed, then Enter.
    page.keyboard.type("https://www.esun3d.com/pla-pro", delay=5)
    page.keyboard.press("Enter")
    page.wait_for_selector("#scan-note:has-text('not a product barcode')")
    assert "/onboard" in page.url
    assert page.input_value("input[name=cost]") == ""
    assert len(sim.get_json("/api/spools")) == 2
    assert sim.get_json("/api/spools")[-1]["needs_onboarding"] is True


def test_a_barcode_on_a_page_with_no_use_for_it_changes_nothing(sim, page):
    page.goto(sim.url("/config"))
    box = page.locator("main input[type=text], main input:not([type])").first
    before = box.input_value()
    box.focus()
    scan(page, EAN)
    page.wait_for_selector("#scan-note:has-text('nothing changed here')")
    assert box.input_value() == before
    assert "/config" in page.url
    assert page.js_errors == []


def test_clearing_a_scan_records_nothing_and_asks_nothing(sim, page):
    pid = _linked_stock_row(sim, page)
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    scan(page, gs1("400000000000"))
    page.wait_for_selector("#scan-msg:has-text('Unknown barcode')")
    page.click(f"#stk-list .stk[data-prod='{pid}']")
    page.click("#scan-clear")
    assert page.locator("#scan-learn").is_hidden()
    dialogs = []
    page.on("dialog", lambda d: (dialogs.append(d.message), d.accept()))
    page.click("button[type=submit]")
    page.wait_for_url("**/spool?id=*")
    assert dialogs == []
    assert "learned" not in page.url
    assert "gtin" not in _product(sim, pid)


def test_a_held_scan_can_be_forgotten(sim, page):
    _linked_stock_row(sim, page)                    # so the form has a product list
    page.goto(sim.url("/onboard"))
    scan(page, gs1("400000000000"))
    page.wait_for_selector("#scan-msg:has-text('within a minute')")
    page.click("#scan-forget")
    assert page.locator("#scan-msg").is_hidden()
    sim.blank(gross=1100)
    page.wait_for_selector("select#prod", state="attached")
    page.wait_for_timeout(300)
    assert page.locator("#scan-msg").is_hidden()
    assert page.input_value("#scan-gtin") == ""


def test_menu_has_administration_and_erase_lives_under_it(sim, page):
    page.goto(sim.url("/"))
    nav = page.locator("nav a")
    assert nav.all_inner_texts() == ["Inventory", "Reorder", "Restock", "Usage",
                                     "Administration"]
    page.goto(sim.url("/config"))
    warn = page.locator("#admin-warning")
    assert warn.inner_text() == ("These settings and tools are for use by trained staff only. "
                                 "CHANGES MADE HERE CAN BREAK THINGS!")
    assert warn.evaluate("e => getComputedStyle(e).fontWeight") in ("700", "bold")
    assert warn.evaluate("e => getComputedStyle(e).color") == "rgb(255, 68, 68)"
    # Reorganized: tools are one list, setup and advanced follow, and the
    # raw-JSON editors are off this page.
    assert page.locator("main h3").all_inner_texts() == ["Staff tools", "Station setup", "Advanced"]
    assert page.locator(".tools a").count() == 7
    assert page.locator("textarea").count() == 0
    assert page.locator(".admin-alert").count() == 0      # calibrated, on a network
    page.click("#erase-link")
    page.wait_for_url("**/erase")
    # Erase Tag keeps Administration highlighted in the menu.
    assert page.locator("nav a.active").inner_text() == "Administration"
    assert page.js_errors == []


@pytest.mark.parametrize("link,path", [("#stock-link", "/stock"), ("#backup-link", "/backup"),
                                       ("#erase-link", "/erase"), ("#repair-link", "/repair"),
                                       ("#products-link", "/products"),
                                       ("#calibrate-link", "/calibrate"),
                                       ("#tables-link", "/config/tables")])
def test_staff_pages_open_from_administration_and_keep_it_highlighted(sim, page, link, path):
    page.goto(sim.url("/config"))
    page.click(link)
    page.wait_for_url("**" + path)
    assert page.locator("nav a.active").inner_text() == "Administration"
    assert page.js_errors == []


def test_raw_tables_have_their_own_page_and_saving_returns_there(sim, page):
    # /config/tables must reach its own handler, not /config's (prefix routes).
    page.goto(sim.url("/config/tables"))
    assert page.locator("textarea").count() == 5
    page.locator("form[action='/api/config'] button[type=submit]").first.click()
    page.wait_for_url("**/config/tables")
    assert page.locator("textarea").count() == 5
    assert page.js_errors == []


# ── Page size budget ──────────────────────────────────────────────────────────
# Each page is built as one String in the station's RAM (~200 KB free). The
# simulator has plenty, so size is the only signal it can give: /repair was
# ~200 KB with the default Stock List before its product list was sent once.

def _many_products(sim, n):
    cols = ["Black", "White", "Red", "Blue", "Green", "Grey", "Orange", "Yellow", "Purple", "Pink"]
    for i in range(n):
        _catalog_onboard(sim, "", gross=1200, material=f"PLA Variant{i // 10} {cols[i % 10]}")


def test_admin_pages_stay_small_with_a_realistic_store(sim, page):
    _many_products(sim, 40)
    assert len(sim.get_json("/api/products")) == 40
    budget = {"/repair": 40_000, "/stock": 90_000, "/config": 20_000, "/products": 40_000}
    sim.blank(gross=1100)
    budget["/onboard"] = 90_000
    for path, limit in budget.items():
        size = len(sim.get(path).encode())
        assert size < limit, f"{path} is {size:,} bytes (budget {limit:,})"
    # The Repair page's pickers still offer every product, filled in the browser.
    page.goto(sim.url("/repair"))
    n = page.locator("select[name=a] option").count()
    assert n == 40
    first_row = page.locator("select.link-sel").first
    assert first_row.locator("option").count() >= 41      # "leave" + every product
    assert page.js_errors == []


# ── Restock page review fixes ─────────────────────────────────────────────────
# (The tests block the online catalog, and a catalog that fails to load opens
# "Or enter manually" by itself, which is why the other tests' manual saves
# pass the manual_open check. These close it on purpose.)

def test_station_refuses_a_manual_save_whose_fields_were_never_opened(sim):
    import urllib.error
    sim.blank(gross=1220)
    spool = sim.get_json("/_sim/state")["spool"]
    form = dict(id=spool, product=0, source="manual", vendor="eSun", material="PLA",
                color="Black", profile="eSun 1kg")
    try:
        sim._post("/api/onboard", **form)
        assert False, "accepted"
    except urllib.error.HTTPError as e:
        assert e.code == 400
    assert sim.get_json("/api/spools")[0]["needs_onboarding"] is True   # nothing written
    assert sim.get_json("/api/products") == []
    try:   # opened, but not marked as a new product: still refused
        sim._post("/api/onboard", manual_open=1, **form)
        assert False, "accepted"
    except urllib.error.HTTPError as e:
        assert e.code == 400
    sim._post("/api/onboard", manual_open=1, confirm_new=1, **form)
    assert sim.get_json("/api/spools")[0]["needs_onboarding"] is False


def test_page_asks_what_the_spool_is_before_saving_unopened_manual_fields(sim, page):
    sim.blank(gross=1220)
    page.goto(sim.url("/onboard"))
    page.select_option("select#profile", index=1)
    page.evaluate("document.getElementById('cat-manual-details').open = false")
    msgs = []
    page.once("dialog", lambda d: (msgs.append(d.message), d.dismiss()))
    page.click("button[type=submit]")
    page.wait_for_timeout(300)
    assert msgs and "Say what this spool is" in msgs[0]
    assert "/onboard" in page.url


def test_capture_tare_asks_before_using_a_full_spool_as_the_tare(sim, page):
    sim.blank(gross=1220)                       # the full spool is what's on the scale
    page.goto(sim.url("/onboard"))
    msgs = []
    page.once("dialog", lambda d: (msgs.append(d.message), d.dismiss()))
    page.click("text=Capture tare from scale")
    page.wait_for_timeout(500)
    assert msgs and "1220 g" in msgs[0]
    assert page.input_value("#tare") == ""      # Cancel kept it blank
    page.once("dialog", lambda d: d.accept())
    page.click("text=Capture tare from scale")
    page.wait_for_function("document.getElementById('tare').value == '1220.0'")


def test_cost_comes_after_what_the_spool_is_and_its_tare(sim, page):
    sim.blank(gross=1220)
    page.goto(sim.url("/onboard"))
    order = page.evaluate("""() => ['#cat-manual-details', '#tare', 'input[name=cost]']
        .map(s => document.querySelector(s))
        .map(e => [...document.querySelectorAll('*')].indexOf(e))""")
    assert order == sorted(order)


def test_stock_list_refuses_a_barcode_with_a_bad_check_digit(sim):
    import urllib.error
    pid = _catalog_onboard(sim, EAN)
    bad = UPC[:-1] + str((int(UPC[-1]) + 1) % 10)
    try:
        sim._post("/api/stock/add", product=pid, min_spools=1, pack_qty=1, gtin=bad)
        assert False, "accepted"
    except urllib.error.HTTPError as e:
        assert e.code == 400
    assert not [r for r in sim.get_json("/api/stock")["items"] if r["gtin"] == bad]
    sim._post("/api/stock/add", product=pid, min_spools=1, pack_qty=1, gtin=f" {UPC} ")
    assert [r["gtin"] for r in sim.get_json("/api/stock")["items"] if r["gtin"]] == [UPC]


# ── Names are labels (docs/design/names-are-labels.md) ────────────────────────

def _stock_item(sim, pid):
    return next(r for r in sim.get_json("/api/stock")["items"]
                if r["id"] in [s["id"] for s in sim.get_json("/api/lookup?gtin=" + EAN)["stock"]])


def test_renaming_a_product_changes_nothing_reorder_or_popularity_count(sim, page):
    pid = _catalog_onboard(sim, EAN)
    sim._post("/api/stock/add", product=pid, min_spools=3, pack_qty=1)
    spool = sim.get_json("/api/spools")[0]["spool"]
    sim.place(spool, gross=1220)
    sim.place(spool, gross=1120)                       # 100 g used
    before = _stock_item(sim, pid)["grams_in_window"]
    assert before > 0
    page.goto(sim.url("/reorder"))
    assert f"product #{pid}" in page.content()       # 1 spool < 3: on the list

    p = next(x for x in sim.get_json("/api/products") if x["id"] == pid)
    sim._post("/api/product", id=pid, vendor="Totally Different", material="Renamed Filament",
              abbr="PLA", color="#000000", dia=1.75, empty_g=220, nom_g=1000, gtin=EAN)
    assert _stock_item(sim, pid)["grams_in_window"] == before
    # Used after the rename: still this item's. (Keyed by name, it went to
    # the new name, which no Stock List row carries.)
    sim.place(spool, gross=1020)
    assert _stock_item(sim, pid)["grams_in_window"] == before + 100
    page.goto(sim.url("/reorder"))
    row = page.locator("tr", has_text=f"product #{pid}")
    assert "0.8 spools (800 g)" in row.inner_text()


def test_reorder_never_guesses_an_unlinked_item(sim, page):
    # The default Stock List is all unlinked: none of it is listed as low,
    # and the page says how many it couldn't count.
    _catalog_onboard(sim, EAN, vendor="eSun", material="PLA+ Black")
    page.goto(sim.url("/reorder"))
    assert page.locator("#reorder-unlinked").is_visible()
    assert "Nothing is below half its target" in page.content()
    assert sim.get("/reorder?format=csv").strip().count("\n") == 0   # header only



def test_a_typed_entry_never_matches_a_product_by_name(sim, page):
    # Two spools typed in with the same vendor/material/colour/profile, each
    # ticked "new product" and declining the offered match, make two products:
    # names don't decide identity. (Repair finds them as duplicates.)
    for gross in (1220, 1100):
        sim.blank(gross=gross)
        page.goto(sim.url("/onboard"))
        if page.locator("#stk-list .stk.new").count():   # products exist: choose New
            page.click("#stk-list .stk.new")
        page.select_option("select#profile", index=1)
        page.check("#confirm-new")
        page.once("dialog", lambda d: d.dismiss())        # "already a product. Use it?" -> no
        page.click("button[type=submit]")
        page.wait_for_url("**/spool?id=*")
        sim.remove()
    assert len(sim.get_json("/api/products")) == 2
    page.goto(sim.url("/repair"))
    assert page.locator("tr.dup").count() == 1


def test_a_typed_entry_matching_a_listed_product_is_offered_it(sim, page):
    _onboard_manually(sim, page)
    sim.remove()
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    page.click("#stk-list .stk.new")
    page.select_option("select#profile", index=1)
    page.check("#confirm-new")
    msgs = []
    page.once("dialog", lambda d: (msgs.append(d.message), d.accept()))   # use it
    page.click("button[type=submit]")
    page.wait_for_url("**/spool?id=*")
    assert msgs and "is already a product" in msgs[0]
    assert len(sim.get_json("/api/products")) == 1
    assert len({s["product"] for s in sim.get_json("/api/spools")}) == 1


def test_unticked_new_product_is_explained_not_saved(sim, page):
    sim.blank(gross=1220)
    page.goto(sim.url("/onboard"))
    page.select_option("select#profile", index=1)
    msgs = []
    page.once("dialog", lambda d: (msgs.append(d.message), d.dismiss()))
    page.click("button[type=submit]")
    page.wait_for_timeout(300)
    assert msgs and "This is a new product" in msgs[0]
    assert "/onboard" in page.url


# ── Reorder: grams on hand against half the target ───────────────────────────

def test_reorder_lists_an_item_below_half_its_target_by_weight(sim, page):
    first = _onboard_manually(sim, page)          # one full-ish spool
    pid = first["product"]
    sim._post("/api/stock/add", product=pid, min_spools=1, pack_qty=1)
    sim.remove()
    full = first["remaining_g"] + first["used_g"]   # the spool's nominal weight
    tare = 1220 - first["remaining_g"]               # _onboard_manually's gross

    # A spool with more than half left: not listed, though only one is kept.
    page.goto(sim.url("/reorder"))
    assert page.locator(f"tr:has(a[href='/product?id={pid}'])").count() == 0

    # Weigh it down to under half: listed, with one spool to order.
    sim.place(first["spool"], gross=tare + 0.4 * full)
    sim.remove()
    page.goto(sim.url("/reorder"))
    row = page.locator(f"tr:has(a[href='/product?id={pid}'])")
    assert row.count() == 1
    assert "0.4 spools" in row.inner_text()
    assert row.locator("td").nth(4).inner_text() == "1 spool"

    csv = sim.get("/reorder?format=csv").splitlines()
    assert ",to_order_spools," in csv[0]
    assert any(l.endswith(",1,0,1,0") for l in csv[1:])   # target 1, no grams target, order 1, no subs
    assert page.js_errors == []


def test_reorder_orders_target_minus_on_hand_in_whole_spools(sim, page):
    first = _onboard_manually(sim, page)
    pid = first["product"]
    sim._post("/api/stock/add", product=pid, min_spools=4)
    sim.remove()
    # One full spool against a target of 4: below 2 spools' worth, so listed,
    # with 3 to order.
    page.goto(sim.url("/reorder"))
    row = page.locator(f"tr:has(a[href='/product?id={pid}'])")
    assert row.count() == 1
    assert row.locator("td").nth(4).inner_text() == "3 spools"


def test_stock_list_sorts_by_clicking_a_heading(sim, page):
    page.goto(sim.url("/stock"))
    col = lambda i: page.eval_on_selector_all(
        "#stock-table tr", f"rs => rs.slice(1).filter(r => r.cells.length > 1).map(r => r.cells[{i}].textContent.trim())")
    page.click("#stock-table th:has-text('Material')")
    names = col(1)
    assert len(names) > 3 and names == sorted(names, key=str.lower)
    page.click("#stock-table th:has-text('Material')")
    assert col(1) == sorted(names, key=str.lower, reverse=True)
    page.click("#stock-table th:has-text('Target')")
    targets = page.eval_on_selector_all(
        "#stock-table tr", "rs => rs.slice(1).filter(r => r.cells.length > 1).map(r => parseFloat(r.cells[3].dataset.v))")
    assert targets == sorted(targets)
    assert "▲" in page.inner_text("#stock-table th:has-text('Target')")
    assert page.js_errors == []


def test_every_table_sorts_and_inventory_keeps_each_spool_list_with_its_row(sim, page):
    assert sim.cmd("SEED 6 20").startswith("ok")
    page.goto(sim.url("/"))
    pairs = lambda: page.eval_on_selector_all(
        "table:first-of-type tr.inv-row",
        "rs => rs.map(r => [r.cells[1].textContent.trim(), r.nextElementSibling.id,"
        " parseFloat(r.cells[3].textContent)])")
    before = {name: detail for name, detail, _ in pairs()}
    assert len(before) > 2
    page.click("th:has-text('Remaining')")
    after = pairs()
    grams = [g for _, _, g in after]
    assert grams == sorted(grams)
    assert {name: detail for name, detail, _ in after} == before   # lists moved with their rows
    page.click("th:has-text('Filament')")
    names = [n for n, _, _ in pairs()]
    assert names == sorted(names, key=str.lower)

    page.goto(sim.url("/usage"))
    page.locator("th:has-text('Consumed')").first.click()
    vals = page.eval_on_selector_all(
        "table:first-of-type tr",
        "rs => rs.slice(1).map(r => parseFloat(r.cells[1].textContent.replace(/,/g,'')))")
    assert vals == sorted(vals)
    assert page.js_errors == []


def test_stock_list_tare_starts_at_175_and_a_catalog_weight_replaces_it(sim, page):
    page.goto(sim.url("/stock"))
    assert page.input_value("#stock-tare") == "175"
    submit = "document.querySelector('#stockform ~ .card form').onsubmit()"
    pick = ("document.getElementById('cat-source').value='catalog';"
            "document.getElementById('cat-f-cat_empty_g').value='212';")
    page.evaluate(pick)
    assert page.evaluate(submit) is True
    assert page.input_value("#stock-tare") == "212"          # untouched: catalog wins

    page.goto(sim.url("/stock"))
    page.fill("#stock-tare", "180")                           # typed: the person wins
    page.evaluate(pick)
    page.evaluate(submit)
    assert page.input_value("#stock-tare") == "180"


# ── Stock List remembers the last choice (setting up a stock list in a row) ───

def _stock_add_manual(page, sim, material, color):
    page.goto(sim.url("/stock"))
    if page.locator("#item-list .stk.new").count():   # products exist: choose New
        page.click("#item-list .stk.new")
    page.click("#stock-manual-details summary")
    page.select_option("select[name=vendor]", "eSun")
    page.select_option("select[name=material]", material)
    page.select_option("select[name=color]", color)
    box = page.locator("#confirm-new")          # names-are-labels: a typed entry is new
    if box.count():
        box.check()
    page.click("#stockform ~ .card button[type=submit]")
    page.wait_for_url("**/stock")


def test_stock_list_manual_pickers_start_on_the_last_choice(sim, page):
    _stock_add_manual(page, sim, "PLA", "Black")
    page.goto(sim.url("/stock"))
    assert page.input_value("select[name=material]") == "PLA"
    _stock_add_manual(page, sim, "PETG", "White")
    page.goto(sim.url("/stock"))
    assert page.input_value("select[name=material]") == "PETG"
    assert page.input_value("select[name=color]") == "White"
    assert page.js_errors == []


def test_stock_list_catalog_search_starts_on_the_last_stock_list_pick(sim, page):
    def add(pkg, tok, name):
        sim._post("/api/stock/add", source="catalog", min_spools=1,
                  cat_vendor="eSUN", cat_material=name, cat_abbr=tok, cat_nom_g=1000,
                  cat_empty_g=200, cat_dia=1.75, cat_vendor_slug="esun",
                  cat_type_tok=tok.lower(), cat_pkg_path=pkg)
    add("data/packages/esun/esun-pla-black.yaml", "PLA", "PLA Black")
    add("data/packages/esun/esun-petg-white.yaml", "PETG", "PETG White")
    page.goto(sim.url("/stock"))
    last = page.locator("#cat-last")
    assert last.count() == 1
    assert last.get_attribute("data-pkg") == "data/packages/esun/esun-petg-white.yaml"
    assert last.get_attribute("data-type") == "petg"
    # Restock keeps its own memory: a Stock List pick doesn't change it.
    sim.blank()
    page.goto(sim.url("/onboard"))
    assert page.locator("#cat-last").count() == 0


def test_stock_list_target_starts_at_one(sim, page):
    page.goto(sim.url("/stock"))
    assert page.input_value("input[name=min_spools]") == "1"


def test_stock_list_item_is_a_scrolling_list(sim, page):
    pid = _linked_stock_row(sim, page)                 # on the Stock List already
    other = _catalog_onboard(sim, EAN, material="PETG Blue")
    page.goto(sim.url("/stock"))
    assert page.locator("select#sprod").is_hidden()
    assert page.locator("#stocknew").is_hidden()        # nothing chosen yet
    assert "already on the Stock List" in page.inner_text(f"#item-list .stk[data-prod='{pid}']")

    msgs = []                                           # nothing chosen: refused
    page.once("dialog", lambda d: (msgs.append(d.message), d.accept()))
    page.click("#stockform ~ .card button[type=submit]")
    page.wait_for_timeout(200)
    assert msgs and "Pick the item first" in msgs[0] and "/stock" in page.url

    page.fill("#item-q", "petg")
    page.press("#item-q", "Enter")                      # the only match: picked
    assert page.input_value("select#sprod") == str(other)
    assert page.locator("#item-list").is_hidden() and page.locator("#stocknew").is_hidden()
    page.click("#item-change")
    page.click("#item-list .stk.new")
    assert page.input_value("select#sprod") == "0"
    assert page.locator("#stocknew").is_visible()

    page.goto(sim.url("/stock"))                        # an unknown barcode opens New
    scan(page, gs1("400000000000"))
    page.wait_for_selector("#scan-msg:has-text('no product has this barcode')")
    assert page.locator("#stocknew").is_visible()
    assert page.js_errors == []


def test_stock_list_new_material_takes_the_items_diameter(sim, page):
    page.goto(sim.url("/stock"))
    if page.locator("#item-list .stk.new").count():
        page.click("#item-list .stk.new")
    page.click("#stock-manual-details summary")
    assert page.locator("input[name=material_new_dia]").get_attribute("type") == "hidden"
    page.fill("#stock-dia", "2.85")
    assert page.input_value("#material-new-dia") == "2.85"
    assert page.locator("label:text-is('Diameter (mm)')").count() == 1


# ── The station's eSUN US catalogue (tools/esun-catalogue/station_catalogue.py) ──
# A small catalogue in the real format. Every value a pick needs is in the
# row; the station only stores, serves and copies it. These tests run with the
# internet blocked (conftest), like a station whose online catalog won't load.
_CAT_FIELDS = ["pn", "fid", "material", "colour", "size_g", "dia", "packaging", "name", "list_label", "abbr",
               "type", "gtin", "rgba", "tare", "brand_uuid", "pkg_uuid", "mat_uuid", "temps"]
_BRAND = "db92e17c3c305fa9bec91022e233a216"
_CAT_ITEMS = [
    ["PLA+175B1P1", "PLA+175B", "PLA+", "Black", 1000, 1.75, "Paper spool", "PLA+ Black",
     "PLA+ Black · 6922572219021", "PLA Pro", 0, "6922572219021", "000000ff", None, _BRAND,
     "0b22fb8db1c653ae8cb24cd80e9c4e78", "b1e17cfb4cc85181b88076343255214b", [210, 230, 45, 60]],
    ["PLA+175B1R1", "PLA+175B", "PLA+", "Black", 1000, 1.75, "Refill", "PLA+ Black Refill",
     "PLA+ Black Refill · 6922572208148", "PLA Pro", 0, "6922572208148", "000000ff", None, _BRAND,
     "7f73a76f429f5054827b94d2dbeff33c", "b1e17cfb4cc85181b88076343255214b", [210, 230, 45, 60]],
    ["PETG+175H1P1", "PETG+175H", "PETG+", "Grey", 1000, 1.75, "Paper spool", "PETG+ Grey",
     "PETG+ Grey", "PETG", 1, "", "", None, _BRAND, "93a4a0ad1bf25c13a7c1f7c2c1d6e0aa",
     "5d0aa0e2a8b25b6e9d5c2bf8c2a8f9f1", [230, 250, 75, 90]],
    # A spool with a barcode and its refill without one, as eSUN US sells
    # 16 PLA+ colours: same filament, same weight, two products.
    ["PLA+175U1P1", "PLA+175U", "PLA+", "Aqua", 1000, 1.75, "Paper spool", "PLA+ Aqua",
     "PLA+ Aqua · 6922572218796", "PLA Pro", 0, "6922572218796", "00b0c0ff", None, _BRAND,
     "4c1d2e3f405162738495a6b7c8d9eaf0", "a1a2a3a4a5a6a7a8a9aaabacadaeafb0", [210, 230, 45, 60]],
    ["PLA+175U1R1", "PLA+175U", "PLA+", "Aqua", 1000, 1.75, "Refill", "PLA+ Aqua Refill",
     "PLA+ Aqua Refill", "PLA Pro", 0, "", "00b0c0ff", None, _BRAND,
     "5d2e3f40516273849506a7b8c9daebf1", "a1a2a3a4a5a6a7a8a9aaabacadaeafb0", [210, 230, 45, 60]],
]


def _upload_catalogue(sim, items=None):
    items = _CAT_ITEMS if items is None else items
    data = json.dumps({"date": "2026-11-06", "count": len(items), "vendor": "eSUN", "source": "test",
                       "fields": _CAT_FIELDS, "items": items}).encode()
    return json.loads(sim.upload("/catalogue/import?json=1", data, filename="station-catalogue.json"))


def _cat_field(page, name):
    return page.input_value("#cat-f-" + name)


def _wait_catalogue(page):
    page.wait_for_function("document.getElementById('cat-in-vendor') && !document.getElementById('cat-in-vendor').disabled")


def _pick(page, field, text):
    page.click("#cat-in-" + field)
    page.locator(f"#cat-panel-{field} .cat-row", has_text=text).first.click()


def test_catalogue_upload_is_checked_and_shown_on_administration(sim, page):
    page.goto(sim.url("/config"))
    assert "None loaded" in page.inner_text("#catalogue-status")
    assert _upload_catalogue(sim) == {"count": len(_CAT_ITEMS), "date": "2026-11-06"}
    import urllib.error
    with pytest.raises(urllib.error.HTTPError):   # not a catalogue: refused ...
        sim.upload("/catalogue/import?json=1", b"{\"vendors\":[]}", filename="x.json")
    assert sim.get_json("/catalogue.json")["count"] == len(_CAT_ITEMS)   # ... and the loaded one kept
    page.goto(sim.url("/config"))
    assert "%d items" % len(_CAT_ITEMS) in page.inner_text("#catalogue-status")


def test_restock_searches_the_catalogue_offline_and_copies_its_values(sim, page):
    _upload_catalogue(sim)
    sim.blank(gross=1220)
    page.goto(sim.url("/onboard"))
    _wait_catalogue(page)
    page.click("#cat-in-vendor")
    rows = page.locator("#cat-panel-vendor .cat-row")
    assert "eSUN US catalogue" in rows.first.inner_text()   # first, and works without internet
    rows.first.click()
    _pick(page, "type", "PLA+")
    _pick(page, "material", "PLA+ Black Refill")
    assert page.input_value("#cat-source") == "catalog"
    assert _cat_field(page, "cat_material") == "PLA+ Black Refill"
    assert _cat_field(page, "cat_abbr") == "PLA Pro" and _cat_field(page, "cat_type") == "0"
    assert _cat_field(page, "cat_gtin") == "6922572208148"
    assert _cat_field(page, "cat_mat_uuid") == "b1e17cfb4cc85181b88076343255214b"
    assert _cat_field(page, "cat_print_min") == "210" and _cat_field(page, "cat_bed_max") == "60"
    assert "from the eSUN US catalogue" in page.inner_text("#cat-resolved")
    page.select_option("select#profile", index=1)          # the catalogue has no tare: the profile's
    page.click("button[type=submit]")
    page.wait_for_url("**/spool?id=*")
    prods = sim.get_json("/api/products")
    p = [x for x in prods if x["material"] == "PLA+ Black Refill"][0]
    assert p["vendor"] == "eSUN" and p["gtin"] == "6922572208148" and p["abbr"] == "PLA Pro"
    assert p["material_uuid"] == "b1e17cfb4cc85181b88076343255214b"
    assert p["brand_uuid"] == _BRAND and p["package_uuid"] == "7f73a76f429f5054827b94d2dbeff33c"
    assert page.js_errors == []


def _restock_catalogue_item(sim, pn):
    """Save the spool on the scale as catalogue item `pn`, posting exactly the
    fields a pick fills."""
    it = dict(zip(_CAT_FIELDS, next(i for i in _CAT_ITEMS if i[0] == pn)))
    sim.blank(gross=1220)
    spool = sim.get_json("/_sim/state")["spool"]
    sim._post("/api/onboard", id=spool, source="catalog", profile="", empty_g=200,
              cat_vendor="eSUN", cat_material=it["name"], cat_abbr=it["abbr"], cat_type=it["type"],
              cat_dia=1.75, cat_nom_g=it["size_g"], cat_brand_uuid=_BRAND, cat_gtin=it["gtin"],
              cat_pkg_uuid=it["pkg_uuid"], cat_mat_uuid=it["mat_uuid"])
    sim.remove()


def test_the_same_catalogue_item_twice_is_one_product(sim, page):
    """Even without a barcode: the item's package ID finds it again."""
    _upload_catalogue(sim)
    for _ in range(2):
        _restock_catalogue_item(sim, "PETG+175H1P1")
    prods = [p for p in sim.get_json("/api/products") if p["material"] == "PETG+ Grey"]
    assert len(prods) == 1


@pytest.mark.parametrize("order", [("PLA+175U1P1", "PLA+175U1R1"), ("PLA+175U1R1", "PLA+175U1P1")])
def test_a_spool_and_its_refill_are_two_products(sim, page, order):
    """Same filament, same 1 kg: only the package tells them apart, and it must,
    whichever arrives first. Then a second box of each finds its own."""
    _upload_catalogue(sim)
    for pn in order + order:
        _restock_catalogue_item(sim, pn)
    prods = {p["material"]: p["id"] for p in sim.get_json("/api/products") if "Aqua" in p["material"]}
    assert sorted(prods) == ["PLA+ Aqua", "PLA+ Aqua Refill"]
    per = collections.Counter(s["product"] for s in sim.get_json("/api/spools"))
    assert per[prods["PLA+ Aqua"]] == 2 and per[prods["PLA+ Aqua Refill"]] == 2


def test_manual_entry_with_esun_lists_esun_materials(sim, page):
    _upload_catalogue(sim)
    sim.blank(gross=1220)
    page.goto(sim.url("/onboard"))
    _wait_catalogue(page)
    page.click("#cat-manual-details summary")
    vendors = page.eval_on_selector_all("select[name=vendor] option", "o => o.map(x => x.value)")
    esun = [v for v in vendors if v.lower() == "esun"][0]
    page.select_option("select[name=vendor]", esun)
    mats = page.eval_on_selector_all("select[name=material] option", "o => o.map(x => x.textContent)")
    assert "PLA+" in mats and "PETG+" in mats and "Not in the eSUN US list…" in mats
    page.select_option("select[name=material]", "PLA+")
    page.select_option("select[name=color]", "Black")
    assert page.is_visible("#esun-package")                  # spool and refill
    page.select_option("#esun-package", "PLA+175B1R1")
    assert page.input_value("#cat-source") == "catalog"
    assert _cat_field(page, "cat_material") == "PLA+ Black Refill"
    # Not in the list: the ordinary materials come back, and it's a typed entry again.
    page.select_option("select[name=material]", "__other__")
    assert page.input_value("#cat-source") == "manual"
    mats = page.eval_on_selector_all("select[name=material] option", "o => o.map(x => x.textContent)")
    assert "PLA+" not in mats or "Not in the eSUN US list…" not in mats
    assert page.js_errors == []


def test_scanning_a_catalogue_barcode_picks_it_as_a_new_product(sim, page):
    _upload_catalogue(sim)
    sim.blank(gross=1220)
    page.goto(sim.url("/onboard"))
    _wait_catalogue(page)
    scan(page, "6922572219021")
    page.wait_for_selector("#scan-msg:not([hidden])")
    assert "from the eSUN US catalogue" in page.inner_text("#scan-msg")
    assert page.input_value("#cat-source") == "catalog"
    assert _cat_field(page, "cat_material") == "PLA+ Black"


def test_stock_list_scan_and_search_use_the_catalogue(sim, page):
    _upload_catalogue(sim)
    page.goto(sim.url("/stock"))
    _wait_catalogue(page)
    scan(page, "6922572208148")
    page.wait_for_selector("#scan-msg:not([hidden])")
    assert "from the eSUN US catalogue" in page.inner_text("#scan-msg")
    assert _cat_field(page, "cat_material") == "PLA+ Black Refill"


def test_without_a_catalogue_the_pages_behave_as_before(sim, page):
    sim.blank(gross=1220)
    page.goto(sim.url("/onboard"))
    page.wait_for_timeout(500)
    assert "Couldn't load the catalog" in page.inner_text("#cat-status")
    assert page.evaluate("document.getElementById('cat-manual-details').open")
    assert page.js_errors == []


def test_scan_then_a_catalogue_item_without_a_barcode_teaches_it(sim, page):
    """The box's barcode is unknown, and the catalogue item has none either:
    saving records the scanned code on the new product, so the next box of it
    is recognised."""
    _linked_stock_row(sim, page)                          # some other product exists
    _upload_catalogue(sim)
    code = gs1("400000000777")
    sim.blank(gross=1220)
    page.goto(sim.url("/onboard"))
    _wait_catalogue(page)
    scan(page, code)
    page.wait_for_selector("#scan-msg:has-text('Unknown barcode')")
    page.click("#stk-list .stk.new")                      # + New product...
    page.click("#cat-in-vendor")
    page.locator("#cat-panel-vendor .cat-row", has_text="eSUN US catalogue").click()
    _pick(page, "type", "PETG+")
    _pick(page, "material", "PETG+ Grey")
    assert _cat_field(page, "cat_gtin") == ""
    assert "will be recorded for the new product" in page.inner_text("#scan-learn")
    page.select_option("select#profile", index=1)
    msgs = []
    page.once("dialog", lambda d: (msgs.append(d.message), d.accept()))
    page.click("button[type=submit]")
    page.wait_for_url("**/spool?id=*learned=1*")
    assert msgs and "scanned before the product was chosen" in msgs[0]
    found = sim.get_json("/api/lookup?gtin=" + code)["products"]
    assert len(found) == 1 and found[0]["material"] == "PETG+ Grey"
    pid = found[0]["id"]
    sim.remove()
    sim.blank(gross=1050)
    page.goto(sim.url("/onboard"))
    scan(page, code)
    page.wait_for_function("document.getElementById('prod').value==%r" % str(pid))


# ── Substitutes (CfgStock::subs) ──────────────────────────────────────────────

SUB_GTIN = gs1("400000000001")


def _item_with_substitute_on_shelf(sim, page):
    """A Stock List item (target 2) and a separate product with a spool of
    1 kg, not linked yet. Returns (item, usual product, other product)."""
    usual = _linked_stock_row(sim, page)
    other = _catalog_onboard(sim, SUB_GTIN, vendor="Overture", material="PLA Black")
    return sim.get_json("/api/stock")["items"][-1], usual, other


def _save_subs(sim, item, usual, subs):
    sim._post("/api/stock/update", id=item["id"], product=usual, min_spools=2,
              pack_qty=1, subs=subs)


def _status(fn, *a, **k):
    import urllib.error
    try:
        fn(*a, **k)
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode()
    return 200, ""


def test_restock_links_a_substitute_and_only_offers_it_for_other_products(sim, page):
    item, usual, other = _item_with_substitute_on_shelf(sim, page)
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    assert page.locator("#sub-box").is_hidden()

    page.click(f"#stk-list .stk[data-prod='{usual}'][data-sid]")
    assert page.locator("#sub-box").is_hidden()           # it IS the Stock List item
    page.click("#stk-clear")

    page.click(f"#stk-list .stk[data-prod='{other}']")
    assert page.locator("#sub-box").is_visible()
    page.fill("#sub-q", "no such thing")
    assert page.locator(f"#sub-list .stk[data-to='{item['id']}']").is_hidden()
    page.fill("#sub-q", "")
    page.click(f"#sub-list .stk[data-to='{item['id']}']")
    assert page.input_value("#stands-for") == str(item["id"])
    page.click("button[type=submit]")
    page.wait_for_url("**/spool?id=*")
    assert "now counts toward" in page.inner_text("#sub-result")

    assert sim.get_json("/api/stock")["items"][-1]["substitutes"] == [other]
    # The spool keeps its identity: it is still the other product.
    assert sim.get_json("/api/spools")[-1]["product"] == other

    # The next spool of it needs no second choice, and says where it counts.
    sim.remove()
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    page.click(f"#stk-list .stk[data-prod='{other}']")
    assert page.locator("#sub-box").is_hidden()
    assert "Counts toward" in page.inner_text("#sub-note")
    assert page.js_errors == []


def test_a_one_off_links_nothing(sim, page):
    item, _, other = _item_with_substitute_on_shelf(sim, page)
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    page.click(f"#stk-list .stk[data-prod='{other}']")
    page.click("button[type=submit]")
    page.wait_for_url("**/spool?id=*")
    assert page.locator("#sub-result").count() == 0
    assert sim.get_json("/api/stock")["items"][-1]["substitutes"] == []


def test_reorder_counts_substitutes_and_shows_them_split(sim, page):
    item, usual, other = _item_with_substitute_on_shelf(sim, page)
    # Run the usual spool down, so only the substitute can keep it off the list.
    spool = sim.get_json("/api/spools")[0]["spool"]
    sim.place(spool, gross=300)
    sim.remove()
    page.goto(sim.url("/reorder"))
    assert page.locator("tr", has_text=f"product #{usual}").count() == 1

    _save_subs(sim, item, usual, str(other))
    page.goto(sim.url("/reorder"))
    assert page.locator("tr", has_text=f"product #{usual}").count() == 0
    covered = page.inner_text("#reorder-covered")
    assert "1000 g" in covered

    # Run the substitute down too: back on the list, with the split shown.
    sub_spool = next(s for s in sim.get_json("/api/spools") if s["product"] == other)["spool"]
    sim.place(sub_spool, gross=520)
    sim.remove()
    page.goto(sim.url("/reorder"))
    row = page.locator("tr", has_text=f"product #{usual}")
    assert "300 g of it substitutes" in row.inner_text()
    assert page.locator("#reorder-covered").count() == 0
    csv = sim.get("/reorder?format=csv").strip().split("\n")
    assert csv[0].endswith(",on_hand_substitute_g")
    assert csv[1].endswith(",300")


def test_a_substitute_counts_toward_popularity_not_usage(sim, page):
    item, usual, other = _item_with_substitute_on_shelf(sim, page)
    _save_subs(sim, item, usual, str(other))
    sub_spool = next(s for s in sim.get_json("/api/spools") if s["product"] == other)["spool"]
    sim.place(sub_spool, gross=1220)
    sim.place(sub_spool, gross=1120)                     # 100 g of the substitute used
    sim.remove()
    assert sim.get_json("/api/stock")["items"][-1]["grams_in_window"] == 100

    # Usage still names what was really used, under the real product.
    usage = sim.get("/usage.csv")
    assert "Overture" in usage


def test_stock_form_adds_and_removes_substitutes(sim, page):
    item, usual, other = _item_with_substitute_on_shelf(sim, page)
    page.goto(sim.url(f"/stock?edit={item['id']}#stockform"))
    page.fill("#subs-q", "Overture")
    page.click(f"#subs-add .stk[data-prod='{other}']")
    assert page.input_value("#subs") == str(other)
    page.click("#stockform ~ .card button[type=submit]")
    page.wait_for_url("**/stock")
    assert sim.get_json("/api/stock")["items"][-1]["substitutes"] == [other]
    assert "+ substitutes" in page.inner_text("#stock-table")

    page.goto(sim.url(f"/product?id={other}"))
    assert "counts toward" in page.inner_text("#product-subof")

    page.goto(sim.url(f"/stock?edit={item['id']}#stockform"))
    assert page.locator(f"#subs-add .stk[data-prod='{other}']").is_hidden()
    page.click("#subs-now a")
    assert page.input_value("#subs") == ""
    page.click("#stockform ~ .card button[type=submit]")
    page.wait_for_url("**/stock")
    assert sim.get_json("/api/stock")["items"][-1]["substitutes"] == []
    assert page.js_errors == []


def test_a_save_without_the_subs_field_keeps_them(sim, page):
    item, usual, other = _item_with_substitute_on_shelf(sim, page)
    _save_subs(sim, item, usual, str(other))
    sim._post("/api/stock/update", id=item["id"], product=usual, min_spools=5, pack_qty=1)
    row = sim.get_json("/api/stock")["items"][-1]
    assert row["min_spools"] == 5 and row["substitutes"] == [other]


def test_making_a_substitute_the_usual_product_drops_it_from_the_list(sim, page):
    item, usual, other = _item_with_substitute_on_shelf(sim, page)
    _save_subs(sim, item, usual, str(other))
    sim._post("/api/stock/update", id=item["id"], product=other, min_spools=2, pack_qty=1,
              subs=f"{other},{usual}")
    assert sim.get_json("/api/stock")["items"][-1]["substitutes"] == [usual]


def test_a_substitute_belongs_to_one_item(sim, page):
    item, usual, other = _item_with_substitute_on_shelf(sim, page)
    third = _catalog_onboard(sim, gs1("400000000002"), vendor="Sunlu", material="PLA Black")
    sim._post("/api/stock/add", product=third, min_spools=1, pack_qty=1)
    second = sim.get_json("/api/stock")["items"][-1]

    # Another item's usual product can't be a substitute: it counts there.
    code, body = _status(_save_subs, sim, item, usual, str(third))
    assert code == 400 and "already counts there" in body

    _save_subs(sim, item, usual, str(other))
    code, body = _status(_save_subs, sim, second, third, str(other))
    assert code == 400 and "already counts toward" in body
    code, body = _status(_save_subs, sim, item, usual, "99999")
    assert code == 400 and "unknown product" in body

    # And the Restock path refuses before saving anything about the spool.
    sim.blank(gross=1100)
    spool = sim.get_json("/_sim/state")["spool"]
    code, body = _status(sim._post, "/api/onboard", id=spool, product=other,
                         stands_for=second["id"])
    assert code == 400 and "already counts toward" in body
    assert next(s for s in sim.get_json("/api/spools") if s["spool"] == spool)["product"] != other
    assert page.js_errors == []


def test_stored_substitutes_are_checked_before_they_count(sim, page):
    """A restored config or raw-JSON edit bypasses the forms."""
    item, usual, other = _item_with_substitute_on_shelf(sim, page)
    third = _catalog_onboard(sim, gs1("400000000002"), vendor="Sunlu", material="PLA Black")
    sim._post("/api/stock/add", product=third, min_spools=1, pack_qty=1)

    cfg = sim.get_json("/config/export")
    rows = cfg["stock-items"]
    first, second = rows[-2], rows[-1]
    first["subs"] = [99999, usual, third, other]    # gone, itself, another's usual, real
    second["subs"] = [other]                        # listed twice: the first row wins
    sim.upload("/config/import", json.dumps(cfg).encode())
    items = sim.get_json("/api/stock")["items"]
    assert items[-2]["substitutes"] == [other]
    assert items[-1]["substitutes"] == []


def test_substitutes_survive_a_config_backup_and_restore(sim, page):
    item, usual, other = _item_with_substitute_on_shelf(sim, page)
    _save_subs(sim, item, usual, str(other))
    cfg = sim.get_json("/config/export")
    rows = cfg["stock-items"]
    assert rows[-1]["subs"] == [other]
    assert "subs" not in rows[0]                     # omitted when there are none
    sim.upload("/config/import", json.dumps(cfg).encode())
    assert sim.get_json("/api/stock")["items"][-1]["substitutes"] == [other]


def test_substitutes_follow_a_merge(sim, page):
    item, usual, other = _item_with_substitute_on_shelf(sim, page)
    third = _catalog_onboard(sim, gs1("400000000002"), vendor="Overture", material="PLA Blk")
    _save_subs(sim, item, usual, str(other))
    sim._post("/api/repair/merge", a=other, b=third, into=third)
    assert sim.get_json("/api/stock")["items"][-1]["substitutes"] == [third]


# ── Review fixes (2026-11-07) ─────────────────────────────────────────────────

def test_names_with_quotes_and_ampersands_round_trip(sim, page):
    """An apostrophe used to cut a name short on the product page (single-quoted
    attribute), and the JSON APIs returned '&' as '&amp;'."""
    _upload_catalogue(sim)
    sim.blank(gross=1220)
    spool = sim.get_json("/_sim/state")["spool"]
    name = "PLA \"Bob's\" Red & <Blue>"
    sim._post("/api/onboard", id=spool, source="catalog", profile="", empty_g=200,
              cat_vendor="Bambu's & Co", cat_material=name, cat_abbr="PLA", cat_type=0,
              cat_dia=1.75, cat_nom_g=1000, cat_brand_uuid=_BRAND,
              cat_mat_uuid="c0c1c2c3c4c5c6c7c8c9cacbcccdcecf", cat_pkg_uuid="d0d1d2d3d4d5d6d7d8d9dadbdcdddedf")
    p = next(p for p in sim.get_json("/api/products") if p["vendor"] == "Bambu's & Co")
    assert p["material"] == name                                     # JSON: the text itself
    page.goto(sim.url(f"/product?id={p['id']}"))
    assert page.input_value("input[name=vendor]") == "Bambu's & Co"  # not cut at the apostrophe
    assert page.input_value("input[name=material]") == name
    page.click("button[type=submit]")
    page.wait_for_url(f"**/product?id={p['id']}")
    q = next(q for q in sim.get_json("/api/products") if q["id"] == p["id"])
    assert (q["vendor"], q["material"]) == ("Bambu's & Co", name)   # saved back unchanged
    assert page.js_errors == []


def test_a_restored_config_moves_the_stock_list_id_counter(sim, page):
    """Item ids from a restored config must never be reissued to a new item."""
    pid = _linked_stock_row(sim, page)
    cfg = sim.get_json("/config/export")
    for i, s in enumerate(cfg["stock-items"]):
        s["id"] = 500 + i
    cfg["stock-items"].append(dict(cfg["stock-items"][0], id=500))   # a duplicate, too
    sim.upload("/config/import", json.dumps(cfg).encode(), filename="config.json")
    sim._post("/api/stock/add", product=pid, min_spools=1, pack_qty=1)
    ids = [s["id"] for s in sim.get_json("/config/export")["stock-items"]]
    assert len(ids) == len(set(ids)), ids
    assert max(ids) > 500 + len(cfg["stock-items"]) - 1


def test_status_reports_the_audit_phase(sim, page):
    assert sim.get_json("/api/status")["audit"] == "idle"
    sim._post("/api/audit/start")
    assert sim.get_json("/api/status")["audit"] == "scanning"
    sim._post("/api/audit/abandon")
    assert sim.get_json("/api/status")["audit"] == "idle"
