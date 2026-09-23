package com.taolesi.dlssmc.render;

import com.mojang.blaze3d.platform.GlStateManager;
import com.mojang.blaze3d.platform.TextureUtil;
import com.mojang.blaze3d.systems.RenderSystem;
import org.joml.Matrix4f;
import org.lwjgl.opengl.GL11C;
import org.lwjgl.opengl.GL13C;
import org.lwjgl.opengl.GL15C;
import org.lwjgl.opengl.GL20C;
import org.lwjgl.opengl.GL30C;

import java.nio.FloatBuffer;

/**
 * 生成 DLSS 需要的「线性深度 + 相机运动向量」缓冲。
 *
 * 为什么需要它：MC 主帧缓冲的深度是 DEPTH_COMPONENT32F，
 * 这种格式没法通过 WGL_NV_DX_interop2 直接共享给 D3D11，
 * 所以先在 GL 侧做一次转换，输出到一张 RGBA16F：
 *   R = 原始非线性深度 [0,1]
 *   G = 运动向量 x（NDC 增量）
 *   B = 运动向量 y（NDC 增量）
 *   A = 1
 */
public final class DepthMotionPass implements AutoCloseable {

    private static final String VERT = """
            #version 150
            in vec2 aPos;
            out vec2 vUv;
            void main() {
                vUv = aPos * 0.5 + 0.5;
                gl_Position = vec4(aPos, 0.0, 1.0);
            }
            """;

    private static final String FRAG = """
            #version 150
            in vec2 vUv;
            out vec4 outColor;
            uniform sampler2D uDepth;
            uniform mat4 uInvViewProj;
            uniform mat4 uPrevViewProj;
            void main() {
                // 预翻转采样：D3D 侧读这张纹理才是正立的（与 FGDepthMotionPass 同款约定）
                vec2 sourceUv = vec2(vUv.x, 1.0 - vUv.y);
                float d = texture(uDepth, sourceUv).r;
                if (d >= 1.0) {
                    // 天空：没有有效的世界坐标，运动向量置零
                    outColor = vec4(d, 0.0, 0.0, 1.0);
                    return;
                }
                vec3 ndc = vec3(sourceUv * 2.0 - 1.0, d * 2.0 - 1.0);
                vec4 world = uInvViewProj * vec4(ndc, 1.0);
                world /= world.w;
                vec4 prevClip = uPrevViewProj * world;
                vec2 prevNdc = prevClip.xy / prevClip.w;
                // prev-current：与 FG 路径同方向，配 mvecScale=(0.5,-0.5)
                vec2 mv = prevNdc - ndc.xy;
                outColor = vec4(d, mv, 1.0);
            }
            """;

    private int program;
    private int vao;
    private int vbo;
    private int auxTexture = -1;
    private int fbo = -1;
    private int width;
    private int height;

    private int uDepth;
    private int uInvViewProj;
    private int uPrevViewProj;

    private final FloatBuffer matBuf = org.lwjgl.system.MemoryUtil.memAllocFloat(16);

