#include "web_app.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <vector>
#include <algorithm>
#include <string.h>
#include <stdlib.h>   // strtoull
#include <ctype.h>    // isalnum, for urlEnc
#include "config.h"
#include "device_state.h"
#include "opt_tag.h"
#include "store.h"
#include "config_store.h"
#include "api_key.h"

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
// SoftAP SSID when the station fell back to its own AP; empty in station
// mode. Reported by /api/status.
extern char                 gApSsid[24];

static AsyncWebServer sServer(80);

// ── Small HTML helpers ────────────────────────────────────────────────────────

// ── Auth ──────────────────────────────────────────────────────────────────────
// Applied to state-changing endpoints only; GETs stay open so dashboards and
// scrapers need no credentials. See api_key.h for the threat model and why an
// unset key deliberately leaves everything open.
//
// Returns true if the request may proceed. On refusal it has already sent the
// 401, so the caller must simply return.
static bool authOk(AsyncWebServerRequest* req) {
    if (!apiKeyIsSet()) return true;                 // not configured: open
    const String key = apiKeyGet();
    if (req->hasHeader("X-API-Key") && req->header("X-API-Key") == key) return true;
    if (req->hasParam("key") && req->getParam("key")->value() == key)   return true;
    // Basic auth accepts any username: the secret is the password. Lets browsers
    // prompt natively and `curl -u :secret` work without a header.
    if (req->authenticate("", key.c_str()) ||
        req->authenticate("admin", key.c_str()))     return true;
    req->requestAuthentication();                    // sends 401 + WWW-Authenticate
    return false;
}

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
    // -webkit-text-size-adjust stops iOS inflating text when a phone is turned
    // to landscape, which otherwise reflows every page unpredictably.
    "body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee;"
    "-webkit-text-size-adjust:100%}"
    // Flex, not float. Eight nav links floated right do not fit a phone: they
    // collide with the title and wrap into an unreadable stack. Flex-wrap drops
    // them onto their own line instead, and margin-left:auto keeps them right-
    // aligned whenever there IS room.
    "header{background:#1c2b1c;padding:12px 16px;font-size:20px;font-weight:600;"
    "display:flex;flex-wrap:wrap;align-items:baseline}"
    "header nav{display:flex;flex-wrap:wrap;gap:4px 16px;margin-left:auto}"
    "header a{color:#8f8;text-decoration:none;font-size:15px;font-weight:400;"
    "padding:4px 0}"
    "header a.active{color:#fff;font-weight:600;border-bottom:2px solid #8f8}"
    // overflow-x on main, not on the page: a table wider than the phone scrolls
    // inside the content area instead of sliding the whole layout sideways,
    // which is the usual way a page "looks broken" on mobile.
    "main{padding:18px;max-width:760px;margin:0 auto;overflow-x:auto}"
    "table{border-collapse:collapse;width:100%}"
    "th,td{padding:6px 10px;text-align:left;border-bottom:1px solid #333}"
    "th{color:#9c9}"
    ".sw{display:inline-block;width:14px;height:14px;border-radius:3px;vertical-align:middle;margin-right:6px;border:1px solid #000}"
    // "No colour assigned" — a crossed-out square, so it cannot be misread as a
    // black filament. The stroke is a gradient rather than a glyph or an SVG so
    // it stays crisp at 14px and needs no extra request.
    ".sw0{background:#1c1c1c;border-color:#666;background-image:"
    "linear-gradient(to top right,transparent 45%,#8a8a8a 45%,#8a8a8a 55%,transparent 55%)}"
    ".ob{color:#fd6}"
    "form label{display:block;margin:12px 0 4px;color:#9c9}"
    "select,input{font-size:16px;padding:8px;width:100%;box-sizing:border-box;background:#222;color:#eee;border:1px solid #444;border-radius:5px}"
    "button{margin-top:16px;font-size:16px;padding:10px 18px;background:#2a5;color:#000;border:0;border-radius:6px;cursor:pointer}"
    "button.sec{background:#345;color:#eee}"
    ".card{background:#1a1a1a;border:1px solid #333;border-radius:8px;padding:14px;margin-bottom:14px}"
    ".big{font-size:28px;font-weight:700}"
    ".muted{color:#888}"
    // Spec strip: the numbers onboarding stored, which are otherwise typed once
    // into the form and never seen again. Flex-wrap rather than a table so it
    // reflows to one item per line on a phone instead of scrolling.
    ".spec{display:flex;flex-wrap:wrap;gap:5px 16px;font-size:14px;color:#aaa;margin-top:10px}"
    ".spec b{color:#ddd;font-weight:600}"
    // Phone-width tuning. Tighter cells and slightly smaller type let the wider
    // tables (Usage has five columns) fit without scrolling at all on most
    // handsets; the ones that still don't fit scroll inside main.
    "@media(max-width:520px){"
      "main{padding:12px}"
      "header{font-size:18px;padding:10px 12px}"
      "th,td{padding:5px 6px;font-size:14px}"
      ".big{font-size:24px}"
      "button{width:100%;padding:12px 18px}"
      "button.sec{width:auto}"
    "}"
    "</style>";

// One nav link, marked `active` when its href matches the current page.
static void navlink(String& h, const char* href, const char* label, const char* active) {
    h += "<a href='";
    h += href;
    h += (strcmp(href, active) == 0) ? "' class='active'>" : "'>";
    h += label;
    h += "</a>";
}

