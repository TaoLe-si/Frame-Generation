package com.taolesi.dlssmc.core;

import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.platform.GlStateManager;
import com.mojang.blaze3d.platform.TextureUtil;
import com.taolesi.dlssmc.DLSSMC;
import com.taolesi.dlssmc.config.DLSSConfig;
import com.taolesi.dlssmc.mixin.IrisPipelineAccessor;
import com.taolesi.dlssmc.nativebridge.DLSSDXNative;
import com.taolesi.dlssmc.nativebridge.DLSSFGNative;
import com.taolesi.dlssmc.render.FGDepthMotionPass;
import net.irisshaders.iris.Iris;
import net.minecraft.client.Camera;
import net.minecraft.client.Minecraft;
import org.joml.Matrix4f;
import org.lwjgl.glfw.GLFWNativeWin32;
import org.lwjgl.opengl.GL11C;
import org.lwjgl.opengl.GL15C;
import org.lwjgl.opengl.GL30C;
import org.lwjgl.opengl.GL33C;
import org.lwjgl.opengl.GL45C;

import java.nio.file.Path;

/** 世界缩放、SR 回写与三后端帧生成调度。 */
public final class FGRuntime {

    private static final FGRuntime INSTANCE = new FGRuntime();

    private static final org.apache.logging.log4j.Logger LOG =
            org.apache.logging.log4j.LogManager.getLogger("dlssmc");

    public static FGRuntime get() {
        return INSTANCE;
    }

    private boolean initAttempted = false;
    private boolean nativeReady = false;
    private boolean deviceReady = false;
    private boolean worldActive;
    private boolean frameCaptured;
    private boolean pendingDisable;

    // 当前后端与其能力表（DLSS 走 Streamline/Vulkan，FSR 与 XeSS 走 D3D12）
    private DLSSConfig.Backend backend;
    private String[] dxCaps;
    private java.nio.file.Path vendorRoot;

    // 熔断：初始化失败后不要每帧重试（那会把整台 GPU 设备反复重建，卡到没法玩）
    private int initFailures = 0;
    private long nextRetryAt = 0L;
    private static final int MAX_INIT_FAILURES = 3;
    private static final long RETRY_COOLDOWN_MS = 5000L;
    private static final int MAX_PRESENT_FAILURES = 5;
    /** 预检帧数上限：桥一直不通就别再每帧锁纹理+glFinish 白付桥接开销（卡顿来源），等重绑再试 */
    private static final int MAX_PROBE_FRAMES = 600;
    private int presentFailures = 0;
    private boolean takeoverOff = false;
    private boolean armed = false;
    private int probeCounter = 0;


    // GL 资源
    private int hudlessTex = -1;
    private int finalTex = -1;
    private int copyReadFbo;
    private int copyDrawFbo;
    private int configuredFrames = 0;
    private int texW = 0, texH = 0;
    private FGDepthMotionPass depthMv;
    private com.mojang.blaze3d.pipeline.MainTarget worldTarget;
    private boolean inWorldPhase;
    private int worldTex = -1, srOutTex = -1;
    private int renderW, renderH;
    private int configuredSr = -2;
    private int configuredPreset = -1;
    private boolean nativeFrameStarted;
    private boolean configuredSharpen;
    private float configuredSharpness;
    private double srElapsedMs;
    private net.irisshaders.iris.pathways.colorspace.ColorSpaceConverter sizedColorConverter;
    private net.irisshaders.iris.pathways.colorspace.ColorSpace sizedColorSpace;

    // 相机矩阵
    private final Matrix4f proj = new Matrix4f();
    private final Matrix4f modelViewRot = new Matrix4f();
    private final Matrix4f prevProj = new Matrix4f();
    private final Matrix4f prevView = new Matrix4f();
    private boolean hasPrev = false;

    private final float[] matrices = new float[64];
    private final float[] params = new float[8];

    private String status = "未初始化";
    private String loggedStatus = "";
    private String glDiag = "";
    private String[] modelVersions;
    private String mfgUnlockReport;
    private int lastPresented = 0;
    private int lastResult = 0;
    private long presentedFrames = 0;
    private final FpsSampler fps = new FpsSampler();
    private int tunedFlags = -1;
    private int tunedFps = -1;
    private double[] latency;
    private double[] bridge;
    private long latencyTick;
    // DX 侧调优的已下发值：变了才推原生，避免每帧 JNI
    private int fsrTuningPushed = -1;
    private float xessThresholdPushed = -1f;
    private int xellFpsPushed = -1;
    private boolean xessLegacyPushed;
    // GPU 分段计时：0=深度+MV 1=hudless拷贝 2=final拷贝
    private final int[] gpuQueryIds = new int[6];
    private final double[] gpuMs = new double[3];
    private final boolean[] gpuPending = new boolean[3];

    /** 非阻塞取回已完成的 GPU 计时；没回来的沿用上一次数值 */
    private void gpuTimingCollect() {
        if (gpuQueryIds[0] == 0) GL15C.glGenQueries(gpuQueryIds);
        for (int i = 0; i < 3; ++i) {
            if (!gpuPending[i]) continue;
            int id = gpuQueryIds[i * 2];
            if (GL15C.glGetQueryObjectui(id, GL15C.GL_QUERY_RESULT_AVAILABLE) == 0) continue;
            gpuMs[i] = GL33C.glGetQueryObjectui64(id, GL33C.GL_QUERY_RESULT) / 1_000_000.0;
            gpuPending[i] = false;
        }
    }

    private boolean gpuTimingBegin(int stage) {
        if (!DLSSConfig.DEBUG_OVERLAY.get() || gpuPending[stage] || gpuQueryIds[0] == 0) return false;
        GL15C.glBeginQuery(GL33C.GL_TIME_ELAPSED, gpuQueryIds[stage * 2]);
        return true;
    }

    private void gpuTimingEnd(int stage, boolean started) {
        if (!started) return;
        GL15C.glEndQuery(GL33C.GL_TIME_ELAPSED);
        gpuPending[stage] = true;
    }

    private FGRuntime() {}

