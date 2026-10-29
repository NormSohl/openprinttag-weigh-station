// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

// The weigh station's web app, running on the host. Compiles the REAL
// src/web_app.cpp, store.cpp, config_store.cpp and friends; replaces only
// the hardware tasks (scale, NFC, sync, controller, display) with a small
// model driven from a control panel at /_sim. See README.md.
#include "sim.h"
#include "httplib.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <atomic>
#include <random>
#include <thread>
#include "device_state.h"
#include "opt_tag.h"
#include "store.h"
#include "config_store.h"
#include "api_key.h"
#include "display_tz.h"
#include "station_name.h"
#include "last_onboard.h"
#include "last_stock.h"
#include "web_app.h"

// ── Shared globals (main.cpp on the device) ──────────────────────────────────
// gClockSet is defined by the store harness's shim.cpp.
extern volatile bool gClockSet;
volatile DeviceState gState = DeviceState::Boot;
SemaphoreHandle_t    gStateMutex  = nullptr;
volatile float       gWeightGrams = 0.0f;
SemaphoreHandle_t    gWeightMutex = nullptr;
volatile bool        gScaleCalibrated = true;
volatile bool        gCalZeroReq  = false;
volatile float       gCalSetGrams = 0.0f;
uint8_t              gTagUid[8]   = {};
OptMain              gTagMain     = {};
SemaphoreHandle_t    gTagMutex    = nullptr;
volatile bool        gWriteMainPending = false;
volatile bool        gEraseModeActive     = false;
volatile uint32_t    gEraseModeActivityMs = 0;
volatile int         gSpoolId              = -1;
volatile bool        gSpoolNeedsOnboarding = false;
char                 gApSsid[24] = {};

