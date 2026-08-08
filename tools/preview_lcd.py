#!/usr/bin/env python3
# Mock the 3.5" ILI9488 480x320 TFT for each device state, from display_task.cpp.
# Layout: title size-3 at (12,12); body rows size-2 at (12, 12+r*28); black bg.
# Text is confined to the left TEXT_W=316 px; the QR panel occupies (322,70,148,148).

# TFT colors -> hex
W="#ffffff"; GREY="#7b7d7b"; GREEN="#00ff00"; CYAN="#00ffff"
RED="#ff0000"; YEL="#ffff00"; BLUE="#0064dc"; AMBER="#dc8c00"

# Geometry mirrored from display_task.cpp.
QR_X, QR_Y, QR_BOX, QUIET = 332, 70, 140, 4

def T(text,color):        return (12,12,26,color,text,True)   # title (size 3, bold)
def R(r,text,color):      return (12,12+r*28,17,color,text,False)
def BIG(x,y,text,color):  return (x,y,64,color,text,True)     # countdown digit (size 8)
def QR(which):            return ("qr",which)

# Real, scannable QR module grids, generated with the firmware's own encoder
# (ricmoo/QRCode) via tools/qr — so the preview shows the actual code the panel
# draws, not a placeholder. Regenerate if the URLs change.
#
# Note both are version 2 (25 modules): the firmware always encodes the NUMERIC
# address, never the .local name, because mDNS is exactly what a phone camera
# may fail to resolve.
QR_GRIDS = {
    "idle": (25, [
        "#######..###.#..#.#######", "#.....#....##.#...#.....#",
        "#.###.#..###....#.#.###.#", "#.###.#.#.##.####.#.###.#",
        "#.###.#.#.#.#.#...#.###.#", "#.....#..##..#..#.#.....#",
        "#######.#.#.#.#.#.#######", ".........#..#............",
        "##...###.##.##.##...##...", "###.##.#..#..#####..####.",
        "...#.###.#.#####..##.#.##", "...#.#.#..##..###...##..#",
        "##...##.###..##.###.....#", "##.###..#.#.##.#.......#.",
        "#.#..###.##..###.#.#.#.##", "#.##...##...###.#...#.#.#",
        "#..######.#.#.#.#####.#..", "........##.....##...#.#..",
        "#######.#.#.###.#.#.##..#", "#.....#.#.##..#.#...#...#",
        "#.###.#..###...##########", "#.###.#..##.##....##.#.##",
        "#.###.#..##..##..#....#.#", "#.....#.###.##.##.###...#",
        "#######.######.##.#..#..#",
    ]),
    "onboard": (25, [
        "#######.#..###..#.#######", "#.....#..##..#....#.....#",
        "#.###.#...#.##....#.###.#", "#.###.#.....#####.#.###.#",
        "#.###.#..#.#......#.###.#", "#.....#...#.##....#.....#",
        "#######.#.#.#.#.#.#######", "........#.##.##..........",
        "##.##.#..####..#..#.....#", "....##.###..######..####.",
        ".####.#...####.#.#####..#", "##...#...###..##.#..#####",
        "..#..###.#.##...###.....#", "#.#.#..###.#..##...##..#.",
        "###.####.##.#.####...####", "#.##...#..###...#...#.#.#",
        "#..####.##.#.#..#####.##.", "........###..#.##...#..#.",
        "#######..#.#....#.#.##..#", "#.....#..#..###.#...#..#.",
        "#.###.#.#...#.########...", "#.###.#.#..#..#...##.#.##",
        "#.###.#..#.#.##.....#.###", "#.....#.###....#.####.###",
        "#######.#.#.#..##.#..#..#",
    ]),
}

def qr_rects(which):
    """Yield (x,y,w,h) black rects, using drawQr()'s exact placement arithmetic."""
    size, rows = QR_GRIDS[which]
    total = size + QUIET*2
    scale = QR_BOX // total
    side  = total * scale
    ox    = QR_X + (QR_BOX - side)//2
    oy    = QR_Y + (QR_BOX - side)//2
    yield ("bg", ox, oy, side, side)
    for my,line in enumerate(rows):
        mx = 0
        while mx < size:
            if line[mx] != "#":
                mx += 1; continue
            run = 0
            while mx+run < size and line[mx+run] == "#":   # coalesced, as in firmware
                run += 1
            yield ("fg", ox+(QUIET+mx)*scale, oy+(QUIET+my)*scale, run*scale, scale)
            mx += run

