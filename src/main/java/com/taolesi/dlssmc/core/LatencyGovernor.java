package com.taolesi.dlssmc.core;

/**
 * 输入延迟预算：把「倍数为 N 的插帧要付多少输入延迟」算清楚，并据此压档位。
 *
 * <p>为什么需要它：插帧不产生延迟，但会**摊薄真实帧率**。一个真实帧上屏要占掉 N+1 个
 * 显示器刷新周期，所以倍数越高画面越顺、输入延迟也线性变高 —— 这是插帧的固有代价，
 * 不是实现缺陷。
 *
 * <p><b>延迟模型（两项都可测，不再靠估）：</b>
 * <pre>
 *     输入到光子 ≈ 工作耗时 + (N+1)/刷新率
 * </pre>
 * <ul>
 *   <li><b>工作耗时</b>：这段帧自己产出画面所必需的开销（Reflex 的
 *       <b>模拟 + 提交 + GPU</b> 三段之和）。输入是在 {@code slReflexSleep} 之后才采样的，
 *       所以这段直接加在输入到光子的关键路径上；机器越快它越短，这也是 Reflex
 *       「尽量晚采样输入」的意义。
 *       <b>有意不含</b> Reflex 的「OS 渲染队列」与「Present」两段：那是节奏等待发生的地方
 *       （DLSS-G 在 Present 里等显示槽位就落在其中），而显示等待已经由下面的
 *       {@code (N+1)/刷新率} 项覆盖 —— 两段都算就是双重计入，会让模型偏保守。</li>
 *   <li><b>(N+1)/刷新率</b>：显示侧。一次 Present 吐出 N+1 帧占满 N+1 个刷新周期，
 *       而真实帧是这一组里的**最后一帧**（前面先放 N 张插帧去补上一组到这一组之间的空隙），
 *       所以它比 Present 晚 N 个周期上屏；再加最多一个周期的对齐余量，即 (N+1)/刷新率。</li>
 * </ul>
 *
 * <p>刷新率优先取 {@code Window.getRefreshRate()}（系统给的，准），拿不到时才退回用
 * 呈现帧率的峰值去学（插帧跑满时呈现帧率会被顶到刷新率）。工作耗时没有实测值时按
 * 一个刷新周期估，即退化成 (N+2)/刷新率。
 *
 * <p>这个类只管算术：无 GL / 无原生依赖，可独立单测。
 */
final class LatencyGovernor {

    /** 刷新率估计的合理范围；超出这个范围的采样一律不采信。 */
    private static final double MIN_REFRESH = 30.0;
    private static final double MAX_REFRESH = 500.0;

    /** 系统给的刷新率，权威值；0 = 还没拿到。 */
    private double referenceRefresh;
    /** 从呈现帧率峰值学出来的刷新率，只作为 referenceRefresh 拿不到时的退路。 */
    private double estimatedRefresh;
    /** Reflex 实测的「模拟→Present」工作耗时（毫秒），平滑后的；0 = 还没有实测值。 */
    private double pipelineMs;

    private int lastApplied = -1;
    private String lastReason;

    // ---------------------------------------------------------------- 输入

    /** 系统给的刷新率（Window.getRefreshRate）。不合理时忽略，避免驱动报 0 把预算算飞。 */
    void setReferenceRefresh(int hz) {
        if (hz >= MIN_REFRESH && hz <= MAX_REFRESH) referenceRefresh = hz;
    }

    /**
     * 用呈现帧率学习刷新率（退路）。插帧在跑时呈现帧率会被顶到刷新率上限，取历史最大值逼近。
     * 只在明确高于已有估计时才上调，避免瞬时抖动把估计带偏。
     */
    void observePresentationFps(double presentedFps) {
        if (!(presentedFps > MIN_REFRESH) || presentedFps > MAX_REFRESH) return;
        if (presentedFps > estimatedRefresh) estimatedRefresh = presentedFps;
    }

    /**
     * 实测的「工作耗时」（模拟 + 提交 + GPU，不含队列与呈现段，理由见类注释）。
     * 按「取最坏、向下衰减 15%」平滑：预算要防的是最坏那一帧，但一次尖峰不该永久毒化预算。
     */
    void observePipelineMs(double ms) {
        if (!(ms > 0.0) || ms > 1000.0) return;
        pipelineMs = pipelineMs <= 0.0 ? ms : Math.max(ms, pipelineMs * 0.85);
    }

