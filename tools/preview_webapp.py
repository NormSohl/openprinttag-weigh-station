#!/usr/bin/env python3
# Reproduce the ESP32 web-app pages statically, using the exact STYLE + markup
# from src/web_app.cpp, with sample data. Output one stacked preview HTML.

STYLE = (
"<style>"
"body{font-family:system-ui,sans-serif;margin:0;background:#111;color:#eee}"
"header{background:#1c2b1c;padding:14px 18px;font-size:20px;font-weight:600}"
"header a{color:#8f8;text-decoration:none;margin-right:16px;font-size:15px;font-weight:400}"
"header a.active{color:#fff;font-weight:600;border-bottom:2px solid #8f8;padding-bottom:3px}"
"main{padding:18px;max-width:760px;margin:0 auto}"
"table{border-collapse:collapse;width:100%}"
"th,td{padding:6px 10px;text-align:left;border-bottom:1px solid #333}"
"th{color:#9c9}"
".sw{display:inline-block;width:14px;height:14px;border-radius:3px;vertical-align:middle;margin-right:6px;border:1px solid #000}"
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
".spec{display:flex;flex-wrap:wrap;gap:5px 16px;font-size:14px;color:#aaa;margin-top:10px}"
".spec b{color:#ddd;font-weight:600}"
# preview-only chrome:
".plabel{max-width:760px;margin:34px auto 0;padding:6px 12px;font:600 13px/1.4 ui-monospace,Menlo,monospace;"
"color:#9c9;background:#0c140c;border-left:3px solid #2a5;border-radius:4px}"
"</style>"
)

def nav(active):
    s="<header>Weigh Station<span style='float:right'>"
    for label in ["Inventory","Spools","Onboard","Reorder","Config","Calibrate","Backup"]:
        cls=" class='active'" if label==active else ""
        s+=f"<a href='#'{cls}>{label}</a>"
    return s+"</span></header>"

def page(route, active, body):
    return f"<div class='plabel'>GET {route}</div>{nav(active)}<main>{body}</main>"

def sw(hexrgb):
    # None / "" mirrors alpha==0 in the firmware: no colour assigned, drawn as a
    # crossed-out square rather than as black.
    if not hexrgb: return "<span class='sw sw0' title='No colour assigned'></span>"
    return f"<span class='sw' style='background:#{hexrgb}'></span>"

def kv(label,value): return f"<span><b>{label}</b> {value}</span>"

def spec(*items):
    body="".join(kv(l,v) for l,v in items if v)
    return f"<div class='spec'>{body}</div>" if body else ""

def sparkline(v):
    W,H,PAD=320,64,6
    lo,hi=min(v),max(v); span=(hi-lo) if (hi-lo)>0.001 else 1.0
    X=lambda i:PAD+(W-2*PAD)*i/(len(v)-1)
    Y=lambda val:PAD+(H-2*PAD)*(1.0-(val-lo)/span)
    pts=" ".join(f"{X(i):.1f},{Y(v[i]):.1f}" for i in range(len(v)))
    s=f"<svg viewBox='0 0 320 64' width='320' style='max-width:100%;height:auto' role='img' aria-label='remaining'>"
    s+="<line x1='6' y1='58' x2='314' y2='58' stroke='#333' stroke-width='1'/>"
    s+=f"<polyline fill='none' stroke='#8f8' stroke-width='2' stroke-linejoin='round' stroke-linecap='round' points='{pts}'/>"
    s+=f"<circle cx='{X(len(v)-1):.1f}' cy='{Y(v[-1]):.1f}' r='3' fill='#cffccf'/></svg>"
    return s

# ---- Dashboard (/) ----
dash = ("<div class='card'>"
"<div class='muted'>On the scale now</div>"
"<div class='big'><a href='#' style='color:inherit;text-decoration:none'>"+sw('f99963')+"#42 PLA Summer Grass</a></div>"
"<p class='muted'>Bambu Labs &middot; PLA &middot; 612 g remaining &middot; 388 g used</p>"
+spec(("Tare","201 g"),("Nominal","1000 g"),("&Oslash;","1.75 mm"),
      ("Nozzle","220&ndash;240 &deg;C"),("Bed","55&ndash;65 &deg;C"))+"</div>"
"<h3>Inventory</h3><table>"
"<tr><th>Filament</th><th>Spools</th><th>Remaining</th></tr>"
f"<tr><td>{sw('8fd8a0')}PLA Summer Grass</td><td>2</td><td>1250 g</td></tr>"
f"<tr><td>{sw('1b1b1b')}PLA Galaxy Black</td><td>1</td><td>850 g</td></tr>"
f"<tr><td>{sw('8fd8a0')}PETG Summer Grass</td><td>2</td><td>1380 g</td></tr>"
f"<tr><td>{sw(None)}ASA (colour not set)</td><td>1</td><td>620 g</td></tr></table>"
"<p class='muted'>6 spools tracked &middot; 148 log entries</p>")

