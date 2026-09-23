import zipfile, os, sys

jar = r"E:\DLSS for Minecraft\build\moddev\artifacts\neoforge-21.1.228-sources.jar"
out = r"E:\DLSS for Minecraft\ref\mcsrc"
wanted = sys.argv[1:] if len(sys.argv) > 1 else None

os.makedirs(out, exist_ok=True)
z = zipfile.ZipFile(jar)
names = z.namelist()
hits = []
for n in names:
    base = os.path.basename(n)
    if wanted:
        if base in wanted:
            hits.append(n)
    else:
        hits = []
        break

if not wanted:
    # 列出顶层包，帮助定位
    print("total entries:", len(names))
    for n in names[:20]:
        print(n)
else:
    for n in hits:
        target = os.path.join(out, os.path.basename(n))
        with open(target, "wb") as f:
            f.write(z.read(n))
        print("extracted:", target, len(z.read(n)))
