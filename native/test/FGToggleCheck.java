import com.taolesi.dlssmc.DLSSMC;
import com.taolesi.dlssmc.core.FGRuntime;
import com.taolesi.dlssmc.config.DLSSConfig;
import com.taolesi.dlssmc.mixin.MixinWindow;
import com.taolesi.dlssmc.nativebridge.DLSSFGNative;
import org.joml.Matrix4f;
import org.lwjgl.glfw.GLFW;
import org.lwjgl.glfw.GLFWNativeWin32;
import org.lwjgl.opengl.GL;
import org.lwjgl.opengl.GL45C;
import org.lwjgl.opengl.GLDebugMessageCallback;

import java.nio.file.Path;

public class FGToggleCheck {
    static int debugErrors;
    static boolean visible;
    static int width = 1280;
    static int height = 720;

    static boolean uiOn;
    static boolean worlds;

    static void runtimeField(String name, Object value) throws Exception {
        var field = FGRuntime.class.getDeclaredField(name);
        field.setAccessible(true);
        field.set(FGRuntime.get(), value);
    }

    static com.sun.jna.Pointer child(long window) {
        return com.sun.jna.NativeLibrary.getInstance("user32").getFunction("GetWindow")
                .invokePointer(new Object[]{new com.sun.jna.Pointer(GLFWNativeWin32.glfwGetWin32Window(window)), 5});
    }

    static void worldEvent(String type) throws Exception {
        var unsafeField = sun.misc.Unsafe.class.getDeclaredField("theUnsafe");
        unsafeField.setAccessible(true);
        var unsafe = (sun.misc.Unsafe) unsafeField.get(null);
        // Only bypass the Forge container constructor; execute the production event handler unchanged.
        Object mod = unsafe.allocateInstance(DLSSMC.class);
        var handler = java.util.Arrays.stream(DLSSMC.class.getDeclaredMethods())
                .filter(m -> m.getParameterCount() == 1
                        && m.getParameterTypes()[0].getSimpleName().equals(type))
                .findFirst().orElseThrow();
        handler.setAccessible(true);
        handler.invoke(mod, new Object[]{null});
    }

    static void logout(long window) throws Exception {
        for (String name : new String[]{"hasPrev", "armed", "frameCaptured"}) runtimeField(name, true);
        var previousChild = child(window);
        check(previousChild != null, "FG child before logout");
        worldEvent("LoggingOut");
        check(FGRuntime.get().isReady(), "logout must retain the process graphics device and Streamline");
        check(previousChild.equals(child(window)), "logout must not expose the stale GL front buffer before swap");
        check(!FGRuntime.get().presentFrame(), "logout must discard the captured world frame");
        glFrame(window, 1, "world selection menu");
        check(child(window) == null, "FG child must be released after the menu GL swap");
        check(DLSSFGNative.nativeIsSupported() == 1, "process device must survive logout");
        FGRuntime.get().beginFrame();
        FGRuntime.get().renderStart();
        check(child(window) == null, "nested exit ticks must not recreate the FG window");
        check(!FGRuntime.get().presentFrame(), "menu without fresh world inputs must not take over");
    }

    static void inputBeforeFrame(long window) {
        GLFW.glfwPollEvents();
        int[] received = {0};
        var cursor = org.lwjgl.glfw.GLFWCursorPosCallback.create((w, x, y) -> {
            if ((x == 41 && y == 17) || (x == 83 && y == 29)) received[0]++;
        });
        var previous = GLFW.glfwSetCursorPosCallback(window, cursor);
        var post = com.sun.jna.NativeLibrary.getInstance("user32").getFunction("PostMessageW");
        var hwnd = new com.sun.jna.Pointer(GLFWNativeWin32.glfwGetWin32Window(window));
        try {
            check(post.invokeInt(new Object[]{hwnd, 0x0200, 0L, (17L << 16) | 41}) != 0, "post control mouse event");
            check(received[0] == 0, "posted input must remain pending without event polling");
            GLFW.glfwPollEvents();
            check(received[0] == 1, "control polling must dispatch the hidden window's queued input");
            check(post.invokeInt(new Object[]{hwnd, 0x0200, 0L, (29L << 16) | 83}) != 0, "post frame mouse event");
            check(received[0] == 1, "new input must still be queued before beginFrame");
            FGRuntime.get().beginFrame();
            check(received[0] == 2, "Reflex frame start must poll queued input before simulation/rendering");
            System.out.println("PASS: queued input reaches this frame after Reflex sleep, with an unpolled negative control");
        } finally {
            GLFW.glfwSetCursorPosCallback(window, previous);
            cursor.free();
        }
    }

