import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import jcp

root = pathlib.Path(__file__).resolve().parents[2]
cp = jcp.classpath(root)
result = subprocess.run([
    jcp.JAVAC, "-cp", cp, "-proc:none", "-d", str(root / "native/build"),
    str(root / "native/test/MfgProbeCheck.java"),
], cwd=root, timeout=60)
if result.returncode:
    sys.exit(result.returncode)
dll = sys.argv[1] if len(sys.argv) > 1 else str(root / "native/build/dlssmc_fg.dll")
# 第二个参数可选：插件目录。用来验证「只带 sl.*、不带 nvngx」时 NGX 还能不能起来
extra = sys.argv[2:] if len(sys.argv) > 2 else []
result = subprocess.run([
    jcp.JAVA, "-cp", cp + ";" + str(root / "native/build"), "MfgProbeCheck", dll, *extra,
], cwd=root, timeout=300)
print(f"MFG probe exit: {result.returncode}", flush=True)
sys.exit(result.returncode)