def qr_svg(which):
    out=[]
    for kind,x,y,w,h in qr_rects(which):
        fill = "#fff" if kind=="bg" else "#000"
        out.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{fill}"/>')
    return "".join(out)

def span(s):
    if s[0]=="qr":
        return (f"<svg style='position:absolute;left:0;top:0' width='480' height='320' "
                f"viewBox='0 0 480 320'>{qr_svg(s[1])}</svg>")
    left,top,size,color,text,bold=s
    fw="700" if bold else "400"
    return (f"<span style='position:absolute;left:{left}px;top:{top}px;"
            f"font-size:{size}px;font-weight:{fw};color:{color};"
            f"white-space:nowrap'>{text}</span>")

# state -> (label, neopixel hue+name, [spans])
PX_OFF=("#000","off")
states=[
 ("Boot", PX_OFF,
  [T("Weigh Station",W), R(2,"Starting...",GREY)]),
 ("WiFiSetupMode", ("#3355ff","dim blue"),
  [T("WiFi Setup",BLUE), R(2,"Join network:",W), R(3,"WeighStation-Setup",CYAN),
   R(5,"Then open browser to",GREY), R(6,"192.168.4.1",GREY)]),
 ("Idle  (calibrated)", ("#2ecc40","dim green"),
  [T("Seattle Makers",GREEN), R(2,"Place spool to begin",W),
   R(5,"Web app:",GREY), R(6,"weighstation.local",CYAN),
   R(7,"or 192.168.1.42",GREY), QR("idle")]),
 ("Idle  (not yet calibrated)", ("#2ecc40","dim green"),
  [T("Seattle Makers",GREEN), R(2,"Place spool to begin",W),
   R(3,"Scale not calibrated",AMBER), R(4,"Calibrate in web app",AMBER),
   R(5,"Web app:",GREY), R(6,"weighstation.local",CYAN),
   R(7,"or 192.168.1.42",GREY), QR("idle")]),
 ("IdleNoWiFi  (SoftAP fallback)", ("#ffaa22","amber"),
  [T("Weigh Station",AMBER), R(2,"Place spool to weigh",W),
   R(4,"Join WiFi:",GREY), R(5,"WeighStation",CYAN), R(6,"http://192.168.4.1",CYAN),
   QR("idle")]),
 ("TagReadError", ("#ff3333","red"),
  [T("Read Error",RED), R(2,"Reposition spool",W), R(3,"or remove tag",W)]),
 ("AwaitingFormatConfirm  (blank-tag countdown)", ("#ffaa22","amber, blinking")
  ,[T("New tag found",AMBER), R(2,"Remove to cancel",W), R(3,"Registering in:",W),
    BIG(216,160,"3",AMBER)]),
 ("FormattingAndRegistering", ("#3355ff","blue"),
  [T("Registering...",BLUE), R(2,"Please wait",W)]),
 ("ForeignTagFound / Registering", ("#33cccc","cyan"),
  [T("New spool found",CYAN), R(2,"Prusament",W), R(3,"PETG Prusa Orange",W),
   R(4,"Registering spool...",GREY)]),
 ("WeighingAndSync", ("#3355ff","blue"),
  [T("Weighing...",BLUE), R(2,"612 g",W)]),
 ("Present  (needs onboarding)", ("#ffe033","yellow"),
  [T("Registered!",YEL), R(2,"Spool #47",W), R(3,"612 g",W),
   R(4,"NEEDS ONBOARDING",AMBER), R(5,"Scan QR to add details,",GREY),
   R(6,"or visit the Onboard page:",GREY),
   R(7,"weighstation.local/onboard",CYAN), R(8,"or 192.168.1.42/onboard",GREY),
   QR("onboard")]),
 ("Present  (normal)", ("#2ecc40","green"),
  [R(0,"Spool #42",W), R(2,"PLA",W), R(3,"612 g remaining",GREEN),
   R(5,"Saved locally",GREY)]),
 ("ReconcilingMainSection", ("#ffe033","yellow"),
  [T("Updating tag...",YEL)]),
 ("Idle  (storage full)", ("#2ecc40","dim green"),
  [T("Seattle Makers",GREEN), R(2,"Place spool to begin",W),
   R(3,"STORAGE FULL - not",RED), R(4,"recording weighs!",RED),
   R(5,"Web app:",GREY), R(6,"weighstation.local",CYAN),
   R(7,"or 192.168.1.42",GREY), QR("idle")]),
 ("ValidTagFound", ("#33aacc","cyan"),
  [T("Tag read",CYAN), R(2,"PETG",W), R(3,"Prusament",GREY)]),
 ("(any unhandled state)", ("#333","dim white"),
  [T("...",GREY), R(2,"weighing_and_sync",W), R(3,"(no screen for this state)",GREY)]),
]

