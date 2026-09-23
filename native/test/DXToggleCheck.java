import com.taolesi.dlssmc.nativebridge.DLSSDXNative;
import org.joml.Matrix4f;
import org.lwjgl.glfw.GLFW;
import org.lwjgl.glfw.GLFWNativeWin32;
import org.lwjgl.opengl.GL;
import org.lwjgl.opengl.GL45C;

import java.nio.file.Path;

/**
 * D3D12 后端的独立回归：真 GL 上下文 + 真厂商 DLL，窗口不显示、不抢前台。
 *
 * 判据分两层，缺一不可：
 *   1) 交付层：nativeProbe 必须看到 GL 画的非黑画面（证明 GL→D3D11→跨 API→D3D12 通了）；
 *   2) 插帧层：XeSS 分支稳定期 nativeGetPresentedCount 必须 >1（证明真的多吐了帧）。
 * passthrough 分支只验第 1 层 + 每帧 Present 成功。
 * 加 --visible --foreground 时还有第三层：桌面取像素必须非黑。计数与预检全绿而屏幕全黑
 * 是真实出现过的缺陷形态（XeSS 分支漏写代理 backbuffer），只跑前两层测不出来。
 *
 * 超分（SR）段落额外验两件事：
 *   a) 反向交付 —— 渲染分辨率的世界颜色经厂商超分后，必须能从 GL 侧读回呈现分辨率的结果，
 *      而且上下两半的顺序不能反（读回的是绑定时的输出纹理，颜色对得上才算真的写回来了）；
 *   b) 混合尺寸喂 FG —— 深度/MV 留在渲染档、两张颜色在呈现档时，FG 仍然多吐帧、桌面像素仍然亮。
 * (a) 是实机画面的唯一来源：SR 的输出要贴回 MC 主 target，手持物品和 HUD 才不用跟着被放大。
 */
public class DXToggleCheck {
    static int width = 1280, height = 720;
    /** 输入尺寸由厂商 API 返回。 */
    static int rw = width, rh = height;
    static boolean visible, foreground;
    /** 这一帧要在反向交付的锁窗口里读回 SR 输出（只有超分段落的最后一帧开） */
    static boolean readOutput;
    static long windowHandle;
    static int worldColor, mainColor;
    static int srQuality = DLSSDXNative.SR_OFF;
    static java.lang.reflect.Method copyImage;

    static void copy(int src, int dst, int w, int h) {
        try {
            copyImage.invoke(com.taolesi.dlssmc.core.FGRuntime.get(), src, dst, w, h);
        } catch (ReflectiveOperationException e) {
            throw new AssertionError(e);
        }
    }

    static void configureSr(int quality, boolean sharpen, float sharpness) {
        check(DLSSDXNative.nativeConfigureSR(quality, sharpen, sharpness) == 0, "configure SR " + quality);
        srQuality = quality;
        if (!srOn()) {
            rw = width;
            rh = height;
            return;
        }
        int[] size = DLSSDXNative.nativeGetRenderSize(quality);
        check(size.length == 2 && size[0] > 0 && size[1] > 0
                && size[0] <= width && size[1] <= height, "厂商尺寸无效 " + java.util.Arrays.toString(size));
        rw = size[0];
        rh = size[1];
        System.out.println("厂商建议 " + quality + " -> " + rw + "x" + rh);
    }

    /** 世界纹理的上下两半：SR 输出读回时靠它判断有没有被翻转 */
    static final float[] WORLD_BOTTOM = {0.15f, 0.35f, 0.85f};
    static final float[] WORLD_TOP = {0.95f, 0.85f, 0.15f};

    /** Native AA 同样经过 SR，即使输入输出等大。 */
    static boolean srOn() {
        return srQuality >= 0;
    }

    static void check(boolean cond, String msg) {
        if (!cond) throw new AssertionError(msg);
    }

    static int tex(int glFormat, int w, int h) {
        int t = GL45C.glGenTextures();
        GL45C.glBindTexture(GL45C.GL_TEXTURE_2D, t);
        int components = glFormat == GL45C.GL_R32F ? GL45C.GL_RED
                : glFormat == GL45C.GL_RG16F ? GL45C.GL_RG : GL45C.GL_RGBA;
        GL45C.glTexImage2D(GL45C.GL_TEXTURE_2D, 0, glFormat, w, h, 0, components, GL45C.GL_FLOAT, 0L);
        GL45C.glBindTexture(GL45C.GL_TEXTURE_2D, 0);
        return t;
    }

