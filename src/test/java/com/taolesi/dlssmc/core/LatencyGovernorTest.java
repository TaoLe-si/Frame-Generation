package com.taolesi.dlssmc.core;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * 延迟预算的算术：输入到光子 ≈ 工作耗时 + (N+1)/刷新率。
 * 三项输入（系统刷新率 / 估计刷新率 / 实测工作耗时）各自缺失时的退化行为、
 * 边界值、以及「至少留 2x」都要钉住。
 */
class LatencyGovernorTest {

    /** 造一个「系统给了刷新率、有实测工作耗时」的常规状态。 */
    private static LatencyGovernor withRefresh(int hz) {
        LatencyGovernor g = new LatencyGovernor();
        g.setReferenceRefresh(hz);
        return g;
    }

    // ------------------------------------------------------------ 输入缺失

    @Test
    void noBudgetMeansNoConstraint() {
        LatencyGovernor g = withRefresh(100);
        assertEquals(4, g.apply(4, 0.0));
        assertNull(g.lastReason());
    }

    @Test
    void noRefreshAtAllMeansNoConstraint() {
        LatencyGovernor g = new LatencyGovernor();
        assertFalse(g.refreshIsMeasured());
        assertEquals(0.0, g.refresh());
        assertEquals(3, g.apply(3, 16.0));
        assertNull(g.lastReason());
        assertEquals(-1.0, g.estimatedLatencyMs(3));
    }

    @Test
    void estimatedRefreshUsedWhenSystemValueMissing() {
        LatencyGovernor g = new LatencyGovernor();
        g.observePresentationFps(100.0);
        assertFalse(g.refreshIsMeasured());
        assertEquals(100.0, g.refresh());
        // 没有实测工作耗时时按一个刷新周期估，模型退化成 (N+2)/R：
        // N=4（5x）要 10 + 5/100*1000 = 60ms，预算 60ms 正好放得下
        assertEquals(4, g.apply(4, 60.0));
        // 收紧到 59ms 就只放得下 N=3（4x，要 50ms）
        assertEquals(3, g.apply(4, 59.0));
    }

    @Test
    void systemRefreshWinsOverEstimate() {
        LatencyGovernor g = new LatencyGovernor();
        g.observePresentationFps(100.0);
        g.setReferenceRefresh(60);
        assertTrue(g.refreshIsMeasured());
        assertEquals(60.0, g.refresh());
    }

    @Test
    void implausibleSystemRefreshIgnored() {
        LatencyGovernor g = new LatencyGovernor();
        g.setReferenceRefresh(0);      // 驱动报 0 很常见
        g.setReferenceRefresh(10000);
        assertFalse(g.refreshIsMeasured());
        g.observePresentationFps(120.0);
        assertEquals(120.0, g.refresh());
    }

    @Test
    void refreshEstimateOnlyRises() {
        LatencyGovernor g = new LatencyGovernor();
        g.observePresentationFps(100.0);
        g.observePresentationFps(60.0);
        assertEquals(100.0, g.refresh());
        g.observePresentationFps(5.0);
        g.observePresentationFps(5000.0);
        assertEquals(100.0, g.refresh());
    }

    // ------------------------------------------------------------ 工作耗时

    @Test
    void pipelineTimeShrinksTheAllowedTier() {
        LatencyGovernor g = withRefresh(100);
        // 无实测：工作按 1 个周期算，60ms 预算 -> (预算-10)*100/1000-1 = 4
        assertEquals(4, g.apply(5, 60.0));
        // 实测工作 25ms：allowed = (60-25)*100/1000-1 = 2（3x）
        g.observePipelineMs(25.0);
        assertEquals(2, g.apply(5, 60.0));
        assertEquals(2, g.lastApplied());
        assertNotNull(g.lastReason());
    }