def screen(label, px, spans):
    dot,pxname=px
    body="".join(span(s) for s in spans)
    return (
      "<div class='cell'>"
      f"<div class='lbl'>{label}</div>"
      f"<div class='tft'>{body}</div>"
      f"<div class='px'><span class='dot' style='background:{dot}'></span>"
      f"NeoPixel: {pxname}</div>"
      "</div>")

css=("<style>"
"body{margin:0;background:#141414;color:#ddd;font-family:system-ui,sans-serif}"
"main{max-width:1060px;margin:0 auto;padding:22px}"
"h2{color:#8f8}"
".muted{color:#888}"
".grid{display:flex;flex-wrap:wrap;gap:26px;justify-content:center}"
".cell{width:480px}"
".lbl{font:600 14px/1.4 ui-monospace,Menlo,monospace;color:#9c9;margin:0 0 6px}"
".tft{position:relative;width:480px;height:320px;background:#000;border-radius:10px;"
"box-shadow:0 0 0 10px #1b1b1b,0 0 0 12px #000;font-family:ui-monospace,Menlo,Consolas,monospace;"
"overflow:hidden;letter-spacing:.5px}"
".px{margin:16px 0 0;font-size:13px;color:#aaa}"
".dot{display:inline-block;width:13px;height:13px;border-radius:50%;margin-right:7px;"
"vertical-align:middle;border:1px solid #000;box-shadow:0 0 6px currentColor}"
"</style>")

html=("<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>Weigh Station — TFT display preview</title>"+css+"</head><body><main>"
"<h2>TFT display — per-state preview</h2>"
"<p class='muted'>3.5&quot; ILI9488, 480&times;320, landscape. Reproduced from "
"<code>display_task.cpp</code> (positions, colors, text). Font here is a stand-in "
"for the TFT's blocky GLCD font; layout &amp; colors are accurate. The QR codes are "
"real — generated with the firmware's own encoder — and scannable. The dot shows "
"the onboard NeoPixel's status hue (dim on the device).</p>"
"<div class='grid'>"
+ "".join(screen(*st) for st in states) +
"</div></main></body></html>")

open("lcd-preview.html","w").write(html)
print("wrote lcd-preview.html", len(html), "bytes;", len(states), "states")

# --- also emit each state as a standalone SVG (for embedding in docs) ---
import os
# Positionally paired with `states` — keep the two lists in step when adding one.
slugs=["boot","wifi-setup","idle","idle-uncalibrated","idle-softap","read-error",
       "new-tag-countdown","registering","foreign-spool","weighing","registered",
       "present","updating-tag","idle-storage-full","tag-read","unknown-state"]
assert len(slugs)==len(states), f"{len(slugs)} slugs vs {len(states)} states"

def svg_screen(spans):
    out=['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 480 320" width="480" '
         'font-family="ui-monospace,Menlo,Consolas,monospace">',
         '<rect width="480" height="320" rx="6" fill="#000"/>']
    for s in spans:
        if s[0]=="qr":
            out.append(qr_svg(s[1]))
            continue
        left,top,size,color,text,bold=s
        fw=' font-weight="700"' if bold else ''
        y=top+round(size*0.78)
        t=text.replace('&','&amp;').replace('<','&lt;')
        out.append(f'<text x="{left}" y="{y}" font-size="{size}"{fw} fill="{color}">{t}</text>')
    out.append('</svg>')
    return "\n".join(out)

os.makedirs("lcd-svg",exist_ok=True)
for slug,(label,px,spans) in zip(slugs,states):
    open(f"lcd-svg/{slug}.svg","w").write(svg_screen(spans))
print("wrote", len(slugs), "SVGs to lcd-svg/")
