#include "store.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>
#include <string.h>
#include <vector>
#include <unordered_map>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ── State ─────────────────────────────────────────────────────────────────────
static const char*        LOG_PATH = "/log/events.ndjson";
static SemaphoreHandle_t  sMutex   = nullptr;
static std::vector<SpoolRecord>  sSpools;
static std::unordered_map<std::string, size_t> sByUuid;   // uuid → sSpools index (O(1) lookup)
static std::vector<MatInventory> sInv;
static bool               sInvDirty = true;   // rebuild sInv lazily on next query
static bool               sLogEndsNL = true;  // does the log end with a newline?
static uint32_t           sNextId  = 1;

// RAII lock. No public function that takes the lock calls another that does,
// so a plain (non-recursive) mutex is safe.
struct Lock {
    Lock()  { if (sMutex) xSemaphoreTake(sMutex, portMAX_DELAY); }
    ~Lock() { if (sMutex) xSemaphoreGive(sMutex); }
};

// ── Event-name mapping ────────────────────────────────────────────────────────
const char* storeEvName(StoreEv ev) {
    switch (ev) {
        case StoreEv::Onboard:     return "onboard";
        case StoreEv::Weigh:       return "weigh";
        case StoreEv::Reconcile:   return "reconcile";
        case StoreEv::ReorderFlag: return "reorder_flag";
        case StoreEv::Export:      return "export";
        default:                   return "unknown";
    }
}
StoreEv storeEvFromName(const char* s) {
    if (!strcmp(s, "onboard"))      return StoreEv::Onboard;
    if (!strcmp(s, "weigh"))        return StoreEv::Weigh;
    if (!strcmp(s, "reconcile"))    return StoreEv::Reconcile;
    if (!strcmp(s, "reorder_flag")) return StoreEv::ReorderFlag;
    if (!strcmp(s, "export"))       return StoreEv::Export;
    return StoreEv::Unknown;
}

// ── CRC-32 (IEEE 802.3, reflected, poly 0xEDB88320) ───────────────────────────
uint32_t storeCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return ~crc;
}

