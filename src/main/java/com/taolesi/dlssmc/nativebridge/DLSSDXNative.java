package com.taolesi.dlssmc.nativebridge;

import java.nio.file.Files;
import java.nio.file.Path;

/**
 * D3D12 帧生成后端（FSR / XeSS）的 JNI 桥。对应 native/src/dlssmc_dx.cpp。
 *
 * 与 DLSSFGNative 分开是因为那条走 Vulkan，而 FSR/XeSS 只有 D3D12 前端；
 * 两边的输入交付方式不同，混在一个 DLL 里只会互相拖累。
 */
public final class DLSSDXNative {

    public static final String NATIVE_RESOURCE = "/dlssmc/native/dlssmc_dx.dll";

    /** 构建期打进 jar 的厂商二进制（相对 /dlssmc/native/），运行时解到缓存目录再按路径加载 */
    public static final String[] VENDOR_FILES = {
            "vendor/intel/bin/libxess_fg.dll",
            "vendor/intel/bin/libxell.dll",
            "vendor/intel/bin/libxess.dll",
            "vendor/amd/bin/amd_fidelityfx_loader_dx12.dll",
            "vendor/amd/bin/amd_fidelityfx_framegeneration_dx12.dll",
            "vendor/amd/bin/amd_fidelityfx_upscaler_dx12.dll",
    };

    /** 两家许可都要求随二进制一起再分发声明，缺了就是违反许可 */
    public static final String[] VENDOR_NOTICES = {
            "vendor/intel/LICENSE.txt",
            "vendor/intel/third-party-programs.txt",
            "vendor/amd/LICENSE.txt",
            "vendor/amd/license.md",
    };

    /** 呈现后端。FSR 的原生分支还没实现，nativeCreateDevice 会直接返回错误码。 */
    public static final int BACKEND_PASSTHROUGH = 0;
    public static final int BACKEND_XESS = 1;
    public static final int BACKEND_FSR = 2;

    /** 超分关闭（nativeConfigureSR 的 quality 传这个） */
    public static final int SR_OFF = -1;

    /**
     * 六条互操作链的下标，nativeLock 按段锁。分段是因为超分开着时各链的写入时机不同：
     * 深度/MV/世界颜色在世界阶段收尾就有内容，最终画面要等到呈现前，HUD-less 颜色则由原生层
     * 直接把超分结果拷进去、根本不经 GL。一次锁全部等于每帧多搬几张当帧还没内容的呈现分辨率纹理。
     */
    public static final int CHAIN_FINAL = 0;
    public static final int CHAIN_HUDLESS = 1;
    public static final int CHAIN_DEPTH = 2;
    public static final int CHAIN_MOTION = 3;
    public static final int CHAIN_WORLD = 4;
    public static final int CHAIN_SR_OUT = 5;

    /** XeSS-SR 档位，值就是 xess_quality_settings_t；档位只决定厂商建议的缩放区间 */
    public static final int XESS_ULTRA_PERFORMANCE = 100;
    public static final int XESS_PERFORMANCE = 101;
    public static final int XESS_BALANCED = 102;
    public static final int XESS_QUALITY = 103;
    public static final int XESS_ULTRA_QUALITY = 104;
    public static final int XESS_ULTRA_QUALITY_PLUS = 105;
    public static final int XESS_AA = 106;

    /** FSR 超分档位，值就是 FfxApiUpscaleQualityMode */
    public static final int FSR_NATIVE_AA = 0;
    public static final int FSR_QUALITY = 1;
    public static final int FSR_BALANCED = 2;
    public static final int FSR_PERFORMANCE = 3;
    public static final int FSR_ULTRA_PERFORMANCE = 4;

    private static boolean loaded = false;
    private static Throwable lastError = null;

    private DLSSDXNative() {}

    public static boolean isLoaded() {
        return loaded;
    }

    public static Throwable getLastError() {
        return lastError;
    }