    static void bindTextures(int[] textures) {
        int[] previous = textures.clone();
        textures[0] = texture(GL45C.GL_RGBA8, width, height);
        textures[1] = texture(GL45C.GL_RGBA8, width, height);
        textures[2] = texture(GL45C.GL_R32F, width, height);
        textures[3] = texture(GL45C.GL_RG16F, width, height);
        check(DLSSFGNative.nativeBindTextures(textures[0], textures[1], textures[2], textures[3],
                0, 0, width, height) == 0, "bind textures");
        GL45C.glDeleteTextures(previous);
    }

    static void desktopPixel(long window, int channel, String label) {
        if (!visible) return;
        var user32 = com.sun.jna.platform.win32.User32.INSTANCE;
        var hwnd = new com.sun.jna.platform.win32.WinDef.HWND(
                new com.sun.jna.Pointer(GLFWNativeWin32.glfwGetWin32Window(window)));
        var point = new com.sun.jna.platform.win32.WinDef.POINT(320, 180);
        check(com.sun.jna.NativeLibrary.getInstance("user32").getFunction("ClientToScreen")
                .invokeInt(new Object[]{hwnd, point}) != 0, "client position");
        var dc = user32.GetDC(null);
        int color = 0;
        try {
            for (int attempt = 0; attempt < 30; ++attempt) {
                com.sun.jna.NativeLibrary.getInstance("dwmapi").getFunction("DwmFlush").invokeInt(new Object[]{});
                color = com.sun.jna.NativeLibrary.getInstance("gdi32").getFunction("GetPixel")
                        .invokeInt(new Object[]{dc, point.x, point.y});
                if (color == (255 << (channel * 8))) break;
            }
        } finally {
            user32.ReleaseDC(null, dc);
        }
        System.out.printf("%s desktop COLORREF=0x%06x%n", label, color);
        check(color == (255 << (channel * 8)), label + ": stale desktop pixel");
    }

    static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    static int texture(int format, int width, int height) {
        int texture = GL45C.glGenTextures();
        GL45C.glBindTexture(GL45C.GL_TEXTURE_2D, texture);
        int components = format == GL45C.GL_R32F ? GL45C.GL_RED :
                format == GL45C.GL_RG16F ? GL45C.GL_RG : GL45C.GL_RGBA;
        GL45C.glTexImage2D(GL45C.GL_TEXTURE_2D, 0, format, width, height, 0,
                components, GL45C.GL_FLOAT, 0L);
        GL45C.glTexParameteri(GL45C.GL_TEXTURE_2D, GL45C.GL_TEXTURE_MIN_FILTER, GL45C.GL_NEAREST);
        GL45C.glTexParameteri(GL45C.GL_TEXTURE_2D, GL45C.GL_TEXTURE_MAG_FILTER, GL45C.GL_NEAREST);
        GL45C.glBindTexture(GL45C.GL_TEXTURE_2D, 0);
        return texture;
    }

    static void clearInputs(int[] textures, float r, float g, float b) {
        check(DLSSFGNative.nativeLock() == 0, "input lock");
        try {
            float[] color = {r, g, b, 1};
            GL45C.glClearTexImage(textures[0], 0, GL45C.GL_RGBA, GL45C.GL_FLOAT, color);
            GL45C.glClearTexImage(textures[1], 0, GL45C.GL_RGBA, GL45C.GL_FLOAT, color);
            GL45C.glClearTexImage(textures[2], 0, GL45C.GL_RED, GL45C.GL_FLOAT, new float[]{0.5f});
            GL45C.glClearTexImage(textures[3], 0, GL45C.GL_RG, GL45C.GL_FLOAT, new float[]{0, 0});
            GL45C.glFinish();
        } finally {
            check(DLSSFGNative.nativeUnlock() == 0, "input unlock");
        }
    }

