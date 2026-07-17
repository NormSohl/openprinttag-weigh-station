#include "web_app.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <vector>
#include <string.h>
#include "config.h"
#include "device_state.h"
#include "opt_tag.h"
#include "store.h"
#include "config_store.h"

// ── Shared globals (defined in main.cpp) ──────────────────────────────────────
extern volatile DeviceState gState;
extern SemaphoreHandle_t    gStateMutex;
extern volatile float       gWeightGrams;
extern SemaphoreHandle_t    gWeightMutex;
extern OptMain              gTagMain;
extern SemaphoreHandle_t    gTagMutex;
extern volatile bool        gWriteMainPending;
extern volatile int         gSpoolId;
extern volatile bool        gSpoolNeedsOnboarding;
extern volatile bool        gScaleCalibrated;
extern volatile bool        gCalZeroReq;
extern volatile float       gCalSetGrams;

static AsyncWebServer sServer(80);

// ── Small HTML helpers ────────────────────────────────────────────────────────

static String esc(const char* s) {
    String o;
    for (const char* p = s; *p; ++p) {
        switch (*p) {
            case '&': o += "&amp;";  break;
            case '<': o += "&lt;";   break;
            case '>': o += "&gt;";   break;
            case '"': o += "&quot;"; break;
            default:  o += *p;       break;
        }
    }
    return o;
}

static const char* STYLE =
    "<style>"
    "body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee}"
    "header{background:#1c2b1c;padding:14px 18px;font-size:20px;font-weight:600}"
    "header a{color:#8f8;text-decoration:none;margin-right:16px;font-size:15px;font-weight:400}"
    "main{padding:18px;max-width:760px;margin:0 auto}"
    "table{border-collapse:collapse;width:100%}"
    "th,td{padding:6px 10px;text-align:left;border-bottom:1px solid #333}"
    "th{color:#9c9}"
    ".sw{display:inline-block;width:14px;height:14px;border-radius:3px;vertical-align:middle;margin-right:6px;border:1px solid #000}"
    ".ob{color:#fd6}"
    "form label{display:block;margin:12px 0 4px;color:#9c9}"
    "select,input{font-size:16px;padding:8px;width:100%;box-sizing:border-box;background:#222;color:#eee;border:1px solid #444;border-radius:5px}"
    "button{margin-top:16px;font-size:16px;padding:10px 18px;background:#2a5;color:#000;border:0;border-radius:6px;cursor:pointer}"
    "button.sec{background:#345;color:#eee}"
    ".card{background:#1a1a1a;border:1px solid #333;border-radius:8px;padding:14px;margin-bottom:14px}"
    ".big{font-size:28px;font-weight:700}"
    ".muted{color:#888}"
    "</style>";

static String head(const char* title) {
    String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>";
    h += title;
    h += "</title>";
    h += STYLE;
    h += "</head><body><header>Weigh Station"
         "<span style='float:right'>"
         "<a href='/'>Inventory</a>"
         "<a href='/spools'>Spools</a>"
         "<a href='/onboard'>Onboard</a>"
         "<a href='/reorder'>Reorder</a>"
         "<a href='/config'>Config</a>"
         "<a href='/calibrate'>Calibrate</a>"
         "<a href='/backup'>Backup</a>"
         "</span></header><main>";
    return h;
}

static const char* FOOT = "</main></body></html>";

// ── Current spool on the scale ────────────────────────────────────────────────
// Returns the local spool id if a tag is presently weighed/registered, else -1.
static int currentSpool() {
    xSemaphoreTake(gStateMutex, portMAX_DELAY);
    DeviceState s = gState;
    xSemaphoreGive(gStateMutex);
    bool live = (s == DeviceState::Present || s == DeviceState::WeighingAndSync ||
                 s == DeviceState::ReconcilingMainSection);
    return (live && gSpoolId > 0) ? gSpoolId : -1;
}