# ---- Dashboard, uncalibrated variant (banner) ----
dash_uncal = ("<div class='card' style='border-color:#a70'>"
"<b class='ob'>Scale not calibrated.</b> Weights will be wrong until you "
"<a href='#' style='color:#fd6'>calibrate the scale</a>.</div>"
"<div class='card'><div class='muted'>On the scale now</div>"
"<div class='big'><a href='#' style='color:inherit;text-decoration:none'>"+sw(None)+"#47 Unknown</a></div>"
"<p class='ob'>Needs onboarding &mdash; <a href='#' style='color:#fd6'>add details</a></p></div>"
"<h3>Inventory</h3><table>"
"<tr><th>Filament</th><th>Spools</th><th>Remaining</th></tr>"
"<tr><td colspan='3' class='muted'>No spools yet</td></tr></table>"
"<p class='muted'>1 spools tracked &middot; 2 log entries</p>")

# ---- Spools (/spools) ----
rows=[("42","f99963","PLA Summer Grass","Bambu Labs","612",False),
      ("43","1a1a1a","PETG Galaxy Black","Prusament","740",False),
      ("44","2e9e4f","PLA Summer Grass","Hatchbox","300",False),
      ("47",None,"Unknown","Unknown","0",True)]
spools="<h3>Spools</h3><table><tr><th>#</th><th>Filament</th><th>Vendor</th><th>Remaining</th><th></th></tr>"
for n,rgb,mat,ven,rem,ob in rows:
    flag="<span class='ob'>needs onboarding</span>" if ob else ""
    spools+=f"<tr><td><a href='#' style='color:#8f8'>#{n}</a></td><td>{sw(rgb)}{mat}</td><td>{ven}</td><td>{rem} g</td><td>{flag}</td></tr>"
spools+="</table>"

# ---- Spool detail (/spool?id=42) ----
series=[812,760,690,631,588,540,505,441,388,331,300,255,212]
ts=[f"2026-07-{4+i:02d}T18:0{i%6}:11Z" for i in range(len(series))]
detail=("<div class='card'>"
f"<div class='big'>{sw('f99963')}#42 PLA Summer Grass</div>"
"<p class='muted'>Bambu Labs &middot; PLA &middot; 13 weigh session(s)</p>"
"<p class='big'>212 g <span class='muted' style='font-size:15px'>remaining &middot; 788 g used</span></p>"
+spec(("Tare","201 g"),("Nominal","1000 g"),("&Oslash;","1.75 mm"),
      ("Last weighed","2026-07-16T18:05:11Z"))+"</div>"
"<div class='card'><label style='margin-top:0'>Remaining over time</label>"
f"{sparkline(series)}</div>"
"<h3>Weigh history <a href='#' style='font-size:14px;color:#8f8'>(CSV)</a></h3>"
"<table><tr><th>When (UTC)</th><th>Gross</th><th>Remaining</th><th>Used</th></tr>")
for k in range(len(series)-1,-1,-1):
    detail+=f"<tr><td>{ts[k]}</td><td>{series[k]+250} g</td><td>{series[k]} g</td><td>{1000-series[k]} g</td></tr>"
detail+="</table><p class='muted'><a href='#' style='color:#8f8'>&larr; all spools</a></p>"

# ---- Onboard (/onboard) ----
def opts(items): return "".join(f"<option>{x}</option>" for x in items)
onboard=("<h3>Onboard spool #47</h3>"
"<form>"
"<label>Vendor</label><select name='vendor'>"+opts(["Bambu Labs","Prusament","Hatchbox","Overture"])+"</select>"
"<label>Material</label><select name='material'>"+opts(["PLA","PETG","ASA","TPU"])+"</select>"
"<label>Color</label><select name='color'>"+opts(["Black","Prusa Orange","Galaxy Silver","White"])+"</select>"
"<label>Spool profile</label><select name='profile'>"+opts(["Bambu 1kg (253g tare)","Prusament 1kg PETG","Generic 1kg cardboard"])+"</select>"
"<label>Empty spool tare (g) &mdash; blank to use profile</label>"
"<input type='number' step='0.1' placeholder='from profile'>"
"<button type='button' class='sec'>Capture tare from scale</button>"
"<div><button type='submit'>Save &amp; write tag</button></div></form>")

