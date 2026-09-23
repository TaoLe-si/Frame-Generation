"""跑 EmbeddiumPageCheck：把 SodiumConfigIntegration 的注册结果对着真实 Embeddium 选项 API 验一遍。

用生产 classpath（SRG 名 + 已经 reobf 过的 mod jar），和实机跑的是同一套字节码，
只是不起游戏、不碰玩家配置。编译与运行都在这个脚本里做，不依赖 Gradle。
"""
import glob
import json
import os
import subprocess
import sys

FORK = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASE = r"E:\MC\.minecraft"
INST = os.path.join(BASE, "versions", "GregTech Leisure")
JAVA_HOME = r"D:\Java17\jdk-17.0.18"
JAVAC = os.path.join(JAVA_HOME, "bin", "javac.exe")
JAVA = os.path.join(JAVA_HOME, "bin", "java.exe")

MOD_JAR = os.path.join(FORK, "build", "libs", "frame-generation-0.2.2.jar")
SRC = os.path.join(FORK, "native", "test", "EmbeddiumPageCheck.java")
OUT = os.path.join(FORK, "native", "test", "build", "java")

if not os.path.exists(MOD_JAR):
    sys.exit("先构建 mod：" + MOD_JAR + " 不存在")

profile = json.load(open(os.path.join(INST, "GregTech Leisure.json"), encoding="utf-8"))


def allowed(entry):
    rules = entry.get("rules")
    if not rules:
        return True
    result = False
    for r in rules:
        os_rule = r.get("os", {})
        if "name" in os_rule and os_rule["name"] != "windows":
            continue
        if "arch" in os_rule and os_rule["arch"] != "x86_64":
            continue
        result = r.get("action") == "allow"
    return result


cp = []
for lib in profile["libraries"]:
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

# 前置模组：Embeddium 提供选项 API，Oculus 只为了让 FGRuntime 能被类加载器验过
cp.insert(0, os.path.join(INST, "mods", "oculus-mc1.20.1-1.8.0.jar"))
cp.insert(0, os.path.join(INST, "mods", "embeddium-0.3.31+mc1.20.1.jar"))
# MC 本体（SRG 名）与 Forge 的配置 API 都不在启动 classpath 里，得自己加
cp.insert(0, os.path.join(BASE, "libraries", "net", "minecraft", "client",
                          "1.20.1-20230612.114412", "client-1.20.1-20230612.114412-srg.jar"))
cp.insert(0, os.path.join(BASE, "libraries", "net", "minecraftforge", "forge",
                          "1.20.1-47.4.16", "forge-1.20.1-47.4.16-universal.jar"))
# IConfigSpec 在 fmlcore 里，ForgeConfigSpec 实现了它
cp.insert(0, os.path.join(BASE, "libraries", "net", "minecraftforge", "fmlcore",
                          "1.20.1-47.4.16", "fmlcore-1.20.1-47.4.16.jar"))
cp.insert(0, MOD_JAR)

# nightconfig 是 Forge 的依赖，正常在 libraries 里；缺了就自己找一份
if not any("night-config" in p or "nightconfig" in p for p in cp):
    hits = glob.glob(os.path.join(BASE, "libraries", "com", "electronwill", "**", "*.jar"),
                     recursive=True)
    if not hits:
        sys.exit("找不到 nightconfig（ForgeConfigSpec 要用）")
    cp.extend(hits)

os.makedirs(OUT, exist_ok=True)
print("classpath:", len(cp), "项")

r = subprocess.run([JAVAC, "-encoding", "UTF-8", "-nowarn", "-d", OUT,
                    "-cp", os.pathsep.join(cp), SRC],
                   capture_output=True, text=True, encoding="utf-8", errors="replace")
if r.returncode != 0:
    print(r.stdout)
    print(r.stderr)
    sys.exit("编译失败")

r = subprocess.run([JAVA, "-cp", os.pathsep.join([OUT] + cp), "EmbeddiumPageCheck"],
                   capture_output=True, text=True, encoding="utf-8", errors="replace")
print(r.stdout)
if r.stderr.strip():
    print(r.stderr)
sys.exit(r.returncode)
