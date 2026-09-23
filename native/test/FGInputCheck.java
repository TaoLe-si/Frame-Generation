import com.mojang.blaze3d.systems.RenderSystem;
import com.taolesi.dlssmc.core.FGRuntime;
import com.taolesi.dlssmc.render.FGDepthMotionPass;
import org.joml.Matrix4f;
import org.lwjgl.glfw.GLFW;
import org.lwjgl.opengl.GL;
import org.lwjgl.opengl.GL45C;

import java.util.Arrays;

public class FGInputCheck {
    static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    static void near(float actual, float expected) {
        check(Math.abs(actual - expected) < 0.002f, actual + " != " + expected);
    }

    static int texture(int format) {
        int texture = GL45C.glCreateTextures(GL45C.GL_TEXTURE_2D);
        GL45C.glTextureStorage2D(texture, 1, format, 2, 2);
        GL45C.glTextureParameteri(texture, GL45C.GL_TEXTURE_MIN_FILTER, GL45C.GL_NEAREST);
        GL45C.glTextureParameteri(texture, GL45C.GL_TEXTURE_MAG_FILTER, GL45C.GL_NEAREST);
        return texture;
    }

    static float[] read(int texture, int format, int size) {
        float[] values = new float[size];
        GL45C.glGetTextureImage(texture, 0, format, GL45C.GL_FLOAT, values);
        return values;
    }

    static void motion(FGDepthMotionPass pass, int source, Matrix4f previous, float x, float y, boolean zeroMotion) {
        pass.run(source, 0, 0, new Matrix4f(), previous, zeroMotion);
        check(GL45C.glGetError() == GL45C.GL_NO_ERROR, "motion pass OpenGL error");
        float[] result = read(pass.getMotionTexture(), GL45C.GL_RG, 8);
        for (int i = 0; i < 8; i += 2) {
            near(result[i], x);
            near(result[i + 1], y);
        }
    }

