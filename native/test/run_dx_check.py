"""把 Java 侧的探测跑一遍：设置页注册、帧生成输入、帧率统计、DX 后端切换。

classpath 从整合包实例拼（见 jcp.py），和实机跑的是同一批字节码。
DXToggleCheck 会真的建 GL/D3D12 设备，所以得在能拿到显卡的会话里跑。
"""
import pathlib
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import jcp

root = pathlib.Path(__file__).resolve().parents[2]
cp = jcp.classpath(root)
checks = ("EmbeddiumPageCheck", "FGInputCheck", "FrameRateCheck", "DXToggleCheck")
subprocess.run([
    jcp.JAVAC, "-cp", cp, "-proc:none", "-d", str(root / "native/build"),
    *(str(root / "native/test" / (name + ".java")) for name in checks),
], cwd=root, timeout=120, check=True)
for name in checks[:-1]:
    print("Running " + name, flush=True)
    subprocess.run([jcp.JAVA, "-cp", cp + ";" + str(root / "native/build"), name],
                   cwd=root, timeout=60, check=True)
dll = sys.argv[1] if len(sys.argv) > 1 else str(root / "native/build/dlssmc_dx.dll")
with tempfile.TemporaryDirectory(prefix="dlssmc-dx-check-") as cache:
    result = subprocess.run([
        jcp.JAVA, "-Djava.io.tmpdir=" + cache, "-cp", cp + ";" + str(root / "native/build"),
        "DXToggleCheck", dll, *sys.argv[2:],
    ], cwd=root, timeout=300)
print(f"DX check process exit: {result.returncode} (0x{result.returncode & 0xffffffff:08x})",
      flush=True)
sys.exit(result.returncode)
