import os, zipfile
from urllib.request import urlopen, Request

url = "https://api.adoptium.net/v3/binary/latest/21/ga/windows/x64/jdk/hotspot/normal/eclipse"
dest_zip = r"E:\DLSS for Minecraft\toolchain\jdk21.zip"
dest_dir = r"E:\DLSS for Minecraft\toolchain"

os.makedirs(dest_dir, exist_ok=True)
print("downloading...", flush=True)
req = Request(url)
with urlopen(req, timeout=600) as r, open(dest_zip, "wb") as f:
    while True:
        chunk = r.read(1 << 20)
        if not chunk:
            break
        f.write(chunk)
print("downloaded", os.path.getsize(dest_zip), flush=True)
print("extracting...", flush=True)
with zipfile.ZipFile(dest_zip) as z:
    z.extractall(dest_dir)
print("done", flush=True)
