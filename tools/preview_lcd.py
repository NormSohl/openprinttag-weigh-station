#!/usr/bin/env python3
# Mock the 3.5" ILI9488 480x320 TFT for each device state, from display_task.cpp.
# Layout: title size-3 at (12,12); body rows size-2 at (12, 12+r*28); black bg.

# TFT colors -> hex
W="#ffffff"; GREY="#7b7d7b"; GREEN="#00ff00"; CYAN="#00ffff"
RED="#ff0000"; YEL="#ffff00"; BLUE="#0064dc"; AMBER="#dc8c00"

def T(text,color):        return (12,12,26,color,text,True)   # title (size 3, bold)
def R(r,text,color):      return (12,12+r*28,17,color,text,False)
def BIG(x,y,text,color):  return (x,y,64,color,text,True)     # countdown digit (size 8)

def span(s):
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
   R(5,"Web app:",GREY), R(6,"http://weighstation.local",CYAN)]),
 ("Idle  (not yet calibrated)", ("#2ecc40","dim green"),
  [T("Seattle Makers",GREEN), R(2,"Place spool to begin",W),
   R(3,"Scale not calibrated",AMBER), R(4,"Calibrate in web app",AMBER),
   R(5,"Web app:",GREY), R(6,"http://weighstation.local",CYAN)]),
 ("IdleNoWiFi  (SoftAP fallback)", ("#ffaa22","amber"),
  [T("Weigh Station",AMBER), R(2,"Place spool to weigh",W),
   R(4,"Join WiFi:",GREY), R(5,"WeighStation",CYAN), R(6,"http://192.168.4.1",CYAN)]),
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
  [T("Registered!",YEL), R(2,"Spool #47",W), R(3,"Add details in web app:",GREY),
   R(4,"http://weighstation.local",CYAN), R(5,"612 g",W)]),
 ("Present  (normal)", ("#2ecc40","green"),
  [R(0,"Spool #42",W), R(2,"PLA",W), R(3,"612 g remaining",GREEN),
   R(5,"Saved locally",GREY)]),
 ("ReconcilingMainSection", ("#ffe033","yellow"),
  [T("Updating tag...",YEL)]),
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
"for the TFT's blocky GLCD font; layout &amp; colors are accurate. The dot shows "
"the onboard NeoPixel's status hue (dim on the device).</p>"
"<div class='grid'>"
+ "".join(screen(*st) for st in states) +
"</div></main></body></html>")

open("lcd-preview.html","w").write(html)
print("wrote lcd-preview.html", len(html), "bytes;", len(states), "states")

# --- also emit each state as a standalone SVG (for embedding in docs) ---
import os
slugs=["boot","wifi-setup","idle","idle-uncalibrated","idle-softap","read-error",
       "new-tag-countdown","registering","foreign-spool","weighing","registered",
       "present","updating-tag"]
def svg_screen(spans):
    out=['<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 480 320" width="480" '
         'font-family="ui-monospace,Menlo,Consolas,monospace">',
         '<rect width="480" height="320" rx="6" fill="#000"/>']
    for left,top,size,color,text,bold in spans:
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
