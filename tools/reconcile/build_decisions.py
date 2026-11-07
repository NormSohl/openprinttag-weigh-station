# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""work/recon.json -> docs/reference/product-reconciliation-DATE.xlsx, the decision
spreadsheet (Products, Merges, Stock List, How to read).

  build_decisions.py config.json [previous.xlsx]

DATE defaults to today. Decisions carry over from previous.xlsx, else the output file
if it exists, else the newest earlier product-reconciliation-*.xlsx: each product keeps its Decision and Your note as long as its
proposal (new vendor, new name, what changes, duplicate) is unchanged; anything new
or changed gets the default ("Apply" wherever something changes) and is listed.
"""
import json,collections,os,sys,datetime
from openpyxl import Workbook, load_workbook
from openpyxl.styles import Font,PatternFill,Alignment
from openpyxl.utils import get_column_letter
from openpyxl.worksheet.datavalidation import DataValidation
from openpyxl.formatting.rule import FormulaRule
HERE=os.path.dirname(os.path.abspath(__file__))
RWORK=os.environ.get("RWORK",os.path.join(HERE,"work"))
DATE=os.environ.get("DATE") or datetime.date.today().isoformat()
OUT=os.path.join(HERE,"..","..","docs","reference",f"product-reconciliation-{DATE}.xlsx")
R=json.load(open(os.path.join(RWORK,"recon.json"))); rows=R["rows"]; byid={r["pid"]:r for r in rows}
cfg=json.load(open(sys.argv[1])); STOCK=cfg["stock-items"]
OVR=json.load(open(os.path.join(HERE,"overrides.json")))
# ---- decisions carried over from an earlier copy of this spreadsheet
import glob
_earlier=sorted(f for f in glob.glob(os.path.join(os.path.dirname(OUT),"product-reconciliation-*.xlsx")) if f!=OUT and not os.path.basename(f).startswith("~$"))
prev_path=sys.argv[2] if len(sys.argv)>2 else (OUT if os.path.exists(OUT) else (_earlier[-1] if _earlier else OUT))
print("decisions from:",os.path.basename(prev_path) if os.path.exists(prev_path) else "none (first run)")
PREV={}; PREVM={}
if os.path.exists(prev_path):
    pw=load_workbook(prev_path)
    for r in pw["Products"].iter_rows(min_row=2,values_only=True):
        PREV[r[0]]=dict(proposal=(r[11],r[12],r[13],r[15]),decision=r[17],note=r[18])
    for r in pw["Merges"].iter_rows(min_row=2,values_only=True):
        PREVM[(r[0],r[2])]=dict(proposal=r[7],decision=r[8])
carried=[]; fresh=[]
F=Font(name="Arial",size=10); FB=Font(name="Arial",size=10,bold=True,color="FFFFFF"); HF=PatternFill("solid",fgColor="2F5D3A")
YEL=PatternFill("solid",fgColor="FFF2CC")
def sheet(ws,head,data,widths):
    ws.append(head)
    for c in ws[1]: c.font=FB; c.fill=HF; c.alignment=Alignment(vertical="center",wrap_text=True)
    for r in data: ws.append(r)
    for rr in ws.iter_rows(min_row=2):
        for c in rr: c.font=F; c.alignment=Alignment(vertical="top",wrap_text=False)
    for i,w in enumerate(widths,1): ws.column_dimensions[get_column_letter(i)].width=w
    ws.freeze_panes="B2"; ws.auto_filter.ref=ws.dimensions
names=collections.Counter((r["newvendor"],r["newname"]) for r in rows)
wb=Workbook(); ws=wb.active; ws.title="Products"
data=[]
for r in rows:
    has=r["changes"] not in("","none")
    note=[r["note"]] if r["note"] else []
    if names[(r["newvendor"],r["newname"])]>1 and not r.get("dup"): note.append("another product gets the same name")
    if not r["pn"] and r["vendor"].upper()=="ESUN": note.append("no eSUN US item; name kept")
    dec,yn="Apply" if (has or r.get("dup")) else "",""
    prop=(r["newvendor"],r["newname"],r["changes"] if has else "none",r.get("dup","") or None)
    pv=PREV.get(r["pid"])
    if pv and tuple(x or None if i==3 else x for i,x in enumerate(pv["proposal"]))==prop:
        dec,yn=pv["decision"] or "",pv["note"] or ""; carried.append(r["pid"])
    elif dec: fresh.append(r["pid"])
    data.append([r["pid"],r["vendor"],r["name"],r["was"],r["gtin"],r["spools"],r["grams"],r["how"],r["pn"],r["fid"],r["orderable"],
                 r["newvendor"],r["newname"],r["changes"] if has else "none",r["addbc"],r.get("dup",""),"; ".join(note),dec,yn])
head=["Product #","Vendor now","Name now","Name when created","Barcode now","Spools on hand","Grams on hand","Matched by","eSUN part no","Filament ID","Orderable on eSUN US",
      "New vendor","New name","What changes","Barcode to add","Same eSUN item as","Notes","Decision","Your note"]
sheet(ws,head,data,[9,11,30,26,15,8,8,40,22,18,10,10,34,22,15,26,46,10,30])
n=len(data)+1
for c in ws["E"][1:]: c.number_format="@"
for c in ws["O"][1:]: c.number_format="@"
dv=DataValidation(type="list",formula1='"Apply,Skip,Change"',allow_blank=True); ws.add_data_validation(dv); dv.add(f"R2:R{n}")
for col in "RS":
    for c in ws[col][1:]: c.fill=YEL
ws.conditional_formatting.add(f"M2:M{n}",FormulaRule(formula=['AND($N2<>"none",$N2<>"")'],fill=PatternFill("solid",fgColor="E2EFDA")))
ws.conditional_formatting.add(f"P2:P{n}",FormulaRule(formula=['$P2<>""'],fill=PatternFill("solid",fgColor="FCE4D6")))
# merges
ws2=wb.create_sheet("Merges")
md=[]
for pn,ids in R["dups"].items():
    rs=[byid[i] for i in ids]; keep=byid[R["keep"][pn]]
    for r in rs:
        prop="keep" if r is keep else f"merge into #{keep['pid']}"
        pv=PREVM.get((pn,r["pid"]))
        dec=pv["decision"] or "" if pv and pv["proposal"]==prop else ("Apply" if r is not keep else "")
        md.append([pn,rs[0]["newname"],r["pid"],r["name"],r["gtin"],r["spools"],r["grams"],prop,dec])
sheet(ws2,["eSUN part no","New name","Product #","Name now","Barcode now","Spools on hand","Grams on hand","Proposal","Decision"],md,[22,30,10,28,15,8,8,18,10])
dv2=DataValidation(type="list",formula1='"Apply,Skip"',allow_blank=True); ws2.add_data_validation(dv2); dv2.add(f"I2:I{len(md)+1}")
for c in ws2["I"][1:]: c.fill=YEL
# stock list
ws3=wb.create_sheet("Stock List")
sd=[]
for s in STOCK:
    p=byid.get(s.get("product"))
    now=(s["vendor"]+" "+s["material"]+(" "+s["color"] if s["color"] else "")).strip()
    after=(p["newvendor"]+" "+p["newname"]) if p else now
    tgt=p["pid"] if p else ""
    if p and p.get("dup","").find("merge into")>=0: tgt=f'{p["pid"]} → {p["dup"].split("merge into #")[1].rstrip("?")}'
    sd.append([s["id"],now,tgt,after,"yes" if after!=now else "no",s["min_spools"]])
sheet(ws3,["Stock List item #","Label now","Linked product #","Label after","Changes","Target (spools)"],sd,[10,40,14,40,9,10])
ab=wb.create_sheet("How to read")
for r in [["Product reconciliation: station products vs the eSUN US catalogue"],[],
 ["Data",f"Station backup {os.environ.get('BACKUP') or R['source']['events']} ({R['source']['products']} products, {R['source']['stock']} Stock List items) after removing test vendors ({', '.join(OVR.get('remove_vendors',[])) or 'none'}), matched against the eSUN US catalogue in tools/esun-catalogue. Built {DATE}."],
 ["Rule 1: the barcode decides","A product with a barcode is whichever eSUN item that barcode belongs to, in this order: the barcode on the eSUN US list; the barcode's eSUN part number from Lieske (older or EU packaging of the same filament); the barcode in the OpenPrintTag database. Where a product was renamed to something its barcode contradicts (PLA+ on a PLA Basic barcode), the barcode wins."],
 ["Lab corrections","Overrides of rule 1 decided by the lab (tools/reconcile/overrides.json), shown as 'lab correction' in Matched by: "+("; ".join(f"#{k}: {v['note']}" for k,v in OVR["products"].items()) or "none")+"."],
 ["Rule 2: no barcode","Matched by the product's OpenPrintTag package ID if it has one, else by its current name. Three-colour 'Silk Magic' names match eSUN's current 'Silk Mystic'."],
 ["New name","Material + colour from the eSUN US list, plus packaging when it isn't a standard 1 kg spool ('Refill', '3 kg'). Vendor 'eSUN'. Products with no eSUN US item keep their name (vendor spelling still normalised to eSUN). Other vendors are untouched."],
 ["Barcode to add","Only for products that have no barcode, and only where the eSUN US item has exactly one. A product's existing barcode is never changed."],
 ["Same eSUN item","Two products that land on the same eSUN part number are duplicates. The Merges tab proposes keeping the one with more spools, then the one whose barcode is on the eSUN US list, then the one with more filament. A merge moves the spools; each keeps its own tare and cost."],
 ["Decision","Carried over from the previous copy of this spreadsheet where the proposal is unchanged; otherwise pre-filled 'Apply' wherever something changes. Change it to 'Skip' to leave a product alone, or 'Change' and write what you want in 'Your note'. Blank means nothing to do."],
 ["What applying does","Run in the simulator on names-are-labels through the station's own product edit (which updates every spool of the product and, on next placement, its tag) and Repair's merge. Stock List labels follow their product. Usage history relabels automatically under names-are-labels."]]:
    ab.append(r)
ab["A1"].font=Font(name="Arial",size=12,bold=True)
for rr in ab.iter_rows(min_row=3):
    rr[0].font=Font(name="Arial",size=10,bold=True); rr[0].alignment=Alignment(vertical="top")
    if len(rr)>1: rr[1].font=F; rr[1].alignment=Alignment(wrap_text=True,vertical="top")
ab.column_dimensions["A"].width=26; ab.column_dimensions["B"].width=110
wb.move_sheet("How to read",-3); wb.active=1
wb.save(OUT)
c=collections.Counter(d[13] if d[13]=="none" else ("changes") for d in data)
print(OUT, len(data),"products;",c,"; merges:",len(R["dups"]),"; stock labels changing:",sum(1 for d in sd if d[4]=="yes"),"of",len(sd))
print("decisions carried over:",len(carried),"| new or changed proposals defaulted to Apply:",fresh)
