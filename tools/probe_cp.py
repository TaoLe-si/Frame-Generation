import json, os, sys

BASE = r"E:\MC\.minecraft"
INST = os.path.join(BASE, "versions", "GregTech Leisure")
d = json.load(open(os.path.join(INST, "GregTech Leisure.json"), encoding="utf-8"))

missing = []
cp = []
for l in d["libraries"]:
    dl = l.get("downloads", {}).get("artifact")
    if dl and dl.get("path"):
        p = os.path.join(BASE, "libraries", dl["path"].replace("/", os.sep))
    else:
        g, art, ver = l["name"].split(":")[:3]
        p = os.path.join(BASE, "libraries", *g.split("."), art, ver, f"{art}-{ver}.jar")
    if os.path.exists(p):
        cp.append(p)
    else:
        missing.append(l["name"])

print("libs", len(d["libraries"]), "resolved", len(cp), "missing", len(missing))
for m in missing[:20]:
    print("  MISSING", m)

# client jar
cli = os.path.join(INST, "GregTech Leisure.jar")
print("client jar", os.path.exists(cli), os.path.getsize(cli) if os.path.exists(cli) else "-")
print("assets", os.path.exists(os.path.join(BASE, "assets", "indexes", d["assetIndex"]["id"] + ".json")))
