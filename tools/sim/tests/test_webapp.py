# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""End-to-end tests of the real web app (src/web_app.cpp) in the simulator."""
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
    page.select_option("select#prod", str(first["product"]))
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
    page.click("#stk-clear")
    assert page.input_value("select#prod") == "0"
    assert page.locator("#stk-ok").is_hidden()
    assert page.locator("#newprod").is_visible()
    page.click(f"#stk-list .stk[data-prod='{pid}']")
    page.select_option("select#prod", "0")
    assert page.locator("#stk-ok").is_hidden()


def test_product_filter_narrows_the_list_and_keeps_the_selection(sim, page):
    pid = _linked_stock_row(sim, page)
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    page.select_option("select#prod", str(pid))
    page.fill("#prod-q", "zzz-no-such-product")
    values = page.eval_on_selector_all("select#prod option", "os => os.map(o => o.value)")
    assert sorted(values) == sorted(["0", str(pid)])   # "new" + the selection stay
    assert page.input_value("select#prod") == str(pid)
    page.press("#prod-q", "Enter")
    assert "/onboard" in page.url
    page.fill("#prod-q", "")
    assert page.locator("select#prod option").count() >= 2


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
    page.fill("#stk-q", "abc")
    page.focus("#stk-q")
    scan(page, EAN)
    page.wait_for_selector("#scan-msg:not([hidden])")
    page.wait_for_function("document.getElementById('prod').value==%r" % str(pid))
    assert "/onboard" in page.url                   # Enter did not submit
    assert page.input_value("#stk-q") == "abc"      # the digits were taken back
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
    page.focus("#prod-q")
    scan(page, EAN, gap_ms=120)                     # a person typing
    page.wait_for_timeout(200)
    assert page.input_value("#prod-q") == EAN       # left alone
    page.fill("#prod-q", "")
    bad = EAN[:-1] + str((int(EAN[-1]) + 1) % 10)
    scan(page, bad)                                 # a scanner, but no product barcode
    page.wait_for_selector("#scan-note:has-text('not a product barcode')")
    assert page.input_value("#prod-q") == ""        # taken back out
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
    page.wait_for_selector("select#prod")
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
    page.select_option("select#prod", str(pid))

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
    page.wait_for_selector("select#prod")
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
    assert csv[0].endswith(",to_order_spools")
    assert any(l.endswith(",1,0,1") for l in csv[1:])   # target 1, no grams target, order 1
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
