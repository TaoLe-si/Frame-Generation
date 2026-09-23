package com.taolesi.dlssmc.core;

import com.mojang.blaze3d.pipeline.MainTarget;
import com.mojang.blaze3d.pipeline.RenderTarget;
import com.mojang.blaze3d.platform.GlStateManager;
import com.mojang.blaze3d.platform.TextureUtil;
import com.taolesi.dlssmc.config.DLSSConfig;
import com.taolesi.dlssmc.nativebridge.DLSSNative;
import com.taolesi.dlssmc.render.DepthMotionPass;
import net.minecraft.client.Minecraft;
import net.minecraft.client.Camera;
import org.joml.Matrix4f;
import org.lwjgl.opengl.GL11C;
import org.lwjgl.opengl.GL30C;
import org.lwjgl.opengl.GL45C;

import java.nio.file.Path;

/**
 * DLSS 运行时调度。
 *
 * 渲染流程（对照反编译源码）：
 *   GameRenderer.renderLevel() HEAD        -> beginWorldPhase()  切到低分辨率 worldTarget
 *     LevelRenderer.renderLevel(...)       -> 世界 + Iris 光影合成 -> worldTarget
 *   GameRenderer.renderLevel() 该调用返回后 -> endWorldPhase()    DLSS 放大写回主 target，切回全分辨率
 *     renderItemInHand / GUI               -> 天然全分辨率
 *   Minecraft:1216 blitToScreen            -> 原样走
 */
public final class DLSSRuntime {

    private static final DLSSRuntime INSTANCE = new DLSSRuntime();

    public static DLSSRuntime get() {
        return INSTANCE;
    }

    private boolean nativeReady = false;
    private boolean initAttempted = false;

    private MainTarget worldTarget;
    private DepthMotionPass auxPass;

    // 互操作暂存纹理：注册进 WGL 互操作的必须是「我们自己独占」的纹理。
    // 绝不能注册 worldTarget / 主 target 的颜色附件 —— 注册后锁外一切 GL 使用
    // （包括原版渲染）都会变成 GL_INVALID_FRAMEBUFFER_OPERATION（整屏白屏的根因）。
    private int worldTex = -1;   // 渲染分辨率，世界颜色的交付中转
    private int outTex = -1;     // 呈现分辨率，超分结果的回读中转
    private int copyReadFbo;
    private int copyDrawFbo;

    private int displayW, displayH;
    private int renderW, renderH;
    private boolean inWorldPhase = false;

    // 每帧相机矩阵
    private final Matrix4f proj = new Matrix4f();
    private final Matrix4f modelViewRot = new Matrix4f();
    private final Matrix4f prevProj = new Matrix4f();
    private final Matrix4f prevView = new Matrix4f();
    private boolean hasPrev = false;

    private final float[] matrices = new float[64];
    private final float[] params = new float[8];

    private String status = "未初始化";
    private int lastErrorCode = 0;

    private String setupKey = "";

    // 原生初始化失败后的重试冷却，避免每帧反复 slInit
    private long nextNativeRetryAt = 0L;
    private static final long NATIVE_RETRY_COOLDOWN_MS = 5000L;

    /**
     * dlssmc_native.dll 超分通道的启用开关。
     *
     * 2026-09-23 白屏根因已在原生层修复：nativeSetup 改为注册我们自己的交付中转纹理
     * （worldTex/outTex），不再碰 MC 的附件；输入/输出分段加锁（nativeLockInputs /
     * nativeUnlockInputs / nativeEvaluateDone），GL 写入全部在锁内；slInit 补上了
     * eUseFrameBasedResourceTagging。MV 约定与已验证的 FG 路径同构（预翻转采样、
     * prev-current、mvecScale 0.5/-0.5）。
     */
    static final boolean SR_NATIVE_PATH_ENABLED = true;

    private DLSSRuntime() {}

    // ------------------------------------------------------------------ 生命周期