    static void clearTo(int texture, float r, float g, float b, int w, int h) {
        int fbo = GL45C.glGenFramebuffers();
        GL45C.glBindFramebuffer(GL45C.GL_FRAMEBUFFER, fbo);
        GL45C.glFramebufferTexture2D(GL45C.GL_FRAMEBUFFER, GL45C.GL_COLOR_ATTACHMENT0,
                GL45C.GL_TEXTURE_2D, texture, 0);
        GL45C.glViewport(0, 0, w, h);
        GL45C.glClearColor(r, g, b, 1f);
        GL45C.glClear(GL45C.GL_COLOR_BUFFER_BIT);
        GL45C.glBindFramebuffer(GL45C.GL_FRAMEBUFFER, 0);
        GL45C.glDeleteFramebuffers(fbo);
    }

    /** 世界纹理画成上下两半不同色（GL 取向：y=0 在下），用来验 SR 输出的朝向 */
    static void clearHalves(int texture, int w, int h) {
        int fbo = GL45C.glGenFramebuffers();
        GL45C.glBindFramebuffer(GL45C.GL_FRAMEBUFFER, fbo);
        GL45C.glFramebufferTexture2D(GL45C.GL_FRAMEBUFFER, GL45C.GL_COLOR_ATTACHMENT0,
                GL45C.GL_TEXTURE_2D, texture, 0);
        GL45C.glViewport(0, 0, w, h);
        GL45C.glEnable(GL45C.GL_SCISSOR_TEST);
        GL45C.glScissor(0, 0, w, h / 2);
        GL45C.glClearColor(WORLD_BOTTOM[0], WORLD_BOTTOM[1], WORLD_BOTTOM[2], 1f);
        GL45C.glClear(GL45C.GL_COLOR_BUFFER_BIT);
        GL45C.glScissor(0, h / 2, w, h - h / 2);
        GL45C.glClearColor(WORLD_TOP[0], WORLD_TOP[1], WORLD_TOP[2], 1f);
        GL45C.glClear(GL45C.GL_COLOR_BUFFER_BIT);
        GL45C.glDisable(GL45C.GL_SCISSOR_TEST);
        GL45C.glBindFramebuffer(GL45C.GL_FRAMEBUFFER, 0);
        GL45C.glDeleteFramebuffers(fbo);
    }

    static void bindTexturesInto(int[] t, boolean toDx) {
        int[] previous = t.clone();
        t[0] = tex(GL45C.GL_RGBA8, width, height);
        t[1] = tex(GL45C.GL_RGBA8, width, height);
        t[2] = tex(GL45C.GL_R32F, rw, rh);
        t[3] = tex(GL45C.GL_RG16F, rw, rh);
        t[4] = t[5] = 0;
        GL45C.glDeleteTextures(worldColor);
        GL45C.glDeleteTextures(mainColor);
        worldColor = mainColor = 0;
        int r;
        if (toDx) {
            check(DLSSDXNative.nativeUnbindTextures() == 0, "DX 解绑后重绑");
            if (srOn()) {
                worldColor = tex(GL45C.GL_RGBA8, rw, rh);
                mainColor = tex(GL45C.GL_RGBA8, width, height);
                t[4] = tex(GL45C.GL_RGBA8, rw, rh);
                t[5] = tex(GL45C.GL_RGBA8, width, height);
            }
            r = DLSSDXNative.nativeBindTextures(t[0], t[1], t[2], t[3], t[4], t[5], rw, rh);
        } else {
            r = com.taolesi.dlssmc.nativebridge.DLSSFGNative.nativeBindTextures(
                    t[0], t[1], t[2], t[3], 0, 0, rw, rh);
        }
        check(r == 0, (toDx ? "DX" : "DLSS") + " 绑定输入 -> " + r);
        GL45C.glDeleteTextures(previous);
    }

    static void bind(int[] t) {
        bindTexturesInto(t, true);
    }

    /** 锁内 GL 写入：每个输入各清成不同值 */
    static void clearAll(int[] t) {
        clearTo(t[0], 0.8f, 0.2f, 0.2f, width, height);
        clearTo(t[1], 0.2f, 0.8f, 0.2f, width, height);
        clearTo(t[2], 0.5f, 0f, 0f, rw, rh);
        clearTo(t[3], 0.004f, -0.002f, 0f, rw, rh);
        if (t[4] != 0) clearHalves(t[4], rw, rh);
        GL45C.glFinish();
    }