// `active` = the nav href for the current page (e.g. "/spools"); "" = none.
static String head(const char* title, const char* active = "") {
    String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>";
    h += title;
    h += "</title>";
    h += STYLE;
    h += "</head><body><header>Weigh Station<nav>";
    navlink(h, "/",          "Inventory", active);
    navlink(h, "/spools",    "Spools",    active);
    navlink(h, "/products",  "Products",  active);
    navlink(h, "/onboard",   "Onboard",   active);
    navlink(h, "/usage",     "Usage",     active);
    navlink(h, "/reorder",   "Reorder",   active);
    navlink(h, "/config",    "Config",    active);
    navlink(h, "/calibrate", "Calibrate", active);
    navlink(h, "/backup",    "Backup",    active);
    h += "</nav></header><main>";
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

// Colour swatch, with a distinct "not assigned" rendering.
//
// alpha == 0 means no colour has been entered (see StoreEvent::rgba). Drawing
// that as #000000 would claim the spool is black, which is both wrong and
// unfalsifiable — nobody looking at the page could tell the difference. A
// crossed-out square says "unknown", and stays sayable if the colour is
// measured and filled in later.
static String swatch(const uint8_t rgba[4]) {
    if (rgba[3] == 0)
        return "<span class='sw sw0' title='No colour assigned'></span>";
    char b[80];
    snprintf(b, sizeof(b),
             "<span class='sw' style='background:#%02x%02x%02x'></span>",
             rgba[0], rgba[1], rgba[2]);
    return String(b);
}

// JSON value for a colour: the hex string, or null when none is assigned.
// Emitting "000000" would make an unassigned colour indistinguishable from
// black to every consumer, which is the same mistake the swatch avoids.
static String colorJson(const uint8_t rgba[4]) {
    if (rgba[3] == 0) return "null";
    char b[10];
    snprintf(b, sizeof(b), "\"%02x%02x%02x\"", rgba[0], rgba[1], rgba[2]);
    return String(b);
}

// Percent-encode for a URL query component (the mailto: subject and body).
//
// RFC 3986 unreserved set only. Everything else is escaped, including space —
// as %20, not "+", because "+" only means space in form-encoded bodies and a
// mail client shows it literally. Newlines matter most here: the reorder body
// is nothing but newlines, and a raw one truncates the whole URL.
static String urlEnc(const String& s) {
    static const char* HEX = "0123456789ABCDEF";
    String o;
    o.reserve(s.length() + 16);
    for (size_t i = 0; i < s.length(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += HEX[c >> 4]; o += HEX[c & 0x0F]; }
    }
    return o;
}

// One "label value" item for a .spec strip.
static String kv(const char* label, const String& value) {
    return "<span><b>" + String(label) + "</b> " + value + "</span>";
}

// The physical numbers a spool record carries: the tare, nominal weight and
// diameter chosen during onboarding.
//
// These exist only because someone typed them into the Onboard form, and until
// now nothing displayed them back — so a wrong spool profile (the commonest
// onboarding mistake, and the one that makes every later remaining-weight
// wrong) was invisible. Omit anything still zero rather than printing "0 g",
// which reads like a measurement instead of a blank.
static String specStrip(const SpoolRecord& r) {
    String s;
    if (r.empty_g > 0.0f) s += kv("Tare",    String(r.empty_g, 0) + " g");
    if (r.nom_g   > 0.0f) s += kv("Nominal", String(r.nom_g,   0) + " g");
    if (r.dia     > 0.0f) s += kv("&Oslash;",  String(r.dia,   2) + " mm");
    return s;
}

// ── Dashboard: material roll-up + current spool ───────────────────────────────
static void handleRoot(AsyncWebServerRequest* req) {
    String p = head("Inventory", "/");

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
               + "' style='color:inherit;text-decoration:none'>"
               + swatch(r.rgba) + "#" + String((unsigned)r.spool)
               + " " + esc(r.material[0] ? r.material : "Unknown") + "</a></div>";
            if (r.needs_ob) {
                p += "<p class='ob'>Needs onboarding &mdash; "
                     "<a href='/onboard' style='color:#fd6'>add details</a></p>";
            } else {
                p += "<p class='muted'>" + esc(r.vendor)
                   + (r.abbr[0] ? " &middot; " + esc(r.abbr) : String())
                   + " &middot; " + String(r.remaining_g, 0) + " g remaining &middot; "
                   + String(r.used_g, 0) + " g used</p>";

                // Spec strip, plus the print temperatures. The temps are not in
                // the store record at all — they live on the tag, so read them
                // from the decoded Main section of whatever is on the scale.
                // This card is the only place they can be shown, and they are
                // what a member actually wants when setting up a print.
                String spec = specStrip(r);
                OptMain t;
                xSemaphoreTake(gTagMutex, portMAX_DELAY);
                t = gTagMain;
                xSemaphoreGive(gTagMutex);
                if (t.max_print_temperature > 0)
                    spec += kv("Nozzle", String(t.min_print_temperature) + "&ndash;"
                                       + String(t.max_print_temperature) + " &deg;C");
                if (t.max_bed_temperature > 0)
                    spec += kv("Bed", String(t.min_bed_temperature) + "&ndash;"
                                    + String(t.max_bed_temperature) + " &deg;C");
                if (spec.length()) p += "<div class='spec'>" + spec + "</div>";
            }
            p += "</div>";
        }
    }

    // Rows are OPT display strings now ("PLA Summer Grass"), not bare material
    // types, so several spools of the same product roll up into one line.
    p += "<h3>Inventory</h3><table>"
         "<tr><th>Filament</th><th>Spools</th><th>Remaining</th></tr>";
    size_t n = storeInventoryCount();
    MatInventory m;
    if (n == 0) p += "<tr><td colspan='3' class='muted'>No spools yet</td></tr>";
    for (size_t i = 0; i < n; i++) {
        if (!storeInventoryAt(i, m)) continue;
        p += "<tr><td>" + swatch(m.rgba) + esc(m.material) + "</td><td>" + String(m.count)
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
    String p = head("Spools", "/spools");
    p += "<h3>Spools</h3><table>"
         "<tr><th>#</th><th>Filament</th><th>Vendor</th><th>Remaining</th><th></th></tr>";
    size_t n = storeSpoolCount();
    SpoolRecord r;
    if (n == 0) p += "<tr><td colspan='5' class='muted'>No spools yet</td></tr>";
    for (size_t i = 0; i < n; i++) {
        if (!storeSpoolAt(i, r)) continue;
        p += "<tr><td><a href='/spool?id=" + String((unsigned)r.spool)
           + "' style='color:#8f8'>#" + String((unsigned)r.spool) + "</a></td><td>" + swatch(r.rgba)
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
        j += "{\"spool\":" + String((unsigned)r.spool)
           + ",\"uuid\":\"" + r.uuid + "\""
           + ",\"vendor\":\"" + esc(r.vendor) + "\""
           + ",\"material\":\"" + esc(r.material) + "\""
           + ",\"color\":" + colorJson(r.rgba)
           + ",\"remaining_g\":" + String(r.remaining_g, 1)
           + ",\"used_g\":" + String(r.used_g, 1)
           // null, not 0: a spool predating products has no product, which is
           // not the same as belonging to product zero.
           + ",\"product\":" + (r.product ? String((unsigned)r.product) : String("null"))
           + ",\"needs_onboarding\":" + (r.needs_ob ? "true" : "false") + "}";
    }
    j += "]";
    req->send(200, "application/json", j);
}

// ── Products ──────────────────────────────────────────────────────────────────
// The page that answers "is adoption converging?". If the same filament appears
// twice here, the matching ladder missed — and nothing else in the app would
// show that, because two products with the same name roll up into one
// inventory row and look correct.
static void handleProducts(AsyncWebServerRequest* req) {
    String p = head("Products", "/products");
    p += "<h3>Products</h3>"
         "<p class='muted'>What we stock, as opposed to the individual spools on "
         "the shelf. A spool inherits its vendor, filament, colour, tare and "
         "nominal weight from its product.</p>";

    // Snapshot the products once. Matching each spool to its product is
    // inherently O(spools x products); doing it against a local copy keeps the
    // store lock out of the inner loop rather than taking it thousands of times.
    std::vector<uint32_t> ids;
    {
        ProductRecord q;
        for (size_t i = 0; i < storeProductCount(); i++)
            if (storeProductAt(i, q)) ids.push_back(q.id);
    }
    const size_t np = ids.size();
    std::vector<uint16_t> spoolsPer(np, 0);
    std::vector<float>    remPer(np, 0.0f);
    uint16_t unassigned = 0;
    {
        SpoolRecord r;
        for (size_t i = 0; i < storeSpoolCount(); i++) {
            if (!storeSpoolAt(i, r)) continue;
            if (!r.product) { unassigned++; continue; }
            for (size_t k = 0; k < np; k++)
                if (ids[k] == r.product) {
                    spoolsPer[k]++; remPer[k] += r.remaining_g; break;
                }
        }
    }

    p += "<table><tr><th>#</th><th>Filament</th><th>Vendor</th><th>Spools</th>"
         "<th>Remaining</th><th>Tare</th><th>Nominal</th><th></th></tr>";
    if (np == 0)
        p += "<tr><td colspan='8' class='muted'>No products yet — onboard a spool "
             "or place a tagged one on the scale</td></tr>";
    // By id, not by index: a spool placed on the scale mid-request can append a
    // product, and re-indexing would then attribute the counts to the wrong row.
    ProductRecord q;
    for (size_t i = 0; i < np; i++) {
        if (!storeGetProduct(ids[i], q)) continue;
        p += "<tr><td><a href='/product?id=" + String((unsigned)q.id)
           + "' style='color:#8f8'>#" + String((unsigned)q.id) + "</a></td><td>"
           + swatch(q.rgba)
           + esc(q.material[0] ? q.material : "Unknown")
           + (q.abbr[0] ? " <span class='muted'>" + esc(q.abbr) + "</span>" : String())
           + "</td><td>" + esc(q.vendor)
           + "</td><td>" + String((unsigned)spoolsPer[i])
           + "</td><td>" + String(remPer[i], 0) + " g"
           + "</td><td>" + (q.empty_g > 0 ? String(q.empty_g, 0) + " g" : String("&mdash;"))
           + "</td><td>" + (q.nom_g   > 0 ? String(q.nom_g,   0) + " g" : String("&mdash;"))
           + "</td><td>"
           // Provisional means this was inferred from a tag and no human has
           // confirmed it, which is why it is excluded from tag write-back.
           + (q.provisional ? "<span class='ob'>provisional</span>" : "")
           + "</td></tr>";
    }
    p += "</table>";
    if (unassigned)
        p += "<p class='muted'>" + String((unsigned)unassigned)
           + " spool(s) predate products and have none assigned. They keep working; "
             "re-onboarding one assigns it.</p>";
    p += FOOT;
    req->send(200, "text/html", p);
}

