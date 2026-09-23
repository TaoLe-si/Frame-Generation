import argparse
import pathlib
import shlex
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]


def classpath(override=None):
    if override is not None:
        # Supply a complete classpath (packaged mod jar + dependencies) to test jar-only extraction.
        return ["-classpath", ";".join(str(pathlib.Path(p).resolve()) for p in override.split(";"))]
    args = shlex.split((root / "build/moddev/clientRunClasspath.txt").read_text())
    args[1] += ";" + ";".join(str(root / "libs" / name) for name in (
        "sodium-neoforge-0.8.13-mod.jar", "iris-neoforge-1.8.14-beta.1+mc1.21.1.jar"))
    return args


if "--sr" in sys.argv[1:]:
    parser = argparse.ArgumentParser(description="Hidden production Vulkan SR/FG regression; no game launch")
    parser.add_argument("--sr", action="store_true")
    modes = parser.add_mutually_exclusive_group()
    modes.add_argument("--routing-only", action="store_true", help="Only test in-memory DLSSConfig, without native/GL")
    modes.add_argument("--timing", action="store_true",
                       help="Four fixed-pattern 2560x1346 bridge comparisons in one JVM; not game input latency")
    parser.add_argument("--order", choices=("all", "sr-first", "fg-first", "sr-only"), default="all")
    parser.add_argument("--classpath", help="Complete semicolon-separated classpath, forwarded to javac and every JVM")
    parser.add_argument("--plugins", default="D:/Backup/Downloads/streamline-sdk-v2.14.1/bin/x64")
    options = parser.parse_args()
    if options.timing and options.order != "all":
        parser.error("--timing runs its own four cases; do not select an --order")
    args = classpath(options.classpath)
    plugins = str(pathlib.Path(options.plugins).resolve())
    # Keep diagnostics in the system temp directory, including any relative JVM/SDK/log4j output.
    # No native/build DLL argument: the production loader must extract its classpath resource.
    cache = pathlib.Path(tempfile.mkdtemp(prefix="dlssmc-sr-check-"))
    print(f"SR check temporary classes/cache/logs (retained): {cache}", flush=True)
    result = subprocess.run([
        "D:/Java21/bin/javac.exe", *args, "-proc:none", "-d", str(cache),
        str(root / "native/test/SrInitOrderCheck.java"),
    ], cwd=cache, timeout=60)
    if result.returncode:
        sys.exit(result.returncode)
    args[1] = str(cache) + ";" + args[1]
    orders = ("routing",) if options.routing_only else (
        ("sr-first", "fg-first", "sr-only") if options.order == "all" else (options.order,))
    if options.timing:
        orders = ("timing",)  # One native init/device, not the three init-order JVMs.
    for order in orders:
        run_dir = cache / order
        run_dir.mkdir()
        test_args = ["--routing-only"] if options.routing_only else ["--order", order, "--plugins", plugins]
        if options.timing:
            test_args = ["--timing", "--plugins", plugins]
        print(f"Running SrInitOrderCheck {order} (hidden)", flush=True)
        result = subprocess.run([
            "D:/Java21/bin/java.exe", "-Djava.io.tmpdir=" + str(run_dir), *args,
            "SrInitOrderCheck", *test_args,
        ], cwd=run_dir, timeout=600 if options.timing else 300)
        print(f"SR {order} process exit: {result.returncode} (0x{result.returncode & 0xffffffff:08x})", flush=True)
        if result.returncode:
            sys.exit(result.returncode)
    sys.exit(0)

# Existing FGToggleCheck invocation and arguments remain unchanged.
args = classpath()
result = subprocess.run([
    "D:/Java21/bin/javac.exe", *args, "-proc:none", "-d", str(root / "native/build"),
    str(root / "native/test/FGToggleCheck.java"),
], cwd=root, timeout=60)
if result.returncode:
    sys.exit(result.returncode)
args[1] += ";" + str(root / "native/build")
dll = sys.argv[1] if len(sys.argv) > 1 else str(root / "native/build/dlssmc_fg.dll")
result = subprocess.run([
    "D:/Java21/bin/java.exe", *args, "FGToggleCheck", dll, *sys.argv[2:],
], cwd=root, timeout=120)
print(f"FG check process exit: {result.returncode} (0x{result.returncode & 0xffffffff:08x})", flush=True)
sys.exit(result.returncode)
