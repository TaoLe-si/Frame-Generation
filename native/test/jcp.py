"""给探测脚本用的 Java classpath。

1.21.1 那份用的是 NeoGradle 生成的 `build/moddev/clientRunClasspath.txt`，
ForgeGradle 6 不生成这个文件。这里直接从整合包实例拼：libraries 里的 artifact +
版本目录里的 client jar + 编译期那两个光影模组，和实机加载的是同一批字节码。
"""
import glob
import json
import os

BASE = r"E:\MC\.minecraft"
INST = os.path.join(BASE, "versions", "GregTech Leisure")
JAVA_HOME = r"D:\Java17\jdk-17.0.18"
JAVAC = os.path.join(JAVA_HOME, "bin", "javac.exe")
JAVA = os.path.join(JAVA_HOME, "bin", "java.exe")

# 编译期依赖：Embeddium 给选项 API，Oculus 是 FGRuntime 的编译期依赖
PREREQ_MODS = ("embeddium-0.3.31+mc1.20.1.jar", "oculus-mc1.20.1-1.8.0.jar")


def _allowed(entry):
    """Mojang 的 rules 求值：默认 allow，命中的规则按顺序覆盖。"""
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


def _fml_args():
    """版本 JSON 的 arguments.game 里那串 --fml.* 值。"""
    profile = json.load(open(os.path.join(INST, "GregTech Leisure.json"), encoding="utf-8"))
    args = profile["arguments"]["game"]
    fml = {}
    for i, a in enumerate(args):
        if isinstance(a, str) and a.startswith("--fml."):
            fml[a[len("--fml."):]] = args[i + 1]
    return fml


def _srg_client_jar():
    """SRG 名（运行期）的 MC 本体。它不在版本 JSON 的 libraries 里 —— Forge 的
    MinecraftLocator 自己按 --fml.mcVersion / --fml.mcpVersion 去 libraries 下找。"""
    fml = _fml_args()
    path = os.path.join(BASE, "libraries", "net", "minecraft", "client",
                        f"{fml['mcVersion']}-{fml['mcpVersion']}",
                        f"client-{fml['mcVersion']}-{fml['mcpVersion']}-srg.jar")
    if not os.path.exists(path):
        raise FileNotFoundError("找不到 SRG 名 MC 本体：" + path)
    return path


def classpath(fork_root):
    """fork_root: 仓库根目录，用于把 build/resources/main 之类的产物挂上去。"""
    profile = json.load(open(os.path.join(INST, "GregTech Leisure.json"), encoding="utf-8"))
    entries = []
    # forge-client 是 Forge 打过补丁的 MC（SRG 名 + 补丁新增的方法如 enableStencil），
    # 放在 SRG 本体前面，让它盖住被改过的那些类。实机也是这么叠的。
    entries.append(os.path.join(BASE, "libraries", "net", "minecraftforge", "forge",
                                f"{_fml_args()['mcVersion']}-{_fml_args()['forgeVersion']}",
                                f"forge-{_fml_args()['mcVersion']}-{_fml_args()['forgeVersion']}-client.jar"))
    entries.append(_srg_client_jar())
    for lib in profile["libraries"]:
        if not _allowed(lib):
            continue
        dl = lib.get("downloads", {}).get("artifact")
        if dl and dl.get("path"):
            p = os.path.join(BASE, "libraries", dl["path"].replace("/", os.sep))
        else:
            g, art, ver = lib["name"].split(":")[:3]
            p = os.path.join(BASE, "libraries", *g.split("."), art, ver, f"{art}-{ver}.jar")
        if os.path.exists(p):
            entries.append(p)
    # 实例目录里那个 `GregTech Leisure.jar` 是原版混淆 jar（里面有个叫 `com` 的类），
    # 挂上它 javac 会把 `com` 当成类而不是包，所有 `com.mojang.*` / `com.taolesi.*`
    # 全部解析失败。Forge 实机也不用它：MC 本体由 MinecraftLocator 从 SRG jar 取。
    # Forge 本体同样不在版本 JSON 里：ModLauncher 靠 --fml.forgeVersion 自己拼路径。
    # forge-universal 给 ForgeConfigSpec / MinecraftForge，fmlcore 给 IConfigSpec。
    forge_ver = f"{_fml_args()['mcVersion']}-{_fml_args()['forgeVersion']}"
    for group, art in (("forge", "forge"), ("fmlcore", "fmlcore")):
        entries.append(os.path.join(BASE, "libraries", "net", "minecraftforge", group,
                                    forge_ver, f"{art}-{forge_ver}-universal.jar"
                                    if art == "forge" else f"{art}-{forge_ver}.jar"))
    # 被测的 mod 本体：用 reobf 过的 jar，和实机加载的是同一份字节码
    # 被测的 mod 本体按 glob 找最新产物 —— 写死版本号的话每次提升版本都要跟着改
    libs = os.path.join(fork_root, "build", "libs")
    jars = sorted(glob.glob(os.path.join(libs, "frame-generation-*.jar")),
                  key=os.path.getmtime, reverse=True)
    entries.append(jars[0] if jars else os.path.join(libs, "frame-generation.jar"))
    for name in PREREQ_MODS:
        entries.append(os.path.join(INST, "mods", name))
    # 生产加载路径是从 classpath 资源里解包 DLL，所以产物目录也得在
    entries.append(os.path.join(fork_root, "build", "resources", "main"))
    return os.pathsep.join(entries)
