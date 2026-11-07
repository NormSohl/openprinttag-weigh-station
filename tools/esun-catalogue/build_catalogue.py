# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""work/catalogue.json + barcodes.json + optmatch.json
-> docs/reference/esun-us-catalogue-DATE.xlsx and work/catalogue_full.json.

Main tab: what eSUN US will sell today. The rest stay on a grey tab, not deleted.
"""
import collections, json, os, subprocess
from openpyxl import Workbook
from openpyxl.styles import Font,PatternFill,Alignment
from openpyxl.utils import get_column_letter
from openpyxl.formatting.rule import FormulaRule
from esunlib import DATE, OPT_DIR, REF, load, save

CAT = load("catalogue.json")
_BC = load("barcodes.json")
_uses = collections.Counter(g for d in _BC.values() for g in set(sum(d.values(), [])))
SRC = {"OpenPrintTag": "the OpenPrintTag database", "Lieske": "Lieske (EU distributor)"}
_OPT = load("optmatch.json")
_OPTM = _OPT


def barcode(pn):
    d = _BC.get(pn)
    if not d:
        m = _OPTM.get(pn)
        if m and m[0] == "Yes" and len(m) > 2 and m[2]:
            return ", ".join(m[2]), "Barcode from the OpenPrintTag database (matched by name)"
        return "", ""
    gs = sorted(set(sum(d.values(), [])))
    srcs = [SRC[s] for s in ("OpenPrintTag", "Lieske") if s in d]
    note = ["Barcode from " + (" and ".join(srcs)) + (", which agree" if len(srcs) > 1 and len(gs) == 1 else "")]
    if len(gs) > 1: note.append("two barcodes listed, probably old and new packaging")
    if any(_uses[g] > 1 for g in gs): note.append("the same barcode is listed for both PETG spool types (S1 and Y1); which one it belongs to is unknown")
    return ", ".join(gs), "; ".join(note)


try:
    OPT_REV = subprocess.run(["git", "-C", OPT_DIR, "log", "-1", "--format=%h of %cd", "--date=short"],
                             capture_output=True, text=True).stdout.strip() or "unknown"
except OSError:
    OPT_REV = "unknown"
OUT = os.path.join(REF, f"esun-us-catalogue-{DATE}.xlsx")
rows = []
for c in CAT:
    bc, bnote = barcode(c["pn"])
    note = "; ".join(x for x in [bnote, "eSUN lists this PETG refill under a PLA+ part number" if c["pn"] == "PLA+175T1OR1" else ""] if x)
    # [0 category, 1 series, 2 product, 3 material, 4 colour, 5 part no, 6 filament id, 7 barcode,
    #  8 size, 9 packaging, 10 salable, 11 price, 12 regular, 13 page, 14 notes]
    rows.append([c["category"], c["series"], c["product"], c["material"], c["colour"], c["pn"], c["fid"], bc,
                 c["size"], c["packaging"], c["salable"], c["price"], c["regular"], c["page"], note])
save("catalogue_full.json", [dict(c, barcode=r[7], notes=r[14], opt=_OPT[c["pn"]][0], orderable=(c["salable"] == "yes"))
                             for c, r in zip(CAT, rows)])
F=Font(name="Arial",size=10); FB=Font(name="Arial",size=10,bold=True,color="FFFFFF"); HF=PatternFill("solid",fgColor="2F5D3A")
MAIN="eSUN US catalogue"; OTHER="Not orderable on eSUN US"
# (optmatch loaded above)
OPTLBL={"Yes":"Yes","Material only":"Colour only, not this package","No":"No"}
head=["Category","Material","Colour / option","eSUN part no","Filament ID","Barcode","Size (kg)","Packaging","In OpenPrintTag database","Salable on eSUN US","Price (USD)","Regular price (USD)","Product page","Notes","Series"]
def data_sheet(ws,data):
    ws.append(head)
    for c in ws[1]: c.font=FB; c.fill=HF; c.alignment=Alignment(vertical="center",wrap_text=True)
    for r in data:
        o=r[3:]
        ws.append(r[:1]+o[:7]+[OPTLBL[_OPT[r[5]][0]]]+o[7:]+[r[1]])   # no Product; OpenPrintTag after Packaging; Series last
    for rr in ws.iter_rows(min_row=2):
        for c in rr: c.font=F
    for i,w in enumerate([13,18,30,30,22,16,8,14,16,11,10,11,46,60,30],1): ws.column_dimensions[get_column_letter(i)].width=w
    for col in "KL":
        for c in ws[col][1:]: c.number_format="$#,##0.00"
    ws.conditional_formatting.add(f"I2:I{len(data)+1}",FormulaRule(formula=['$I2="No"'],fill=PatternFill("solid",fgColor="FCE4D6")))
    ws.conditional_formatting.add(f"I2:I{len(data)+1}",FormulaRule(formula=['LEFT($I2,6)="Colour"'],fill=PatternFill("solid",fgColor="FFF2CC")))
    for c in ws["G"][1:]: c.number_format="0.##"
    for c in ws["F"][1:]: c.number_format="@"
    ws.freeze_panes="A2"; ws.auto_filter.ref=ws.dimensions
    return len(data)+1
rows.sort(key=lambda r:(r[3],r[4],r[8] or 0,r[9] or "",r[5]))   # material, colour, size, packaging
ok=[r for r in rows if r[10]=="yes"]; no=[r for r in rows if r[10]!="yes"]
wb=Workbook(); ws=wb.active; ws.title=MAIN
n=data_sheet(ws,ok)
wo=wb.create_sheet(OTHER); n2=data_sheet(wo,no)
wo.sheet_properties.tabColor="BFBFBF"
# summary: materials you can order, formulas over the main tab
sm=wb.create_sheet("By material",0)
sm.append([f"eSUN US catalogue, read from esun3d.com on {DATE}"]); sm["A1"].font=Font(name="Arial",size=12,bold=True)
sm.append(["Materials with at least one variant the US store can sell today. Variants it lists but can't sell are on the 'Not orderable on eSUN US' tab."]); sm["A2"].font=F
sm.append([])
sm.append(["Category","Material","Orderable variants","Not orderable","Lowest price (USD)","Orderable, not in OpenPrintTag"])
for c in sm[4]: c.font=FB; c.fill=HF; c.alignment=Alignment(wrap_text=True,vertical="center")
prods=sorted({(r[0],r[3]) for r in ok})
r0=5
rng=f"'{MAIN}'!$B$2:$B${n}"; rng2=f"'{OTHER}'!$B$2:$B${n2}"
for i,(cat,p) in enumerate(prods,r0):
    sm.append([cat,p,f'=COUNTIF({rng},B{i})',f'=COUNTIF({rng2},B{i})',
               f"=IFERROR(_xlfn.MINIFS('{MAIN}'!$K$2:$K${n},{rng},B{i}),\"\")",
               f"=COUNTIFS({rng},B{i},'{MAIN}'!$I$2:$I${n},\"No\")"])
last=r0+len(prods)-1
sm.append(["Total","",f"=SUM(C{r0}:C{last})",f"=SUM(D{r0}:D{last})","",f"=SUM(F{r0}:F{last})"])
for rr in sm.iter_rows(min_row=5):
    for c in rr: c.font=F
for c in sm[last+1]: c.font=Font(name="Arial",size=10,bold=True)
for c in sm["E"][4:]: c.number_format="$#,##0.00"
for i,w in enumerate([14,34,12,12,16,16],1): sm.column_dimensions[get_column_letter(i)].width=w
sm.freeze_panes="A5"; sm.auto_filter.ref=f"A4:F{last}"
ab=wb.create_sheet("About")
for r in [["About this list"],[],
 ["Source","esun3d.com, eSUN's own US store: every product page under Filaments, read in a browser on "+DATE+". Resins and accessories are left out."],
 ["eSUN part no","eSUN's own part number for the variant, as the page publishes it."],
 ["Salable on eSUN US","Whether the US store ('us_site') will sell that variant today, as the page publishes it. The main tab holds only variants it will sell; the rest are kept on the 'Not orderable on eSUN US' tab rather than deleted. eSUN doesn't say whether 'not orderable' means out of stock for now or not sold in the US, so an item can move between the tabs when the list is refreshed."],
 ["Material","The base material, from eSUN's product name with the packaging words taken out: eSUN's 'PLA+', 'PLA+Refilament', 'PLA+ Refilament Set' and 'PLA+3KG1.75MM' are all PLA+. eSUN's own product name is not kept as a column, since it only adds what Size and Packaging already show."],
 ["Filament ID","The eSUN part number with its size and packaging removed: the same filament in every package it comes in. PLA+175B is PLA+ Black, whether bought as PLA+175B1P1 (1 kg paper spool), PLA+175B1R1 (refill), PLA+175B1RS1 (refill set), PLA+175B3B1 (3 kg) or PLA+175B5B1 (5 kg). Colour codes only mean something within a product line, so the ID keeps the line prefix. Multi-packs and bundles (CMYK sets, HJ- bundles, the PLA-Basic 250 g x8 mini pack) are left out: every row is one unit of one filament. A Refilament Set is one refill sold with a reusable eSpool+. One eSUN listing is corrected: PETG Refilament Clear is listed as PLA+175T1OR1 and is given PETG175T."],
 ["Barcode","eSUN publishes no barcodes on its US site. These come from the OpenPrintTag database and from Lieske Elektronik's catalogue (an EU distributor), matched to eSUN's part number; the Notes column says which. A barcode is given only where the packaging matches (spool, refill and refill set each have their own). Where both sources list one they agree. Two OpenPrintTag barcodes that the database gives to two different products were left out. Not yet confirmed by scanning a US box."],
 ["Series","The product's place in the eSUN US store menu, from the page's breadcrumb trail (levels between Filaments and the product name, joined with ' > '). A shop-menu label, not a product attribute; a product filed in several menus shows only one."],
 ["In OpenPrintTag database","Whether the OpenPrintTag database (the one the station's Restock search uses; openprinttag-database on GitHub, read at commit "+OPT_REV+") has this item. Yes: this package (same colour, size, and spool or refill), matched by name or barcode. Colour only: it has the colour, but not in this packaging or size, so a search finds a near miss. No: it has nothing for this colour. Names are compared as sets of words, with the database's different wording mapped across: Carbon Fiber PLA = PLA-CF, Super Tough PLA = PLA-ST, PLA Silk Metal = PLA Silk, Transparent = Translucent, and Solid / Opaque ignored."],
 ["Price","The US store's current price and its regular price, in USD."],
 ["Size, Packaging","Decoded from the part number: the digits after the colour code are the size in kg (075 = 0.75 kg); the suffix is the packaging (P1 paper spool, R1 refill, RS1 refill set, B1 plastic spool; S1, Y1 and TZ1 are other spool types eSUN uses but doesn't name). Blank where the part number doesn't follow that pattern."],
 ["Not included","Barcodes: eSUN's site doesn't publish them."]]:
    ab.append(r)
ab["A1"].font=Font(name="Arial",size=12,bold=True)
for rr in ab.iter_rows(min_row=3):
    rr[0].font=Font(name="Arial",size=10,bold=True); rr[0].alignment=Alignment(vertical="top")
    if len(rr)>1: rr[1].font=F; rr[1].alignment=Alignment(wrap_text=True,vertical="top")
ab.column_dimensions["A"].width=20; ab.column_dimensions["B"].width=110
wb.move_sheet(OTHER, offset=len(wb.sheetnames)); wb.active=0; wb.save(OUT)
print(OUT,"orderable",len(ok),"not",len(no),"materials",len(prods))
