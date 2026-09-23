import com.electronwill.nightconfig.core.CommentedConfig;
import com.mojang.blaze3d.systems.RenderSystem;
import com.taolesi.dlssmc.config.DLSSConfig;
import com.taolesi.dlssmc.core.FGRuntime;
import com.taolesi.dlssmc.nativebridge.DLSSFGNative;
import com.taolesi.dlssmc.render.FGDepthMotionPass;
import org.joml.Matrix4f;
import org.lwjgl.glfw.GLFW;
import org.lwjgl.glfw.GLFWNativeWin32;
import org.lwjgl.opengl.GL;
import org.lwjgl.opengl.GL45C;

import java.lang.reflect.Method;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.LinkedHashSet;
import java.util.regex.Pattern;

/** Hidden, real Vulkan SR/FG regression. Run each init order in a fresh JVM; never launch Minecraft. */
public class SrInitOrderCheck {
    static final String PLUGINS = "D:/Backup/Downloads/streamline-sdk-v2.14.1/bin/x64";
    // GL bottom-left, bottom-right, top-left, top-right. Unequal colors expose both flips and stale frames.
    static final float[][] COLORS = {
            {0.12f, 0.30f, 0.82f, 1}, {0.85f, 0.18f, 0.25f, 1},
            {0.85f, 0.75f, 0.14f, 1}, {0.18f, 0.75f, 0.35f, 1}};
    static final float[] HUD = {0.95f, 0.15f, 0.85f, 1};
    static final float[] POISON = {0.02f, 0.02f, 0.02f, 1};
    static final float[] matrices = new float[64];
    static final float[] params = {0.05f, 1000, (float) Math.toRadians(70), 16f / 9, 0, 0, 0.5f, -0.5f};
    static final int[] textures = new int[6]; // final, hudless, depth, motion, world, SR output
    static int width = 1280, height = 720, rw, rh, quality;
    static int worldSource, depthSource, mainColor, readFbo, copyRead, copyDraw;
    static long window;
    static boolean bound, inputsLocked, outputLocked;
    static FGDepthMotionPass depthMotion;
    static Method copyImage;

    static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    static void ok(int result, String label) {
        check(result == 0, label + " returned " + result);
    }

    static void fails(int result, String label) {
        check(result != 0, label + " unexpectedly succeeded");
        System.out.println("Expected rejection: " + label + " -> " + result);
    }

    static void routing(DLSSConfig.Backend backend, int frames, int sr, String label) {
        check(DLSSConfig.activeBackend() == backend, label + ": backend=" + DLSSConfig.activeBackend());
        check(DLSSConfig.activeFramesToGenerate() == frames, label + ": FG frames");
        check(DLSSConfig.activeSrQuality() == sr, label + ": SR=" + DLSSConfig.activeSrQuality());
    }

