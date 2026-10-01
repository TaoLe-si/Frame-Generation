package com.taolesi.dlssmc.nativebridge;

import java.nio.file.Files;
import java.nio.file.Path;

/**
 * DLSS 帧生成（DLSS-G）的 JNI 桥。对应 native/src/dlssmc_fg.cpp。
 *
 * 加载顺序：必须先 System.load(sl.interposer.dll)，dlssmc_fg.dll 静态导入它的符号。
 */
public final class DLSSFGNative {

    public static final String NATIVE_RESOURCE = "/dlssmc/native/dlssmc_fg.dll";
    public static final String RUNTIME_DIR = "streamline_runtime";

    /** 随 mod 打包的 Streamline 运行时，用户没指定 streamlinePath 时用这一份 */
    private static final String[] RUNTIME_DLLS = {
            "sl.interposer.dll", "sl.common.dll", "sl.dlss.dll",
            "sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll",
            // Reflex 在 Vulkan 上的低延迟实现，DLSS-G 要求 Reflex 在跑
            "NvLowLatencyVk.dll",
            // NGX 模型本体。Streamline 的插件目录里没有它们时 DLSS-G/DLSS 会直接报不支持，
            // 不能指望驱动里的那份（DriverStore 路径随驱动版本变）。
            "nvngx_dlss.dll", "nvngx_dlssg.dll"};

    private static boolean loaded = false;
    private static Throwable lastError = null;

    private DLSSFGNative() {}

    public static boolean isLoaded() {
        return loaded;
    }

    public static Throwable getLastError() {
        return lastError;
    }