    static void skyCameraMotion(FGDepthMotionPass pass, int source) {
        Matrix4f projection = new Matrix4f().perspective((float) Math.toRadians(70), 1f, 0.05f, 1000f);
        float previousAngle = 0;
        for (float angle : new float[]{0f, 0.14f, -0.10f}) {
            Matrix4f current = new Matrix4f(projection).rotateY(angle).translate(-30, -4, 50);
            Matrix4f previous = new Matrix4f(projection).rotateY(previousAngle).translate(-18, -4, -70);
            pass.run(source, 0, 0, current.invert(), previous, false);
            float[] motion = read(pass.getMotionTexture(), GL45C.GL_RG, 8);
            double delta = previousAngle - angle;
            double c = Math.cos(delta), s = Math.sin(delta);
            for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) {
                float nx = x - 0.5f, ny = 0.5f - y;
                double rayX = nx / projection.m00();
                double denominator = s * rayX + c;
                float expectedX = (float) (projection.m00() * (c * rayX - s) / denominator) - nx;
                float expectedY = (float) (ny / denominator) - ny;
                int i = (y * 2 + x) * 2;
                check(Math.abs(motion[i] - expectedX) < 0.002f
                                && Math.abs(motion[i + 1] - expectedY) < 0.002f,
                        "sky must follow camera rotation, not translation: angle=" + angle
                                + " actual=" + motion[i] + "," + motion[i + 1]
                                + " expected=" + expectedX + "," + expectedY);
            }
            for (float depth : read(pass.getDepthTexture(), GL45C.GL_RED, 4)) near(depth, 1);
            previousAngle = angle;
        }
        System.out.println("PASS: consecutive yaw frames keep sky rotational motion and reject camera translation");
    }

    static void handMotion(FGDepthMotionPass pass, int source) {
        int depthFormat = GL45C.glGetTextureLevelParameteri(source, 0, GL45C.GL_TEXTURE_INTERNAL_FORMAT);
        int uploadFormat = depthFormat == GL45C.GL_R32F ? GL45C.GL_RED : GL45C.GL_DEPTH_COMPONENT;
        int preHand = texture(depthFormat), postHand = texture(depthFormat);
        int oldActive = GL45C.glGetInteger(GL45C.GL_ACTIVE_TEXTURE);
        int[] oldTextures = new int[3], oldSamplers = new int[3], samplers = new int[3];
        int[] textures = {preHand, postHand, source};
        for (int i = 0; i < 3; i++) {
            GL45C.glActiveTexture(GL45C.GL_TEXTURE0 + i);
            oldTextures[i] = GL45C.glGetInteger(GL45C.GL_TEXTURE_BINDING_2D);
            oldSamplers[i] = GL45C.glGetInteger(GL45C.GL_SAMPLER_BINDING);
            samplers[i] = GL45C.glCreateSamplers();
            GL45C.glSamplerParameteri(samplers[i], GL45C.GL_TEXTURE_COMPARE_MODE, GL45C.GL_COMPARE_REF_TO_TEXTURE);
            GL45C.glBindTexture(GL45C.GL_TEXTURE_2D, textures[i]);
            GL45C.glBindSampler(i, samplers[i]);
        }
        try {
            GL45C.glTextureParameteri(preHand, GL45C.GL_TEXTURE_MIN_FILTER, GL45C.GL_LINEAR);
            GL45C.glTextureParameteri(postHand, GL45C.GL_TEXTURE_MAG_FILTER, GL45C.GL_LINEAR);
            GL45C.glTextureSubImage2D(preHand, 0, 0, 0, 2, 2, uploadFormat, GL45C.GL_FLOAT,
                    new float[]{0.8f, 0.8f, 0.05f, 0.8f});
            GL45C.glTextureSubImage2D(postHand, 0, 0, 0, 2, 2, uploadFormat, GL45C.GL_FLOAT,
                    new float[]{0.4f, 0.4f, 0.05f, 0.8f});
            GL45C.glTextureSubImage2D(source, 0, 0, 0, 2, 2, uploadFormat, GL45C.GL_FLOAT,
                    new float[]{0.4f, 0.2f, 0.05f, 0.6f});
            Matrix4f cameraMotion = new Matrix4f().translation(0.2f, 0.4f, 0);
            pass.run(source, preHand, postHand, new Matrix4f(), cameraMotion, false);
            check(GL45C.glGetInteger(GL45C.GL_ACTIVE_TEXTURE) == GL45C.GL_TEXTURE2, "active texture changed");
            for (int i = 0; i < 3; i++) {
                GL45C.glActiveTexture(GL45C.GL_TEXTURE0 + i);
                check(GL45C.glGetInteger(GL45C.GL_TEXTURE_BINDING_2D) == textures[i], "texture binding " + i);
                check(GL45C.glGetInteger(GL45C.GL_SAMPLER_BINDING) == samplers[i], "sampler binding " + i);
            }
            float[] mv = read(pass.getMotionTexture(), GL45C.GL_RG, 8);
            near(mv[4], 0); near(mv[5], 0);
            for (int i : new int[]{0, 2, 6}) {
                near(mv[i], 0.2f); near(mv[i + 1], 0.4f);
            }
            float[] depth = read(pass.getDepthTexture(), GL45C.GL_RED, 4);
            float[] expectedDepth = {0.05f, 0.6f, 0.4f, 0.2f};
            for (int i = 0; i < 4; i++) near(depth[i], expectedDepth[i]);
            pass.run(source, preHand, postHand, new Matrix4f(), cameraMotion, true);
            for (float value : read(pass.getMotionTexture(), GL45C.GL_RG, 8)) near(value, 0);
            motion(pass, source, cameraMotion, 0.2f, 0.4f, false);
            GL45C.glTextureSubImage2D(postHand, 0, 0, 0, 2, 2, uploadFormat, GL45C.GL_FLOAT,
                    new float[]{0.8f, 0.8f, 0.05f, 0.8f});
            pass.run(source, preHand, postHand, new Matrix4f(), cameraMotion, false);
            mv = read(pass.getMotionTexture(), GL45C.GL_RG, 8);
            for (int i = 0; i < 8; i += 2) {
                near(mv[i], 0.2f); near(mv[i + 1], 0.4f);
            }
            check(GL45C.glGetError() == GL45C.GL_NO_ERROR, "hand motion OpenGL error");
        } finally {
            for (int i = 0; i < 3; i++) {
                GL45C.glActiveTexture(GL45C.GL_TEXTURE0 + i);
                GL45C.glBindTexture(GL45C.GL_TEXTURE_2D, oldTextures[i]);
                GL45C.glBindSampler(i, oldSamplers[i]);
                GL45C.glDeleteSamplers(samplers[i]);
            }
            GL45C.glActiveTexture(oldActive);
            GL45C.glDeleteTextures(preHand);
            GL45C.glDeleteTextures(postHand);
        }
    }

    static void worldTargets() throws Exception {
        // Forge 1.20.1 实机跑的是 SRG 名的 MC，所以这里直接按 SRG 名调
        // （m_83947_ = bindWrite，m_83930_ = destroyBuffers，f_83920_ = frameBufferId，
        //  m_83975_ = getColorTextureId，m_83980_ = getDepthTextureId）。
        // enableStencil / isStencilEnabled 是 Forge 后加的，没有 SRG 名。
        var constructor = Class.forName("com.taolesi.dlssmc.core.FGRuntime$WorldTarget")
                .getDeclaredConstructor(int.class, int.class, boolean.class);
        constructor.setAccessible(true);
        var plain = new com.mojang.blaze3d.pipeline.MainTarget(4, 3);
        int previousVersion = 0;
        try {
            GL45C.glViewport(0, 0, 16, 12);
            plain.m_83947_(false);
            int[] viewport = new int[4];
            GL45C.glGetIntegerv(GL45C.GL_VIEWPORT, viewport);
            check(!Arrays.equals(viewport, new int[]{0, 0, 4, 3}),
                    "negative control: ordinary MainTarget must reproduce the stale viewport");
            for (int width : new int[]{4, 7, 4}) {
                var target = (com.mojang.blaze3d.pipeline.MainTarget) constructor.newInstance(width, 3, false);
                try {
                    GL45C.glViewport(0, 0, 16, 12);
                    target.m_83947_(false);
                    GL45C.glGetIntegerv(GL45C.GL_VIEWPORT, viewport);
                    check(Arrays.equals(viewport, new int[]{0, 0, width, 3}), "world rebind must restore viewport");
                    check(GL45C.glGetInteger(GL45C.GL_DRAW_FRAMEBUFFER_BINDING) == target.f_83920_,
                            "world rebind must preserve its framebuffer");
                    var iris = (net.irisshaders.iris.targets.Blaze3dRenderTargetExt) target;
                    int version = iris.iris$getDepthBufferVersion();
                    check(version < 0 && version != previousVersion,
                            "Iris must distinguish replacement world targets and the ordinary main target");
                    check(iris.iris$getColorBufferVersion() == version, "color attachment version mismatch");
                    previousVersion = version;
                } finally {
                    target.m_83930_();
                }
            }
        } finally {
            plain.m_83930_();
        }
        check(GL45C.glGetError() == GL45C.GL_NO_ERROR, "world target OpenGL error");
        System.out.println("PASS: production world target rebind viewport (with failing plain-target control), replacement Iris depth/color versions");
    }

    /** 把一份内存配置装进 ForgeConfigSpec：correct 补默认值，setConfig 让 ConfigValue.get() 不再抛。 */
    static void loadInMemory(net.minecraftforge.common.ForgeConfigSpec spec) {
        var config = com.electronwill.nightconfig.core.CommentedConfig.inMemory();
        spec.correct(config);
        spec.setConfig(config);
    }

    static void irisAttachments() throws Exception {
        // 独立 JVM 里没有 FML 配置加载流程，自己把内存配置塞进 SPEC，
        // 否则 FGRuntime 读配置项会拿到未加载的 spec。
        var dsa = net.irisshaders.iris.gl.IrisRenderSystem.class.getDeclaredField("dsaState");
        dsa.setAccessible(true);
        dsa.set(null, new net.irisshaders.iris.gl.IrisRenderSystem.DSACore());
        var constructor = Class.forName("com.taolesi.dlssmc.core.FGRuntime$WorldTarget")
                .getDeclaredConstructor(int.class, int.class, boolean.class);
        constructor.setAccessible(true);
        var clientSpec = net.minecraftforge.common.ForgeConfig.class.getDeclaredField("clientSpec");
        clientSpec.setAccessible(true);
        loadInMemory((net.minecraftforge.common.ForgeConfigSpec) clientSpec.get(null));
        loadInMemory(com.taolesi.dlssmc.config.DLSSConfig.SPEC);
        for (boolean stencil : new boolean[]{false, true}) {
            var main = new com.mojang.blaze3d.pipeline.MainTarget(16, 12);
            if (stencil) main.enableStencil();
            var framebuffer = new net.irisshaders.iris.gl.framebuffer.GlFramebuffer();
            try {
                framebuffer.addColorAttachment(0, main.m_83975_());
                framebuffer.addDepthAttachment(main.m_83980_());
                check(framebuffer.getStatus() == GL45C.GL_FRAMEBUFFER_COMPLETE, "initial Iris framebuffer");
                if (stencil) {
                    var plain = new com.mojang.blaze3d.pipeline.MainTarget(8, 6);
                    try {
                        framebuffer.addDepthAttachment(plain.m_83980_());
                        int status = framebuffer.getStatus();
                        check(status != GL45C.GL_FRAMEBUFFER_COMPLETE, "negative control must reject mixed depth/stencil");
                        GL45C.glClear(GL45C.GL_DEPTH_BUFFER_BIT);
                        check(GL45C.glGetError() == GL45C.GL_INVALID_FRAMEBUFFER_OPERATION,
                                "negative control must reproduce game GL1286");
                        System.out.println("PASS: old stencil mismatch reproduces status=0x"
                                + Integer.toHexString(status) + " and GL1286");
                    } finally {
                        framebuffer.addDepthAttachment(main.m_83980_());
                        net.irisshaders.iris.texture.TextureInfoCache.INSTANCE.onDeleteTexture(plain.m_83980_());
                        plain.m_83930_();
                    }
                }
                for (int width : new int[]{8, 16, 10, 8}) {
                    var world = (com.mojang.blaze3d.pipeline.MainTarget)
                            constructor.newInstance(width, 6, main.isStencilEnabled());
                    try {
                        framebuffer.addDepthAttachment(world.m_83980_());
                        check(framebuffer.getStatus() == GL45C.GL_FRAMEBUFFER_COMPLETE,
                                "SR world target breaks Iris stencil framebuffer");
                        GL45C.glClearDepth(0.375);
                        GL45C.glClear(GL45C.GL_DEPTH_BUFFER_BIT);
                        float[] depth = new float[1];
                        GL45C.glReadPixels(0, 0, 1, 1, GL45C.GL_DEPTH_COMPONENT, GL45C.GL_FLOAT, depth);
                        near(depth[0], 0.375f);
                        check(GL45C.glGetError() == GL45C.GL_NO_ERROR, "Iris world depth write/read");
                    } finally {
                        framebuffer.addDepthAttachment(main.m_83980_());
                        // 独立 JVM 无删除纹理的 Mixin 回调，显式调用同一缓存失效入口。
                        net.irisshaders.iris.texture.TextureInfoCache.INSTANCE.onDeleteTexture(world.m_83980_());
                        world.m_83930_();
                    }
                    check(framebuffer.getStatus() == GL45C.GL_FRAMEBUFFER_COMPLETE, "SR off restores Iris framebuffer");
                }
            } finally {
                framebuffer.destroy();
                net.irisshaders.iris.texture.TextureInfoCache.INSTANCE.onDeleteTexture(main.m_83980_());
                main.m_83930_();
            }
        }
        System.out.println("PASS: real Iris attachments survive SR/AA/resize/OFF with and without stencil");
    }

    public static void main(String[] args) throws Exception {
        check(GLFW.glfwInit(), "GLFW init failed");
        GLFW.glfwWindowHint(GLFW.GLFW_VISIBLE, GLFW.GLFW_FALSE);
        GLFW.glfwWindowHint(GLFW.GLFW_CONTEXT_VERSION_MAJOR, 4);
        GLFW.glfwWindowHint(GLFW.GLFW_CONTEXT_VERSION_MINOR, 5);
        long window = GLFW.glfwCreateWindow(32, 32, "FG input check", 0, 0);
        check(window != 0, "OpenGL context unavailable");
        try {
            GLFW.glfwMakeContextCurrent(window);
            GL.createCapabilities();
            RenderSystem.initRenderThread();
            System.out.println("GPU: " + GL45C.glGetString(GL45C.GL_RENDERER));
            worldTargets();
            irisAttachments();
            int source = texture(GL45C.GL_R32F);
            GL45C.glTextureSubImage2D(source, 0, 0, 0, 2, 2, GL45C.GL_RED, GL45C.GL_FLOAT,
                    new float[]{0.2f, 0.4f, 0.6f, 0.8f});
            try (FGDepthMotionPass pass = new FGDepthMotionPass()) {
                pass.resize(2, 2);
                check(GL45C.glGetTextureLevelParameteri(pass.getDepthTexture(), 0,
                        GL45C.GL_TEXTURE_INTERNAL_FORMAT) == GL45C.GL_R32F, "depth format");
                check(GL45C.glGetTextureLevelParameteri(pass.getMotionTexture(), 0,
                        GL45C.GL_TEXTURE_INTERNAL_FORMAT) == GL45C.GL_RG16F, "motion format");
                GL45C.glViewport(3, 4, 17, 19);
                GL45C.glEnable(GL45C.GL_SCISSOR_TEST);
                GL45C.glScissor(0, 0, 0, 0);
                GL45C.glEnable(GL45C.GL_BLEND);
                GL45C.glEnable(GL45C.GL_DEPTH_TEST);
                GL45C.glColorMask(false, false, false, false);
                motion(pass, source, new Matrix4f(), 0, 0, false);
                int[] viewport = new int[4];
                GL45C.glGetIntegerv(GL45C.GL_VIEWPORT, viewport);
                check(Arrays.equals(viewport, new int[]{3, 4, 17, 19}), "viewport changed");
                check(GL45C.glIsEnabled(GL45C.GL_SCISSOR_TEST), "scissor changed");
                check(GL45C.glIsEnabled(GL45C.GL_BLEND), "blend changed");
                check(GL45C.glIsEnabled(GL45C.GL_DEPTH_TEST), "depth state changed");
                float[] depth = read(pass.getDepthTexture(), GL45C.GL_RED, 4);
                float[] expected = {0.6f, 0.8f, 0.2f, 0.4f};
                for (int i = 0; i < 4; ++i) near(depth[i], expected[i]);
                motion(pass, source, new Matrix4f().translation(0.2f, 0.4f, 0), 0.2f, 0.4f, false);
                motion(pass, source, new Matrix4f().translation(0.2f, 0.4f, 0), 0, 0, true);
                depth = read(pass.getDepthTexture(), GL45C.GL_RED, 4);
                for (int i = 0; i < 4; ++i) near(depth[i], expected[i]);
                motion(pass, source, new Matrix4f().translation(0.2f, 0.4f, 0), 0.2f, 0.4f, false);
                motion(pass, source, new Matrix4f().m33(-1), 0, 0, false);
                GL45C.glTextureSubImage2D(source, 0, 0, 0, 2, 2, GL45C.GL_RED, GL45C.GL_FLOAT,
                        new float[]{1, 1, 1, 1});
                motion(pass, source, new Matrix4f().translation(0.2f, 0.4f, 0), 0, 0, false);
                skyCameraMotion(pass, source);
                pass.resize(3, 3);
                pass.resize(2, 2);
                handMotion(pass, source);
                for (int format : new int[]{GL45C.GL_DEPTH_COMPONENT24, GL45C.GL_DEPTH_COMPONENT32F}) {
                    int depthSource = texture(format);
                    try {
                        handMotion(pass, depthSource);
                    } finally {
                        GL45C.glDeleteTextures(depthSource);
                    }
                }
            }
            GL45C.glDeleteTextures(source);
            FGRuntime fg = FGRuntime.get();
            for (String field : new String[]{"copyReadFbo", "copyDrawFbo"}) {
                var f = FGRuntime.class.getDeclaredField(field);
                f.setAccessible(true);
                f.setInt(fg, GL45C.glCreateFramebuffers());
            }
            int color = texture(GL45C.GL_RGBA8), flipped = texture(GL45C.GL_RGBA8);
            GL45C.glTextureSubImage2D(color, 0, 0, 0, 2, 2, GL45C.GL_RGBA, GL45C.GL_FLOAT,
                    new float[]{1,0,0,1, 1,0,0,1, 0,1,0,1, 0,1,0,1});
            var copy = FGRuntime.class.getDeclaredMethod("copyImage", int.class, int.class, int.class, int.class);
            copy.setAccessible(true);
            copy.invoke(fg, color, flipped, 2, 2);
            float[] pixels = read(flipped, GL45C.GL_RGBA, 16);
            near(pixels[0], 0); near(pixels[1], 1);
            near(pixels[8], 1); near(pixels[9], 0);
            check(GL45C.glIsEnabled(GL45C.GL_SCISSOR_TEST), "copy changed scissor");
            check(GL45C.glGetError() == GL45C.GL_NO_ERROR, "OpenGL error");
            GL45C.glDeleteTextures(color);
            GL45C.glDeleteTextures(flipped);
            fg.shutdown();
            System.out.println("PASS: production color flip, MRT formats/depth flip, stationary/translated/sky/behind-camera motion, zero-motion toggle/depth preserved, opaque-hand camera motion isolation, near-world/translucent-occluder/hand-removed/no-shader cases, 3 texture/sampler bindings, GL state, resize");
        } finally {
            GLFW.glfwDestroyWindow(window);
            GLFW.glfwTerminate();
        }
    }
}