    static void routingOnly() throws Exception {
        // No FileConfig, NeoForgeConfig, save(), or game bootstrap: only this spec in memory.
        var config = CommentedConfig.inMemory();
        DLSSConfig.SPEC.correct(config);
        var loaded = Class.forName("net.neoforged.fml.config.LoadedConfig").getDeclaredConstructor(
                CommentedConfig.class, Path.class, net.neoforged.fml.config.ModConfig.class);
        loaded.setAccessible(true);
        DLSSConfig.SPEC.acceptConfig((net.neoforged.fml.config.IConfigSpec.ILoadedConfig)
                loaded.newInstance(config, null, null));
        DLSSConfig.MODE.set(DLSSConfig.Quality.OFF);
        DLSSConfig.FRAME_GEN.set(DLSSConfig.FrameGen.OFF);
        DLSSConfig.FSR_FRAME_GEN.set(DLSSConfig.FsrFrameGen.OFF);
        DLSSConfig.XESS_FRAME_GEN.set(DLSSConfig.FrameGen.OFF);
        DLSSConfig.FSR_SR.set(DLSSConfig.FsrSr.OFF);
        DLSSConfig.XESS_SR.set(DLSSConfig.XessSr.OFF);
        routing(null, 0, -1, "all OFF");
        DLSSConfig.MODE.set(DLSSConfig.Quality.QUALITY);
        DLSSConfig.FRAME_GEN.set(DLSSConfig.FrameGen.X2);
        routing(DLSSConfig.Backend.DLSS, 1, 3, "DLSS QUALITY + X2");
        DLSSConfig.FRAME_GEN.set(DLSSConfig.FrameGen.OFF);
        routing(DLSSConfig.Backend.DLSS, 0, 3, "DLSS QUALITY alone");
        DLSSConfig.FRAME_GEN.set(DLSSConfig.FrameGen.X2);
        DLSSConfig.MODE.set(DLSSConfig.Quality.OFF);
        routing(DLSSConfig.Backend.DLSS, 1, -1, "DLSS SR OFF + X2");
        DLSSConfig.MODE.set(DLSSConfig.Quality.QUALITY);
        DLSSConfig.makeExclusive(DLSSConfig.Backend.FSR);
        DLSSConfig.FSR_FRAME_GEN.set(DLSSConfig.FsrFrameGen.X2);
        DLSSConfig.FSR_SR.set(DLSSConfig.FsrSr.QUALITY);
        routing(DLSSConfig.Backend.FSR, 1, 1, "FSR owns its SR");
        DLSSConfig.FSR_SR.set(DLSSConfig.FsrSr.NATIVE_AA);
        routing(DLSSConfig.Backend.FSR, 1, 0, "FSR native AA is not DLSS OFF");
        check(DLSSConfig.MODE.get() == DLSSConfig.Quality.QUALITY, "FSR must retain DLSS mode");
        DLSSConfig.makeExclusive(DLSSConfig.Backend.XESS);
        DLSSConfig.XESS_FRAME_GEN.set(DLSSConfig.FrameGen.X2);
        DLSSConfig.XESS_SR.set(DLSSConfig.XessSr.QUALITY);
        routing(DLSSConfig.Backend.XESS, 1, 103, "XeSS owns its SR");
        DLSSConfig.XESS_SR.set(DLSSConfig.XessSr.AA);
        routing(DLSSConfig.Backend.XESS, 1, 106, "XeSS AA");
        check(DLSSConfig.MODE.get() == DLSSConfig.Quality.QUALITY, "XeSS must retain DLSS mode");
        check(DLSSConfig.FSR_SR.get() == DLSSConfig.FsrSr.NATIVE_AA, "XeSS must not change FSR SR");
        DLSSConfig.makeExclusive(DLSSConfig.Backend.DLSS);
        routing(DLSSConfig.Backend.DLSS, 0, 3, "return to remembered DLSS SR-only");
        DLSSConfig.MODE.set(DLSSConfig.Quality.OFF);
        DLSSConfig.FSR_SR.set(DLSSConfig.FsrSr.OFF);
        DLSSConfig.XESS_SR.set(DLSSConfig.XessSr.OFF);
        routing(null, 0, -1, "return to all OFF");
        check(!DLSSFGNative.isLoaded(), "routing must not initialize native graphics");
        System.out.println("PASS: in-memory DLSS/FSR/XeSS routing; no native backend loaded");
    }

    // Resolve the new contract lazily so --routing-only can compile/run against the pre-change bridge.
    // These invoke production JNI declarations, not substitute native implementations.
    static Object nativeCall(String name, Class<?>[] types, Object... args) throws Exception {
        return DLSSFGNative.class.getMethod(name, types).invoke(null, args);
    }

    static int upscale(boolean reset) throws Exception {
        return (int) nativeCall("nativeUpscale", new Class<?>[]{float[].class, float[].class, boolean.class},
                matrices, params, reset);
    }

    static int upscaleDone() throws Exception {
        return (int) nativeCall("nativeUpscaleDone", new Class<?>[]{});
    }

    static void runtimeFbo(String name, int value) throws Exception {
        var field = FGRuntime.class.getDeclaredField(name);
        field.setAccessible(true);
        field.setInt(FGRuntime.get(), value);
    }

    static void copy(int src, int dst, int w, int h) throws Exception {
        try {
            copyImage.invoke(FGRuntime.get(), src, dst, w, h);
        } finally {
            // Do not leave an interop output attached to a test FBO after its lock is released.
            GL45C.glNamedFramebufferTexture(copyRead, GL45C.GL_COLOR_ATTACHMENT0, 0, 0);
            GL45C.glNamedFramebufferTexture(copyDraw, GL45C.GL_COLOR_ATTACHMENT0, 0, 0);
        }
    }

    static int texture(int format, int w, int h) {
        int t = GL45C.glGenTextures();
        GL45C.glBindTexture(GL45C.GL_TEXTURE_2D, t);
        GL45C.glTexImage2D(GL45C.GL_TEXTURE_2D, 0, format, w, h, 0,
                format == GL45C.GL_R32F ? GL45C.GL_RED : GL45C.GL_RGBA, GL45C.GL_FLOAT, 0L);
        GL45C.glTexParameteri(GL45C.GL_TEXTURE_2D, GL45C.GL_TEXTURE_MIN_FILTER, GL45C.GL_NEAREST);
        GL45C.glTexParameteri(GL45C.GL_TEXTURE_2D, GL45C.GL_TEXTURE_MAG_FILTER, GL45C.GL_NEAREST);
        GL45C.glBindTexture(GL45C.GL_TEXTURE_2D, 0);
        return t;
    }

