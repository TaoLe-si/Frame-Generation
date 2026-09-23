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
result = subprocess.run([
    "D:/Java21/bin/java.exe", *args, "MfgProbeCheck", dll,
], cwd=root, timeout=300)
print(f"MFG probe exit: {result.returncode}", flush=True)
sys.exit(result.returncode)
