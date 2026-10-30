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


def _catalog_onboard(sim, gtin, gross=1220):
    """Onboard the spool on the scale as a catalog pick carrying `gtin`;
    returns the product id."""
    sim.blank(gross=gross)
    spool = sim.get_json("/_sim/state")["spool"]
    sim._post("/api/onboard", id=spool, source="catalog",
              cat_vendor="eSUN", cat_material="PLA+ Black", cat_abbr="PLA",
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


def test_slow_typing_and_bad_check_digits_are_not_scans(sim, page):
    _catalog_onboard(sim, EAN)
    sim.blank(gross=1100)
    page.goto(sim.url("/onboard"))
    page.focus("#prod-q")
    scan(page, EAN, gap_ms=120)                     # a person typing
    bad = EAN[:-1] + str((int(EAN[-1]) + 1) % 10)
    page.fill("#prod-q", "")
    scan(page, bad)                                 # fast, wrong check digit
    page.wait_for_timeout(300)
    assert page.locator("#scan-msg").is_hidden()
    assert page.input_value("#prod-q") == bad
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
    page.click("button[type=submit]")
    page.wait_for_url("**/spool?id=*learned=1*")
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