WiFiShim WiFi;
EspShim  ESP;
MdnsShim MDNS;
void delay(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ── Simulated hardware ───────────────────────────────────────────────────────
// Mirrors what syncTask does on a placement (sync_task.cpp, Resolving and
// Weighing), using the same store calls, so the log the sim writes looks
// like the device's. Every function here runs under gSimLock.
static std::mt19937_64 sRng{std::random_device{}()};

static void randomHex(char* out, size_t nbytes) {
    for (size_t i = 0; i < nbytes; i++) snprintf(out + i * 2, 3, "%02x", (unsigned)(sRng() & 0xFF));
}
static bool hexToBytes(const char* hex, uint8_t* out, size_t n) {
    if (strlen(hex) != n * 2) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

static void setState(DeviceState s) {
    gState = s;
    webAppNotifyStateChanged();
}

// The weigh: store tare when the record has one, else the tag's -- exactly
// syncTask's rule -- and one Weigh event.
static void weigh(const SpoolRecord& rec, float gross) {
    float tare = rec.empty_g > 0.0f ? rec.empty_g : gTagMain.empty_container_weight;
    float remaining = tare > 0.0f ? gross - tare : gross;
    if (remaining < 0.0f) remaining = 0.0f;
    float used = gTagMain.actual_netto_full_weight > 0.0f
               ? gTagMain.actual_netto_full_weight - remaining : 0.0f;
    if (used < 0.0f) used = 0.0f;
    Serial.printf("[sim] weigh #%u: gross %.1f  tare %.1f  remaining %.1f  used %.1f\n",
                  (unsigned)rec.spool, gross, tare, remaining, used);
    StoreEvent w; w.ev = StoreEv::Weigh;
    strlcpy(w.uuid, rec.uuid, sizeof(w.uuid));
    w.spool = rec.spool;
    w.gross_g = gross; w.remaining_g = remaining; w.used_g = used;
    storeAppendEvent(w);
}

static std::string simRemove() {
    gSpoolId = -1;
    gSpoolNeedsOnboarding = false;
    gWeightGrams = 0.0f;
    gTagMain = {};
    memset(gTagUid, 0, sizeof(gTagUid));
    setState(DeviceState::Idle);
    return "removed";
}

// A known spool's tag lands on the scale.
static std::string simPlace(uint32_t id, float gross) {
    SpoolRecord rec;
    if (!storeGetSpool(id, rec)) return "no such spool";
    gTagMain = {};
    hexToBytes(rec.uuid, gTagMain.instance_uuid, 16);
    hexToBytes(rec.nfc_uid, gTagUid, 8);
    gTagMain.empty_container_weight   = rec.empty_g;
    gTagMain.nominal_netto_full_weight = rec.nom_g;
    gTagMain.actual_netto_full_weight  = rec.nom_g;
    if (gross <= 0.0f) gross = rec.remaining_g + rec.empty_g;   // "same as last time"
    gWeightGrams = gross;
    gSpoolId = (int)rec.spool;
    gSpoolNeedsOnboarding = rec.needs_ob;
    weigh(rec, gross);
    setState(DeviceState::Present);
    return "placed #" + std::to_string(rec.spool);
}

// A blank tag lands on the scale and the confirm countdown runs out: mint a
// stub exactly as syncTask's Resolving phase does, then weigh it.
static std::string simBlank(float gross) {
    char hex[33], phys[17];
    randomHex(hex, 16);
    randomHex(phys, 8);
    StoreEvent e; e.ev = StoreEv::Onboard;
    strlcpy(e.uuid, hex, sizeof(e.uuid));
    strlcpy(e.vendor,   "Unknown", sizeof(e.vendor));
    strlcpy(e.material, "Unknown", sizeof(e.material));
    e.needs_ob = true;
    e.foreign  = false;
    strlcpy(e.nfc_uid, phys, sizeof(e.nfc_uid));
    e.spool = storeNextSpoolId();
    storeAppendEvent(e);

    gTagMain = {};
    hexToBytes(hex, gTagMain.instance_uuid, 16);
    hexToBytes(phys, gTagUid, 8);
    gWeightGrams = gross;
    gSpoolId = (int)e.spool;
    gSpoolNeedsOnboarding = true;
    SpoolRecord rec;
    storeGetSpool(e.spool, rec);
    weigh(rec, gross);
    setState(DeviceState::Present);
    return "new stub #" + std::to_string(e.spool);
}

// What nfcTask, syncTask's Holding poll and scaleTask would do in the
// background, once a tick: accept tag writes, follow needs_onboarding, answer
// calibration requests.
static void simTick() {
    std::lock_guard<std::recursive_mutex> g(gSimLock);
    if (gWriteMainPending) {
        gWriteMainPending = false;
        Serial.printf("[sim] tag Main written: %s %s\n", gTagMain.brand_name, gTagMain.material_name);
    }
    if (gSpoolId > 0) {
        SpoolRecord r;
        if (storeGetSpool((uint32_t)gSpoolId, r) && r.needs_ob != gSpoolNeedsOnboarding) {
            gSpoolNeedsOnboarding = r.needs_ob;
            webAppNotifyStateChanged();
        }
    }
    if (gCalZeroReq) { gCalZeroReq = false; Serial.println("[sim] scale zeroed"); }
    if (gCalSetGrams > 0.0f) {
        Serial.printf("[sim] scale calibrated against %.1f g\n", gCalSetGrams);
        gCalSetGrams = 0.0f;
        gScaleCalibrated = true;
    }
}

// ── Control panel / API (/_sim) ──────────────────────────────────────────────
static float paramF(const httplib::Request& r, const char* k, float def) {
    return r.has_param(k) ? strtof(r.get_param_value(k).c_str(), nullptr) : def;
}

static std::string stateJson() {
    std::string j = "{\"state\":\"";
    j += deviceStateName(gState);
    j += "\",\"spool\":" + std::to_string(gSpoolId);
    j += ",\"needs_onboarding\":" + std::string(gSpoolNeedsOnboarding ? "true" : "false");
    j += ",\"weight\":" + std::to_string(gWeightGrams);
    j += ",\"spools\":[";
    SpoolRecord r;
    for (size_t i = 0, n = storeSpoolCount(); i < n; i++) {
        if (!storeSpoolAt(i, r)) continue;
        if (j.back() != '[') j += ",";
        j += "{\"id\":" + std::to_string(r.spool) + ",\"name\":\"";
        for (const char* s : {(const char*)r.vendor, " ", (const char*)r.material})
            for (const char* c = s; *c; c++) { if (*c == '"' || *c == '\\') j += '\\'; j += *c; }
        j += "\",\"remaining_g\":" + std::to_string(r.remaining_g);
        j += ",\"retired\":" + std::string(r.retired ? "true" : "false") + "}";
    }
    j += "]}";
    return j;
}

static const char* PANEL = R"HTML(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Weigh Station Sim</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee;display:flex;height:100vh}
#ctl{width:300px;padding:14px;box-sizing:border-box;background:#1a1a1a;border-right:1px solid #333;overflow:auto}
#ctl h2{font-size:16px;margin:0 0 10px;color:#9c9}
#ctl label{display:block;margin:10px 0 4px;color:#9c9;font-size:13px}
#ctl select,#ctl input{width:100%;box-sizing:border-box;padding:6px;background:#222;color:#eee;border:1px solid #444;border-radius:4px}
#ctl button{margin:6px 4px 0 0;padding:7px 10px;background:#2a5;color:#000;border:0;border-radius:5px;cursor:pointer}
#ctl button.sec{background:#345;color:#eee}
#st,#log{font:12px ui-monospace,Menlo,monospace;white-space:pre-wrap;color:#aaa;margin-top:10px}
iframe{flex:1;border:0;background:#111}
</style></head><body>
<div id="ctl">
<h2>Weigh Station Sim</h2>
<div id="st">…</div>
<label>Spool</label><select id="spool"></select>
<label>Gross weight on the scale (g) — blank = last remaining + tare</label><input id="gross" type="number" step="0.1">
<button onclick="act('place')">Place spool</button>
<button onclick="act('blank')">Place blank tag</button>
<button class="sec" onclick="act('remove')">Remove</button>
<label>Serial command (SEED 12 20, DUMP, COMPACT, …)</label><input id="cmd" placeholder="SEED 12 20">
<button class="sec" onclick="cmd()">Run</button>
<div id="log"></div>
</div>
<iframe id="app" src="/"></iframe>
<script>
const $=id=>document.getElementById(id);
function log(t){$("log").textContent=t+"\n"+$("log").textContent;}
async function refresh(){
  const s=await (await fetch("/_sim/state")).json();
  $("st").textContent=`state: ${s.state}\nspool: ${s.spool}${s.needs_onboarding?" (needs onboarding)":""}\nweight: ${s.weight.toFixed(1)} g`;
  const sel=$("spool"),keep=sel.value;
  sel.innerHTML=s.spools.map(p=>`<option value="${p.id}">#${p.id} ${p.name.replace(/</g,"&lt;")} — ${p.remaining_g.toFixed(0)} g${p.retired?" (retired)":""}</option>`).join("");
  if(keep)sel.value=keep;
}
async function act(a){
  const b=new URLSearchParams();
  if(a=="place")b.set("spool",$("spool").value);
  if($("gross").value)b.set("gross",$("gross").value);
  else if(a=="blank")b.set("gross","1220");
  log(await (await fetch("/_sim/"+a,{method:"POST",body:b})).text());
  refresh();
}
async function cmd(){
  const b=new URLSearchParams({line:$("cmd").value});
  log(await (await fetch("/_sim/cmd",{method:"POST",body:b})).text());
  refresh(); $("app").contentWindow.location.reload();
}
refresh(); setInterval(refresh,2000);
</script></body></html>)HTML";

static void simRoutes(httplib::Server& svr) {
    svr.Get("/_sim", [](const httplib::Request&, httplib::Response& res) {
        res.set_content(PANEL, "text/html");
    });
    svr.Get("/_sim/state", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::recursive_mutex> g(gSimLock);
        res.set_content(stateJson(), "application/json");
    });
    svr.Post("/_sim/place", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::recursive_mutex> g(gSimLock);
        uint32_t id = (uint32_t)strtoul(req.get_param_value("spool").c_str(), nullptr, 10);
        std::string out = simPlace(id, paramF(req, "gross", 0));
        res.status = out.rfind("placed", 0) == 0 ? 200 : 404;
        res.set_content(out, "text/plain");
    });
    svr.Post("/_sim/blank", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::recursive_mutex> g(gSimLock);
        res.set_content(simBlank(paramF(req, "gross", 1220)), "text/plain");
    });
    svr.Post("/_sim/remove", [](const httplib::Request&, httplib::Response& res) {
        std::lock_guard<std::recursive_mutex> g(gSimLock);
        res.set_content(simRemove(), "text/plain");
    });
    // The serial console's store commands (SEED, DUMP, COMPACT, WIPE ...).
    // Their output goes to the sim's stdout, as it goes to serial on the device.
    svr.Post("/_sim/cmd", [](const httplib::Request& req, httplib::Response& res) {
        std::lock_guard<std::recursive_mutex> g(gSimLock);
        String line = req.get_param_value("line").c_str();
        line.trim();
        bool ok = storeSerialCommand(line);
        webAppNotifyStateChanged();
        res.set_content(std::string(ok ? "ok: " : "not a store command: ") + line.c_str()
                        + " (output on the sim's terminal)", "text/plain");
    });
}