    public boolean ensureNative(Path nativeCacheDir) {
        if (initAttempted) return nativeReady;
        if (System.currentTimeMillis() < nextNativeRetryAt) return false;
        initAttempted = true;

        String slPath = DLSSConfig.STREAMLINE_PATH.get();
        try {
            Path slDir = Path.of(slPath);
            if (!DLSSNative.load(slDir, nativeCacheDir)) {
                status = "加载原生库失败: " + DLSSNative.getLastError();
                return false;
            }

            int r = DLSSNative.nativeInit(slPath, DLSSConfig.LOG_PATH.get());
            if (r != 0) {
                status = "slInit 失败，代码 " + r;
                return false;
            }
            if (DLSSNative.nativeIsDlssSupported() != 1) {
                status = "当前 GPU / 驱动不支持 DLSS";
                return false;
            }
            nativeReady = true;
            status = "就绪";
            return true;
        } catch (Throwable t) {
            status = "初始化异常: " + t;
            return false;
        } finally {
            // 失败不能留下「试过了」：否则改了 Streamline 路径后也不会再试
            if (!nativeReady) {
                initAttempted = false;
                nextNativeRetryAt = System.currentTimeMillis() + NATIVE_RETRY_COOLDOWN_MS;
            }
        }
    }

    public void shutdown() {
        if (nativeReady) {
            DLSSNative.nativeReleaseResources();
            DLSSNative.nativeShutdown();
            nativeReady = false;
        }
        destroyTargets();
        initAttempted = false;
        nextNativeRetryAt = 0L;
        status = "未初始化";
    }

    private void destroyTargets() {
        if (auxPass != null) {
            auxPass.close();
            auxPass = null;
        }
        if (worldTarget != null) {
            worldTarget.destroyBuffers();
            worldTarget = null;
        }
        if (worldTex >= 0) {
            GlStateManager._deleteTexture(worldTex);
            worldTex = -1;
        }
        if (outTex >= 0) {
            GlStateManager._deleteTexture(outTex);
            outTex = -1;
        }
        if (copyReadFbo != 0) {
            GL30C.glDeleteFramebuffers(copyReadFbo);
            copyReadFbo = 0;
        }
        if (copyDrawFbo != 0) {
            GL30C.glDeleteFramebuffers(copyDrawFbo);
            copyDrawFbo = 0;
        }
        setupKey = "";
    }

    public boolean isEnabled() {
        return nativeReady && DLSSConfig.isEnabled();
    }

    public String getStatus() {
        return status + (lastErrorCode != 0 ? " (err " + lastErrorCode + ")" : "");
    }

    /**
     * 世界渲染阶段由 MixinMinecraft 调用。返回 null 表示用原版主 target。
     */
    public RenderTarget getActiveMainTarget() {
        return inWorldPhase ? worldTarget : null;
    }

    public int getRenderWidth() { return renderW; }
    public int getRenderHeight() { return renderH; }
    public int getDisplayWidth() { return displayW; }
    public int getDisplayHeight() { return displayH; }

    // ------------------------------------------------------------------ 每帧

    public void beginWorldPhase(Path nativeCacheDir) {
        if (!SR_NATIVE_PATH_ENABLED) return; // 冻结通道，见字段注释
        // 只按配置档位把关：nativeReady 要到 ensureNative 才会置位，
        // 用 isEnabled()（含 nativeReady）做门卫会把第一次初始化永远挡在外面
        if (!DLSSConfig.isEnabled()) return;
        Minecraft mc = Minecraft.getInstance();
        if (mc.level == null) return;
        if (!nativeReady && !ensureNative(nativeCacheDir)) return;
        if (!ensureTargets(nativeCacheDir)) return;

        // MC 在 renderLevel 之前已经清过一次主 target，切过来后必须自己清
        worldTarget.setClearColor(0.0F, 0.0F, 0.0F, 0.0F);
        worldTarget.bindWrite(true);
        worldTarget.clear(false);
        inWorldPhase = true;
    }

    public void captureMatrices(Matrix4f modelView, Matrix4f projection, Camera camera) {
        if (!isEnabled()) return;
        this.modelViewRot.set(modelView);
        this.proj.set(projection);
    }

