#include "web_app.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
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

    int cur = currentSpool();
    if (cur > 0) {
        SpoolRecord r;
        if (storeGetSpool((uint32_t)cur, r)) {
            p += "<div class='card'>";
            p += "<div class='muted'>On the scale now</div>";
            p += "<div class='big'>#" + String((unsigned)r.spool) + " "
               + esc(r.material[0] ? r.material : "Unknown") + "</div>";
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
        p += "<tr><td>#" + String((unsigned)r.spool) + "</td><td>" + sw
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

// ── Setup ─────────────────────────────────────────────────────────────────────
void webAppBegin() {
    if (MDNS.begin(DEVICE_HOSTNAME))
        MDNS.addService("http", "tcp", 80);

    sServer.on("/",            HTTP_GET,  handleRoot);
    sServer.on("/spools",      HTTP_GET,  handleSpools);
    sServer.on("/api/spools",  HTTP_GET,  handleApiSpools);
    sServer.on("/onboard",     HTTP_GET,  handleOnboardForm);
    sServer.on("/api/tare",    HTTP_POST, handleApiTare);
    sServer.on("/api/onboard", HTTP_POST, handleApiOnboard);

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