// ── Dashboard: material roll-up + current spool ───────────────────────────────
static void handleRoot(AsyncWebServerRequest* req) {
    String p = head("Inventory");

    if (!gScaleCalibrated)
        p += "<div class='card' style='border-color:#a70'>"
             "<b class='ob'>Scale not calibrated.</b> Weights will be wrong until "
             "you <a href='/calibrate' style='color:#fd6'>calibrate the scale</a>.</div>";

    int cur = currentSpool();
    if (cur > 0) {
        SpoolRecord r;
        if (storeGetSpool((uint32_t)cur, r)) {
            p += "<div class='card'>";
            p += "<div class='muted'>On the scale now</div>";
            p += "<div class='big'><a href='/spool?id=" + String((unsigned)r.spool)
               + "' style='color:inherit;text-decoration:none'>#" + String((unsigned)r.spool)
               + " " + esc(r.material[0] ? r.material : "Unknown") + "</a></div>";
            if (r.needs_ob)
                p += "<p class='ob'>Needs onboarding &mdash; "
                     "<a href='/onboard' style='color:#fd6'>add details</a></p>";
            else
                p += "<p class='muted'>" + esc(r.vendor) + " &middot; "
                   + String(r.remaining_g, 0) + " g remaining</p>";
            p += "</div>";
        }
    }

    p += "<h3>Inventory by material</h3><table>"
         "<tr><th>Material</th><th>Spools</th><th>Remaining</th></tr>";
    size_t n = storeInventoryCount();
    MatInventory m;
    if (n == 0) p += "<tr><td colspan='3' class='muted'>No spools yet</td></tr>";
    for (size_t i = 0; i < n; i++) {
        if (!storeInventoryAt(i, m)) continue;
        p += "<tr><td>" + esc(m.material) + "</td><td>" + String(m.count)
           + "</td><td>" + String(m.remaining_g, 0) + " g</td></tr>";
    }
    p += "</table>";
    p += "<p class='muted'>" + String((unsigned)storeSpoolCount())
       + " spools tracked &middot; " + String((unsigned)storeLogLineCount())
       + " log entries</p>";
    p += FOOT;
    req->send(200, "text/html", p);
}

// ── Spool list ────────────────────────────────────────────────────────────────
static void handleSpools(AsyncWebServerRequest* req) {
    String p = head("Spools");
    p += "<h3>Spools</h3><table>"
         "<tr><th>#</th><th>Material</th><th>Vendor</th><th>Remaining</th><th></th></tr>";
    size_t n = storeSpoolCount();
    SpoolRecord r;
    if (n == 0) p += "<tr><td colspan='5' class='muted'>No spools yet</td></tr>";
    for (size_t i = 0; i < n; i++) {
        if (!storeSpoolAt(i, r)) continue;
        char sw[40];
        snprintf(sw, sizeof(sw), "<span class='sw' style='background:#%02x%02x%02x'></span>",
                 r.rgba[0], r.rgba[1], r.rgba[2]);
        p += "<tr><td><a href='/spool?id=" + String((unsigned)r.spool)
           + "' style='color:#8f8'>#" + String((unsigned)r.spool) + "</a></td><td>" + sw
           + esc(r.material[0] ? r.material : "Unknown") + "</td><td>" + esc(r.vendor)
           + "</td><td>" + String(r.remaining_g, 0) + " g</td><td>"
           + (r.needs_ob ? "<span class='ob'>needs onboarding</span>" : "")
           + "</td></tr>";
    }
    p += "</table>";
    p += FOOT;
    req->send(200, "text/html", p);
}

// ── JSON API: spools ──────────────────────────────────────────────────────────
static void handleApiSpools(AsyncWebServerRequest* req) {
    String j = "[";
    size_t n = storeSpoolCount();
    SpoolRecord r;
    for (size_t i = 0; i < n; i++) {
        if (!storeSpoolAt(i, r)) continue;
        if (j.length() > 1) j += ",";
        char rgb[8];
        snprintf(rgb, sizeof(rgb), "%02x%02x%02x", r.rgba[0], r.rgba[1], r.rgba[2]);
        j += "{\"spool\":" + String((unsigned)r.spool)
           + ",\"uuid\":\"" + r.uuid + "\""
           + ",\"vendor\":\"" + esc(r.vendor) + "\""
           + ",\"material\":\"" + esc(r.material) + "\""
           + ",\"color\":\"" + rgb + "\""
           + ",\"remaining_g\":" + String(r.remaining_g, 1)
           + ",\"used_g\":" + String(r.used_g, 1)
           + ",\"needs_onboarding\":" + (r.needs_ob ? "true" : "false") + "}";
    }
    j += "]";
    req->send(200, "application/json", j);
}

