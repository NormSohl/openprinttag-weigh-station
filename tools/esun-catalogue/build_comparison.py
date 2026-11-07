# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""work/lieske_match.json + data/lieske-DATE.json -> docs/reference/esun-products-DATE.xlsx:
Lieske (EU distributor) against eSUN US, with both raw lists."""
import json,collections,os
from openpyxl import Workbook
from openpyxl.styles import Font,PatternFill,Alignment
from openpyxl.utils import get_column_letter
from openpyxl.formatting.rule import FormulaRule
from esunlib import DATE, LIESKE_JSON, REF, load
OUT=os.path.join(REF,f"esun-products-{DATE}.xlsx")
C=load("lieske_match.json"); US=C["US"]; LI={o["lieske"]:o for o in C["LI"]}; USk={}
for u in US: USk.setdefault(u["sku"],u)
PACK={"P1":"paper spool","R1":"refill","RS1":"refill set","B1":"plastic spool","S1":"spool (S1)","Y1":"spool (Y1)","TZ1":"spool (TZ1)","P4":"4-pack","":""}
dup=collections.Counter(o["mpn"] for o in C["LI"])
F=Font(name="Arial",size=10); FB=Font(name="Arial",size=10,bold=True,color="FFFFFF"); HF=PatternFill("solid",fgColor="2F5D3A")
def sheet(ws,head,rows,widths):
    ws.append(head)
    for c in ws[1]: c.font=FB; c.fill=HF; c.alignment=Alignment(vertical="center",wrap_text=True)
    for r in rows: ws.append(r)
    for rr in ws.iter_rows(min_row=2):
        for c in rr: c.font=F
    for i,w in enumerate(widths,1): ws.column_dimensions[get_column_letter(i)].width=w
    ws.freeze_panes="A2"; ws.auto_filter.ref=ws.dimensions
def kg(s):
    try: return float(s)
    except: return None
cmp=[]
for mpn,lk,skus,basis in C["rows"]:
    o=LI[lk]; us=[USk[s] for s in skus]
    sal="yes" if any(u["salable"]=="yes" for u in us) else ("no" if us and all(u["salable"]=="no" for u in us) else ("not shown" if us else ""))
    if not us: status="Lieske only"
    elif sal=="yes": status="Both"
    else: status="Both, out of stock at eSUN US"
    note=[]
    if lk in C["conflicts"]: note.append(C["conflicts"][lk])
    if dup[mpn]>1: note.append("Lieske lists this part number twice")
    if o["eol"]: note.append("Lieske: end-of-life "+o["eol"])
    if basis.startswith("colour name"): note.append("matched by colour name; part codes differ")
    if "(PETG+)" in basis: note.append("Lieske's PETG175 code; eSUN US sells it as PETG+")
    prices=[u["price"] for u in us if u["price"] is not None]
    cmp.append([status, us[0]["title"] if us else o["fam"], (us[0]["colour"] if us else o["ncol"]).title(), kg(o["size"]),
        o["mpn"], o["gtin"], "in stock" if o["stock_note"].startswith("in stock") else "made to order",
        ", ".join(skus), ", ".join(sorted({PACK.get(u["pack"],u["pack"]) for u in us})), sal, min(prices) if prices else None, basis, "; ".join(note)])
for i in C["usOnly"]:
    u=US[i]
    cmp.append(["eSUN US only",u["title"],u["colour"].title(),kg(u["size"]),"","","",u["sku"],PACK.get(u["pack"],u["pack"]),u["salable"],u["price"],"",""])
order={"Both":0,"Both, out of stock at eSUN US":1,"Lieske only":2,"eSUN US only":3}
cmp.sort(key=lambda r:(order[r[0]],r[1],r[2]))
wb=Workbook(); ws=wb.active; ws.title="Comparison"
sheet(ws,["Status","Product line","Colour","Size (kg)","Lieske part no","EAN (Lieske)","Lieske stock","eSUN US part no(s)","eSUN US packaging","Salable on eSUN US","eSUN US price (USD)","Matched by","Notes"],cmp,[26,22,26,8,24,16,14,30,18,12,11,26,60])
for c in ws["F"][1:]: c.number_format="@"
for c in ws["K"][1:]: c.number_format="$#,##0.00"
n=len(cmp)+1
fills={"Both":"E2EFDA","Both, out of stock at eSUN US":"FFF2CC","Lieske only":"FCE4D6","eSUN US only":"DDEBF7"}
for k,col in fills.items(): ws.conditional_formatting.add(f"A2:A{n}",FormulaRule(formula=[f'$A2="{k}"'],fill=PatternFill("solid",fgColor=col)))
# summary sheet with formulas
sm=wb.create_sheet("Summary",0)
sm.append(["eSUN availability: Lieske (EU distributor) vs eSUN's US site, read "+DATE]); sm["A1"].font=Font(name="Arial",size=12,bold=True)
sm.append([]); sm.append(["Status","Rows","What it means"])
for c in sm[3]: c.font=FB; c.fill=HF
expl={"Both":"Lieske lists it and eSUN US shows it as salable",
"Both, out of stock at eSUN US":"Lieske lists it; eSUN US shows the variant but not salable",
"Lieske only":"Lieske lists it; no matching variant on eSUN's US site (EU-only, discontinued, or Lieske naming)",
"eSUN US only":"On eSUN's US site; Lieske does not carry it (mostly refills, resins, newer lines)"}
for i,k in enumerate(order,4):
    sm.append([k,f'=COUNTIF(Comparison!$A$2:$A${n},A{i})',expl[k]])
sm.append(["Total",f"=SUM(B4:B7)",""])
sm.append([]); sm.append(["eSUN US variants salable",f'=COUNTIF(\'eSUN US site\'!$E$2:$E${len(US)+1},"yes")',"of "+str(len(US))+" variants listed on esun3d.com"])
sm.append(["Lieske rows with a code/description conflict",f'=COUNTIF(Comparison!$M$2:$M${n},"*eSUN uses code*")',"Lieske's colour name contradicts eSUN's own colour code"])
for rr in sm.iter_rows(min_row=4):
    for c in rr: c.font=F
for i in (8,):
    for c in sm[i]: c.font=Font(name="Arial",size=10,bold=True)
sm.column_dimensions["A"].width=40; sm.column_dimensions["B"].width=10; sm.column_dimensions["C"].width=90
# raw US sheet
ws2=wb.create_sheet("eSUN US site")
sheet(ws2,["Product","Category","eSUN part no","Option(s)","Salable on US store","Price (USD)","Regular price (USD)","Page"],[[u["title"],u["cat"],u["sku"],u["colour"],u["salable"],u["price"],u["was"],u["page"]] for u in US],[26,60,30,30,12,10,12,48])
for col in "FG":
    for c in ws2[col][1:]: c.number_format="$#,##0.00"
# raw Lieske sheet (copy from earlier raw workbook data)
R=json.load(open(LIESKE_JSON))   # net/gross prices are stored by crawl_lieske.py
_pg=collections.Counter(c.split(" > ")[0] for c in {u["page"]:u["cat"] for u in US}.values())
PAGES=", ".join(f"{n} {k.lower()}" for k,n in _pg.most_common())
R.sort(key=lambda o:(o["category"],o["mpn"].upper()))
ws3=wb.create_sheet("Lieske (EU)")
sheet(ws3,["eSUN part no","EAN (barcode)","Name as listed","Category","Shipping weight","Stock","Availability as shown","Price 1 pc, EUR net","Price 1 pc, EUR incl. VAT","Volume prices (EUR net)","End-of-life date","Lieske part no","Source page","Note"],
 [[o["mpn"],o["gtin"],o["name"],o["category"],o["weight"],o["stock_note"],o["availability_text"],o["net"],o["gross"],o["tiers"],o["eol"],o["lieske"],o["url"],"part no listed twice" if dup[o["mpn"]]>1 else ""] for o in R],[24,16,52,15,10,44,40,11,11,40,14,11,60,18])
for c in ws3["B"][1:]: c.number_format="@"
ws4=wb.create_sheet("Sources")
for r in [["Sources and method, "+DATE],[],
 ["eSUN US site tab","Every product page on esun3d.com ("+PAGES+"; "+str(len(US))+" variants), read in a browser "+DATE+". eSUN's pages embed each variant's eSUN part number and whether the US store ('us_site') can sell it; that is what 'Salable' reports. Prices are the US store's current and regular prices. eSUN publishes no barcodes."],
 ["Lieske (EU) tab","Every eSUN item in Lieske Elektronik's catalogue (industry-electronics.com), "+str(len(R))+" pages read "+DATE+": eSUN part number, EAN, name, stock, prices and end-of-life as published."],
 ["How rows were matched","The two use different part-number schemes for the same product (Lieske PLAP175B1 = eSUN US PLA+175B1P1). Each part number was split into product family, colour code, size and packaging. Lieske's older family prefixes were mapped to eSUN's current ones (PLAP/PLA→PLA+, ePLAHS→PLA+HS, ePLA-Matte→PLA-MT, EPLA-SILK→PLA-SK, …). A row matched on family + colour code + size; failing that, on family + colour name + size. 'Matched by' says which."],
 ["Guard against bad names","A colour-name match was refused where eSUN's own data uses Lieske's colour code for a different colour in that product line (3 rows, all PLA+HS, flagged in Notes; e.g. code PP is Magenta at eSUN, Lieske's name says Red)."],
 ["PETG","Lieske's PETG175…P1 items are described as PETG+ on paper spools; they match eSUN US PETG+ (PETG+175…P1), not eSUN US PETG (PETG175…Y1/S1). Noted per row."],
 ["Limits","Availability on eSUN US is for the US store only. Lieske is one European distributor. 'Lieske only' can mean EU-only, discontinued in the US, or a naming difference the rules didn't catch: check the Notes and the raw tabs."]]:
    ws4.append(r)
ws4["A1"].font=Font(name="Arial",size=12,bold=True)
for rr in ws4.iter_rows(min_row=3):
    rr[0].font=Font(name="Arial",size=10,bold=True); rr[0].alignment=Alignment(vertical="top")
    if len(rr)>1: rr[1].font=F; rr[1].alignment=Alignment(wrap_text=True,vertical="top")
ws4.column_dimensions["A"].width=24; ws4.column_dimensions["B"].width=110
wb.active=0; wb.save(OUT)
print(collections.Counter(r[0] for r in cmp))
