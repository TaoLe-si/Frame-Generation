package com.taolesi.dlssmc.render;

import com.mojang.blaze3d.systems.RenderSystem;
import org.joml.Matrix4f;
import org.lwjgl.opengl.GL;
import org.lwjgl.opengl.GL11C;
import org.lwjgl.opengl.GL13C;
import org.lwjgl.opengl.GL15C;
import org.lwjgl.opengl.GL20C;
import org.lwjgl.opengl.GL21C;
import org.lwjgl.opengl.GL30C;
import org.lwjgl.opengl.GL32C;
import org.lwjgl.opengl.GL33C;
import org.lwjgl.system.MemoryStack;

import java.nio.ByteBuffer;
import java.nio.FloatBuffer;
import java.nio.IntBuffer;

/** 通过 MRT 生成 R32F 原始深度和 RG16F 相机运动向量。 */
public final class FGDepthMotionPass implements AutoCloseable {

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
            out float outDepth;
            out vec2 outMotion;
            uniform sampler2D uDepth;
            uniform mat4 uInvViewProj;
            uniform mat4 uPrevViewProj;
            uniform bool uZeroMotion;
            uniform sampler2D uPreHandDepth;
            uniform sampler2D uPostHandDepth;
            uniform bool uHasHandDepth;
            void main() {
                vec2 sourceUv = vec2(vUv.x, 1.0 - vUv.y);
                ivec2 sourcePixel = ivec2(sourceUv * vec2(textureSize(uDepth, 0)));
                float d = texelFetch(uDepth, sourcePixel, 0).r;
                outDepth = d;
                outMotion = vec2(0.0);
                if (uZeroMotion) return;
                if (uHasHandDepth) {
                    float beforeHand = texelFetch(uPreHandDepth, sourcePixel, 0).r;
                    float afterHand = texelFetch(uPostHandDepth, sourcePixel, 0).r;
                    // ponytail: 仅排除不透明手的相机运动；挥手/透明手需要独立物体运动向量。
                    if (afterHand < beforeHand && d == afterHand) return;
                }
                vec3 ndc = vec3(sourceUv * 2.0 - 1.0, d * 2.0 - 1.0);
                vec4 world = uInvViewProj * vec4(ndc, 1.0);
                if (d >= 1.0) {
                    // Sky follows rotation, not translation; homogeneous subtraction also supports an infinite far plane.
                    vec4 nearWorld = uInvViewProj * vec4(ndc.xy, -1.0, 1.0);
                    world = vec4(world.xyz * nearWorld.w - nearWorld.xyz * world.w, 0.0);
                } else {
                    world /= world.w;
                }
                vec4 prevClip = uPrevViewProj * world;
                if (prevClip.w <= 0.0) return;
                vec2 prevNdc = prevClip.xy / prevClip.w;
                // prev-current NDC；top-left 转换由 SL mvecScale=(0.5,-0.5) 完成。
                outMotion = prevNdc - ndc.xy;
            }
            """;

    private int program;
    private int vao;
    private int vbo;
    private int depthTexture = -1;
    private int motionTexture = -1;
    private int fbo = -1;
    private int width;
    private int height;

    private int uDepth;
    private int uInvViewProj;
    private int uPrevViewProj;
    private int uZeroMotion;
    private int uPreHandDepth;
    private int uPostHandDepth;
    private int uHasHandDepth;

    public FGDepthMotionPass() {
        RenderSystem.assertOnRenderThread();
        int oldVao = GL11C.glGetInteger(GL30C.GL_VERTEX_ARRAY_BINDING);
        int oldBuffer = GL11C.glGetInteger(GL15C.GL_ARRAY_BUFFER_BINDING);
        try {
            program = buildProgram(VERT, FRAG);
            uDepth = GL20C.glGetUniformLocation(program, "uDepth");
            uInvViewProj = GL20C.glGetUniformLocation(program, "uInvViewProj");
            uPrevViewProj = GL20C.glGetUniformLocation(program, "uPrevViewProj");
            uZeroMotion = GL20C.glGetUniformLocation(program, "uZeroMotion");
            uPreHandDepth = GL20C.glGetUniformLocation(program, "uPreHandDepth");
            uPostHandDepth = GL20C.glGetUniformLocation(program, "uPostHandDepth");
            uHasHandDepth = GL20C.glGetUniformLocation(program, "uHasHandDepth");

            vao = GL30C.glGenVertexArrays();
            GL30C.glBindVertexArray(vao);
            vbo = GL15C.glGenBuffers();
            GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, vbo);
            float[] quad = {-1, -1, 3, -1, -1, 3};
            GL15C.glBufferData(GL15C.GL_ARRAY_BUFFER, quad, GL15C.GL_STATIC_DRAW);
            int aPos = GL20C.glGetAttribLocation(program, "aPos");
            GL20C.glEnableVertexAttribArray(aPos);
            GL20C.glVertexAttribPointer(aPos, 2, GL11C.GL_FLOAT, false, 0, 0);
        } catch (RuntimeException | Error e) {
            close();
            throw e;
        } finally {
            GL30C.glBindVertexArray(oldVao);
            GL15C.glBindBuffer(GL15C.GL_ARRAY_BUFFER, oldBuffer);
        }
    }

    public void resize(int w, int h) {
        RenderSystem.assertOnRenderThread();
        if (w <= 0 || h <= 0) throw new IllegalArgumentException("无效的深度/运动纹理尺寸: " + w + "x" + h);
        if (depthTexture >= 0 && motionTexture >= 0 && w == width && h == height) return;
        int oldTexture = GL11C.glGetInteger(GL11C.GL_TEXTURE_BINDING_2D);
        int oldDrawFbo = GL11C.glGetInteger(GL30C.GL_DRAW_FRAMEBUFFER_BINDING);
        int oldReadFbo = GL11C.glGetInteger(GL30C.GL_READ_FRAMEBUFFER_BINDING);
        int oldUnpackBuffer = GL11C.glGetInteger(GL21C.GL_PIXEL_UNPACK_BUFFER_BINDING);
        int newDepth = depthTexture;
        int newMotion = motionTexture;
        int newFbo = fbo;
        width = height = 0;
        try {
            if (newDepth < 0) newDepth = GL11C.glGenTextures();
            if (newMotion < 0) newMotion = GL11C.glGenTextures();
            if (newFbo < 0) newFbo = GL30C.glGenFramebuffers();
            if (newDepth == 0 || newMotion == 0 || newFbo == 0) {
                throw new IllegalStateException("深度/运动 GL 资源创建失败");
            }
            GL30C.glBindFramebuffer(GL30C.GL_DRAW_FRAMEBUFFER, newFbo);
            GL15C.glBindBuffer(GL21C.GL_PIXEL_UNPACK_BUFFER, 0);
            for (int i = 0; i < 2; i++) {
                int texture = i == 0 ? newDepth : newMotion;
                GL11C.glBindTexture(GL11C.GL_TEXTURE_2D, texture);
                GL11C.glTexParameteri(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_MIN_FILTER, GL11C.GL_NEAREST);
                GL11C.glTexParameteri(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_MAG_FILTER, GL11C.GL_NEAREST);
                GL11C.glTexParameteri(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_WRAP_S, GL30C.GL_CLAMP_TO_EDGE);
                GL11C.glTexParameteri(GL11C.GL_TEXTURE_2D, GL11C.GL_TEXTURE_WRAP_T, GL30C.GL_CLAMP_TO_EDGE);
                GL11C.glTexImage2D(GL11C.GL_TEXTURE_2D, 0, i == 0 ? GL30C.GL_R32F : GL30C.GL_RG16F,
                        w, h, 0, i == 0 ? GL11C.GL_RED : GL30C.GL_RG, GL11C.GL_FLOAT, 0L);
                GL30C.glFramebufferTexture2D(GL30C.GL_DRAW_FRAMEBUFFER, GL30C.GL_COLOR_ATTACHMENT0 + i,
                        GL11C.GL_TEXTURE_2D, texture, 0);
            }
            GL20C.glDrawBuffers(new int[]{GL30C.GL_COLOR_ATTACHMENT0, GL30C.GL_COLOR_ATTACHMENT1});
            int status = GL30C.glCheckFramebufferStatus(GL30C.GL_DRAW_FRAMEBUFFER);
            int err = GL11C.glGetError();
            String diag = "depthMvFbo=" + newFbo + " depth=" + newDepth + " motion=" + newMotion
                    + " status=0x" + Integer.toHexString(status) + " err=" + err;
            com.taolesi.dlssmc.core.FGRuntime.get().setGlDiag(diag);
            if (status != GL30C.GL_FRAMEBUFFER_COMPLETE || err != GL11C.GL_NO_ERROR) {
                throw new IllegalStateException("深度/运动 FBO 创建失败: " + diag);
            }
            depthTexture = newDepth;
            motionTexture = newMotion;
            fbo = newFbo;
            width = w;
            height = h;
            newDepth = newMotion = newFbo = -1;
        } finally {
            GL30C.glBindFramebuffer(GL30C.GL_DRAW_FRAMEBUFFER, oldDrawFbo);
            GL30C.glBindFramebuffer(GL30C.GL_READ_FRAMEBUFFER, oldReadFbo);
            GL11C.glBindTexture(GL11C.GL_TEXTURE_2D, oldTexture);
            GL15C.glBindBuffer(GL21C.GL_PIXEL_UNPACK_BUFFER, oldUnpackBuffer);
            if (newDepth >= 0 && newDepth != depthTexture) GL11C.glDeleteTextures(newDepth);
            if (newMotion >= 0 && newMotion != motionTexture) GL11C.glDeleteTextures(newMotion);
            if (newFbo >= 0 && newFbo != fbo) GL30C.glDeleteFramebuffers(newFbo);
        }
    }

    public int getDepthTexture() {
        return depthTexture;
    }

    public int getMotionTexture() {
        return motionTexture;
    }

    /**
     * @param depthTexture worldTarget 的深度纹理
     * @param invViewProj  当前帧 (proj*view) 的逆
     * @param prevViewProj 上一帧的 proj*view
     * @param zeroMotion   仅将运动向量置零，保留深度写入
     */
    public void run(int depthTexture, int preHandDepth, int postHandDepth,
                    Matrix4f invViewProj, Matrix4f prevViewProj, boolean zeroMotion) {
        RenderSystem.assertOnRenderThread();
        if (this.depthTexture < 0 || motionTexture < 0 || fbo < 0 || width <= 0 || height <= 0) return;

        try (MemoryStack stack = MemoryStack.stackPush()) {
            int oldProgram = GL11C.glGetInteger(GL20C.GL_CURRENT_PROGRAM);
            int oldVao = GL11C.glGetInteger(GL30C.GL_VERTEX_ARRAY_BINDING);
            int oldDrawFbo = GL11C.glGetInteger(GL30C.GL_DRAW_FRAMEBUFFER_BINDING);
            int oldReadFbo = GL11C.glGetInteger(GL30C.GL_READ_FRAMEBUFFER_BINDING);
            int oldActiveTexture = GL11C.glGetInteger(GL13C.GL_ACTIVE_TEXTURE);
            IntBuffer viewport = stack.mallocInt(4);
            GL11C.glGetIntegerv(GL11C.GL_VIEWPORT, viewport);
            IntBuffer polygonMode = stack.mallocInt(2);
            GL11C.glGetIntegerv(GL11C.GL_POLYGON_MODE, polygonMode);
            ByteBuffer colorMask0 = stack.malloc(4);
            ByteBuffer colorMask1 = stack.malloc(4);
            GL30C.glGetBooleani_v(GL11C.GL_COLOR_WRITEMASK, 0, colorMask0);
            GL30C.glGetBooleani_v(GL11C.GL_COLOR_WRITEMASK, 1, colorMask1);
            boolean blend0 = GL30C.glIsEnabledi(GL11C.GL_BLEND, 0);
            boolean blend1 = GL30C.glIsEnabledi(GL11C.GL_BLEND, 1);
            int[] caps = {GL11C.GL_DEPTH_TEST, GL11C.GL_STENCIL_TEST, GL11C.GL_SCISSOR_TEST,
                    GL11C.GL_CULL_FACE, GL30C.GL_RASTERIZER_DISCARD, GL11C.GL_COLOR_LOGIC_OP,
                    GL13C.GL_SAMPLE_ALPHA_TO_COVERAGE, GL13C.GL_SAMPLE_COVERAGE, GL32C.GL_SAMPLE_MASK};
            boolean[] enabled = new boolean[caps.length];
            for (int i = 0; i < caps.length; i++) enabled[i] = GL11C.glIsEnabled(caps[i]);
            boolean[] clipEnabled = new boolean[GL11C.glGetInteger(GL30C.GL_MAX_CLIP_DISTANCES)];
            for (int i = 0; i < clipEnabled.length; i++) {
                clipEnabled[i] = GL11C.glIsEnabled(GL30C.GL_CLIP_DISTANCE0 + i);
            }
            FloatBuffer matBuf = stack.mallocFloat(16);
            boolean hasSamplers = GL.getCapabilities().OpenGL33 || GL.getCapabilities().GL_ARB_sampler_objects;
            boolean hasHandDepth = preHandDepth > 0 && postHandDepth > 0;
            int textureCount = hasHandDepth ? 3 : 1;
            IntBuffer oldTextures = stack.mallocInt(textureCount);
            IntBuffer oldSamplers = stack.mallocInt(textureCount);
            for (int i = 0; i < textureCount; i++) {
                GL13C.glActiveTexture(GL13C.GL_TEXTURE0 + i);
                oldTextures.put(i, GL11C.glGetInteger(GL11C.GL_TEXTURE_BINDING_2D));
                oldSamplers.put(i, hasSamplers ? GL11C.glGetInteger(GL33C.GL_SAMPLER_BINDING) : 0);
            }
            try {
                GL30C.glBindFramebuffer(GL30C.GL_DRAW_FRAMEBUFFER, fbo);
                GL11C.glViewport(0, 0, width, height);
                for (int cap : caps) GL11C.glDisable(cap);
                for (int i = 0; i < clipEnabled.length; i++) GL11C.glDisable(GL30C.GL_CLIP_DISTANCE0 + i);
                GL30C.glDisablei(GL11C.GL_BLEND, 0);
                GL30C.glDisablei(GL11C.GL_BLEND, 1);
                GL30C.glColorMaski(0, true, true, true, true);
                GL30C.glColorMaski(1, true, true, true, true);
                GL11C.glPolygonMode(GL11C.GL_FRONT_AND_BACK, GL11C.GL_FILL);
                GL20C.glUseProgram(program);
                GL30C.glBindVertexArray(vao);
                for (int i = 0; i < textureCount; i++) {
                    GL13C.glActiveTexture(GL13C.GL_TEXTURE0 + i);
                    if (hasSamplers) GL33C.glBindSampler(i, 0);
                    GL11C.glBindTexture(GL11C.GL_TEXTURE_2D,
                            i == 0 ? depthTexture : i == 1 ? preHandDepth : postHandDepth);
                }
                GL20C.glUniform1i(uDepth, 0);
                GL20C.glUniform1i(uPreHandDepth, hasHandDepth ? 1 : 0);
                GL20C.glUniform1i(uPostHandDepth, hasHandDepth ? 2 : 0);
                GL20C.glUniform1i(uHasHandDepth, hasHandDepth ? 1 : 0);
                invViewProj.get(matBuf);
                GL20C.glUniformMatrix4fv(uInvViewProj, false, matBuf);
                prevViewProj.get(matBuf);
                GL20C.glUniformMatrix4fv(uPrevViewProj, false, matBuf);
                GL20C.glUniform1i(uZeroMotion, zeroMotion ? 1 : 0);
                GL11C.glDrawArrays(GL11C.GL_TRIANGLES, 0, 3);
            } finally {
                for (int i = 0; i < textureCount; i++) {
                    GL13C.glActiveTexture(GL13C.GL_TEXTURE0 + i);
                    GL11C.glBindTexture(GL11C.GL_TEXTURE_2D, oldTextures.get(i));
                    if (hasSamplers) GL33C.glBindSampler(i, oldSamplers.get(i));
                }
                GL13C.glActiveTexture(oldActiveTexture);
                GL20C.glUseProgram(oldProgram);
                GL30C.glBindVertexArray(oldVao);
                GL30C.glBindFramebuffer(GL30C.GL_DRAW_FRAMEBUFFER, oldDrawFbo);
                GL30C.glBindFramebuffer(GL30C.GL_READ_FRAMEBUFFER, oldReadFbo);
                GL11C.glViewport(viewport.get(0), viewport.get(1), viewport.get(2), viewport.get(3));
                for (int i = 0; i < caps.length; i++) {
                    if (enabled[i]) GL11C.glEnable(caps[i]);
                    else GL11C.glDisable(caps[i]);
                }
                for (int i = 0; i < clipEnabled.length; i++) {
                    if (clipEnabled[i]) GL11C.glEnable(GL30C.GL_CLIP_DISTANCE0 + i);
                    else GL11C.glDisable(GL30C.GL_CLIP_DISTANCE0 + i);
                }
                if (blend0) GL30C.glEnablei(GL11C.GL_BLEND, 0);
                else GL30C.glDisablei(GL11C.GL_BLEND, 0);
                if (blend1) GL30C.glEnablei(GL11C.GL_BLEND, 1);
                else GL30C.glDisablei(GL11C.GL_BLEND, 1);
                GL30C.glColorMaski(0, colorMask0.get(0) != 0, colorMask0.get(1) != 0,
                        colorMask0.get(2) != 0, colorMask0.get(3) != 0);
                GL30C.glColorMaski(1, colorMask1.get(0) != 0, colorMask1.get(1) != 0,
                        colorMask1.get(2) != 0, colorMask1.get(3) != 0);
                GL11C.glPolygonMode(GL11C.GL_FRONT_AND_BACK, polygonMode.get(0));
            }
        }
    }

    private static int buildProgram(String vert, String frag) {
        int v = 0;
        int f = 0;
        int p = 0;
        try {
            v = compile(GL20C.GL_VERTEX_SHADER, vert);
            f = compile(GL20C.GL_FRAGMENT_SHADER, frag);
            p = GL20C.glCreateProgram();
            GL20C.glAttachShader(p, v);
            GL20C.glAttachShader(p, f);
            GL30C.glBindFragDataLocation(p, 0, "outDepth");
            GL30C.glBindFragDataLocation(p, 1, "outMotion");
            GL20C.glLinkProgram(p);
            if (GL20C.glGetProgrami(p, GL20C.GL_LINK_STATUS) == 0) {
                throw new IllegalStateException("着色器链接失败: " + GL20C.glGetProgramInfoLog(p));
            }
            return p;
        } catch (RuntimeException | Error e) {
            if (p != 0) GL20C.glDeleteProgram(p);
            throw e;
        } finally {
            if (v != 0) GL20C.glDeleteShader(v);
            if (f != 0) GL20C.glDeleteShader(f);
        }
    }

    private static int compile(int type, String src) {
        int s = GL20C.glCreateShader(type);
        try {
            GL20C.glShaderSource(s, src);
            GL20C.glCompileShader(s);
            if (GL20C.glGetShaderi(s, GL20C.GL_COMPILE_STATUS) == 0) {
                throw new IllegalStateException("着色器编译失败 (type=" + type + "): " + GL20C.glGetShaderInfoLog(s));
            }
            return s;
        } catch (RuntimeException | Error e) {
            GL20C.glDeleteShader(s);
            throw e;
        }
    }

    private void destroyTargets() {
        if (depthTexture >= 0) {
            GL11C.glDeleteTextures(depthTexture);
            depthTexture = -1;
        }
        if (motionTexture >= 0) {
            GL11C.glDeleteTextures(motionTexture);
            motionTexture = -1;
        }
        if (fbo >= 0) {
            GL30C.glDeleteFramebuffers(fbo);
            fbo = -1;
        }
    }

    @Override
    public void close() {
        RenderSystem.assertOnRenderThread();
        destroyTargets();
        if (vbo != 0) GL15C.glDeleteBuffers(vbo);
        if (vao != 0) GL30C.glDeleteVertexArrays(vao);
        if (program != 0) GL20C.glDeleteProgram(program);
        vbo = vao = program = 0;
    }
}