// ── Per-spool weigh history + sparkline ───────────────────────────────────────
struct WeighSeries {
    std::vector<float>  rem, used, gross;
    std::vector<String> ts;
};
static void collectWeigh(const StoreEvent& e, void* ctx) {
    WeighSeries* s = (WeighSeries*)ctx;
    s->rem.push_back(e.remaining_g);
    s->used.push_back(e.used_g);
    s->gross.push_back(e.gross_g);
    s->ts.push_back(String(e.ts));
}

// Remaining-weight-over-sessions sparkline: one series, so no legend — the title
// names it and the table below carries exact values. Thin 2px line, recessive
// baseline, endpoint dot; colour on the mark only, text stays in ink tokens.
static String sparkline(const std::vector<float>& v) {
    if (v.size() < 2)
        return "<p class='muted'>Not enough weigh sessions yet for a trend.</p>";
    const float W = 320, H = 64, PAD = 6;
    float lo = v[0], hi = v[0];
    for (float x : v) { if (x < lo) lo = x; if (x > hi) hi = x; }
    float span = (hi - lo) > 0.001f ? (hi - lo) : 1.0f;
    auto X = [&](size_t i){ return PAD + (W - 2*PAD) * (float)i / (v.size() - 1); };
    auto Y = [&](float val){ return PAD + (H - 2*PAD) * (1.0f - (val - lo) / span); };
    String pts;
    for (size_t i = 0; i < v.size(); i++) {
        if (i) pts += " ";
        pts += String(X(i), 1) + "," + String(Y(v[i]), 1);
    }
    char aria[96];
    snprintf(aria, sizeof(aria),
             "Remaining weight over %u weigh sessions, latest %.0f g",
             (unsigned)v.size(), v.back());
    String s = "<svg viewBox='0 0 320 64' width='320' style='max-width:100%;height:auto' "
               "role='img' aria-label='"; s += aria; s += "'>";
    s += "<line x1='6' y1='58' x2='314' y2='58' stroke='#333' stroke-width='1'/>";
    s += "<polyline fill='none' stroke='#8f8' stroke-width='2' "
         "stroke-linejoin='round' stroke-linecap='round' points='" + pts + "'/>";
    s += "<circle cx='" + String(X(v.size()-1), 1) + "' cy='" + String(Y(v.back()), 1)
       + "' r='3' fill='#cffccf'/>";
    s += "</svg>";
    return s;
}

static void handleSpoolDetail(AsyncWebServerRequest* req) {
    if (!req->hasParam("id")) { req->send(400, "text/plain", "missing id"); return; }
    uint32_t id = (uint32_t)req->getParam("id")->value().toInt();
    SpoolRecord r;
    if (!storeGetSpool(id, r)) { req->send(404, "text/plain", "unknown spool"); return; }

    WeighSeries s;
    storeForEachWeigh(id, collectWeigh, &s);

    if (req->hasParam("format") && req->getParam("format")->value() == "csv") {
        String out = "ts,gross_g,remaining_g,used_g\n";
        for (size_t i = 0; i < s.rem.size(); i++)
            out += s.ts[i] + "," + String(s.gross[i], 1) + ","
                 + String(s.rem[i], 1) + "," + String(s.used[i], 1) + "\n";
        AsyncWebServerResponse* resp = req->beginResponse(200, "text/csv", out);
        resp->addHeader("Content-Disposition",
                        "attachment; filename=spool-" + String(id) + ".csv");
        req->send(resp);
        return;
    }

    String p = head("Spool");
    char rgb[8];
    snprintf(rgb, sizeof(rgb), "%02x%02x%02x", r.rgba[0], r.rgba[1], r.rgba[2]);
    p += "<div class='card'>";
    p += "<div class='big'><span class='sw' style='background:#" + String(rgb)
       + "'></span>#" + String((unsigned)r.spool) + " "
       + esc(r.material[0] ? r.material : "Unknown") + "</div>";
    p += "<p class='muted'>" + esc(r.vendor) + (r.abbr[0] ? " &middot; " + esc(r.abbr) : String())
       + " &middot; " + String((unsigned)s.rem.size()) + " weigh session(s)</p>";
    p += "<p class='big'>" + String(r.remaining_g, 0) + " g <span class='muted' "
         "style='font-size:15px'>remaining &middot; " + String(r.used_g, 0)
       + " g used</span></p>";
    p += "</div>";

    p += "<div class='card'><label style='margin-top:0'>Remaining over time</label>";
    p += sparkline(s.rem);
    p += "</div>";

    p += "<h3>Weigh history <a href='/spool?id=" + String(id)
       + "&format=csv' style='font-size:14px;color:#8f8'>(CSV)</a></h3>";
    p += "<table><tr><th>When (UTC)</th><th>Gross</th><th>Remaining</th><th>Used</th></tr>";
    if (s.rem.empty())
        p += "<tr><td colspan='4' class='muted'>No weigh sessions yet</td></tr>";
    for (size_t k = s.rem.size(); k-- > 0; )   // newest first
        p += "<tr><td>" + s.ts[k] + "</td><td>" + String(s.gross[k], 0)
           + " g</td><td>" + String(s.rem[k], 0) + " g</td><td>"
           + String(s.used[k], 0) + " g</td></tr>";
    p += "</table>";
    p += "<p class='muted'><a href='/spools' style='color:#8f8'>&larr; all spools</a></p>";
    p += FOOT;
    req->send(200, "text/html", p);
}

