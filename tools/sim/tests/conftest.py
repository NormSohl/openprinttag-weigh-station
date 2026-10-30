# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""Fixtures: a fresh simulator per test, and a browser page pointed at it.

Each test gets its own data directory and port, so tests never see each
other's spools. The simulator is built once per session by run.sh --build.
"""
import os
import socket
import subprocess
import tempfile
import time
import urllib.parse
import urllib.request

import pytest

SIM_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.path.join(SIM_DIR, ".deps", "build", "weighstation-sim")


@pytest.fixture(scope="session", autouse=True)
def built():
    subprocess.run([os.path.join(SIM_DIR, "run.sh"), "--build"], check=True,
                   stdout=subprocess.DEVNULL)


def _free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Sim:
    """A running simulator: base URL, its log, and the /_sim controls."""

    def __init__(self, base, log_path):
        self.base = base
        self.log_path = log_path

    def url(self, path="/"):
        return self.base + path

    def _post(self, path, **params):
        data = urllib.parse.urlencode(params).encode()
        with urllib.request.urlopen(self.base + path, data=data) as r:
            return r.read().decode()

    def upload(self, path, data, name="file", filename="upload"):
        """POST `data` as a multipart file, the way the Backup page does."""
        b = "----simboundary"
        body = (f"--{b}\r\nContent-Disposition: form-data; name=\"{name}\"; "
                f"filename=\"{filename}\"\r\nContent-Type: application/octet-stream"
                f"\r\n\r\n").encode() + data + f"\r\n--{b}--\r\n".encode()
        req = urllib.request.Request(self.base + path, data=body, headers={
            "Content-Type": "multipart/form-data; boundary=" + b})
        with urllib.request.urlopen(req) as r:
            return r.read().decode()

    def get(self, path):
        with urllib.request.urlopen(self.base + path) as r:
            return r.read().decode()

    def get_json(self, path):
        import json
        return json.loads(self.get(path))

    # Simulated hardware
    def blank(self, gross=1220):
        return self._post("/_sim/blank", gross=gross)

    def place(self, spool, gross=None):
        p = {"spool": spool}
        if gross is not None:
            p["gross"] = gross
        return self._post("/_sim/place", **p)

    def remove(self):
        return self._post("/_sim/remove")

    def cmd(self, line):
        return self._post("/_sim/cmd", line=line)

    def log(self):
        with open(self.log_path) as f:
            return f.read()


@pytest.fixture
def sim():
    data = tempfile.mkdtemp(prefix="sim-data-")
    port = _free_port()
    log_path = os.path.join(data, "sim.log")
    env = dict(os.environ, STORE_TEST_ROOT=data, SIM_PORT=str(port))
    with open(log_path, "w") as log:
        proc = subprocess.Popen([BINARY], env=env, stdout=log, stderr=subprocess.STDOUT)
    base = f"http://127.0.0.1:{port}"
    for _ in range(100):
        try:
            urllib.request.urlopen(base + "/api/status", timeout=1).read()
            break
        except OSError:
            time.sleep(0.05)
    else:
        proc.kill()
        raise RuntimeError("simulator did not start; see " + log_path)
    try:
        yield Sim(base, log_path)
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


@pytest.fixture(scope="session")
def browser():
    from playwright.sync_api import sync_playwright
    with sync_playwright() as p:
        b = p.chromium.launch(headless=os.environ.get("HEADED") is None)
        yield b
        b.close()


@pytest.fixture
def page(sim, browser):
    """A browser page with outside network blocked: the catalog widget would
    otherwise fetch the live OpenPrintTag database from GitHub, which makes a
    test depend on the internet and on whatever the database holds today."""
    ctx = browser.new_context()
    ctx.route(lambda url: not url.startswith(sim.base), lambda route: route.abort())
    pg = ctx.new_page()
    errors = []
    pg.on("pageerror", lambda e: errors.append(str(e)))
    pg.js_errors = errors
    yield pg
    ctx.close()


def scan(page, code, gap_ms=5):
    """Type a barcode the way a keyboard-wedge scanner does: digits a few ms
    apart, then Enter. Measured scanner timing can replace gap_ms."""
    page.keyboard.type(code, delay=gap_ms)
    page.keyboard.press("Enter")
