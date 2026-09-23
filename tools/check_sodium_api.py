import sys
from urllib.request import urlopen, Request

base = "https://maven.caffeinemc.net/net/caffeinemc/"
for art in ["sodium-neoforge-api", "sodium-fabric-api"]:
    url = base + art + "/maven-metadata.xml"
    try:
        with urlopen(Request(url), timeout=30) as r:
            data = r.read().decode("utf-8", "replace")
        versions = []
        for line in data.splitlines():
            line = line.strip()
            if line.startswith("<version>"):
                versions.append(line[len("<version>"):-len("</version>")])
        print(art, "->", len(versions), "versions")
        mc1211 = [v for v in versions if "1.21.1" in v]
        print("   1.21.1 matches:", mc1211[-12:] if mc1211 else "NONE")
        print("   latest:", versions[-8:] if versions else "NONE")
    except Exception as e:
        print(art, "ERROR", e)