    /** @param vendorBinDir 厂商根目录（下面按 intel/bin、amd/bin 分层），DLL 按绝对路径加载 */
    public static synchronized boolean load(Path vendorBinDir, Path nativeCacheDir) {
        if (loaded) return true;
        try {
            if (!Files.isDirectory(vendorBinDir)) {
                throw new IllegalStateException("找不到厂商目录 " + vendorBinDir);
            }
            Files.createDirectories(nativeCacheDir);
            Path dll = nativeCacheDir.resolve("dlssmc_dx.dll");
            try (var in = DLSSDXNative.class.getResourceAsStream(NATIVE_RESOURCE)) {
                if (in == null) {
                    throw new IllegalStateException("classpath 缺少 " + NATIVE_RESOURCE
                            + "，先跑 native/build_dx.sh");
                }
                Files.copy(in, dll, java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            }
            // D3D12 后端同样吃 MSVC 运行库（同样由 JDK 自带），只记录来源供诊断
            NativeRuntime.probeVcRuntime();
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

    /** 本进程已经解出来的 vendor 根；换后端会再走一次初始化，不能去覆盖自己 LoadLibrary 过的 DLL */
    private static Path extractedVendor;

    /**
     * 把内置的厂商 DLL 与许可声明解到缓存目录。
     *
     * @return nativeInit 需要的 vendor 根目录（下面分 intel/bin 与 amd/bin）
     */
    public static Path extractVendor(Path cacheDir) throws java.io.IOException {
        if (extractedVendor != null) return extractedVendor;
        Path root = cacheDir.resolve("vendor");
        for (String rel : java.util.stream.Stream.concat(
                java.util.Arrays.stream(VENDOR_FILES), java.util.Arrays.stream(VENDOR_NOTICES))
                .toArray(String[]::new)) {
            // rel 自带 vendor/ 前缀，跟 jar 里的资源路径一致，直接挂在缓存目录下
            Path out = cacheDir.resolve(rel);
            Files.createDirectories(out.getParent());
            try (var in = DLSSDXNative.class.getResourceAsStream("/dlssmc/native/" + rel)) {
                if (in == null) throw new java.io.IOException("jar 里缺少 " + rel);
                // 每次启动都覆盖：缓存留在 %TEMP% 里，模组更新后不能继续用旧版厂商 DLL
                Files.copy(in, out, java.nio.file.StandardCopyOption.REPLACE_EXISTING);
            }
        }
        extractedVendor = root;
        return root;
    }

    // ------------------------------------------------------------- JNI

    /** @return 0 成功，-1 WGL 互操作不可用，-2 XeSS DLL/导出缺失 */
    public static native int nativeInit(String vendorRoot, String logDir);

    /**
     * [XeLL 版本, 本机最大插帧数, FSR provider 版本, FSR 内置 DLL 版本, XeSS 内置 DLL 版本,
     * FSR 是否可加载, XeSS 超分, FSR 超分]；第 2 项在 nativeCreateDevice 之前恒为 0，
     * 第 8 项的 provider 版本同样要建完设备才查得到。
     */
    public static native String[] nativeGetCapabilities();

    /** @param backend BACKEND_* 之一；width/height 是呈现（窗口）分辨率 */
    public static native int nativeCreateDevice(long parentHwnd, int width, int height, int backend);

    /**
     * 建立 GL→D3D11(KMT)→跨 API(NT)→D3D12 的交付链。
     *
     * 前四路是呈现用的（final/hudless 跟呈现分辨率，depth/motion 跟渲染分辨率）；配了超分时还要
     * 交第五路世界颜色（渲染分辨率，超分的输入）和第六路输出纹理（呈现分辨率，方向反过来：
     * 原生写、GL 读，拿到 {@link #nativeUpscale} 的结果）。后两路必须成对，缺一路原生直接拒。
     *
     * @param renderWidth  渲染分辨率，也就是世界颜色/深度/MV 的尺寸；超分关着时等于呈现分辨率
     * @param renderHeight 同上
     */
    public static native int nativeBindTextures(int finalGl, int hudlessGl, int depthGl,
                                                int motionGl, int worldGl, int srOutGl,
                                                int renderWidth, int renderHeight);

    /**
     * 锁定互操作对象 [from, from+count)，之后 GL 才能写这几张输入纹理。
     * 注册过的对象只有锁内才能被 GL 用（锁外连 FBO 都挂不上），所以每段 GL 写入都要包在锁里。
     *
     * @param from  段起点，CHAIN_* 之一
     * @param count 段长度；[from, from+count) 必须都已绑定
     */
    public static native int nativeLock(int from, int count);

    /** GL 画完（要先 glFinish）后把当前锁住那一段搬进 D3D12 并解锁 */
    public static native int nativeDeliver();

    /** 注销纹理后才能删除或改变其 GL 存储；不销毁设备和厂商上下文。 */
    public static native int nativeUnbindTextures();

    /**
     * 先 nativeLock(2, 3)、GL 写入、glFinish、nativeDeliver，再执行超分。
     * 结果写入 hudless，并锁定 srOutGl 供 GL 读回。
     *
     * 返回时 srOutGl 是**锁着**的：wglDX 互操作规定注册过的对象只有锁内才能被 GL 用（锁外连 FBO
     * 都挂不上）。把它贴回主 target 之后必须调 {@link #nativeUpscaleDone}。
     *
     * @param params [near, far, fov, aspect, jitterX, jitterY, mvecScaleX, mvecScaleY]
     * @return 0 成功（srOutGl 已锁定）；-2/-3 互操作锁失败，-4 厂商超分执行失败，
     *         -6 没配超分或没绑输出，-7 还在 nativeLock 的锁内
     */
    public static native int nativeUpscale(float[] params, boolean reset);

    /** 交还 {@link #nativeUpscale} 锁住的那张输出纹理。@return 0 成功，-1 本来就没锁 */
    public static native int nativeUpscaleDone();

    /** 轻量预检：1 = D3D12 侧已经看到非黑的 GL 画面 */
    public static native int nativeProbe();

    /**
     * 交一帧给后端并呈现。
     *
     * @param matrices 列主序 4x4 x4：[proj, view, prevProj, prevView]
     * @param params   [near, far, fov, aspect, jitterX, jitterY, mvecScaleX, mvecScaleY]
     * @return 0 正常，负数出错
     */
    public static native int nativePresent(float[] matrices, float[] params, boolean reset);

    /** 上一次 Present 实际送呈了几帧（>1 说明插帧生效） */
    public static native int nativeGetPresentedCount();

    /**
     * 请求每张真实帧后面生成几张插帧；XeSS 会夹到本机上限并回返回生效值，
     * FSR 的张数由库自己决定，返回 -1 表示不支持调节。
     */
    public static native int nativeSetInterpolatedFrames(int extra);

    /**
     * FSR 调优：生成工作跑异步计算队列（allowAsyncWorkloads，AMD 官方示例默认开）
     * 与「只呈现生成帧」诊断开关（onlyPresentGenerated，对应 DLSS 的只显示插帧）。
     * 逐帧 configure 自动生效，这里只更新值。
     */
    public static native int nativeSetFsrTuning(boolean asyncWorkloads, boolean onlyPresentGenerated);

    /**
     * XeSS 场景变化检测阈值（xefgSwapChainSetSceneChangeThreshold），0~1，SDK 默认 0.7。
     * 越高越容易判定场景突变并暂停插帧（防糊防鬼影），越低越坚持插帧（流畅但突变画面可能糊一下）。
     */
    public static native int nativeSetSceneChangeThreshold(float threshold);

    /**
     * XeSS-SR 传统档位倍率（xessForceLegacyScaleFactors）。true=平衡档 1.7x/质量档 1.5x 等，
     * false=SDK 按 GPU 自选（本机有时更激进，如这版 SDK 平衡档直接给 50% 分辨率）。
     * 下一次 nativeConfigureSR（SR 上下文重建）时生效。
     */
    public static native int nativeSetXessLegacyScale(boolean enable);

    /**
     * XeLL 限帧（minimumIntervalUs，0=不限）。与 Reflex 限帧独立：Reflex 仅控 DLSS-G；
     * XeSS / FSR 走 XeLL，需调用自己的限帧接口。bLowLatencyMode 必须保持开启。
     */
    public static native int nativeSetXellFpsLimit(int fpsLimit);

    /**
     * 开关超分。输入尺寸由 nativeBindTextures 交进来的纹理决定，输出尺寸由 nativeCreateDevice
     * 决定，所以调用顺序是：建设备 → 配超分 → 按渲染分辨率建纹理并绑定。
     *
     * @param quality   {@link #SR_OFF} 关闭，否则是各家的档位（XESS_* / FSR_*）
     * @param sharpen   只有 FSR 有锐化（RCAS）；XeSS-SR 没有这个接口，传了也只会被记进日志
     * @param sharpness 0..1
     * @return 0 成功；-1 没有设备，-2 该后端/这台机器没有超分库，-3 上下文初始化失败，
     *         -4 档位不在厂商枚举里，-5 输出纹理建不出来
     */
    public static native int nativeConfigureSR(int quality, boolean sharpen, float sharpness);

    /**
     * 这个档位该按多大分辨率渲染 —— 厂商自己算的（XeSS 给建议输入分辨率，FSR 给该档位的渲染分辨率），
     * 不是我们列的倍率表。要在 {@link #nativeConfigureSR} 之后调（XeSS 的查询需要上下文）。
     *
     * @return [宽, 高]；查不到回 [0, 0]，调用方必须拒绝无效尺寸
     */
    public static native int[] nativeGetRenderSize(int quality);

    public static native void nativeShowPresentation();

    public static native void nativeHidePresentation();

    public static native void nativeShutdown();
}
