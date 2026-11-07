# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Norm Sohl
"""Match Lieske's eSUN catalogue (EU distributor, has barcodes) to eSUN US's
variants -> work/lieske_match.json.

The two use different part-number schemes for one product (Lieske PLAP175B1 =
eSUN US PLA+175B1P1): match on family + colour code + size, else family + colour
name + size, refusing a colour-name match where eSUN's own codes say otherwise.
"""
import json,re,collections
from esunlib import ESUN_US_JSON, LIESKE_JSON, split, save
G=json.load(open(ESUN_US_JSON)); L=json.load(open(LIESKE_JSON))
FAM={"PLAP":"PLA+","PLA":"PLA+","ABSP":"ABS+","ASAP":"ASA+","EABS-CF-P":"ABS-CF","EABS-GF-P":"ABS-GF","EABS-HT-P":"ABS-HT","EABSHS-P":"ABS+HS",
 "EPA12-CF":"PA12+CF","EPA-CF":"PA-CF","EPETG-CF-P":"PETG-CF","EPETGHS-P":"PETG+HS","EPLA-CF-P":"PLA-CF","EPLA-HS-P":"PLA-HS","EPLA-LITE-P":"PLA-LITE",
 "EPLA-LUMINOUS":"PLA-LM","PLA-P":"PLA-LM","EPLA-MAGIC-P":"PLA-MG","EPLA-MATTE-P":"PLA-MT","EPLA-METAL-P":"PLA-METAL","EPLA-SILK-P":"PLA-SK","ESILK-PLA":"PLA-SK",
 "EPLA-SILKCANDY":"EPLA-SILKCANDY-P","EPLA-SILKMAGIC-P":"PLA-SKMG","EPLA-SILKMYSTIC-P":"PLA-SKMC","EPLA-ST":"PLA-ST","EPLAHS-P":"PLA+HS","ESTARS-PLA":"PLA-STARS",
 "ETPU-95A":"TPU-95A","ETWINKLING":"PLA-TK","EMARBLE":"PLA-MB","PLA-ROCK75O-QT1P1":"PLA-ROCK","EPLA-SILK":"PLA-SKRB"}
DE={"SCHWARZ":"BLACK","GRÜN":"GREEN","GRAU":"GREY","ROT":"RED","BLAU":"BLUE","WEISS":"WHITE","WEIß":"WHITE","GELB":"YELLOW","TRANSPARENT ORANGE":"TRANSLUCENT ORANGE","DUNKELGRAU":"DARK GREY"}
def ncol(c):
    c=c.upper().strip(); c=DE.get(c,c)
    for a,b in [("GRAY","GREY"),("FIRE ENG. RED","FIRE ENGINE RED"),("TRANSL ","TRANSLUCENT "),("VERY PRI","VERY PERI"),("DARK TWINKL ","DARK TWINKLING "),("RASBERRY","RASPBERRY"),("-"," ")]:
        c=c.replace(a,b)
    if c=="FIRE ENGINE": c="FIRE ENGINE RED"
    return re.sub(r"\s+"," ",c)
def nsize(s):
    if not s: return ""
    if "." in s: return str(float(s)).rstrip("0").rstrip(".")
    if s.startswith("0") and len(s)>1: return str(float("0."+s[1:])).rstrip("0").rstrip(".")
    return str(int(s))
PACK={"P1":"paper spool","R1":"refill","RS1":"refill set","B1":"plastic spool","S1":"spool (S1)","Y1":"spool (Y1)","TZ1":"tub (TZ1)","P4":"4-pack","":""}
US=[]
for path,v in G:
    for s in v["v"]:
        sku,col,sal,price,was=s.split("|")
        f=split(sku); fam=f[0]
        US.append({"title":v["t"],"page":"https://www.esun3d.com/"+path,"cat":v["c"],"sku":sku,"colour":col,"ncol":ncol(col),"fam":fam,"code":f[2],"size":nsize(f[3]),"pack":f[4],
                   "salable":{"1":"yes","0":"no"}.get(sal,"not shown"),"price":float(price) if price not in("","?") else None,"was":float(was) if was not in("","?") else None})
