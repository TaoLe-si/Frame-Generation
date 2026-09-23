package com.taolesi.dlssmc.nativebridge;

import java.nio.file.Files;
import java.nio.file.Path;

/**
 * JNI 桥。对应 native/src/dlssmc_native.cpp 中导出的 7 个函数。
 *
 * 加载顺序很重要：必须先 System.load(sl.interposer.dll)，再 load(dlssmc_native.dll)。
 * 因为 dlssmc_native.dll 静态导入了 Streamline 的符号，而 Windows 只会按模块基名去
 * 已加载模块里解析，不会自动搜索 dll 所在目录。
 */
public final class DLSSNative {

    public static final String NATIVE_RESOURCE = "/dlssmc/native/dlssmc_native.dll";
    public static final String INTERPOSER_NAME = "sl.interposer.dll";

    private static boolean loaded = false;
    private static Throwable lastError = null;

    private DLSSNative() {}

    public static boolean isLoaded() {
        return loaded;
    }

    public static Throwable getLastError() {
        return lastError;
    }

    public static synchronized boolean load(Path streamlineBinDir, Path nativeCacheDir) {
        if (loaded) return true;
        try {
            Path interposer = streamlineBinDir.resolve(INTERPOSER_NAME);
            if (!Files.isRegularFile(interposer)) {
                throw new IllegalStateException("找不到 " + interposer
                        + "（Streamline 运行时目录配置有误，且随包自带的那份没解出来）");
            }

            Path dll = extractNative(nativeCacheDir);

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

    private static Path extractNative(Path cacheDir) throws Exception {
        Files.createDirectories(cacheDir);
        Path target = cacheDir.resolve("dlssmc_native.dll");
        try (var in = DLSSNative.class.getResourceAsStream(NATIVE_RESOURCE)) {
            if (in == null) {
                throw new IllegalStateException("classpath 里缺少 " + NATIVE_RESOURCE + "，先跑 native/build.sh");
            }
            Files.copy(in, target, java.nio.file.StandardCopyOption.REPLACE_EXISTING);
        }
        return target;
    }

    // ---------------------------------------------------------------- JNI

    public static native int nativeInit(String pluginPath, String logPath);
    public static native void nativeShutdown();
    public static native int nativeIsDlssSupported();

    /** 返回 [renderW, renderH, minW, minH, maxW, maxH, sharpness*1000] */
    public static native int[] nativeGetOptimalSettings(int displayW, int displayH, int mode);

    public static native int nativeSetup(
            int displayW, int displayH, int renderW, int renderH,
            int mode, float sharpness, boolean hdr, int preset,
            int colorGl, int auxGl, int outGl);

    public static native void nativeReleaseResources();

    /**
     * 输入段锁：排空 D3D11 后锁 worldTex + aux。锁内 GL 才能写这两张
     * （auxPass 渲染 + 世界颜色 blit），写完 glFinish 再调 {@link #nativeUnlockInputs} 交回。
     *
     * @return 0 成功，-1 未就绪，-2 锁失败
     */
    public static native int nativeLockInputs();

    /** @return 0 成功，-2 解锁失败 */
    public static native int nativeUnlockInputs();

    /**
     * GL 已把超分结果贴回主 target 后交还输出纹理。
     * {@link #nativeEvaluate} 成功返回时输出是锁着的，必须调这个解锁。
     *
     * @return 0 成功，-1 本来就没锁
     */
    public static native int nativeEvaluateDone();

    /**
     * @param matrices 列主序 4x4：[proj, view, prevProj, prevView]
     * @param params   [near, far, fov, aspect, jitterX, jitterY, mvecScaleX, mvecScaleY]
     */
    public static native int nativeEvaluate(float[] matrices, float[] params, boolean reset);
}