    /** 一帧：锁 -> GL 画 -> 交付 ->（超分开着就跑反向交付，锁内验一次输出）-> 预检 -> Present */
    static int frame(int[] t, float[] matrices, float[] params, boolean reset, boolean expectProbe) {
        check(DLSSDXNative.nativeLock(srOn() ? 2 : 1, 3) == 0, "world lock");
        try {
            clearTo(t[2], 0.5f, 0f, 0f, rw, rh);
            clearTo(t[3], 0f, 0f, 0f, rw, rh);
            if (srOn()) {
                clearHalves(worldColor, rw, rh);
                copy(worldColor, t[4], rw, rh);
            } else clearTo(t[1], 0.2f, 0.8f, 0.2f, width, height);
        } finally {
            GL45C.glFinish();
            check(DLSSDXNative.nativeDeliver() == 0, "world deliver");
        }
        if (srOn()) {
            check(DLSSDXNative.nativeUpscale(params, reset) == 0, "超分反向交付");
            try {
                copy(t[5], mainColor, width, height);
                if (readOutput) checkSrOutput(mainColor);
            } finally {
                GL45C.glFinish();
                check(DLSSDXNative.nativeUpscaleDone() == 0, "upscaleDone");
            }
            int fbo = GL45C.glCreateFramebuffers();
            GL45C.glNamedFramebufferTexture(fbo, GL45C.GL_COLOR_ATTACHMENT0, mainColor, 0);
            GL45C.glBindFramebuffer(GL45C.GL_FRAMEBUFFER, fbo);
            GL45C.glEnable(GL45C.GL_SCISSOR_TEST);
            GL45C.glScissor(width / 2, 8, 1, 1);
            GL45C.glClearColor(1, 0, 1, 1);
            GL45C.glClear(GL45C.GL_COLOR_BUFFER_BIT);
            GL45C.glDisable(GL45C.GL_SCISSOR_TEST);
            if (readOutput) {
                java.nio.ByteBuffer pixel = java.nio.ByteBuffer.allocateDirect(8);
                GL45C.glReadPixels(width / 2, 8, 2, 1, GL45C.GL_RGBA, GL45C.GL_UNSIGNED_BYTE, pixel);
                check((pixel.get(0) & 255) == 255 && (pixel.get(1) & 255) == 0
                        && (pixel.get(2) & 255) == 255 && (pixel.get(4) & 255) < 100,
                        "HUD 单像素被放大或模糊");
            }
            GL45C.glBindFramebuffer(GL45C.GL_FRAMEBUFFER, 0);
            GL45C.glDeleteFramebuffers(fbo);
        }
        check(DLSSDXNative.nativeLock(0, 1) == 0, "final lock");
        try {
            if (srOn()) copy(mainColor, t[0], width, height);
            else clearTo(t[0], 0.8f, 0.2f, 0.2f, width, height);
        } finally {
            GL45C.glFinish();
            check(DLSSDXNative.nativeDeliver() == 0, "final deliver");
        }
        if (expectProbe) check(DLSSDXNative.nativeProbe() == 1, "D3D12 侧没看到 GL 画的画面");
        int r = DLSSDXNative.nativePresent(matrices, params, reset);
        check(r == 0, "present -> " + r);
        return DLSSDXNative.nativeGetPresentedCount();
    }

    /**
     * 把 SR 的输出纹理挂上 FBO 读回上下两处（GL 取向：y=0 在下）。
     * 挂不上就回 null —— 互操作注册过的纹理只有锁内才能被 GL 用，这不是错误而是判据。
     * 用 FBO + glReadPixels 而不是 glGetTexImage：实机就是把它挂 FBO 上 blit 回主 target 的，
     * 而且锁外走 glGetTexImage 会直接在驱动里炸（本机实测 0xc0000005，连 hs_err 都写不出来）。
     */
    static int[][] readSrOutput(int outTex) {
        int fbo = GL45C.glGenFramebuffers();
        GL45C.glBindFramebuffer(GL45C.GL_FRAMEBUFFER, fbo);
        GL45C.glFramebufferTexture2D(GL45C.GL_FRAMEBUFFER, GL45C.GL_COLOR_ATTACHMENT0,
                GL45C.GL_TEXTURE_2D, outTex, 0);
        int[][] out = null;
        if (GL45C.glCheckFramebufferStatus(GL45C.GL_FRAMEBUFFER) == GL45C.GL_FRAMEBUFFER_COMPLETE) {
            java.nio.ByteBuffer buf = java.nio.ByteBuffer.allocateDirect(width * height * 4);
            GL45C.glReadBuffer(GL45C.GL_COLOR_ATTACHMENT0);
            GL45C.glReadPixels(0, 0, width, height, GL45C.GL_RGBA, GL45C.GL_UNSIGNED_BYTE, buf);
            out = new int[][]{sample(buf, height / 4), sample(buf, height * 3 / 4)};
        }
        GL45C.glBindFramebuffer(GL45C.GL_FRAMEBUFFER, 0);
        GL45C.glDeleteFramebuffers(fbo);
        return out;
    }

