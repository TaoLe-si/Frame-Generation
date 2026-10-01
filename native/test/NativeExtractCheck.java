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

        // 5) 缺运行库的提示：把 SystemRoot 指到空目录（由 run 脚本用环境变量注入），
        //    提示必须出现且指明装什么；正常环境下必须为空串（不误报）
        var hintMethod = nativeClass.getMethod("missingVcRuntimeHint");
        String hint = (String) hintMethod.invoke(null);
        String fakeRoot = System.getenv("DLSSMC_FAKE_SYSTEMROOT");
        boolean expectMissing = fakeRoot != null && !fakeRoot.isEmpty();
        if (expectMissing) {
            check(!hint.isEmpty() && hint.contains("Visual C++"),
                    "SystemRoot 指向空目录时应提示装 VC++ 运行库 -> " + hint);
        } else {
            check(hint.isEmpty(), "本机运行库齐全时不应误报 -> " + (hint.isEmpty() ? "(空)" : hint));
        }

        System.out.println(fails == 0 ? "PASS: 解包 / 自愈 / 回落 / 加载 / 运行库提示 全部通过"
                : "FAIL: " + fails + " 项不通过");
        System.exit(fails == 0 ? 0 : 1);
    }
}
