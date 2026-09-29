import com.taolesi.dlssmc.nativebridge.DLSSFGNative;
import org.lwjgl.glfw.GLFW;
import org.lwjgl.glfw.GLFWNativeWin32;
import org.lwjgl.opengl.GL;
import org.lwjgl.opengl.GL45C;

import java.nio.file.Path;

/**
 * 无头探测：RTX 40 上 NGX 是否接受强制多帧（3x/4x）。
 * 不开游戏：GLFW 隐藏窗口 + GL 4.5 + 直载新编的 dlssmc_fg.dll，
 * 按 FGRuntime 的锁纪律喂帧、present、数呈现计数。
 */
public class MfgProbeCheck {
    static int width = 1280, height = 720;
    static int tFinal, tHudless, tDepth, tMotion;

    static void check(boolean ok, String what) {
        System.out.println((ok ? "[OK] " : "[FAIL] ") + what);
        if (!ok) System.exit(1);
    }

    static int tex(int internalFormat) {
        // 必须与生产路径一致：glGenTextures + glTexImage2D。
        // glCreateTextures/glTextureStorage2D 建出的不可变纹理 wglDXRegisterObjectNV 会拒绝。
        int t = GL45C.glGenTextures();
        GL45C.glBindTexture(GL45C.GL_TEXTURE_2D, t);
        int components = internalFormat == GL45C.GL_R32F ? GL45C.GL_RED
                : internalFormat == GL45C.GL_RG16F ? GL45C.GL_RG : GL45C.GL_RGBA;
        GL45C.glTexImage2D(GL45C.GL_TEXTURE_2D, 0, internalFormat, width, height, 0,
                components, GL45C.GL_FLOAT, 0L);
        GL45C.glTexParameteri(GL45C.GL_TEXTURE_2D, GL45C.GL_TEXTURE_MIN_FILTER, GL45C.GL_NEAREST);
        GL45C.glTexParameteri(GL45C.GL_TEXTURE_2D, GL45C.GL_TEXTURE_MAG_FILTER, GL45C.GL_NEAREST);
        GL45C.glBindTexture(GL45C.GL_TEXTURE_2D, 0);
        return t;
    }

    static void feedFrame() {
        // 喂非黑画面：黑画面会触发 FG 的全黑熔断
        GL45C.glClearTexImage(tFinal, 0, GL45C.GL_RGBA, GL45C.GL_FLOAT, new float[]{0.35f, 0.5f, 0.78f, 1f});
        GL45C.glClearTexImage(tHudless, 0, GL45C.GL_RGBA, GL45C.GL_FLOAT, new float[]{0.35f, 0.5f, 0.78f, 1f});
        GL45C.glClearTexImage(tDepth, 0, GL45C.GL_RED, GL45C.GL_FLOAT, new float[]{0.3f});
        GL45C.glClearTexImage(tMotion, 0, GL45C.GL_RG, GL45C.GL_FLOAT, new float[]{0f, 0f});
        org.lwjgl.opengl.GL11C.glFinish();
    }