    /**
     * 把打包进来的 Streamline 运行时解包到 {@code nativeCacheDir/streamline_runtime}。
     * 配置里 streamlinePath 留空时走这里，用户不必自己准备 SDK。
     *
     * @return 解包出的目录；资源缺失时返回 null（调用方回落到用户配置的路径）
     */
    public static Path ensureBundledRuntime(Path nativeCacheDir) {
        try {
            Path dir = nativeCacheDir.resolve(RUNTIME_DIR);
            Files.createDirectories(dir);
            for (String name : RUNTIME_DLLS) {
                Path out = dir.resolve(name);
                String res = "/dlssmc/native/" + RUNTIME_DIR + "/" + name;
                long expected = resourceSize(res);
                // 「存在且非空」就跳过是不够的：进程被中途杀掉（或磁盘写满）会留下半截 DLL，
                // 而半截文件同样非空 —— 于是它会被永久沿用，表现为每次启动都
                // UnsatisfiedLinkError，用户重装 mod 也修不好。改成比对 jar 内的真实大小。
                if (Files.isRegularFile(out) && expected > 0 && Files.size(out) == expected) continue;
                try (var in = DLSSFGNative.class.getResourceAsStream(res)) {
                    if (in == null) return null;
                    // 先写临时文件再原子改名：中途失败不会留下可用的半截文件
                    Path tmp = dir.resolve(name + ".part");
                    Files.copy(in, tmp, java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                    if (expected > 0 && Files.size(tmp) != expected) {
                        Files.deleteIfExists(tmp);
                        throw new IllegalStateException(
                                name + " 解包不完整：期望 " + expected + " 字节，实得 "
                                        + Files.size(tmp) + " 字节（磁盘空间或杀软拦截？）");
                    }
                    moveIntoPlace(tmp, out);
                }
            }
            return dir;
        } catch (Throwable t) {
            lastError = t;
            return null;
        }
    }

    /**
     * 决定用哪个 Streamline 运行时目录：配置里给的就用配置的，没给（或给的目录里没有
     * sl.interposer.dll）就用随包解出来的那份。
     *
     * @return 可用目录；连自带的那份都解不出来时返回 null
     */
    public static Path resolveStreamlineDir(String configured, Path nativeCacheDir) {
        if (configured != null && !configured.isBlank()) {
            Path dir = Path.of(configured);
            if (Files.isRegularFile(dir.resolve("sl.interposer.dll"))) return dir;
        }
        return ensureBundledRuntime(nativeCacheDir);
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

    /**
     * 本机有没有 MSVC 运行库。返回空串表示看起来是齐的，否则返回一句可直接照做的提示。
     *
     * <p>为什么值得单独判：{@code dlssmc_fg.dll} 与随包的全部 9 个 Streamline DLL 都静态导入
     * {@code MSVCP140 / VCRUNTIME140 / VCRUNTIME140_1}。开发机上装了 MSVC 所以永远有，
     * 换一台没装运行库的机器就会以 {@code UnsatisfiedLinkError} 的形式炸掉，
     * 而那条消息在叠加层里通常被屏幕边缘截断 —— 用户只看到「加载原生库失败」，无从下手。
     *
     * <p>只作为**失败后的解释**，不前置拦截：CRT 也可能是别的应用 app-local 部署的，
     * 那种情况下 System32 里没有这几个文件但加载照样成功。
     */
    public static String missingVcRuntimeHint() {
        String root = System.getenv("SystemRoot");
        if (root == null || root.isEmpty()) return "";
        for (String name : new String[]{"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"}) {
            if (!Files.isRegularFile(Path.of(root, "System32", name))) {
                return "｜本机缺 MSVC 运行库（" + name + " 不在 System32）：装 Microsoft Visual C++ "
                        + "2015-2022 Redistributable (x64) 后重进游戏。随包的 Streamline 与 dlssmc 本体都依赖它。";
            }
        }
        return "";
    }

    /** classpath 资源的解压后大小；拿不到（不在 jar 里等）时返回 -1 */
    private static long resourceSize(String resource) {
        try {
            var url = DLSSFGNative.class.getResource(resource);
            if (url == null) return -1;
            return url.openConnection().getContentLengthLong();
        } catch (Throwable t) {
            return -1;
        }
    }

    public static synchronized boolean load(Path streamlineBinDir, Path nativeCacheDir) {
        if (loaded) return true;
        try {
            Path interposer = streamlineBinDir.resolve("sl.interposer.dll");
            if (!Files.isRegularFile(interposer)) {
                throw new IllegalStateException("找不到 " + interposer
                        + "（Streamline 运行时目录配置有误，且随包自带的那份没解出来）");
            }
            Files.createDirectories(nativeCacheDir);
            Path dll = nativeCacheDir.resolve("dlssmc_fg.dll");
            long expected = resourceSize(NATIVE_RESOURCE);
            try (var in = DLSSFGNative.class.getResourceAsStream(NATIVE_RESOURCE)) {
                if (in == null) {
                    throw new IllegalStateException("classpath 缺少 " + NATIVE_RESOURCE + "，先跑 native/build_fg.sh");
                }
                Path tmp = nativeCacheDir.resolve("dlssmc_fg.dll.part");
                Files.copy(in, tmp, java.nio.file.StandardCopyOption.REPLACE_EXISTING);
                if (expected > 0 && Files.size(tmp) != expected) {
                    Files.deleteIfExists(tmp);
                    throw new IllegalStateException("dlssmc_fg.dll 解包不完整：期望 " + expected
                            + " 字节，实得 " + Files.size(tmp) + " 字节");
                }
                moveIntoPlace(tmp, dll);
            }

            System.load(interposer.toAbsolutePath().toString());
            System.load(dll.toAbsolutePath().toString());
            loaded = true;
            lastError = null;
            return true;
        } catch (Throwable t) {
            lastError = t;
            loaded = false;
            return false;
        }
    }

    // ------------------------------------------------------------- JNI

    /** @param mfgUnlock 实验：slInit 前改掉 nvngx_dlssg.dll 里的多帧架构门禁（只改内存映像）
     *  @return 0 成功 */
    public static native int nativeInit(String pluginPath, String logPath, boolean mfgUnlock);

    /** MFG 解锁结果说明（未启用 / 已改 N 处 / 没找到门禁） */
    public static native String nativeGetMfgUnlockReport();

    /** @return 1 支持 0 不支持（需要先 nativeCreateDevice） */
    public static native int nativeIsSupported();

    /** @return 0 成功 */
    public static native int nativeCreateDevice(long hwnd, int width, int height, boolean reflexBoost,
                                                int reflexFpsLimit);

    public static native int nativeBindTextures(int finalGl, int hudlessGl, int depthGl, int motionGl,
                                                int worldGl, int outGl, int renderWidth, int renderHeight);

    public static native int nativeConfigureSR(int quality, float sharpness, int preset);

    public static native int[] nativeGetRenderSize(int quality);

    /** 与帧生成共用本帧 token；成功返回时输出纹理已锁定供 GL 读取。 */
    public static native int nativeUpscale(float[] matrices, float[] params, boolean reset);

    public static native int nativeUpscaleDone();

    /** 只解绑输入纹理，保留 Streamline 与 Vulkan 设备；换到 FSR / XeSS 时用 */
    public static native int nativeUnbindTextures();

    public static native int nativeBeginFrame(boolean reflexBoost, int reflexFpsLimit);

    public static native void nativeRenderStart();

    /**
     * @param framesToGenerate 1=2x 2=3x 3=4x；@return 生效张数（本机上限低于请求时会夹小），负数出错
     * @param forceMultiFrame  实验：跳过本地夹值，把请求原样交给 SDK（用于探测 NGX 是否接受多帧）
     */
    public static native int nativeSetMode(int framesToGenerate, boolean forceMultiFrame);

    /**
     * 每帧上屏。
     *
     * @param matrices 列主序 4x4 x4：[proj, view, prevProj, prevView]
     * @param params   [near, far, fov, aspect, jitterX, jitterY, mvecScaleX, mvecScaleY]
     * @return 0 正常，1 需要重建 swapchain，负数出错
     */
    public static native int nativePresent(float[] matrices, float[] params, boolean reset, int statusLogInterval);

    /** 上一次 present 实际呈现了几帧（>1 说明插帧生效） */
    public static native int nativeGetPresentedCount();

    /** [DLSS-G 模型, DLSS-SR 模型, Streamline SDK]，来自插件目录里那份 DLL 的文件版本 */
    public static native String[] nativeGetModelVersions();

    /** 下发 DLSS-G 底层开关：只显示插帧 / 关闭时保留资源 / 全屏菜单检测 / 队列并行（仅 Vulkan 生效） / UI 重合成 / 工作模式（0 固定 1 自动 2 动态） / 动态模式目标帧率（0=刷新率） */
    public static native void nativeSetTuning(boolean showOnlyInterpolated, boolean retainResourcesWhenOff,
                                              boolean fullscreenMenuDetection, boolean queueParallelism,
                                              boolean uiRecomposition, int mode, float dynamicTargetFps);

    /** [模拟→Present, 模拟, 渲染提交, 驱动队列, GPU 渲染, Present, 是否有效]，毫秒，非屏幕延迟；无数据时首项为 -1 */
    public static native double[] nativeGetLatency();

    /** 桥接路径自身分段耗时 [等上一轮帧, acquire, 提交, 呈现, 取状态, 锁-提交fence, 锁-输入fence, 锁-DX]，毫秒 */
    public static native double[] nativeGetBridgeTimings();

    /**
     * 锁定 / 解锁互操作对象。
     *
     * 实测结论：GL 只有在对象锁定期间写入的内容，D3D11/Vulkan 才看得到；
     * 锁之前画的读回来全是 0。所以所有写进这三张纹理的 GL 操作
     * 都必须夹在 nativeLock() / nativeUnlock() 之间。
     */
    public static native int nativeLock();

    public static native int nativeUnlock();

    /** 轻量预检：1 = 画面已成功经桥传来（可以接管上屏），0 = 还是黑的（绝不能接管） */
    public static native int nativeProbe();

    /** 总开关：0 关闭（present 直接让回 GL），1 开启 */
    public static native int nativeSetEnabled(int on);

    public static native int nativeResize(int width, int height);

    public static native void nativeHidePresentation();

    public static native void nativeShutdown();
}