    private static final class WorldTarget extends com.mojang.blaze3d.pipeline.MainTarget
            implements net.irisshaders.iris.targets.Blaze3dRenderTargetExt {
        // Iris 的普通 target 版本从零递增；负数区分每次替换的世界 target。
        private static int nextVersion;
        private final int attachmentVersion = --nextVersion;

        private WorldTarget(int width, int height, boolean stencil) {
            super(width, height);
            // Iris 换深度附件时不会移除旧 stencil，必须保持主目标的附件类型。
            if (stencil) enableStencil();
        }

        @Override
        public void bindWrite(boolean setViewport) {
            super.bindWrite(true);
        }

        @Override
        public int iris$getDepthBufferVersion() {
            return attachmentVersion;
        }

        @Override
        public int iris$getColorBufferVersion() {
            return attachmentVersion;
        }
    }

    // ------------------------------------------------------------------ 状态

    public boolean isEnabled() {
        return DLSSConfig.activeBackend() != null;
    }

    private boolean usesDx() {
        return backend == DLSSConfig.Backend.FSR || backend == DLSSConfig.Backend.XESS;
    }

    private int dxBackendIndex() {
        return backend == DLSSConfig.Backend.FSR ? DLSSDXNative.BACKEND_FSR
                : DLSSDXNative.BACKEND_XESS;
    }

    public boolean isReady() {
        return nativeReady && deviceReady;
    }

    public String getStatus() {
        return status;
    }

    public String getSrText() {
        if (configuredSr < 0 || srElapsedMs == 0) return "SR：" + status;
        return backend.label + " SR：" + DLSSConfig.activeSrLabel() + " "
                + renderW + "x" + renderH + "→" + texW + "x" + texH
                + String.format(" · SR+回写 %.2f ms（墙钟）", srElapsedMs);
    }

    /** 状态行只在变化时写进 latest.log：实机出问题时得能直接读出原因，不用靠猜 */
    private void logStatusChange() {
        if (status.equals(loggedStatus)) return;
        loggedStatus = status;
        LOG.info("[FG] {}", status);
    }

    public int getLastPresented() {
        return lastPresented;
    }

    public long getPresentedFrames() {
        return presentedFrames;
    }

    public double getRenderFps() {
        return fps.renderFps();
    }

    public double getPresentationFps() {
        return fps.presentationFps();
    }

    public boolean hasFpsSample() {
        return fps.isAvailable();
    }

    public String getFpsText() {
        if (!fps.isAvailable()) return "真实渲染 / 呈现 FPS：采样中…";
        if (fps.isTakeover() && backend == DLSSConfig.Backend.FSR)
            return "真实渲染 " + Math.round(fps.renderFps()) + " FPS / FSR 提交估算 "
                    + Math.round(fps.presentationFps()) + " FPS（非实测呈现）";
        return "真实渲染 " + Math.round(fps.renderFps()) + " FPS / 呈现 " + Math.round(fps.presentationFps())
                + (fps.isTakeover() ? " FPS（SDK计数估算）" : " FPS（GL）");
    }

    /** Reflex 回报的实测延迟分段；驱动没上报时返回 null */
    public String getLatencyText() {
        double[] l = latency;
        if (backend != DLSSConfig.Backend.DLSS || DLSSConfig.activeFramesToGenerate() == 0
                || l == null || l[6] < 0.5) return null;
        return String.format("Reflex 模拟→Present %.1f ms（非屏幕延迟；模拟 %.1f·提交 %.1f·队列 %.1f·GPU %.1f·Present %.1f）",
                l[0], l[1], l[2], l[3], l[4], l[5]);
    }

