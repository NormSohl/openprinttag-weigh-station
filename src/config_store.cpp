#include "config_store.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <vector>
#include <string.h>

// ── Tables (in RAM) ───────────────────────────────────────────────────────────
static std::vector<String>      sVendors;
static std::vector<CfgMaterial> sMaterials;
static std::vector<CfgProfile>  sProfiles;
static std::vector<CfgColor>    sColors;
static std::vector<CfgStock>    sStock;

#define P_VENDORS  "/config/vendors.json"
#define P_MATS     "/config/materials.json"
#define P_PROFILES "/config/spool-profiles.json"
#define P_COLORS   "/config/colors.json"
#define P_STOCK    "/config/stock-items.json"

// ── File helpers ──────────────────────────────────────────────────────────────
static bool readDoc(const char* path, JsonDocument& doc) {
    File f = LittleFS.open(path, "r");
    if (!f) return false;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    return !err;
}
static bool writeStr(const char* path, const String& s) {
    File f = LittleFS.open(path, "w");
    if (!f) return false;
    f.print(s);
    f.close();
    return true;
}

// ── Per-table parse / serialize ───────────────────────────────────────────────
static void parseVendors(JsonArrayConst a) {
    sVendors.clear();
    for (JsonVariantConst v : a) sVendors.push_back(String((const char*)(v | "")));
}
static String serVendors() {
    JsonDocument d; JsonArray a = d.to<JsonArray>();
    for (auto& v : sVendors) a.add(v);
    String s; serializeJson(d, s); return s;
}

static void parseMaterials(JsonArrayConst arr) {
    sMaterials.clear();
    for (JsonObjectConst o : arr) {
        CfgMaterial m{};
        strlcpy(m.name, o["name"] | "", sizeof(m.name));
        strlcpy(m.abbr, o["abbr"] | "", sizeof(m.abbr));
        m.cls = o["cls"] | 0; m.type = o["type"] | 0;
        m.dia = o["dia"] | 1.75f;
        m.print_min = o["print_min"] | 0; m.print_max = o["print_max"] | 0;
        m.bed_min = o["bed_min"] | 0; m.bed_max = o["bed_max"] | 0;
        sMaterials.push_back(m);
    }
}
static String serMaterials() {
    JsonDocument d; JsonArray a = d.to<JsonArray>();
    for (auto& m : sMaterials) {
        JsonObject o = a.add<JsonObject>();
        o["name"] = m.name; o["abbr"] = m.abbr; o["cls"] = m.cls; o["type"] = m.type;
        o["dia"] = m.dia;
        o["print_min"] = m.print_min; o["print_max"] = m.print_max;
        o["bed_min"] = m.bed_min; o["bed_max"] = m.bed_max;
    }
    String s; serializeJson(d, s); return s;
}

static void parseProfiles(JsonArrayConst arr) {
    sProfiles.clear();
    for (JsonObjectConst o : arr) {
        CfgProfile p{};
        strlcpy(p.label, o["label"] | "", sizeof(p.label));
        p.nominal_full_g = o["nominal_full_g"] | 0.0f;
        p.empty_g = o["empty_g"] | 0.0f;
        sProfiles.push_back(p);
    }
}
static String serProfiles() {
    JsonDocument d; JsonArray a = d.to<JsonArray>();
    for (auto& p : sProfiles) {
        JsonObject o = a.add<JsonObject>();
        o["label"] = p.label; o["nominal_full_g"] = p.nominal_full_g; o["empty_g"] = p.empty_g;
    }
    String s; serializeJson(d, s); return s;
}

static void parseColors(JsonArrayConst arr) {
    sColors.clear();
    for (JsonObjectConst o : arr) {
        CfgColor c{};
        strlcpy(c.name, o["name"] | "", sizeof(c.name));
        for (int i = 0; i < 4; i++) c.rgba[i] = o["rgba"][i] | 0;
        sColors.push_back(c);
    }
}
static String serColors() {
    JsonDocument d; JsonArray a = d.to<JsonArray>();
    for (auto& c : sColors) {
        JsonObject o = a.add<JsonObject>();
        o["name"] = c.name;
        JsonArray r = o["rgba"].to<JsonArray>();
        for (int i = 0; i < 4; i++) r.add(c.rgba[i]);
    }
    String s; serializeJson(d, s); return s;
}

