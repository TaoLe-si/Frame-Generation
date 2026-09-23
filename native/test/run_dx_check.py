import pathlib
import shlex
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
args = shlex.split((root / "build/moddev/clientRunClasspath.txt").read_text())
args[1] += ";" + ";".join(str(root / "libs" / name) for name in (
    "sodium-neoforge-0.8.13-mod.jar", "iris-neoforge-1.8.14-beta.1+mc1.21.1.jar"))
checks = ("SodiumOptionsCheck", "FGInputCheck", "FrameRateCheck", "DXToggleCheck")
subprocess.run([
    "D:/Java21/bin/javac.exe", *args, "-proc:none", "-d", str(root / "native/build"),
    *(str(root / "native/test" / (name + ".java")) for name in checks),
], cwd=root, timeout=120, check=True)
# build/resources/main 必须在 classpath 上：生产加载路径是从 classpath 资源里解包 DLL
args[1] += ";" + str(root / "native/build") + ";" + str(root / "build/resources/main")
for name in checks[:-1]:
    print("Running " + name, flush=True)
    subprocess.run(["D:/Java21/bin/java.exe", *args, name], cwd=root, timeout=60, check=True)
dll = sys.argv[1] if len(sys.argv) > 1 else str(root / "native/build/dlssmc_dx.dll")
with tempfile.TemporaryDirectory(prefix="dlssmc-dx-check-") as cache:
    result = subprocess.run([
        "D:/Java21/bin/java.exe", "-Djava.io.tmpdir=" + cache, *args, "DXToggleCheck", dll, *sys.argv[2:],
    ], cwd=root, timeout=300)
print(f"DX check process exit: {result.returncode} (0x{result.returncode & 0xffffffff:08x})",
      flush=True)
sys.exit(result.returncode)
