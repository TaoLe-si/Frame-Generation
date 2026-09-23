import com.taolesi.dlssmc.config.DLSSConfig;
import com.taolesi.dlssmc.core.FGRuntime;

public class FrameRateCheck {
    private static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    private static void near(double actual, double expected) {
        check(Math.abs(actual - expected) < 0.0001, actual + " != " + expected);
    }

    public static void main(String[] args) throws Exception {
        check(DLSSConfig.REFLEX_BOOST.getDefault(), "Boost default");
        check(DLSSConfig.DEBUG_OVERLAY.getDefault(), "overlay default");
        check(DLSSConfig.REFLEX_FPS_LIMIT.getDefault() == 0, "FPS cap default");
        check(!DLSSConfig.ZERO_MOTION_VECTORS.getDefault(), "zero motion default");
        check(!DLSSConfig.RESET_HISTORY_EVERY_FRAME.getDefault(), "history reset default");
        check(DLSSConfig.STATUS_LOG_INTERVAL.getDefault() == 120, "status log default");

        FGRuntime fg = FGRuntime.get();
        fg.recordPresentation(0, 1, false);
        check(!fg.hasFpsSample(), "startup must sample first");
        for (int i = 1; i <= 60; i++) fg.recordPresentation(i * 1_000_000_000L / 60, 1, false);
        check(fg.hasFpsSample(), "one-second sample");
        near(fg.getRenderFps(), 60);
        near(fg.getPresentationFps(), 60);

        long start = 2_000_000_000L;
        fg.recordPresentation(start, 2, true);
        check(!fg.hasFpsSample(), "GL/FG switch must not mix windows");
        near(fg.getRenderFps(), 0);
        for (int i = 1; i <= 60; i++) fg.recordPresentation(start + i * 1_000_000_000L / 60, 2, true);
        near(fg.getRenderFps(), 60);
        near(fg.getPresentationFps(), 120);
        var backend = FGRuntime.class.getDeclaredField("backend");
        backend.setAccessible(true);
        for (var value : DLSSConfig.Backend.values()) {
            backend.set(fg, value);
            String text = fg.getFpsText();
            check(!text.contains("SL计数"), "DX counters must not be labelled Streamline");
            if (value == DLSSConfig.Backend.FSR)
                check(text.contains("FSR 提交估算 120") && text.contains("非实测呈现"),
                        "FSR callbacks are not display telemetry");
        }
        backend.set(fg, null);

        start += 1_000_000_000L;
        for (int i = 1; i <= 60; i++) fg.recordPresentation(start + i * 1_000_000_000L / 60, i % 2 == 0 ? 2 : 1, true);
        near(fg.getRenderFps(), 60);
        near(fg.getPresentationFps(), 90);

        start += 1_000_000_000L;
        for (int i = 1; i <= 60; i++) fg.recordPresentation(start + i * 1_000_000_000L / 60, 0, true);
        near(fg.getRenderFps(), 60);
        near(fg.getPresentationFps(), 0);

        start += 1_000_000_000L;
        fg.recordPresentation(start + 2_000_000_000L, 2, true);
        near(fg.getRenderFps(), 0.5);
        near(fg.getPresentationFps(), 1);

        start += 3_000_000_000L;
        fg.recordPresentation(start, 1, false);
        check(!fg.hasFpsSample(), "FG/GL switch must clear old sample");
        for (int i = 1; i <= 30; i++) fg.recordPresentation(start + i * 1_500_000_000L / 30, 1, false);
        near(fg.getRenderFps(), 20);
        near(fg.getPresentationFps(), 20);
        fg.shutdown();
        check(!fg.hasFpsSample(), "shutdown clears sample");
        near(fg.getRenderFps(), 0);
        System.out.println("PASS: debug defaults, real/present FPS, variable generated counts, zero output, stalls, mode changes, elapsed-time normalization, shutdown");
    }
}