LI=[]
for o in L:
    f=split(o["mpn"]); fam=FAM.get(f[0],f[0]); code,size,pack=f[2],nsize(f[3]),f[4]
    rest=o["name"].split(o["mpn"],1)[-1]
    m=re.search(r"1[.,]75\s*mm\s*(.*?)\s*(\d+(?:[.,]\d+)?\s*kg|$)",rest,re.I)
    col=ncol(m.group(1)) if m else ""
    col=re.sub(r"\s*(PAPIER.*|KUNSTSTOFF.*|REFILL|FILAMENT)$","",col).strip()
    LI.append(dict(o,fam=fam,code=code,size=size,pack=pack,ncol=col))
# match
SOLID={"PLA+","PLA+HS","PLA-BASIC","PLA-HS","PETG","PETG+","PETG-BASIC","ABS","ABS+","ABS+HS","ASA+","PLA-LITE"}
grp=lambda f:"solid" if f in SOLID else f
byCode=collections.defaultdict(list); byCol=collections.defaultdict(list); CODE=collections.defaultdict(set)
nh=lambda c:c.replace("-","")
for i,u in enumerate(US):
    byCode[(u["fam"],nh(u["code"]),u["size"])].append(i); byCol[(u["fam"],u["ncol"],u["size"])].append(i)
    if u["code"] and u["ncol"]: CODE[(grp(u["fam"]),nh(u["code"]))].add(u["ncol"])
conflicts={}
usedUS=set(); rows=[]
def pick(cands,pack):
    same=[i for i in cands if US[i]["pack"]==pack]; return (same or cands)
ALT={"PETG":["PETG+","PETG"],"PLA-SK":["PLA-SK","PLA-SKRB"],"PLA-MT":["PLA-MT","PLA-MTRB"],"PLA-LM":["PLA-LM"]}
def colvars(c):
    v=[c]
    if c.startswith("SOLID "): v.append(c[6:])
    if c.startswith("TRANSPARENT "): v.append("TRANSLUCENT "+c[12:])
    v.append("LUMINOUS "+c)
    return v
for o in LI:
    basis=""; cands=[]
    fams=ALT.get(o["fam"],[o["fam"]])
    if o["code"]:
        for fam in fams:
            c=byCode.get((fam,nh(o["code"]),o["size"]),[])
            if o["fam"]=="PETG" and fam=="PETG+" and o["pack"] not in ("P1",""): c=[]
            if c: cands=c; basis="part code"+("" if fam==o["fam"] else f" ({fam})"); break
        means=set().union(*[CODE.get((grp(f),nh(o["code"])),set()) for f in fams])
        if not cands and o["ncol"] and means and not (set(colvars(o["ncol"]))&means):
            conflicts[o["lieske"]]="eSUN uses code "+o["code"]+" for "+"/".join(sorted(means))+"; Lieske's name says "+o["ncol"]
        elif not cands and o["ncol"]:
            for fam in fams:
                for cv in colvars(o["ncol"]):
                    c=byCol.get((fam,cv,o["size"]),[])
                    if c: cands=c; basis="colour name"+("" if fam==o["fam"] else f" ({fam})"); break
                if cands: break
    if not cands:
        k=re.sub(r"\d+G$","",o["mpn"].upper().replace(" ",""))
        c=[i for i,u in enumerate(US) if u["sku"].upper().replace(" ","")==k]
        if c: cands=c; basis="part code (resin, pack size dropped)"
    if cands:
        c=pick(cands,o["pack"])
        for i in c: usedUS.add(i)
        rows.append((o,[US[i] for i in c],basis))
    else: rows.append((o,[],""))
save("lieske_match.json",{"US":US,"LI":LI,"rows":[(o["mpn"],o["lieske"],[u["sku"] for u in us],b) for o,us,b in rows],"usOnly":[i for i in range(len(US)) if i not in usedUS],"conflicts":conflicts})
m=sum(1 for _,us,_ in rows if us); print("Lieske rows",len(rows),"matched",m,"unmatched",len(rows)-m,"| US variants",len(US),"matched",len(usedUS),"US-only",len(US)-len(usedUS))
print(collections.Counter(b for _,us,b in rows if us))
