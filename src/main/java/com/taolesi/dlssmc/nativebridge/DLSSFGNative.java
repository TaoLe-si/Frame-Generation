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
                // 已解包且非空就跳过：换世界/换后端会重复调，不必每次重写 3MB
                if (Files.isRegularFile(out) && Files.size(out) > 0) continue;
                try (var in = DLSSFGNative.class.getResourceAsStream(
                        "/dlssmc/native/" + RUNTIME_DIR + "/" + name)) {
                    if (in == null) return null;
                    Files.copy(in, out, java.nio.file.StandardCopyOption.REPLACE_EXISTING);
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
            try (var in = DLSSFGNative.class.getResourceAsStream(NATIVE_RESOURCE)) {
                if (in == null) {
                    throw new IllegalStateException("classpath 缺少 " + NATIVE_RESOURCE + "，先跑 native/build_fg.sh");
                }
                Files.copy(in, dll, java.nio.file.StandardCopyOption.REPLACE_EXISTING);
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