    /**
     * SR 的输出必须真的回到 GL：颜色要对得上世界纹理的上下两半。
     * 全黑 = 反向交付没通；两半对调 = 朝向反了（实机就是天地颠倒）。
     */
    static void checkSrOutput(int outTex) {
        int[][] px = readSrOutput(outTex);
        check(px != null, "SR 输出纹理在锁内应当能挂上 FBO 读回");
        System.out.printf("超分输出读回 %dx%d：下半 RGB=(%d,%d,%d) 期望≈%s，上半 RGB=(%d,%d,%d) 期望≈%s%n",
                width, height, px[0][0], px[0][1], px[0][2], expect(WORLD_BOTTOM),
                px[1][0], px[1][1], px[1][2], expect(WORLD_TOP));
        check(close(px[0], WORLD_BOTTOM) && close(px[1], WORLD_TOP),
                "SR 的输出没有正确写回 GL 纹理：下半=" + java.util.Arrays.toString(px[0])
                        + " 上半=" + java.util.Arrays.toString(px[1])
                        + "（全黑=反向交付没通，两半对调=朝向反了）");
    }

    static int[] sample(java.nio.ByteBuffer buf, int y) {
        int off = (y * width + width / 2) * 4;
        return new int[]{buf.get(off) & 0xFF, buf.get(off + 1) & 0xFF, buf.get(off + 2) & 0xFF};
    }

    static boolean close(int[] got, float[] want) {
        for (int i = 0; i < 3; ++i) if (Math.abs(got[i] - want[i] * 255f) > 24f) return false;
        return true;
    }

    static String expect(float[] c) {
        return "(" + (int) (c[0] * 255) + "," + (int) (c[1] * 255) + "," + (int) (c[2] * 255) + ")";
    }

    /** 子窗口中心点的桌面像素：代理呈现真的贴上屏幕才算数，全黑即呈现链断了 */
    static void desktopPixel(String label) {
        if (!visible || !foreground) {
            System.out.println(label + "：跳过桌面像素（需 --visible --foreground）");
            return;
        }
        var hwnd = new com.sun.jna.platform.win32.WinDef.HWND(
                new com.sun.jna.Pointer(GLFWNativeWin32.glfwGetWin32Window(windowHandle)));
        var point = new com.sun.jna.platform.win32.WinDef.POINT(width / 2, height / 2);
        check(com.sun.jna.NativeLibrary.getInstance("user32").getFunction("ClientToScreen")
                .invokeInt(new Object[]{hwnd, point}) != 0, "client position");
        var user32 = com.sun.jna.platform.win32.User32.INSTANCE;
        var dc = user32.GetDC(null);
        int color = 0;
        try {
            for (int attempt = 0; attempt < 90; ++attempt) {
                com.sun.jna.NativeLibrary.getInstance("dwmapi").getFunction("DwmFlush")
                        .invokeInt(new Object[]{});
                color = com.sun.jna.NativeLibrary.getInstance("gdi32").getFunction("GetPixel")
                        .invokeInt(new Object[]{dc, point.x, point.y});
                if (lit(color)) break;
                java.util.concurrent.locks.LockSupport.parkNanos(16_666_667L);
            }
        } finally {
            user32.ReleaseDC(null, dc);
        }
        System.out.printf("%s：桌面像素 #%06X%n", label, color & 0xFFFFFF);
        check(lit(color), label + "：桌面像素全黑（计数绿 ≠ 屏幕亮）got #"
                + Integer.toHexString(color));
    }

    static boolean lit(int color) {
        return color != 0xFFFFFFFF && ((color & 0xFF) > 40 || ((color >> 8) & 0xFF) > 40
                || ((color >> 16) & 0xFF) > 40);
    }