// ── One product: edit, and propagate the edit to its spools ───────────────────
//
// This is the page the whole model exists for: correct a wrong tare or a
// misspelled name once, and every spool of that filament follows — including
// their physical tags, on next placement.
static void handleProductDetail(AsyncWebServerRequest* req) {
    if (!req->hasParam("id")) { req->send(400, "text/plain", "missing id"); return; }
    uint32_t id = (uint32_t)req->getParam("id")->value().toInt();
    ProductRecord q;
    if (!storeGetProduct(id, q)) { req->send(404, "text/plain", "unknown product"); return; }

    // Which spools this edit would reach. Saying so up front is the point: an
    // edit here is not local, and the page should not pretend it is.
    std::vector<uint32_t> mine;
    {
        SpoolRecord r;
        for (size_t i = 0; i < storeSpoolCount(); i++)
            if (storeSpoolAt(i, r) && r.product == id) mine.push_back(r.spool);
    }

    String p = head("Product", "/products");
    p += "<div class='card'><div class='big'>" + swatch(q.rgba) + "#"
       + String((unsigned)id) + " " + esc(q.material[0] ? q.material : "Unknown")
       + "</div><p class='muted'>" + esc(q.vendor)
       + (q.abbr[0] ? " &middot; " + esc(q.abbr) : String()) + "</p>";
    if (q.provisional)
        p += "<p class='ob'>Provisional &mdash; this was read off a tag and nobody "
             "has confirmed it. Provisional products are never written back to "
             "tags. Saving this form confirms it.</p>";
    String ident;
    if (q.pkg_uuid[0])   ident += kv("package_uuid",  esc(q.pkg_uuid));
    if (q.mat_uuid[0])   ident += kv("material_uuid", esc(q.mat_uuid));
    if (q.brand_uuid[0]) ident += kv("brand_uuid",    esc(q.brand_uuid));
    if (q.gtin) { char g[24]; snprintf(g, sizeof(g), "%llu", (unsigned long long)q.gtin);
                  ident += kv("GTIN", String(g)); }
    if (q.has_lab) ident += kv("L*a*b*", String(q.lab[0], 2) + " / " + String(q.lab[1], 2)
                                       + " / " + String(q.lab[2], 2));
    if (ident.length()) p += "<div class='spec'>" + ident + "</div>";
    p += "</div>";

    char hex[8];
    snprintf(hex, sizeof(hex), "#%02x%02x%02x", q.rgba[0], q.rgba[1], q.rgba[2]);

    p += "<form method='POST' action='/api/product'>";
    p += "<input type='hidden' name='id' value='" + String((unsigned)id) + "'>";
    p += "<label>Vendor</label><input name='vendor' value='" + esc(q.vendor) + "'>";
    p += "<label>Filament (as shown on tags and in the app)</label>"
         "<input name='material' value='" + esc(q.material) + "'>";
    p += "<label>Type abbreviation &mdash; what usage totals group by</label>"
         "<input name='abbr' maxlength='7' value='" + esc(q.abbr) + "'>";
    p += "<label>Colour</label><input type='color' name='color' value='"
       + String(hex) + "'>";
    p += "<label><input type='checkbox' name='nocolor' style='width:auto' "
       + String(q.rgba[3] == 0 ? "checked" : "")
       + "> No colour assigned</label>";
    p += "<label>Diameter (mm)</label><input type='number' step='0.01' name='dia' value='"
       + String(q.dia, 2) + "'>";
    p += "<label>Empty spool tare (g)</label><input type='number' step='0.1' name='empty_g' value='"
       + String(q.empty_g, 1) + "'>";
    p += "<label>Nominal full weight (g)</label><input type='number' step='0.1' name='nom_g' value='"
       + String(q.nom_g, 1) + "'>";

    p += "<div class='card' style='margin-top:14px'>";
    if (mine.empty()) {
        p += "<p class='muted'>No spools reference this product yet, so saving "
             "changes nothing else.</p>";
    } else {
        p += "<p>Saving updates <b>" + String((unsigned)mine.size())
           + " spool(s)</b>: ";
        for (size_t i = 0; i < mine.size(); i++)
            p += (i ? ", " : "") + String("<a href='/spool?id=")
               + String((unsigned)mine[i]) + "' style='color:#8f8'>#"
               + String((unsigned)mine[i]) + "</a>";
        p += ".</p><p class='muted'>Their tags are rewritten the next time each "
             "spool is placed on the scale. A tag marked write-protected by its "
             "vendor is skipped &mdash; Main is not ours to overwrite.</p>";
    }
    p += "</div>";
    p += "<div><button type='submit'>Save &amp; update spools</button></div>";
    p += "</form>";
    p += "<p class='muted'><a href='/products' style='color:#8f8'>&larr; all products</a></p>";
    p += FOOT;
    req->send(200, "text/html", p);
}

static void handleApiProduct(AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    auto arg = [&](const char* k) -> String {
        const AsyncWebParameter* pp = req->getParam(k, true);
        return pp ? pp->value() : String();
    };
    uint32_t id = (uint32_t)arg("id").toInt();
    ProductRecord q;
    if (!id || !storeGetProduct(id, q)) { req->send(404, "text/plain", "unknown product"); return; }

    strlcpy(q.vendor,   arg("vendor").c_str(),   sizeof(q.vendor));
    strlcpy(q.material, arg("material").c_str(), sizeof(q.material));
    strlcpy(q.abbr,     arg("abbr").c_str(),     sizeof(q.abbr));
    // An unchecked checkbox is simply absent from the POST, which is how
    // "no colour" is distinguished from a colour that happens to be black.
    if (arg("nocolor").length()) {
        memset(q.rgba, 0, 4);            // alpha 0 = unassigned
    } else {
        const String c = arg("color");   // "#rrggbb" from <input type=color>
        long v = strtol(c.c_str() + (c.startsWith("#") ? 1 : 0), nullptr, 16);
        q.rgba[0] = (uint8_t)(v >> 16); q.rgba[1] = (uint8_t)(v >> 8);
        q.rgba[2] = (uint8_t)v;          q.rgba[3] = 255;
    }
    q.dia     = arg("dia").toFloat();
    q.empty_g = arg("empty_g").toFloat();
    q.nom_g   = arg("nom_g").toFloat();
    // A human just reviewed and saved these values, which is exactly what
    // "provisional" was waiting for. Confirming it is what re-admits the
    // product to tag write-back.
    q.provisional = false;

    if (!storeUpsertProduct(q)) { req->send(500, "text/plain", "write failed"); return; }
    // Separate call, deliberately: without it every spool of this product keeps
    // a stale cached copy and a stale tag, and the edit would be invisible
    // everywhere except this page.
    storePropagateProduct(id);

    req->redirect("/product?id=" + String((unsigned)id));
}