// ── Onboarding form ───────────────────────────────────────────────────────────
static void handleOnboardForm(AsyncWebServerRequest* req) {
    String p = head("Onboard");
    int cur = currentSpool();

    if (cur < 0) {
        p += "<div class='card'><p>No spool is on the scale.</p>"
             "<p class='muted'>Place a spool, wait for it to register, then reload "
             "this page to fill in its details.</p></div>";
        p += FOOT;
        req->send(200, "text/html", p);
        return;
    }

    SpoolRecord r;
    storeGetSpool((uint32_t)cur, r);

    p += "<h3>Onboard spool #" + String((unsigned)cur) + "</h3>";
    p += "<form method='POST' action='/api/onboard'>";
    p += "<input type='hidden' name='id' value='" + String((unsigned)cur) + "'>";

    // Vendor
    p += "<label>Vendor</label><select name='vendor'>";
    char vbuf[64];
    for (size_t i = 0; i < cfgVendorCount(); i++)
        if (cfgVendorAt(i, vbuf, sizeof(vbuf)))
            p += "<option>" + esc(vbuf) + "</option>";
    p += "</select>";

    // Material
    p += "<label>Material</label><select name='material'>";
    CfgMaterial m;
    for (size_t i = 0; i < cfgMaterialCount(); i++)
        if (cfgMaterialAt(i, m))
            p += "<option>" + esc(m.name) + "</option>";
    p += "</select>";

    // Color
    p += "<label>Color</label><select name='color'>";
    CfgColor c;
    for (size_t i = 0; i < cfgColorCount(); i++)
        if (cfgColorAt(i, c))
            p += "<option>" + esc(c.name) + "</option>";
    p += "</select>";

    // Spool profile (fills nominal-full + empty tare)
    p += "<label>Spool profile</label><select name='profile'>";
    CfgProfile pr;
    for (size_t i = 0; i < cfgProfileCount(); i++)
        if (cfgProfileAt(i, pr))
            p += "<option>" + esc(pr.label) + "</option>";
    p += "</select>";

    // Tare override (optional). "Capture tare" reads the load cell once.
    p += "<label>Empty spool tare (g) &mdash; blank to use profile</label>"
         "<input type='number' step='0.1' name='empty_g' id='tare' placeholder='from profile'>";
    p += "<button type='button' class='sec' onclick=\"fetch('/api/tare',{method:'POST'})"
         ".then(r=>r.json()).then(d=>{document.getElementById('tare').value=d.weight.toFixed(1)})\">"
         "Capture tare from scale</button>";

    p += "<div><button type='submit'>Save &amp; write tag</button></div>";
    p += "</form>";
    p += FOOT;
    req->send(200, "text/html", p);
}

// ── POST /api/tare — one load-cell reading ────────────────────────────────────
static void handleApiTare(AsyncWebServerRequest* req) {
    xSemaphoreTake(gWeightMutex, portMAX_DELAY);
    float w = gWeightGrams;
    xSemaphoreGive(gWeightMutex);
    req->send(200, "application/json", "{\"weight\":" + String(w, 1) + "}");
}

