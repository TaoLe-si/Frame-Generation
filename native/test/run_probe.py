import hashlib
import itertools
import json
from pathlib import Path
import statistics
import subprocess
import sys
import time


root = Path(__file__).resolve().parent
exe = root / "build" / "fg_probe.exe"
sdk = Path("D:/Backup/Downloads/streamline-sdk-v2.14.1/bin/x64")
output = root / "build" / "low-overhead-retest" / time.strftime("%Y%m%d-%H%M%S")
output.mkdir(parents=True, exist_ok=False)
paths = [exe, root / "fg_probe.cpp", root / "build_fg.sh", Path(__file__).resolve()]
paths += [sdk / name for name in ("sl.interposer.dll", "sl.common.dll", "sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll", "nvngx_dlssg.dll")]


def hashes():
    result = {}
    for path in paths:
        with path.open("rb") as source:
            result[str(path)] = hashlib.file_digest(source, "sha256").hexdigest()
    return result


before = hashes()
rows = []
failures = []
report = {
    "scope": "Headless entry availability and vkGetDeviceQueue CPU dispatch only; not frame generation or FPS",
    "timing_order": ["interposer export target", "SL device proc address", "system device proc address"],
    "warmup_rounds": 1,
    "measured_rounds": 7,
    "calls_per_round_per_path": 100000,
    "log_level": "off",
    "ota": False,
    "sha256": before,
    "rows": rows,
    "failures": failures,
}
if len(sys.argv) == 4:
    cases = [(sys.argv[1], int(sys.argv[2]), int(sys.argv[3]))]
elif len(sys.argv) == 1:
    cases = list(itertools.product(("export", "proc"), (0, 1), (0, 1)))
else:
    raise SystemExit("Usage: run_probe.py [export|proc nvx:0|1 manual:0|1]")
for route, nvx, manual in cases:
    name = f"{route}-nvx{nvx}-manual{manual}"
    print(f"Running {name}", flush=True)
    stdout_path = output / f"{name}.stdout.log"
    stderr_path = output / f"{name}.stderr.log"
    process_exit = None
    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        try:
            result = subprocess.run([str(exe), route, str(nvx), str(manual)], cwd=output,
                                    stdout=stdout, stderr=stderr, timeout=20)
            process_exit = result.returncode
        except subprocess.TimeoutExpired:
            failures.append({"case": name, "error": "timeout after 20 seconds; child killed and waited"})
    records = [json.loads(line) for line in stdout_path.read_text(encoding="utf-8", errors="replace").splitlines()
               if line.startswith("{")]
    if process_exit not in (None, 0):
        failures.append({"case": name, "process_exit": process_exit})
    if records and "route" in records[0]:
        row = records[0]
        row["process_exit"] = process_exit
        row["clean_exit"] = process_exit == 0 and len(records) == 2 and records[1]["exit_code"] == 0
        assert (row["route"], row["nvx"], row["manual"]) == (route, nvx, manual)
        assert row["sl_nvx_entries"] == row["driver_nvx_entries"] == (8 if nvx else 0), row
        assert row["nvx_addresses_equal"] == 8 and row["hook_addresses_equal"] == 5, row
        assert row["queue_proc_equals_driver"] == 1, row
        assert row["allocation_attempted"] is False and row["present_calls"] == 0, row
        rows.append(row)
        print(json.dumps(row, ensure_ascii=False), flush=True)
    if process_exit != 0:
        print(f"INCOMPLETE: {name}; {stderr_path.read_text(errors='replace')}", flush=True)
    (output / "results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
assert before == hashes(), "Probe or runtime files changed during the comparison"
report["median_of_case_medians_ns"] = [statistics.median(row["queue_ns_median"][i] for row in rows) for i in range(3)] if rows else None
report["all_children_reaped"] = True
(output / "results.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
print(f"DONE: {len(rows)} measured, {len(failures)} failed or timed out; results: {output / 'results.json'}", flush=True)
raise SystemExit(1 if failures else 0)
