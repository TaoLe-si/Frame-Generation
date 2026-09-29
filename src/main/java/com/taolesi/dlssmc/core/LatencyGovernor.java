package com.taolesi.dlssmc.core;

/**
 * 输入延迟预算：把「倍数为 N 的插帧要付多少输入延迟」算清楚，并据此压档位。
 *
 * <p>为什么需要它：插帧本身不产生延迟，但会**摊薄真实帧率**。
 * 一个真实帧上屏一次要占掉 N+1 个显示器刷新周期，所以
 * <pre>   输入到光子 ≈ (N+1) / 刷新率 + 呈现延迟</pre>
 * 倍数越高画面越顺，输入延迟也线性变高。4x 在 100Hz 屏上就是 50 ms，这是插帧的固有代价，
 * 不是实现缺陷 —— 所以在代码里"优化"掉它是不可能的，能做的是把它变成一个可约束的量。
 *
 * <p>这个类只管算术：给定请求的倍数、刷新率估计、延迟预算，给出实际该用的倍数。
 * 无 GL / 无原生依赖，可独立单测。
 */
final class LatencyGovernor {

    /** 刷新率估计的合理范围；超出这个范围的采样一律不采信。 */
    private static final double MIN_REFRESH = 30.0;
    private static final double MAX_REFRESH = 500.0;

    private double refreshEstimate;
    private int lastApplied = -1;
    private String lastReason;

    /**
     * 用呈现帧率学习刷新率。插帧在跑时呈现帧率会被顶到刷新率上限，取历史最大值即可逼近。
     * 只在明确高于已有估计时才上调，避免瞬时抖动把估计带偏。
     */
    void observePresentationFps(double presentedFps) {
        if (!(presentedFps > MIN_REFRESH) || presentedFps > MAX_REFRESH) return;
        if (presentedFps > refreshEstimate) refreshEstimate = presentedFps;
    }

    void reset() {
        refreshEstimate = 0.0;
        lastApplied = -1;
        lastReason = null;
    }

    boolean hasRefreshEstimate() {
        return refreshEstimate > 0.0;
    }

    /**
     * @param requestedFrames 用户/配置请求的生成张数（N，2x = 1）
     * @param budgetMs        允许的输入延迟预算，毫秒；&lt;= 0 表示不约束
     * @return 实际应下发的生成张数；没有刷新率估计时原样返回 requestedFrames
     */
    int apply(int requestedFrames, double budgetMs) {
        lastReason = null;
        if (requestedFrames <= 0) {
            lastApplied = 0;
            return 0;
        }
        if (!(budgetMs > 0.0) || refreshEstimate <= 0.0) {
            lastApplied = requestedFrames;
            return requestedFrames;
        }
        // (N+1) / refresh <= budget  ->  N <= budget * refresh - 1
        final double slots = budgetMs * refreshEstimate / 1000.0;
        final int allowed = (int) Math.floor(slots) - 1;
        if (allowed >= requestedFrames) {
            lastApplied = requestedFrames;
            return requestedFrames;
        }
        // 至少留 1（2x）。预算压到连 2x 都放不下时也仍然给 2x：
        // 用户要的是"少插"，不是"关掉"；真要关他去把档位改成关闭。
        final int effective = Math.max(1, allowed);
        if (effective < requestedFrames) {
            lastReason = String.format(
                    "延迟预算 %.0f ms：%.0fHz 下 %dx 要付 %.0f ms，已降到 %dx",
                    budgetMs, refreshEstimate, requestedFrames + 1,
                    (requestedFrames + 1) * 1000.0 / refreshEstimate, effective + 1);
        }
        lastApplied = effective;
        return effective;
    }

    /** 上一次 apply 后的生效倍数对应的生成张数；-1 表示还没算过。 */
    int lastApplied() {
        return lastApplied;
    }

    /** 上一次 apply 的降档原因；没降档时为 null。 */
    String lastReason() {
        return lastReason;
    }

    double refreshEstimate() {
        return refreshEstimate;
    }

    /** 估计的输入到光子延迟（毫秒）；参数是生效的生成张数。没有刷新率估计时返回 -1。 */
    double estimatedLatencyMs(int frames) {
        if (refreshEstimate <= 0.0 || frames < 0) return -1.0;
        return (frames + 1) * 1000.0 / refreshEstimate;
    }
}
