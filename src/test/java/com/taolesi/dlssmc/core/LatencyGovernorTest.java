package com.taolesi.dlssmc.core;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * 延迟预算的算术：输入到光子 ≈ (N+1)/刷新率，预算放不下就压倍数。
 * 判据是"(N+1)/刷新率 ≤ 预算"，边界和"至少留 2x"都要钉住。
 */
class LatencyGovernorTest {

    @Test
    void noBudgetMeansNoConstraint() {
        LatencyGovernor g = new LatencyGovernor();
        g.observePresentationFps(100.0);
        assertEquals(4, g.apply(4, 0.0));
        assertNull(g.lastReason());
    }

    @Test
    void noRefreshEstimateMeansNoConstraint() {
        LatencyGovernor g = new LatencyGovernor();
        assertFalse(g.hasRefreshEstimate());
        assertEquals(3, g.apply(3, 16.0));
        assertNull(g.lastReason());
        assertEquals(-1.0, g.estimatedLatencyMs(3));
    }

    @Test
    void budgetExactlyFitsKeepsRequestedTier() {
        LatencyGovernor g = new LatencyGovernor();
        g.observePresentationFps(100.0);
        // 100Hz 下 3x 要 (2+1)/100 = 30ms，预算 30ms 正好放得下
        assertEquals(2, g.apply(2, 30.0));
        assertNull(g.lastReason());
    }

    @Test
    void budgetOverrunStepsTierDown() {
        LatencyGovernor g = new LatencyGovernor();
        g.observePresentationFps(100.0);
        // 4x 要 50ms > 40ms 预算 -> 40ms*100Hz=4 档 -> N=3，即 4x 放得下？
        // floor(40*100/1000)-1 = 3，请求 3（4x）时 allowed=3 刚好，所以降一档要看 5x。
        assertEquals(3, g.apply(4, 40.0));
        assertEquals(3, g.lastApplied());
        assertNotNull(g.lastReason());
        // 5x 要 60ms，预算 40ms -> N<=3，降到 4x
        LatencyGovernor g2 = new LatencyGovernor();
        g2.observePresentationFps(100.0);
        assertEquals(3, g2.apply(4, 40.0));
    }

    @Test
    void sixtyHzBudgetFortyMsAllowsAtMostThreeX() {
        LatencyGovernor g = new LatencyGovernor();
        g.observePresentationFps(60.0);
        // floor(40*60/1000)-1 = 1 -> 只放得下 2x
        assertEquals(1, g.apply(3, 40.0));
        assertEquals(1, g.lastApplied());
    }

    @Test
    void alwaysKeepsAtLeastTwoX() {
        LatencyGovernor g = new LatencyGovernor();
        g.observePresentationFps(60.0);
        // 预算 5ms 连 2x（33ms）都放不下，但仍给 2x：用户要的是"少插"，不是"关掉"
        assertEquals(1, g.apply(4, 5.0));
        assertEquals(1, g.lastApplied());
    }

    @Test
    void disabledTierStaysDisabled() {
        LatencyGovernor g = new LatencyGovernor();
        g.observePresentationFps(100.0);
        assertEquals(0, g.apply(0, 40.0));
    }

    @Test
    void refreshEstimateOnlyRises() {
        LatencyGovernor g = new LatencyGovernor();
        g.observePresentationFps(100.0);
        g.observePresentationFps(60.0);
        assertEquals(100.0, g.refreshEstimate());
        // 离谱的采样不采信
        g.observePresentationFps(5.0);
        g.observePresentationFps(5000.0);
        assertEquals(100.0, g.refreshEstimate());
    }

    @Test
    void estimatedLatencyFollowsTier() {
        LatencyGovernor g = new LatencyGovernor();
        g.observePresentationFps(100.0);
        assertEquals(20.0, g.estimatedLatencyMs(1), 0.001);
        assertEquals(50.0, g.estimatedLatencyMs(4), 0.001);
    }

    @Test
    void resetClearsEstimateAndReason() {
        LatencyGovernor g = new LatencyGovernor();
        g.observePresentationFps(60.0);
        g.apply(3, 20.0);
        g.reset();
        assertFalse(g.hasRefreshEstimate());
        assertEquals(-1, g.lastApplied());
        assertTrue(g.apply(3, 20.0) == 3);
    }
}
