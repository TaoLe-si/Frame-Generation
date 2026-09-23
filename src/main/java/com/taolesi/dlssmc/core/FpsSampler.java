package com.taolesi.dlssmc.core;

/**
 * 呈现 FPS 采样器：1 秒滚动窗口，真实帧数与呈现张数分开累计，
 * 接管状态翻转即作废重开。纯数值、无 GL 依赖，可独立单测。
 */
final class FpsSampler {

    private static final long WINDOW_NS = 1_000_000_000L;

    private boolean sampling;
    private boolean available;
    private boolean takeover;
    private long sampleStart;
    private int renderedFrames;
    private long presentedFrames;
    private double renderFps;
    private double presentationFps;

    void record(long now, int presented, boolean takeover) {
        if (!sampling || takeover != this.takeover) {
            sampling = true;
            available = false;
            this.takeover = takeover;
            sampleStart = now;
            renderedFrames = 0;
            presentedFrames = 0;
            renderFps = presentationFps = 0;
            return;
        }
        renderedFrames++;
        presentedFrames += presented;
        long elapsed = now - sampleStart;
        if (elapsed >= WINDOW_NS) {
            renderFps = renderedFrames * 1_000_000_000.0 / elapsed;
            presentationFps = presentedFrames * 1_000_000_000.0 / elapsed;
            available = true;
            sampleStart = now;
            renderedFrames = 0;
            presentedFrames = 0;
        }
    }

    void reset() {
        sampling = false;
        available = false;
        renderFps = presentationFps = 0;
    }

    boolean isAvailable() {
        return available;
    }

    boolean isTakeover() {
        return takeover;
    }

    double renderFps() {
        return renderFps;
    }

    double presentationFps() {
        return presentationFps;
    }
}
