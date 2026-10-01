// 原生库解包路径的判据程序（纯 JVM，不需要 MC）
//
// 要回答三件事：
//   1. 随包的 Streamline 运行时能不能正确解包出来（每个文件大小与 jar 内一致）
//   2. —— 也就是我改动过的那段：**半截文件能不能自愈**
//      旧逻辑是「文件存在且非空就跳过」，进程被中途杀掉留下半截 DLL 后，它会永久沿用，
//      表现为每次启动都 UnsatisfiedLinkError（重装 mod 也修不好，因为解包同样被跳过）。
//      新逻辑比对 jar 内的真实大小，不一致就重解。
//   3. 顺带打印解包目录与 dlssmc_fg.dll 的加载结果，以及失败时的可读信息。
//
// 编译运行：native/test/run_native_extract.py

import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Comparator;

public class NativeExtractCheck {

    private static int fails = 0;

    private static void check(boolean ok, String what) {
        System.out.println("  [" + (ok ? "ok" : "!!") + "] " + what);
        if (!ok) fails++;
    }

    private static long resourceSize(String res) throws Exception {
        var url = Class.forName("com.taolesi.dlssmc.nativebridge.DLSSFGNative").getResource(res);
        if (url == null) return -1;
        return url.openConnection().getContentLengthLong();
    }