# ---- Reorder (/reorder) ----
reorder=("<h3>Reorder list</h3>"
"<p class='muted'>Standard-stock items at or below their threshold. Review, then "
"<a href='#' style='color:#8f8'>download CSV</a> to place the order.</p>"
"<table><tr><th>Vendor</th><th>Material</th><th>Color</th><th>On hand</th><th>Threshold</th></tr>"
"<tr><td>Prusament</td><td>PETG</td><td>Prusa Orange</td><td>0 spool(s), 0 g</td><td>2 spools</td></tr>"
"<tr><td>Bambu Labs</td><td>PLA</td><td>Black</td><td>1 spool(s), 212 g</td><td>2 spools</td></tr>"
"</table>")

# ---- Config (/config) ----
def cfgform(label,which,json):
    return ("<div class='card'><label style='margin-top:0'>"+label+"</label>"
    "<form><textarea name='json' rows='6' style='width:100%;font-family:monospace;font-size:13px;"
    "background:#222;color:#8f8;border:1px solid #444;border-radius:5px'>"+json+"</textarea>"
    "<div><button type='submit'>Save "+label+"</button></div></form></div>")
config=("<h3>Config catalog</h3>"
"<p class='muted'>Edit the reference tables that drive onboarding and reordering. "
"Each is a JSON array; Save validates and persists it.</p>"
+cfgform("Vendors","vendors",'["Bambu Labs","Prusament","Hatchbox","Overture"]')
+cfgform("Materials","materials",'[{"name":"PLA","abbr":"PLA","dia":1.75,"print_min":190,"print_max":220,"bed_min":45,"bed_max":60}]')
+cfgform("Spool profiles","spool-profiles",'[{"label":"Bambu 1kg","nominal_full_g":1000,"empty_g":253}]')
+cfgform("Colors","colors",'[{"name":"Prusa Orange","rgba":[249,153,99,255]}]')
+cfgform("Stock items","stock-items",'[{"vendor":"Prusament","material":"PETG","color":"Prusa Orange","min_spools":2}]'))

# ---- Calibrate (/calibrate) ----
calibrate=("<h3>Scale calibration</h3>"
"<div class='card'><div class='muted'>Live weight</div>"
"<div class='big'>212.4 g</div><div class='muted'>Calibrated</div></div>"
"<div class='card'><label style='margin-top:0'>Step 1 &mdash; Zero</label>"
"<p class='muted'>Remove everything from the scale, then zero it.</p>"
"<button type='button'>Zero (tare)</button></div>"
"<div class='card'><label style='margin-top:0'>Step 2 &mdash; Calibrate</label>"
"<p class='muted'>Place a known weight on the scale and enter its exact mass in grams.</p>"
"<input type='number' step='0.1' placeholder='grams'><button type='button'>Set calibration</button></div>")

# ---- Backup (/backup) ----
backup=("<h3>Backup &amp; restore</h3>"
"<div class='card'><label style='margin-top:0'>Download</label>"
"<p class='muted'>The event log is the source of truth &mdash; download it to your machine as an off-device backup.</p>"
"<a href='#'><button type='button'>Download event log</button></a></div>"
"<div class='card'><label style='margin-top:0'>Restore</label>"
"<p class='muted'>Upload a previously downloaded log to replace the current one. "
"The upload is validated first; a bad file leaves your data untouched.</p>"
"<form><input type='file'><div><button type='submit'>Upload &amp; restore</button></div></form></div>"
"<p class='muted'>Config tables back up separately via the <a href='#' style='color:#8f8'>Config</a> page "
"(copy the JSON). SD-card snapshots arrive once the card is wired.</p>")

pages=[("/","Inventory",dash),
       ("/  (uncalibrated / needs-onboarding variant)","Inventory",dash_uncal),
       ("/spools","Spools",spools),
       ("/spool?id=42","Spools",detail),
       ("/onboard","Onboard",onboard),
       ("/reorder","Reorder",reorder),
       ("/config","Config",config),
       ("/calibrate","Calibrate",calibrate),
       ("/backup","Backup",backup)]

html="<!doctype html><html><head><meta charset='utf-8'>"
html+="<meta name='viewport' content='width=device-width,initial-scale=1'>"
html+="<title>Weigh Station — web-app preview</title>"+STYLE+"</head><body>"
html+="<main><h2 style='color:#8f8'>Built-in web app — static preview</h2>"
html+="<p class='muted'>Rendered from the exact CSS/markup in <code>src/web_app.cpp</code> with sample data. "
html+="Buttons/links are inert here.</p></main>"
for route,active,body in pages:
    html+=page(route,active,body)
html+="</body></html>"

open("webapp-preview.html","w").write(html)
print("wrote webapp-preview.html", len(html), "bytes")
