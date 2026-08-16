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
#include "display_tz.h"
#include "station_name.h"

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
extern volatile bool        gClockSet;

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

// 32 lowercase hex chars (no dashes, matching the store's uuid format) -> 16
// bytes. Used only for catalog-sourced UUIDs coming off the onboard form —
// false on anything malformed, and the caller simply leaves the tag field
// untouched rather than writing a bogus identity.
static bool hex32ToBytes(const String& hex, uint8_t out[16]) {
    if (hex.length() != 32) return false;
    for (int i = 0; i < 16; i++) {
        char b[3] = { hex[i * 2], hex[i * 2 + 1], 0 };
        char* end = nullptr;
        long v = strtol(b, &end, 16);
        if (end != b + 2) return false;
        out[i] = (uint8_t)v;
    }
    return true;
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
    ".inv-row{cursor:pointer}"
    ".inv-row:hover{background:#1a1a1a}"
    ".arrow{display:inline-block;font-size:22px;line-height:1;color:#8f8}"
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

// ── Onboard page: OpenPrintTag catalog search widget ──────────────────────────
// Cascading Vendor -> Material type -> Material combo boxes that live-search
// github.com/OpenPrintTag/openprinttag-database straight from the browser (the
// device is never in the loop — no firmware cost, no extra heap). A pick fills
// the hidden cat_* fields in the onboard form with the vendor's own GTIN, the
// three OPT identity UUIDs, and real print/bed temps when the material file
// carries them, instead of whatever a person free-types.
//
// Prototyped and verified against the live DB at tools/opt-catalog/prototype.html
// before landing here; this is the same logic, minus the standalone page chrome.
static const char* CATALOG_STYLE =
    "<style>"
    ".cat-status{font-size:12px;color:#888;margin:6px 0 14px}"
    ".cat-status b{color:#eee}"
    ".cat-status.err b{color:#fd6}"
    ".cat-field{margin-bottom:12px;position:relative}"
    ".cat-field.disabled{opacity:.45}"
    ".cat-field label{display:block;font-size:12px;color:#9c9;text-transform:uppercase;"
      "letter-spacing:.04em;margin-bottom:4px}"
    ".cat-input.picked{border-color:#2a5}"
    ".cat-panel{display:none;position:absolute;left:0;right:0;top:calc(100% + 2px);z-index:20;"
      "background:#1a1a1a;border:1px solid #444;border-radius:6px;max-height:260px;"
      "overflow-y:auto;box-shadow:0 10px 30px #0008}"
    ".cat-group{padding:6px 10px 2px;font-size:11px;color:#888;text-transform:uppercase;letter-spacing:.04em}"
    ".cat-divider{border-top:1px solid #333;margin:2px 0}"
    ".cat-empty{padding:10px;color:#888;font-size:13px}"
    ".cat-row{padding:10px;display:flex;justify-content:space-between;align-items:center;gap:8px;cursor:pointer}"
    ".cat-row:active,.cat-row.hi{background:#20263a}"
    ".cat-row .meta{color:#888;font-size:12px;white-space:nowrap}"
    "</style>";

static const char* CATALOG_SCRIPT = R"OPTJS(<script>
(function(){
const OWNER="OpenPrintTag", REPO="openprinttag-database", BRANCH="main";
const API=`https://api.github.com/repos/${OWNER}/${REPO}`;
const RAW=`https://raw.githubusercontent.com/${OWNER}/${REPO}/${BRANCH}/data`;
const PKG_PREFIX="data/material-packages/";
const TYPE_ENUM_URL="https://raw.githubusercontent.com/prusa3d/OpenPrintTag/main/data/material_type_enum.yaml";

const $ = s => document.querySelector(s);
const statusEl=$("#cat-status"), resolvedEl=$("#cat-resolved");
let PACKAGES = [];
let brandNames = new Map();

function esc(s){ return String(s==null?"":s).replace(/[&<>"]/g, c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c])); }
function setStatus(html, err){ statusEl.className="cat-status"+(err?" err":""); statusEl.innerHTML=html; }

// ── minimal YAML subset parser (maps, nested maps, lists of scalars/maps) ────
function parseYaml(text){
  const lines=text.split(/\r?\n/).map(l=>l.replace(/\s+$/,"")).filter(l=>l.trim()!==""&&!/^\s*#/.test(l));
  let pos=0; const indentOf=l=>l.length-l.trimStart().length;
  const scalar=v=>{v=v.trim();
    if((v[0]==="'"&&v.endsWith("'"))||(v[0]==='"'&&v.endsWith('"')))return v.slice(1,-1);
    if(/^-?\d+$/.test(v))return v.length>15?v:parseInt(v,10);
    if(/^-?\d*\.\d+$/.test(v))return parseFloat(v); return v;};
  function parseMap(indent){const obj={};
    while(pos<lines.length){const line=lines[pos],ind=indentOf(line),t=line.trim();
      if(ind<indent)break; if(t.startsWith("- "))break;
      const m=t.match(/^([^:]+):\s*(.*)$/); if(!m){pos++;continue;}
      const key=m[1].trim(),val=m[2]; pos++;
      if(val===""){const nx=lines[pos];
        if(nx!==undefined){const ni=indentOf(nx);
          if(nx.trim().startsWith("- ")&&ni>=indent)obj[key]=parseList(ni);
          else if(ni>indent)obj[key]=parseMap(ni); else obj[key]=null;
        }else obj[key]=null;
      }else obj[key]=scalar(val);}
    return obj;}
  function parseList(indent){const arr=[];
    while(pos<lines.length){const line=lines[pos],ind=indentOf(line),t=line.trim();
      if(ind<indent||!t.startsWith("- "))break;
      const rest=t.slice(2),m=rest.match(/^([^:]+):\s*(.*)$/); pos++;
      if(m){const item={};item[m[1].trim()]=m[2]===""?null:scalar(m[2]);
        const child=ind+2;
        if(pos<lines.length&&indentOf(lines[pos])>=child&&!lines[pos].trim().startsWith("- "))
          Object.assign(item,parseMap(child));
        arr.push(item);}else arr.push(scalar(rest));}
    return arr;}
  return parseMap(0);
}
async function fetchText(url){ const r=await fetch(url,{cache:"no-store"}); if(!r.ok) throw new Error(`${r.status} ${r.statusText} for ${url}`); return r.text(); }
async function fetchYaml(url){ return parseYaml(await fetchText(url)); }

// ── index: one tree fetch (cached by data_hash) + one small type-enum fetch ──
async function loadIndex(){
  let hash="nohash";
  try { const man=await fetchText(`${RAW}/manifest.yaml`); hash=(man.match(/data_hash:\s*(\S+)/)||[])[1]||"nohash"; } catch(e){}

  const cacheKey="opt_pkgindex2_"+hash;
  const cached=localStorage.getItem(cacheKey);
  if(cached){ PACKAGES=JSON.parse(cached); setStatus(`${PACKAGES.length.toLocaleString()} packages <span class="muted">(cached)</span>`); }
  else {
    setStatus("Fetching catalog tree from GitHub (one request)&hellip;");
    const tree=JSON.parse(await fetchText(`${API}/git/trees/${BRANCH}?recursive=1`));
    PACKAGES=(tree.tree||[]).filter(e=>e.type==="blob"&&e.path.startsWith(PKG_PREFIX)&&e.path.endsWith(".yaml"))
      .map(e=>{const rel=e.path.slice(PKG_PREFIX.length); const brand=rel.split("/")[0];
        const file=rel.split("/").pop().replace(/\.yaml$/,"");
        return {path:e.path, brand, file};});
    try { localStorage.setItem(cacheKey, JSON.stringify(PACKAGES)); } catch(e){}
    for(let i=localStorage.length-1;i>=0;i--){ const k=localStorage.key(i); if(k&&k.startsWith("opt_pkgindex2_")&&k!==cacheKey) localStorage.removeItem(k); }
    setStatus(`${PACKAGES.length.toLocaleString()} packages <span class="muted">(fresh)</span>`);
  }

  // Type tokens: scan filenames for exact hyphen-delimited matches against the
  // OPT's own material_type_enum abbreviations. No per-item fetch. ~93% of
  // packages resolve a type; the rest fall into "Other" honestly rather than
  // guessing.
  try {
    const enumText=await fetchText(TYPE_ENUM_URL);
    const ABBR=new Set([...enumText.matchAll(/abbreviation:\s*(\S+)/g)].map(m=>m[1].toLowerCase()));
    PACKAGES.forEach(p=>{ p.typeTok = p.file.split("-").find(t=>ABBR.has(t)) || null; });
  } catch(e){ PACKAGES.forEach(p=>p.typeTok=null); }

  prefetchBrandNames();  // progressive; does not block the UI
}

async function prefetchBrandNames(){
  const slugs=[...new Set(PACKAGES.map(p=>p.brand))];
  await Promise.all(slugs.map(async slug=>{
    try { const b=await fetchYaml(`${RAW}/brands/${slug}.yaml`); if(b.name) brandNames.set(slug,b.name); } catch(e){}
  }));
  vendorCombo.renderPanel();  // refresh labels if the panel happens to be open
}

// ── small helpers ──────────────────────────────────────────────────────────
function prettifySlug(s){ return s.replace(/-/g," ").replace(/\b\w/g,c=>c.toUpperCase()); }
function prettyName(file, brand){
  let n=file.replace(/-/g," ");
  if(n.toLowerCase().startsWith(brand.replace(/-/g," ").toLowerCase()+" ")) n=n.slice(brand.length+1);
  return n.replace(/\b\w/g,c=>c.toUpperCase());
}
function weightBadge(file){
  let m=file.match(/-(\d+(?:\.\d+)?kg)(?:-(spool|old-spool|refill))?$/i);
  if(m) return m[1].toUpperCase().replace("KG"," kg") + (m[2]?` (${m[2].replace("-"," ")})`:"");
  m=file.match(/-(\d+)-(spool|old-spool|refill|no-spool)$/i);
  if(m) return `${m[1]} g` + (m[2]!=="spool" ? ` (${m[2].replace("-"," ")})` : "");
  return null;
}
function loadMru(key){ try { return JSON.parse(localStorage.getItem(key)||"[]"); } catch(e){ return []; } }
function pushMru(key,val,cap){ let a=loadMru(key).filter(v=>v!==val); a.unshift(val); a=a.slice(0,cap); localStorage.setItem(key,JSON.stringify(a)); }

// ── candidate lists (all client-side, zero extra fetches) ───────────────────
function vendorCandidates(){
  const counts=new Map();
  for(const p of PACKAGES) counts.set(p.brand,(counts.get(p.brand)||0)+1);
  return [...counts.entries()]
    .map(([slug,count])=>({value:slug, label:brandNames.get(slug)||prettifySlug(slug), count}))
    .sort((a,b)=>a.label.localeCompare(b.label));
}
function typeCandidatesFor(vendorSlug){
  const counts=new Map();
  for(const p of PACKAGES){ if(p.brand!==vendorSlug) continue; const tok=p.typeTok||"other"; counts.set(tok,(counts.get(tok)||0)+1); }
  const total=[...counts.values()].reduce((a,b)=>a+b,0);
  const rows=[{value:"", label:"All types", count:total}];
  for(const [tok,count] of [...counts.entries()].sort((a,b)=>b[1]-a[1]))
    rows.push({value:tok, label: tok==="other" ? "Other / unspecified" : tok.toUpperCase(), count});
  return rows;
}
function materialCandidatesFor(vendorSlug, typeTok){
  return PACKAGES.filter(p=>p.brand===vendorSlug && (!typeTok || p.typeTok===typeTok))
    .map(p=>{
      const badge=weightBadge(p.file);
      return { value:p.path, label:prettyName(p.file,p.brand)+(badge?` — ${badge}`:"") };
    })
    .sort((a,b)=> a.label.localeCompare(b.label));
}

// ── reusable combo box: MRU (if any) → divider → list; type-to-filter ────────
function makeCombo(fieldId, inputId, panelId, placeholder){
  const field=$(fieldId), input=$(inputId), panel=$(panelId);
  let candidatesFn=()=>[], mruKey=null, mruCap=0, onPick=()=>{};
  let rows=[], hiIdx=-1;

  function setSource(cfg){ candidatesFn=cfg.candidatesFn; mruKey=cfg.mruKey||null; mruCap=cfg.mruCap||0; onPick=cfg.onPick; }
  function rowHtml(c,i){
    return `<div class="cat-row${i===hiIdx?" hi":""}" data-i="${i}">`+
      `<span>${esc(c.label)}</span>`+
      (c.count!=null?`<span class="meta">${c.count}</span>`:"")+`</div>`;
  }
  function renderPanel(){
    const all=candidatesFn();
    const filt=input.value.trim().toLowerCase();
    const terms=filt.split(/\s+/).filter(Boolean);
    const match=c=>terms.every(t=>c.label.toLowerCase().includes(t));
    const mruVals=(!filt && mruKey) ? loadMru(mruKey) : [];
    const mruRows=mruVals.map(v=>all.find(c=>c.value===v)).filter(Boolean);
    let rest=all.filter(c=>!mruRows.includes(c));
    if(filt) rest=rest.filter(match);
    rest=rest.slice(0,80);
    rows=[...mruRows, ...rest];
    hiIdx=-1;
    panel.innerHTML =
      (mruRows.length? `<div class="cat-group">Recently used</div>${mruRows.map((c,i)=>rowHtml(c,i)).join("")}<div class="cat-divider"></div>` : "") +
      (rest.length ? rest.map((c,i)=>rowHtml(c,mruRows.length+i)).join("")
                   : (all.length ? `<div class="cat-empty">No matches</div>` : `<div class="cat-empty">Nothing here yet</div>`));
  }
  function show(){ if(input.disabled) return; renderPanel(); panel.style.display="block"; }
  function hide(){ panel.style.display="none"; }
  function pick(c){
    input.value=c.label; input.classList.add("picked"); hide();
    if(mruKey) pushMru(mruKey, c.value, mruCap);
    onPick(c);
  }
  input.addEventListener("focus", show);
  input.addEventListener("input", ()=>{ input.classList.remove("picked"); show(); });
  input.addEventListener("blur", ()=> setTimeout(hide, 150));
  input.addEventListener("keydown", e=>{
    if(panel.style.display!=="block") return;
    if(e.key==="ArrowDown"){ e.preventDefault(); hiIdx=Math.min(hiIdx+1, rows.length-1); renderHi(); }
    else if(e.key==="ArrowUp"){ e.preventDefault(); hiIdx=Math.max(hiIdx-1, 0); renderHi(); }
    else if(e.key==="Enter"){ e.preventDefault(); if(rows[hiIdx]) pick(rows[hiIdx]); }
    else if(e.key==="Escape"){ hide(); }
  });
  function renderHi(){ panel.querySelectorAll(".cat-row").forEach(r=>r.classList.toggle("hi", +r.dataset.i===hiIdx)); }
  panel.addEventListener("mousedown", e=>{
    const row=e.target.closest(".cat-row[data-i]"); if(!row) return;
    e.preventDefault(); pick(rows[+row.dataset.i]);
  });
  function reset(){ input.value=""; input.classList.remove("picked"); }
  function enable(text){ input.disabled=false; input.placeholder=text||placeholder; field.classList.remove("disabled"); }
  function disable(text){ input.disabled=true; reset(); input.placeholder=text; field.classList.add("disabled"); }
  return { setSource, reset, enable, disable, renderPanel };
}

const vendorCombo   = makeCombo("#cat-field-vendor",   "#cat-in-vendor",   "#cat-panel-vendor",   "Type or tap to browse vendors…");
const typeCombo     = makeCombo("#cat-field-type",     "#cat-in-type",     "#cat-panel-type",     "Type or tap to browse types…");
const materialCombo = makeCombo("#cat-field-material", "#cat-in-material", "#cat-panel-material", "Type or tap to browse materials…");
typeCombo.disable("Pick a vendor first");
materialCombo.disable("Pick a vendor first");

vendorCombo.setSource({
  candidatesFn: vendorCandidates,
  mruKey: "opt_mru_vendor", mruCap: 3,
  onPick(c){
    typeCombo.reset(); typeCombo.enable();
    materialCombo.disable("Pick a material type first");
    resolvedEl.innerHTML = "";
    typeCombo.setSource({
      candidatesFn: ()=>typeCandidatesFor(c.value),
      mruKey: "opt_mru_type", mruCap: 4,
      onPick(tc){
        materialCombo.reset(); materialCombo.enable();
        resolvedEl.innerHTML = "";
        materialCombo.setSource({
          candidatesFn: ()=>materialCandidatesFor(c.value, tc.value || null),
          onPick(mc){ resolvePick(mc.value); }
        });
      }
    });
  }
});

// ── resolve a picked package: package → material + container + brand, then
//    fill the form's hidden cat_* fields with the authoritative values ───────
function stripDash(u){ return (u||"").replace(/-/g,""); }
function setField(name, val){ const el=$("#cat-f-"+name); if(el) el.value = (val==null?"":val); }

async function resolvePick(pkgPath){
  resolvedEl.innerHTML = "<p class='muted'>Resolving…</p>";
  const brandSlug = pkgPath.slice(PKG_PREFIX.length).split("/")[0];
  try {
    const pkg = await fetchYaml(`${RAW}/material-packages/${brandSlug}/${pkgPath.split("/").pop()}`);
    const matSlug = pkg.material && pkg.material.slug, contSlug = pkg.container && pkg.container.slug;
    const [mat, cont, brand] = await Promise.all([
      matSlug  ? fetchYaml(`${RAW}/materials/${brandSlug}/${matSlug}.yaml`)       : Promise.resolve({}),
      contSlug ? fetchYaml(`${RAW}/material-containers/${contSlug}.yaml`)         : Promise.resolve({}),
                 fetchYaml(`${RAW}/brands/${brandSlug}.yaml`).catch(()=>({})),
    ]);
    applyPick(pkg, mat, cont, brand, brandSlug);
  } catch(e){
    resolvedEl.innerHTML = `<p class="ob">Couldn't resolve that pick: ${esc(e.message)}</p>`;
  }
}

function applyPick(pkg, mat, cont, brand, brandSlug){
  const rgba = (mat.primary_color && mat.primary_color.color_rgba) || "";
  const dia  = pkg.filament_diameter ? (pkg.filament_diameter/1000) : null;
  const tare = (cont.empty_weight != null) ? cont.empty_weight : null;
  const props = mat.properties || {};
  const tempKeys = ["min_print_temperature","max_print_temperature","min_bed_temperature","max_bed_temperature"];
  const haveTemps = tempKeys.every(k => typeof props[k] === "number");

  const vendor = brand.name || prettifySlug(brandSlug);
  const material = mat.name || "";
  const abbr = mat.abbreviation || mat.type || "";

  setField("cat_vendor", vendor);
  setField("cat_material", material);
  setField("cat_abbr", abbr);
  setField("cat_rgba", rgba ? rgba.replace("#","") : "");
  setField("cat_dia", dia);
  setField("cat_nom_g", pkg.nominal_netto_full_weight);
  setField("cat_empty_g", tare);
  setField("cat_gtin", pkg.gtin != null ? String(pkg.gtin) : "");
  setField("cat_pkg_uuid", stripDash(pkg.uuid));
  setField("cat_mat_uuid", stripDash(mat.uuid));
  setField("cat_brand_uuid", stripDash(brand.uuid));
  if (haveTemps) {
    setField("cat_print_min", props.min_print_temperature);
    setField("cat_print_max", props.max_print_temperature);
    setField("cat_bed_min",   props.min_bed_temperature);
    setField("cat_bed_max",   props.max_bed_temperature);
  } else {
    setField("cat_print_min",""); setField("cat_print_max","");
    setField("cat_bed_min","");   setField("cat_bed_max","");
  }
  $("#cat-source").value = "catalog";

  const sw = rgba ? `<span class="sw" style="background:${esc(rgba.slice(0,7))}"></span>` : "";
  const missTare = tare==null ? " <span class='ob'>&mdash; missing; capture below</span>" : "";
  resolvedEl.innerHTML =
    `<div class="card" style="border-color:#2a5;margin-top:10px">` +
    `<div style="font-size:17px;font-weight:600">${sw}${esc(vendor)} ${esc(material)}</div>` +
    `<p class="muted">${esc(abbr)}` +
      (dia!=null?` &middot; ${dia.toFixed(2)} mm`:``) +
      (pkg.nominal_netto_full_weight?` &middot; ${pkg.nominal_netto_full_weight} g`:``) +
      ` &middot; tare ${tare!=null?tare+" g":"unknown"}${missTare}</p>` +
    `<p class="muted" style="font-size:12px">GTIN ${pkg.gtin||"&mdash;"}` +
      (haveTemps ? ` &middot; print ${props.min_print_temperature}&ndash;${props.max_print_temperature}&deg;C` : ``) +
    `</p></div>`;
}

// ── boot ──────────────────────────────────────────────────────────────────
(async () => {
  try {
    await loadIndex();
    vendorCombo.enable();
    setStatus(`${PACKAGES.length.toLocaleString()} packages, ${new Set(PACKAGES.map(p=>p.brand)).size} vendors ready.`);
  } catch(e){
    setStatus(`Couldn't load the catalog: <b>${esc(e.message)}</b> &mdash; use manual entry below.`, true);
    const det = document.getElementById("cat-manual-details");
    if (det) det.open = true;
  }
})();
})();
</script>)OPTJS";

// One nav link, marked `active` when its href matches the current page.
static void navlink(String& h, const char* href, const char* label, const char* active) {
    h += "<a href='";
    h += href;
    h += (strcmp(href, active) == 0) ? "' class='active'>" : "'>";
    h += label;
    h += "</a>";
}

// `active` = the nav href for the current page (e.g. "/reorder"); "" = none.
static String head(const char* title, const char* active = "") {
    String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>";
    h += title;
    h += "</title>";
    h += STYLE;
    h += "</head><body><header>Weigh Station<nav>";
    navlink(h, "/",          "Inventory", active);
    navlink(h, "/onboard",   "Onboard",   active);
    navlink(h, "/reorder",   "Reorder",   active);
    navlink(h, "/stock",     "Stock List", active);
    navlink(h, "/usage",     "Usage",     active);
    navlink(h, "/config",    "Settings",  active);
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
    // NOT `HEX`: Arduino's Print.h does `#define HEX 16`, so that name expands
    // to a numeric constant here and the declaration fails to parse. DEC, OCT
    // and BIN are the same. Anything named after a print base needs a prefix.
    static const char* kHexDigits = "0123456789ABCDEF";
    String o;
    o.reserve(s.length() + 16);
    for (size_t i = 0; i < s.length(); i++) {
        const unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { o += '%'; o += kHexDigits[c >> 4]; o += kHexDigits[c & 0x0F]; }
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

    // ── Physical inventory audit ────────────────────────────────────────────
    // "We weigh and scan every reel in the library" -- state lives entirely in
    // the log (storeAuditPhase() derives it), so this bar is correct even right
    // after a reboot mid-audit. A spool counts as found simply by having been
    // weighed since audit_start; nothing about normal weighing changes.
    const AuditPhase auditPhase = storeAuditPhase();
    char auditStartTs[25] = {};
    storeAuditStartTs(auditStartTs, sizeof(auditStartTs));

    uint32_t auditTotal = 0, auditFound = 0, retiredCount = 0;
    {
        SpoolRecord rr;
        for (size_t i = 0; i < storeSpoolCount(); i++) {
            if (!storeSpoolAt(i, rr)) continue;
            if (rr.retired) { retiredCount++; continue; }
            if (rr.remaining_g <= 1.0f) continue;
            if (auditPhase != AuditPhase::Idle) {
                auditTotal++;
                if (strcmp(rr.last_ts, auditStartTs) >= 0) auditFound++;
            }
        }
    }

    if (auditPhase == AuditPhase::Idle) {
        p += "<div class='card'><label style='margin-top:0'>Physical inventory</label>"
             "<p class='muted'>Weigh and scan every reel in the library. Anything not "
             "seen by the end gets reviewed before it's marked disposed.</p>"
             "<form method='POST' action='/api/audit/start'>"
             "<button type='submit'>Start audit</button></form></div>";
    } else if (auditPhase == AuditPhase::Scanning) {
        p += "<div class='card' style='border-color:#2a5'><label style='margin-top:0'>"
             "Audit in progress</label><p><b>" + String(auditFound) + " of " + String(auditTotal)
           + "</b> found so far. Weigh spools as normal &mdash; each expanded row below "
             "shows found status.</p>"
             "<form method='POST' action='/api/audit/finish' style='display:inline'>"
             "<button type='submit'>Finish audit</button></form> "
             "<form method='POST' action='/api/audit/abandon' style='display:inline' "
             "onsubmit=\"return confirm('Abandon this audit? Nothing already found is "
             "affected, but the audit itself ends with no review.')\">"
             "<button type='submit' class='sec'>Abandon audit</button></form></div>";
    } else {  // Resolving
        uint32_t notFound = auditTotal - auditFound;
        p += "<div class='card' style='border-color:#a70'><label style='margin-top:0'>"
             "Audit &mdash; resolving</label><p><b class='ob'>" + String(notFound)
           + " spool(s) not found.</b> Mark each Closed (confirmed disposed) or Found "
             "below before this audit can complete. Still weighable normally &mdash; "
             "weighing one resolves it too.</p>"
             "<form method='POST' action='/api/audit/abandon' "
             "onsubmit=\"return confirm('Abandon this audit? Nothing already closed or "
             "found is affected, but the remaining not-found spools stay unresolved.')\">"
             "<button type='submit' class='sec'>Abandon audit</button></form></div>";
    }

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
                   + " &middot; " + String(r.remaining_g, 0) + " grams remaining &middot; "
                   + String(r.used_g, 0) + " grams used</p>";

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
    // Bucketed by (vendor, material) — see MatInventory's comment in store.h —
    // so the vendor prefix here ("eSun PLA Summer Grass") is always a single
    // vendor's, never a merge of two.
    // Click a row to expand it in place — the individual spools behind that
    // count, scoped to just this (vendor, material) bucket. This IS the
    // spool list now; there is no separate /spools page any more. No fetch:
    // the detail rows are server-rendered right
    // here and hidden with display:none, so expanding costs nothing over the
    // network, only a bit more HTML in the initial page load. Filtered to
    // remaining_g > 1 g, same as m.count itself, so the row count in the
    // summary always matches the number of rows revealed underneath it —
    // showing every last near-empty spool here would make that number lie.
    const bool showRetired = req->hasParam("retired");
    p += "<p class='muted'>" + String(retiredCount) + " spool(s) retired &mdash; "
       + (showRetired
           ? "<a href='/' style='color:#8f8'>hide</a>"
           : "<a href='/?retired=1' style='color:#8f8'>show</a>")
       + "</p>";

    p += "<h3>Inventory</h3><table>"
         "<tr><th style='width:2em'></th><th>Filament</th><th>Spools</th><th>Remaining</th></tr>";
    size_t n = storeInventoryCount();
    MatInventory m;
    if (n == 0) p += "<tr><td colspan='4' class='muted'>No spools yet</td></tr>";
    for (size_t i = 0; i < n; i++) {
        if (!storeInventoryAt(i, m)) continue;
        String label = m.vendor[0] ? String(m.vendor) + " " + m.material : String(m.material);
        String rowId = "inv-detail-" + String((unsigned)i);

        // Build the detail rows first so the summary row above them can carry
        // a "not found" badge -- needs to know before it renders, not after.
        String detail = "<table style='margin:2px 0 10px'><tr><th>Spool #</th>"
                         "<th>Remaining</th><th></th></tr>";
        uint32_t bucketNotFound = 0;
        SpoolRecord r;
        size_t sn = storeSpoolCount();
        for (size_t k = 0; k < sn; k++) {
            if (!storeSpoolAt(k, r)) continue;
            if (strcmp(r.vendor, m.vendor) != 0 || strcmp(r.material, m.material) != 0) continue;
            if (r.retired) {
                if (!showRetired) continue;
                detail += "<tr style='opacity:.6'><td><a href='/spool?id=" + String((unsigned)r.spool)
                        + "' style='color:#8f8'>#" + String((unsigned)r.spool) + "</a></td><td>0 g</td>"
                          "<td class='muted'>retired</td></tr>";
                continue;
            }
            if (r.remaining_g <= 1.0f) continue;
            const bool found = (auditPhase == AuditPhase::Idle) ||
                                (strcmp(r.last_ts, auditStartTs) >= 0);
            if (!found) bucketNotFound++;
            detail += "<tr><td><a href='/spool?id=" + String((unsigned)r.spool)
                     + "' style='color:#8f8'>#" + String((unsigned)r.spool) + "</a></td><td>"
                     + String(r.remaining_g, 0) + " g</td><td>";
            if (r.needs_ob) detail += "<span class='ob'>needs onboarding</span> ";
            if (auditPhase == AuditPhase::Scanning) {
                detail += found ? "<span style='color:#8f8'>found</span>"
                                : "<span class='muted'>not yet found</span>";
            } else if (auditPhase == AuditPhase::Resolving && !found) {
                detail += "<form method='POST' action='/api/audit/close' style='display:inline' "
                           "onsubmit=\"return confirm('Confirm spool #" + String((unsigned)r.spool)
                         + " is disposed? This cannot be undone.')\">"
                           "<input type='hidden' name='spool' value='" + String((unsigned)r.spool) + "'>"
                           "<button type='submit' style='background:#522;color:#eee;margin:0;padding:4px 10px;"
                           "font-size:13px;border-radius:4px;border:0;cursor:pointer'>Close</button></form> "
                           "<form method='POST' action='/api/audit/found' style='display:inline'>"
                           "<input type='hidden' name='spool' value='" + String((unsigned)r.spool) + "'>"
                           "<button type='submit' style='margin:0;padding:4px 10px;font-size:13px'>Found</button>"
                           "</form>";
            }
            detail += "</td></tr>";
        }
        detail += "</table>";

        p += "<tr class='inv-row' onclick=\"var d=document.getElementById('" + rowId + "');"
             "var open=d.style.display=='table-row';d.style.display=open?'none':'table-row';"
             "this.querySelector('.arrow').textContent=open?'\\u25b8':'\\u25be'\">"
             "<td class='muted'><span class='arrow'>&#9656;</span></td><td>"
           + swatch(m.rgba) + esc(label.c_str())
           + (bucketNotFound ? " <span class='ob'>(" + String(bucketNotFound) + " not found)</span>" : "")
           + "</td><td>" + String(m.count) + "</td><td>" + String(m.remaining_g, 0) + " g</td></tr>";
        p += "<tr id='" + rowId + "' style='display:none'><td></td><td colspan='3'>" + detail + "</td></tr>";
    }
    p += "</table>";
    p += "<p class='muted'>" + String((unsigned)storeSpoolCount())
       + " spools tracked &middot; " + String((unsigned)storeLogLineCount())
       + " log entries</p>";
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
           + ",\"needs_onboarding\":" + (r.needs_ob ? "true" : "false")
           + ",\"retired\":" + (r.retired ? "true" : "false") + "}";
    }
    j += "]";
    req->send(200, "application/json", j);
}

// ── Physical inventory audit ────────────────────────────────────────────────
static void handleApiAuditStart(AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    storeAuditStart();
    req->redirect("/");
}
static void handleApiAuditFinish(AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    storeAuditFinish();
    req->redirect("/");
}
static void handleApiAuditAbandon(AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    storeAuditAbandon();
    req->redirect("/");
}
static void handleApiAuditClose(AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    const AsyncWebParameter* ps = req->getParam("spool", true);
    if (!ps) { req->send(400, "text/plain", "missing spool"); return; }
    storeAuditClose((uint32_t)ps->value().toInt());
    req->redirect("/");
}
static void handleApiAuditFound(AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    const AsyncWebParameter* ps = req->getParam("spool", true);
    if (!ps) { req->send(400, "text/plain", "missing spool"); return; }
    storeAuditFound((uint32_t)ps->value().toInt());
    req->redirect("/");
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
    std::vector<bool>   retired;   // this row is the spool's closing entry
};
static void collectWeigh(const StoreEvent& e, void* ctx) {
    WeighSeries* s = (WeighSeries*)ctx;
    s->rem.push_back(e.remaining_g);
    s->used.push_back(e.used_g);
    s->gross.push_back(e.gross_g);
    // Localized here, once, so both the HTML table and the CSV export below
    // get the same human-readable local time -- the log's own e.ts stays UTC
    // and untouched; this is a display-time conversion only.
    char local[32];
    storeLocalizeIso(e.ts, local, sizeof(local));
    s->ts.push_back(local[0] ? String(local) : String(e.ts));
    s->retired.push_back(e.ev == StoreEv::Retire);
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
        // "when_local" (not "ts"): the value is already converted to the
        // configured display timezone (Settings page), unlike every other
        // timestamp this station exposes (/export, /api/*), which stay UTC
        // on purpose -- this is the one download meant for a human to open
        // directly, not a machine to parse.
        String out = "when_local,gross_g,remaining_g,used_g,event\n";
        for (size_t i = 0; i < s.rem.size(); i++)
            out += s.ts[i] + "," + String(s.gross[i], 1) + ","
                 + String(s.rem[i], 1) + "," + String(s.used[i], 1) + ","
                 + (s.retired[i] ? "retired" : "weigh") + "\n";
        AsyncWebServerResponse* resp = req->beginResponse(200, "text/csv", out);
        resp->addHeader("Content-Disposition",
                        "attachment; filename=spool-" + String(id) + ".csv");
        req->send(resp);
        return;
    }

    String p = head("Spool", "/");
    p += "<div class='card'>";
    p += "<div class='big'>" + swatch(r.rgba)
       + "#" + String((unsigned)r.spool) + " "
       + esc(r.material[0] ? r.material : "Unknown") + "</div>";
    p += "<p class='muted'>" + esc(r.vendor) + (r.abbr[0] ? " &middot; " + esc(r.abbr) : String())
       + " &middot; " + String((unsigned)s.rem.size()) + " weigh session(s)</p>";
    p += "<p class='big'>" + String(r.remaining_g, 0) + " grams <span class='muted' "
         "style='font-size:15px'>remaining &middot; " + String(r.used_g, 0)
       + " grams used</span></p>";
    // This is where onboarding now lands, so it has to show what was entered —
    // otherwise submitting the form gives no confirmation that the tare and
    // nominal weight (the values every later reading depends on) took.
    String spec = specStrip(r);
    if (r.last_ts[0]) {
        char local[32];
        storeLocalizeIso(r.last_ts, local, sizeof(local));
        spec += kv("Last weighed", esc(local[0] ? local : r.last_ts));
    }
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
    p += "<table><tr><th>When</th><th>Gross</th><th>Remaining</th><th>Used</th><th></th></tr>";
    if (s.rem.empty())
        p += "<tr><td colspan='5' class='muted'>No weigh sessions yet</td></tr>";
    for (size_t k = s.rem.size(); k-- > 0; )   // newest first
        p += "<tr><td>" + s.ts[k] + "</td><td>" + String(s.gross[k], 0)
           + " g</td><td>" + String(s.rem[k], 0) + " g</td><td>"
           + String(s.used[k], 0) + " g</td><td>"
           + (s.retired[k] ? "<span class='ob'>closed &mdash; audit</span>" : "") + "</td></tr>";
    p += "</table>";
    p += "<p class='muted'><a href='/' style='color:#8f8'>&larr; back</a></p>";
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

    // ── OpenPrintTag catalog search ───────────────────────────────────────────
    // Live search of the OPT database (github.com/OpenPrintTag/openprinttag-
    // database) straight from the browser — the device is never in the loop,
    // so this costs the firmware nothing. A pick fills the hidden cat_* fields
    // below with authoritative vendor data (GTIN, the three OPT UUIDs, real
    // print/bed temps when the material file has them) instead of whatever a
    // person free-types. See tools/opt-catalog/prototype.html for how this was
    // validated against the live DB before landing here.
    p += "<div class='card'>"
         "<label>Search filament catalog</label>"
         "<div class='cat-status' id='cat-status'>Loading catalog&hellip;</div>"
         "<div class='cat-field' id='cat-field-vendor'>"
           "<label>Vendor</label>"
           "<input class='cat-input' id='cat-in-vendor' placeholder='Loading&hellip;' disabled autocomplete='off'>"
           "<div class='cat-panel' id='cat-panel-vendor'></div>"
         "</div>"
         "<div class='cat-field disabled' id='cat-field-type'>"
           "<label>Material type</label>"
           "<input class='cat-input' id='cat-in-type' placeholder='Pick a vendor first' disabled autocomplete='off'>"
           "<div class='cat-panel' id='cat-panel-type'></div>"
         "</div>"
         "<div class='cat-field disabled' id='cat-field-material'>"
           "<label>Material</label>"
           "<input class='cat-input' id='cat-in-material' placeholder='Pick a vendor first' disabled autocomplete='off'>"
           "<div class='cat-panel' id='cat-panel-material'></div>"
         "</div>"
         "<div id='cat-resolved'></div>"
         "</div>";
    p += "<input type='hidden' name='source' id='cat-source' value='manual'>"
         "<input type='hidden' name='cat_vendor'      id='cat-f-cat_vendor'>"
         "<input type='hidden' name='cat_material'    id='cat-f-cat_material'>"
         "<input type='hidden' name='cat_abbr'        id='cat-f-cat_abbr'>"
         "<input type='hidden' name='cat_rgba'        id='cat-f-cat_rgba'>"
         "<input type='hidden' name='cat_dia'         id='cat-f-cat_dia'>"
         "<input type='hidden' name='cat_nom_g'       id='cat-f-cat_nom_g'>"
         "<input type='hidden' name='cat_empty_g'     id='cat-f-cat_empty_g'>"
         "<input type='hidden' name='cat_gtin'        id='cat-f-cat_gtin'>"
         "<input type='hidden' name='cat_pkg_uuid'    id='cat-f-cat_pkg_uuid'>"
         "<input type='hidden' name='cat_mat_uuid'    id='cat-f-cat_mat_uuid'>"
         "<input type='hidden' name='cat_brand_uuid'  id='cat-f-cat_brand_uuid'>"
         "<input type='hidden' name='cat_print_min'   id='cat-f-cat_print_min'>"
         "<input type='hidden' name='cat_print_max'   id='cat-f-cat_print_max'>"
         "<input type='hidden' name='cat_bed_min'     id='cat-f-cat_bed_min'>"
         "<input type='hidden' name='cat_bed_max'     id='cat-f-cat_bed_max'>";

    p += "<details id='cat-manual-details'><summary style='color:#9c9;cursor:pointer;margin:10px 0'>"
         "Or enter manually</summary>";

    // Every manual select ends with a "+ Add new ..." option that reveals a
    // hidden panel of plain inputs, submitted alongside it; handleApiOnboard
    // recognises the '__new__' sentinel, builds the Cfg* row from those
    // fields, and appends it to the local catalog table (cfgVendorAdd() and
    // friends) so it is there for next time — same mechanism the catalog
    // search already uses for Vendor, extended to the fields it doesn't
    // reach (Material, Color, Spool profile).
    auto revealOnchange = [&](const char* wrapId) -> String {
        return String(" onchange=\"document.getElementById('cat-source').value='manual';"
                       "document.getElementById('") + wrapId + "').style.display="
                       "this.value=='__new__'?'block':'none'\"";
    };

    // Vendor
    p += "<label>Vendor</label><select name='vendor'" + revealOnchange("vendor-new-wrap") + ">";
    char vbuf[64];
    for (size_t i = 0; i < cfgVendorCount(); i++)
        if (cfgVendorAt(i, vbuf, sizeof(vbuf)))
            p += "<option>" + esc(vbuf) + "</option>";
    p += "<option value='__new__'>&plus; Add new vendor&hellip;</option>";
    p += "</select>";
    p += "<div id='vendor-new-wrap' style='display:none;margin-top:8px'>"
         "<label>New vendor name</label>"
         "<input type='text' name='vendor_new' placeholder='e.g. Filamentive'>"
         "</div>";

    // Material — the "+ Add new" panel needs the full CfgMaterial shape
    // (class/type enums plus print/bed temps), not just a name: those are
    // exactly the fields that silently write 0 C onto a tag if skipped. The
    // enum options below are a static copy of prusa3d/OpenPrintTag's
    // material_class_enum.yaml / material_type_enum.yaml — the same source
    // CATALOG_SCRIPT's type-token matching already uses — kept local so this
    // panel works with no network, matching the whole point of the manual
    // fallback.
    p += "<label>Material</label><select name='material'" + revealOnchange("material-new-wrap") + ">";
    CfgMaterial m;
    for (size_t i = 0; i < cfgMaterialCount(); i++)
        if (cfgMaterialAt(i, m))
            p += "<option>" + esc(m.name) + "</option>";
    p += "<option value='__new__'>&plus; Add new material&hellip;</option>";
    p += "</select>";
    p += "<div id='material-new-wrap' style='display:none;margin-top:8px'>"
         "<label>New material name</label>"
         "<input type='text' name='material_new_name' placeholder='e.g. PETG Carbon Fiber'>"
         "<label>Abbreviation (max 7 chars)</label>"
         "<input type='text' name='material_new_abbr' maxlength='7' placeholder='e.g. PETGCF'>"
         "<label>Class</label><select name='material_new_class'>"
         "<option value='0'>FFF (filament)</option><option value='1'>SLA (resin)</option></select>"
         "<label>Type</label><select name='material_new_type'>";
    static const struct { int8_t key; const char* abbr; } kMatTypes[] = {
        {0,"PLA"},{1,"PETG"},{2,"TPU"},{3,"ABS"},{4,"ASA"},{5,"PC"},{6,"PCTG"},{7,"PP"},
        {8,"PA6"},{9,"PA11"},{10,"PA12"},{42,"PA612"},{11,"PA66"},{12,"CPE"},{13,"TPE"},
        {14,"HIPS"},{15,"PHA"},{16,"PET"},{17,"PEI"},{18,"PBT"},{19,"PVB"},{20,"PVA"},
        {21,"PEKK"},{22,"PEEK"},{23,"BVOH"},{24,"TPC"},{25,"PPS"},{26,"PPSU"},{27,"PVC"},
        {28,"PEBA"},{29,"PVDF"},{30,"PPA"},{31,"PCL"},{32,"PES"},{33,"PMMA"},{34,"POM"},
        {35,"PPE"},{36,"PS"},{37,"PSU"},{38,"TPI"},{39,"SBS"},{40,"OBC"},{41,"EVA"},
    };
    for (const auto& t : kMatTypes)
        p += "<option value='" + String((int)t.key) + "'>" + t.abbr + "</option>";
    p += "</select>"
         "<label>Diameter (mm)</label><input type='number' step='0.01' name='material_new_dia' value='1.75'>"
         "<label>Print temp min/max (&deg;C)</label>"
         "<input type='number' name='material_new_print_min' placeholder='min' style='width:48%;display:inline-block'>"
         " <input type='number' name='material_new_print_max' placeholder='max' style='width:48%;display:inline-block'>"
         "<label>Bed temp min/max (&deg;C)</label>"
         "<input type='number' name='material_new_bed_min' placeholder='min' style='width:48%;display:inline-block'>"
         " <input type='number' name='material_new_bed_max' placeholder='max' style='width:48%;display:inline-block'>"
         "</div>";

    // Color
    p += "<label>Color</label><select name='color'" + revealOnchange("color-new-wrap") + ">";
    CfgColor c;
    for (size_t i = 0; i < cfgColorCount(); i++)
        if (cfgColorAt(i, c))
            p += "<option>" + esc(c.name) + "</option>";
    p += "<option value='__new__'>&plus; Add new colour&hellip;</option>";
    p += "</select>";
    p += "<div id='color-new-wrap' style='display:none;margin-top:8px'>"
         "<label>New colour name</label>"
         "<input type='text' name='color_new_name' placeholder='e.g. Summer Grass'>"
         "<label>Swatch</label><input type='color' name='color_new' value='#808080'>"
         "</div>";

    // Spool profile (fills nominal-full + empty tare)
    p += "<label>Spool profile</label><select name='profile'" + revealOnchange("profile-new-wrap") + ">";
    CfgProfile pr;
    for (size_t i = 0; i < cfgProfileCount(); i++)
        if (cfgProfileAt(i, pr))
            p += "<option>" + esc(pr.label) + "</option>";
    p += "<option value='__new__'>&plus; Add new spool profile&hellip;</option>";
    p += "</select>";
    p += "<div id='profile-new-wrap' style='display:none;margin-top:8px'>"
         "<label>New profile label</label>"
         "<input type='text' name='profile_new_label' placeholder='e.g. Filamentive 1kg PETG'>"
         "<label>Nominal full weight (g)</label><input type='number' step='0.1' name='profile_new_nom'>"
         "<label>Empty spool weight / tare (g)</label><input type='number' step='0.1' name='profile_new_empty'>"
         "</div>";
    p += "</details>";

    // Tare override (optional). "Capture tare" reads the load cell once.
    p += "<label>Empty spool tare (g) &mdash; blank to use profile</label>"
         "<input type='number' step='0.1' name='empty_g' id='tare' placeholder='from profile'>";
    p += "<button type='button' class='sec' onclick=\"fetch('/api/tare',{method:'POST'})"
         ".then(r=>r.json()).then(d=>{document.getElementById('tare').value=d.weight.toFixed(1)})\">"
         "Capture tare from scale</button>";
    p += "</div>";   // #newprod

    p += "<div><button type='submit'>Save &amp; write tag</button></div>";
    p += "</form>";
    p += CATALOG_STYLE;
    p += CATALOG_SCRIPT;
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

    // OPT identity carried through regardless of path, so a catalog-sourced
    // pick's real GTIN/UUIDs (and, when the material file has them, real
    // print/bed temps) reach both the product record and the physical tag
    // instead of being dropped on the floor after resolving. Empty/zero here
    // just means "the source didn't have one" — the tag-write block below
    // already treats that as absent.
    String   pkgUuid, matUuid, brandUuid;
    uint64_t gtin = 0;
    bool     haveCatTemps = false;
    int16_t  catPrintMin = 0, catPrintMax = 0, catBedMin = 0, catBedMax = 0;

    ProductRecord q;
    if (chosen && storeGetProduct(chosen, q)) {
        pid     = q.id;
        vendor  = q.vendor;
        display = q.material;
        abbr    = q.abbr;
        memcpy(rgba, q.rgba, 4);
        dia = q.dia; empty = q.empty_g; nominal = q.nom_g;
        pkgUuid = q.pkg_uuid; matUuid = q.mat_uuid; brandUuid = q.brand_uuid; gtin = q.gtin;
        // The tag also wants print/bed temps and the material class/type, which
        // live on the config catalog rather than on the product. Look them up
        // by abbreviation; if there is no matching row the tag simply keeps the
        // temps it already had, which is what happens today for a foreign tag.
        haveMat = cfgMaterialByName(abbr.c_str(), m);
    } else if (arg("source") == "catalog") {
        // Picked from the OpenPrintTag catalog search (see CATALOG_SCRIPT) —
        // the browser already resolved package -> material + container + brand
        // against the live DB, so these are the vendor's own published values,
        // not a local guess. No cfgMaterialByName/cfgColorByName lookup needed;
        // the catalog gave us the real numbers directly.
        float tareOvr = arg("empty_g").toFloat();   // 0 → use catalog's own tare

        vendor  = arg("cat_vendor");
        display = arg("cat_material");
        abbr    = arg("cat_abbr");
        String rgbaHex = arg("cat_rgba");
        if (rgbaHex.length() == 8)
            for (int i = 0; i < 4; i++)
                rgba[i] = (uint8_t)strtol(rgbaHex.substring(i * 2, i * 2 + 2).c_str(), nullptr, 16);

        dia     = arg("cat_dia").toFloat();
        if (dia <= 0.0f) dia = 1.75f;
        nominal = arg("cat_nom_g").toFloat();
        empty   = (tareOvr > 0.0f) ? tareOvr : arg("cat_empty_g").toFloat();

        pkgUuid   = arg("cat_pkg_uuid");
        matUuid   = arg("cat_mat_uuid");
        brandUuid = arg("cat_brand_uuid");
        gtin      = strtoull(arg("cat_gtin").c_str(), nullptr, 10);

        haveCatTemps = arg("cat_print_min").length() > 0;
        if (haveCatTemps) {
            catPrintMin = (int16_t)arg("cat_print_min").toInt();
            catPrintMax = (int16_t)arg("cat_print_max").toInt();
            catBedMin   = (int16_t)arg("cat_bed_min").toInt();
            catBedMax   = (int16_t)arg("cat_bed_max").toInt();
        }
        // Local material class/type enum still comes from the config catalog
        // (the OPT database's class/type strings aren't mapped to our enum) —
        // by abbreviation, same lookup the manual path uses.
        haveMat = cfgMaterialByName(abbr.c_str(), m);

        ProductRecord prod;
        strlcpy(prod.vendor,     vendor.c_str(),    sizeof(prod.vendor));
        strlcpy(prod.material,   display.c_str(),   sizeof(prod.material));
        strlcpy(prod.abbr,       abbr.c_str(),       sizeof(prod.abbr));
        strlcpy(prod.pkg_uuid,   pkgUuid.c_str(),    sizeof(prod.pkg_uuid));
        strlcpy(prod.mat_uuid,   matUuid.c_str(),    sizeof(prod.mat_uuid));
        strlcpy(prod.brand_uuid, brandUuid.c_str(),  sizeof(prod.brand_uuid));
        prod.gtin = gtin;
        memcpy(prod.rgba, rgba, 4);
        prod.dia = dia; prod.empty_g = empty; prod.nom_g = nominal;
        ProductRecord found;
        if (storeFindProduct(prod, found))      pid = found.id;
        else if (storeUpsertProduct(prod))      pid = prod.id;
    } else {
        String matName  = arg("material");
        String colName  = arg("color");
        String profName = arg("profile");
        float  tareOvr  = arg("empty_g").toFloat();   // 0 → use profile

        // "+ Add new ..." sentinel from the onboard form (see handleOnboardForm)
        // — build the Cfg* row from the revealed fields and append it to the
        // local catalog so it is there for next time, same as a typed vendor
        // already was via cfgVendorAdd() below.
        vendor = arg("vendor");
        if (vendor == "__new__") vendor = arg("vendor_new");

        if (matName == "__new__") {
            CfgMaterial nm = {};
            strlcpy(nm.name, arg("material_new_name").c_str(), sizeof(nm.name));
            strlcpy(nm.abbr, arg("material_new_abbr").c_str(), sizeof(nm.abbr));
            nm.cls       = (int8_t)arg("material_new_class").toInt();
            nm.type      = (int8_t)arg("material_new_type").toInt();
            nm.dia       = arg("material_new_dia").toFloat();
            if (nm.dia <= 0.0f) nm.dia = 1.75f;
            nm.print_min = (int16_t)arg("material_new_print_min").toInt();
            nm.print_max = (int16_t)arg("material_new_print_max").toInt();
            nm.bed_min   = (int16_t)arg("material_new_bed_min").toInt();
            nm.bed_max   = (int16_t)arg("material_new_bed_max").toInt();
            cfgMaterialAdd(nm);
            matName = nm.name;
            m = nm; haveMat = true;
        } else {
            haveMat = cfgMaterialByName(matName.c_str(), m);
        }

        CfgColor col = {};
        if (colName == "__new__") {
            CfgColor nc = {};
            strlcpy(nc.name, arg("color_new_name").c_str(), sizeof(nc.name));
            String hex = arg("color_new");   // "#rrggbb" from <input type='color'>
            if (hex.length() == 7 && hex[0] == '#') {
                nc.rgba[0] = (uint8_t)strtol(hex.substring(1, 3).c_str(), nullptr, 16);
                nc.rgba[1] = (uint8_t)strtol(hex.substring(3, 5).c_str(), nullptr, 16);
                nc.rgba[2] = (uint8_t)strtol(hex.substring(5, 7).c_str(), nullptr, 16);
                nc.rgba[3] = 255;
            }
            cfgColorAdd(nc);
            colName = nc.name;
            col = nc;
        } else {
            cfgColorByName(colName.c_str(), col);
        }
        memcpy(rgba, col.rgba, 4);

        CfgProfile pr = {};
        bool havePr = false;
        if (profName == "__new__") {
            CfgProfile np = {};
            strlcpy(np.label, arg("profile_new_label").c_str(), sizeof(np.label));
            np.nominal_full_g = arg("profile_new_nom").toFloat();
            np.empty_g        = arg("profile_new_empty").toFloat();
            cfgProfileAdd(np);
            profName = np.label;
            pr = np; havePr = true;
        } else {
            for (size_t i = 0; i < cfgProfileCount(); i++)
                if (cfgProfileAt(i, pr) && profName == pr.label) { havePr = true; break; }
        }

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
        // Catalog-sourced temps are the vendor's own published numbers, more
        // specific than the config catalog's per-type defaults, so they win
        // when present. Class/type enums still come from the local catalog
        // either way — the OPT database's strings aren't mapped to our enum.
        if (haveCatTemps) {
            gTagMain.min_print_temperature = catPrintMin;
            gTagMain.max_print_temperature = catPrintMax;
            gTagMain.min_bed_temperature   = catBedMin;
            gTagMain.max_bed_temperature   = catBedMax;
        } else if (haveMat) {
            gTagMain.min_print_temperature = m.print_min;
            gTagMain.max_print_temperature = m.print_max;
            gTagMain.min_bed_temperature   = m.bed_min;
            gTagMain.max_bed_temperature   = m.bed_max;
        }
        if (haveMat) {
            gTagMain.material_class = m.cls;
            gTagMain.material_type  = m.type;
        }
        // Real vendor identity (GTIN, the three OPT UUIDs) — set unconditionally,
        // same as every other identity field above. This is a REPLACE, not a
        // merge: a manual/no-uuid submission must clear whatever a previous
        // catalog-sourced write left behind, or the tag ends up asserting a
        // vendor identity (a real package_uuid/GTIN) underneath data someone
        // just typed by hand for a different product entirely — worse than
        // never having written one, since it reads as verified when it isn't.
        // A nil UUID/zero GTIN is simply omitted on the next encode
        // (optUuidIsNil), which is the correct "no identity asserted" state.
        uint8_t idBytes[16];
        memset(gTagMain.package_uuid,  0, sizeof(gTagMain.package_uuid));
        memset(gTagMain.material_uuid, 0, sizeof(gTagMain.material_uuid));
        memset(gTagMain.brand_uuid,    0, sizeof(gTagMain.brand_uuid));
        if (hex32ToBytes(pkgUuid,   idBytes)) memcpy(gTagMain.package_uuid,  idBytes, 16);
        if (hex32ToBytes(matUuid,   idBytes)) memcpy(gTagMain.material_uuid, idBytes, 16);
        if (hex32ToBytes(brandUuid, idBytes)) memcpy(gTagMain.brand_uuid,    idBytes, 16);
        gTagMain.gtin = gtin;
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
         "or e-mail the list to place the order. Manage what's tracked (add, edit, "
         "remove) on the <a href='/stock' style='color:#8f8'>Stock List</a> page. A row "
         "matched by name rather than a product below means <a href='/products' "
         "style='color:#8f8'>Products</a> is worth a look.</p>";
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
                   : s.min_grams  > 0 ? (String(s.min_grams, 0) + " grams")
                   : String("empty");
        p += "<tr><td>" + esc(s.vendor) + "</td><td>" + esc(s.material) + "</td><td>"
           + esc(s.color) + "</td><td>" + String(oh.count) + " spool(s), "
           + String(oh.grams, 0) + " grams</td><td>" + thr + "</td><td>"
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

// ── Stock items: per-row add/edit/remove for what /reorder tracks ─────────────
// The friendlier counterpart to the raw-JSON "Stock items" textarea on
// /config, which still exists for bulk edits and import/export. Rows are
// addressed by position (cfgStockAt()'s index), same as the rest of this
// table's API — valid for the lifetime of one page load, which is fine here
// since there is one operator at a time, not concurrent editors.
static void stockFormFields(String& p, const CfgStock& s) {
    p += "<label>Vendor</label><input type='text' name='vendor' value='" + esc(s.vendor) + "' required>";
    p += "<label>Material (bare type, e.g. PLA)</label>"
         "<input type='text' name='material' value='" + esc(s.material) + "'>";
    p += "<label>Color</label><input type='text' name='color' value='" + esc(s.color) + "'>";
    p += "<label>Diameter (mm)</label>"
         "<input type='number' step='0.01' name='dia' value='" + String(s.dia > 0 ? s.dia : 1.75f, 2) + "'>";
    p += "<label>Nominal spool weight (g)</label>"
         "<input type='number' step='0.1' name='spool_g' value='" + String(s.spool_g > 0 ? s.spool_g : 1000.0f, 0) + "'>";
    p += "<label>Minimum spools to keep &mdash; 0 = use grams instead</label>"
         "<input type='number' name='min_spools' value='" + String(s.min_spools) + "'>";
    p += "<label>Minimum grams to keep &mdash; 0 = use spools; both 0 means at least 1 spool</label>"
         "<input type='number' step='0.1' name='min_grams' value='" + String(s.min_grams, 0) + "'>";
    p += "<label>SKU</label><input type='text' name='sku' value='" + esc(s.sku) + "'>";
    p += "<label>GTIN</label><input type='text' name='gtin' value='" + esc(s.gtin) + "'>";
    p += "<label>Pack quantity</label>"
         "<input type='number' name='pack_qty' value='" + String(s.pack_qty > 0 ? s.pack_qty : 1) + "'>";
}

// STOCK_POPULARITY_WINDOW_DAYS itself lives in config.h now, shared with
// storeCompact()'s retention floor -- see the comment there.

static void handleStockPage(AsyncWebServerRequest* req) {
    String p = head("Stock List", "/stock");

    long editIdx = -1;
    if (req->hasParam("edit")) editIdx = req->getParam("edit")->value().toInt();
    CfgStock editRow{};
    bool haveEdit = editIdx >= 0 && cfgStockAt((size_t)editIdx, editRow);

    p += "<h3>" + String(haveEdit ? "Edit stock item" : "Add a stock item") + "</h3>";
    p += "<p class='muted'>What you want to keep on the shelf, and how low it can go "
         "before <a href='/reorder' style='color:#8f8'>Reorder</a> flags it.</p>";
    p += "<div class='card'>";
    p += "<form method='POST' action='" + String(haveEdit ? "/api/stock/update" : "/api/stock/add") + "'>";
    if (haveEdit) p += "<input type='hidden' name='index' value='" + String((long)editIdx) + "'>";
    stockFormFields(p, editRow);
    p += "<div><button type='submit'>" + String(haveEdit ? "Save changes" : "Add") + "</button>";
    if (haveEdit) p += " <a href='/stock' style='margin-left:14px;color:#8f8'>Cancel</a>";
    p += "</div></form></div>";

    p += "<h3>Currently tracked</h3>";
    p += "<p class='muted'>Sorted by popularity, lowest first &mdash; the top of this "
         "list is where to look for items to remove. Popularity is grams consumed per "
         "day the material was actually IN STOCK over the last " + String(STOCK_POPULARITY_WINDOW_DAYS)
       + " days, not per calendar day &mdash; a spool that sold out early and sat empty "
         "is scored on the days it was available, not diluted across the ones it wasn't, "
         "so running out never makes something look unpopular.</p>";

    CfgStock s;
    const size_t n = cfgStockCount();
    std::vector<CfgStock> rows(n);
    std::vector<PopularityQuery> q(n);
    for (size_t i = 0; i < n; i++) {
        cfgStockAt(i, rows[i]);
        strlcpy(q[i].vendor, rows[i].vendor, sizeof(q[i].vendor));
        // Same composition Reorder's rollUp() uses: CfgStock.material is the bare
        // type, colour rides separately, but consumption is keyed on the full
        // display string a spool's Main section actually carries.
        String disp = rows[i].material;
        if (rows[i].color[0]) disp += String(" ") + rows[i].color;
        strlcpy(q[i].material, disp.c_str(), sizeof(q[i].material));
    }
    std::vector<PopularityResult> pop(n);
    char earliestTs[25] = {};
    if (n) storeMaterialPopularity(q.data(), pop.data(), n, STOCK_POPULARITY_WINDOW_DAYS,
                                    earliestTs, sizeof(earliestTs));

    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        // No-data sorts first (the clearest removal candidates), then by rate.
        if (pop[a].has_data != pop[b].has_data) return !pop[a].has_data;
        return (pop[a].grams / (pop[a].available_days > 0 ? pop[a].available_days : 1.0f))
             < (pop[b].grams / (pop[b].available_days > 0 ? pop[b].available_days : 1.0f));
    });

    p += "<table><tr><th>Vendor</th><th>Material</th><th>Color</th><th>Popularity</th>"
         "<th>Threshold</th><th>SKU</th><th>Pack</th><th></th></tr>";
    for (size_t i : order) {
        const CfgStock& sr = rows[i];
        String thr = sr.min_spools > 0 ? (String(sr.min_spools) + " spools")
                   : sr.min_grams  > 0 ? (String(sr.min_grams, 0) + " grams")
                   : String("&ge; 1 spool");
        String popCell;
        if (!pop[i].has_data) {
            popCell = "<span class='ob'>no data</span>";
        } else {
            const float perWeek = pop[i].grams / pop[i].available_days * 7.0f;
            popCell = String(perWeek, 0) + " g/wk";
        }
        p += "<tr><td>" + esc(sr.vendor) + "</td><td>" + esc(sr.material) + "</td><td>"
           + esc(sr.color) + "</td><td>" + popCell + "</td><td>" + thr + "</td><td>"
           + esc(sr.sku) + "</td><td>" + String(sr.pack_qty) + "</td><td style='white-space:nowrap'>"
           + "<a href='/stock?edit=" + String((unsigned)i) + "' style='color:#8f8'>Edit</a> "
           + "<form method='POST' action='/api/stock/delete' style='display:inline' "
             "onsubmit=\"return confirm('Remove this stock item? This cannot be undone.')\">"
           + "<input type='hidden' name='index' value='" + String((unsigned)i) + "'>"
           + "<button type='submit' style='background:#522;color:#eee;margin:0;padding:4px 10px;"
             "font-size:13px;border-radius:4px;border:0;cursor:pointer'>Remove</button>"
           + "</form></td></tr>";
    }
    if (n == 0)
        p += "<tr><td colspan='8' class='muted'>Nothing tracked yet &mdash; add one above.</td></tr>";
    p += "</table>";
    if (n && earliestTs[0]) {
        char local[32];
        storeLocalizeIso(earliestTs, local, sizeof(local));
        p += "<p class='muted'>Popularity data available since "
           + esc(local[0] ? local : earliestTs) + ".</p>";
    }
    p += "<p class='muted'>Bulk edit, or export/import all five Config tables together, "
         "on the <a href='/config' style='color:#8f8'>Settings</a> and "
         "<a href='/backup' style='color:#8f8'>Backup</a> pages.</p>";
    p += FOOT;
    req->send(200, "text/html", p);
}

static CfgStock parseStockForm(AsyncWebServerRequest* req) {
    auto arg = [&](const char* k) -> String {
        const AsyncWebParameter* pp = req->getParam(k, true);
        return pp ? pp->value() : String();
    };
    CfgStock s{};
    strlcpy(s.vendor,   arg("vendor").c_str(),   sizeof(s.vendor));
    strlcpy(s.material, arg("material").c_str(), sizeof(s.material));
    strlcpy(s.color,    arg("color").c_str(),    sizeof(s.color));
    s.dia = arg("dia").toFloat();
    if (s.dia <= 0.0f) s.dia = 1.75f;
    s.spool_g    = arg("spool_g").toFloat();
    s.min_spools = (uint16_t)arg("min_spools").toInt();
    s.min_grams  = arg("min_grams").toFloat();
    strlcpy(s.sku,  arg("sku").c_str(),  sizeof(s.sku));
    strlcpy(s.gtin, arg("gtin").c_str(), sizeof(s.gtin));
    s.pack_qty = (uint8_t)arg("pack_qty").toInt();
    if (s.pack_qty == 0) s.pack_qty = 1;
    return s;
}

static void handleApiStockAdd(AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    cfgStockAdd(parseStockForm(req));
    req->redirect("/stock");
}

static void handleApiStockUpdate(AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    const AsyncWebParameter* pi = req->getParam("index", true);
    if (!pi) { req->send(400, "text/plain", "missing index"); return; }
    if (!cfgStockUpdate((size_t)pi->value().toInt(), parseStockForm(req))) {
        req->send(400, "text/plain", "invalid index");
        return;
    }
    req->redirect("/stock");
}

static void handleApiStockDelete(AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    const AsyncWebParameter* pi = req->getParam("index", true);
    if (!pi) { req->send(400, "text/plain", "missing index"); return; }
    if (!cfgStockRemove((size_t)pi->value().toInt())) {
        req->send(400, "text/plain", "invalid index");
        return;
    }
    req->redirect("/stock");
}

// Read-only: every Stock List row plus its popularity over the same trailing
// STOCK_POPULARITY_WINDOW_DAYS window /stock itself shows. Open, like the
// other /api/* reads — no key needed.
static void handleApiStock(AsyncWebServerRequest* req) {
    const size_t n = cfgStockCount();
    std::vector<CfgStock> rows(n);
    std::vector<PopularityQuery> q(n);
    for (size_t i = 0; i < n; i++) {
        cfgStockAt(i, rows[i]);
        strlcpy(q[i].vendor, rows[i].vendor, sizeof(q[i].vendor));
        String disp = rows[i].material;
        if (rows[i].color[0]) disp += String(" ") + rows[i].color;
        strlcpy(q[i].material, disp.c_str(), sizeof(q[i].material));
    }
    std::vector<PopularityResult> pop(n);
    char earliestTs[25] = {};
    if (n) storeMaterialPopularity(q.data(), pop.data(), n, STOCK_POPULARITY_WINDOW_DAYS,
                                    earliestTs, sizeof(earliestTs));

    String j = "{\"window_days\":" + String(STOCK_POPULARITY_WINDOW_DAYS)
             + ",\"data_since\":" + (earliestTs[0] ? ("\"" + String(earliestTs) + "\"") : String("null"))
             + ",\"items\":[";
    for (size_t i = 0; i < n; i++) {
        if (i) j += ",";
        const CfgStock& s = rows[i];
        j += "{\"vendor\":\"" + esc(s.vendor) + "\""
           + ",\"material\":\"" + esc(s.material) + "\""
           + ",\"color\":\"" + esc(s.color) + "\""
           + ",\"dia\":" + String(s.dia, 2)
           + ",\"spool_g\":" + String(s.spool_g, 1)
           + ",\"min_spools\":" + String(s.min_spools)
           + ",\"min_grams\":" + String(s.min_grams, 1)
           + ",\"sku\":\"" + esc(s.sku) + "\""
           + ",\"gtin\":\"" + esc(s.gtin) + "\""
           + ",\"pack_qty\":" + String(s.pack_qty)
           + ",\"has_data\":" + (pop[i].has_data ? "true" : "false")
           + ",\"grams_in_window\":" + String(pop[i].grams, 1)
           + ",\"available_days\":" + String(pop[i].available_days, 5)
           + ",\"grams_per_week\":" + (pop[i].has_data
                 ? String(pop[i].grams / pop[i].available_days * 7.0f, 1) : String("null"))
           + "}";
    }
    j += "]}";
    req->send(200, "application/json", j);
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
    String p = head("Settings", "/config");

    // Station name: the Idle screen's green heading, and the only place on
    // the whole device that greets by the owning org's name -- everything
    // else (every other TFT title, the web app's own header) is deliberately
    // generic. Configurable because this firmware is meant to run at more
    // than one lab eventually.
    p += "<h3>Station name</h3><div class='card'>";
    p += "<p class='muted'>Shown on the idle screen. Max " + String(STATION_NAME_MAX_LEN)
       + " characters -- the title has no wrap.</p>";
    p += "<input id='snin' type='text' maxlength='" + String(STATION_NAME_MAX_LEN)
       + "' value='" + esc(stationNameGet()) + "'>"
         "<div style='margin-top:8px'><button type='button' onclick='ssn()'>Save name</button></div>"
         "<p class='muted' id='snmsg'></p></div>";
    p += "<script>"
         "function ssn(){var n=document.getElementById('snin').value;"
         "fetch('/api/station-name',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
         "body:'name='+encodeURIComponent(n)})"
         ".then(r=>{document.getElementById('snmsg').textContent="
         "r.ok?'Saved. Takes effect immediately.':'Refused (HTTP '+r.status+') — "
         "check it is not empty or over " + String(STATION_NAME_MAX_LEN) + " characters.';});}"
         "</script>";

    // Network. Nothing in the app offered this before — /reset existed but was
    // reachable only by typing the URL, so a station that had fallen back to
    // its own AP (the setup window closes after WIFI_PORTAL_TIMEOUT_SEC with
    // nobody connected) looked from the outside like it had no way to be put on
    // a network at all. Which is what happened the first time it was carried to
    // the lab.
    p += "<h3>Network</h3><div class='card'>";
    if (gApSsid[0])
        p += "<p><b class='ob'>Not on a network.</b> This page is being served "
             "from the station's own access point <b>" + esc(gApSsid) + "</b>, so "
             "nobody else can reach it and <code>weighstation.local</code> will "
             "not resolve.</p>";
    else
        p += "<p>Connected to <b>" + esc(WiFi.SSID().c_str()) + "</b> as <b>"
           + WiFi.localIP().toString() + "</b>.</p>";
    p += "<p class='muted'>Choosing a network clears the stored credentials and "
         "reboots into the setup portal. Join <b>WeighStation-Setup</b> from a "
         "phone within " + String(WIFI_PORTAL_TIMEOUT_SEC) + " seconds of the "
         "reboot &mdash; once you are connected the timer stops, so there is no "
         "rush after that. The station is 2.4 GHz only and cannot join "
         "WPA2-Enterprise networks.</p>";
    p += "<form method='POST' action='/reset'>"
         "<button type='submit' class='sec'>Choose a WiFi network&hellip;</button>"
         "</form></div>";

    // A setup/maintenance task like Network above, not a shelf-facing page —
    // dropped off the top nav for the same reason Products did. Unlike
    // Products, there's already a built-in "come here now" surface for this
    // one: Inventory shows a "Scale not calibrated" banner linking straight
    // to /calibrate whenever gScaleCalibrated is false, so this link is for
    // the proactive/maintenance case (drift, after moving the station).
    p += "<h3>Scale calibration</h3><div class='card'>";
    if (gScaleCalibrated)
        p += "<p>Scale is calibrated.</p>";
    else
        p += "<p><b class='ob'>Not calibrated.</b> Weights will be wrong until "
             "this is done.</p>";
    p += "<a href='/calibrate'><button type='button' class='sec'>Calibrate&hellip;</button></a></div>";

    // Display-only: the log itself always stores UTC (storeNowIso(), gmtime_r)
    // so it stays unambiguous and string-sortable across a DST transition —
    // the popularity window math, the audit "found" comparison, and
    // storeCompact()'s retention floor all depend on that. Changing this only
    // changes what the TFT corner clock and the timestamps on this site show.
    p += "<h3>Display timezone</h3><div class='card'>";
    p += "<p class='muted'>Affects the station's on-screen clock and every "
         "timestamp shown in the web app (weigh history, popularity dates). "
         "The event log itself always records UTC, so backups and the API "
         "are unaffected and never need converting.</p>";
    p += "<select id='tzsel'>";
    for (size_t i = 0; i < tzZoneCount(); i++) {
        const TzZone& z = tzZoneAt(i);
        p += "<option value='" + String(z.id) + "'"
           + (!strcmp(z.id, tzGetId()) ? " selected" : "") + ">"
           + esc(z.label) + "</option>";
    }
    p += "</select>";
    p += "<label>Clock format</label><select id='tzh24'>"
         "<option value='0'" + String(tzGet24Hour() ? "" : " selected")
       + ">12-hour (2:30 PM)</option>"
         "<option value='1'" + String(tzGet24Hour() ? " selected" : "")
       + ">24-hour (14:30)</option></select>";
    p += "<div style='margin-top:8px'><button type='button' onclick='stz()'>Save timezone</button></div>"
         "<p class='muted' id='tzmsg'></p></div>";
    p += "<script>"
         "function stz(){var z=document.getElementById('tzsel').value;"
         "var h=document.getElementById('tzh24').value;"
         "fetch('/api/tz',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
         "body:'tz='+encodeURIComponent(z)+'&h24='+encodeURIComponent(h)})"
         ".then(r=>{document.getElementById('tzmsg').textContent="
         "r.ok?'Saved. Takes effect immediately.':'Refused (HTTP '+r.status+').';});}"
         "</script>";

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

    // Products dropped off the top nav — it's a diagnostic/admin list (is
    // adoption converging, edit a product's definition), not a shelf-facing
    // page like Inventory/Reorder/Onboard, and belongs with the other
    // occasional-admin surfaces on this page. A spool's own detail page
    // already links straight to ITS product's edit page for the common
    // "fix this one spool's product" case; this is for browsing everything.
    p += "<h3>Products</h3>"
         "<p class='muted'>What we stock, and whether tag adoption is converging "
         "on it correctly — two spools of the same filament should be one entry, "
         "not two. Also the only place a product's definition can be edited.</p>"
         "<a href='/products'><button type='button' class='sec'>Products</button></a>";

    // Raw-JSON catalog tables — the bulk-edit / power-user path. Everyday
    // add/edit/remove for Stock items lives on the friendlier /stock page;
    // these stay here for the tables that don't have one yet, and for
    // pasting in a bulk edit. Kept last since Network and API access are
    // what most visits to this page are actually for.
    p += "<h3>Config catalog</h3>";
    p += "<p class='muted'>Edit the reference tables that drive onboarding and "
         "reordering. Each is a JSON array; Save validates and persists it. "
         "Stock items has a friendlier editor, Stock List, at "
         "<a href='/stock' style='color:#8f8'>/stock</a>.</p>";
    configTableForm(p, "Vendors",        "vendors");
    configTableForm(p, "Materials",      "materials");
    configTableForm(p, "Spool profiles", "spool-profiles");
    configTableForm(p, "Colors",         "colors");
    configTableForm(p, "Stock items",    "stock-items");

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

static void handleApiTzSet(AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    String z;
    if (req->hasParam("tz", true))      z = req->getParam("tz", true)->value();
    else if (req->hasParam("tz"))       z = req->getParam("tz")->value();
    if (!tzSet(z.c_str())) { req->send(400, "text/plain", "unknown timezone id"); return; }

    String h;
    if (req->hasParam("h24", true))      h = req->getParam("h24", true)->value();
    else if (req->hasParam("h24"))       h = req->getParam("h24")->value();
    if (h.length()) tzSet24Hour(h == "1");

    req->send(200, "application/json",
              String("{\"ok\":true,\"tz\":\"") + tzGetId()
            + "\",\"h24\":" + (tzGet24Hour() ? "true" : "false") + "}");
}

static void handleApiStationName(AsyncWebServerRequest* req) {
    if (!authOk(req)) return;
    String n;
    if (req->hasParam("name", true))      n = req->getParam("name", true)->value();
    else if (req->hasParam("name"))       n = req->getParam("name")->value();
    if (!stationNameSet(n.c_str())) {
        req->send(400, "text/plain", "name must be 1-" + String(STATION_NAME_MAX_LEN) + " characters");
        return;
    }
    // Not echoing the saved name back: it is arbitrary user text and esc()
    // is HTML-escaping, not JSON-escaping, so embedding it here the way
    // handleApiTzSet() embeds tzGetId() (always one of a fixed set of safe
    // internal ids) would be wrong for a value someone actually typed.
    req->send(200, "application/json", "{\"ok\":true}");
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

// ── Config catalog backup/restore — a SEPARATE artifact from the event log.
// Nothing in a SpoolRecord/ProductRecord points back into these tables by ID
// (vendor/material/colour/temps are values, copied once at onboarding), so
// this file and the event-log export never need to be the same age, and
// restoring one has no effect on the other. Small enough (a few KB) that the
// streaming-upload dance below is more about matching the log-import pattern
// than about needing it, but ESPAsyncWebServer's multipart handling always
// goes through the upload callback regardless of size.
static const char* CONFIG_IMPORT_STAGING = "/config/import.staging";

static void handleConfigExport(AsyncWebServerRequest* req) {
    AsyncWebServerResponse* r =
        req->beginResponse(200, "application/json", cfgExportAll());
    r->addHeader("Content-Disposition", "attachment; filename=weighstation-config.json");
    req->send(r);
}

static bool sConfigImportAuthed = false;

static void handleConfigImportUpload(AsyncWebServerRequest* req, const String& filename,
                                      size_t index, uint8_t* data, size_t len, bool final) {
    if (index == 0) sConfigImportAuthed = !apiKeyIsSet() || authOk(req);
    if (!sConfigImportAuthed) return;
    File f = LittleFS.open(CONFIG_IMPORT_STAGING, index == 0 ? "w" : "a");
    if (f) { f.write(data, len); f.close(); }
}

static void handleConfigImportDone(AsyncWebServerRequest* req) {
    if (!sConfigImportAuthed) { LittleFS.remove(CONFIG_IMPORT_STAGING); return; }
    if (!authOk(req)) return;
    // Buffered read, not File::readString() -- that reads one byte per VFS
    // call (Stream::timedRead() in a loop), the same trap CLAUDE.md's log-
    // reading gotcha warns about. This file is small, but there is no reason
    // to reintroduce the pattern the project already paid to learn to avoid.
    String body;
    File f = LittleFS.open(CONFIG_IMPORT_STAGING, "r");
    if (f) {
        body.reserve(f.size());
        uint8_t buf[513];
        size_t n;
        while ((n = f.read(buf, sizeof(buf) - 1)) > 0) {
            buf[n] = 0;
            body += (const char*)buf;
        }
        f.close();
    }
    LittleFS.remove(CONFIG_IMPORT_STAGING);
    if (cfgImportAll(body)) {
        String p = head("Restore", "/backup");
        p += "<div class='card'><p>Config catalog restored &mdash; "
             + String((unsigned)cfgVendorCount())   + " vendors, "
             + String((unsigned)cfgMaterialCount()) + " materials, "
             + String((unsigned)cfgProfileCount())  + " spool profiles, "
             + String((unsigned)cfgColorCount())    + " colors, "
             + String((unsigned)cfgStockCount())    + " stock items.</p>"
             "<a href='/config'><button type='button'>Back to Settings</button></a></div>";
        p += FOOT;
        req->send(200, "text/html", p);
    } else {
        req->send(400, "text/plain",
                   "Import failed: expected a JSON object with vendors/materials/"
                   "spool-profiles/colors/stock-items arrays");
    }
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
    // git describe at build time (tag-commits-hash[-dirty]) -- FW_VERSION
    // above is a hand-maintained semver that hasn't moved since 1.0.0; this
    // is the one that actually answers "which commit is flashed on THIS unit".
    j += "\",\"build_version\":\""; j += BUILD_VERSION;
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

    // Absent means "we do not know what month anything happened in", which a
    // dashboard needs to be able to say out loud rather than plotting 1970.
    j += ",\"clock_set\":"; j += gClockSet ? "true" : "false";
    j += ",\"auth\":{\"required\":"; j += apiKeyIsSet() ? "true" : "false";
    j += "}}";
    req->send(200, "application/json", j);
}

static void handleUsagePage(AsyncWebServerRequest* req) {
    String p = head("Usage", "/usage");
    p += "<h3>Filament consumed</h3>";
    p += "<p class='muted'>This is the lab's permanent consumption record "
         "&mdash; how much of each material has actually gone through the "
         "station, by month and year, since tracking began. Once an entry "
         "lands here it's permanent &mdash; even the raw weigh events it's "
         "built from eventually get folded away, but these totals don't.</p>";

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

    // Yearly rollup, same shape as "by month" below but grouped one level
    // coarser — drilling down from the all-time summary above. Rows with no
    // synced clock at the time (period == "unknown") stay their own bucket
    // rather than being sliced into a fake year.
    p += "<h3>By year</h3><table>"
         "<tr><th>Year</th><th>Vendor</th><th>Material</th><th>Consumed</th><th>Weighs</th></tr>";
    struct YKey { String year, vendor, material; };
    struct YTot { YKey k; float g; uint32_t w; };
    std::vector<YTot> byYear;
    for (size_t i = 0; i < n; i++) {
        if (!storeUsageAt(i, u)) continue;
        String yr = strcmp(u.period, "unknown") == 0 ? String("unknown")
                                                       : String(u.period).substring(0, 4);
        bool found = false;
        for (auto& t : byYear)
            if (t.k.year == yr && t.k.vendor == u.vendor && t.k.material == u.material) {
                t.g += u.grams; t.w += u.weighs; found = true; break;
            }
        if (!found) byYear.push_back({{yr, String(u.vendor), String(u.material)}, u.grams, u.weighs});
    }
    std::sort(byYear.begin(), byYear.end(), [](const YTot& a, const YTot& b) {
        int c = strcmp(b.k.year.c_str(), a.k.year.c_str());   // newest year first
        return c ? (c < 0) : (a.g > b.g);
    });
    for (const auto& t : byYear) {
        p += "<tr><td>" + esc(t.k.year.c_str()) + "</td><td>" + esc(t.k.vendor.c_str())
           + "</td><td>" + esc(t.k.material.c_str()) + "</td><td>" + String(t.g, 0)
           + " grams</td><td>" + String((unsigned)t.w) + "</td></tr>";
    }
    p += "</table>";

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
           + " grams</td><td>" + String((unsigned)u.weighs) + "</td></tr>";
    }
    p += "</table>";
    p += "<p><a href='/usage.csv'><button type='button'>Download CSV</button></a></p>";
    p += "<p class='muted'>Consumption is the drop in a spool's remaining weight "
         "between two weighings, attributed to the vendor and material recorded "
         "at that time; a spool's first weighing establishes its baseline and "
         "counts as zero.</p>";
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

    // One click, two files -- NOT one combined file. The event log and the
    // config catalog stay independent artifacts (see the Config catalog card
    // below for why); this button is pure convenience for "back up
    // everything before I do something risky" without picking each download
    // separately. Both requests already set Content-Disposition with real
    // filenames, so the browser saves two distinctly-named files. Most
    // browsers ask permission once for "this site wants to download multiple
    // files" the first time a page does this -- expected, not a bug.
    p += "<div class='card'><label style='margin-top:0'>Everything</label>"
         "<p class='muted'>Downloads the event log and the config catalog in one "
         "click &mdash; still two separate files, saved together for convenience.</p>"
         "<button type='button' onclick=\""
         "var a=document.createElement('a');a.href='/export';document.body.appendChild(a);a.click();a.remove();"
         "var b=document.createElement('a');b.href='/config/export';document.body.appendChild(b);b.click();b.remove();"
         "\">Download everything</button></div>";

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

    // A separate artifact on purpose — see cfgExportAll()'s comment in
    // config_store.h. Nothing here needs to be the same age as the log above.
    p += "<h3>Config catalog</h3>";
    p += "<div class='card'><label style='margin-top:0'>Download</label>"
         "<p class='muted'>Vendors, materials, spool profiles, colors and stock "
         "items &mdash; the reference tables that drive onboarding and reordering. "
         "Independent of the event log above: nothing in a spool or product record "
         "points back into these by ID, so the two backups never need to match.</p>"
         "<a href='/config/export'><button type='button'>Download config catalog</button></a></div>";
    p += "<div class='card'><label style='margin-top:0'>Restore</label>"
         "<p class='muted'>Upload a previously downloaded config catalog to replace "
         "all five tables. Validated first &mdash; a missing or malformed table "
         "rejects the whole file and leaves the current tables untouched.</p>"
         "<form method='POST' action='/config/import' enctype='multipart/form-data'>"
         "<input type='file' name='file' accept='.json,application/json'>"
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
    String p = head("Calibrate", "/config");
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
    sServer.on("/spool",       HTTP_GET,  handleSpoolDetail);
    sServer.on("/api/spools",  HTTP_GET,  handleApiSpools);
    sServer.on("/api/audit/start",   HTTP_POST, handleApiAuditStart);
    sServer.on("/api/audit/finish",  HTTP_POST, handleApiAuditFinish);
    sServer.on("/api/audit/abandon", HTTP_POST, handleApiAuditAbandon);
    sServer.on("/api/audit/close",   HTTP_POST, handleApiAuditClose);
    sServer.on("/api/audit/found",   HTTP_POST, handleApiAuditFound);
    sServer.on("/products",     HTTP_GET,  handleProducts);
    sServer.on("/product",      HTTP_GET,  handleProductDetail);
    sServer.on("/api/products", HTTP_GET,  handleApiProducts);
    sServer.on("/api/product",  HTTP_POST, handleApiProduct);
    sServer.on("/onboard",     HTTP_GET,  handleOnboardForm);
    sServer.on("/api/tare",    HTTP_POST, handleApiTare);
    sServer.on("/api/onboard", HTTP_POST, handleApiOnboard);
    sServer.on("/reorder",     HTTP_GET,  handleReorder);
    sServer.on("/stock",            HTTP_GET,  handleStockPage);
    sServer.on("/api/stock/add",    HTTP_POST, handleApiStockAdd);
    sServer.on("/api/stock/update", HTTP_POST, handleApiStockUpdate);
    sServer.on("/api/stock/delete", HTTP_POST, handleApiStockDelete);
    sServer.on("/api/stock",        HTTP_GET,  handleApiStock);
    // These two MUST be registered before "/config": ESPAsyncWebServer's
    // default URI matching is "backward compatible" (WebServer.cpp,
    // AsyncURIMatcher::matches, Type::BackwardCompatible) -- a plain string
    // route matches the exact path OR anything starting with "path/", and
    // _attachHandler() takes the first handler in registration order whose
    // canHandle() returns true. Registered after "/config", these two are
    // silently unreachable -- every request lands on handleConfig() instead
    // (found by testing GET /config/export and getting the Config PAGE's
    // HTML back instead of JSON).
    sServer.on("/config/export", HTTP_GET,  handleConfigExport);
    sServer.on("/config/import", HTTP_POST, handleConfigImportDone, handleConfigImportUpload);
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
    sServer.on("/api/tz",         HTTP_POST, handleApiTzSet);
    sServer.on("/api/station-name", HTTP_POST, handleApiStationName);

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