// ── Time ──────────────────────────────────────────────────────────────────────
void storeNowIso(char* buf, size_t buflen) {
    time_t now = time(nullptr);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(buf, buflen, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

// ── Line codec ────────────────────────────────────────────────────────────────
static String jsonEsc(const char* s) {
    String o;
    for (const char* p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') o += '\\';
        o += *p;
    }
    return o;
}

// Body = the object up to and INCLUDING the comma before "crc" (the CRC input).
static String encodeBody(const StoreEvent& e) {
    char n[40];
    String b = "{";
    b += "\"ts\":\"";   b += e.ts;               b += "\",";
    b += "\"ev\":\"";   b += storeEvName(e.ev);  b += "\",";
    b += "\"uuid\":\""; b += e.uuid;             b += "\",";
    snprintf(n, sizeof(n), "%u", (unsigned)e.spool);
    b += "\"spool\":"; b += n; b += ",";

    if (e.ev == StoreEv::Weigh) {
        snprintf(n, sizeof(n), "%.1f", e.gross_g);     b += "\"gross_g\":";     b += n; b += ",";
        snprintf(n, sizeof(n), "%.1f", e.remaining_g); b += "\"remaining_g\":"; b += n; b += ",";
        snprintf(n, sizeof(n), "%.1f", e.used_g);      b += "\"used_g\":";      b += n; b += ",";
    } else if (e.ev == StoreEv::Onboard || e.ev == StoreEv::Reconcile) {
        b += "\"vendor\":\""; b += jsonEsc(e.vendor);   b += "\",";
        b += "\"mat\":\"";    b += jsonEsc(e.material);  b += "\",";
        b += "\"abbr\":\"";   b += jsonEsc(e.abbr);      b += "\",";
        snprintf(n, sizeof(n), "[%u,%u,%u,%u]", e.rgba[0], e.rgba[1], e.rgba[2], e.rgba[3]);
        b += "\"rgba\":"; b += n; b += ",";
        snprintf(n, sizeof(n), "%.2f", e.dia);     b += "\"dia\":";     b += n; b += ",";
        snprintf(n, sizeof(n), "%.1f", e.empty_g); b += "\"empty_g\":"; b += n; b += ",";
        snprintf(n, sizeof(n), "%.1f", e.nom_g);   b += "\"nom_g\":";   b += n; b += ",";
        b += "\"needs_ob\":"; b += (e.needs_ob ? "true" : "false"); b += ",";
    }
    return b;   // always ends with ','
}

static String encodeLine(const StoreEvent& e) {
    String body = encodeBody(e);
    uint32_t crc = storeCrc32((const uint8_t*)body.c_str(), body.length());
    char tail[20];
    snprintf(tail, sizeof(tail), "\"crc\":\"%08x\"}", (unsigned)crc);
    return body + tail;
}

// Parse + CRC-verify one line. Returns false on any corruption (caller skips).
static bool decodeLine(const String& line, StoreEvent& e) {
    int pos = line.indexOf("\"crc\":\"");
    if (pos < 0) return false;
    String body = line.substring(0, pos);           // ends with ',' by construction
    int hs = pos + 7;
    int he = line.indexOf('"', hs);
    if (he < 0) return false;
    uint32_t want = (uint32_t)strtoul(line.substring(hs, he).c_str(), nullptr, 16);
    uint32_t got  = storeCrc32((const uint8_t*)body.c_str(), body.length());
    if (want != got) return false;

    JsonDocument doc;
    if (deserializeJson(doc, line)) return false;
    e = StoreEvent{};
    e.ev = storeEvFromName(doc["ev"] | "unknown");
    strlcpy(e.ts,   doc["ts"]   | "", sizeof(e.ts));
    strlcpy(e.uuid, doc["uuid"] | "", sizeof(e.uuid));
    e.spool = doc["spool"] | 0u;
    if (e.ev == StoreEv::Weigh) {
        e.gross_g     = doc["gross_g"]     | 0.0f;
        e.remaining_g = doc["remaining_g"] | 0.0f;
        e.used_g      = doc["used_g"]      | 0.0f;
    } else if (e.ev == StoreEv::Onboard || e.ev == StoreEv::Reconcile) {
        strlcpy(e.vendor,   doc["vendor"] | "", sizeof(e.vendor));
        strlcpy(e.material, doc["mat"]    | "", sizeof(e.material));
        strlcpy(e.abbr,     doc["abbr"]   | "", sizeof(e.abbr));
        for (int i = 0; i < 4; i++) e.rgba[i] = doc["rgba"][i] | 0;
        e.dia      = doc["dia"]     | 0.0f;
        e.empty_g  = doc["empty_g"] | 0.0f;
        e.nom_g    = doc["nom_g"]   | 0.0f;
        e.needs_ob = doc["needs_ob"] | false;
    }
    return true;
}

// ── Index maintenance (helpers assume the caller holds the lock) ──────────────
static int findByUuid_(const char* uuid) {
    auto it = sByUuid.find(uuid);
    return (it == sByUuid.end()) ? -1 : (int)it->second;
}

static void applyEvent_(const StoreEvent& e) {
    if (e.uuid[0] == 0) return;
    int idx = findByUuid_(e.uuid);
    if (idx < 0) {
        SpoolRecord r;
        strlcpy(r.uuid, e.uuid, sizeof(r.uuid));
        r.spool = e.spool;
        r.valid = true;
        sSpools.push_back(r);
        idx = (int)sSpools.size() - 1;
        sByUuid[e.uuid] = (size_t)idx;   // vector indices are stable (append-only)
    }
    sInvDirty = true;                    // inventory recomputed lazily
    SpoolRecord& r = sSpools[idx];
    if (e.spool) r.spool = e.spool;
    strlcpy(r.last_ts, e.ts, sizeof(r.last_ts));
    switch (e.ev) {
        case StoreEv::Onboard:
        case StoreEv::Reconcile:
            strlcpy(r.vendor,   e.vendor,   sizeof(r.vendor));
            strlcpy(r.material, e.material, sizeof(r.material));
            strlcpy(r.abbr,     e.abbr,     sizeof(r.abbr));
            memcpy(r.rgba, e.rgba, 4);
            r.dia = e.dia; r.empty_g = e.empty_g; r.nom_g = e.nom_g;
            r.needs_ob = e.needs_ob;
            break;
        case StoreEv::Weigh:
            r.remaining_g = e.remaining_g;
            r.used_g      = e.used_g;
            break;
        default:
            break;
    }
}

static void rebuildInventory_() {
    sInv.clear();
    // Materials are few (dozens); linear bucket search is fine.
    for (auto& r : sSpools) {
        if (!r.valid || r.material[0] == 0) continue;
        int mi = -1;
        for (size_t i = 0; i < sInv.size(); i++)
            if (!strcmp(sInv[i].material, r.material)) { mi = (int)i; break; }
        if (mi < 0) {
            MatInventory m;
            strlcpy(m.material, r.material, sizeof(m.material));
            sInv.push_back(m);
            mi = (int)sInv.size() - 1;
        }
        sInv[mi].remaining_g += r.remaining_g;
        if (r.remaining_g > 1.0f) sInv[mi].count++;
    }
}

static void ensureInventory_() {
    if (sInvDirty) { rebuildInventory_(); sInvDirty = false; }
}

// ── Log ───────────────────────────────────────────────────────────────────────
bool storeRebuildIndices() {
    Lock lk;
    sSpools.clear();
    sByUuid.clear();
    File f = LittleFS.open(LOG_PATH, "r");
    if (f) {
        while (f.available()) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) continue;
            StoreEvent e;
            if (decodeLine(line, e)) applyEvent_(e);
            // else: torn/corrupt line — skip, keep going
        }
        f.close();
    }
    sInvDirty = true;   // inventory rebuilt lazily on next query
    return true;
}

