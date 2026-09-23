import pathlib
import shlex
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[2]
args = shlex.split((root / "build/moddev/clientRunClasspath.txt").read_text())
args[1] += ";" + ";".join(str(root / "libs" / name) for name in (
    "sodium-neoforge-0.8.13-mod.jar", "iris-neoforge-1.8.14-beta.1+mc1.21.1.jar"))
result = subprocess.run([
    "D:/Java21/bin/javac.exe", *args, "-proc:none", "-d", str(root / "native/build"),
    str(root / "native/test/MfgProbeCheck.java"),
], cwd=root, timeout=60)
if result.returncode:
    sys.exit(result.returncode)
args[1] += ";" + str(root / "native/build")
dll = sys.argv[1] if len(sys.argv) > 1 else str(root / "native/build/dlssmc_fg.dll")
# 第二个参数可选：插件目录。用来验证「只带 sl.*、不带 nvngx」时 NGX 还能不能起来
extra = sys.argv[2:] if len(sys.argv) > 2 else []
result = subprocess.run([
    "D:/Java21/bin/java.exe", *args, "MfgProbeCheck", dll, *extra,
], cwd=root, timeout=300)
print(f"MFG probe exit: {result.returncode}", flush=True)
sys.exit(result.returncode)