    static void paint(int texture, int w, int h, int phase) {
        for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) {
            int x0 = x * (w / 2), y0 = y * (h / 2);
            GL45C.glClearTexSubImage(texture, 0, x0, y0, 0,
                    x == 0 ? w / 2 : w - x0, y == 0 ? h / 2 : h - y0, 1,
                    GL45C.GL_RGBA, GL45C.GL_FLOAT, COLORS[(x + 2 * y + phase) % 4]);
        }
    }

    static float[] pixel(int texture, int x, int y) {
        if (texture == textures[5] && texture != 0) check(outputLocked, "SR output read outside WGL lock");
        for (int i = 0; i < 5; i++) {
            if (texture == textures[i] && texture != 0) check(inputsLocked, "input read outside WGL lock");
        }
        int old = GL45C.glGetInteger(GL45C.GL_READ_FRAMEBUFFER_BINDING);
        try {
            GL45C.glNamedFramebufferTexture(readFbo, GL45C.GL_COLOR_ATTACHMENT0, texture, 0);
            check(GL45C.glCheckNamedFramebufferStatus(readFbo, GL45C.GL_READ_FRAMEBUFFER)
                    == GL45C.GL_FRAMEBUFFER_COMPLETE, "locked texture must be framebuffer-readable: " + texture);
            GL45C.glBindFramebuffer(GL45C.GL_READ_FRAMEBUFFER, readFbo);
            GL45C.glReadBuffer(GL45C.GL_COLOR_ATTACHMENT0);
            float[] rgba = new float[4];
            GL45C.glReadPixels(x, y, 1, 1, GL45C.GL_RGBA, GL45C.GL_FLOAT, rgba);
            return rgba;
        } finally {
            GL45C.glBindFramebuffer(GL45C.GL_READ_FRAMEBUFFER, old);
            GL45C.glNamedFramebufferTexture(readFbo, GL45C.GL_COLOR_ATTACHMENT0, 0, 0);
        }
    }

    static void color(float[] actual, float[] expected, String label) {
        for (int c = 0; c < 3; c++) check(Math.abs(actual[c] - expected[c]) < 0.16f,
                label + ": got " + Arrays.toString(actual) + ", expected " + Arrays.toString(expected));
    }

    static void image(int texture, int w, int h, int phase, boolean flipped, String label) {
        for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) {
            color(pixel(texture, (2 * x + 1) * w / 4, (2 * y + 1) * h / 4),
                    COLORS[(x + 2 * (flipped ? 1 - y : y) + phase) % 4], label + " quadrant " + x + "," + y);
        }
    }

    static void lock() {
        ok(DLSSFGNative.nativeLock(), "input lock (output excluded)");
        inputsLocked = true;
    }

    static void unlock() {
        GL45C.glFinish();
        ok(DLSSFGNative.nativeUnlock(), "input unlock");
        inputsLocked = false;
    }

    static void deleteTargets() {
        if (depthMotion != null) depthMotion.close();
        depthMotion = null;
        GL45C.glDeleteTextures(new int[]{textures[0], textures[1], textures[4], textures[5],
                worldSource, depthSource, mainColor});
        Arrays.fill(textures, 0);
        worldSource = depthSource = mainColor = 0;
    }

    static void replaceTargets(int q, int w, int h) throws Exception {
        replaceTargets(q, w, h, 0);
    }

    static void replaceTargets(int q, int w, int h, int preset) throws Exception {
        if (bound) {
            ok(DLSSFGNative.nativeUnbindTextures(), "unbind without destroying device");
            bound = false;
            check(DLSSFGNative.nativeIsSupported() == 1, "unbind destroyed process device");
            fails(DLSSFGNative.nativeBeginFrame(false, 0), "begin without bound textures");
            fails(upscale(true), "upscale without bound textures");
            deleteTargets();
        }
        if (w != width || h != height) {
            GLFW.glfwSetWindowSize(window, w, h); // still hidden; no show/focus/swap calls
            GLFW.glfwPollEvents();
            ok(DLSSFGNative.nativeResize(w, h), "resize retained Vulkan device");
            width = w;
            height = h;
        }
        quality = q;
        ok((int) nativeCall("nativeConfigureSR", new Class<?>[]{int.class, float.class, int.class},
                q, 0f, preset), "configure SR " + q);
        rw = width;
        rh = height;
        if (q > 0) {
            int[] size = (int[]) nativeCall("nativeGetRenderSize", new Class<?>[]{int.class}, q);
            check(size != null && size.length == 2 && size[0] > 0 && size[1] > 0
                    && size[0] <= width && size[1] <= height, "invalid SDK render size: " + Arrays.toString(size));
            rw = size[0];
            rh = size[1];
            check(q == 6 ? rw == width && rh == height : rw < width && rh < height,
                    "SDK size must distinguish DLAA from upscaling");
        }
        textures[0] = texture(GL45C.GL_RGBA8, width, height);
        textures[1] = texture(GL45C.GL_RGBA8, width, height);
        depthMotion = new FGDepthMotionPass();
        depthMotion.resize(rw, rh);
        textures[2] = depthMotion.getDepthTexture();
        textures[3] = depthMotion.getMotionTexture();
        if (q > 0) {
            textures[4] = texture(GL45C.GL_RGBA8, rw, rh);
            textures[5] = texture(GL45C.GL_RGBA8, width, height);
        }
        worldSource = texture(GL45C.GL_RGBA8, rw, rh);
        depthSource = texture(GL45C.GL_R32F, rw, rh);
        mainColor = texture(GL45C.GL_RGBA8, width, height); // detached from Minecraft and all native registrations
        ok((int) nativeCall("nativeBindTextures", new Class<?>[]{int.class, int.class, int.class, int.class,
                        int.class, int.class, int.class, int.class},
                textures[0], textures[1], textures[2], textures[3], textures[4], textures[5], rw, rh), "bind six textures");
        bound = true;
        params[3] = (float) width / height;
        Matrix4f projection = new Matrix4f().perspective(params[2], params[3], params[0], params[1]);
        projection.get(matrices, 0);
        new Matrix4f().get(matrices, 16);
        projection.get(matrices, 32);
        new Matrix4f().get(matrices, 48);
        fails(upscale(true), "upscale before begin token, quality=" + q);
        System.out.printf("SR quality=%d SDK render=%dx%d display=%dx%d%n", q, rw, rh, width, height);
    }

    static void fg(boolean enabled) throws Exception {
        ok(DLSSFGNative.nativeSetEnabled(enabled ? 1 : 0), "FG " + (enabled ? "ON" : "OFF"));
        check(DLSSFGNative.nativeIsSupported() == 1, "FG toggle must retain Vulkan device");
        if (enabled) check((int) nativeCall("nativeSetMode", new Class<?>[]{int.class, boolean.class}, 1, false) == 1,
                "RTX 4070 must accept unforced X2");
    }

    static void frames(String label, boolean present) throws Exception {
        int minSdk = Integer.MAX_VALUE, maxSdk = 0;
        for (int frame = 0; frame < 6; frame++) {
            int phase = frame / 3; // also test history reuse, then reset to a different nonblack image
            boolean reset = frame % 3 == 0;
            ok(DLSSFGNative.nativeBeginFrame(false, 0), label + ": begin (including SR-only without swapchain)");
            DLSSFGNative.nativeRenderStart();
            paint(worldSource, rw, rh, phase);
            GL45C.glClearTexImage(depthSource, 0, GL45C.GL_RED, GL45C.GL_FLOAT, new float[]{0.5f});
            lock();
            try {
                GL45C.glClearTexImage(textures[0], 0, GL45C.GL_RGBA, GL45C.GL_FLOAT, POISON);
                GL45C.glClearTexImage(textures[1], 0, GL45C.GL_RGBA, GL45C.GL_FLOAT, POISON);
                Matrix4f projection = new Matrix4f().set(matrices);
                depthMotion.run(depthSource, 0, 0, new Matrix4f(projection).invert(), projection, false);
                float[] depth = pixel(textures[2], rw / 4, rh / 4);
                float[] motion = pixel(textures[3], rw / 4, rh / 4);
                check(Math.abs(depth[0] - 0.5f) < 0.002f, "production depth shader");
                check(Math.abs(motion[0]) < 0.002f && Math.abs(motion[1]) < 0.002f, "stationary production motion");
                copy(worldSource, quality > 0 ? textures[4] : textures[1], rw, rh);
                image(quality > 0 ? textures[4] : textures[1], rw, rh, phase, true, "production input Y flip");
                if (quality > 0 && frame == 0) fails(upscale(reset), "upscale while inputs locked with valid token");
            } finally {
                unlock();
            }
            if (quality > 0) {
                ok(upscale(reset), label + ": real native SR");
                outputLocked = true;
                try {
                    if (frame == 0) fails(upscale(reset), "upscale while previous output held");
                    image(textures[5], width, height, phase, true, "native SR output under WGL lock");
                    copy(textures[5], mainColor, width, height);
                    image(mainColor, width, height, phase, false, "production SR output return Y flip");
                } finally {
                    GL45C.glFinish();
                    ok(upscaleDone(), "SR output unlock");
                    outputLocked = false;
                }
                if (frame == 0) ok(upscaleDone(), "repeated SR output unlock (idempotent)");
            } else {
                fails(upscale(reset), "SR OFF rejects upscale even with a token");
                paint(mainColor, width, height, phase);
            }
            // Add a native-resolution HUD patch only after returning SR to the detached main target.
            GL45C.glClearTexSubImage(mainColor, 0, width / 16, height * 13 / 16, 0,
                    width / 8, height / 8, 1, GL45C.GL_RGBA, GL45C.GL_FLOAT, HUD);
            color(pixel(mainColor, width / 8, height * 7 / 8), HUD, "detached main composition");
            lock();
            try {
                // Never copy into hudless here: only native SR may have replaced its poisoned contents.
                image(textures[1], width, height, phase, true, "native full-resolution hudless");
                color(pixel(textures[1], width / 8, height / 8), COLORS[(2 + phase) % 4], "hudless excludes HUD");
                copy(mainColor, textures[0], width, height);
                image(textures[0], width, height, phase, true, "full final input after composition");
                color(pixel(textures[0], width / 8, height / 8), HUD, "final includes full-resolution HUD");
            } finally {
                unlock();
            }
            // Intentionally no second beginFrame: SR and FG must share this exact frame token.
            if (present) {
                ok(DLSSFGNative.nativePresent(matrices, params, reset, 1), label + ": same-token nativePresent");
                int count = DLSSFGNative.nativeGetPresentedCount();
                check(count >= 0, "invalid SDK presented count");
                minSdk = Math.min(minSdk, count);
                maxSdk = Math.max(maxSdk, count);
            }
            GLFW.glfwPollEvents();
            check(GLFW.glfwGetWindowAttrib(window, GLFW.GLFW_VISIBLE) == GLFW.GLFW_FALSE, "test window became visible");
            ok(GL45C.glGetError(), label + ": OpenGL error");
        }
        System.out.println("PASS: " + label + (present
                ? "; SDK numFramesActuallyPresented range=" + minSdk + ".." + maxSdk + " (hidden; extra generation not required)"
                : "; SR-only, no presentation requested"));
    }

    static void exercise(String order) throws Exception {
        boolean srOnly = order.equals("sr-only");
        if (order.equals("fg-first")) {
            replaceTargets(-1, width, height);
            fg(true);
            frames("FG evaluated before SR configuration", true);
        } else {
            fg(false);
        }
        replaceTargets(3, width, height);
        fg(order.equals("fg-first"));
        frames("QUALITY first SR", order.equals("fg-first"));
        int qualityW = rw, qualityH = rh;
        if (!srOnly) {
            fg(true);
            frames("QUALITY SR + FG", true);
        }
        for (int cycle = 0; cycle < 2; cycle++) {
            fg(false); // no configure/rebind: specifically prove OFF preserves the already-ready SR
            frames("FG OFF preserves SR cycle " + cycle, false);
            if (!srOnly) {
                fg(true);
                frames("FG ON reuses SR cycle " + cycle, true);
            }
        }
        for (int q : new int[]{2, 6, -1, 3, 3}) {
            replaceTargets(q, width, height);
            if (q == 2) check(rw < qualityW && rh < qualityH, "Balanced must render below Quality");
            fg(!srOnly);
            if (q == -1 && srOnly) {
                fails(DLSSFGNative.nativeBeginFrame(false, 0), "all OFF has no frame work");
                fails(upscale(true), "all OFF upscale");
            } else {
                frames("quality/OFF/rebind " + q, !srOnly);
            }
        }
        replaceTargets(3, 1360, 768);
        fg(!srOnly);
        frames("resize and reconfigure", !srOnly);
        // There is deliberately only one nativeInit/nativeCreateDevice in this entire JVM.
    }

    // Same production ownership/copy path as frames(), without pixel readbacks or deliberate failures.
    static void timedFrame(boolean present, boolean reset, long[] elapsed) throws Exception {
        long start = System.nanoTime();
        ok(DLSSFGNative.nativeBeginFrame(false, 0), "timing begin (Reflex)");
        long begun = System.nanoTime();
        elapsed[0] = begun - start;
        DLSSFGNative.nativeRenderStart();
        paint(worldSource, rw, rh, 0);
        GL45C.glClearTexImage(depthSource, 0, GL45C.GL_RED, GL45C.GL_FLOAT, new float[]{0.5f});
        long lockStart = System.nanoTime();
        lock();
        long locked = System.nanoTime();
        elapsed[1] = locked - lockStart;
        try {
            Matrix4f projection = new Matrix4f().set(matrices);
            depthMotion.run(depthSource, 0, 0, new Matrix4f(projection).invert(), projection, false);
            copy(worldSource, quality > 0 ? textures[4] : textures[1], rw, rh);
        } finally {
            unlock(); // includes the production glFinish before returning WGL input ownership
        }
        long inputsDone = System.nanoTime();
        elapsed[2] = inputsDone - locked;
        elapsed[3] = elapsed[4] = 0;
        if (quality > 0) {
            long srStart = System.nanoTime();
            ok(upscale(reset), "timing nativeUpscale");
            outputLocked = true;
            long upscaled = System.nanoTime();
            elapsed[3] = upscaled - srStart;
            try {
                copy(textures[5], mainColor, width, height);
            } finally {
                GL45C.glFinish();
                ok(upscaleDone(), "timing SR output unlock");
                outputLocked = false;
            }
            elapsed[4] = System.nanoTime() - upscaled;
        } else {
            paint(mainColor, width, height, 0);
        }
        GL45C.glClearTexSubImage(mainColor, 0, width / 16, height * 13 / 16, 0,
                width / 8, height / 8, 1, GL45C.GL_RGBA, GL45C.GL_FLOAT, HUD);
        long finalStart = System.nanoTime();
        lock();
        try {
            copy(mainColor, textures[0], width, height);
        } finally {
            unlock();
        }
        elapsed[5] = System.nanoTime() - finalStart;
        elapsed[6] = 0;
        if (present) {
            long presentStart = System.nanoTime();
            // Keep the same frame token; log status changes, not every measured frame.
            ok(DLSSFGNative.nativePresent(matrices, params, reset, 0), "timing nativePresent");
            elapsed[6] = System.nanoTime() - presentStart;
        }
        elapsed[7] = System.nanoTime() - start;
    }

    // Nearest rank on the actual sorted measurement array, not a histogram or a last-frame estimate.
    static double percentileMs(long[] sorted, int percentile) {
        return sorted[(int) Math.ceil(sorted.length * percentile / 100.0) - 1] / 1e6;
    }

    static void timingStatisticsCheck() {
        long[] samples = new long[120];
        Arrays.setAll(samples, i -> (120L - i) * 1_000_000);
        Arrays.sort(samples);
        check(percentileMs(samples, 50) == 60 && percentileMs(samples, 95) == 114
                && percentileMs(samples, 99) == 119, "timing percentile rank/unit self-check");
    }

    static void timingCase(String label, int q, int preset, boolean present) throws Exception {
        replaceTargets(q, 2560, 1346, preset);
        fg(present);
        System.out.printf("TIMING %s: SR=%d preset=%d FG=%s render=%dx%d display=%dx%d%n",
                label, q, preset, present ? "X2" : "OFF", rw, rh, width, height);
        frames(label + " preflight", present); // Unchanged six-frame pixel/lock regression, exactly once per case.
        final int warmup = 30, measured = 120;
        String[] stages = {"begin (Reflex)", "input lock", "GL input pass+copy+finish+unlock",
                "nativeUpscale", "SR return copy+finish+upscaleDone", "final lock+copy+finish+unlock",
                "nativePresent", "total frame"};
        long[][] samples = new long[stages.length][measured];
        long[] elapsed = new long[stages.length];
        int minSdk = Integer.MAX_VALUE, maxSdk = 0;
        for (int frame = 0; frame < warmup + measured; frame++) {
            timedFrame(present, frame == 0, elapsed);
            if (frame >= warmup) {
                for (int stage = 0; stage < stages.length; stage++) samples[stage][frame - warmup] = elapsed[stage];
                if (present) {
                    int count = DLSSFGNative.nativeGetPresentedCount();
                    check(count >= 0, "invalid timing SDK presented count");
                    minSdk = Math.min(minSdk, count);
                    maxSdk = Math.max(maxSdk, count);
                }
            }
            // No image()/pixel()/glReadPixels in warmup or measurement; diagnostics are outside the frame timer.
            GLFW.glfwPollEvents();
            if (frame == warmup - 1 || frame == warmup + measured - 1) {
                check(!inputsLocked && !outputLocked, "timing leaked WGL ownership");
                check(GLFW.glfwGetWindowAttrib(window, GLFW.GLFW_VISIBLE) == GLFW.GLFW_FALSE, "test window became visible");
                ok(GL45C.glGetError(), label + ": timing OpenGL error");
            }
        }
        double[] bridge = DLSSFGNative.nativeGetBridgeTimings();
        check(bridge != null && bridge.length == 8, "expected all eight native bridge timings");
        System.out.printf("%s: warmup=%d measured=%d; CPU wall ms, nearest-rank percentiles%n", label, warmup, measured);
        for (int stage = 0; stage < stages.length; stage++) {
            if ((q < 0 && (stage == 3 || stage == 4)) || (!present && stage == 6)) {
                System.out.println("  " + stages[stage] + ": N/A (disabled; not called)");
                continue;
            }
            Arrays.sort(samples[stage]);
            System.out.printf(java.util.Locale.ROOT, "  %-36s p50=%.3f p95=%.3f p99=%.3f%n", stages[stage],
                    percentileMs(samples[stage], 50), percentileMs(samples[stage], 95), percentileMs(samples[stage], 99));
        }
        System.out.println(present
                ? "  SDK numFramesActuallyPresented measured range=" + minSdk + ".." + maxSdk + " (not FPS)"
                : "  SDK numFramesActuallyPresented range=N/A (SR-only; no nativePresent)");
        System.out.println("  Native bridge LAST instantaneous snapshot ms, NOT an average: "
                + "[slotWait, acquire, submit, present, state, lockSubmitFence, lockInputFence, lockDX]="
                + Arrays.toString(bridge) + (present ? "" : " (present fields may be stale from preceding FG case)"));
    }

    static void timing() throws Exception {
        System.out.println("TIMING: fixed test-pattern bridge throughput only; no simulated Minecraft world workload. "
                + "Not game input latency, screen pacing, or an FPS improvement measurement. "
                + "SR+return time is not input lag; SDK presented counts are not measured display FPS.");
        System.out.println("Reflex boost=false, FPS limit=0; one JVM/device, hidden 2560x1346, fixed stationary history. "
                + "Total includes renderStart and pattern/HUD setup; excludes polling/telemetry. "
                + "SR-only retains final copy for comparison but never presents; no per-frame readbacks in timed loops.");
        timingCase("FG-only", -1, 0, true);
        timingCase("Balanced/AUTO + X2", 2, 0, true);
        timingCase("Balanced/M + X2", 2, 13, true);
        timingCase("SR-only Balanced/M", 2, 13, false);
    }

    static void sdkLogs(Path logs, boolean expectFg) throws Exception {
        Path bridge = logs.resolve("dlssmc_fg.log");
        check(Files.isRegularFile(bridge) && Files.size(bridge) > 0, "missing native diagnostic log: " + bridge);
        Pattern errors = Pattern.compile("(?i)\\[SL/ERR\\]|\\[\\s*(?:error|fatal)\\s*\\]");
        Pattern state = Pattern.compile("stateResult=(-?\\d+) status=0x[0-9A-Fa-f]+");
        var states = new LinkedHashSet<String>();
        try (var files = Files.walk(logs)) {
            for (Path file : files.filter(p -> p.toString().endsWith(".log")).toList()) {
                // Native callback logs are byte strings; ISO-8859-1 preserves ASCII severity tokens even for non-UTF8 SDK text.
                for (String line : Files.readAllLines(file, StandardCharsets.ISO_8859_1)) {
                    check(!errors.matcher(line).find(), "SDK error in " + file + ": " + line);
                    var match = state.matcher(line);
                    if (match.find()) {
                        check(match.group(1).equals("0"), "SDK state query failed: " + line);
                        states.add(match.group());
                    }
                }
            }
        }
        check(!expectFg || !states.isEmpty(), "FG SDK state telemetry missing");
        System.out.println("PASS: SDK log severity check; SDK states=" + states + "; logs=" + logs);
    }

    public static void main(String[] args) throws Exception {
        String order = "sr-first";
        Path plugins = Path.of(PLUGINS).toAbsolutePath();
        boolean routingOnly = false, timing = false, orderSpecified = false;
        for (int i = 0; i < args.length; i++) {
            switch (args[i]) {
                case "--routing-only" -> routingOnly = true;
                case "--timing" -> timing = true;
                case "--order" -> {
                    order = args[++i];
                    orderSpecified = true;
                }
                case "--plugins" -> plugins = Path.of(args[++i]).toAbsolutePath();
                default -> throw new IllegalArgumentException("Unsupported option (hidden test only): " + args[i]);
            }
        }
        check(Arrays.asList("sr-first", "fg-first", "sr-only").contains(order), "unknown init order: " + order);
        check(!timing || (!routingOnly && !orderSpecified), "--timing cannot be combined with --routing-only or --order");
        routingOnly();
        if (routingOnly) return;
        if (timing) {
            timingStatisticsCheck();
            width = 2560;
            height = 1346;
        }
        Path temp = Files.createTempDirectory(Path.of(System.getProperty("java.io.tmpdir")), "sr-init-order-");
        Path logs = Files.createDirectory(temp.resolve("logs"));
        System.out.println("Order=" + (timing ? "timing" : order) + "; temporary cache/logs=" + temp
                + "; production resource=" + DLSSFGNative.class.getResource(DLSSFGNative.NATIVE_RESOURCE));
        boolean glfw = false, gl = false, initAttempted = false;
        try {
            check(DLSSFGNative.load(plugins, temp.resolve("cache")), "production DLL extraction/load: " + DLSSFGNative.getLastError());
            glfw = GLFW.glfwInit();
            check(glfw, "GLFW initialization");
            GLFW.glfwDefaultWindowHints();
            GLFW.glfwWindowHint(GLFW.GLFW_VISIBLE, GLFW.GLFW_FALSE);
            GLFW.glfwWindowHint(GLFW.GLFW_CONTEXT_VERSION_MAJOR, 4);
            GLFW.glfwWindowHint(GLFW.GLFW_CONTEXT_VERSION_MINOR, 5);
            GLFW.glfwWindowHint(GLFW.GLFW_OPENGL_PROFILE, GLFW.GLFW_OPENGL_CORE_PROFILE);
            window = GLFW.glfwCreateWindow(width, height, "Hidden SR init-order regression", 0, 0);
            check(window != 0, "hidden GL 4.5 context");
            GLFW.glfwMakeContextCurrent(window);
            GL.createCapabilities();
            gl = true;
            RenderSystem.initRenderThread();
            check(GL.getCapabilities().OpenGL45, "OpenGL 4.5 required");
            System.out.println("GPU=" + GL45C.glGetString(GL45C.GL_RENDERER));
            copyImage = FGRuntime.class.getDeclaredMethod("copyImage", int.class, int.class, int.class, int.class);
            copyImage.setAccessible(true);
            copyRead = GL45C.glCreateFramebuffers();
            copyDraw = GL45C.glCreateFramebuffers();
            readFbo = GL45C.glCreateFramebuffers();
            runtimeFbo("copyReadFbo", copyRead);
            runtimeFbo("copyDrawFbo", copyDraw);
            initAttempted = true;
            ok(DLSSFGNative.nativeInit(plugins.toString(), logs.toString(), false), "single Vulkan slInit");
            ok(DLSSFGNative.nativeCreateDevice(GLFWNativeWin32.glfwGetWin32Window(window),
                    width, height, false, 0), "single Vulkan device creation");
            check(DLSSFGNative.nativeIsSupported() == 1, "RTX 4070 DLSS-G support");
            System.out.println("SDK/model versions=" + Arrays.toString(DLSSFGNative.nativeGetModelVersions()));
            DLSSFGNative.nativeSetTuning(false, true, true, true, false);
            if (timing) timing();
            else exercise(order);
        } finally {
            try {
                if (initAttempted) DLSSFGNative.nativeShutdown(); // also releases any lock on assertion failure
            } finally {
                try {
                    if (gl) {
                        deleteTargets();
                        GL45C.glDeleteFramebuffers(new int[]{readFbo, copyRead, copyDraw});
                        if (copyImage != null) {
                            runtimeFbo("copyReadFbo", 0);
                            runtimeFbo("copyDrawFbo", 0);
                        }
                    }
                } finally {
                    if (window != 0) GLFW.glfwDestroyWindow(window);
                    if (glfw) GLFW.glfwTerminate();
                }
            }
        }
        check(DLSSFGNative.nativeIsSupported() == 0, "client shutdown must release process device");
        fails(DLSSFGNative.nativeBeginFrame(false, 0), "shutdown leaves no bound frame resources");
        fails(upscale(true), "shutdown rejects SR without bound resources");
        sdkLogs(logs, timing || !order.equals("sr-only"));
        System.out.println(timing
                ? "PASS: four timing cases with pixel/lock preflights and strict SDK logs; not game latency, display pacing or FPS gains"
                : "PASS: " + order + "; actual SR pixels, production copies/shader, shared-device toggles; desktop/extra FG not verified");
    }
}