    void reset() {
        referenceRefresh = 0.0;
        estimatedRefresh = 0.0;
        pipelineMs = 0.0;
        lastApplied = -1;
        lastReason = null;
    }

    // ---------------------------------------------------------------- 查询

    /** 生效的刷新率：优先系统值，其次估计值；0 = 都还没有。 */
    double refresh() {
        return referenceRefresh > 0.0 ? referenceRefresh : estimatedRefresh;
    }

    /** 刷新率是否来自系统而不是估计。 */
    boolean refreshIsMeasured() {
        return referenceRefresh > 0.0;
    }

    double pipelineMs() {
        return pipelineMs;
    }

    /** 估计的输入到光子延迟（毫秒）；参数是生效的生成张数。没有刷新率时返回 -1。 */
    double estimatedLatencyMs(int frames) {
        final double r = refresh();
        if (r <= 0.0 || frames < 0) return -1.0;
        return workMs(r) + (frames + 1) * 1000.0 / r;
    }

    /** 没有实测工作耗时时按一个刷新周期估，模型退化成 (N+2)/刷新率。 */
    private double workMs(double r) {
        return pipelineMs > 0.0 ? pipelineMs : 1000.0 / r;
    }

    /** 上一次 apply 后的生效倍数对应的生成张数；-1 表示还没算过。 */
    int lastApplied() {
        return lastApplied;
    }

    /** 上一次 apply 的降档原因；没降档时为 null。 */
    String lastReason() {
        return lastReason;
    }

    // ------------------------------------------------- 从 Reflex 分段里挑工作耗时

    /**
     * 从 Reflex 分段数组里挑出「工作耗时」：模拟 + 提交 + GPU。
     *
     * <p>数组顺序与 {@code DLSSFGNative.nativeGetLatency()} 一致：
     * {@code [0]=模拟→Present 全长, [1]=模拟, [2]=提交, [3]=OS 渲染队列, [4]=GPU, [5]=Present, [6]=有效标志}。
     * 有意不含 [3] 与 [5]（节奏等待，已由显示项覆盖），理由见类注释。
     *
     * @return 毫秒；数组不合法或该帧无效时返回 0
     */
    static double workFromReflex(double[] reflex) {
        if (reflex == null || reflex.length < 7 || reflex[6] < 0.5) return 0.0;
        return reflex[1] + reflex[2] + reflex[4];
    }

    /** 同一份分段里没进模型的两段（队列 + 呈现），只用于在叠加层里让人看见。 */
    static double excludedFromReflex(double[] reflex) {
        if (reflex == null || reflex.length < 7 || reflex[6] < 0.5) return 0.0;
        return reflex[3] + reflex[5];
    }

    // ---------------------------------------------------------------- 决策

    /**
     * @param requestedFrames 用户/配置请求的生成张数（N，2x = 1）
     * @param budgetMs        允许的输入延迟预算，毫秒；&lt;= 0 表示不约束
     * @return 实际应下发的生成张数；没有刷新率时原样返回 requestedFrames
     */
    int apply(int requestedFrames, double budgetMs) {
        lastReason = null;
        if (requestedFrames <= 0) {
            lastApplied = 0;
            return 0;
        }
        final double r = refresh();
        if (!(budgetMs > 0.0) || r <= 0.0) {
            lastApplied = requestedFrames;
            return requestedFrames;
        }
        // 工作耗时 + (N+1)/R <= 预算  ->  N <= (预算 - 工作耗时)*R/1000 - 1
        final double work = workMs(r);
        final int allowed = (int) Math.floor((budgetMs - work) * r / 1000.0) - 1;
        if (allowed >= requestedFrames) {
            lastApplied = requestedFrames;
            return requestedFrames;
        }
        // 至少留 1（2x）：预算压到连 2x 都放不下时也仍然给 2x ——
        // 用户要的是"少插"，不是"关掉"；真要关他去把档位改成关闭。
        final int effective = Math.max(1, allowed);
        lastReason = String.format(
                "延迟预算 %.0f ms：%.0fHz%s 下 %dx 要付 %.0f ms（工作 %.1f + 显示 %.1f），已降到 %dx",
                budgetMs, r, refreshIsMeasured() ? "" : "（估）", requestedFrames + 1,
                work + (requestedFrames + 1) * 1000.0 / r, work,
                (requestedFrames + 1) * 1000.0 / r, effective + 1);
        lastApplied = effective;
        return effective;
    }
}