// ── Boot ─────────────────────────────────────────────────────────────────────
int main(int, char**) {
    // Line-buffered even when redirected, so the log reads like the serial monitor.
    setvbuf(stdout, nullptr, _IOLBF, 0);
    // STORE_TEST_ROOT is where the shim keeps LittleFS + NVS; give the sim
    // its own default so it never touches a test run's files.
    if (!getenv("STORE_TEST_ROOT")) setenv("STORE_TEST_ROOT", "sim-data", 1);
    const int port = getenv("SIM_PORT") ? atoi(getenv("SIM_PORT")) : 8080;

    gClockSet = true;   // the Mac's clock is real; NTP would have set the device's

    // Same order as setup() in main.cpp.
    LittleFS.begin(true);
    if (storeBegin())
        Serial.printf("[store] ready: %u spools, %u log lines\n",
                      (unsigned)storeSpoolCount(), (unsigned)storeLogLineCount());
    cfgBegin();
    Serial.printf("[cfg] ready: %u vendors, %u materials, %u stock items\n",
                  (unsigned)cfgVendorCount(), (unsigned)cfgMaterialCount(),
                  (unsigned)cfgStockCount());
    apiKeyBegin();
    tzBegin();
    stationNameBegin();
    lastOnboardBegin();
    lastStockBegin();

    gState = DeviceState::Idle;
    webAppBegin();

    std::atomic<bool> running{true};
    std::thread ticker([&] {
        while (running) { simTick(); delay(500); }
    });

    httplib::Server svr;
    simRoutes(svr);
    simServeFirmware(svr);
    Serial.printf("[sim] data in %s\n", getenv("STORE_TEST_ROOT"));
    Serial.printf("[sim] web app:       http://localhost:%d/\n", port);
    Serial.printf("[sim] control panel: http://localhost:%d/_sim\n", port);
    fflush(stdout);
    bool ok = svr.listen("127.0.0.1", port);
    running = false;
    ticker.join();
    if (!ok) { fprintf(stderr, "[sim] could not listen on port %d\n", port); return 1; }
    return 0;
}