    public void endWorldPhase() {
        if (!inWorldPhase) return;
        inWorldPhase = false;

        Minecraft mc = Minecraft.getInstance();
        RenderTarget main = mc.getMainRenderTarget();
        boolean delivered = false;
        try {
            // view = modelViewRot * T(-camPos)
            var cam = mc.gameRenderer.getMainCamera();
            var camPos = cam.getPosition();
            Matrix4f view = new Matrix4f(modelViewRot)
                    .translate((float) -camPos.x, (float) -camPos.y, (float) -camPos.z);
            Matrix4f viewProj = new Matrix4f(proj).mul(view);
            Matrix4f invViewProj = new Matrix4f(viewProj).invert();

            Matrix4f pProj = hasPrev ? prevProj : proj;
            Matrix4f pView = hasPrev ? prevView : view;

            // ---- 输入段：锁内写 aux 与世界颜色（锁外写 D3D 读不到），写完交回 ----
            int lr = DLSSNative.nativeLockInputs();
            if (lr != 0) throw new IllegalStateException("输入锁定失败 " + lr);
            try {
                auxPass.run(worldTarget.getDepthTextureId(), invViewProj, new Matrix4f(pProj).mul(pView));
                // 翻转交付：与 FG 路径同款，D3D 侧才看到正立画面
                blitColor(worldTarget.getColorTextureId(), worldTex, renderW, renderH, renderW, renderH, true);
                GL11C.glFinish();
            } finally {
                DLSSNative.nativeUnlockInputs();
            }

            proj.get(matrices, 0);
            view.get(matrices, 16);
            pProj.get(matrices, 32);
            pView.get(matrices, 48);

            float aspect = renderH > 0 ? (float) renderW / renderH : 1.0F;
            params[0] = 0.05F;               // near
            params[1] = 1000.0F;             // far（仅供参考，DLSS 主要靠矩阵）
            params[2] = (float) Math.toRadians(mc.options.fov().get());
            params[3] = aspect;
            params[4] = 0.0F;                // jitterX
            params[5] = 0.0F;                // jitterY
            // 与 FG 路径同一套约定：MV 是 prev-current，Y 换算到 D3D 的 +Y 向下
            params[6] = 0.5F;                // mvecScaleX
            params[7] = -0.5F;               // mvecScaleY

            int r = DLSSNative.nativeEvaluate(matrices, params, !hasPrev);
            if (r != 0) {
                lastErrorCode = r;
                status = "DLSS 评估失败，代码 " + r;
            } else {
                // 输出此刻是锁着的（nativeEvaluate 成功返回时保持锁定）：
                // 读回、贴进主 target，然后必须解锁
                try {
                    // 回读同样翻转：out 在 D3D 侧是正立的，翻回来才是 GL 的正立
                    blitColor(outTex, main.getColorTextureId(), displayW, displayH, displayW, displayH, true);
                    GL11C.glFinish();
                } finally {
                    DLSSNative.nativeEvaluateDone();
                }
                delivered = true;
                if (lastErrorCode != 0) {
                    // 之前评估失败、这次成功：清掉错误码并恢复状态
                    lastErrorCode = 0;
                    status = "就绪";
                }
            }

            prevProj.set(proj);
            prevView.set(view);
            hasPrev = true;
        } catch (Throwable t) {
            status = "渲染异常: " + t;
        } finally {
            if (!delivered && worldTarget != null && main != null) {
                // 评估失败也要把已渲染的世界交回主 target（拉伸放大），不能留一张空屏
                blitColor(worldTarget.getColorTextureId(), main.getColorTextureId(),
                        renderW, renderH, displayW, displayH, false);
            }
            // 切回全分辨率主 target，手持物品与 HUD 才会是原生的
            if (main != null) main.bindWrite(true);
        }
    }

