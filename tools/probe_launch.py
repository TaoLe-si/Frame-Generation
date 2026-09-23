"""在 GregTech Leisure 实例里直接起一次客户端，用于确认 dlssmc 能加载。

不复用启动器的 classpath 拼装逻辑，只按 1.20.1 的标准布局拼：
libraries 里的 artifact + 版本目录里的 client jar，参数用占位值。

默认把游戏目录指到一个干净的探测实例（run-probe/），mods 里只放 Embeddium / Oculus /
dlssmc —— 整合包本身有别的模组会在构造 Minecraft 时崩（SkyblockBuilder 的已知问题，
与 dlssmc 无关），留在里面就分不清是谁的错。加 --instance 则用整合包本体跑。

日志写在探测实例的 logs/probe-launch.log。
"""
import json, os, shutil, subprocess, sys

FORK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = r"E:\MC\.minecraft"
INST = os.path.join(BASE, "versions", "GregTech Leisure")
JAVA = r"D:\Java17\jdk-17.0.18\bin\java.exe"
NATIVES = os.path.join(INST, "GregTech Leisure-natives")

# 探测实例只要这几个：Embeddium 给选项 API，Oculus 是 FGRuntime 编译期依赖，
# dlssmc 是被测对象。Forge 与 MC 本身在 classpath 上，不用放进 mods。
MINIMAL_MODS = ["embeddium-0.3.31+mc1.20.1.jar", "oculus-mc1.20.1-1.8.0.jar",
                "frame-generation-0.2.2.jar"]

use_instance = "--instance" in sys.argv
GAME_DIR = INST if use_instance else os.path.join(FORK, "run-probe")

d = json.load(open(os.path.join(INST, "GregTech Leisure.json"), encoding="utf-8"))


def allowed(entry):
    """Mojang 的 rules 求值：默认 allow，命中的规则按顺序覆盖。

    带 features 的规则（--demo / quickPlay 那些）一律不选：探测用不上，
    而且 --demo 会让游戏进试玩模式，和实际游玩不是一回事。
    """
    rules = entry.get("rules")
    if not rules:
        return True
    result = False
    for r in rules:
        if "features" in r:
            continue
        os_rule = r.get("os", {})
        if "name" in os_rule and os_rule["name"] != "windows":
            continue
        if "arch" in os_rule and os_rule["arch"] != "x86_64":
            continue
        if r.get("action") == "allow":
            result = True
        elif r.get("action") == "disallow":
            result = False
    return result


cp = []
for lib in d["libraries"]:
    if not allowed(lib):
        continue
    dl = lib.get("downloads", {}).get("artifact")
    if dl and dl.get("path"):
        p = os.path.join(BASE, "libraries", dl["path"].replace("/", os.sep))
    else:
        g, art, ver = lib["name"].split(":")[:3]
        p = os.path.join(BASE, "libraries", *g.split("."), art, ver, f"{art}-{ver}.jar")
    if os.path.exists(p):
        cp.append(p)
cp.append(os.path.join(INST, "GregTech Leisure.jar"))

lib_path = [NATIVES]
lib_path += [os.path.join(BASE, "libraries", "org", "lwjgl", "lwjgl", "3.3.1", "lwjgl-3.3.1-natives-windows.jar")]
lib_path = [p for p in lib_path if os.path.exists(p)]

args = [JAVA]
for a in d["arguments"]["jvm"]:
    if isinstance(a, dict):
        if allowed(a):
            v = a["value"]
            args += v if isinstance(v, list) else [v]
    else:
        args.append(a)

if not use_instance:
    os.makedirs(os.path.join(GAME_DIR, "mods"), exist_ok=True)
    for name in MINIMAL_MODS:
        src = os.path.join(INST, "mods", name)
        dst = os.path.join(GAME_DIR, "mods", name)
        if not os.path.exists(src):
            sys.exit("前置模组缺失：" + src)
        if not os.path.exists(dst) or os.path.getsize(dst) != os.path.getsize(src):
            shutil.copy2(src, dst)
    print("探测实例:", GAME_DIR, "mods:", MINIMAL_MODS)

subs = {
    "${natives_directory}": NATIVES,
    "${launcher_name}": "probe",
    "${launcher_version}": "1",
    "${classpath}": os.pathsep.join(cp),
    "${classpath_separator}": os.pathsep,
    "${library_directory}": os.path.join(BASE, "libraries"),
    "${version_name}": "GregTech Leisure",
    "${path}": os.path.join(BASE, "assets", "log_configs", "client-1.12.xml"),
}
resolved = []
for a in args:
    for k, v in subs.items():
        a = a.replace(k, v)
    resolved.append(a)

resolved.append(d["mainClass"])

# arguments.game 里的 --launchTarget / --fml.* 不能省：ModLauncher 靠它决定走哪条启动路径。
# 认证相关的占位值随便填，离线探测用不上。
subs.update({
    "${auth_player_name}": "probe",
    "${auth_uuid}": "00000000000000000000000000000000",
    "${auth_access_token}": "0",
    "${clientid}": "0",
    "${xuid}": "0",
    "${user_type}": "legacy",
    "${version_type}": "probe",
    "${game_directory}": GAME_DIR,
    "${assets_root}": os.path.join(BASE, "assets"),
    "${assets_index_name}": d["assetIndex"]["id"],
    "${quickPlayPath}": "",
    "${quickPlaySingleplayer}": "",
    "${quickPlayMultiplayer}": "",
    "${quickPlayRealms}": "",
})
for a in d["arguments"]["game"]:
    if isinstance(a, dict):
        if allowed(a):
            v = a["value"]
            v = v if isinstance(v, list) else [v]
            for x in v:
                for k, s in subs.items():
                    x = x.replace(k, s)
                resolved.append(x)
    else:
        for k, s in subs.items():
            a = a.replace(k, s)
        resolved.append(a)
resolved += ["--width", "1280", "--height", "720"]

os.makedirs(os.path.join(GAME_DIR, "logs"), exist_ok=True)
log = open(os.path.join(GAME_DIR, "logs", "probe-launch.log"), "w",
           encoding="utf-8", errors="replace")
print("classpath entries:", len(cp))
p = subprocess.Popen(resolved, cwd=GAME_DIR, stdout=log, stderr=subprocess.STDOUT)
print("pid", p.pid)