    static void glFrame(long window, int channel, String label) {
        for (int frame = 0; frame < 3; ++frame) {
            GL45C.glBindFramebuffer(GL45C.GL_FRAMEBUFFER, 0);
            GL45C.glDrawBuffer(GL45C.GL_BACK);
            GL45C.glClearColor(channel == 0 ? 1 : 0, channel == 1 ? 1 : 0, channel == 2 ? 1 : 0, 1);
            GL45C.glClear(GL45C.GL_COLOR_BUFFER_BIT);
            if (worlds) {
                try {
                    var swap = MixinWindow.class.getDeclaredMethod("dlssmc$swapBuffers", long.class);
                    swap.setAccessible(true);
                    swap.invoke(null, window);
                } catch (ReflectiveOperationException e) {
                    throw new AssertionError(e);
                }
            } else {
                GLFW.glfwSwapBuffers(window);
            }
            GLFW.glfwPollEvents();
        }
        GL45C.glFinish();
        GL45C.glReadBuffer(GL45C.GL_FRONT);
        float[] pixel = new float[4];
        GL45C.glReadPixels(32, 32, 1, 1, GL45C.GL_RGBA, GL45C.GL_FLOAT, pixel);
        int error = GL45C.glGetError();
        System.out.printf("%s front=(%.2f,%.2f,%.2f) glError=0x%x debugErrors=%d%n",
                label, pixel[0], pixel[1], pixel[2], error, debugErrors);
        check(error == GL45C.GL_NO_ERROR && debugErrors == 0, label + ": GL error");
        check(pixel[channel] > 0.95f && pixel[(channel + 1) % 3] < 0.05f
                && pixel[(channel + 2) % 3] < 0.05f, label + ": stale GL front buffer");
        desktopPixel(window, channel, label);
    }

    static Object runtimeFieldRead(String name) throws Exception {
        var field = FGRuntime.class.getDeclaredField(name);
        field.setAccessible(true);
        return field.get(FGRuntime.get());
    }

    /**
     * 回归：某个后端初始化失败之后（initAttempted=true 而 nativeReady=false），
     * 换后端必须放行新的初始化尝试。旧版只在 nativeReady 为真时清闩锁，
     * 于是 FSR/XeSS 初始化一失败，连切回 DLSS 都再也起不来 —— 实机三个后端全灭。
     */
    static void failedBackendMustNotLatch() throws Exception {
        var fg = FGRuntime.get();
        runtimeField("backend", DLSSConfig.Backend.XESS);
        runtimeField("initAttempted", true);
        runtimeField("nativeReady", false);
        runtimeField("deviceReady", false);
        runtimeField("dxCaps", null);
        DLSSConfig.XESS_FRAME_GEN.set(DLSSConfig.FrameGen.X2);
        DLSSConfig.FRAME_GEN.set(DLSSConfig.FrameGen.OFF);
        fg.beginFrame();                    // 停在 XeSS；世界没激活，只走后端切换那一段
        DLSSConfig.makeExclusive(DLSSConfig.Backend.DLSS);
        DLSSConfig.FRAME_GEN.set(DLSSConfig.FrameGen.X2);
        fg.beginFrame();                    // 界面上切回 DLSS 走的就是这一条
        check(DLSSConfig.activeBackend() == DLSSConfig.Backend.DLSS, "生效后端应当切回 DLSS");
        check(runtimeFieldRead("backend") == DLSSConfig.Backend.DLSS, "运行时后端应当切回 DLSS");
        check(!(boolean) runtimeFieldRead("initAttempted"),
                "换后端必须清掉初始化闩锁，否则一个后端失败会连累所有后端");

        runtimeField("initAttempted", true);   // 再摆一次失败后的状态，验重进世界这条路
        fg.setWorldActive(true);
        check(!(boolean) runtimeFieldRead("initAttempted"), "重进世界必须允许重试初始化");
        fg.setWorldActive(false);
        // 这一段是摆出来的假状态；DLSS 原生侧其实一直活着，还原回去让收尾照常释放进程设备
        runtimeField("nativeReady", true);
        runtimeField("backend", DLSSConfig.Backend.DLSS);
        System.out.println("PASS: 后端初始化失败不会闩死其它后端（换后端与重进世界都能重试）");
    }