    /**
     * 工作耗时按「取最坏、但向下衰减 15%」平滑：预算要防的是最坏那一帧，
     * 但也不能让一次尖峰永久毒化预算 —— 机器真的变快了要能降下来。
     */
    @Test
    void pipelineTimeIsSmoothedTowardTheWorstCase() {
        LatencyGovernor g = withRefresh(100);
        g.observePipelineMs(30.0);
        assertEquals(30.0, g.pipelineMs(), 0.001);
        // 好帧不会立刻把估计拉低，而是按 15% 衰减
        g.observePipelineMs(10.0);
        assertEquals(25.5, g.pipelineMs(), 0.001);
        // 坏帧立刻抬上去
        g.observePipelineMs(40.0);
        assertEquals(40.0, g.pipelineMs(), 0.001);
        // 连续好帧能把估计收敛下去
        for (int i = 0; i < 40; i++) g.observePipelineMs(5.0);
        assertEquals(5.0, g.pipelineMs(), 0.001);
    }

    @Test
    void implausiblePipelineSamplesIgnored() {
        LatencyGovernor g = withRefresh(100);
        g.observePipelineMs(-1.0);
        g.observePipelineMs(0.0);
        g.observePipelineMs(5000.0);
        assertEquals(0.0, g.pipelineMs());
    }

    @Test
    void workTimeIsAddedToTheDisplayTerm() {
        LatencyGovernor g = withRefresh(100);
        g.observePipelineMs(8.0);
        // 2x（N=1）：8 + 2/100*1000 = 28ms
        assertEquals(28.0, g.estimatedLatencyMs(1), 0.001);
        // 5x（N=4）：8 + 5/100*1000 = 58ms
        assertEquals(58.0, g.estimatedLatencyMs(4), 0.001);
    }

    // ------------------------------------------------------------ 决策边界

    @Test
    void budgetExactlyFitsKeepsRequestedTier() {
        LatencyGovernor g = withRefresh(100);
        g.observePipelineMs(10.0);
        // 4x 要 10 + 4/100*1000 = 50ms，预算 50ms 正好放得下
        assertEquals(3, g.apply(3, 50.0));
        assertNull(g.lastReason());
    }

    @Test
    void budgetOverrunStepsTierDown() {
        LatencyGovernor g = withRefresh(100);
        g.observePipelineMs(10.0);
        // 5x 要 60ms > 40ms 预算 -> allowed = (40-10)*100/1000-1 = 2（3x）
        assertEquals(2, g.apply(4, 40.0));
        assertEquals(2, g.lastApplied());
        assertNotNull(g.lastReason());
    }

    @Test
    void sixtyHzBudgetFortyMsAllowsAtMostThreeX() {
        LatencyGovernor g = withRefresh(60);
        // 无实测工作耗时：allowed = (40-16.67)*60/1000-1 = 0 -> 夹到 1（2x）
        assertEquals(1, g.apply(3, 40.0));
        assertEquals(1, g.lastApplied());
    }

    @Test
    void alwaysKeepsAtLeastTwoX() {
        LatencyGovernor g = withRefresh(60);
        // 预算压到连 2x 都放不下（工作耗时就已经超预算）时仍给 2x
        g.observePipelineMs(50.0);
        assertEquals(1, g.apply(4, 5.0));
        assertEquals(1, g.lastApplied());
    }

    @Test
    void disabledTierStaysDisabled() {
        LatencyGovernor g = withRefresh(100);
        g.observePipelineMs(10.0);
        assertEquals(0, g.apply(0, 40.0));
    }

    @Test
    void resetClearsEverything() {
        LatencyGovernor g = new LatencyGovernor();
        g.setReferenceRefresh(60);
        g.observePresentationFps(60.0);
        g.observePipelineMs(20.0);
        g.apply(3, 20.0);
        g.reset();
        assertFalse(g.refreshIsMeasured());
        assertEquals(0.0, g.refresh());
        assertEquals(0.0, g.pipelineMs());
        assertEquals(-1, g.lastApplied());
        assertEquals(3, g.apply(3, 20.0));
    }
}