bool storeAppendEvent(const StoreEvent& in) {
    StoreEvent e = in;
    if (e.ts[0] == 0) storeNowIso(e.ts, sizeof(e.ts));
    String line = encodeLine(e);

    Lock lk;
    File f = LittleFS.open(LOG_PATH, "a");
    if (!f) return false;
    // If the file ended without a newline (torn tail from a prior power cut),
    // start on a fresh line so our record stays parseable. Tracked in a flag
    // so we don't re-read the file on every append.
    if (!sLogEndsNL) f.print('\n');
    f.print(line);
    f.print('\n');
    f.flush();
    f.close();
    sLogEndsNL = true;

    applyEvent_(e);   // marks inventory dirty; rebuilt lazily on next query
    return true;
}

size_t storeLogBytes() {
    Lock lk;
    File f = LittleFS.open(LOG_PATH, "r");
    size_t n = f ? f.size() : 0;
    if (f) f.close();
    return n;
}

size_t storeLogLineCount() {
    Lock lk;
    File f = LittleFS.open(LOG_PATH, "r");
    if (!f) return 0;
    size_t n = 0;
    while (f.available()) {
        String l = f.readStringUntil('\n');
        l.trim();
        if (l.length()) n++;
    }
    f.close();
    return n;
}

// ── Spool-ID counter (NVS) ────────────────────────────────────────────────────
uint32_t storeNextSpoolId() {
    Lock lk;
    Preferences p;
    p.begin("store", false);
    uint32_t id = p.getUInt("counter", 1);
    p.putUInt("counter", id + 1);
    p.end();
    sNextId = id + 1;
    return id;
}
uint32_t storePeekSpoolId() { return sNextId; }