// ── POST /api/onboard — apply the form, write the tag, log a reconcile ────────
static void handleApiOnboard(AsyncWebServerRequest* req) {
    auto arg = [&](const char* k) -> String {
        const AsyncWebParameter* pp = req->getParam(k, true);
        return pp ? pp->value() : String();
    };

    uint32_t id = (uint32_t)arg("id").toInt();
    if (id == 0) { req->send(400, "text/plain", "missing id"); return; }

    SpoolRecord rec;
    if (!storeGetSpool(id, rec)) { req->send(404, "text/plain", "unknown spool"); return; }

    String vendor   = arg("vendor");
    String matName  = arg("material");
    String colName  = arg("color");
    String profName = arg("profile");
    float  tareOvr  = arg("empty_g").toFloat();   // 0 → use profile

    // Resolve config rows (fall back to sensible defaults when a pick is absent).
    CfgMaterial m = {};
    bool haveMat = cfgMaterialByName(matName.c_str(), m);
    CfgColor col = {};
    cfgColorByName(colName.c_str(), col);

    CfgProfile pr = {};
    bool havePr = false;
    for (size_t i = 0; i < cfgProfileCount(); i++)
        if (cfgProfileAt(i, pr) && profName == pr.label) { havePr = true; break; }

    float nominal = havePr ? pr.nominal_full_g : 0.0f;
    float empty   = (tareOvr > 0.0f) ? tareOvr : (havePr ? pr.empty_g : 0.0f);
    float dia     = haveMat ? m.dia : 1.75f;

    // 1) Append the identity to the log so indices + inventory reflect it.
    StoreEvent e; e.ev = StoreEv::Reconcile;
    strlcpy(e.uuid, rec.uuid, sizeof(e.uuid));
    e.spool = id;
    strlcpy(e.vendor,   vendor.c_str(),  sizeof(e.vendor));
    strlcpy(e.material, matName.c_str(), sizeof(e.material));
    strlcpy(e.abbr,     haveMat ? m.abbr : "", sizeof(e.abbr));
    memcpy(e.rgba, col.rgba, 4);
    e.dia = dia; e.empty_g = empty; e.nom_g = nominal;
    e.needs_ob = false;
    storeAppendEvent(e);

    // 2) If this spool is on the scale, write the full OPT Main to its tag now.
    //    (Identity + material temps/class/type + weights.) syncTask's reconcile
    //    loop is idempotent, so a redundant later write is harmless.
    if (currentSpool() == (int)id) {
        xSemaphoreTake(gTagMutex, portMAX_DELAY);
        strlcpy(gTagMain.brand_name,            vendor.c_str(),  sizeof(gTagMain.brand_name));
        strlcpy(gTagMain.material_name,         matName.c_str(), sizeof(gTagMain.material_name));
        strlcpy(gTagMain.material_abbreviation, haveMat ? m.abbr : "", sizeof(gTagMain.material_abbreviation));
        memcpy(gTagMain.primary_color_rgba, col.rgba, 4);
        gTagMain.filament_diameter         = dia;
        gTagMain.nominal_netto_full_weight = nominal;
        gTagMain.actual_netto_full_weight  = nominal;   // new spool assumed full
        gTagMain.empty_container_weight    = empty;
        if (haveMat) {
            gTagMain.min_print_temperature = m.print_min;
            gTagMain.max_print_temperature = m.print_max;
            gTagMain.min_bed_temperature   = m.bed_min;
            gTagMain.max_bed_temperature   = m.bed_max;
            gTagMain.material_class        = m.cls;
            gTagMain.material_type         = m.type;
        }
        xSemaphoreGive(gTagMutex);
        gWriteMainPending     = true;
        gSpoolNeedsOnboarding = false;
    }

    // Optionally remember a free-typed vendor for reuse.
    if (vendor.length()) cfgVendorAdd(vendor.c_str());

    req->redirect("/");
}

// ── Reorder: roll up on-hand per stock item, flag shortfalls ──────────────────
struct OnHand { uint16_t count; float grams; };

// Sum active spools that match a stock item's identity. Matching is by vendor +
// material (exact) — the reliable subset of the design's optional color/diameter
// keys; a one-off spool not in the catalog never generates reorder noise.
static OnHand rollUp(const CfgStock& s) {
    OnHand oh{0, 0.0f};
    size_t n = storeSpoolCount();
    SpoolRecord r;
    for (size_t i = 0; i < n; i++) {
        if (!storeSpoolAt(i, r)) continue;
        if (strcmp(r.vendor, s.vendor) == 0 && strcmp(r.material, s.material) == 0
                && r.remaining_g > 1.0f) {
            oh.count++;
            oh.grams += r.remaining_g;
        }
    }
    return oh;
}

