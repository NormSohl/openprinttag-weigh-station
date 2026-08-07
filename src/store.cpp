#include "store.h"
#include "config.h"
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
static const char*        LOG_PATH    = "/log/events.ndjson";
static const char*        COMPACT_TMP = "/log/compact.staging";
static SemaphoreHandle_t  sMutex   = nullptr;
static std::vector<SpoolRecord>  sSpools;
static std::unordered_map<std::string, size_t> sByUuid;   // uuid → sSpools index (O(1) lookup)
static std::vector<MatInventory> sInv;
static std::vector<UsageRow>     sUsage;   // permanent consumption rollup
static bool               sInvDirty = true;   // rebuild sInv lazily on next query
static bool               sLogEndsNL = true;  // does the log end with a newline?
static bool               sWriteFailed = false; // last append was short/failed
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
        case StoreEv::Checkpoint:  return "checkpoint";
        case StoreEv::Usage:       return "usage";
        default:                   return "unknown";
    }
}
StoreEv storeEvFromName(const char* s) {
    if (!strcmp(s, "onboard"))      return StoreEv::Onboard;
    if (!strcmp(s, "weigh"))        return StoreEv::Weigh;
    if (!strcmp(s, "reconcile"))    return StoreEv::Reconcile;
    if (!strcmp(s, "reorder_flag")) return StoreEv::ReorderFlag;
    if (!strcmp(s, "export"))       return StoreEv::Export;
    if (!strcmp(s, "checkpoint"))   return StoreEv::Checkpoint;
    if (!strcmp(s, "usage"))        return StoreEv::Usage;
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

    // A Checkpoint is an Onboard and a Weigh rolled into one record, so it
    // emits both groups. Independent ifs, not if/else.
    if (e.ev == StoreEv::Weigh || e.ev == StoreEv::Checkpoint) {
        snprintf(n, sizeof(n), "%.1f", e.gross_g);     b += "\"gross_g\":";     b += n; b += ",";
        snprintf(n, sizeof(n), "%.1f", e.remaining_g); b += "\"remaining_g\":"; b += n; b += ",";
        snprintf(n, sizeof(n), "%.1f", e.used_g);      b += "\"used_g\":";      b += n; b += ",";
    }
    if (e.ev == StoreEv::Usage) {
        b += "\"vendor\":\""; b += jsonEsc(e.vendor);   b += "\",";
        b += "\"mat\":\"";    b += jsonEsc(e.material); b += "\",";
        snprintf(n, sizeof(n), "%.1f", e.usage_g);              b += "\"grams\":";  b += n; b += ",";
        snprintf(n, sizeof(n), "%u", (unsigned)e.usage_weighs); b += "\"weighs\":"; b += n; b += ",";
    }
    if (e.ev == StoreEv::Usage) {
        strlcpy(e.vendor,   doc["vendor"] | "", sizeof(e.vendor));
        strlcpy(e.material, doc["mat"]    | "", sizeof(e.material));
        e.usage_g      = doc["grams"]  | 0.0f;
        e.usage_weighs = doc["weighs"] | 0u;
    }
    if (e.ev == StoreEv::Onboard || e.ev == StoreEv::Reconcile ||
        e.ev == StoreEv::Checkpoint) {
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
    if (e.ev == StoreEv::Weigh || e.ev == StoreEv::Checkpoint) {
        e.gross_g     = doc["gross_g"]     | 0.0f;
        e.remaining_g = doc["remaining_g"] | 0.0f;
        e.used_g      = doc["used_g"]      | 0.0f;
    }
    if (e.ev == StoreEv::Onboard || e.ev == StoreEv::Reconcile ||
        e.ev == StoreEv::Checkpoint) {
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

// "2026-08-07T01:14:09Z" -> "2026-08". Anything unparseable buckets together
// rather than being dropped — an unattributed gram still happened.
static void periodOf_(const char* ts, char* out, size_t n) {
    if (ts && strlen(ts) >= 7 && ts[4] == '-') snprintf(out, n, "%.7s", ts);
    else                                       strlcpy(out, "unknown", n);
}

// Merge grams/weighs into the (period, vendor, material) bucket. The table is
// months x categories — order of 100 rows — so a linear scan is the right shape
// here, same as rebuildInventory_.
static void usageAdd_(std::vector<UsageRow>& tbl, const char* period,
                      const char* vendor, const char* material,
                      float grams, uint32_t weighs) {
    const char* mat = (material && material[0]) ? material : "(unspecified)";
    const char* ven = (vendor   && vendor[0])   ? vendor   : "(unspecified)";
    for (auto& u : tbl) {
        if (!strcmp(u.period, period) && !strcmp(u.vendor, ven) &&
            !strcmp(u.material, mat)) {
            u.grams += grams; u.weighs += weighs;
            return;
        }
    }
    UsageRow r;
    strlcpy(r.period,   period, sizeof(r.period));
    strlcpy(r.vendor,   ven,    sizeof(r.vendor));
    strlcpy(r.material, mat,    sizeof(r.material));
    r.grams = grams; r.weighs = weighs;
    tbl.push_back(r);
}

// Replay one event into an arbitrary index + usage table.
//
// Parameterised rather than hard-wired to the globals because storeCompact()
// needs a SECOND, independent fold covering only the events it is about to
// discard — see the comment there for why the latest state is the wrong thing
// to checkpoint.
static void applyInto_(std::vector<SpoolRecord>& spools,
                       std::unordered_map<std::string, size_t>& byUuid,
                       std::vector<UsageRow>& usage,
                       const StoreEvent& e) {
    // Usage rollups carry no spool identity — they ARE the folded history.
    if (e.ev == StoreEv::Usage) {
        char p[8];
        strlcpy(p, e.ts, sizeof(p));        // ts holds "YYYY-MM" for these
        usageAdd_(usage, p, e.vendor, e.material, e.usage_g, e.usage_weighs);
        return;
    }
    if (e.uuid[0] == 0) return;
    auto it = byUuid.find(e.uuid);
    int idx = (it == byUuid.end()) ? -1 : (int)it->second;
    if (idx < 0) {
        SpoolRecord r;
        strlcpy(r.uuid, e.uuid, sizeof(r.uuid));
        r.spool = e.spool;
        r.valid = true;
        spools.push_back(r);
        idx = (int)spools.size() - 1;
        byUuid[e.uuid] = (size_t)idx;    // vector indices are stable (append-only)
    }
    SpoolRecord& r = spools[idx];

    // Consumption = drop in remaining weight since this spool's previous
    // reading, and the record still HOLDS that previous reading until the
    // switch below overwrites it — so the delta must be taken here, before.
    //
    // Ignoring non-positive deltas covers three cases at once: sensor noise, a
    // spool that was refilled or swapped onto the same tag, and a spool's very
    // first weigh (previous remaining is 0, so the delta is negative). None of
    // those is filament anyone consumed.
    if (e.ev == StoreEv::Weigh) {
        const float delta = r.remaining_g - e.remaining_g;
        if (delta > 0.05f) {
            char p[8]; periodOf_(e.ts, p, sizeof(p));
            usageAdd_(usage, p, r.vendor, r.material, delta, 1);
        }
    }

    if (e.spool) r.spool = e.spool;
    strlcpy(r.last_ts, e.ts, sizeof(r.last_ts));
    switch (e.ev) {
        // A checkpoint is the folded state of everything that came before it,
        // so it sets both halves of the record.
        case StoreEv::Checkpoint:
            r.remaining_g = e.remaining_g;
            r.used_g      = e.used_g;
            // fall through
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

static void applyEvent_(const StoreEvent& e) {
    applyInto_(sSpools, sByUuid, sUsage, e);
    sInvDirty = true;                    // inventory recomputed lazily
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
// Caller holds the lock.
static bool rebuildIndices_() {
    sSpools.clear();
    sByUuid.clear();
    sUsage.clear();
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

bool storeRebuildIndices() { Lock lk; return rebuildIndices_(); }

// Does the log end with a newline? Caller holds the lock.
static void primeLogEndsNL_() {
    sLogEndsNL = true;
    File rf = LittleFS.open(LOG_PATH, "r");
    if (rf) {
        if (rf.size() > 0) { rf.seek(rf.size() - 1); if (rf.read() != '\n') sLogEndsNL = false; }
        rf.close();
    }
}

bool storeAppendEvent(const StoreEvent& in) {
    StoreEvent e = in;
    if (e.ts[0] == 0) storeNowIso(e.ts, sizeof(e.ts));
    String line = encodeLine(e);

    Lock lk;
    File f = LittleFS.open(LOG_PATH, "a");
    if (!f) { sWriteFailed = true; return false; }

    // Every byte is accounted for. A full LittleFS does not fail open() and
    // does not throw from print() — it just writes fewer bytes than asked. An
    // unchecked append therefore goes on reporting success while recording
    // nothing at all, which for a logbook is the worst possible failure mode.
    //
    // If the file ended without a newline (torn tail from a prior power cut),
    // start on a fresh line so our record stays parseable. Tracked in a flag
    // so we don't re-read the file on every append.
    size_t want = line.length() + 1;                    // line + '\n'
    size_t got  = 0;
    if (!sLogEndsNL) { want += 1; got += f.print('\n'); }
    got += f.print(line);
    got += f.print('\n');
    f.flush();
    f.close();

    if (got != want) {
        // A partial line may be on disk. Its CRC cannot match, so replay skips
        // it and the log stays parseable; flag the tail as unterminated so the
        // next append starts cleanly. Deliberately do NOT applyEvent_() — an
        // index that holds data the log does not would silently disagree with
        // itself after the next reboot.
        sWriteFailed = true;
        sLogEndsNL   = false;
        return false;
    }

    sWriteFailed = false;
    sLogEndsNL   = true;
    applyEvent_(e);   // only once the bytes are durably on disk
    return true;
}

// ── Health ────────────────────────────────────────────────────────────────────
bool   storeWriteFailed() { return sWriteFailed; }
size_t storeTotalBytes()  { return LittleFS.totalBytes(); }
size_t storeFreeBytes()   {
    size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
    return (total > used) ? total - used : 0;
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
size_t storeUsageCount() { Lock lk; return sUsage.size(); }
bool storeUsageAt(size_t idx, UsageRow& out) {
    Lock lk;
    if (idx >= sUsage.size()) return false;
    out = sUsage[idx];
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
    { Lock lk; primeLogEndsNL_(); }   // does the existing log end cleanly?
    return true;
}

// ── Per-spool weigh history (analytics) ───────────────────────────────────────
size_t storeForEachWeigh(uint32_t spool,
                         void (*cb)(const StoreEvent&, void*), void* ctx) {
    Lock lk;
    File f = LittleFS.open(LOG_PATH, "r");
    if (!f) return 0;
    size_t n = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) continue;
        StoreEvent e;
        if (decodeLine(line, e) && e.ev == StoreEv::Weigh && e.spool == spool) {
            if (cb) cb(e, ctx);
            n++;
        }
    }
    f.close();
    return n;
}

// ── Backup / restore (host export/import) ─────────────────────────────────────
const char* storeLogPath() { return LOG_PATH; }

bool storeImportLogFile(const char* stagingPath) {
    // 1) Validate staging first — never destroy a good log for a bad upload.
    uint32_t maxId = 0;
    size_t   good  = 0;
    {
        Lock lk;
        File f = LittleFS.open(stagingPath, "r");
        if (!f) return false;
        while (f.available()) {
            String line = f.readStringUntil('\n');
            line.trim();
            if (line.length() == 0) continue;
            StoreEvent e;
            if (decodeLine(line, e)) { good++; if (e.spool > maxId) maxId = e.spool; }
        }
        f.close();
    }
    if (good == 0) return false;

    // 2) Promote staging → live log (rename is cheap; byte-copy fallback).
    {
        Lock lk;
        LittleFS.remove(LOG_PATH);
        if (!LittleFS.rename(stagingPath, LOG_PATH)) {
            File in  = LittleFS.open(stagingPath, "r");
            File out = LittleFS.open(LOG_PATH, "w");
            if (in && out) {
                uint8_t buf[256];
                while (in.available()) { size_t n = in.read(buf, sizeof(buf)); out.write(buf, n); }
            }
            if (in)  in.close();
            if (out) out.close();
            LittleFS.remove(stagingPath);
        }
    }

    // 3) Rebuild indices (takes the lock itself) and re-prime the newline flag.
    storeRebuildIndices();
    {
        Lock lk;
        primeLogEndsNL_();
        sWriteFailed = false;
        // 4) Advance the ID counter past the highest imported id (no reuse).
        Preferences p;
        p.begin("store", false);
        uint32_t cur  = p.getUInt("counter", 1);
        uint32_t want = maxId + 1;
        if (want > cur) { p.putUInt("counter", want); sNextId = want; }
        else            { sNextId = cur; }
        p.end();
    }
    return true;
}

// ── Compaction ────────────────────────────────────────────────────────────────
bool storeCompactNeeded() {
    return storeLogBytes() > STORE_LOG_COMPACT_BYTES;   // takes the lock itself
}

// Rewrite the log as: the folded consumption rollup, one Checkpoint per spool,
// then the most recent STORE_LOG_KEEP_EVENTS lines verbatim.
//
// Everything is built in a separate staging file and promoted by rename, so a
// failure, a full disk, or a power cut at any point before the promote leaves
// the original log untouched.
bool storeCompact() {
    Lock lk;

    // 1) Count good lines to find the cutoff. Everything before the last
    //    STORE_LOG_KEEP_EVENTS is what the checkpoints will stand in for.
    size_t total = 0;
    {
        File f = LittleFS.open(LOG_PATH, "r");
        if (!f) return false;
        while (f.available()) {
            String l = f.readStringUntil('\n');
            l.trim();
            if (l.length()) total++;
        }
        f.close();
    }
    const size_t skip = (total > STORE_LOG_KEEP_EVENTS) ? total - STORE_LOG_KEEP_EVENTS : 0;
    if (skip == 0) return false;   // nothing old enough to be worth folding

    // 2) Replay ONLY the region being discarded, into its own index.
    //
    //    It would be cheaper to write checkpoints straight from sSpools, but
    //    that is wrong in a way that corrupts the usage totals: sSpools holds
    //    each spool's LATEST remaining weight, i.e. its state after the
    //    retained tail. A checkpoint carrying that value, followed by the tail
    //    replayed on top of it, would measure the tail's weigh deltas against
    //    the wrong baseline and double-count or lose consumption on every
    //    compaction. The checkpoint has to capture the fold boundary exactly.
    //
    //    Folding this region also produces the Usage rows that preserve what
    //    the discarded weigh events were evidence of. Pre-existing Usage rows
    //    in the region merge in naturally, so totals accumulate across repeated
    //    compactions instead of resetting.
    std::vector<SpoolRecord> foldSpools;
    std::unordered_map<std::string, size_t> foldByUuid;
    std::vector<UsageRow>    foldUsage;
    {
        File in = LittleFS.open(LOG_PATH, "r");
        if (!in) return false;
        size_t seen = 0;
        while (in.available() && seen < skip) {
            String l = in.readStringUntil('\n');
            l.trim();
            if (l.length() == 0) continue;
            seen++;
            StoreEvent e;
            if (decodeLine(l, e)) applyInto_(foldSpools, foldByUuid, foldUsage, e);
        }
        in.close();
    }

    // 3) Build the replacement.
    LittleFS.remove(COMPACT_TMP);
    File out = LittleFS.open(COMPACT_TMP, "w");
    if (!out) return false;
    bool ok = true;

    auto emit = [&out](const String& line) -> bool {
        return out.print(line) == line.length() && out.print('\n') == 1;
    };

    // Usage first: it is independent of spool state, and putting it at the head
    // keeps it obvious in an exported file.
    for (const auto& u : foldUsage) {
        StoreEvent x;
        x.ev = StoreEv::Usage;
        strlcpy(x.ts,       u.period,   sizeof(x.ts));
        strlcpy(x.vendor,   u.vendor,   sizeof(x.vendor));
        strlcpy(x.material, u.material, sizeof(x.material));
        x.usage_g      = u.grams;
        x.usage_weighs = u.weighs;
        if (!emit(encodeLine(x))) { ok = false; break; }
    }

    // Then one checkpoint per spool, at the fold boundary.
    if (ok) for (const auto& r : foldSpools) {
        if (!r.valid || r.uuid[0] == 0) continue;
        StoreEvent c;
        c.ev = StoreEv::Checkpoint;
        strlcpy(c.ts, r.last_ts[0] ? r.last_ts : "1970-01-01T00:00:00Z", sizeof(c.ts));
        strlcpy(c.uuid, r.uuid, sizeof(c.uuid));
        c.spool       = r.spool;
        c.remaining_g = r.remaining_g;
        c.used_g      = r.used_g;
        strlcpy(c.vendor,   r.vendor,   sizeof(c.vendor));
        strlcpy(c.material, r.material, sizeof(c.material));
        strlcpy(c.abbr,     r.abbr,     sizeof(c.abbr));
        memcpy(c.rgba, r.rgba, 4);
        c.dia = r.dia; c.empty_g = r.empty_g; c.nom_g = r.nom_g;
        c.needs_ob = r.needs_ob;
        if (!emit(encodeLine(c))) { ok = false; break; }
    }

    // Then the retained tail, byte for byte.
    if (ok) {
        File in = LittleFS.open(LOG_PATH, "r");
        if (!in) ok = false;
        else {
            size_t seen = 0;
            while (in.available()) {
                String l = in.readStringUntil('\n');
                l.trim();
                if (l.length() == 0) continue;
                if (seen++ < skip) continue;      // folded above
                if (!emit(l)) { ok = false; break; }
            }
            in.close();
        }
    }
    out.flush();
    out.close();

    if (!ok) {                       // short write => disk full mid-compaction
        LittleFS.remove(COMPACT_TMP);
        sWriteFailed = true;
        return false;
    }

    // 4) Promote. Try an atomic replace first; only fall back to
    //    remove-then-rename if the filesystem refuses to overwrite.
    if (!LittleFS.rename(COMPACT_TMP, LOG_PATH)) {
        LittleFS.remove(LOG_PATH);
        if (!LittleFS.rename(COMPACT_TMP, LOG_PATH)) {
            LittleFS.remove(COMPACT_TMP);
            rebuildIndices_();       // resync to whatever is actually on disk
            primeLogEndsNL_();
            sWriteFailed = true;
            return false;
        }
    }

    rebuildIndices_();
    primeLogEndsNL_();
    sWriteFailed = false;
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
        } else if (what == "usage") {
            Serial.printf("[store] usage (%u buckets):\n", (unsigned)storeUsageCount());
            UsageRow u;
            float total = 0;
            for (size_t i = 0; i < storeUsageCount(); i++)
                if (storeUsageAt(i, u)) {
                    Serial.printf("  %-8s %-12s %-10s %9.1f g  (%u weighs)\n",
                                  u.period, u.vendor, u.material, u.grams,
                                  (unsigned)u.weighs);
                    total += u.grams;
                }
            Serial.printf("  total %.1f g (%.2f kg)\n", total, total / 1000.0f);
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
        Serial.printf("[store] fs: %u kB free / %u kB%s%s\n",
                      (unsigned)(storeFreeBytes() >> 10), (unsigned)(storeTotalBytes() >> 10),
                      storeCompactNeeded() ? "  [compaction due]" : "",
                      storeWriteFailed()   ? "  [WRITES FAILING]" : "");
        Serial.printf("[store] usage: %u buckets\n", (unsigned)storeUsageCount());
        return true;
    }
    // Force a compaction regardless of the size threshold. The point is to make
    // the fold testable on the bench — SEED a few thousand events, DUMP usage,
    // COMPACT, DUMP usage again: the totals must be identical, and the spool
    // records unchanged. Waiting for the log to reach 900 kB naturally is not a
    // test anyone will run.
    if (cmd == "COMPACT") {
        const size_t before = storeLogBytes(), lines = storeLogLineCount();
        const bool ok = storeCompact();
        Serial.printf("[store] compact %s: %u -> %u bytes, %u -> %u lines, "
                      "%u usage buckets\n",
                      ok ? "ok" : "skipped/failed",
                      (unsigned)before, (unsigned)storeLogBytes(),
                      (unsigned)lines, (unsigned)storeLogLineCount(),
                      (unsigned)storeUsageCount());
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