// ── Queries ───────────────────────────────────────────────────────────────────
bool storeGetSpool(uint32_t id, SpoolRecord& out) {
    Lock lk;
    for (auto& r : sSpools) if (r.spool == id) { out = r; return true; }
    return false;
}
bool storeFindByUuid(const char* uuid, SpoolRecord& out) {
    Lock lk;
    int i = findByUuid_(uuid);
    if (i < 0) return false;
    out = sSpools[i];
    return true;
}
size_t storeSpoolCount() { Lock lk; return sSpools.size(); }
bool storeSpoolAt(size_t idx, SpoolRecord& out) {
    Lock lk;
    if (idx >= sSpools.size()) return false;
    out = sSpools[idx];
    return true;
}
size_t storeInventoryCount() { Lock lk; ensureInventory_(); return sInv.size(); }
bool storeInventoryAt(size_t idx, MatInventory& out) {
    Lock lk;
    ensureInventory_();
    if (idx >= sInv.size()) return false;
    out = sInv[idx];
    return true;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────────
bool storeBegin() {
    if (!sMutex) sMutex = xSemaphoreCreateMutex();
    if (!LittleFS.begin(true)) return false;   // format on first-boot mount fail
    if (!LittleFS.exists("/log")) LittleFS.mkdir("/log");
    Preferences p;
    p.begin("store", true);
    sNextId = p.getUInt("counter", 1);
    p.end();
    storeRebuildIndices();
    // Prime the newline flag: does the existing log end cleanly?
    sLogEndsNL = true;
    File rf = LittleFS.open(LOG_PATH, "r");
    if (rf) {
        if (rf.size() > 0) { rf.seek(rf.size() - 1); if (rf.read() != '\n') sLogEndsNL = false; }
        rf.close();
    }
    return true;
}

// ── Serial test harness ───────────────────────────────────────────────────────
static String tok(const String& s, int& pos) {
    while (pos < (int)s.length() && s[pos] == ' ') pos++;
    int start = pos;
    while (pos < (int)s.length() && s[pos] != ' ') pos++;
    return s.substring(start, pos);
}

bool storeSerialCommand(const String& lineIn) {
    String line = lineIn; line.trim();
    int pos = 0;
    String cmd = tok(line, pos); cmd.toUpperCase();

    if (cmd == "EV") {
        String sub = tok(line, pos);
        if (sub == "onboard") {
            StoreEvent e; e.ev = StoreEv::Onboard;
            strlcpy(e.uuid,     tok(line, pos).c_str(), sizeof(e.uuid));
            strlcpy(e.vendor,   tok(line, pos).c_str(), sizeof(e.vendor));
            strlcpy(e.material, tok(line, pos).c_str(), sizeof(e.material));
            e.needs_ob = true; e.dia = 1.75f; e.nom_g = 1000.0f; e.empty_g = 200.0f;
            e.spool = storeNextSpoolId();
            storeAppendEvent(e);
            Serial.printf("[store] onboard spool #%u uuid %s\n", (unsigned)e.spool, e.uuid);
        } else if (sub == "weigh") {
            StoreEvent e; e.ev = StoreEv::Weigh;
            strlcpy(e.uuid, tok(line, pos).c_str(), sizeof(e.uuid));
            e.gross_g = tok(line, pos).toFloat();
            SpoolRecord r;
            if (storeFindByUuid(e.uuid, r)) {
                e.spool = r.spool;
                e.remaining_g = (r.empty_g > 0) ? e.gross_g - r.empty_g : e.gross_g;
                if (e.remaining_g < 0) e.remaining_g = 0;
                e.used_g = (r.nom_g > 0) ? r.nom_g - e.remaining_g : 0;
                if (e.used_g < 0) e.used_g = 0;
            } else {
                e.remaining_g = e.gross_g;
            }
            storeAppendEvent(e);
            Serial.printf("[store] weigh uuid %s gross %.1f -> rem %.1f\n",
                          e.uuid, e.gross_g, e.remaining_g);
        } else {
            Serial.println("[store] usage: EV onboard <uuid> <vendor> <material> | EV weigh <uuid> <gross_g>");
        }
        return true;
    }

    if (cmd == "DUMP") {
        String what = tok(line, pos);
        if (what == "inv") {
            Serial.printf("[store] inventory (%u materials):\n", (unsigned)storeInventoryCount());
            MatInventory m;
            for (size_t i = 0; i < storeInventoryCount(); i++)
                if (storeInventoryAt(i, m))
                    Serial.printf("  %-16s %8.1f g  (%u spools)\n", m.material, m.remaining_g, m.count);
        } else {
            Serial.printf("[store] spools (%u):\n", (unsigned)storeSpoolCount());
            SpoolRecord r;
            for (size_t i = 0; i < storeSpoolCount(); i++)
                if (storeSpoolAt(i, r))
                    Serial.printf("  #%-4u %-10s %-8s rem %8.1f g %s uuid %s\n",
                                  (unsigned)r.spool, r.vendor, r.material, r.remaining_g,
                                  r.needs_ob ? "[needs-ob]" : "", r.uuid);
        }
        return true;
    }

    if (cmd == "REBUILD") {
        storeRebuildIndices();
        Serial.println("[store] indices rebuilt from log");
        return true;
    }
    if (cmd == "LOGSTATS") {
        Serial.printf("[store] log: %u lines, %u bytes, next id #%u\n",
                      (unsigned)storeLogLineCount(), (unsigned)storeLogBytes(),
                      (unsigned)storePeekSpoolId());
        return true;
    }
    if (cmd == "TORN") {
        File f = LittleFS.open(LOG_PATH, "a");
        if (f) {
            // A crc-less, newline-less partial — simulates a power cut mid-write.
            f.print("{\"ts\":\"2026-01-01T00:00:00Z\",\"ev\":\"weigh\",\"uuid\":\"deadbeef\",\"spool\":999,\"gross_g\":123.4");
            f.close();
        }
        sLogEndsNL = false;   // next append will heal it with a leading newline
        Serial.println("[store] appended a torn (crc-less, newline-less) tail line");
        return true;
    }
    if (cmd == "WIPE") {
        LittleFS.remove(LOG_PATH);
        sLogEndsNL = true;
        storeRebuildIndices();
        Serial.println("[store] log wiped");
        return true;
    }
    if (cmd == "SEED") {
        uint32_t nsp = (uint32_t)tok(line, pos).toInt();
        uint32_t nev = (uint32_t)tok(line, pos).toInt();
        if (nev == 0) nev = 5;
        const char* vends[] = {"Prusament", "Hatchbox", "Overture", "Generic"};
        const char* mats[]  = {"PLA", "PETG", "ASA", "TPU"};
        uint32_t t0 = millis();
        for (uint32_t i = 0; i < nsp; i++) {
            char uuid[33];
            snprintf(uuid, sizeof(uuid), "%08x%08x%08x%08x", 0x5EEDu, 0u, 0u, (unsigned)i);
            StoreEvent e; e.ev = StoreEv::Onboard;
            strlcpy(e.uuid, uuid, sizeof(e.uuid));
            strlcpy(e.vendor, vends[i & 3], sizeof(e.vendor));
            strlcpy(e.material, mats[i & 3], sizeof(e.material));
            e.dia = 1.75f; e.nom_g = 1000.0f; e.empty_g = 200.0f; e.needs_ob = false;
            e.spool = storeNextSpoolId();
            storeAppendEvent(e);
            for (uint32_t j = 0; j < nev; j++) {
                StoreEvent w; w.ev = StoreEv::Weigh;
                strlcpy(w.uuid, uuid, sizeof(w.uuid));
                w.spool = e.spool;
                w.gross_g = 1200.0f - (float)j * (900.0f / (float)nev);
                w.remaining_g = w.gross_g - 200.0f;
                w.used_g = 1000.0f - w.remaining_g;
                storeAppendEvent(w);
            }
        }
        Serial.printf("[store] SEED %u spools x %u events done in %u ms — now %u lines / %u bytes, %u spools\n",
                      (unsigned)nsp, (unsigned)nev, (unsigned)(millis() - t0),
                      (unsigned)storeLogLineCount(), (unsigned)storeLogBytes(),
                      (unsigned)storeSpoolCount());
        return true;
    }
    return false;
}