    public DepthMotionPass() {
        RenderSystem.assertOnRenderThread();
        program = buildProgram(VERT, FRAG);
        uDepth = GL20C.glGetUniformLocation(program, "uDepth");
        uInvViewProj = GL20C.glGetUniformLocation(program, "uInvViewProj");
        uPrevViewProj = GL20C.glGetUniformLocation(program, "uPrevViewProj");

        vao = GL30C.glGenVertexArrays();
        GL30C.glBindVertexArray(vao);
        vbo = GL15C.glGenBuffers();
        GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, vbo);
        float[] quad = {-1, -1, 3, -1, -1, 3};
        GL15C.glBufferData(GL15C.GL_ARRAY_BUFFER, quad, GL15C.GL_STATIC_DRAW);
        int aPos = GL20C.glGetAttribLocation(program, "aPos");
        GL20C.glEnableVertexAttribArray(aPos);
        GL20C.glVertexAttribPointer(aPos, 2, GL11C.GL_FLOAT, false, 0, 0);
        GL30C.glBindVertexArray(0);
        GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, 0);
    }

    /** 按需重建 aux 纹理与 FBO */
    public void resize(int w, int h) {
        RenderSystem.assertOnRenderThread();
        if (auxTexture >= 0 && w == this.width && h == this.height) return;
        destroyTargets();
        this.width = w;
        this.height = h;

        auxTexture = TextureUtil.generateTextureId();
        GlStateManager._bindTexture(auxTexture);
        GlStateManager._texParameter(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_MIN_FILTER, GL11C.GL_LINEAR);
        GlStateManager._texParameter(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_MAG_FILTER, GL11C.GL_LINEAR);
        GlStateManager._texParameter(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_WRAP_S, GL30C.GL_CLAMP_TO_EDGE);
        GlStateManager._texParameter(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_WRAP_T, GL30C.GL_CLAMP_TO_EDGE);
        GlStateManager._texImage2D(GL11C.GL_TEXTURE_2D, 0, GL30C.GL_RGBA16F, w, h, 0,
                GL11C.GL_RGBA, GL30C.GL_HALF_FLOAT, null);

        fbo = GlStateManager.glGenFramebuffers();
        GlStateManager._glBindFramebuffer(GL30C.GL_FRAMEBUFFER, fbo);
        GlStateManager._glFramebufferTexture2D(GL30C.GL_FRAMEBUFFER, GL30C.GL_COLOR_ATTACHMENT0,
                GL11C.GL_TEXTURE_2D, auxTexture, 0);
        int status = GL30C.glCheckFramebufferStatus(GL30C.GL_FRAMEBUFFER);
        int err = GL11C.glGetError();
        com.taolesi.dlssmc.core.FGRuntime.get()
                .setGlDiag("auxFbo=" + fbo + " tex=" + auxTexture + " status=0x"
                        + Integer.toHexString(status) + " err=" + err);
        GlStateManager._glBindFramebuffer(GL30C.GL_FRAMEBUFFER, 0);
    }

    public int getAuxTexture() {
        return auxTexture;
    }

    /**
     * @param depthTexture worldTarget 的深度纹理
     * @param invViewProj  当前帧 (proj*view) 的逆
     * @param prevViewProj 上一帧的 proj*view
     */
    public void run(int depthTexture, Matrix4f invViewProj, Matrix4f prevViewProj) {
        RenderSystem.assertOnRenderThread();
        if (auxTexture < 0 || fbo < 0) return;

        GlStateManager._glBindFramebuffer(GL30C.GL_FRAMEBUFFER, fbo);
        GL30C.glViewport(0, 0, width, height);

        GL20C.glUseProgram(program);
        GL30C.glBindVertexArray(vao);

        RenderSystem.activeTexture(GL13C.GL_TEXTURE0);
        GlStateManager._bindTexture(depthTexture);
        GL20C.glUniform1i(uDepth, 0);

        invViewProj.get(matBuf);
        GL20C.glUniformMatrix4fv(uInvViewProj, false, matBuf);
        prevViewProj.get(matBuf);
        GL20C.glUniformMatrix4fv(uPrevViewProj, false, matBuf);

        GL11C.glDrawArrays(GL11C.GL_TRIANGLES, 0, 3);

        GL30C.glBindVertexArray(0);
        GL20C.glUseProgram(0);
        GlStateManager._glBindFramebuffer(GL30C.GL_FRAMEBUFFER, 0);
        GlStateManager._bindTexture(0);
    }

    private static int buildProgram(String vert, String frag) {
        int v = compile(GL20C.GL_VERTEX_SHADER, vert);
        int f = compile(GL20C.GL_FRAGMENT_SHADER, frag);
        int p = GL20C.glCreateProgram();
        GL20C.glAttachShader(p, v);
        GL20C.glAttachShader(p, f);
        GL20C.glLinkProgram(p);
        if (GL20C.glGetProgrami(p, GL20C.GL_LINK_STATUS) == 0) {
            throw new IllegalStateException("着色器链接失败: " + GL20C.glGetProgramInfoLog(p));
        }
        GL20C.glDeleteShader(v);
        GL20C.glDeleteShader(f);
        return p;
    }

    private static int compile(int type, String src) {
        int s = GL20C.glCreateShader(type);
        GL20C.glShaderSource(s, src);
        GL20C.glCompileShader(s);
        if (GL20C.glGetShaderi(s, GL20C.GL_COMPILE_STATUS) == 0) {
            throw new IllegalStateException("着色器编译失败: " + GL20C.glGetShaderInfoLog(s));
        }
        return s;
    }

    private void destroyTargets() {
        if (auxTexture >= 0) {
            GlStateManager._deleteTexture(auxTexture);
            auxTexture = -1;
        }
        if (fbo >= 0) {
            GlStateManager._glDeleteFramebuffers(fbo);
            fbo = -1;
        }
    }

    @Override
    public void close() {
        destroyTargets();
        if (vbo >= 0) GL15C.glDeleteBuffers(vbo);
        if (vao >= 0) GL30C.glDeleteVertexArrays(vao);
        if (program != 0) GL20C.glDeleteProgram(program);
        org.lwjgl.system.MemoryUtil.memFree(matBuf);
    }
}
