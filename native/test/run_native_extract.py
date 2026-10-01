import pathlib, subprocess, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import jcp

root = pathlib.Path(__file__).resolve().parents[2]
cp = jcp.classpath(root)
out = root / "native" / "build"
r = subprocess.run([jcp.JAVAC, "-cp", cp, "-proc:none", "-d", str(out),
                    str(root / "native/test/NativeExtractCheck.java")], cwd=root, timeout=120)
if r.returncode:
    sys.exit(r.returncode)
r = subprocess.run([jcp.JAVA, "-cp", cp + ";" + str(out), "NativeExtractCheck"],
                   cwd=root, timeout=300)
if r.returncode:
    sys.exit(r.returncode)

# 第二遍：把 SystemRoot 指到一个空目录，验证「缺运行库」那条提示确实会出来
import os, tempfile
env = dict(os.environ)
env["SystemRoot"] = tempfile.mkdtemp(prefix="dlssmc-fake-root-")
env["DLSSMC_FAKE_SYSTEMROOT"] = "1"
print("\n=== 假装没装运行库（SystemRoot=" + env["SystemRoot"] + "）===")
r = subprocess.run([jcp.JAVA, "-cp", cp + ";" + str(out), "NativeExtractCheck",
                    str(root / "build" / "native-extract-check2")],
                   cwd=root, timeout=300, env=env)
print("原生解包判据（缺运行库场景）exit:", r.returncode)
sys.exit(r.returncode)
