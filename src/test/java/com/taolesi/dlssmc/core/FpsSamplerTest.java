package com.taolesi.dlssmc.core;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

/** 1 秒滚动窗口的采样数学：真实帧 / 呈现张数分开累计，接管翻转即重开窗口。 */
class FpsSamplerTest {

    private static final long MS = 1_000_000L; // 1ms in ns
    private static final long SECOND = 1_000_000_000L;

    @Test
    void firstCallOnlyInitializesSampling() {
        FpsSampler s = new FpsSampler();
        s.record(0L, 1, false);
        assertFalse(s.isAvailable());
        assertEquals(0.0, s.renderFps());
        assertEquals(0.0, s.presentationFps());
    }

    @Test
    void glFramesProduceFpsAfterOneSecondWindow() {
        FpsSampler s = new FpsSampler();
        s.record(0L, 1, false); // 初始化，不计入
        for (int i = 1; i <= 100; i++) {
            s.record(i * 10 * MS, 1, false); // 10ms 一帧，GL 上屏
        }
        assertTrue(s.isAvailable());
        assertEquals(100.0, s.renderFps(), 1e-6);
        assertEquals(100.0, s.presentationFps(), 1e-6);
    }

    @Test
    void windowShorterThanOneSecondKeepsSampling() {
        FpsSampler s = new FpsSampler();
        s.record(0L, 1, false);
        for (int i = 1; i <= 99; i++) { // 只到 990ms，不满窗口
            s.record(i * 10 * MS, 1, false);
        }
        assertFalse(s.isAvailable());
    }

    @Test
    void takeoverFramesAccumulatePresentedCounts() {
        FpsSampler s = new FpsSampler();
        s.record(0L, 3, true);
        for (int i = 1; i <= 100; i++) {
            s.record(i * 10 * MS, 3, true); // 每次真实帧 present 3 张
        }
        assertTrue(s.isAvailable());
        assertEquals(100.0, s.renderFps(), 1e-6);
        assertEquals(300.0, s.presentationFps(), 1e-6);
    }

    @Test
    void takeoverFlipRestartsSampling() {
        FpsSampler s = new FpsSampler();
        s.record(0L, 1, false);
        for (int i = 1; i <= 100; i++) {
            s.record(i * 10 * MS, 1, false);
        }
        assertTrue(s.isAvailable());

        // 翻转到接管：采样立刻作废
        s.record(2 * SECOND, 2, true);
        assertFalse(s.isAvailable());
        assertEquals(0.0, s.renderFps());

        // 接管下的新窗口：50 帧 × 每帧 2 张
        for (int i = 1; i <= 50; i++) {
            s.record(2 * SECOND + i * 20 * MS, 2, true);
        }
        assertTrue(s.isAvailable());
        assertEquals(50.0, s.renderFps(), 1e-6);
        assertEquals(100.0, s.presentationFps(), 1e-6);
    }

    @Test
    void unevenSpacingUsesRealElapsedTime() {
        FpsSampler s = new FpsSampler();
        s.record(0L, 1, false);
        // 2 帧：10ms 和 1990ms —— 窗口按真实 2 秒算，不是按帧数
        s.record(10 * MS, 1, false);
        s.record(2 * SECOND, 1, false);
        assertTrue(s.isAvailable());
        assertEquals(1.0, s.renderFps(), 1e-6);
    }

    @Test
    void resetReturnsToUnsampledState() {
        FpsSampler s = new FpsSampler();
        s.record(0L, 1, false);
        for (int i = 1; i <= 100; i++) {
            s.record(i * 10 * MS, 1, false);
        }
        assertTrue(s.isAvailable());
        s.reset();
        assertFalse(s.isAvailable());
        assertEquals(0.0, s.renderFps());
    }
}
