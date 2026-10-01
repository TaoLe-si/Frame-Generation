package com.taolesi.dlssmc.nativebridge;

import java.nio.file.Files;
import java.nio.file.Path;

/**
 * 原生库的公共准备工作：解包 + MSVC 运行库。
 *
 * <p>三个后端（DLSS / D3D12 / 旧超分）都是 `/MD` 编译，都静态导入
 * {@code MSVCP140 / VCRUNTIME140 / VCRUNTIME140_1}；随包的 9 个 Streamline DLL 也一样。
 * 开发机装了 MSVC 所以永远有，换一台没装的机器 {@code System.load} 会以
 * {@code UnsatisfiedLinkError} 失败 —— 因此把这三个文件随包发一份（app-local 部署，
 * 微软许可允许），系统没有时自己加载，用户不必装 vc_redist。
 *
 * <p>加载顺序有讲究：Windows 解析导入时按 **base name** 找已加载模块，
 * 所以先把运行库 load 进来，后续 DLL 的导入才能绑上。
 * 这与 {@code sl.interposer.dll} 必须先于 {@code dlssmc_fg.dll} 加载是同一个机制。
 */
final class NativeRuntime {

    /** 只需要这三个：它们的非系统依赖只有 KERNEL32 与 api-ms-win-crt-*（UCRT 属系统组件），
     *  彼此构成闭集。concrt140 / msvcp140_1 / msvcp140_2 / vccorlib140 都不需要。 */
    private static final String[] VC_RUNTIME_DLLS = {
            "vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"};

    /** 未探测 / 系统自带 / 随包 / 随包缺失 / 随包加载失败: ... */
    private static String vcStatus = "未探测";

    private NativeRuntime() {}

    static String vcRuntimeStatus() {
        return vcStatus;
    }

    /**
     * 宿主进程是不是已经提供了运行库 —— 只要进程里已经有同 base name 的模块，
     * 我们后来加载的 DLL 就会绑到它，随包那份根本用不上。
     *
     * <p>两处都算：
     * <ul>
     *   <li>System32：装了 VC++ redist 的机器</li>
     *   <li><b>{@code <java.home>/bin}</b>：**这才是常见情况** —— Temurin 等 JDK 发行版
     *       把这三个文件 app-local 放在自己的 bin 里，java.exe 启动时就已从那儿加载。
     *       所以「用户没装 redist」通常并不影响游戏，不能据此乱指方向。</li>
     * </ul>
     *
     * @return "系统" / "JDK" / ""（都没有）
     */
    static String hostVcRuntimeSource() {
        String root = System.getenv("SystemRoot");
        if (root != null && !root.isEmpty() && hasAll(Path.of(root, "System32"))) return "系统";
        String javaHome = System.getProperty("java.home");
        if (javaHome != null && !javaHome.isEmpty() && hasAll(Path.of(javaHome, "bin"))) return "JDK";
        return "";
    }

    private static boolean hasAll(Path dir) {
        for (String name : VC_RUNTIME_DLLS) {
            if (!Files.isRegularFile(dir.resolve(name))) return false;
        }
        return true;
    }

    /**
     * 在加载任何厂商 DLL 之前调用一次：查一下宿主进程是不是已经提供了 MSVC 运行库。
     *
     * <p>**不需要随包发运行库**：实测 Java 17 起的 JDK（Adoptium / Microsoft / Oracle 21 都试过）
     * 都把 {@code msvcp140 / vcruntime140 / vcruntime140_1} app-local 放在自己的 bin 下，
     * java.exe 启动时就已经加载了，我们后来加载的 DLL 按 base name 就能绑上。
     * —— 只有 2018 年的 Oracle 11 那类老 JVM 才没有，而 MC 1.20.1+ 跑不到 Java 11。
     *
     * <p>这里只做判定，用来在**加载仍然失败**时给出准确解释，不加载任何东西。
     */
    static String probeVcRuntime() {
        String src = hostVcRuntimeSource();
        vcStatus = src.isEmpty() ? "宿主未提供" : src + "自带";
        return vcStatus;
    }

    /**
     * 加载失败后的补充说明。只有**系统没有、随包那份也没能顶上**时才指向装运行库 ——
     * 随包那份正常工作时，后来的失败与运行库无关，乱指方向比不说更糟。
     */
    static String vcRuntimeHint() {
        if (!hostVcRuntimeSource().isEmpty()) return "";
        // 宿主既没有（System32 没有 = 没装 redist；JDK bin 也没有 = 非主流 JVM 发行版）。
        // 这时才值得让用户去装一次 redist —— 而且这确实能修好。
        return "｜本机既没装 VC++ 运行库、当前 JVM 目录里也没有（" + vcStatus
                + "）：装 Microsoft Visual C++ 2015-2022 Redistributable (x64) 后重进游戏";
    }

    /**
     * 把 classpath 上某个目录下的若干 DLL 解包出来。
     *
     * <p>「存在且非空」就跳过是不够的：进程被中途杀掉（或磁盘写满、杀软拦截）会留下
     * **半截 DLL，而半截文件同样非空** —— 于是它被永久沿用，表现为每次启动都
     * UnsatisfiedLinkError，用户重装 mod 也修不好（解包照样被跳过）。
     * 所以比对 jar 内的真实大小，不一致就重解；先写 .part 再原子改名，写完校验大小。
     *
     * @return 解包目录；资源缺失时返回 null
     */
    static Path extract(Path nativeCacheDir, String subDir, String resourceDir, String[] names) {
        try {
            Path dir = (subDir == null || subDir.isEmpty()) ? nativeCacheDir
                                                            : nativeCacheDir.resolve(subDir);
            Files.createDirectories(dir);
            for (String name : names) {
                Path out = dir.resolve(name);
                String res = resourceDir + "/" + name;
                long expected = resourceSize(res);
                if (Files.isRegularFile(out) && expected > 0 && Files.size(out) == expected) continue;
                try (var in = NativeRuntime.class.getResourceAsStream(res)) {
                    if (in == null) return null;
                    Path tmp = dir.resolve(name + ".part");
                    Files.copy(in, tmp, java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                    long got = Files.size(tmp);
                    if (expected > 0 && got != expected) {
                        Files.deleteIfExists(tmp);
                        throw new IllegalStateException(name + " 解包不完整：期望 " + expected
                                + " 字节，实得 " + got + " 字节（磁盘空间或杀软拦截？）");
                    }
                    moveIntoPlace(tmp, out);
                }
            }
            return dir;
        } catch (Throwable t) {
            return null;
        }
    }

    /** 原子改名；文件系统不支持时退回普通改名（网络盘 / exFAT 上 ATOMIC_MOVE 会抛） */
    private static void moveIntoPlace(Path tmp, Path out) throws java.io.IOException {
        try {
            Files.move(tmp, out, java.nio.file.StandardCopyOption.REPLACE_EXISTING,
                    java.nio.file.StandardCopyOption.ATOMIC_MOVE);
        } catch (java.nio.file.AtomicMoveNotSupportedException e) {
            Files.move(tmp, out, java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        }
    }

    /** classpath 资源的解压后大小；拿不到（不在 jar 里等）时返回 -1 */
    static long resourceSize(String resource) {
        try {
            var url = NativeRuntime.class.getResource(resource);
            if (url == null) return -1;
            return url.openConnection().getContentLengthLong();
        } catch (Throwable t) {
            return -1;
        }
    }
}