    public static void main(String[] args) throws Exception {
        Path dll = Path.of(args.length > 0 ? args[0] : "native/build/dlssmc_fg.dll").toAbsolutePath();
        String plugins = args.length > 1 ? args[1]
                : "D:\\Backup\\Downloads\\streamline-sdk-v2.14.1\\bin\\x64";
        System.out.println("插件目录: " + plugins);
        System.load(Path.of(plugins, "sl.interposer.dll").toString());
        System.load(dll.toString());

        check(GLFW.glfwInit(), "GLFW init");
        // 必须可见：隐藏窗口上 Vulkan 的 present 不进合成器，DLSS-G 会把帧当成没上屏，
        // numFramesActuallyPresented 永远读回 1，看不出插帧是否真的发生。
        GLFW.glfwWindowHint(GLFW.GLFW_VISIBLE, GLFW.GLFW_TRUE);
        GLFW.glfwWindowHint(GLFW.GLFW_CONTEXT_VERSION_MAJOR, 4);
        GLFW.glfwWindowHint(GLFW.GLFW_CONTEXT_VERSION_MINOR, 5);
        long window = GLFW.glfwCreateWindow(width, height, "mfg probe", 0, 0);
        check(window != 0, "GL 窗口");
        // 光 VISIBLE 不够：窗口在后台时合成器不认，DLSS-G 会当成没上屏，
        // numFramesActuallyPresented 恒为 1，会读成「多帧被拒」这种假结论。
        GLFW.glfwShowWindow(window);
        GLFW.glfwFocusWindow(window);
        GLFW.glfwMakeContextCurrent(window);
        GL.createCapabilities();
        GLFW.glfwSwapInterval(0);
        for (int i = 0; i < 20; i++) {
            GLFW.glfwPollEvents();
            Thread.sleep(16);
        }
        System.out.println("GPU: " + GL45C.glGetString(GL45C.GL_RENDERER));

        long hwnd = GLFWNativeWin32.glfwGetWin32Window(window);

        boolean mfgUnlock = args.length < 3 || Boolean.parseBoolean(args[2]);
        check(DLSSFGNative.nativeInit(plugins, dll.getParent().toString(), mfgUnlock) == 0, "slInit");
        System.out.println("MFG 解锁: " + DLSSFGNative.nativeGetMfgUnlockReport());
        check(DLSSFGNative.nativeCreateDevice(hwnd, width, height, false, 0) == 0, "设备/代理呈现创建");
        System.out.println("DLSS-G 支持: " + DLSSFGNative.nativeIsSupported());

        tFinal = tex(GL45C.GL_RGBA8);
        tHudless = tex(GL45C.GL_RGBA8);
        tDepth = tex(GL45C.GL_R32F);
        tMotion = tex(GL45C.GL_RG16F);
        int bind = DLSSFGNative.nativeBindTextures(tFinal, tHudless, tDepth, tMotion, 0, 0, width, height);
        check(bind == 0, "输入纹理绑定（返回码 " + bind + "）");
        check(DLSSFGNative.nativeSetEnabled(1) == 0, "启用 FG");

        // 1) 基线 2x
        int base = DLSSFGNative.nativeSetMode(1, false);
        System.out.println("基线 2x 生效张数: " + base);
        presentLoop(window, "2x 基线", 40);

        // 2) 生产路径：forceMultiFrame=false，靠 SDK 自己报的 sdkMax 放行。
        //    解锁生效时 sdkMax 应为 5，逐档都不会被夹；未解锁则 3x 起被夹回 2x。
        int ceiling = 0;
        for (int frames = 2; frames <= 5; frames++) {
            int eff = DLSSFGNative.nativeSetMode(frames, false);
            int got = presentLoop(window, (frames + 1) + "x 生产路径", 40);
            System.out.println((frames + 1) + "x 生产路径：生效张数=" + eff + " 实测呈现=" + got);
            if (got == frames + 1 && eff == frames) ceiling = got;
            else break;
        }
        System.out.println("生产路径上限 CEILING " + ceiling + "x");
        // 一帧都没插出来时别说成「多帧被拒」：先分清楚是被拒还是窗口没被合成器认。
        // 窗口不在前台时 numFramesActuallyPresented 恒为 1，读数会误导成上限很低。
        if (ceiling == 0) {
            System.out.println("  [!!] 全程没观察到多帧。若窗口不在前台，读数恒为 1 并不代表上限低，"
                    + "请把探针窗口点到前台后重跑再判断。");
        }

        DLSSFGNative.nativeSetEnabled(0);
        System.out.println("PROBE DONE");
    }

    static int presentLoop(long window, String label, int frames) throws InterruptedException {
        float[] m = new float[64];
        java.util.Arrays.fill(m, 0f);
        for (int i = 0; i < 4; i++) m[i * 5] = 1f;   // 单位阵占位
        float[] p = {0.05f, 1000f, 1.0f, (float) width / height, 0f, 0f, 0.5f, -0.5f};
        int maxCount = 0;
        for (int i = 0; i < frames; i++) {
            GLFW.glfwPollEvents();
            DLSSFGNative.nativeBeginFrame(false, 0);
            if (DLSSFGNative.nativeLock() == 0) {
                try {
                    feedFrame();
                } finally {
                    DLSSFGNative.nativeUnlock();
                }
            }
            // statusLogInterval=1：每帧都让原生层记一行 DLSSGState，能直接看到实际呈现张数
            int r = DLSSFGNative.nativePresent(m, p, i == 0, 1);
            int count = DLSSFGNative.nativeGetPresentedCount();
            maxCount = Math.max(maxCount, count);
            if (i % 10 == 0 || r != 0)
                System.out.println("  " + label + " #" + i + " present=" + r + " 呈现计数=" + count);
            if (r != 0 && r != 1 && r != 2) {
                System.out.println("  " + label + " 呈现失败码 " + r + "，停止该档");
                return maxCount;
            }
            Thread.sleep(16);
        }
        System.out.println("== " + label + "：最大呈现计数 " + maxCount
                + (maxCount >= 3 ? " —— NGX 接受了多帧！" : " —— 维持在 " + maxCount + "x"));
        return maxCount;
    }
}