// Below threshold? min_spools takes precedence; else min_grams; else "empty".
static bool belowThreshold(const CfgStock& s, const OnHand& oh) {
    if (s.min_spools > 0) return oh.count < s.min_spools;
    if (s.min_grams  > 0) return oh.grams < s.min_grams;
    return oh.count < 1;
}

static void handleReorder(AsyncWebServerRequest* req) {
    bool csv = req->hasParam("format") &&
               req->getParam("format")->value() == "csv";

    if (csv) {
        String out = "vendor,material,color,diameter,sku,gtin,pack_qty,"
                     "on_hand_spools,on_hand_g,min_spools,min_grams\n";
        CfgStock s;
        for (size_t i = 0; i < cfgStockCount(); i++) {
            if (!cfgStockAt(i, s)) continue;
            OnHand oh = rollUp(s);
            if (!belowThreshold(s, oh)) continue;
            out += String(s.vendor) + "," + s.material + "," + s.color + ","
                 + String(s.dia, 2) + "," + s.sku + "," + s.gtin + ","
                 + String(s.pack_qty) + "," + String(oh.count) + ","
                 + String(oh.grams, 0) + "," + String(s.min_spools) + ","
                 + String(s.min_grams, 0) + "\n";
        }
        AsyncWebServerResponse* resp = req->beginResponse(200, "text/csv", out);
        resp->addHeader("Content-Disposition", "attachment; filename=reorder.csv");
        req->send(resp);
        return;
    }

    String p = head("Reorder");
    p += "<h3>Reorder list</h3>";
    p += "<p class='muted'>Standard-stock items at or below their threshold. "
         "Review, then <a href='/reorder?format=csv' style='color:#8f8'>download CSV</a> "
         "to place the order.</p>";
    p += "<table><tr><th>Vendor</th><th>Material</th><th>Color</th>"
         "<th>On hand</th><th>Threshold</th></tr>";
    CfgStock s;
    int flagged = 0;
    for (size_t i = 0; i < cfgStockCount(); i++) {
        if (!cfgStockAt(i, s)) continue;
        OnHand oh = rollUp(s);
        if (!belowThreshold(s, oh)) continue;
        flagged++;
        String thr = s.min_spools > 0 ? (String(s.min_spools) + " spools")
                   : s.min_grams  > 0 ? (String(s.min_grams, 0) + " g")
                   : String("empty");
        p += "<tr><td>" + esc(s.vendor) + "</td><td>" + esc(s.material) + "</td><td>"
           + esc(s.color) + "</td><td>" + String(oh.count) + " spool(s), "
           + String(oh.grams, 0) + " g</td><td>" + thr + "</td></tr>";
    }
    if (flagged == 0)
        p += "<tr><td colspan='5' class='muted'>Everything is above threshold "
             "(or no stock items configured)</td></tr>";
    p += "</table>";
    p += FOOT;
    req->send(200, "text/html", p);
}

// ── Config catalog editor (raw-JSON round-trip per table) ─────────────────────
static void configTableForm(String& p, const char* label, const char* which) {
    p += "<div class='card'><label style='margin-top:0'>" + String(label) + "</label>";
    p += "<form method='POST' action='/api/config'>";
    p += "<input type='hidden' name='which' value='" + String(which) + "'>";
    p += "<textarea name='json' rows='6' style='width:100%;font-family:monospace;"
         "font-size:13px;background:#222;color:#8f8;border:1px solid #444;border-radius:5px'>";
    p += esc(cfgTableJson(which).c_str());
    p += "</textarea>";
    p += "<div><button type='submit'>Save " + String(label) + "</button></div>";
    p += "</form></div>";
}

static void handleConfig(AsyncWebServerRequest* req) {
    String p = head("Config");
    p += "<h3>Config catalog</h3>";
    p += "<p class='muted'>Edit the reference tables that drive onboarding and "
         "reordering. Each is a JSON array; Save validates and persists it.</p>";
    configTableForm(p, "Vendors",        "vendors");
    configTableForm(p, "Materials",      "materials");
    configTableForm(p, "Spool profiles", "spool-profiles");
    configTableForm(p, "Colors",         "colors");
    configTableForm(p, "Stock items",    "stock-items");
    p += FOOT;
    req->send(200, "text/html", p);
}