    /**
     * 超分段落：低分辨率输入跑若干帧，一起验四件事 ——
     * SR 的输出在锁窗口内反向写回 GL（颜色与朝向都对）、解锁后 GL 就读不到了（锁是必需的）、
     * FG 在「颜色呈现档 + 深度/MV 渲染档」下仍然多吐帧、桌面像素仍然亮。
     */
    static void srRun(String label, int[] t, float[] matrices, float[] params, int frames) {
        int max = 0;
        for (int i = 0; i < frames; ++i) {
            java.util.concurrent.locks.LockSupport.parkNanos(16_666_667L);
            readOutput = i == frames - 1;   // 最后一帧在锁窗口里读回 SR 的输出
            max = Math.max(max, frame(t, matrices, params, i == 0, i < 3));
            if (i == 0) DLSSDXNative.nativeShowPresentation();
            GLFW.glfwPollEvents();
        }
        readOutput = false;
        System.out.println(label + "：渲染 " + rw + "x" + rh + " -> 呈现 " + width + "x" + height
                + "，单帧最多送呈 " + max + " 帧");
        // 该失败案例：解锁之后同一张纹理 GL 就用不了了，所以反向交付必须成对 lock/unlock。
        // 省掉这层锁不报错，实机表现是主 target 拿不到 SR 结果（黑屏或残影）。
        check(readSrOutput(t[5]) == null,
                "解锁后 SR 输出纹理居然还能被 GL 读，那锁内那次读回就证明不了「只有锁内可见」");
        if (foreground) GLFW.glfwFocusWindow(windowHandle);
        desktopPixel(label + " 上屏");
        check(max > 1, label + "：开了超分就不插帧了（maxPresented=" + max + "）");
    }