    /** GL→GL 颜色 blit。flip=true 时垂直翻转（交付给 D3D / 从 D3D 回读用）。 */
    private void blitColor(int src, int dst, int sw, int sh, int dw, int dh, boolean flip) {
        if (copyReadFbo == 0 || copyDrawFbo == 0 || src < 0 || dst < 0) return;
        GL45C.glNamedFramebufferTexture(copyReadFbo, GL30C.GL_COLOR_ATTACHMENT0, src, 0);
        GL45C.glNamedFramebufferTexture(copyDrawFbo, GL30C.GL_COLOR_ATTACHMENT0, dst, 0);
        int filter = (sw == dw && sh == dh) ? GL11C.GL_NEAREST : GL11C.GL_LINEAR;
        boolean scissor = GL11C.glIsEnabled(GL11C.GL_SCISSOR_TEST);
        if (scissor) GL11C.glDisable(GL11C.GL_SCISSOR_TEST);
        if (flip) {
            GL45C.glBlitNamedFramebuffer(copyReadFbo, copyDrawFbo, 0, sh, sw, 0, 0, 0, dw, dh,
                    GL11C.GL_COLOR_BUFFER_BIT, filter);
        } else {
            GL45C.glBlitNamedFramebuffer(copyReadFbo, copyDrawFbo, 0, 0, sw, sh, 0, 0, dw, dh,
                    GL11C.GL_COLOR_BUFFER_BIT, filter);
        }
        if (scissor) GL11C.glEnable(GL11C.GL_SCISSOR_TEST);
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

    public void onDimensionChanged() {
        hasPrev = false;
    }

    /** 档位/配置改变后调用：下一帧重建 worldTarget 与 DLSS 资源 */
    public void invalidate() {
        setupKey = "";
        lastErrorCode = 0;
        hasPrev = false;
    }

    // ------------------------------------------------------------------ 资源

    private boolean ensureTargets(Path nativeCacheDir) {
        Minecraft mc = Minecraft.getInstance();
        int newDisplayW = mc.getWindow().getWidth();
        int newDisplayH = mc.getWindow().getHeight();

        DLSSConfig.Quality q = DLSSConfig.MODE.get();
        String key = newDisplayW + "x" + newDisplayH + "@" + q.slValue;
        if (key.equals(setupKey) && worldTarget != null && auxPass != null) return true;

        if (!nativeReady) {
            if (!ensureNative(nativeCacheDir)) return false;
        }

        displayW = newDisplayW;
        displayH = newDisplayH;

        int[] opt = DLSSNative.nativeGetOptimalSettings(displayW, displayH, q.slValue);
        renderW = opt[0];
        renderH = opt[1];
        if (renderW <= 0 || renderH <= 0) {
            status = "无法获取 DLSS 推荐分辨率";
            return false;
        }

        if (worldTarget != null) worldTarget.destroyBuffers();
        worldTarget = new MainTarget(renderW, renderH);

        if (auxPass == null) {
            auxPass = new DepthMotionPass();
        }
        auxPass.resize(renderW, renderH);

        if (copyReadFbo == 0) copyReadFbo = GL45C.glCreateFramebuffers();
        if (copyDrawFbo == 0) copyDrawFbo = GL45C.glCreateFramebuffers();
        if (worldTex >= 0) GlStateManager._deleteTexture(worldTex);
        if (outTex >= 0) GlStateManager._deleteTexture(outTex);
        worldTex = createRgba8Tex(renderW, renderH);
        outTex = createRgba8Tex(displayW, displayH);

        RenderTarget main = mc.getMainRenderTarget();
        float sharpness = DLSSConfig.SHARPNESS.get().floatValue();

        // 交付中转纹理而不是 MC 自己的附件：见字段注释（白屏根因）
        int r = DLSSNative.nativeSetup(displayW, displayH, renderW, renderH,
                q.slValue, sharpness, false, DLSSConfig.SR_MODEL_PRESET.get().slValue,
                worldTex, auxPass.getAuxTexture(), outTex);
        lastErrorCode = r;
        if (r != 0) {
            status = "nativeSetup 失败，代码 " + r;
            return false;
        }

        // setupKey 只在完全成功后提交：提前提交会让下一帧带着坏掉的 DLSS 设置
        // 直接早退成功，世界渲进低分 target 却永远等不到放大
        setupKey = key;
        status = "就绪 " + renderW + "x" + renderH + " -> " + displayW + "x" + displayH;
        hasPrev = false;
        return true;
    }
}