static void handleApiConfigSave(AsyncWebServerRequest* req) {
    const AsyncWebParameter* pw = req->getParam("which", true);
    const AsyncWebParameter* pj = req->getParam("json", true);
    if (!pw || !pj) { req->send(400, "text/plain", "missing which/json"); return; }
    String which = pw->value();
    if (!cfgReplaceTable(which.c_str(), pj->value())) {
        req->send(400, "text/plain", "invalid JSON for " + which);
        return;
    }
    cfgSave(which.c_str());
    req->redirect("/config");
}

// ── Backup / restore (host download + upload; SD snapshots are hardware-gated) ─
static const char* IMPORT_STAGING = "/log/import.staging";

static void handleBackupPage(AsyncWebServerRequest* req) {
    String p = head("Backup");
    p += "<h3>Backup &amp; restore</h3>";
    p += "<div class='card'><label style='margin-top:0'>Download</label>"
         "<p class='muted'>The event log is the source of truth &mdash; download it "
         "to your machine as an off-device backup.</p>"
         "<a href='/export'><button type='button'>Download event log</button></a></div>";
    p += "<div class='card'><label style='margin-top:0'>Restore</label>"
         "<p class='muted'>Upload a previously downloaded log to replace the current "
         "one. The upload is validated first; a bad file leaves your data untouched.</p>"
         "<form method='POST' action='/import' enctype='multipart/form-data'>"
         "<input type='file' name='file' accept='.ndjson,text/plain'>"
         "<div><button type='submit'>Upload &amp; restore</button></div>"
         "</form></div>";
    p += "<p class='muted'>Config tables back up separately via the "
         "<a href='/config' style='color:#8f8'>Config</a> page (copy the JSON). "
         "SD-card snapshots arrive once the card is wired.</p>";
    p += FOOT;
    req->send(200, "text/html", p);
}

static void handleExport(AsyncWebServerRequest* req) {
    if (!LittleFS.exists(storeLogPath())) {
        req->send(200, "application/x-ndjson", "");   // nothing logged yet
        return;
    }
    AsyncWebServerResponse* r =
        req->beginResponse(LittleFS, storeLogPath(), "application/x-ndjson", false);
    r->addHeader("Content-Disposition", "attachment; filename=weighstation-log.ndjson");
    req->send(r);
}

// Streams the multipart upload to a staging file, chunk by chunk.
static void handleImportUpload(AsyncWebServerRequest* req, const String& filename,
                               size_t index, uint8_t* data, size_t len, bool final) {
    File f = LittleFS.open(IMPORT_STAGING, index == 0 ? "w" : "a");
    if (f) { f.write(data, len); f.close(); }
}

// Called after the upload completes: validate + promote, or reject.
static void handleImportDone(AsyncWebServerRequest* req) {
    bool ok = storeImportLogFile(IMPORT_STAGING);
    LittleFS.remove(IMPORT_STAGING);   // no-op if promotion already consumed it
    if (ok) {
        String p = head("Restore");
        p += "<div class='card'><p>Restore complete &mdash; "
             + String((unsigned)storeSpoolCount()) + " spools, "
             + String((unsigned)storeLogLineCount()) + " log entries.</p>"
             "<a href='/'><button type='button'>Back to inventory</button></a></div>";
        p += FOOT;
        req->send(200, "text/html", p);
    } else {
        req->send(400, "text/plain", "Import failed: no valid records in the uploaded file");
    }
}

// ── Scale calibration (web-driven; scaleTask does the actual NAU7802 work) ────
static void handleApiScale(AsyncWebServerRequest* req) {
    xSemaphoreTake(gWeightMutex, portMAX_DELAY);
    float w = gWeightGrams;
    xSemaphoreGive(gWeightMutex);
    req->send(200, "application/json",
        "{\"weight\":" + String(w, 1) +
        ",\"calibrated\":" + (gScaleCalibrated ? "true" : "false") + "}");
}

static void handleApiCalZero(AsyncWebServerRequest* req) {
    gCalZeroReq = true;                       // scaleTask tares on its next loop
    req->send(200, "application/json", "{\"ok\":true}");
}

static void handleApiCal(AsyncWebServerRequest* req) {
    const AsyncWebParameter* pp = req->getParam("grams", true);
    float g = pp ? pp->value().toFloat() : 0.0f;
    if (g <= 0.0f) { req->send(400, "text/plain", "need grams>0"); return; }
    gCalSetGrams = g;                         // scaleTask calibrates on its next loop
    req->send(200, "application/json", "{\"ok\":true}");
}