    public static void main(String[] args) throws Exception {
        visible = java.util.Arrays.asList(args).contains("--visible");
        worlds = java.util.Arrays.asList(args).contains("--worlds");
        if (worlds) {
            var config = com.electronwill.nightconfig.core.CommentedConfig.inMemory();
            DLSSConfig.SPEC.correct(config);
            var constructor = Class.forName("net.neoforged.fml.config.LoadedConfig").getDeclaredConstructor(
                    com.electronwill.nightconfig.core.CommentedConfig.class, Path.class,
                    net.neoforged.fml.config.ModConfig.class);
            constructor.setAccessible(true);
            DLSSConfig.SPEC.acceptConfig((net.neoforged.fml.config.IConfigSpec.ILoadedConfig)
                    constructor.newInstance(config, null, null));
            DLSSConfig.FRAME_GEN.set(DLSSConfig.FrameGen.X2);
            DLSSConfig.REFLEX_FPS_LIMIT.set(60);
        }
        Path dll = Path.of(args.length > 0 ? args[0] : "native/build/dlssmc_fg.dll").toAbsolutePath();
        String plugins = "D:\\Backup\\Downloads\\streamline-sdk-v2.14.1\\bin\\x64";
        System.load(Path.of(plugins, "sl.interposer.dll").toString());
        System.load(dll.toString());
        check(GLFW.glfwInit(), "GLFW init");
        GLFW.glfwWindowHint(GLFW.GLFW_VISIBLE, GLFW.GLFW_FALSE);
        GLFW.glfwWindowHint(GLFW.GLFW_FOCUSED, GLFW.GLFW_FALSE);
        GLFW.glfwWindowHint(GLFW.GLFW_FOCUS_ON_SHOW, GLFW.GLFW_FALSE);
        GLFW.glfwWindowHint(GLFW.GLFW_FLOATING, GLFW.GLFW_TRUE);
        GLFW.glfwWindowHint(GLFW.GLFW_CONTEXT_VERSION_MAJOR, 4);
        GLFW.glfwWindowHint(GLFW.GLFW_CONTEXT_VERSION_MINOR, 5);
        GLFW.glfwWindowHint(GLFW.GLFW_OPENGL_DEBUG_CONTEXT, GLFW.GLFW_TRUE);
        long window = GLFW.glfwCreateWindow(1280, 720, "FG toggle check", 0, 0);
        check(window != 0, "GL window");
        boolean initialized = false;
        boolean foreground = java.util.Arrays.asList(args).contains("--foreground");
        var previousForeground = foreground ? com.sun.jna.platform.win32.User32.INSTANCE.GetForegroundWindow() : null;
        int[] textures = new int[4];
        GLDebugMessageCallback callback = null;
        try {
            GLFW.glfwMakeContextCurrent(window);
            GL.createCapabilities();
            GLFW.glfwSwapInterval(0);
            callback = GLDebugMessageCallback.create((source, type, id, severity, length, message, user) -> {
                if (type == GL45C.GL_DEBUG_TYPE_ERROR) {
                    ++debugErrors;
                    System.out.println("GL ERROR: " + GLDebugMessageCallback.getMessage(length, message));
                }
            });
            GL45C.glEnable(GL45C.GL_DEBUG_OUTPUT);
            GL45C.glEnable(GL45C.GL_DEBUG_OUTPUT_SYNCHRONOUS);
            GL45C.glDebugMessageCallback(callback, 0);
            if (visible) {
                GLFW.glfwSetWindowPos(window, 32, 32);
                GLFW.glfwShowWindow(window);
                if (foreground) GLFW.glfwFocusWindow(window);
            }
            System.out.println("GPU: " + GL45C.glGetString(GL45C.GL_RENDERER));
            glFrame(window, 0, "initial GL");
            check(DLSSFGNative.nativeInit(plugins, dll.getParent().toString(), false) == 0, "SL init");
            initialized = true;
            check(DLSSFGNative.nativeCreateDevice(GLFWNativeWin32.glfwGetWin32Window(window),
                    1280, 720, true, 0) == 0, "device");
            check(DLSSFGNative.nativeIsSupported() == 1, "FG support");
            String[] model = DLSSFGNative.nativeGetModelVersions();
            System.out.println("模型：" + java.util.Arrays.toString(model));
            check(model.length == 3 && model[0].matches("DLSS-G \\d+\\.\\d+\\.\\d+\\.\\d+"),
                    "DLSS-G model version must be a real file version, got " + model[0]);
            check(model[2].equals("Streamline 2.14.1"), "SDK version must match the loaded plugin, got " + model[2]);
            bindTextures(textures);
            if (worlds) {
                for (String name : new String[]{"nativeReady", "deviceReady", "initAttempted"}) runtimeField(name, true);
            // ensureNative 正常会设这个字段；beginFrame 用它判断要不要换后端
            runtimeField("backend", DLSSConfig.Backend.DLSS);
            }
            float[] matrices = new float[64];
            Matrix4f projection = new Matrix4f().perspective((float) Math.toRadians(70), 1280f / 720, 0.05f, 1000);
            projection.get(matrices, 0);
            new Matrix4f().get(matrices, 16);
            projection.get(matrices, 32);
            new Matrix4f().get(matrices, 48);
            float[] params = {0.05f, 1000, (float) Math.toRadians(70), 1280f / 720, 0, 0, 0.5f, -0.5f};
            for (int cycle = 0; cycle < 3; ++cycle) {
                // 第三轮打开 UI 重合成标志，验证 enableUserInterfaceRecomposition 路径不报错
                uiOn = cycle == 2;
                DLSSFGNative.nativeSetTuning(false, true, true, true, uiOn);
                if (!worlds) bindTextures(textures);
                if (worlds) {
                    DLSSConfig.FG_UI_RECOMPOSITION.set(uiOn);
                    worldEvent("LoggingIn");
                    if (cycle == 0) inputBeforeFrame(window);
                    else FGRuntime.get().beginFrame();
                    check(child(window) != null, "joining another world must recreate the FG window");
                    check(!FGRuntime.get().presentFrame(), "new world must wait for fresh capture");
                } else {
                    check(DLSSFGNative.nativeSetEnabled(1) == 0, "enable");
                    // 回的是生效张数；本机 DLSS-G 上限小于请求档位时必须夹小并如实回报
                    check(DLSSFGNative.nativeSetMode(1, false) == 1, "mode");
                    int effective = DLSSFGNative.nativeSetMode(3, false);
                    check(effective >= 1 && effective <= 3, "4x 请求应当回生效张数，got " + effective);
                    check(DLSSFGNative.nativeSetMode(effective, false) == effective,
                            "回报的生效档位必须能被原样重新下发");
                }
                if (foreground) GLFW.glfwFocusWindow(window);
                int generated = 0;
                for (int frame = 0; frame < 120; ++frame) {
                    java.util.concurrent.locks.LockSupport.parkNanos(16_666_667L);
                    if (worlds) {
                        runtimeField("frameCaptured", true);
                        FGRuntime.get().beginFrame();
                        check(!FGRuntime.get().presentFrame(), "beginFrame must discard the previous capture");
                        FGRuntime.get().renderStart();
                    } else {
                        check(DLSSFGNative.nativeBeginFrame(true, 60) == 0, "begin");
                        DLSSFGNative.nativeRenderStart();
                    }
                    clearInputs(textures, (frame % 2) == 0 ? 0.2f : 0, 0, 1);
                    if (frame == 0) check(DLSSFGNative.nativeProbe() == 1, "input probe");
                    check(DLSSFGNative.nativePresent(matrices, params, frame == 0, 0) == 0, "present");
                    generated += Math.max(0, DLSSFGNative.nativeGetPresentedCount() - 1);
                    GLFW.glfwPollEvents();
                    if (frame == 59 && cycle == 1) {
                        if (!worlds) DLSSFGNative.nativeHidePresentation();
                        glFrame(window, 1, "temporary GL fallback");
                    }
                    if (worlds && frame == 89 && cycle == 1) {
                        var previousChild = child(window);
                        DLSSConfig.FRAME_GEN.set(DLSSConfig.FrameGen.OFF);
                        FGRuntime.get().beginFrame();
                        check(previousChild.equals(child(window)), "OFF must wait for the new GL front buffer");
                        glFrame(window, 1, "configured OFF");
                        check(child(window) == null, "OFF must release the FG window after swap");
                        DLSSConfig.FRAME_GEN.set(DLSSConfig.FrameGen.X2);
                    }
                    if (frame == 59 && cycle == 2) {
                        width = 1360;
                        height = 768;
                        GLFW.glfwSetWindowSize(window, width, height);
                        GLFW.glfwPollEvents();
                        check(DLSSFGNative.nativeResize(width, height) == 0, "resize");
                        bindTextures(textures);
                        params[3] = (float) width / height;
                        projection.identity().perspective(params[2], params[3], params[0], params[1]);
                        projection.get(matrices, 0);
                        projection.get(matrices, 32);
                        glFrame(window, 1, "GL during resize warmup");
                    }
                }
                System.out.println("cycle=" + cycle + " generated=" + generated
                        + " 桥接ms=" + java.util.Arrays.toString(DLSSFGNative.nativeGetBridgeTimings()));
                System.out.println("cycle=" + cycle
                        + " focused=" + GLFW.glfwGetWindowAttrib(window, GLFW.GLFW_FOCUSED)
                        + " foreground=" + com.sun.jna.platform.win32.User32.INSTANCE.GetForegroundWindow()
                        + " parent=0x" + Long.toHexString(GLFWNativeWin32.glfwGetWin32Window(window))
                        + " child=" + com.sun.jna.NativeLibrary.getInstance("user32").getFunction("GetWindow")
                                .invokePointer(new Object[]{new com.sun.jna.Pointer(GLFWNativeWin32.glfwGetWin32Window(window)), 5}));
                desktopPixel(window, 2, "FG blue");
                if (java.util.Arrays.asList(args).contains("--worlds")) {
                    logout(window);
                } else {
                    check(DLSSFGNative.nativeSetEnabled(0) == 0, "disable");
                }
                glFrame(window, 1, "after FG OFF green");
                glFrame(window, 0, "after FG OFF red");
                check(!foreground || generated > 0, "FG did not generate frames (requires foreground ownership)");
                if (java.util.Arrays.asList(args).contains("--reinitialize") && cycle < 2) {
                    System.out.println("Reinitializing Streamline after world " + cycle);
                    DLSSFGNative.nativeShutdown();
                    initialized = false;
                    check(DLSSFGNative.nativeInit(plugins, dll.getParent().toString(), false) == 0, "SL reinit");
                    initialized = true;
                    check(DLSSFGNative.nativeCreateDevice(GLFWNativeWin32.glfwGetWin32Window(window),
                            width, height, true, 0) == 0, "recreated device");
                }
            }
            if (worlds) failedBackendMustNotLatch();
            if (worlds) FGRuntime.get().shutdown();
            else DLSSFGNative.nativeShutdown();
            initialized = false;
            check(DLSSFGNative.nativeIsSupported() == 0, "client shutdown must release the process device");
            glFrame(window, 1, "after shutdown");
            if (worlds) System.out.println("PASS: production logout/login and beginFrame, stale capture rejection, deferred OFF/GL swap, three worlds with retained device/inputs, final shutdown");
            System.out.println(visible ? "PASS: production presentation toggle cycles, temporary fallback, resize, shutdown, GL and desktop pixels"
                    : "PASS: hidden-window toggle, fallback, resize and GL buffers; desktop output not verified");
            System.out.println(foreground ? "PASS: every cycle generated extra frames"
                    : "Generation not required in background mode; use --visible --foreground for generation assertions");
        } finally {
            if (initialized) DLSSFGNative.nativeShutdown();
            GL45C.glDeleteTextures(textures);
            GL45C.glDebugMessageCallback(null, 0);
            if (callback != null) callback.free();
            GLFW.glfwDestroyWindow(window);
            GLFW.glfwTerminate();
            if (previousForeground != null) com.sun.jna.platform.win32.User32.INSTANCE.SetForegroundWindow(previousForeground);
        }
    }
}
