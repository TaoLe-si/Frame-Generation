import pathlib

# 自定位：脚本跟着仓库走，别写死工作区绝对路径
FORK = pathlib.Path(__file__).resolve().parents[1]
REFS = FORK.parent / "DLSS-refs"

import json, os, sys
from urllib.parse import quote
from urllib.request import urlopen, Request

OUT = str(FORK / "libs")
os.makedirs(OUT, exist_ok=True)
UA = {"User-Agent": "dlssmc-dev/0.1 (local build script)"}


def get_json(url):
    with urlopen(Request(url, headers=UA), timeout=60) as r:
        return json.load(r)


def search(project, game_version, loader):
    gv = quote(json.dumps([game_version]))
    ld = quote(json.dumps([loader]))
    return get_json("https://api.modrinth.com/v2/project/%s/version?game_versions=%s&loaders=%s"
                    % (project, gv, ld))


def download(project):
    versions = search(project, "1.20.1", "forge")
    if not versions:
        print(project, "-> 该平台没有 1.20.1 forge 版本")
        return
    v = versions[0]
    print("%-8s -> %s  (%s)" % (project, v["version_number"], v["date_published"][:10]))
    files = v["files"]
    primary = next((f for f in files if f.get("primary")), files[0])
    dest = os.path.join(OUT, primary["filename"])
    if os.path.exists(dest) and os.path.getsize(dest) > 0:
        print("          已存在 %s" % primary["filename"])
        return
    with urlopen(Request(primary["url"], headers=UA), timeout=300) as r, open(dest, "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            f.write(chunk)
    print("          下载完成 %s (%d bytes)" % (primary["filename"], os.path.getsize(dest)))


if __name__ == "__main__":
    for p in sys.argv[1:] or ["embeddium", "oculus"]:
        try:
            download(p)
        except Exception as e:
            print(p, "ERROR", e)