static void handleApiProducts(AsyncWebServerRequest* req) {
    String j = "[";
    size_t n = storeProductCount();
    ProductRecord q;
    for (size_t i = 0; i < n; i++) {
        if (!storeProductAt(i, q)) continue;
        if (j.length() > 1) j += ",";
        j += "{\"id\":" + String((unsigned)q.id)
           + ",\"vendor\":\"" + esc(q.vendor) + "\""
           + ",\"material\":\"" + esc(q.material) + "\""
           + ",\"abbr\":\"" + esc(q.abbr) + "\""
           + ",\"color\":" + colorJson(q.rgba)
           + ",\"diameter_mm\":" + String(q.dia, 2)
           + ",\"empty_g\":" + String(q.empty_g, 1)
           + ",\"nominal_g\":" + String(q.nom_g, 1)
           + ",\"provisional\":" + (q.provisional ? "true" : "false");
        // Identity keys are emitted only when the tag actually carried them —
        // "" would read as "we know it is empty" rather than "absent".
        if (q.pkg_uuid[0])   j += ",\"package_uuid\":\""  + String(q.pkg_uuid)   + "\"";
        if (q.mat_uuid[0])   j += ",\"material_uuid\":\"" + String(q.mat_uuid)   + "\"";
        if (q.brand_uuid[0]) j += ",\"brand_uuid\":\""    + String(q.brand_uuid) + "\"";
        // A string, not a number: a GTIN is an identifier, and JSON numbers go
        // through a double in most consumers. snprintf rather than String(),
        // whose unsigned-long-long overload is not on every core.
        if (q.gtin) {
            char g[24];
            snprintf(g, sizeof(g), "%llu", (unsigned long long)q.gtin);
            j += ",\"gtin\":\""; j += g; j += "\"";
        }
        if (q.has_lab)
            j += ",\"lab\":[" + String(q.lab[0], 2) + "," + String(q.lab[1], 2)
               + "," + String(q.lab[2], 2) + "]";
        j += "}";
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

    String p = head("Spool", "/spools");
    p += "<div class='card'>";
    p += "<div class='big'>" + swatch(r.rgba)
       + "#" + String((unsigned)r.spool) + " "
       + esc(r.material[0] ? r.material : "Unknown") + "</div>";
    p += "<p class='muted'>" + esc(r.vendor) + (r.abbr[0] ? " &middot; " + esc(r.abbr) : String())
       + " &middot; " + String((unsigned)s.rem.size()) + " weigh session(s)</p>";
    p += "<p class='big'>" + String(r.remaining_g, 0) + " g <span class='muted' "
         "style='font-size:15px'>remaining &middot; " + String(r.used_g, 0)
       + " g used</span></p>";
    // This is where onboarding now lands, so it has to show what was entered —
    // otherwise submitting the form gives no confirmation that the tare and
    // nominal weight (the values every later reading depends on) took.
    String spec = specStrip(r);
    if (r.last_ts[0]) spec += kv("Last weighed", esc(r.last_ts));
    // Which product this spool is an instance of. Worth showing even though the
    // fields above are its resolved values: if two spools of the same filament
    // report different products, the matching ladder missed, and this line is
    // the only place that is visible.
    ProductRecord q;
    if (r.product && storeGetProduct(r.product, q))
        spec += kv("Product", "<a href='/products' style='color:#8f8'>#"
                              + String((unsigned)q.id) + "</a>"
                              + (q.provisional ? " <span class='ob'>provisional</span>"
                                               : String()));
    if (spec.length()) p += "<div class='spec'>" + spec + "</div>";
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
    String p = head("Onboard", "/onboard");
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

    // ── "Another spool of X" ──────────────────────────────────────────────────
    // The path that makes this worth building. Nine onboardings out of ten are
    // a repeat of one already done, and every retype is a fresh chance to pick
    // the wrong spool profile — which silently biases every later remaining
    // weight, because remaining is gross minus tare.
    const size_t np = storeProductCount();
    if (np) {
        p += "<label>This spool is&hellip;</label><select name='product' id='prod' "
             "onchange=\"document.getElementById('newprod').style.display="
             "this.value=='0'?'block':'none'\">";
        ProductRecord q;
        for (size_t i = 0; i < np; i++) {
            if (!storeProductAt(i, q)) continue;
            p += "<option value='" + String((unsigned)q.id) + "'>"
               + esc(q.vendor) + " " + esc(q.material[0] ? q.material : "Unknown")
               + (q.nom_g > 0 ? " &mdash; " + String(q.nom_g, 0) + " g" : String())
               + (q.provisional ? " (provisional)" : "")
               + "</option>";
        }
        p += "<option value='0'>&mdash; A new product &mdash;</option>";
        p += "</select>";
        // Default to the first existing product, so the common case is one
        // control and a submit; the detail fields start hidden to match.
        p += "<div id='newprod' style='display:none'>";
    } else {
        // Nothing stocked yet, so there is no "another spool of X" to offer.
        p += "<input type='hidden' name='product' value='0'>";
        p += "<div id='newprod'>";
    }

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
    p += "</div>";   // #newprod

    p += "<div><button type='submit'>Save &amp; write tag</button></div>";
    p += "</form>";
    p += FOOT;
    req->send(200, "text/html", p);
}

// ── POST /api/tare — one load-cell reading ────────────────────────────────────
static void handleApiTare(AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    xSemaphoreTake(gWeightMutex, portMAX_DELAY);
    float w = gWeightGrams;
    xSemaphoreGive(gWeightMutex);
    req->send(200, "application/json", "{\"weight\":" + String(w, 1) + "}");
}

// ── POST /api/onboard — apply the form, write the tag, log a reconcile ────────
static void handleApiOnboard(AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    auto arg = [&](const char* k) -> String {
        const AsyncWebParameter* pp = req->getParam(k, true);
        return pp ? pp->value() : String();
    };

    uint32_t id = (uint32_t)arg("id").toInt();
    if (id == 0) { req->send(400, "text/plain", "missing id"); return; }

    SpoolRecord rec;
    if (!storeGetSpool(id, rec)) { req->send(404, "text/plain", "unknown spool"); return; }

    // Two paths. "Another spool of X" inherits everything from an existing
    // product and ignores the detail fields entirely — that is the whole point
    // of it, and it is also what removes the chance to pick the wrong spool
    // profile on a repeat onboarding.
    const uint32_t chosen = (uint32_t)arg("product").toInt();

    String  vendor, display, abbr;
    uint8_t rgba[4] = {};
    float   nominal = 0, empty = 0, dia = 1.75f;
    CfgMaterial m = {};
    bool    haveMat = false;
    uint32_t pid = 0;

    ProductRecord q;
    if (chosen && storeGetProduct(chosen, q)) {
        pid     = q.id;
        vendor  = q.vendor;
        display = q.material;
        abbr    = q.abbr;
        memcpy(rgba, q.rgba, 4);
        dia = q.dia; empty = q.empty_g; nominal = q.nom_g;
        // The tag also wants print/bed temps and the material class/type, which
        // live on the config catalog rather than on the product. Look them up
        // by abbreviation; if there is no matching row the tag simply keeps the
        // temps it already had, which is what happens today for a foreign tag.
        haveMat = cfgMaterialByName(abbr.c_str(), m);
    } else {
        String matName  = arg("material");
        String colName  = arg("color");
        String profName = arg("profile");
        float  tareOvr  = arg("empty_g").toFloat();   // 0 → use profile

        vendor  = arg("vendor");
        haveMat = cfgMaterialByName(matName.c_str(), m);
        CfgColor col = {};
        cfgColorByName(colName.c_str(), col);
        memcpy(rgba, col.rgba, 4);

        CfgProfile pr = {};
        bool havePr = false;
        for (size_t i = 0; i < cfgProfileCount(); i++)
            if (cfgProfileAt(i, pr) && profName == pr.label) { havePr = true; break; }

        nominal = havePr ? pr.nominal_full_g : 0.0f;
        empty   = (tareOvr > 0.0f) ? tareOvr : (havePr ? pr.empty_g : 0.0f);
        dia     = haveMat ? m.dia : 1.75f;
        abbr    = haveMat ? m.abbr : "";

        // OPT key 10 material_name is a DISPLAY string, not a type code. The
        // spec's own example is "PC Blend Carbon Fiber Black", and it says
        // brand_name and material_name should be shown together as "Prusament
        // PLA Galaxy Black"; the short code belongs in key 52
        // material_abbreviation (max 7 chars).
        //
        // So the colour name goes here. That is what makes "Summer Grass" a
        // first-class part of the record: it round-trips on the tag, it is what
        // every OPT-aware reader will display, and it needs no field the spec
        // does not already define.
        display = matName;
        if (colName.length()) display += " " + colName;

        // Resolve which product these values describe, creating one if this is
        // a filament we have never stocked. Not provisional: a person typed it.
        //
        // Deliberately does NOT update a product that already matches. An edit
        // to a product propagates to every spool of it, and this form is about
        // ONE spool — someone correcting a typo here would silently rewrite a
        // whole shelf's tags. That edit belongs on /product, which says how
        // many spools it will touch before you press save.
        ProductRecord prod;
        strlcpy(prod.vendor,   vendor.c_str(),  sizeof(prod.vendor));
        strlcpy(prod.material, display.c_str(), sizeof(prod.material));
        strlcpy(prod.abbr,     abbr.c_str(),    sizeof(prod.abbr));
        memcpy(prod.rgba, rgba, 4);
        prod.dia = dia; prod.empty_g = empty; prod.nom_g = nominal;
        ProductRecord found;
        if (storeFindProduct(prod, found))      pid = found.id;
        else if (storeUpsertProduct(prod))      pid = prod.id;
    }

    // 1) Append the identity to the log so indices + inventory reflect it.
    StoreEvent e; e.ev = StoreEv::Reconcile;
    strlcpy(e.uuid, rec.uuid, sizeof(e.uuid));
    e.spool = id;
    strlcpy(e.vendor,   vendor.c_str(),  sizeof(e.vendor));
    strlcpy(e.material, display.c_str(), sizeof(e.material));
    strlcpy(e.abbr,     abbr.c_str(), sizeof(e.abbr));
    memcpy(e.rgba, rgba, 4);
    e.dia = dia; e.empty_g = empty; e.nom_g = nominal;
    e.needs_ob = false;
    // Identity events replace the record's whole identity group, so this must
    // be set explicitly — leaving it at 0 would silently un-assign the product
    // every time someone edited a spool through this form.
    e.product = pid ? pid : rec.product;
    storeAppendEvent(e);

    // 2) If this spool is on the scale, write the full OPT Main to its tag now.
    //    (Identity + material temps/class/type + weights.) syncTask's reconcile
    //    loop is idempotent, so a redundant later write is harmless.
    if (currentSpool() == (int)id) {
        xSemaphoreTake(gTagMutex, portMAX_DELAY);
        strlcpy(gTagMain.brand_name,            vendor.c_str(),  sizeof(gTagMain.brand_name));
        strlcpy(gTagMain.material_name,         display.c_str(), sizeof(gTagMain.material_name));
        strlcpy(gTagMain.material_abbreviation, abbr.c_str(), sizeof(gTagMain.material_abbreviation));
        memcpy(gTagMain.primary_color_rgba, rgba, 4);
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

    // Land on the spool that was just onboarded, not the dashboard. Submitting
    // eight fields and being returned to an aggregate roll-up gives no
    // confirmation that any of them took; the detail page shows the colour,
    // vendor, tare, nominal and weigh history for this exact record.
    req->redirect("/spool?id=" + String((unsigned)id));
}

// ── Reorder: roll up on-hand per stock item, flag shortfalls ──────────────────
struct OnHand { uint16_t count; float grams; uint32_t product; };

// Sum the active spools a stock item stands for.
//
// PREFERRED: resolve the stock item to a product and count spools by product
// id. That is an exact lookup, and it is colour- and size-specific — "we keep
// four Prusament PETG Orange 1 kg" stops being answered by a shelf of Prusament
// PETG in four other colours. The probe composes `material` the same way the
// onboard form does ("PETG" + " " + "Orange"), because that is what the OPT
// display string holds and what products are keyed on.
//
// FALLBACK, when no product matches: the original vendor + abbreviation match,
// so stock items that predate products keep working exactly as before rather
// than silently reading zero on hand and demanding a reorder of everything.
//
// The fallback matches on the record's ABBREVIATION, not r.material:
// CfgStock.material is a bare type ("PLA") while r.material carries the display
// string ("PLA Summer Grass"), so comparing those directly never matches.
// Records with no abbreviation (seeded rows, foreign tags) fall back to
// r.material, which for those IS the bare type.
static OnHand rollUp(const CfgStock& s) {
    OnHand oh{0, 0.0f, 0};

    ProductRecord probe, found;
    strlcpy(probe.vendor, s.vendor, sizeof(probe.vendor));
    String disp = s.material;
    if (s.color[0]) disp += String(" ") + s.color;
    strlcpy(probe.material, disp.c_str(), sizeof(probe.material));
    strlcpy(probe.abbr, s.material, sizeof(probe.abbr));
    probe.nom_g = s.spool_g;
    if (s.gtin[0]) probe.gtin = strtoull(s.gtin, nullptr, 10);
    if (storeFindProduct(probe, found)) oh.product = found.id;

    size_t n = storeSpoolCount();
    SpoolRecord r;
    for (size_t i = 0; i < n; i++) {
        if (!storeSpoolAt(i, r)) continue;
        if (r.remaining_g <= 1.0f) continue;
        bool hit;
        if (oh.product) {
            hit = (r.product == oh.product);
        } else {
            const char* mat = r.abbr[0] ? r.abbr : r.material;
            hit = (strcmp(r.vendor, s.vendor) == 0 && strcmp(mat, s.material) == 0);
        }
        if (hit) { oh.count++; oh.grams += r.remaining_g; }
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

    String p = head("Reorder", "/reorder");
    p += "<h3>Reorder list</h3>";
    p += "<p class='muted'>Standard-stock items at or below their threshold. "
         "Review, then <a href='/reorder?format=csv' style='color:#8f8'>download CSV</a> "
         "or e-mail the list to place the order.</p>";
    p += "<table><tr><th>Vendor</th><th>Material</th><th>Color</th>"
         "<th>On hand</th><th>Threshold</th><th>Matched by</th></tr>";
    CfgStock s;
    int flagged = 0;
    String body;      // plain-text version, for the mailto: link
    int    unmatched = 0;
    for (size_t i = 0; i < cfgStockCount(); i++) {
        if (!cfgStockAt(i, s)) continue;
        OnHand oh = rollUp(s);
        if (!belowThreshold(s, oh)) continue;
        flagged++;
        if (!oh.product) unmatched++;
        String thr = s.min_spools > 0 ? (String(s.min_spools) + " spools")
                   : s.min_grams  > 0 ? (String(s.min_grams, 0) + " g")
                   : String("empty");
        p += "<tr><td>" + esc(s.vendor) + "</td><td>" + esc(s.material) + "</td><td>"
           + esc(s.color) + "</td><td>" + String(oh.count) + " spool(s), "
           + String(oh.grams, 0) + " g</td><td>" + thr + "</td><td>"
           // Which rule produced this row. A stock item falling back to the name
           // match is not wrong, but it is coarser — it will count every colour
           // of that vendor's PLA — and there is no other way to see that.
           + (oh.product
                ? "<a href='/product?id=" + String((unsigned)oh.product)
                  + "' style='color:#8f8'>product #" + String((unsigned)oh.product) + "</a>"
                : String("<span class='muted'>name (no product)</span>"))
           + "</td></tr>";

        body += String("- ") + s.vendor + " " + s.material;
        if (s.color[0]) body += String(" ") + s.color;
        if (s.sku[0])   body += String(" [SKU ") + s.sku + "]";
        if (s.pack_qty > 1) body += String(" x") + String(s.pack_qty);
        body += String("  (on hand: ") + String(oh.count) + " spools, "
              + String(oh.grams, 0) + " g; keep " + thr + ")\n";
    }
    if (flagged == 0)
        p += "<tr><td colspan='6' class='muted'>Everything is above threshold "
             "(or no stock items configured)</td></tr>";
    p += "</table>";

    if (flagged) {
        // mailto: rather than SMTP. The station has no mail credentials and no
        // business holding any — it hands the list to whoever is standing in
        // front of it, in their own mail client, from their own address.
        //
        // Both the subject and body must be percent-encoded: a raw newline or
        // ampersand truncates the URL, and the body is entirely newlines.
        p += "<p><a style='display:inline-block;margin-top:4px;"
             "padding:10px 18px;background:#2a5;color:#000;border-radius:6px;"
             "text-decoration:none' href='mailto:?subject="
           + urlEnc("Filament reorder — " + String(flagged) + " item(s)")
           + "&body="
           + urlEnc("Below threshold at the weigh station:\n\n" + body
                    + "\nGenerated by the weigh station.\n")
           + "'>E-mail this list</a></p>";
    }
    if (unmatched)
        p += "<p class='muted'>" + String(unmatched) + " item(s) matched by name "
             "rather than to a product, so their on-hand count includes every "
             "colour of that vendor's material. Onboard a spool of each to make "
             "the match exact.</p>";
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
    String p = head("Config", "/config");
    p += "<h3>Config catalog</h3>";
    p += "<p class='muted'>Edit the reference tables that drive onboarding and "
         "reordering. Each is a JSON array; Save validates and persists it.</p>";
    configTableForm(p, "Vendors",        "vendors");
    configTableForm(p, "Materials",      "materials");
    configTableForm(p, "Spool profiles", "spool-profiles");
    configTableForm(p, "Colors",         "colors");
    configTableForm(p, "Stock items",    "stock-items");

    p += "<h3>API access</h3>";
    if (apiKeyIsSet())
        p += "<div class='card'><p>An API key <b>is set</b>. Endpoints that change "
             "state require it; read-only endpoints stay open.</p>";
    else
        p += "<div class='card' style='border-color:#a70'><p><b class='ob'>No API key "
             "set.</b> Anything on this network can onboard spools, edit these tables, "
             "recalibrate the scale, replace the event log, or reset WiFi.</p>";
    p += "<label>API key <span class='muted'>(blank clears it and reopens the API)</span></label>"
         "<input id='ak' type='text' placeholder='a long random string' autocomplete='off'>"
         "<div style='margin-top:8px'><button type='button' onclick='sk()'>Save key</button></div>"
         "<p class='muted' id='akmsg'></p>"
         "<p class='muted'>Send it as <code>X-API-Key: &lt;key&gt;</code>, as "
         "<code>?key=&lt;key&gt;</code>, or as HTTP Basic auth "
         "(<code>curl -u :&lt;key&gt;</code>). Lost it? Clear it over serial with "
         "<code>APIKEY none</code>.</p></div>";
    p += "<script>"
         "function sk(){var k=document.getElementById('ak').value;"
         "fetch('/api/apikey',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
         "body:'key='+encodeURIComponent(k)})"
         ".then(r=>{document.getElementById('akmsg').textContent="
         "r.ok?'Saved. New requests need the new key.':'Refused (HTTP '+r.status+') \u2014 "
         "the current key is required to change it.';});}"
         "</script>";

    p += FOOT;
    req->send(200, "text/html", p);
}

// Set or clear the shared secret. Guarded by the CURRENT key, so the first one
// can be set on an open device but rotating it needs the old one.
static void handleApiKeySet(AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    String k;
    if (req->hasParam("key", true))      k = req->getParam("key", true)->value();
    else if (req->hasParam("key"))       k = req->getParam("key")->value();
    apiKeySet(k.c_str());
    req->send(200, "application/json",
              String("{\"ok\":true,\"required\":") + (apiKeyIsSet() ? "true" : "false") + "}");
}

static void handleApiConfigSave(AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
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

// ── Consumption history ───────────────────────────────────────────────────────
// Which filament the lab actually goes through, as opposed to what is on the
// shelf. Sourced from UsageRow, which survives log compaction — see store.h.
// ── Device status ─────────────────────────────────────────────────────────────
// One poll for everything a dashboard wants: what the machine is doing, what is
// on it, and whether it is healthy. Open (no key) — it is read-only.
static void handleApiStatus(AsyncWebServerRequest* req) {
    xSemaphoreTake(gStateMutex, portMAX_DELAY);
    const DeviceState st = gState;
    xSemaphoreGive(gStateMutex);

    xSemaphoreTake(gWeightMutex, portMAX_DELAY);
    const float w = gWeightGrams;
    xSemaphoreGive(gWeightMutex);

    const bool sta = (WiFi.status() == WL_CONNECTED);

    String j = "{";
    j += "\"firmware\":\"";  j += FW_VERSION;
    j += "\",\"state\":\"";   j += deviceStateName(st);
    j += "\",\"uptime_s\":";  j += String((unsigned)(millis() / 1000));
    j += ",\"heap_free\":";   j += String((unsigned)ESP.getFreeHeap());

    j += ",\"scale\":{\"weight_g\":"; j += String(w, 1);
    j += ",\"calibrated\":";           j += gScaleCalibrated ? "true" : "false";
    j += "}";

    // Spool currently on the scale, if any. -1 when nothing is present.
    const int cur = currentSpool();
    j += ",\"spool\":";
    if (cur > 0) {
        SpoolRecord r;
        if (storeGetSpool((uint32_t)cur, r)) {
            j += "{\"id\":";             j += String((unsigned)r.spool);
            j += ",\"uuid\":\"";          j += r.uuid;
            j += "\",\"vendor\":\"";      j += esc(r.vendor);
            j += "\",\"material\":\"";    j += esc(r.material);
            // colorJson() supplies its own quotes (or bare null), so unlike the
            // lines above this one must NOT open or close a string of its own.
            j += "\",\"color\":";        j += colorJson(r.rgba);
            j += ",\"remaining_g\":";    j += String(r.remaining_g, 1);
            j += ",\"needs_onboarding\":"; j += r.needs_ob ? "true" : "false";
            j += "}";
        } else j += "null";
    } else j += "null";

    j += ",\"wifi\":{\"mode\":\"";  j += sta ? "station" : (gApSsid[0] ? "ap" : "none");
    j += "\",\"ssid\":\"";          j += sta ? esc(WiFi.SSID().c_str()) : esc(gApSsid);
    j += "\",\"ip\":\"";            j += sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    j += "\",\"rssi\":";            j += String(sta ? WiFi.RSSI() : 0);
    j += "}";

    j += ",\"storage\":{\"spools\":";  j += String((unsigned)storeSpoolCount());
    j += ",\"log_bytes\":";             j += String((unsigned)storeLogBytes());
    j += ",\"log_lines\":";             j += String((unsigned)storeLogLineCount());
    j += ",\"usage_rows\":";            j += String((unsigned)storeUsageCount());
    j += ",\"free_bytes\":";            j += String((unsigned)storeFreeBytes());
    j += ",\"compact_due\":";           j += storeCompactNeeded() ? "true" : "false";
    j += ",\"write_failed\":";          j += storeWriteFailed() ? "true" : "false";
    j += "}";

    j += ",\"auth\":{\"required\":"; j += apiKeyIsSet() ? "true" : "false";
    j += "}}";
    req->send(200, "application/json", j);
}

static void handleUsagePage(AsyncWebServerRequest* req) {
    String p = head("Usage", "/usage");
    p += "<h3>Filament consumed</h3>";

    const size_t n = storeUsageCount();
    if (n == 0) {
        p += "<div class='card muted'>No consumption recorded yet. A spool has to "
             "be weighed at least twice before there is a delta to attribute.</div>";
        p += FOOT;
        req->send(200, "text/html", p);
        return;
    }

    // Totals per material across all time — the headline "what is popular"
    // number. Materials are few, so a linear merge is fine.
    struct Tot { String key; float g; uint32_t w; };
    std::vector<Tot> byMat;
    float grand = 0;
    UsageRow u;
    for (size_t i = 0; i < n; i++) {
        if (!storeUsageAt(i, u)) continue;
        grand += u.grams;
        bool found = false;
        for (auto& t : byMat)
            if (t.key == u.material) { t.g += u.grams; t.w += u.weighs; found = true; break; }
        if (!found) byMat.push_back({String(u.material), u.grams, u.weighs});
    }
    std::sort(byMat.begin(), byMat.end(), [](const Tot& a, const Tot& b){ return a.g > b.g; });

    p += "<table><tr><th>Material</th><th>Consumed</th><th>Share</th></tr>";
    for (const auto& t : byMat) {
        const int pct = grand > 0 ? (int)(100.0f * t.g / grand + 0.5f) : 0;
        p += "<tr><td>" + esc(t.key.c_str()) + "</td><td>"
           + String(t.g / 1000.0f, 2) + " kg</td><td>" + String(pct) + "%</td></tr>";
    }
    p += "</table>";
    p += "<p class='muted'>Total " + String(grand / 1000.0f, 2) + " kg across "
       + String((unsigned)n) + " month/vendor/material buckets.</p>";

    p += "<h3>By month</h3><table>"
         "<tr><th>Period</th><th>Vendor</th><th>Material</th><th>Consumed</th><th>Weighs</th></tr>";
    // Newest first: periods are "YYYY-MM", so lexical order is chronological.
    std::vector<size_t> order;
    for (size_t i = 0; i < n; i++) order.push_back(i);
    std::sort(order.begin(), order.end(), [](size_t a, size_t b) {
        UsageRow x, y;
        storeUsageAt(a, x); storeUsageAt(b, y);
        int c = strcmp(y.period, x.period);
        return c ? (c < 0) : (x.grams > y.grams);
    });
    for (size_t i : order) {
        if (!storeUsageAt(i, u)) continue;
        p += "<tr><td>" + esc(u.period) + "</td><td>" + esc(u.vendor)
           + "</td><td>" + esc(u.material) + "</td><td>" + String(u.grams, 0)
           + " g</td><td>" + String((unsigned)u.weighs) + "</td></tr>";
    }
    p += "</table>";
    p += "<p><a href='/usage.csv'><button type='button'>Download CSV</button></a></p>";
    p += "<p class='muted'>These totals are kept permanently and are not lost when "
         "the event log is compacted. Consumption is the drop in a spool's remaining "
         "weight between two weighings, attributed to the vendor and material "
         "recorded at that time; a spool's first weighing establishes its baseline "
         "and counts as zero.</p>";
    p += FOOT;
    req->send(200, "text/html", p);
}

static void handleUsageCsv(AsyncWebServerRequest* req) {
    String c = "period,vendor,material,grams,weighs\n";
    const size_t n = storeUsageCount();
    UsageRow u;
    for (size_t i = 0; i < n; i++) {
        if (!storeUsageAt(i, u)) continue;
        // Quote the free-text columns; vendor/material can contain commas.
        c += String(u.period) + ",\"" + u.vendor + "\",\"" + u.material + "\","
           + String(u.grams, 1) + "," + String((unsigned)u.weighs) + "\n";
    }
    AsyncWebServerResponse* r = req->beginResponse(200, "text/csv", c);
    r->addHeader("Content-Disposition", "attachment; filename=\"usage.csv\"");
    req->send(r);
}

static void handleApiUsage(AsyncWebServerRequest* req) {
    String j = "[";
    const size_t n = storeUsageCount();
    UsageRow u;
    for (size_t i = 0; i < n; i++) {
        if (!storeUsageAt(i, u)) continue;
        if (j.length() > 1) j += ",";
        j += "{\"period\":\"";  j += esc(u.period);
        j += "\",\"vendor\":\"";  j += esc(u.vendor);
        j += "\",\"material\":\""; j += esc(u.material);
        j += "\",\"grams\":";     j += String(u.grams, 1);
        j += ",\"weighs\":";      j += String((unsigned)u.weighs);
        j += "}";
    }
    j += "]";
    req->send(200, "application/json", j);
}

static void handleBackupPage(AsyncWebServerRequest* req) {
    String p = head("Backup", "/backup");
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
    p += "<h3>Storage</h3>";
    p += "<div class='card'><div id='st' class='muted'>&mdash;</div></div>";
    p += "<p class='muted'>The event log is append-only. Once it passes its "
         "high-water mark the station compacts it while the scale is idle: each "
         "spool's history folds into a single checkpoint and the most recent "
         "events are kept as-is.</p>";
    p += "<p class='muted'><b>Kept forever:</b> current spool state, and monthly "
         "consumption per vendor and material (the <a href='/usage' "
         "style='color:#8f8'>Usage</a> page) &mdash; those ride in this same log "
         "file, so this download captures them. <b>Given up:</b> individual weigh "
         "readings older than the retained tail. If you want the raw per-weigh "
         "history long-term, download the log periodically.</p>";
    p += "<script>"
         "function kb(b){return b>1048576?(b/1048576).toFixed(1)+' MB':(b/1024).toFixed(0)+' kB';}"
         "function s(){fetch('/api/storage').then(x=>x.json()).then(d=>{"
         "var e=document.getElementById('st');"
         "var h='<b>Log:</b> '+kb(d.log)+' ('+d.lines+' events)'+"
         "' &nbsp; <b>Filesystem:</b> '+kb(d.free)+' free / '+kb(d.total);"
         "if(d.compact)h+=\"<div style='color:#fc6;margin-top:8px'>Compaction due \"+"
         "\"&mdash; runs automatically next time the scale is idle.</div>\";"
         "if(d.write_failed)h+=\"<div style='color:#f66;margin-top:8px'><b>Writes are \"+"
         "\"failing.</b> Events are not being recorded. Download the log now, then \"+"
         "\"free space or re-import a trimmed log.</div>\";"
         "else if(d.low)h+=\"<div style='color:#fc6;margin-top:8px'>Free space is low.</div>\";"
         "e.innerHTML=h;});}"
         "setInterval(s,5000);s();"
         "</script>";
    p += "<p class='muted'>Config tables also back up separately via the "
         "<a href='/config' style='color:#8f8'>Config</a> page (copy the JSON).</p>";
    p += FOOT;
    req->send(200, "text/html", p);
}

// ── Storage health ────────────────────────────────────────────────────────────
static void handleApiStorage(AsyncWebServerRequest* req) {
    const size_t logB  = storeLogBytes();
    const size_t freeB = storeFreeBytes();
    String j = "{";
    j += "\"log\":";          j += String((unsigned)logB);
    j += ",\"lines\":";       j += String((unsigned)storeLogLineCount());
    j += ",\"free\":";        j += String((unsigned)freeB);
    j += ",\"total\":";       j += String((unsigned)storeTotalBytes());
    j += ",\"compact\":";     j += storeCompactNeeded() ? "true" : "false";
    j += ",\"low\":";         j += (freeB < STORE_FREE_WARN_BYTES) ? "true" : "false";
    j += ",\"write_failed\":";j += storeWriteFailed() ? "true" : "false";
    j += "}";
    req->send(200, "application/json", j);
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
// Checked once at the start of the upload and remembered for the rest of it:
// authOk() sends a 401 when it fails, and re-running it per chunk would emit a
// response for every chunk of the body.
static bool sImportAuthed = false;

static void handleImportUpload(AsyncWebServerRequest* req, const String& filename,
                               size_t index, uint8_t* data, size_t len, bool final) {
    if (index == 0) sImportAuthed = !apiKeyIsSet() || authOk(req);
    if (!sImportAuthed) return;      // discard the body; Done finishes the refusal
    File f = LittleFS.open(IMPORT_STAGING, index == 0 ? "w" : "a");
    if (f) { f.write(data, len); f.close(); }
}

// Called after the upload completes: validate + promote, or reject.
static void handleImportDone(AsyncWebServerRequest* req) {
    if (!sImportAuthed) { LittleFS.remove(IMPORT_STAGING); return; }  // 401 already sent
    if (!authOk(req)) return;
    bool ok = storeImportLogFile(IMPORT_STAGING);
    LittleFS.remove(IMPORT_STAGING);   // no-op if promotion already consumed it
    if (ok) {
        String p = head("Restore", "/backup");
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
    if (!authOk(req)) return;
    gCalZeroReq = true;                       // scaleTask tares on its next loop
    req->send(200, "application/json", "{\"ok\":true}");
}

static void handleApiCal(AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    const AsyncWebParameter* pp = req->getParam("grams", true);
    float g = pp ? pp->value().toFloat() : 0.0f;
    if (g <= 0.0f) { req->send(400, "text/plain", "need grams>0"); return; }
    gCalSetGrams = g;                         // scaleTask calibrates on its next loop
    req->send(200, "application/json", "{\"ok\":true}");
}

static void handleCalibratePage(AsyncWebServerRequest* req) {
    String p = head("Calibrate", "/calibrate");
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

    // ── CORS ──────────────────────────────────────────────────────────────
    // Without these a browser-based dashboard served from any other origin
    // cannot read the JSON endpoints at all. Non-browser clients (curl, Python,
    // Home Assistant, Grafana) never cared — CORS is enforced by browsers only.
    //
    // "*" is right here: this is a read-mostly device on a lab LAN, and the
    // wildcard deliberately makes browsers refuse to send cookies or Basic-auth
    // credentials cross-origin. A cross-origin caller that needs to POST must
    // present X-API-Key explicitly, which is exactly the intent.
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers",
                                         "Content-Type, X-API-Key, Authorization");
    DefaultHeaders::Instance().addHeader("Access-Control-Max-Age", "600");

    // Sending X-API-Key makes a cross-origin POST "non-simple", so the browser
    // fires a preflight OPTIONS first. Answer every path, unauthenticated — a
    // preflight carries no body and performs no action.
    sServer.on("/*", HTTP_OPTIONS, [](AsyncWebServerRequest* req) {
        req->send(204);
    });

    sServer.on("/",            HTTP_GET,  handleRoot);
    sServer.on("/spools",      HTTP_GET,  handleSpools);
    sServer.on("/spool",       HTTP_GET,  handleSpoolDetail);
    sServer.on("/api/spools",  HTTP_GET,  handleApiSpools);
    sServer.on("/products",     HTTP_GET,  handleProducts);
    sServer.on("/product",      HTTP_GET,  handleProductDetail);
    sServer.on("/api/products", HTTP_GET,  handleApiProducts);
    sServer.on("/api/product",  HTTP_POST, handleApiProduct);
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
    sServer.on("/api/storage",    HTTP_GET,  handleApiStorage);
    sServer.on("/usage",          HTTP_GET,  handleUsagePage);
    sServer.on("/usage.csv",      HTTP_GET,  handleUsageCsv);
    sServer.on("/api/usage",      HTTP_GET,  handleApiUsage);
    sServer.on("/api/status",     HTTP_GET,  handleApiStatus);
    sServer.on("/api/apikey",     HTTP_POST, handleApiKeySet);

    // Re-provisioning for a cabinet-installed unit with no physical access:
    // clear stored WiFi credentials and reboot into the captive portal.
    //
    // POST, not GET: this wipes the network config, and as a GET any page on
    // the LAN that merely links to or embeds the URL could trigger it just by
    // being loaded in someone's browser.
    sServer.on("/reset", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (!authOk(req)) return;
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
    // GET still lands somewhere useful instead of 404ing, but only offers the
    // button — it changes nothing by itself.
    sServer.on("/reset", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send(200, "text/html",
            "<!DOCTYPE html><body><h2>Reset WiFi credentials</h2>"
            "<p>Clears the stored network and reboots into the setup portal.</p>"
            "<form method='POST' action='/reset'>"
            "<button type='submit'>Reset WiFi</button></form></body>");
    });

    sServer.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "Not found");
    });

    sServer.begin();
    Serial.printf("Web app: http://%s.local/\n", DEVICE_HOSTNAME);
}
