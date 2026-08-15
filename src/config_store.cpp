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
        // A catalog colour is opaque by definition — it exists to name a colour,
        // so alpha 0 there is never intentional. It is what a three-element
        // "rgba": [143,216,160] yields, and the Config page edits this table as
        // raw JSON, so that is an easy thing to type.
        //
        // Left alone it would be poison downstream: alpha 0 is the record-level
        // sentinel for "no colour assigned", so the swatch would render
        // crossed-out, /api/spools would report null, and optEncodeMain() would
        // omit primary_color from the tag entirely — a colour that vanishes
        // without ever erroring. OPT itself says a three-byte colour means fully
        // opaque, so this matches the spec's own reading.
        if (c.rgba[3] == 0) c.rgba[3] = 255;
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
    sVendors = {"eSun", "Overture"};   // Seattle Makers standard stock (add more via Config)
}
// Explicit builders — GCC 8 (esp32 core) rejects push_back({...}) for these
// aggregates (char arrays / nested array members).
static void addMat(const char* name, const char* abbr, int8_t cls, int8_t type,
                   float dia, int16_t pmin, int16_t pmax, int16_t bmin, int16_t bmax) {
    CfgMaterial m{};
    strlcpy(m.name, name, sizeof(m.name));
    strlcpy(m.abbr, abbr, sizeof(m.abbr));
    m.cls = cls; m.type = type;
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
static void addStock(const char* vendor, const char* material, const char* color,
                     float dia, float spool_g, uint16_t min_spools) {
    CfgStock s{};
    strlcpy(s.vendor,   vendor,   sizeof(s.vendor));
    strlcpy(s.material, material, sizeof(s.material));
    strlcpy(s.color,    color,    sizeof(s.color));
    s.dia = dia; s.spool_g = spool_g;
    s.min_spools = min_spools; s.min_grams = 0.0f;
    s.pack_qty = 1;
    sStock.push_back(s);
}

static void seedMaterials() {
    sMaterials.clear();
    // Temps are eSun's published ranges (nozzle / bed). cls = OPT material_class
    // (key 8; 0 = FFF), type = OPT material_type (key 9; PLA 0, PETG 1, TPU 2 per
    // prusa3d/OpenPrintTag). PLA+ has no distinct OPT type — it's a PLA variant —
    // so it maps to PLA (0) while the name keeps the "+".
    addMat("PLA",  "PLA",  0, 0, 1.75f, 190, 220, 45, 60);
    addMat("PLA+", "PLA+", 0, 0, 1.75f, 205, 225, 60, 80);
    addMat("PETG", "PETG", 0, 1, 1.75f, 230, 250, 70, 80);
    addMat("TPU",  "TPU",  0, 2, 1.75f, 220, 250, 30, 60);
}
static void seedProfiles() {
    sProfiles.clear();
    // Tares are ESTIMATES — eSun spools vary 200–267 g by line/generation, so
    // use Capture tare at onboarding for accuracy. Full = 1 kg net.
    addProf("eSun 1kg",       1000.0f, 200.0f);   // PLA / PETG / TPU standard spool
    addProf("eSun PLA+ 1kg",  1000.0f, 255.0f);   // heavier PLA+ spool
    addProf("Overture 1kg",   1000.0f, 235.0f);
    addProf("Generic 1kg",    1000.0f, 200.0f);
    addProf("Generic 0.75kg",  750.0f, 175.0f);
    addProf("Generic 0.5kg",   500.0f, 150.0f);
}
static void seedColors() {
    sColors.clear();
    // Solids
    addColor("Black",            30,  30,  30,  255);
    addColor("White",            245, 245, 245, 255);
    addColor("Cold White",       238, 242, 247, 255);
    addColor("Warm White",       245, 239, 224, 255);
    addColor("Natural",          239, 232, 216, 255);
    addColor("Beige",            232, 220, 192, 255);
    addColor("Grey",             130, 130, 130, 255);
    addColor("Light Brown",      156, 107, 63,  255);
    addColor("Wood",             156, 122, 77,  255);
    addColor("Gold",             212, 175, 55,  255);
    addColor("Red",              200, 30,  30,  255);
    addColor("Fire Engine Red",  200, 20,  20,  255);
    addColor("Orange",           227, 111, 17,  255);
    addColor("Pink",             255, 143, 191, 255);
    addColor("Magenta",          208, 32,  140, 255);
    addColor("Purple",           128, 64,  192, 255);
    addColor("Blue",             40,  80,  200, 255);
    addColor("Light Blue",       90,  160, 230, 255);
    addColor("Green",            40,  160, 60,  255);
    addColor("Olive Green",      107, 142, 35,  255);
    addColor("RGB Green",        40,  160, 40,  255);
    addColor("Yellow",           245, 197, 24,  255);
    addColor("Dark Yellow",      200, 160, 0,   255);
    addColor("Luminous",         200, 255, 200, 255);
    // Translucent / clear (lower alpha)
    addColor("Clear",            223, 232, 232, 110);
    addColor("Translucent Green",143, 216, 160, 150);
    // Silk
    addColor("Silk Silver",      196, 196, 200, 255);
    addColor("Silk Magic",       166, 166, 200, 255);
    addColor("Silk Mystic",      154, 134, 184, 255);
    // Matte finishes (eSun names)
    addColor("Matte Black",              38,  38,  38,  255);
    addColor("Matte White",              242, 242, 242, 255);
    addColor("Matte Light Khaki",        181, 163, 122, 255);
    addColor("Matte Peach Pink",         246, 198, 176, 255);
    addColor("Matte Lake",               106, 165, 208, 255);
    addColor("Matte Rainbow Sunrise",    255, 143, 64,  255);
    addColor("Matte Rainbow Paddy Field",90,  160, 96,  255);
}
static void seedStock() {
    sStock.clear();
    // Seattle Makers standard stock — from the team inventory (Preferred Qty →
    // min_spools). eSun product names used where the inventory description
    // differed. dia 1.75, 1 kg spools.
    // eSun PLA
    addStock("eSun", "PLA", "Beige",                     1.75f, 1000.0f, 1);
    addStock("eSun", "PLA", "Blue",                      1.75f, 1000.0f, 2);
    addStock("eSun", "PLA", "Cold White",                1.75f, 1000.0f, 2);
    addStock("eSun", "PLA", "Fire Engine Red",           1.75f, 1000.0f, 2);
    addStock("eSun", "PLA", "Gold",                      1.75f, 1000.0f, 1);
    addStock("eSun", "PLA", "Grey",                      1.75f, 1000.0f, 2);
    addStock("eSun", "PLA", "Light Blue",                1.75f, 1000.0f, 2);
    addStock("eSun", "PLA", "Light Brown",               1.75f, 1000.0f, 1);
    addStock("eSun", "PLA", "Luminous",                  1.75f, 1000.0f, 1);
    addStock("eSun", "PLA", "Magenta",                   1.75f, 1000.0f, 2);
    addStock("eSun", "PLA", "Natural",                   1.75f, 1000.0f, 1);
    addStock("eSun", "PLA", "Olive Green",               1.75f, 1000.0f, 2);
    addStock("eSun", "PLA", "Orange",                    1.75f, 1000.0f, 3);
    addStock("eSun", "PLA", "Pink",                      1.75f, 1000.0f, 2);
    addStock("eSun", "PLA", "Purple",                    1.75f, 1000.0f, 2);
    addStock("eSun", "PLA", "Red",                       1.75f, 1000.0f, 2);
    addStock("eSun", "PLA", "RGB Green",                 1.75f, 1000.0f, 2);
    addStock("eSun", "PLA", "Silk Magic",                1.75f, 1000.0f, 2);
    addStock("eSun", "PLA", "Silk Mystic",               1.75f, 1000.0f, 3);
    addStock("eSun", "PLA", "Silk Silver",               1.75f, 1000.0f, 1);
    addStock("eSun", "PLA", "Warm White",                1.75f, 1000.0f, 2);
    addStock("eSun", "PLA", "Wood",                      1.75f, 1000.0f, 1);
    addStock("eSun", "PLA", "Matte Black",               1.75f, 1000.0f, 2);
    addStock("eSun", "PLA", "Matte White",               1.75f, 1000.0f, 1);
    addStock("eSun", "PLA", "Matte Light Khaki",         1.75f, 1000.0f, 1);
    addStock("eSun", "PLA", "Matte Lake",                1.75f, 1000.0f, 1);
    addStock("eSun", "PLA", "Matte Rainbow Sunrise",     1.75f, 1000.0f, 1);
    addStock("eSun", "PLA", "Matte Rainbow Paddy Field", 1.75f, 1000.0f, 1);
    // eSun PLA+
    addStock("eSun", "PLA+", "Green",                    1.75f, 1000.0f, 2);
    addStock("eSun", "PLA+", "Black",                    1.75f, 1000.0f, 2);
    addStock("eSun", "PLA+", "Dark Yellow",              1.75f, 1000.0f, 1);
    addStock("eSun", "PLA+", "Matte Peach Pink",         1.75f, 1000.0f, 1);
    addStock("eSun", "PLA+", "Yellow",                   1.75f, 1000.0f, 2);
    // eSun PETG
    addStock("eSun", "PETG", "Black",                    1.75f, 1000.0f, 3);
    addStock("eSun", "PETG", "Blue",                     1.75f, 1000.0f, 2);
    addStock("eSun", "PETG", "Clear",                    1.75f, 1000.0f, 1);
    addStock("eSun", "PETG", "Fire Engine Red",          1.75f, 1000.0f, 1);
    addStock("eSun", "PETG", "Green",                    1.75f, 1000.0f, 2);
    addStock("eSun", "PETG", "Grey",                     1.75f, 1000.0f, 1);
    addStock("eSun", "PETG", "Orange",                   1.75f, 1000.0f, 1);
    addStock("eSun", "PETG", "Purple",                   1.75f, 1000.0f, 1);
    addStock("eSun", "PETG", "Red",                      1.75f, 1000.0f, 1);
    addStock("eSun", "PETG", "Translucent Green",        1.75f, 1000.0f, 1);
    addStock("eSun", "PETG", "White",                    1.75f, 1000.0f, 1);
    addStock("eSun", "PETG", "Yellow",                   1.75f, 1000.0f, 1);
    // TPU
    addStock("eSun",     "TPU", "White", 1.75f, 1000.0f, 1);
    addStock("Overture", "TPU", "Black", 1.75f, 1000.0f, 1);
    addStock("Overture", "TPU", "Clear", 1.75f, 1000.0f, 1);
}

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

bool cfgStockUpdate(size_t i, const CfgStock& s) {
    if (i >= sStock.size()) return false;
    sStock[i] = s;
    return cfgSave("stock-items");
}
bool cfgStockRemove(size_t i) {
    if (i >= sStock.size()) return false;
    sStock.erase(sStock.begin() + i);
    return cfgSave("stock-items");
}

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

// ── Combined export/import ───────────────────────────────────────────────────
String cfgExportAll() {
    String s = "{\"vendors\":";      s += serVendors();
    s += ",\"materials\":";          s += serMaterials();
    s += ",\"spool-profiles\":";     s += serProfiles();
    s += ",\"colors\":";             s += serColors();
    s += ",\"stock-items\":";        s += serStock();
    s += "}";
    return s;
}

bool cfgImportAll(const String& json) {
    JsonDocument d;
    if (deserializeJson(d, json)) return false;
    // All five keys must be present and array-shaped before ANY table is
    // touched — a partial/malformed upload must leave every table exactly as
    // it was, same guarantee the event-log import already makes.
    static const char* kKeys[] = { "vendors", "materials", "spool-profiles", "colors", "stock-items" };
    for (const char* k : kKeys)
        if (!d[k].is<JsonArrayConst>()) return false;
    for (const char* k : kKeys) {
        String sub;
        serializeJson(d[k], sub);
        cfgReplaceTable(k, sub);   // shape already checked above; cannot fail here
    }
    return true;
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