    /** 我们这条 GL→D3D11→Vulkan 桥每帧自己花掉的时间 */
    public String getBridgeText() {
        double[] b = bridge;
        if (b == null) return null;
        return String.format("桥接 ms：等帧 %.2f 取图 %.2f 提交 %.2f 呈现 %.2f 取状态 %.2f｜锁 %.2f+%.2f+%.2f",
                b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
    }

    public String getGpuStageText() {
        if (gpuQueryIds[0] == 0) return null;
        return String.format("GPU ms：深度+MV %.2f hudless拷贝 %.2f final拷贝 %.2f",
                gpuMs[0], gpuMs[1], gpuMs[2]);
    }

    /** 实际加载的模型与 SDK 版本；取自插件目录里那份 DLL 的文件版本，不是配置里写的名字 */
    public String getModelText() {
        if (usesDx() && dxCaps != null) return runtimeVersion(backend);
        String[] v = modelVersions;
        return v == null ? null : v[0] + " · " + v[1] + " · " + v[2];
    }

    /** 多帧解锁的实际结果（未启用 / 已改 N 处门禁 / 没找到）；原生库没加载时也要能安全取 */
    public String getMfgUnlockText() {
        if (mfgUnlockReport != null) return mfgUnlockReport;
        if (backend != null && usesDx()) return mfgUnlockReport = "只对 DLSS 后端有效";
        if (!DLSSFGNative.isLoaded()) return mfgUnlockReport = "原生库未加载";
        // 叠加层每帧都读，缓存住，别在渲染路径上反复过 JNI 建字符串
        return mfgUnlockReport = DLSSFGNative.nativeGetMfgUnlockReport();
    }

    /** 构建期内置进 jar 的版本；DLSS 内置一份 Streamline，也可用 streamlinePath 覆盖 */
    public static String builtinVersion(DLSSConfig.Backend b) {
        return switch (b) {
            case DLSS -> VendorVersions.DLSS;
            case FSR -> "FSR SDK " + VendorVersions.FSR_SDK + " · FG DLL "
                    + VendorVersions.FSR_FRAME_GENERATION;
            case XESS -> "XeSS SDK " + VendorVersions.XESS_SDK + " · FG DLL "
                    + VendorVersions.XESS_FRAME_GENERATION + " · XeLL " + VendorVersions.XELL;
        };
    }

    /** 本机实际跑起来的版本。没在跑的后端说明原因，不拿内置版本冒充实机版本。 */
    public String runtimeVersion(DLSSConfig.Backend b) {
        if (backend != b) return "未运行（三个后端互斥，切过来后进世界即生效）";
        if (b == DLSSConfig.Backend.DLSS) {
            String[] v = modelVersions;
            return v == null ? "初始化中" : String.join(" · ", v);
        }
        String[] c = dxCaps;
        if (c == null) return "初始化中";
        String sr = configuredSr >= 0
                ? " · SR " + DLSSConfig.activeSrLabel() + " " + renderW + "x" + renderH
                        + "→" + texW + "x" + texH + String.format(" · SR+回写 %.2f ms（墙钟）", srElapsedMs)
                : " · SR 关闭";
        if (b == DLSSConfig.Backend.FSR) {
            return "provider " + c[2] + " · DLL " + c[3] + " · 可加载 " + c[5] + " · SR provider " + c[7] + sr;
        }
        return "XeLL " + c[0] + " · DLL " + c[4] + " · 本机上限 " + (1 + parseOrZero(c[1]))
                + "x · XeSS-SR " + c[6] + sr;
    }

    private static int parseOrZero(String s) {
        try {
            return Integer.parseInt(s);
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    public void recordPresentation(long now, int presented, boolean takeover) {
        fps.record(now, presented, takeover);
    }

    public void invalidate() {
        hasPrev = false;
        frameCaptured = false;
        lastResult = 0;
    }

    public void setWorldActive(boolean active) {
        worldActive = active;
        invalidate();
        armed = false;
        // 重进世界给预检重新计数的机会（上一次可能只是加载期画面没过来）
        probeCounter = 0;
        if (!active) pendingDisable = deviceReady;
        presentFailures = 0;
        takeoverOff = false;
        latency = bridge = null;
        resetFailureLatch();
        // 状态行写着「改配置或重进世界再试」，那就得真的允许重进世界再试
        if (active && !nativeReady) initAttempted = false;
    }

    // ------------------------------------------------------------------ 初始化

    private boolean ensureNative(Path cacheDir) {
        if (initAttempted) return nativeReady;
        DLSSConfig.Backend chosen = DLSSConfig.activeBackend();
        if (chosen == null) {
            status = "没有启用任何帧生成后端";
            return false;
        }
        backend = chosen;
        initAttempted = true;

        String logDir = DLSSConfig.LOG_PATH.get();
        String logPath = logDir.isEmpty() ? cacheDir.toString() : logDir;
        try {
            if (backend == DLSSConfig.Backend.DLSS) {
                // 配置里没写（或写错了）就用随包自带的 Streamline 运行时
                Path slDir = DLSSFGNative.resolveStreamlineDir(
                        DLSSConfig.STREAMLINE_PATH.get(), cacheDir);
                if (slDir == null) {
                    status = "Streamline 运行时不可用：配置未指定且随包自带的那份解包失败";
                    return false;
                }
                String slPath = slDir.toAbsolutePath().toString();
                if (!DLSSFGNative.load(slDir, cacheDir)) {
                    status = "加载原生库失败: " + DLSSFGNative.getLastError();
                    return false;
                }
                int r = DLSSFGNative.nativeInit(slPath, logPath, DLSSConfig.DLSSG_UNLOCK_MFG.get());
                if (r != 0) {
                    status = "slInit 失败 " + r;
                    return false;
                }
                nativeReady = true;
                modelVersions = DLSSFGNative.nativeGetModelVersions();
                status = "Streamline 已就绪";
                return true;
            }

            vendorRoot = DLSSDXNative.extractVendor(cacheDir);
            if (!DLSSDXNative.load(vendorRoot, cacheDir)) {
                status = "加载 D3D12 后端失败: " + DLSSDXNative.getLastError();
                return false;
            }
            int r = DLSSDXNative.nativeInit(vendorRoot.toString(), logPath);
            if (r != 0) {
                status = "D3D12 后端初始化失败 " + r;
                return false;
            }
            dxCaps = DLSSDXNative.nativeGetCapabilities();
            if (backend == DLSSConfig.Backend.FSR && !"是".equals(dxCaps[5])) {
                status = "内置 FSR 未能加载（缺文件或导出）";
                return false;
            }
            nativeReady = true;
            status = backend.label + " 后端已就绪";
            return true;
        } catch (Throwable t) {
            status = "初始化异常: " + t;
            return false;
        } finally {
            // 失败就不能把「试过了」留下：否则熔断冷却过后也不会再试，
            // 而且换后端（包括换回 DLSS）会跟着一起死在这一条上
            if (!nativeReady) initAttempted = false;
        }
    }

    private boolean ensureDevice(RenderTarget main) {
        if (deviceReady) return true;

        Minecraft mc = Minecraft.getInstance();
        long glfw = mc.getWindow().getWindow();
        long hwnd;
        try {
            hwnd = GLFWNativeWin32.glfwGetWin32Window(glfw);
        } catch (Throwable t) {
            status = "取不到窗口句柄: " + t;
            return false;
        }
        if (hwnd == 0) {
            status = "窗口句柄为 0";
            return false;
        }

        if (usesDx()) {
            int r = DLSSDXNative.nativeCreateDevice(hwnd, main.width, main.height, dxBackendIndex());
            if (r != 0) {
                status = backend.label + " D3D12 设备/代理呈现创建失败 " + r;
                return false;
            }
            dxCaps = DLSSDXNative.nativeGetCapabilities();
            deviceReady = true;
            status = backend.label + " 帧生成已就绪";
            return true;
        }

        int r = DLSSFGNative.nativeCreateDevice(hwnd, main.width, main.height,
                DLSSConfig.REFLEX_BOOST.get(), DLSSConfig.REFLEX_FPS_LIMIT.get());
        if (r != 0) {
            status = "nativeCreateDevice 失败 " + r;
            return false;
        }
        if (DLSSConfig.activeFramesToGenerate() > 0 && DLSSFGNative.nativeIsSupported() != 1) {
            status = "本机 / 当前配置不支持 DLSS 帧生成";
            return false;
        }
        deviceReady = true;
        status = "DLSS Vulkan 已就绪";
        return true;
    }

    /** 每帧检查资源是否需要（重）建。失败会被熔断，不再无限重试。 */
    private boolean ensureResources(RenderTarget main) {
        if (initFailures >= MAX_INIT_FAILURES) return false;
        if (!deviceReady && System.currentTimeMillis() < nextRetryAt) return false;

        if (!ensureNative(DLSSMC.nativesDir())) {
            noteFailure("ensureNative");
            return false;
        }
        if (!ensureDevice(main)) {
            noteFailure("ensureDevice");
            return false;
        }

        boolean sizeChanged = (main.width != texW || main.height != texH);
        int quality = DLSSConfig.activeSrQuality();
        boolean sharpen = DLSSConfig.activeSrSharpen();
        float sharpness = DLSSConfig.activeSrSharpness();
        int preset = usesDx() ? 0 : DLSSConfig.SR_MODEL_PRESET.get().slValue;
        boolean srChanged = quality != configuredSr || preset != configuredPreset;
        if (hudlessTex >= 0 && !sizeChanged && !srChanged) {
            if (sharpen != configuredSharpen || sharpness != configuredSharpness) {
                int r = usesDx() ? DLSSDXNative.nativeConfigureSR(quality, sharpen, sharpness)
                        : DLSSFGNative.nativeConfigureSR(quality, sharpness, preset);
                if (r != 0) {
                    noteFailure("nativeConfigureSR=" + r);
                    return false;
                }
                if (!usesDx()) nativeFrameStarted = false;
                configuredSharpen = sharpen;
                configuredSharpness = sharpness;
            }
            return true;
        }

        if (sizeChanged && texW > 0) {
            if (usesDx()) {
                if (!recreateDxDevice(main)) {
                    noteFailure("DX 重建");
                    return false;
                }
            } else if (DLSSFGNative.nativeResize(main.width, main.height) != 0) {
                noteFailure("nativeResize");
                return false;
            }
        }
        int rw = main.width, rh = main.height;
        int configured = usesDx() ? DLSSDXNative.nativeConfigureSR(quality, sharpen, sharpness)
                : DLSSFGNative.nativeConfigureSR(quality, sharpness, preset);
        if (configured != 0) {
            noteFailure("nativeConfigureSR=" + configured);
            return false;
        }
        if (quality >= 0) {
            int[] size = usesDx() ? DLSSDXNative.nativeGetRenderSize(quality)
                    : DLSSFGNative.nativeGetRenderSize(quality);
            if (size == null || size.length != 2 || size[0] <= 0 || size[1] <= 0
                    || size[0] > main.width || size[1] > main.height) {
                noteFailure("厂商未提供有效渲染分辨率");
                return false;
            }
            rw = size[0];
            rh = size[1];
        }
        int unbound = usesDx() ? DLSSDXNative.nativeUnbindTextures()
                : DLSSFGNative.nativeUnbindTextures();
        if (unbound != 0) {
            noteFailure("纹理注销失败=" + unbound);
            return false;
        }
        deleteGlInputs();
        renderW = rw;
        renderH = rh;
        if (copyReadFbo == 0) copyReadFbo = GL45C.glCreateFramebuffers();
        if (copyDrawFbo == 0) copyDrawFbo = GL45C.glCreateFramebuffers();
        hudlessTex = createRgba8Tex(main.width, main.height);
        finalTex = createRgba8Tex(main.width, main.height);
        depthMv = new FGDepthMotionPass();
        depthMv.resize(renderW, renderH);
        if (quality >= 0) {
            worldTarget = new WorldTarget(renderW, renderH, main.isStencilEnabled());
            if (worldTarget.width != renderW || worldTarget.height != renderH) {
                noteFailure("世界渲染目标尺寸分配失败");
                return false;
            }
            worldTex = createRgba8Tex(renderW, renderH);
            srOutTex = createRgba8Tex(main.width, main.height);
        }
        int r = usesDx()
                ? DLSSDXNative.nativeBindTextures(finalTex, hudlessTex, depthMv.getDepthTexture(),
                        depthMv.getMotionTexture(), worldTex < 0 ? 0 : worldTex,
                        srOutTex < 0 ? 0 : srOutTex, renderW, renderH)
                : DLSSFGNative.nativeBindTextures(
                        finalTex, hudlessTex, depthMv.getDepthTexture(), depthMv.getMotionTexture(),
                        worldTex < 0 ? 0 : worldTex, srOutTex < 0 ? 0 : srOutTex, renderW, renderH);
        if (r == 0 && !usesDx()) {
            int requested = DLSSConfig.activeFramesToGenerate();
            r = DLSSFGNative.nativeSetEnabled(requested > 0 ? 1 : 0);
            if (r == 0 && requested > 0) {
                int effective = DLSSFGNative.nativeSetMode(requested, false);
                if (effective < 0) r = effective;
            }
            configuredFrames = requested;
        }
        if (r != 0) {
            // 绑定失败不要每帧重来（会把 swapchain 反复销毁重建）
            noteFailure("nativeBindTextures=" + r);
            if (usesDx()) DLSSDXNative.nativeHidePresentation();
            else DLSSFGNative.nativeSetEnabled(0);
            return false;
        }

        texW = main.width;
        texH = main.height;
        configuredSr = quality;
        configuredPreset = preset;
        configuredSharpen = sharpen;
        configuredSharpness = sharpness;
        hasPrev = false;
        armed = false; // 重新绑定纹理后要重新预检
        probeCounter = 0;
        status = "帧生成资源已绑定 " + texW + "x" + texH + "，等待画面预检";
        return true;
    }

    /** 换后端：两边都只拆「输入 + 呈现」这一层；DLSS 侧绝不能 slShutdown 后再 slInit。 */
    private void switchBackend(DLSSConfig.Backend next) {
        if (next == null || next == backend) return;
        boolean toreDown = false;
        if (nativeReady) {
            try {
                if (usesDx()) DLSSDXNative.nativeShutdown();
                else {
                    DLSSFGNative.nativeSetEnabled(0);
                    DLSSFGNative.nativeHidePresentation();
                    DLSSFGNative.nativeUnbindTextures();
                }
            } catch (Throwable ignored) {
            }
            deleteGlInputs();
            // 设备层保留（DLSS 侧下次 nativeInit/nativeCreateDevice 都会直接复用），
            // 但后端状态清零，让 ensureNative / ensureResources 为新后端重新初始化与绑定
            nativeReady = false;
            deviceReady = false;
            toreDown = true;
        }
        // 上一个后端可能连初始化都没成功（nativeReady 一直是 false），
        // 这种情况下也必须放行新后端的初始化尝试，不能把闩锁带过去
        initAttempted = false;
        dxCaps = null;
        backend = next;
        configuredFrames = 0;
        hasPrev = false;
        armed = false;
        takeoverOff = false;
        presentFailures = 0;
        fsrTuningPushed = -1;
        xessThresholdPushed = -1f;
        xellFpsPushed = -1;
        xessLegacyPushed = false;
        mfgUnlockReport = null;
        resetFailureLatch();
        if (toreDown) status = "切换到 " + next.label + "，等待重建资源";
    }

    /** 只删 GL 侧输入资源；调用前必须已经让原生层解注册过 */
    private void deleteGlInputs() {
        if (depthMv != null) {
            depthMv.close();
            depthMv = null;
        }
        if (hudlessTex >= 0) {
            GlStateManager._deleteTexture(hudlessTex);
            hudlessTex = -1;
        }
        if (finalTex >= 0) {
            GlStateManager._deleteTexture(finalTex);
            finalTex = -1;
        }
        if (worldTarget != null) {
            worldTarget.destroyBuffers();
            worldTarget = null;
        }
        if (worldTex >= 0) GlStateManager._deleteTexture(worldTex);
        if (srOutTex >= 0) GlStateManager._deleteTexture(srOutTex);
        worldTex = srOutTex = -1;
        inWorldPhase = false;
        renderW = renderH = 0;
        configuredSr = -2;
        configuredPreset = -1;
        nativeFrameStarted = false;
        srElapsedMs = 0;
        sizedColorConverter = null;
        sizedColorSpace = null;
        texW = texH = 0;
    }

    /** D3D12 的代理 swapchain 尺寸定死，改分辨率就整条重建（不涉及 Streamline，安全） */
    private boolean recreateDxDevice(RenderTarget main) {
        try {
            DLSSDXNative.nativeShutdown();
            deviceReady = nativeReady = initAttempted = false;
            configuredFrames = 0;
            fsrTuningPushed = -1;
            xessThresholdPushed = -1f;
            xellFpsPushed = -1;
            xessLegacyPushed = false;
        mfgUnlockReport = null;
            deleteGlInputs();
            return ensureNative(DLSSMC.nativesDir()) && ensureDevice(main);
        } catch (Throwable t) {
            status = "DX 重建异常: " + t;
            return false;
        }
    }

    private int lockInputs(int from, int count) {
        return usesDx() ? DLSSDXNative.nativeLock(from, count) : DLSSFGNative.nativeLock();
    }

    /** 交回原生层：D3D12 这条还会把像素搬进跨 API 纹理、等 D3D11 落地后再解锁 */
    private int unlockInputs() {
        return usesDx() ? DLSSDXNative.nativeDeliver() : DLSSFGNative.nativeUnlock();
    }

    private int probeInputs() {
        return usesDx() ? DLSSDXNative.nativeProbe() : DLSSFGNative.nativeProbe();
    }

    /** @return 0 成功；DLSS 侧的 1/2/11 等细分码只属于 Streamline 那条路 */
    private int presentNative() {
        return usesDx()
                ? DLSSDXNative.nativePresent(matrices, params,
                        DLSSConfig.RESET_HISTORY_EVERY_FRAME.get())
                : DLSSFGNative.nativePresent(matrices, params,
                        DLSSConfig.RESET_HISTORY_EVERY_FRAME.get(),
                        DLSSConfig.STATUS_LOG_INTERVAL.get());
    }

    private int presentedCount() {
        return usesDx() ? DLSSDXNative.nativeGetPresentedCount() : DLSSFGNative.nativeGetPresentedCount();
    }

    /** 交回 GL 上屏：DLSS 侧要真的关掉接管，D3D12 侧只需把覆盖窗口藏掉 */
    private void releaseToGl() {
        if (usesDx()) DLSSDXNative.nativeHidePresentation();
        else DLSSFGNative.nativeSetEnabled(0);
    }

    public void setGlDiag(String diag) {
        this.glDiag = diag;
    }

    public String getGlDiag() {
        return glDiag;
    }

    private static int createRgba8Tex(int w, int h) {
        int tex = TextureUtil.generateTextureId();
        GlStateManager._bindTexture(tex);
        GlStateManager._texParameter(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_MIN_FILTER,
                GL11C.GL_LINEAR);
        GlStateManager._texParameter(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_MAG_FILTER,
                GL11C.GL_LINEAR);
        GlStateManager._texParameter(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_WRAP_S,
                GL30C.GL_CLAMP_TO_EDGE);
        GlStateManager._texParameter(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_WRAP_T,
                GL30C.GL_CLAMP_TO_EDGE);
        GlStateManager._texImage2D(GL11C.GL_TEXTURE_2D, 0, GL11C.GL_RGBA8, w, h, 0,
                GL11C.GL_RGBA, GL11C.GL_UNSIGNED_BYTE, null);
        GlStateManager._bindTexture(0);
        return tex;
    }

    private void copyImage(int src, int dst, int w, int h) {
        GL45C.glNamedFramebufferTexture(copyReadFbo, GL30C.GL_COLOR_ATTACHMENT0, src, 0);
        GL45C.glNamedFramebufferTexture(copyDrawFbo, GL30C.GL_COLOR_ATTACHMENT0, dst, 0);
        boolean scissor = GL11C.glIsEnabled(GL11C.GL_SCISSOR_TEST);
        if (scissor) GL11C.glDisable(GL11C.GL_SCISSOR_TEST);
        GL45C.glBlitNamedFramebuffer(copyReadFbo, copyDrawFbo,
                0, h, w, 0, 0, 0, w, h, GL11C.GL_COLOR_BUFFER_BIT, GL11C.GL_NEAREST);
        if (scissor) GL11C.glEnable(GL11C.GL_SCISSOR_TEST);
    }

    private void noteFailure(String what) {
        initFailures++;
        nextRetryAt = System.currentTimeMillis() + RETRY_COOLDOWN_MS;
        // 失败原因写在 status 里，别被这一行盖掉，否则用户看到的只有阶段名
        String reason = status == null || status.isEmpty() || status.startsWith("初始化失败") ? "" : "：" + status;
        status = "初始化失败(" + initFailures + "/" + MAX_INIT_FAILURES + "): " + what + reason
                + (initFailures >= MAX_INIT_FAILURES ? " —— 已熔断，改配置或重进世界再试" : "");
    }

    public void resetFailureLatch() {
        initFailures = 0;
        nextRetryAt = 0L;
    }

    // ------------------------------------------------------------------ 每帧

    public void beginFrame() {
        frameCaptured = false;
        nativeFrameStarted = false;
        logStatusChange();
        DLSSConfig.Backend wanted = DLSSConfig.activeBackend();
        if (wanted != backend) switchBackend(wanted);
        if (!worldActive || !deviceReady || pendingDisable) return;
        int requested = DLSSConfig.activeFramesToGenerate();
        if (requested != configuredFrames) {
            if (requested == 0) {
                pendingDisable = true;
                invalidate();
                armed = false;
                return;
            }
            if (usesDx()) {
                // XeSS 会自己夹到本机上限并回生效值；FSR 的张数由库决定（回 -1，按 2x 跑）
                int effective = DLSSDXNative.nativeSetInterpolatedFrames(requested);
                configuredFrames = requested;
                hasPrev = false;
                armed = false;
                if (effective > 0 && effective != requested) {
                    status = backend.label + " 本机上限只到 " + (effective + 1) + "x，已自动降档";
                }
            } else {
                int result = DLSSFGNative.nativeSetEnabled(1);
                // 不强制：解锁成功后 SDK 自己就把上限报成 5，夹值不会再压档；
                // 解锁没生效时上限仍是 1，这里会夹回 2x 并给出可读的降档原因。
                if (result == 0) result = DLSSFGNative.nativeSetMode(requested, false);
                if (result < 0) {
                    status = "帧生成设置失败 " + result;
                    return;
                }
                if (result > 0 && result != requested) {
                    // nativeSetMode 回的是生效张数：没解锁时本机上限只报 1，3x/4x 会被夹回 2x
                    status = "DLSS 本机上限只到 " + (result + 1) + "x，已自动降档"
                            + (DLSSConfig.DLSSG_UNLOCK_MFG.get()
                                    ? "（解锁多帧没生效：" + DLSSFGNative.nativeGetMfgUnlockReport() + "）"
                                    : "（想上 3x/4x 请开「解锁多帧生成」并重启游戏）");
                } else if (requested > 1) {
                    status = "多帧生成生效 " + (requested + 1) + "x（"
                            + DLSSFGNative.nativeGetMfgUnlockReport() + "）";
                }
                configuredFrames = requested;
                hasPrev = false;
                armed = false;
            }
        }
        if (requested > 0) {
            if (usesDx()) {
                // FSR/XeSS 的调优项：配置变了才推一次原生
                if (backend == DLSSConfig.Backend.FSR) {
                    int f = (DLSSConfig.FSR_FG_ASYNC.get() ? 1 : 0)
                            | (DLSSConfig.FSR_FG_ONLY_GENERATED.get() ? 2 : 0);
                    if (f != fsrTuningPushed
                            && DLSSDXNative.nativeSetFsrTuning((f & 1) != 0, (f & 2) != 0) == 0) {
                        fsrTuningPushed = f;
                    }
                } else if (backend == DLSSConfig.Backend.XESS) {
                    float t = DLSSConfig.XESS_SCENE_THRESHOLD.get().floatValue();
                    if (t != xessThresholdPushed
                            && DLSSDXNative.nativeSetSceneChangeThreshold(t) == 0) {
                        xessThresholdPushed = t;
                    }
                    boolean legacy = DLSSConfig.XESS_LEGACY_SCALE.get();
                    if (legacy != xessLegacyPushed
                            && DLSSDXNative.nativeSetXessLegacyScale(legacy) == 0) {
                        xessLegacyPushed = legacy;
                    }
                }
                // XeLL 限帧（FSR / XeSS 共用；DLSS-G 走 Reflex，不调）
                int xellFps = DLSSConfig.XELL_FPS_LIMIT.get();
                if (xellFps != xellFpsPushed
                        && DLSSDXNative.nativeSetXellFpsLimit(xellFps) == 0) {
                    xellFpsPushed = xellFps;
                }
            }
            if (!usesDx()) {
                // 低 5 位是布尔开关，往上是工作模式；目标帧率单独比，因为它可以有 0..1000
                int tuning = (DLSSConfig.FG_SHOW_ONLY_INTERPOLATED.get() ? 1 : 0)
                        | (DLSSConfig.FG_RETAIN_RESOURCES.get() ? 2 : 0)
                        | (DLSSConfig.FG_MENU_DETECTION.get() ? 4 : 0)
                        | (DLSSConfig.FG_QUEUE_PARALLELISM.get() ? 8 : 0)
                        | (DLSSConfig.FG_UI_RECOMPOSITION.get() ? 16 : 0)
                        | (DLSSConfig.DLSSG_MODE.get().slValue << 5);
                int targetFps = DLSSConfig.DLSSG_DYNAMIC_FPS.get();
                if (tuning != tunedFlags || targetFps != tunedFps) {
                    DLSSFGNative.nativeSetTuning((tuning & 1) != 0, (tuning & 2) != 0, (tuning & 4) != 0,
                            (tuning & 8) != 0, (tuning & 16) != 0, (tuning >> 5) & 3, (float) targetFps);
                    tunedFlags = tuning;
                    tunedFps = targetFps;
                }
                if ((latencyTick++ & 15) == 0) {
                    latency = DLSSFGNative.nativeGetLatency();
                    bridge = DLSSFGNative.nativeGetBridgeTimings();
                }
            }
        }
        if (!usesDx() && isEnabled() && startDlssFrame()) {
            org.lwjgl.glfw.GLFW.glfwPollEvents();
        }
    }

    private boolean startDlssFrame() {
        if (nativeFrameStarted) return true;
        int result = DLSSFGNative.nativeBeginFrame(DLSSConfig.REFLEX_BOOST.get(), DLSSConfig.REFLEX_FPS_LIMIT.get());
        nativeFrameStarted = result == 0;
        if (!nativeFrameStarted) status = "DLSS 帧开始失败 " + result;
        return nativeFrameStarted;
    }

    public void renderStart() {
        if (worldActive && deviceReady && !pendingDisable && isEnabled() && !usesDx())
            DLSSFGNative.nativeRenderStart();
    }

    public void captureMatrices(Matrix4f modelView, Matrix4f projection, Camera camera) {
        if (!isEnabled()) return;
        this.modelViewRot.set(modelView);
        this.proj.set(projection);
    }

    public RenderTarget getActiveMainTarget() {
        return inWorldPhase ? worldTarget : null;
    }

    public void beginWorldPhase() {
        if (!worldActive || pendingDisable || !isEnabled() || DLSSConfig.activeSrQuality() < 0) return;
        RenderTarget main = Minecraft.getInstance().getMainRenderTarget();
        try {
            if (!ensureResources(main) || worldTarget == null) return;
            if (!usesDx() && !nativeFrameStarted) {
                if (!startDlssFrame()) return;
                DLSSFGNative.nativeRenderStart();
            }
            worldTarget.setClearColor(0f, 0f, 0f, 0f);
            worldTarget.clear(false);
            worldTarget.bindWrite(true);
            inWorldPhase = true;
        } catch (Throwable t) {
            status = "超分世界阶段初始化异常: " + t;
            noteFailure("beginWorldPhase");
        } finally {
            if (!inWorldPhase) main.bindWrite(true);
        }
    }

    /** 世界阶段结束：Iris 手已在场景内，原版手与 HUD 尚未绘制。 */
    public void captureHudless() {
        boolean upscale = inWorldPhase;
        inWorldPhase = false;
        Minecraft mc = Minecraft.getInstance();
        RenderTarget main = mc.getMainRenderTarget();
        if (main == null) return;
        RenderTarget source = upscale ? worldTarget : main;
        boolean composed = !upscale;
        try {
            if (!worldActive || pendingDisable || !isEnabled()) return;
            if (!upscale && DLSSConfig.activeSrQuality() >= 0) return;
            if (!upscale && !ensureResources(main)) return;
            if (!usesDx() && !nativeFrameStarted) {
                if (!startDlssFrame()) return;
                DLSSFGNative.nativeRenderStart();
            }
            gpuTimingCollect();
            var camPos = mc.gameRenderer.getMainCamera().getPosition();
            Matrix4f view = new Matrix4f(modelViewRot)
                    .translate((float) -camPos.x, (float) -camPos.y, (float) -camPos.z);
            Matrix4f invViewProj = new Matrix4f(proj).mul(view).invert();
            Matrix4f pProj = hasPrev ? prevProj : proj;
            Matrix4f pView = hasPrev ? prevView : view;
            int preHandDepth = 0, postHandDepth = 0;
            if (Iris.getPipelineManager().getPipelineNullable() instanceof IrisPipelineAccessor pipeline) {
                var targets = pipeline.dlssmc$getRenderTargets();
                if (targets.getDepthTexture() == source.getDepthTextureId()) {
                    preHandDepth = targets.getDepthTextureNoHand().getTextureId();
                    postHandDepth = targets.getDepthTextureNoTranslucents().getTextureId();
                }
            }
            if (lockInputs(upscale ? 2 : 1, 3) != 0)
                throw new IllegalStateException("互操作纹理锁定失败");
            try {
                boolean depthMvTiming = gpuTimingBegin(0);
                try {
                    depthMv.run(source.getDepthTextureId(), preHandDepth, postHandDepth,
                            invViewProj, new Matrix4f(pProj).mul(pView), DLSSConfig.ZERO_MOTION_VECTORS.get());
                } finally {
                    gpuTimingEnd(0, depthMvTiming);
                }
                boolean colorTiming = gpuTimingBegin(1);
                try {
                    copyImage(source.getColorTextureId(), upscale ? worldTex : hudlessTex,
                            source.width, source.height);
                } finally {
                    gpuTimingEnd(1, colorTiming);
                }
            } finally {
                GL11C.glFinish();
                if (unlockInputs() != 0) throw new IllegalStateException("互操作交付失败");
            }
            proj.get(matrices, 0);
            view.get(matrices, 16);
            pProj.get(matrices, 32);
            pView.get(matrices, 48);
            params[0] = 0.05F;
            params[1] = 1000.0F;
            params[2] = (float) Math.toRadians(mc.options.fov().get().intValue());
            params[3] = (float) main.width / main.height;
            params[4] = params[5] = 0.0F;
            params[6] = 0.5F;
            params[7] = -0.5F;
            if (upscale) {
                long start = System.nanoTime();
                boolean reset = !hasPrev || DLSSConfig.RESET_HISTORY_EVERY_FRAME.get();
                int r = usesDx() ? DLSSDXNative.nativeUpscale(params, reset)
                        : DLSSFGNative.nativeUpscale(matrices, params, reset);
                if (r != 0) throw new IllegalStateException("nativeUpscale=" + r);
                try {
                    copyImage(srOutTex, main.getColorTextureId(), main.width, main.height);
                } finally {
                    GL11C.glFinish();
                    int done = usesDx() ? DLSSDXNative.nativeUpscaleDone() : DLSSFGNative.nativeUpscaleDone();
                    if (done != 0) throw new IllegalStateException("超分输出解锁失败");
                }
                srElapsedMs = (System.nanoTime() - start) / 1_000_000.0;
                composed = true;
                if (Iris.getPipelineManager().getPipelineNullable() instanceof IrisPipelineAccessor pipeline) {
                    var converter = pipeline.dlssmc$getColorSpaceConverter();
                    var colorSpace = net.irisshaders.iris.gui.option.IrisVideoSettings.colorSpace;
                    if (!hasPrev || converter != sizedColorConverter || colorSpace != sizedColorSpace) {
                        // Iris 的 TAIL 转换在 SR 之后执行，必须覆盖全尺寸主 target。
                        converter.rebuildProgram(main.width, main.height, colorSpace);
                        sizedColorConverter = converter;
                        sizedColorSpace = colorSpace;
                    }
                }
            }
            prevProj.set(proj);
            prevView.set(view);
            hasPrev = true;
            frameCaptured = true;
        } catch (Throwable t) {
            status = "captureHudless 异常: " + t;
            hasPrev = false;
            srElapsedMs = 0;
            noteFailure("世界交付");
        } finally {
            if (!composed) {
                // 厂商执行失败也要把已渲染的世界交回 GL，不能留下空白主 target。
                GL45C.glNamedFramebufferTexture(copyReadFbo, GL30C.GL_COLOR_ATTACHMENT0, source.getColorTextureId(), 0);
                GL45C.glNamedFramebufferTexture(copyDrawFbo, GL30C.GL_COLOR_ATTACHMENT0, main.getColorTextureId(), 0);
                boolean scissor = GL11C.glIsEnabled(GL11C.GL_SCISSOR_TEST);
                if (scissor) GL11C.glDisable(GL11C.GL_SCISSOR_TEST);
                GL45C.glBlitNamedFramebuffer(copyReadFbo, copyDrawFbo, 0, 0, source.width, source.height,
                        0, 0, main.width, main.height, GL11C.GL_COLOR_BUFFER_BIT, GL11C.GL_LINEAR);
                if (scissor) GL11C.glEnable(GL11C.GL_SCISSOR_TEST);
            }
            main.bindWrite(true);
        }
    }

    /**
     * 替换 GL 的上屏。由 MixinWindow 调用。
     *
     * @return true 表示已经由我们上屏，调用方应跳过 RenderSystem.flipFrame
     */
    public boolean presentFrame() {
        if (!frameCaptured) return false;
        frameCaptured = false;
        if (!worldActive || pendingDisable || DLSSConfig.activeFramesToGenerate() == 0
                || !deviceReady || !hasPrev) return false;
        if (hudlessTex < 0) return false;
        if (takeoverOff) return false; // 已经交回 GL，不要再碰
        if (!armed && probeCounter >= MAX_PROBE_FRAMES) return false; // 预检熔断：等重绑或重进世界

        try {
            Minecraft mc = Minecraft.getInstance();
            RenderTarget main = mc.getMainRenderTarget();
            if (main == null) return false;

            if (lockInputs(0, 1) != 0) {
                status = "互操作纹理锁定失败";
                return false;
            }
            try {
                boolean finalTiming = gpuTimingBegin(2);
                copyImage(main.getColorTextureId(), finalTex, main.width, main.height);
                gpuTimingEnd(2, finalTiming);
                GL11C.glFinish();
            } finally {
                unlockInputs();
            }

            // 还没接管时先预检：只有确认画面真的经桥传过来了才敢接管上屏，
            // 否则一接管就是黑屏
            if (!armed) {
                probeCounter++;
                int ok = probeInputs();
                if (ok == 1) {
                    armed = true;
                    status = backend.label + " 已接管上屏 " + texW + "x" + texH;
                } else {
                    status = "预检中（第 " + probeCounter + " 帧）：画面尚未经桥传过来";
                }
                return false; // 无论结果如何这一帧都让 GL 正常上屏
            }

            lastResult = presentNative();
            lastPresented = presentedCount();
            if (lastResult == 0) {
                presentedFrames++;
                presentFailures = 0;
                if (usesDx()) DLSSDXNative.nativeShowPresentation();
                return true;
            }
            if (!usesDx()) {
                if (lastResult == 11) {
                    // 源画面全黑 —— 原生层已销毁 swapchain 把窗口交回 GL
                    DLSSFGNative.nativeSetEnabled(0);
                    takeoverOff = true;
                    status = "源画面全黑，上屏接管已停用（GL→Vulkan 互操作未生效）";
                    return false;
                }
                if (lastResult == 1 || lastResult == 2) {
                    // 1 = swapchain 过期，2 = 这一帧拿不到图像；都交回 GL 上屏，下一帧再试
                    if (lastResult == 1 && DLSSFGNative.nativeResize(texW, texH) == 0) {
                        int rebound = DLSSFGNative.nativeBindTextures(finalTex, hudlessTex,
                                depthMv.getDepthTexture(), depthMv.getMotionTexture(),
                                worldTex < 0 ? 0 : worldTex, srOutTex < 0 ? 0 : srOutTex, renderW, renderH);
                        if (rebound != 0 || DLSSFGNative.nativeSetEnabled(1) != 0)
                            throw new IllegalStateException("交换链重绑失败=" + rebound);
                        nativeFrameStarted = false;
                        hasPrev = false;
                        armed = false;
                    }
                    return false;
                }
            }

            // 其他错误：连续失败就彻底关掉接管，避免黑屏
            presentFailures++;
            status = "presentFrame 失败 " + lastResult + " (" + presentFailures + "/"
                    + MAX_PRESENT_FAILURES + ")";
            if (presentFailures >= MAX_PRESENT_FAILURES) {
                releaseToGl();
                takeoverOff = true;
                status = "上屏接管已停用（连续失败 " + presentFailures + " 次），交回 GL";
                noteFailure("present");
            }
            return false;
        } catch (Throwable t) {
            status = "presentFrame 异常: " + t;
            return false;
        }
    }

    public void afterGlSwap() {
        // GL 前缓冲在 FG 接管时不会更新，必须先交换本帧再移除覆盖窗口。
        if (nativeReady) {
            if (usesDx()) DLSSDXNative.nativeHidePresentation();
            else DLSSFGNative.nativeHidePresentation();
        }
        if (pendingDisable) {
            if (usesDx()) {
                // D3D12 侧没有「关掉接管」这个开关：不再 Present 就等于交回 GL
                pendingDisable = false;
                configuredFrames = 0;
                status = "已暂停";
                return;
            }
            int result = DLSSFGNative.nativeSetEnabled(0);
            if (result != 0) {
                status = "帧生成暂停失败 " + result;
                return;
            }
            pendingDisable = false;
            configuredFrames = 0;
            status = "已暂停";
        }
    }

    public void shutdown() {
        if (nativeReady) {
            try {
                if (usesDx()) DLSSDXNative.nativeShutdown();
                else DLSSFGNative.nativeShutdown();
            } catch (Throwable ignored) {
            }
            nativeReady = false;
        }
        deviceReady = false;
        initAttempted = false;
        backend = null;
        dxCaps = null;
        vendorRoot = null;
        worldActive = frameCaptured = pendingDisable = false;
        fps.reset();
        latency = null;
        tunedFlags = -1;
        tunedFps = -1;
        fsrTuningPushed = -1;
        xessThresholdPushed = -1f;
        xellFpsPushed = -1;
        xessLegacyPushed = false;
        mfgUnlockReport = null;
        bridge = null;
        modelVersions = null;
        java.util.Arrays.fill(gpuPending, false);
        java.util.Arrays.fill(gpuMs, 0.0);
        if (gpuQueryIds[0] != 0) GL15C.glDeleteQueries(gpuQueryIds);
        java.util.Arrays.fill(gpuQueryIds, 0);
        deleteGlInputs();
        if (copyReadFbo != 0) GL30C.glDeleteFramebuffers(copyReadFbo);
        if (copyDrawFbo != 0) GL30C.glDeleteFramebuffers(copyDrawFbo);
        copyReadFbo = copyDrawFbo = 0;
        depthMv = null;
        configuredFrames = 0;
        texW = texH = 0;
        hasPrev = false;
        armed = false;
        takeoverOff = false;
        status = "已关闭";
    }
}