    public static void main(String[] args) throws Exception {
        visible = java.util.Arrays.asList(args).contains("--visible");
        foreground = java.util.Arrays.asList(args).contains("--foreground");
        Path dll = Path.of(args.length > 0 ? args[0] : "native/build/dlssmc_dx.dll").toAbsolutePath();
        // 厂商 DLL 必须走生产解压路径（jar 资源 -> 缓存目录 -> 交给原生层）。
        // 直接用 native/vendor 会让解压布局的 bug 完全测不出来，实机就是栽在这儿。
        Path extractCache = Path.of(System.getProperty("java.io.tmpdir"), "dlssmc-natives-dxcheck");
        Path vendor = DLSSDXNative.extractVendor(extractCache);
        check(java.nio.file.Files.isRegularFile(vendor.resolve("intel/bin/libxess_fg.dll")),
                "解压根下面必须直接是 intel/bin，got " + vendor);
        check(java.nio.file.Files.isRegularFile(vendor.resolve("intel/bin/libxell.dll")),
                "libxell.dll 必须和 libxess_fg.dll 同目录");
        check(java.nio.file.Files.isRegularFile(
                        vendor.resolve("amd/bin/amd_fidelityfx_framegeneration_dx12.dll")),
                "FSR provider 必须解到 amd/bin 下");
        check(DLSSDXNative.extractVendor(extractCache).equals(vendor),
                "同一进程内重复解压必须复用同一份，否则会去覆盖自己已加载的 DLL");
        Path logs = Path.of("native/build").toAbsolutePath();
        // 走生产加载路径：DLL 从 classpath 资源解包出来再 load（资源没打进 jar 就会在这里失败）
        check(DLSSDXNative.load(vendor, Path.of(System.getProperty("java.io.tmpdir"),
                "dlssmc-natives")), "加载 dlssmc_dx.dll 失败: " + DLSSDXNative.getLastError());
        check(dll.toFile().exists(), "先跑 native/build_dx.sh");

        check(GLFW.glfwInit(), "GLFW init");
        GLFW.glfwWindowHint(GLFW.GLFW_VISIBLE, GLFW.GLFW_FALSE);
        GLFW.glfwWindowHint(GLFW.GLFW_FOCUS_ON_SHOW, GLFW.GLFW_FALSE);
        GLFW.glfwWindowHint(GLFW.GLFW_CONTEXT_VERSION_MAJOR, 4);
        GLFW.glfwWindowHint(GLFW.GLFW_CONTEXT_VERSION_MINOR, 5);
        long window = GLFW.glfwCreateWindow(width, height, "DX backend check", 0, 0);
        check(window != 0, "GL window");
        windowHandle = window;
        int[] textures = new int[6];
        int[] copyFbos = new int[2];
        boolean dlssLoaded = false;
        try {
            GLFW.glfwMakeContextCurrent(window);
            GL.createCapabilities();
            Class<?> runtime = com.taolesi.dlssmc.core.FGRuntime.class;
            copyImage = runtime.getDeclaredMethod("copyImage", int.class, int.class, int.class, int.class);
            copyImage.setAccessible(true);
            String[] fields = {"copyReadFbo", "copyDrawFbo"};
            for (int i = 0; i < fields.length; ++i) {
                var field = runtime.getDeclaredField(fields[i]);
                field.setAccessible(true);
                copyFbos[i] = GL45C.glCreateFramebuffers();
                field.setInt(com.taolesi.dlssmc.core.FGRuntime.get(), copyFbos[i]);
            }
            GLFW.glfwSwapInterval(0);
            if (visible) {
                GLFW.glfwSetWindowPos(window, 32, 32);
                GLFW.glfwShowWindow(window);
                if (foreground) GLFW.glfwFocusWindow(window);
            }
            System.out.println("GPU: " + GL45C.glGetString(GL45C.GL_RENDERER));
            check(DLSSDXNative.nativeInit(vendor.toString(), logs.toString()) == 0, "nativeInit");

            String[] caps = DLSSDXNative.nativeGetCapabilities();
            System.out.println("能力（建设备前）：" + java.util.Arrays.toString(caps));
            check(caps.length == 8, "能力表应当有 8 项，got " + caps.length);
            check(caps[0].matches("\\d+\\.\\d+\\.\\d+"),
                    "XeLL 版本应当是查出来的三段号，got " + caps[0]);
            check(caps[3].matches("\\d+\\.\\d+\\.\\d+\\.\\d+"),
                    "FSR 内置 DLL 版本应当是四段号，got " + caps[3]);
            check(caps[4].matches("\\d+\\.\\d+\\.\\d+\\.\\d+"),
                    "XeSS 内置 DLL 版本应当是四段号，got " + caps[4]);
            check(caps[5].equals("是"), "FSR 应能加载（loader+provider 都在 vendor 下），got " + caps[5]);
            check(caps[6].matches("可用 \\d+\\.\\d+\\.\\d+"),
                    "XeSS-SR（libxess.dll）应可用并查出版本，got " + caps[6]);
            check(!caps[7].startsWith("不可用"), "FSR-SR（upscaler provider）应可用，got " + caps[7]);

            float[] matrices = new float[64];
            new Matrix4f().perspective((float) Math.toRadians(70), (float) width / height, 0.05f, 1000)
                    .get(matrices, 0);
            new Matrix4f().get(matrices, 16);
            new Matrix4f().get(matrices, 32);
            new Matrix4f().get(matrices, 48);
            float[] params = {0.05f, 1000, (float) Math.toRadians(70), (float) width / height,
                    0, 0, 0.5f, -0.5f};

            // ---- A) passthrough：只验交付与呈现 ----
            check(DLSSDXNative.nativeCreateDevice(GLFWNativeWin32.glfwGetWin32Window(window),
                    width, height, DLSSDXNative.BACKEND_PASSTHROUGH) == 0, "passthrough 设备");
            check(DLSSDXNative.nativeConfigureSR(DLSSDXNative.XESS_QUALITY, false, 0f) < 0,
                    "passthrough 后端没有超分通道，configureSR 必须拒绝而不是静默收下");
            bind(textures);
            for (int i = 0; i < 20; ++i) frame(textures, matrices, params, i == 0, true);
            System.out.println("PASS: passthrough 交付 + 20 帧 Present，D3D12 侧看到 GL 画面");
            DLSSDXNative.nativeShutdown();

            // ---- B) XeSS：验真插帧 ----
            check(DLSSDXNative.nativeInit(vendor.toString(), logs.toString()) == 0, "SL re-init");
            check(DLSSDXNative.nativeCreateDevice(GLFWNativeWin32.glfwGetWin32Window(window),
                    width, height, DLSSDXNative.BACKEND_XESS) == 0, "XeSS 设备");
            String[] after = DLSSDXNative.nativeGetCapabilities();
            System.out.println("能力（XeSS 设备后）：" + java.util.Arrays.toString(after));
            check(Integer.parseInt(after[1]) >= 1, "本机最大插帧数应当 >=1，got " + after[1]);
            check(DLSSDXNative.nativeSetInterpolatedFrames(3) == 1,
                    "XeSS 要把 4x 夹到本机上限，got " + DLSSDXNative.nativeSetInterpolatedFrames(3));
            check(DLSSDXNative.nativeSetInterpolatedFrames(1) == 1, "XeSS 每张 1 帧插帧应当被接受");
            bind(textures);
            int maxPresented = 0;
            for (int i = 0; i < 120; ++i) {
                java.util.concurrent.locks.LockSupport.parkNanos(16_666_667L);
                maxPresented = Math.max(maxPresented,
                        frame(textures, matrices, params, i == 0, i < 3));
                if (i == 0) DLSSDXNative.nativeShowPresentation();
                GLFW.glfwPollEvents();
            }
            System.out.println("XeSS 单帧最多送呈 " + maxPresented + " 帧");
            check(maxPresented > 1, "XeSS 没真的多吐帧（maxPresented=" + maxPresented + "）");
            if (foreground) GLFW.glfwFocusWindow(window);
            desktopPixel("XeSS 上屏");

            configureSr(DLSSDXNative.XESS_PERFORMANCE, false, 0f);
            check(rw < width && rh < height, "XeSS Performance 必须降低世界分辨率");
            check(DLSSDXNative.nativeConfigureSR(DLSSDXNative.FSR_QUALITY, false, 0f) == -4,
                    "XeSS 后端不能拿 FSR 的档位号");
            bind(textures);
            srRun("XeSS 超分", textures, matrices, params, 60);
            configureSr(DLSSDXNative.XESS_AA, false, 0f);
            check(rw == width && rh == height, "XeSS AA 必须保持原生分辨率");
            bind(textures);
            srRun("XeSS AA 档位切换", textures, matrices, params, 30);
            configureSr(DLSSDXNative.SR_OFF, false, 0f);
            bind(textures);
            for (int i = 0; i < 10; ++i) frame(textures, matrices, params, i == 0, i < 3);
            System.out.println("PASS: XeSS 厂商尺寸、生产双翻转、原生 HUD、SR/AA/关闭热切换");
            DLSSDXNative.nativeShutdown();

            // FSR 回调统计只证明命令派发，不是显示器呈现遥测。
            check(DLSSDXNative.nativeInit(vendor.toString(), logs.toString()) == 0, "re-init");
            check(DLSSDXNative.nativeCreateDevice(GLFWNativeWin32.glfwGetWin32Window(window),
                    width, height, DLSSDXNative.BACKEND_FSR) == 0, "FSR 设备");
            String[] fsr = DLSSDXNative.nativeGetCapabilities();
            System.out.println("能力（FSR 设备后）：" + java.util.Arrays.toString(fsr));
            check(fsr[2].matches("\\d+\\.\\d+\\.\\d+"), "FSR provider 版本应当是查出来的，got " + fsr[2]);
            check(DLSSDXNative.nativeSetInterpolatedFrames(2) < 0,
                    "FSR 的生成张数由库决定，不能假装接受调节");
            bind(textures);
            int fsrMax = 0;
            long[] fsrSubmitNanos = new long[90];
            for (int i = 0; i < 120; ++i) {
                java.util.concurrent.locks.LockSupport.parkNanos(16_666_667L);
                long start = System.nanoTime();
                fsrMax = Math.max(fsrMax, frame(textures, matrices, params, i == 0, i < 3));
                if (i >= 30) fsrSubmitNanos[i - 30] = System.nanoTime() - start;
                if (i == 0) DLSSDXNative.nativeShowPresentation();
                GLFW.glfwPollEvents();
            }
            java.util.Arrays.sort(fsrSubmitNanos);
            System.out.printf("FSR SR-off CPU bridge+submit ms: p50=%.3f p95=%.3f p99=%.3f (not display pacing)%n",
                    fsrSubmitNanos[44] / 1e6, fsrSubmitNanos[85] / 1e6, fsrSubmitNanos[89] / 1e6);
            System.out.println("FSR 单帧最多提交计数 " + fsrMax + " 帧（非实测呈现，provider " + fsr[2] + "）");
            check(fsrMax > 1, "FSR 未派发额外帧（maxSubmitted=" + fsrMax + "）");
            if (foreground) GLFW.glfwFocusWindow(window);
            desktopPixel("FSR 上屏");

            configureSr(DLSSDXNative.FSR_PERFORMANCE, true, 0.7f);
            check(rw < width && rh < height, "FSR Performance 必须降低世界分辨率");
            check(DLSSDXNative.nativeConfigureSR(99, true, 0.7f) == -4,
                    "FSR 档位超出厂商枚举应当被拒");
            bind(textures);
            srRun("FSR 超分", textures, matrices, params, 60);
            configureSr(DLSSDXNative.FSR_NATIVE_AA, false, 0f);
            check(rw == width && rh == height, "FSR Native AA 必须保持原生分辨率");
            bind(textures);
            srRun("FSR Native AA 档位切换", textures, matrices, params, 30);
            configureSr(DLSSDXNative.SR_OFF, false, 0f);
            bind(textures);
            for (int i = 0; i < 10; ++i) frame(textures, matrices, params, i == 0, i < 3);
            System.out.println("PASS: FSR 厂商尺寸、生产双翻转、原生 HUD、SR/AA/关闭热切换");
            DLSSDXNative.nativeShutdown();

            // ---- D) 换后端的原生顺序：解绑 DLSS 输入 -> 建 DX -> 再回到 DLSS，全程不 slShutdown ----
            // 这正是跨存档崩溃那类问题的雷区：进程内重复 slInit 会踩失效的插件函数指针。
            String sl = "D:\\Backup\\Downloads\\streamline-sdk-v2.14.1\\bin\\x64";
            java.nio.file.Path cache = Path.of(System.getProperty("java.io.tmpdir"), "dlssmc-natives");
            check(com.taolesi.dlssmc.nativebridge.DLSSFGNative.load(Path.of(sl), cache),
                    "加载 DLSS 桥失败: " + com.taolesi.dlssmc.nativebridge.DLSSFGNative.getLastError());
            dlssLoaded = true;
            check(com.taolesi.dlssmc.nativebridge.DLSSFGNative.nativeInit(sl, logs.toString(), false) == 0, "slInit");
            long hwnd = GLFWNativeWin32.glfwGetWin32Window(window);
            for (int round = 0; round < 2; ++round) {
                check(com.taolesi.dlssmc.nativebridge.DLSSFGNative.nativeCreateDevice(
                        hwnd, width, height, true, 0) == 0, "第 " + round + " 轮 DLSS 设备");
                bindTexturesInto(textures, false);
                check(com.taolesi.dlssmc.nativebridge.DLSSFGNative.nativeSetEnabled(1) == 0, "DLSS 开");
                check(com.taolesi.dlssmc.nativebridge.DLSSFGNative.nativeSetMode(1, false) == 1, "DLSS 档位");
                for (int i = 0; i < 30; ++i) {
                    check(com.taolesi.dlssmc.nativebridge.DLSSFGNative.nativeBeginFrame(true, 0) == 0,
                            "DLSS 帧开始");
                    com.taolesi.dlssmc.nativebridge.DLSSFGNative.nativeRenderStart();
                    check(com.taolesi.dlssmc.nativebridge.DLSSFGNative.nativeLock() == 0, "DLSS 锁");
                    clearAll(textures);
                    check(com.taolesi.dlssmc.nativebridge.DLSSFGNative.nativeUnlock() == 0, "DLSS 解锁");
                    check(com.taolesi.dlssmc.nativebridge.DLSSFGNative.nativeProbe() == 1,
                            "第 " + round + " 轮 DLSS 预检：画面没传到 Vulkan");
                    check(com.taolesi.dlssmc.nativebridge.DLSSFGNative.nativePresent(
                            matrices, params, false, 0) == 0, "DLSS present");
                    GLFW.glfwPollEvents();
                }
                check(com.taolesi.dlssmc.nativebridge.DLSSFGNative.nativeUnbindTextures() == 0, "解绑输入");
                GL45C.glDeleteTextures(textures);
                java.util.Arrays.fill(textures, 0);

                check(DLSSDXNative.nativeInit(vendor.toString(), logs.toString()) == 0, "DX 初始化");
                check(DLSSDXNative.nativeCreateDevice(hwnd, width, height,
                        DLSSDXNative.BACKEND_XESS) == 0, "切换后的 XeSS 设备");
                bind(textures);
                for (int i = 0; i < 30; ++i) {
                    check(DLSSDXNative.nativeLock(0, srOn() ? 5 : 4) == 0, "DX 锁");
                    clearAll(textures);
                    check(DLSSDXNative.nativeDeliver() == 0, "DX 交付");
                    check(DLSSDXNative.nativePresent(matrices, params, false) == 0, "XeSS present");
                    GLFW.glfwPollEvents();
                }
                DLSSDXNative.nativeShutdown();
                GL45C.glDeleteTextures(textures);
                java.util.Arrays.fill(textures, 0);
            }
            System.out.println("PASS: DLSS↔XeSS 来回切两轮，全程只 slInit 一次，解绑后重绑仍出帧");
            check(GL45C.glGetError() == GL45C.GL_NO_ERROR, "OpenGL error");
        } finally {
            DLSSDXNative.nativeShutdown();
            if (dlssLoaded) com.taolesi.dlssmc.nativebridge.DLSSFGNative.nativeShutdown();
            GL45C.glDeleteTextures(textures);
            GL45C.glDeleteTextures(worldColor);
            GL45C.glDeleteTextures(mainColor);
            GL45C.glDeleteFramebuffers(copyFbos);
            GLFW.glfwDestroyWindow(window);
            GLFW.glfwTerminate();
        }
    }
}
