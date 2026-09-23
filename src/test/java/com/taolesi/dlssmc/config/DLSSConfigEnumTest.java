package com.taolesi.dlssmc.config;

import org.junit.jupiter.api.Test;

import java.util.HashSet;
import java.util.Set;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

/**
 * 配置枚举与原生层约定的映射。这些值直接传给 JNI（sl::DLSSMode、
 * xess_quality_settings_t、FfxApiUpscaleQualityMode），错一个就是档位错乱。
 */
class DLSSConfigEnumTest {

    @Test
    void qualityFromSlRoundTripsEveryValue() {
        for (DLSSConfig.Quality q : DLSSConfig.Quality.values()) {
            assertSame(q, DLSSConfig.Quality.fromSl(q.slValue), "slValue " + q.slValue);
        }
    }

    @Test
    void qualityFromSlUnknownFallsBackToOff() {
        assertSame(DLSSConfig.Quality.OFF, DLSSConfig.Quality.fromSl(99));
        assertSame(DLSSConfig.Quality.OFF, DLSSConfig.Quality.fromSl(-1));
    }

    @Test
    void qualitySlValuesMatchStreamlineDlssMode() {
        // sl::DLSSMode：1=MaxPerformance 2=Balanced 3=MaxQuality 4=UltraPerformance 5=UltraQuality 6=DLAA
        assertEquals(0, DLSSConfig.Quality.OFF.slValue);
        assertEquals(1, DLSSConfig.Quality.PERFORMANCE.slValue);
        assertEquals(2, DLSSConfig.Quality.BALANCED.slValue);
        assertEquals(3, DLSSConfig.Quality.QUALITY.slValue);
        assertEquals(4, DLSSConfig.Quality.ULTRA_PERFORMANCE.slValue);
        assertEquals(5, DLSSConfig.Quality.ULTRA_QUALITY.slValue);
        assertEquals(6, DLSSConfig.Quality.DLAA.slValue);
    }

    @Test
    void frameGenCountsAreInterpolatedFramesPerRealFrame() {
        assertEquals(0, DLSSConfig.FrameGen.OFF.framesToGenerate);
        assertEquals(1, DLSSConfig.FrameGen.X2.framesToGenerate);
        assertEquals(2, DLSSConfig.FrameGen.X3.framesToGenerate);
        assertEquals(3, DLSSConfig.FrameGen.X4.framesToGenerate);
        assertEquals(4, DLSSConfig.FrameGen.X5.framesToGenerate);
        assertEquals(5, DLSSConfig.FrameGen.X6.framesToGenerate);
    }

    @Test
    void frameGenLabelsMatchMultiplierAndStayWithinNgxCeiling() {
        // 标签就是「1 + 生成张数」倍。原生层把 numFramesToGenerate 原样交给
        // slDLSSGSetOptions，而解锁后的 NGX 上限报 5（即 6x），档位不能超过它。
        for (DLSSConfig.FrameGen fg : DLSSConfig.FrameGen.values()) {
            if (fg == DLSSConfig.FrameGen.OFF) continue;
            assertEquals((fg.framesToGenerate + 1) + "x", fg.label, fg.name());
            assertTrue(fg.framesToGenerate <= 5, fg.name() + " 超过 NGX 的 5 张上限");
        }
    }

    @Test
    void srPresetValuesMatchStreamlinePresetIds() {
        assertEquals(0, DLSSConfig.SrPreset.AUTO.slValue);
        assertEquals(10, DLSSConfig.SrPreset.J.slValue);
        assertEquals(11, DLSSConfig.SrPreset.K.slValue);
        assertEquals(12, DLSSConfig.SrPreset.L.slValue);
        assertEquals(13, DLSSConfig.SrPreset.M.slValue);
    }

    @Test
    void xessSrNativeValuesAreDistinctAndOffIsNegativeOne() {
        Set<Integer> seen = new HashSet<>();
        for (DLSSConfig.XessSr q : DLSSConfig.XessSr.values()) {
            assertTrue(seen.add(q.nativeValue), "重复的 XeSS 档位值 " + q.nativeValue);
        }
        assertEquals(-1, DLSSConfig.XessSr.OFF.nativeValue);
        assertEquals(100, DLSSConfig.XessSr.ULTRA_PERFORMANCE.nativeValue);
        assertEquals(106, DLSSConfig.XessSr.AA.nativeValue);
    }

    @Test
    void fsrSrNativeValuesMatchFfxApiUpscaleQualityMode() {
        assertEquals(-1, DLSSConfig.FsrSr.OFF.nativeValue);
        assertEquals(0, DLSSConfig.FsrSr.NATIVE_AA.nativeValue);
        assertEquals(1, DLSSConfig.FsrSr.QUALITY.nativeValue);
        assertEquals(2, DLSSConfig.FsrSr.BALANCED.nativeValue);
        assertEquals(3, DLSSConfig.FsrSr.PERFORMANCE.nativeValue);
        assertEquals(4, DLSSConfig.FsrSr.ULTRA_PERFORMANCE.nativeValue);
    }

    @Test
    void backendLabelsAreStableForOverlayText() {
        assertEquals("DLSS", DLSSConfig.Backend.DLSS.label);
        assertEquals("FSR", DLSSConfig.Backend.FSR.label);
        assertEquals("XeSS", DLSSConfig.Backend.XESS.label);
    }
}