static void parseStock(JsonArrayConst arr) {
    sStock.clear();
    for (JsonObjectConst o : arr) {
        CfgStock s{};
        strlcpy(s.vendor,   o["vendor"]   | "", sizeof(s.vendor));
        strlcpy(s.material, o["material"] | "", sizeof(s.material));
        strlcpy(s.color,    o["color"]    | "", sizeof(s.color));
        s.dia = o["dia"] | 1.75f;
        s.spool_g = o["spool_g"] | 1000.0f;
        s.min_spools = o["min_spools"] | 0;
        s.min_grams  = o["min_grams"]  | 0.0f;
        strlcpy(s.sku,  o["sku"]  | "", sizeof(s.sku));
        strlcpy(s.gtin, o["gtin"] | "", sizeof(s.gtin));
        s.pack_qty = o["pack_qty"] | 1;
        sStock.push_back(s);
    }
}
static String serStock() {
    JsonDocument d; JsonArray a = d.to<JsonArray>();
    for (auto& s : sStock) {
        JsonObject o = a.add<JsonObject>();
        o["vendor"] = s.vendor; o["material"] = s.material; o["color"] = s.color;
        o["dia"] = s.dia; o["spool_g"] = s.spool_g;
        o["min_spools"] = s.min_spools; o["min_grams"] = s.min_grams;
        o["sku"] = s.sku; o["gtin"] = s.gtin; o["pack_qty"] = s.pack_qty;
    }
    String s; serializeJson(d, s); return s;
}

// ── Defaults ──────────────────────────────────────────────────────────────────
static void seedVendors() {
    sVendors = {"Prusament", "Hatchbox", "Overture", "Polymaker", "Generic"};
}
// Explicit builders — GCC 8 (esp32 core) rejects push_back({...}) for these
// aggregates (char arrays / nested array members).
static void addMat(const char* name, const char* abbr, float dia,
                   int16_t pmin, int16_t pmax, int16_t bmin, int16_t bmax) {
    CfgMaterial m{};
    strlcpy(m.name, name, sizeof(m.name));
    strlcpy(m.abbr, abbr, sizeof(m.abbr));
    m.dia = dia;
    m.print_min = pmin; m.print_max = pmax;
    m.bed_min = bmin;   m.bed_max = bmax;
    sMaterials.push_back(m);
}
static void addProf(const char* label, float full, float empty) {
    CfgProfile p{};
    strlcpy(p.label, label, sizeof(p.label));
    p.nominal_full_g = full; p.empty_g = empty;
    sProfiles.push_back(p);
}
static void addColor(const char* name, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    CfgColor c{};
    strlcpy(c.name, name, sizeof(c.name));
    c.rgba[0] = r; c.rgba[1] = g; c.rgba[2] = b; c.rgba[3] = a;
    sColors.push_back(c);
}

static void seedMaterials() {
    sMaterials.clear();
    addMat("PLA",  "PLA",  1.75f, 190, 220, 50, 60);
    addMat("PETG", "PETG", 1.75f, 230, 250, 70, 85);
    addMat("ASA",  "ASA",  1.75f, 240, 260, 90, 110);
    addMat("ABS",  "ABS",  1.75f, 230, 250, 90, 110);
    addMat("TPU",  "TPU",  1.75f, 210, 230, 30, 50);
}
static void seedProfiles() {
    sProfiles.clear();
    addProf("Prusament 1kg",  1000.0f, 201.0f);
    addProf("Generic 1kg",    1000.0f, 200.0f);
    addProf("Generic 0.75kg",  750.0f, 175.0f);
    addProf("Generic 0.5kg",   500.0f, 150.0f);
}
static void seedColors() {
    sColors.clear();
    addColor("Black",        30,  30,  30,  255);
    addColor("White",        245, 245, 245, 255);
    addColor("Prusa Orange", 227, 111, 17,  255);
    addColor("Red",          200, 30,  30,  255);
    addColor("Green",        40,  160, 60,  255);
    addColor("Blue",         40,  80,  200, 255);
    addColor("Grey",         130, 130, 130, 255);
}
static void seedStock() { sStock.clear(); }   // empty — user defines standard stock

