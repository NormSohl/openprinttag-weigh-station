# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""Receive the eSUN US scrape that scrape_esun_us.js posts from the browser and
save it as data/esun-us-DATE.json (DATE = today unless set). Stops after one post.

  python3 receive.py
"""
import datetime, http.server, json, os, urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
DATE = os.environ.get("DATE") or datetime.date.today().isoformat()
OUT = os.path.join(HERE, "data", f"esun-us-{DATE}.json")


class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0))).decode()
        d = urllib.parse.parse_qs(body).get("d", [""])[0]
        data = json.loads(d)   # refuse anything that isn't the scrape
        json.dump(data, open(OUT, "w"))
        msg = f"saved {len(data)} pages, {sum(len(v['v']) for _, v in data)} variants to {OUT}"
        print(msg)
        self.send_response(200)
        self.send_header("Content-Type", "text/plain")
        self.end_headers()
        self.wfile.write(msg.encode())
        raise KeyboardInterrupt


os.makedirs(os.path.dirname(OUT), exist_ok=True)
print(f"waiting on http://127.0.0.1:8765/ for scrape_esun_us.js (saves {OUT})")
try:
    http.server.HTTPServer(("127.0.0.1", 8765), H).serve_forever()
except KeyboardInterrupt:
    pass