static void handleCalibratePage(AsyncWebServerRequest* req) {
    String p = head("Calibrate");
    p += "<h3>Scale calibration</h3>";
    p += "<div class='card'><div class='muted'>Live weight</div>"
         "<div class='big' id='w'>&mdash;</div>"
         "<div class='muted' id='st'></div></div>";
    p += "<div class='card'><label style='margin-top:0'>Step 1 &mdash; Zero</label>"
         "<p class='muted'>Remove everything from the scale, then zero it.</p>"
         "<button type='button' onclick='z()'>Zero (tare)</button></div>";
    p += "<div class='card'><label style='margin-top:0'>Step 2 &mdash; Calibrate</label>"
         "<p class='muted'>Place a known weight on the scale and enter its exact "
         "mass in grams.</p>"
         "<input type='number' step='0.1' id='g' placeholder='grams'>"
         "<button type='button' onclick='c()'>Set calibration</button></div>";
    p += "<script>"
         "function r(){fetch('/api/scale').then(x=>x.json()).then(d=>{"
         "document.getElementById('w').textContent=d.weight.toFixed(1)+' g';"
         "document.getElementById('st').textContent="
         "d.calibrated?'Calibrated':'Not calibrated yet';});}"
         "function z(){fetch('/api/cal-zero',{method:'POST'}).then(()=>setTimeout(r,700));}"
         "function c(){var g=document.getElementById('g').value;"
         "fetch('/api/cal',{method:'POST',"
         "headers:{'Content-Type':'application/x-www-form-urlencoded'},"
         "body:'grams='+encodeURIComponent(g)}).then(()=>setTimeout(r,700));}"
         "setInterval(r,1000);r();"
         "</script>";
    p += FOOT;
    req->send(200, "text/html", p);
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void webAppBegin() {
    if (MDNS.begin(DEVICE_HOSTNAME))
        MDNS.addService("http", "tcp", 80);

    sServer.on("/",            HTTP_GET,  handleRoot);
    sServer.on("/spools",      HTTP_GET,  handleSpools);
    sServer.on("/spool",       HTTP_GET,  handleSpoolDetail);
    sServer.on("/api/spools",  HTTP_GET,  handleApiSpools);
    sServer.on("/onboard",     HTTP_GET,  handleOnboardForm);
    sServer.on("/api/tare",    HTTP_POST, handleApiTare);
    sServer.on("/api/onboard", HTTP_POST, handleApiOnboard);
    sServer.on("/reorder",     HTTP_GET,  handleReorder);
    sServer.on("/config",      HTTP_GET,  handleConfig);
    sServer.on("/api/config",  HTTP_POST, handleApiConfigSave);
    sServer.on("/calibrate",   HTTP_GET,  handleCalibratePage);
    sServer.on("/api/scale",   HTTP_GET,  handleApiScale);
    sServer.on("/api/cal-zero",HTTP_POST, handleApiCalZero);
    sServer.on("/api/cal",     HTTP_POST, handleApiCal);
    sServer.on("/backup",      HTTP_GET,  handleBackupPage);
    sServer.on("/export",      HTTP_GET,  handleExport);
    sServer.on("/import",      HTTP_POST, handleImportDone, handleImportUpload);

    // Re-provisioning for a cabinet-installed unit with no physical access:
    // clear stored WiFi credentials and reboot into the captive portal.
    sServer.on("/reset", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html",
            "<!DOCTYPE html><body><h2>Resetting WiFi&hellip;</h2>"
            "<p>Credentials cleared; the device is restarting.</p>"
            "<p>Connect to <b>WeighStation-Setup</b> to reconfigure.</p></body>");
        // Erase stored WiFi credentials (equivalent to WiFiManager::resetSettings)
        // without pulling in WebServer.h, whose HTTP_* enum clashes with the async
        // server's. syncTask's autoConnect reopens the portal on the next boot.
        WiFi.disconnect(true, true);   // wifioff=true, eraseap=true
        delay(400);                    // let the response flush before reboot
        ESP.restart();
    });

    sServer.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "Not found");
    });

    sServer.begin();
    Serial.printf("Web app: http://%s.local/\n", DEVICE_HOSTNAME);
}