// ── Lifecycle ─────────────────────────────────────────────────────────────────
bool cfgBegin() {
    if (!LittleFS.exists("/config")) LittleFS.mkdir("/config");
    JsonDocument d;
    if (readDoc(P_VENDORS, d))  parseVendors(d.as<JsonArrayConst>());  else { seedVendors();  writeStr(P_VENDORS,  serVendors()); }
    d.clear(); if (readDoc(P_MATS, d))     parseMaterials(d.as<JsonArrayConst>()); else { seedMaterials(); writeStr(P_MATS, serMaterials()); }
    d.clear(); if (readDoc(P_PROFILES, d)) parseProfiles(d.as<JsonArrayConst>());  else { seedProfiles();  writeStr(P_PROFILES, serProfiles()); }
    d.clear(); if (readDoc(P_COLORS, d))   parseColors(d.as<JsonArrayConst>());    else { seedColors();   writeStr(P_COLORS, serColors()); }
    d.clear(); if (readDoc(P_STOCK, d))    parseStock(d.as<JsonArrayConst>());     else { seedStock();    writeStr(P_STOCK, serStock()); }
    return true;
}

// ── Read / iterate ────────────────────────────────────────────────────────────
size_t cfgVendorCount() { return sVendors.size(); }
bool cfgVendorAt(size_t i, char* out, size_t outlen) {
    if (i >= sVendors.size()) return false;
    strlcpy(out, sVendors[i].c_str(), outlen);
    return true;
}
size_t cfgMaterialCount() { return sMaterials.size(); }
bool cfgMaterialAt(size_t i, CfgMaterial& out) {
    if (i >= sMaterials.size()) return false;
    out = sMaterials[i]; return true;
}
bool cfgMaterialByName(const char* name, CfgMaterial& out) {
    for (auto& m : sMaterials) if (!strcasecmp(m.name, name)) { out = m; return true; }
    return false;
}
size_t cfgProfileCount() { return sProfiles.size(); }
bool cfgProfileAt(size_t i, CfgProfile& out) {
    if (i >= sProfiles.size()) return false;
    out = sProfiles[i]; return true;
}
size_t cfgColorCount() { return sColors.size(); }
bool cfgColorAt(size_t i, CfgColor& out) {
    if (i >= sColors.size()) return false;
    out = sColors[i]; return true;
}
bool cfgColorByName(const char* name, CfgColor& out) {
    for (auto& c : sColors) if (!strcasecmp(c.name, name)) { out = c; return true; }
    return false;
}
size_t cfgStockCount() { return sStock.size(); }
bool cfgStockAt(size_t i, CfgStock& out) {
    if (i >= sStock.size()) return false;
    out = sStock[i]; return true;
}

// ── Mutation ──────────────────────────────────────────────────────────────────
bool cfgVendorAdd(const char* name) {
    if (!name || !name[0]) return false;
    for (auto& v : sVendors) if (v.equalsIgnoreCase(name)) return false;
    sVendors.push_back(String(name));
    return cfgSave("vendors");
}
bool cfgProfileAdd(const CfgProfile& p) { sProfiles.push_back(p); return cfgSave("spool-profiles"); }
bool cfgMaterialAdd(const CfgMaterial& m) { sMaterials.push_back(m); return cfgSave("materials"); }
bool cfgColorAdd(const CfgColor& c) { sColors.push_back(c); return cfgSave("colors"); }
bool cfgStockAdd(const CfgStock& s) { sStock.push_back(s); return cfgSave("stock-items"); }