    public static void main(String[] args) throws Exception {
        Path cache = Path.of(args.length > 0 ? args[0] : "build/native-extract-check");
        // 每次都从干净目录开始，才测得到真正的解包
        if (Files.exists(cache)) {
            try (var s = Files.walk(cache)) {
                s.sorted(Comparator.reverseOrder()).forEach(p -> {
                    try { Files.deleteIfExists(p); } catch (Exception ignored) { }
                });
            }
        }

        Class<?> nativeClass = Class.forName("com.taolesi.dlssmc.nativebridge.DLSSFGNative");
        var ensure = nativeClass.getMethod("ensureBundledRuntime", Path.class);
        var resolve = nativeClass.getMethod("resolveStreamlineDir", String.class, Path.class);

        System.out.println("缓存目录: " + cache.toAbsolutePath());
        long t0 = System.nanoTime();
        Path dir = (Path) ensure.invoke(null, cache);
        long ms = (System.nanoTime() - t0) / 1_000_000;
        check(dir != null, "ensureBundledRuntime 成功，用时 " + ms + " ms -> " + dir);
        if (dir == null) { System.out.println("解包失败，后续跳过"); System.exit(1); }

        // 1) 每个文件的大小必须与 jar 内一致
        String[] names = {"sl.interposer.dll", "sl.common.dll", "sl.dlss.dll", "sl.dlss_g.dll",
                "sl.reflex.dll", "sl.pcl.dll", "NvLowLatencyVk.dll", "nvngx_dlss.dll", "nvngx_dlssg.dll"};
        for (String n : names) {
            Path out = dir.resolve(n);
            long expect = resourceSize("/dlssmc/native/streamline_runtime/" + n);
            long got = Files.isRegularFile(out) ? Files.size(out) : -1;
            check(got == expect, n + " 解出 " + got + " 字节 / jar 内 " + expect);
        }

        // 2) 半截文件自愈：把其中一个截断，再调一次，必须重新解
        Path victim = dir.resolve("sl.reflex.dll");
        long good = Files.size(victim);
        try (var ch = java.nio.channels.FileChannel.open(victim,
                java.nio.file.StandardOpenOption.WRITE)) {
            ch.truncate(good / 2);
        }
        check(Files.size(victim) == good / 2, "已人为把 sl.reflex.dll 截成一半（"
                + Files.size(victim) + " 字节）");
        ensure.invoke(null, cache);
        long healed = Files.size(victim);
        check(healed == good, "再解一次后自愈：" + healed + " 字节（期望 " + good + "）");

        // 3) 没配 streamlinePath 时必须能落到随包那份
        Path resolved = (Path) resolve.invoke(null, "", cache);
        check(resolved != null && Files.isRegularFile(resolved.resolve("sl.interposer.dll")),
                "配置留空时 resolveStreamlineDir 回落到随包那份 -> " + resolved);

        // 4) 真正加载一次，失败也要给出可读原因
        var load = nativeClass.getMethod("load", Path.class, Path.class);
        var getErr = nativeClass.getMethod("getLastError");
        var isLoaded = nativeClass.getMethod("isLoaded");
        boolean ok = (Boolean) load.invoke(null, resolved, cache.resolve("mod"));
        check(ok, "DLSSFGNative.load -> " + ok);
        check((Boolean) isLoaded.invoke(null), "isLoaded -> " + isLoaded.invoke(null));
        Object err = getErr.invoke(null);
        System.out.println("  lastError = " + (err == null ? "null" : err.toString()));
        check(err == null, "加载成功时 lastError 应为 null");
        System.out.println("  dlssmc_fg.dll = " + cache.resolve("mod").resolve("dlssmc_fg.dll")
                + "（" + Files.size(cache.resolve("mod").resolve("dlssmc_fg.dll")) + " 字节）");

        // 5) MSVC 运行库来源判定。
        //    **不需要随包发** —— Java 17 起的 JDK 自带这三个文件；这里验的是「能认出来」。
        var nativeRuntime = Class.forName("com.taolesi.dlssmc.nativebridge.NativeRuntime");
        var hostSrc = nativeRuntime.getDeclaredMethod("hostVcRuntimeSource");
        hostSrc.setAccessible(true);
        var probeVc = nativeRuntime.getDeclaredMethod("probeVcRuntime");
        probeVc.setAccessible(true);
        var vcStatus = nativeRuntime.getDeclaredMethod("vcRuntimeStatus");
        vcStatus.setAccessible(true);

        String src = (String) hostSrc.invoke(null);
        String status = (String) probeVc.invoke(null);
        System.out.println("  宿主提供运行库的来源: " + (src.isEmpty() ? "(无)" : src)
                + "，状态=" + status);
        // 本机装了 VC++ redist 也有 JDK，两处都可能有；只要认出其一即可
        String fakeRoot = System.getenv("DLSSMC_FAKE_SYSTEMROOT");
        boolean fake = fakeRoot != null && !fakeRoot.isEmpty();
        if (fake) {
            // SystemRoot 被指到空目录 -> System32 那条路不通，仍必须靠 JDK 的 bin 认出来。
            // 这一条就是「装了 Java 就一定不用再装运行库」的判据。
            check(src.equals("JDK"),
                    "SystemRoot 无效时仍应识别到 JDK 自带运行库 -> " + (src.isEmpty() ? "(没认出来)" : src));
        } else {
            check(!src.isEmpty(), "本机应能认到运行库来源（System32 或 JDK bin）");
        }

        // 6) 提示只在「宿主确实没有」时出现。随包方案已撤（JDK 自带），
        //    所以只要 JDK 的 bin 在，就不该指引用户去装 redist。
        var hintMethod = nativeClass.getMethod("missingVcRuntimeHint");
        String hint = (String) hintMethod.invoke(null);
        boolean hostHas = !((String) hostSrc.invoke(null)).isEmpty();
        if (hostHas) {
            check(hint.isEmpty(), "宿主已提供运行库时不应提示装 redist -> "
                    + (hint.isEmpty() ? "(空)" : hint));
        } else {
            check(!hint.isEmpty() && hint.contains("Visual C++"),
                    "宿主没有运行库时应提示装 redist -> " + hint);
        }

        System.out.println(fails == 0 ? "PASS: 解包 / 自愈 / 回落 / 加载 / 运行库提示 全部通过"
                : "FAIL: " + fails + " 项不通过");
        System.exit(fails == 0 ? 0 : 1);
    }
}
