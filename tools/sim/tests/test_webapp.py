# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""End-to-end tests of the real web app (src/web_app.cpp) in the simulator."""
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