// ── Save / web JSON ───────────────────────────────────────────────────────────
static bool matchWhich(const char* which, const char* a, const char* b = nullptr) {
    return !strcasecmp(which, a) || (b && !strcasecmp(which, b));
}
bool cfgSave(const char* which) {
    if (matchWhich(which, "vendors"))                  return writeStr(P_VENDORS,  serVendors());
    if (matchWhich(which, "materials"))                return writeStr(P_MATS,     serMaterials());
    if (matchWhich(which, "spool-profiles", "profiles")) return writeStr(P_PROFILES, serProfiles());
    if (matchWhich(which, "colors"))                   return writeStr(P_COLORS,   serColors());
    if (matchWhich(which, "stock-items", "stock"))     return writeStr(P_STOCK,    serStock());
    return false;
}
bool cfgSaveAll() {
    return cfgSave("vendors") && cfgSave("materials") && cfgSave("spool-profiles")
        && cfgSave("colors") && cfgSave("stock-items");
}
String cfgTableJson(const char* which) {
    if (matchWhich(which, "vendors"))                  return serVendors();
    if (matchWhich(which, "materials"))                return serMaterials();
    if (matchWhich(which, "spool-profiles", "profiles")) return serProfiles();
    if (matchWhich(which, "colors"))                   return serColors();
    if (matchWhich(which, "stock-items", "stock"))     return serStock();
    return "[]";
}
bool cfgReplaceTable(const char* which, const String& json) {
    JsonDocument d;
    if (deserializeJson(d, json)) return false;
    JsonArrayConst a = d.as<JsonArrayConst>();
    if (a.isNull()) return false;
    if (matchWhich(which, "vendors"))                  parseVendors(a);
    else if (matchWhich(which, "materials"))           parseMaterials(a);
    else if (matchWhich(which, "spool-profiles", "profiles")) parseProfiles(a);
    else if (matchWhich(which, "colors"))              parseColors(a);
    else if (matchWhich(which, "stock-items", "stock")) parseStock(a);
    else return false;
    return cfgSave(which);
}

// ── Serial harness ────────────────────────────────────────────────────────────
static String tok(const String& s, int& pos) {
    while (pos < (int)s.length() && s[pos] == ' ') pos++;
    int start = pos;
    while (pos < (int)s.length() && s[pos] != ' ') pos++;
    return s.substring(start, pos);
}

bool cfgSerialCommand(const String& lineIn) {
    String line = lineIn; line.trim();
    int pos = 0;
    String cmd = tok(line, pos); cmd.toUpperCase();
    if (cmd != "CFG") return false;
    String sub = tok(line, pos); sub.toLowerCase();

    if (sub == "reload") { cfgBegin(); Serial.println("[cfg] reloaded"); return true; }

    if (sub == "" || sub == "dump" || sub == "all") {
        Serial.printf("[cfg] vendors=%u materials=%u profiles=%u colors=%u stock=%u\n",
                      (unsigned)cfgVendorCount(), (unsigned)cfgMaterialCount(),
                      (unsigned)cfgProfileCount(), (unsigned)cfgColorCount(),
                      (unsigned)cfgStockCount());
        return true;
    }
    if (sub == "vendors") {
        for (auto& v : sVendors) Serial.printf("  %s\n", v.c_str());
    } else if (sub == "materials") {
        for (auto& m : sMaterials)
            Serial.printf("  %-6s dia %.2f  print %d-%d  bed %d-%d\n",
                          m.name, m.dia, m.print_min, m.print_max, m.bed_min, m.bed_max);
    } else if (sub == "profiles") {
        for (auto& p : sProfiles)
            Serial.printf("  %-18s full %.0f g  empty %.0f g\n", p.label, p.nominal_full_g, p.empty_g);
    } else if (sub == "colors") {
        for (auto& c : sColors)
            Serial.printf("  %-14s [%u,%u,%u,%u]\n", c.name, c.rgba[0], c.rgba[1], c.rgba[2], c.rgba[3]);
    } else if (sub == "stock") {
        for (auto& s : sStock)
            Serial.printf("  %s %s %s  min %u spools / %.0f g\n",
                          s.vendor, s.material, s.color, s.min_spools, s.min_grams);
    } else {
        Serial.println("[cfg] CFG [dump|vendors|materials|profiles|colors|stock|reload]");
    }
    return true;
}
