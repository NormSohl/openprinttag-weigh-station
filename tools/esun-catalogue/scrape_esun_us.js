// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl
//
// Read every product on esun3d.com (eSUN's US store) and send it to receive.py.
//
// esun3d.com sits behind Cloudflare and refuses scripted fetches, so this runs
// inside a normal browser tab: start `python3 receive.py`, open
// https://www.esun3d.com/ , open the developer console, paste this file, press
// Enter. It reads ~140 pages, one every 0.7 s (about two minutes), then submits
// the result as a form post to http://127.0.0.1:8765/ (the page's security policy
// blocks fetch() to localhost, but not a form submission). The tab ends on a
// "saved N chars" page.
//
// Each product page embeds Magento's option data (jsonConfig): every variant's
// eSUN part number ("sku"), its options (colour), whether the US store can sell
// it ("salable", sales channel us_site) and its prices. Nothing is inferred here.
(async () => {
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const get = async (u) => new DOMParser().parseFromString(
    await (await fetch(u, { credentials: "include" })).text(), "text/html");

  // 1. every product page under the three top categories
  const urls = new Set();
  for (const c of ["/filaments", "/resins", "/accessories"]) {
    for (let p = 1; p <= 30; p++) {
      const d = await get(`${c}?p=${p}&product_list_limit=96`);
      const before = urls.size;
      d.querySelectorAll("a.product-item-link, .product-item-info a.product")
        .forEach((a) => a.getAttribute("href") && urls.add(a.getAttribute("href").split("?")[0]));
      console.log(c, "page", p, "products so far", urls.size);
      if (urls.size === before) break;
      await sleep(700);
    }
  }

  // 2. each page's variants
  const out = {};
  let n = 0;
  for (const u of urls) {
    const d = await get(u);
    const title = (d.querySelector("h1.page-title, h1") || {}).textContent?.trim() || "";
    const crumbs = [...d.querySelectorAll(".breadcrumbs li")].map((l) => l.textContent.trim())
      .filter(Boolean).join(" > ").replace(/^Home > /, "");
    let jc = null;
    for (const s of d.querySelectorAll('script[type="text/x-magento-init"]')) {
      if (!/jsonConfig/.test(s.textContent)) continue;
      try {
        const walk = (o) => { if (o && typeof o === "object") { if (o.jsonConfig && o.jsonConfig.attributes) { jc = o.jsonConfig; return; } for (const k in o) walk(o[k]); } };
        walk(JSON.parse(s.textContent));
      } catch (e) { /* not this one */ }
    }
    const v = [];
    if (jc) {
      const opts = {};
      for (const a of Object.values(jc.attributes))
        for (const o of a.options) for (const pid of o.products) (opts[pid] = opts[pid] || []).push(o.label);
      const sal = new Set();
      for (const a of Object.values(jc.salable || {})) for (const arr of Object.values(a)) arr.forEach((p) => sal.add(String(p)));
      for (const pid of new Set([...Object.keys(jc.sku || {}), ...Object.keys(opts)])) {
        const pr = (jc.optionPrices || {})[pid] || {};
        v.push([(jc.sku || {})[pid] || "", (opts[pid] || []).join("/"), sal.has(String(pid)) ? 1 : 0,
                pr.finalPrice?.amount ?? "", pr.oldPrice?.amount ?? ""].join("|"));
      }
    } else {   // a simple (one-variant) product
      const sku = (d.querySelector("[itemprop=sku]") || {}).textContent?.trim() || "";
      const price = d.querySelector("[data-price-amount]")?.getAttribute("data-price-amount") || "";
      const st = d.querySelector(".stock");
      const sal = !st ? "?" : (/in stock|available/i.test(st.textContent) && !/out of stock|unavailable/i.test(st.textContent)) ? 1 : 0;
      v.push([sku, "", sal, price, ""].join("|"));
    }
    out[u.replace("https://www.esun3d.com/", "")] = { t: title, c: crumbs, v };
    console.log(++n, "/", urls.size, title, v.length, "variants");
    await sleep(700);
  }

  // 3. hand it to receive.py
  const f = document.createElement("form");
  f.method = "POST";
  f.action = "http://127.0.0.1:8765/";
  const t = document.createElement("textarea");
  t.name = "d";
  t.value = JSON.stringify(Object.entries(out));
  f.appendChild(t);
  document.body.appendChild(f);
  f.submit();
})();
