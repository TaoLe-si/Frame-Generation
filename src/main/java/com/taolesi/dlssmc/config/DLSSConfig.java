package com.taolesi.dlssmc.config;

import com.taolesi.dlssmc.DLSSMC;
import net.minecraftforge.common.ForgeConfigSpec;

/**
 * 档位与运行时开关。
 * mode 的取值直接对应 sl::DLSSMode：
 *   0=eOff 1=eMaxPerformance 2=eBalanced 3=eMaxQuality 4=eUltraPerformance 5=eUltraQuality 6=eDLAA
 */
public final class DLSSConfig {

    public enum Quality {
        OFF(0, "关闭", "不使用 DLSS"),
        ULTRA_QUALITY(5, "极致质量", "约 1.3x 放大"),
        QUALITY(3, "质量", "约 1.5x 放大"),
        BALANCED(2, "平衡", "约 1.7x 放大"),
        PERFORMANCE(1, "性能", "约 2x 放大"),
        ULTRA_PERFORMANCE(4, "超级性能", "约 3x 放大"),
        DLAA(6, "DLAA", "原生分辨率 + AI 抗锯齿，不降分辨率");

        public final int slValue;
        public final String label;
        public final String desc;

        Quality(int slValue, String label, String desc) {
            this.slValue = slValue;
            this.label = label;
            this.desc = desc;
        }

        public static Quality fromSl(int v) {
            for (Quality q : values()) {
                if (q.slValue == v) return q;
            }
            return OFF;
        }
    }

    /** 帧生成档位。1 = 2x，2 = 3x，3 = 4x，4 = 5x，5 = 6x */
    public enum FrameGen {
        OFF(0, "关闭", "不做帧生成"),
        X2(1, "2x", "每个真实帧生成 1 帧"),
        X3(2, "3x", "每个真实帧生成 2 帧"),
        X4(3, "4x", "每个真实帧生成 3 帧"),
        X5(4, "5x", "每个真实帧生成 4 帧（需要解锁多帧生成）"),
        X6(5, "6x", "每个真实帧生成 5 帧（需要解锁多帧生成）");

        public final int framesToGenerate;
        public final String label;
        public final String desc;

        FrameGen(int framesToGenerate, String label, String desc) {
            this.framesToGenerate = framesToGenerate;
            this.label = label;
            this.desc = desc;
        }
    }

    /**
     * DLSS-G 工作模式（sl::DLSSGMode）。
     *
     * 固定倍数会把真实帧率摊薄成「呈现帧率 / (N+1)」——多帧插得越多，真实帧出来的间隔越长，
     * 输入延迟同步变大。动态模式让 DLSS-G 按目标帧率只生成够用的张数：真实帧率掉下来时
     * 自动少插几张把延迟压住，真实帧率有余量时才用满倍数。
     */
    public enum DlssgMode {
        FIXED(0, "固定倍数", "一直按档位生成固定张数。倍数越高越顺，但真实帧率被摊薄，输入延迟同步变高。"),
        AUTO(1, "自动", "由 DLSS-G 自行决定开或关（eAuto），生成张数仍按档位。"),
        DYNAMIC(2, "动态（按目标帧率）", "按目标帧率只生成够用的张数（eDynamic）：真实帧率越低插得越少，"
                + "延迟被压住；有余量时才用满档位。需要驱动支持动态多帧，不支持时自动退回固定倍数。");

        public final int slValue;
        public final String label;
        public final String desc;

        DlssgMode(int slValue, String label, String desc) {
            this.slValue = slValue;
            this.label = label;
            this.desc = desc;
        }
    }

    /** DLSS 超分模型预设，对应 sl::DLSSPreset。只对超分（SR）生效，帧生成没有模型参数。 */
    public enum SrPreset {
        AUTO(0, "自动", "由 DLSS 按当前档位自选，通常是较新的 Transformer 模型"),
        J(10, "J", "接近 K，残影略少但闪烁略多"),
        K(11, "K", "Transformer，DLAA/平衡/质量档默认，画质最好但更吃性能"),
        L(12, "L", "Transformer，超级性能档默认，更锐利稳定、开销高于 J/K"),
        M(13, "M", "Transformer，性能档默认，画质接近 L、速度接近 J/K");

        public final int slValue;
        public final String label;
        public final String desc;

        SrPreset(int slValue, String label, String desc) {
            this.slValue = slValue;
            this.label = label;
            this.desc = desc;
        }
    }

    /** FSR 帧生成档位。分析式 FSR3 每张真实帧只生成 1 帧，没有 3x/4x 可调。 */
    public enum FsrFrameGen {
        OFF("关闭", "不做帧生成"),
        X2("2x", "每个真实帧生成 1 帧");

        public final String label;
        public final String desc;

        FsrFrameGen(String label, String desc) {
            this.label = label;
            this.desc = desc;
        }
    }

    /** 当前生效的帧生成后端。三个档位互相排斥，同时只可能有一个不是关闭。 */
    public enum Backend {
        DLSS("DLSS"), FSR("FSR"), XESS("XeSS");

        public final String label;

        Backend(String label) {
            this.label = label;
        }
    }

    /**
     * XeSS-SR 档位，值就是 xess_quality_settings_t。
     * 具体降到多大分辨率不在这里写死：由 nativeGetRenderSize 问厂商要，档位只是「哪一档」。
     */
    public enum XessSr {
        OFF(-1, "关闭", "不降分辨率，不做超分"),
        AA(106, "XeSS-AA", "原生分辨率 + AI 抗锯齿，不降分辨率"),
        ULTRA_QUALITY_PLUS(105, "极致质量+", "比极致质量再多一点分辨率"),
        ULTRA_QUALITY(104, "极致质量", "降得最少，画质最好、省得最少"),
        QUALITY(103, "质量", "画质与性能折中，偏画质"),
        BALANCED(102, "平衡", "画质与性能折中"),
        PERFORMANCE(101, "性能", "偏性能，画面细节损失开始明显"),
        ULTRA_PERFORMANCE(100, "超级性能", "降得最多、省得最多，画面最糊");

        public final int nativeValue;
        public final String label;
        public final String desc;

        XessSr(int nativeValue, String label, String desc) {
            this.nativeValue = nativeValue;
            this.label = label;
            this.desc = desc;
        }
    }

    /** FSR-SR 档位，值就是 FfxApiUpscaleQualityMode。渲染分辨率同样由厂商算。 */
    public enum FsrSr {
        OFF(-1, "关闭", "不降分辨率，不做超分"),
        NATIVE_AA(0, "FSR-AA", "原生分辨率 + 抗锯齿，不降分辨率"),
        QUALITY(1, "质量", "画质与性能折中，偏画质"),
        BALANCED(2, "平衡", "画质与性能折中"),
        PERFORMANCE(3, "性能", "偏性能，画面细节损失开始明显"),
        ULTRA_PERFORMANCE(4, "超级性能", "降得最多、省得最多，画面最糊");

        public final int nativeValue;
        public final String label;
        public final String desc;

        FsrSr(int nativeValue, String label, String desc) {
            this.nativeValue = nativeValue;
            this.label = label;
            this.desc = desc;
        }
    }

    private static final ForgeConfigSpec.Builder BUILDER = new ForgeConfigSpec.Builder();

    public static final ForgeConfigSpec.EnumValue<FrameGen> FRAME_GEN = BUILDER
            .comment("DLSS 帧生成档位（2x/3x/4x/5x/6x）。需要硬件加速 GPU 调度已开启。"
                    + "3x 以上要先开 dlssgUnlockMultiFrame 并重启游戏，否则会被自动夹回 2x。"
                    + "注意倍数是「以真实帧为代价换呈现帧」：N 倍下真实帧率上限 = 显示器刷新率 / N。"
                    + "100Hz 屏上 2x 的真实帧上限已是 50，6x 只有约 16.7——画面变顺但操作变钝，"
                    + "除非真实帧率本来就很低，否则建议留在 2x。")
            .defineEnum("frameGeneration", FrameGen.OFF);

    public static final ForgeConfigSpec.EnumValue<FsrFrameGen> FSR_FRAME_GEN = BUILDER
            .comment("FSR 帧生成档位。与 DLSS / XeSS 互斥，开了这个就会把另外两个关掉。"
                    + "AMD 的分析式 FSR3 每张真实帧只生成 1 帧，3x/4x 是 SDK 硬限制，没有就是没有。")
            .defineEnum("fsrFrameGeneration", FsrFrameGen.OFF);

    public static final ForgeConfigSpec.BooleanValue FSR_FG_ASYNC = BUILDER
            .comment("FSR 帧生成的生成工作跑异步计算队列（allowAsyncWorkloads，AMD 官方示例默认开）。"
                    + "可与其他 GPU 工作重叠、降低帧生成开销；个别驱动上可能反而更抖，关掉可对比。")
            .define("fsrFgAsyncWorkloads", true);

    public static final ForgeConfigSpec.BooleanValue FSR_FG_ONLY_GENERATED = BUILDER
            .comment("诊断用：只呈现 FSR 生成出来的插帧（onlyPresentGenerated），真实帧不上屏。"
                    + "用来确认插帧是否真的产出画面，会明显影响观感，不用于正常游玩。")
            .define("fsrFgOnlyPresentGenerated", false);

    public static final ForgeConfigSpec.DoubleValue XESS_SCENE_THRESHOLD = BUILDER
            .comment("XeSS 场景变化检测阈值（0.0~1.0，SDK 默认 0.7）。越高越容易判定场景突变并暂停插帧"
                    + "（切场景/开背包不糊），越低越坚持插帧（更流畅，但突变瞬间可能糊一下）。")
            .defineInRange("xessSceneChangeThreshold", 0.7D, 0.0D, 1.0D);

    public static final ForgeConfigSpec.BooleanValue XESS_LEGACY_SCALE = BUILDER
            .comment("XeSS-SR 传统档位倍率（xessForceLegacyScaleFactors）。"
                    + "启用：平衡档=1.7x、质量=1.5x 等传统比例；关闭：SDK 按本机 GPU 自选，"
                    + "有时更激进（如这版 SDK 平衡档建议 50% 分辨率）。改值后下次 SR 重建时生效。")
            .define("xessLegacyScaleFactors", false);

    public static final ForgeConfigSpec.IntValue XELL_FPS_LIMIT = BUILDER
            .comment("XeLL 原生限帧（FPS，0=不限）。仅在 FSR / XeSS 后端运行有效——DLSS-G 走 Reflex。"
                    + "Reflex 限帧只控制 DLSS-G；这条路要限制真实帧必须调 XeLL。")
            .defineInRange("xellFpsLimit", 0, 0, 1000);

    public static final ForgeConfigSpec.EnumValue<FrameGen> XESS_FRAME_GEN = BUILDER
            .comment("XeSS 帧生成档位。非 Intel 显卡上限是 2x，选高了会按本机能力自动降档。"
                    + "与 DLSS / FSR 互斥。")
            .defineEnum("xessFrameGeneration", FrameGen.OFF);

    public static final ForgeConfigSpec.EnumValue<XessSr> XESS_SR = BUILDER
            .comment("XeSS 超分档位。只在帧生成后端是 XeSS 时生效；实际渲染分辨率由 XeSS 自己算，"
                    + "改档位会重建超分上下文（一次卡顿，之后正常）。")
            .defineEnum("xessSuperResolution", XessSr.OFF);

    public static final ForgeConfigSpec.EnumValue<FsrSr> FSR_SR = BUILDER
            .comment("FSR 超分档位。只在帧生成后端是 FSR 时生效；实际渲染分辨率由 FSR 自己算，"
                    + "改档位不用重建上下文。")
            .defineEnum("fsrSuperResolution", FsrSr.OFF);

    public static final ForgeConfigSpec.BooleanValue FSR_SR_SHARPEN = BUILDER
            .comment("FSR 的 RCAS 锐化。只有 FSR 有这个接口，XeSS-SR 的导出表里没有锐化。")
            .define("fsrSrSharpen", true);

    public static final ForgeConfigSpec.DoubleValue FSR_SR_SHARPNESS = BUILDER
            .comment("FSR 锐化强度 0.0~1.0。越高越锐，也越容易把噪点和锯齿一起放大。")
            .defineInRange("fsrSrSharpness", 0.7D, 0.0D, 1.0D);

    public static final ForgeConfigSpec.BooleanValue REFLEX_BOOST = BUILDER
            .comment("超低延迟增强（Reflex Boost），仅在帧生成运行时生效，可能增加功耗；关闭后保留基础 Reflex。")
            .define("reflexBoost", true);

    public static final ForgeConfigSpec.EnumValue<Quality> MODE = BUILDER
            .comment("【DLSS 超分档位】把世界渲染降分辨率再由 DLSS 放大，与帧生成同开后真实帧率更高。"
                    + "仅 DLSS 后端；FSR / XeSS 后端的超分在各自页面设置。")
            .defineEnum("mode", Quality.OFF);

    public static final ForgeConfigSpec.DoubleValue SHARPNESS = BUILDER
            .comment("锐度 0.0~1.0。transformer 预设（K）下锐度由模型内部处理，这里仅作为额外提示。")
            .defineInRange("sharpness", 0.0D, 0.0D, 1.0D);

    public static final ForgeConfigSpec.EnumValue<SrPreset> SR_MODEL_PRESET = BUILDER
            .comment("DLSS 超分模型预设（自动/J/K/L/M）。只对超分档位非关闭时生效，帧生成不使用模型预设。")
            .defineEnum("srModelPreset", SrPreset.AUTO);

    public static final ForgeConfigSpec.BooleanValue FG_QUEUE_PARALLELISM = BUILDER
            .comment("DLSS-G 队列并行（queueParallelismMode=eBlockNoClientQueues，目前只有 Vulkan 支持）。"
                    + "开启后帧生成不再阻塞我们的呈现队列，可降低延迟；代价是改/销毁输入纹理前必须等它的完成 fence。")
            .define("fgQueueParallelism", true);

    public static final ForgeConfigSpec.BooleanValue FG_RETAIN_RESOURCES = BUILDER
            .comment("关闭帧生成时保留 DLSS-G 资源（eRetainResourcesWhenOff），避免下次开启时重建卡顿；会多占一些显存。")
            .define("fgRetainResourcesWhenOff", true);

    public static final ForgeConfigSpec.BooleanValue FG_MENU_DETECTION = BUILDER
            .comment("允许帧生成自行检测全屏菜单/暂停界面（eEnableFullscreenMenuDetection），"
                    + "关闭时它可能继续为静止画面生成插帧。")
            .define("fgFullscreenMenuDetection", true);

    public static final ForgeConfigSpec.BooleanValue FG_UI_RECOMPOSITION = BUILDER
            .comment("实验性 SDK UI 重合成开关。当前未提供独立 UI 透明度纹理，不满足 SDK 的完整输入要求，建议关闭。"
                    + "不能用颜色差代替真实透明度；Iris 不透明手部的相机运动修正独立生效。")
            .define("fgUiRecomposition", false);

    public static final ForgeConfigSpec.BooleanValue FG_SHOW_ONLY_INTERPOLATED = BUILDER
            .comment("诊断用：只显示帧生成出来的那一帧（eShowOnlyInterpolatedFrame），真实帧不上屏。"
                    + "用来确认插帧是否真的在产出新画面，会显著影响观感和延迟，不用于正常游玩。")
            .define("fgShowOnlyInterpolatedFrame", false);

    public static final ForgeConfigSpec.EnumValue<DlssgMode> DLSSG_MODE = BUILDER
            .comment("DLSS-G 工作模式。固定倍数 = 一直按档位插满；动态 = 按目标帧率只插够用的张数，"
                    + "真实帧率低时自动少插，把输入延迟压住（这也是「多帧档位越高延迟越高」的对症开关）。"
                    + "动态模式需要驱动支持，不支持时自动退回固定倍数并在日志里说明。")
            .defineEnum("dlssgMode", DlssgMode.DYNAMIC);

    public static final ForgeConfigSpec.IntValue DLSSG_DYNAMIC_FPS = BUILDER
            .comment("动态模式的每秒目标帧数。0 = 由 DLSS-G 自动取显示器刷新率。"
                    + "调低会在真实帧率不足时插得更少、延迟更低；只在动态模式下生效。")
            .defineInRange("dlssgDynamicTargetFps", 0, 0, 1000);

    public static final ForgeConfigSpec.BooleanValue DLSSG_UNLOCK_MFG = BUILDER
            .comment("解锁多帧生成（3x/4x）。官方把多帧生成限在 RTX 50 上，真正卡住它的是一处"
                    + "GPU 架构门禁：nvngx_dlssg.dll 里低于该架构的显卡一律只报上限 1（即 2x），"
                    + "3x/4x 请求会被 NGX 直接顶回。开启后会在 slInit 之前把这条门禁改成空操作"
                    + "（只改进程内的内存映像，不动磁盘文件），本机上限随之报成 5，多帧才真正生效。"
                    + "RTX 40 实测可用。改档后需重启游戏（门禁在初始化时打）。"
                    + "注意：2x 在 ~100Hz 显示器上已打满刷新率，更高倍数只在真实帧率较高或"
                    + "低刷新率场景才有意义，每多生成一帧就多一份 GPU 开销。")
            .define("dlssgUnlockMultiFrame", false);

    public static final ForgeConfigSpec.BooleanValue DEBUG_OVERLAY = BUILDER
            .comment("在屏幕左上角显示 FG 状态、最近 SDK 帧计数和调试设置；FSR 计数不是实测呈现。")
            .define("debugOverlay", true);

    public static final ForgeConfigSpec.IntValue REFLEX_FPS_LIMIT = BUILDER
            .comment("Reflex 真实帧限帧，仅在帧生成运行时生效；0 不额外限帧，Minecraft 原有限帧仍生效。")
            .defineInRange("reflexFpsLimit", 0, 0, 1000);

    public static final ForgeConfigSpec.BooleanValue ZERO_MOTION_VECTORS = BUILDER
            .comment("诊断用：将运动向量置零，保留深度；会影响运动补偿和插帧质量。")
            .define("zeroMotionVectors", false);

    public static final ForgeConfigSpec.BooleanValue RESET_HISTORY_EVERY_FRAME = BUILDER
            .comment("诊断用：每帧重置 FG 历史；会影响插帧，不用于正常游玩。")
            .define("resetHistoryEveryFrame", false);

    public static final ForgeConfigSpec.IntValue STATUS_LOG_INTERVAL = BUILDER
            .comment("FG 状态日志间隔（真实帧数）；0 仅记录状态变化，1 逐帧记录会影响性能。错误日志不受此项关闭。")
            .defineInRange("statusLogInterval", 120, 0, 36000);

    public static final ForgeConfigSpec.ConfigValue<String> STREAMLINE_PATH = BUILDER
            .comment("Streamline 运行时 DLL 所在目录（含 sl.interposer.dll、nvngx_dlss.dll、nvngx_dlssg.dll）。",
                     "留空则使用随 mod 打包的 Streamline 2.14.1 运行时，正常游玩不需要填。",
                     "只有想换用别的 SDK 版本时才指定：填了但这个目录里没有 sl.interposer.dll 时仍会回落到自带的那份。")
            .define("streamlinePath", "");

    public static final ForgeConfigSpec.ConfigValue<String> LOG_PATH = BUILDER
            .comment("Streamline 日志目录，留空则不写日志。")
            .define("logPath", "");

    public static final ForgeConfigSpec SPEC = BUILDER.build();

    private DLSSConfig() {}

    /** 当前该跑哪个后端；三个档位里唯一非关闭的那个。万一同时开着，按 DLSS > FSR > XeSS 取先。 */
    public static Backend activeBackend() {
        if (FRAME_GEN.get() != FrameGen.OFF) return Backend.DLSS;
        if (FSR_FRAME_GEN.get() != FsrFrameGen.OFF) return Backend.FSR;
        if (XESS_FRAME_GEN.get() != FrameGen.OFF) return Backend.XESS;
        return MODE.get() != Quality.OFF ? Backend.DLSS : null;
    }

    /** 当前后端要求的「每张真实帧后面再生成几张」；没有后端在跑时返回 0。 */
    public static int activeFramesToGenerate() {
        // Java 17 的 switch 还不认 case null，先挡掉
        Backend b = activeBackend();
        if (b == null) return 0;
        return switch (b) {
            case DLSS -> FRAME_GEN.get().framesToGenerate;
            case FSR -> 1;
            case XESS -> XESS_FRAME_GEN.get().framesToGenerate;
        };
    }

    /** 把除 chosen 之外的两个档位写回关闭 —— 界面上「只能开一个」就是靠这个保证的。 */
    public static void makeExclusive(Backend chosen) {
        if (chosen != Backend.DLSS && FRAME_GEN.get() != FrameGen.OFF) FRAME_GEN.set(FrameGen.OFF);
        if (chosen != Backend.FSR && FSR_FRAME_GEN.get() != FsrFrameGen.OFF)
            FSR_FRAME_GEN.set(FsrFrameGen.OFF);
        if (chosen != Backend.XESS && XESS_FRAME_GEN.get() != FrameGen.OFF)
            XESS_FRAME_GEN.set(FrameGen.OFF);
    }

    /** 当前后端的档位文字，供叠加层显示 */
    public static String activeBackendLabel() {
        Backend b = activeBackend();
        return b == null ? "关闭" : b.label;
    }

    public static String activeTierLabel() {
        Backend b = activeBackend();
        if (b == null) return "关闭";
        return switch (b) {
            case DLSS -> FRAME_GEN.get().label;
            case FSR -> FSR_FRAME_GEN.get().label;
            case XESS -> XESS_FRAME_GEN.get().label;
        };
    }

    public static int activeSrQuality() {
        Backend b = activeBackend();
        if (b == null) return -1;
        return switch (b) {
            case DLSS -> MODE.get() == Quality.OFF ? -1 : MODE.get().slValue;
            case FSR -> FSR_SR.get().nativeValue;
            case XESS -> XESS_SR.get().nativeValue;
        };
    }

    /** 锐化只有 FSR 有；XeSS 那边原生层会把它记进日志然后忽略 */
    public static boolean activeSrSharpen() {
        return activeBackend() == Backend.FSR && FSR_SR_SHARPEN.get();
    }

    public static float activeSrSharpness() {
        Backend b = activeBackend();
        if (b == null) return 0f;
        return switch (b) {
            case DLSS -> SHARPNESS.get().floatValue();
            case FSR -> FSR_SR_SHARPNESS.get().floatValue();
            case XESS -> 0f;
        };
    }

    /** 当前后端的超分档位文字，供叠加层显示 */
    public static String activeSrLabel() {
        Backend b = activeBackend();
        if (b == null) return "不适用";
        return switch (b) {
            case DLSS -> MODE.get().label;
            case FSR -> FSR_SR.get().label;
            case XESS -> XESS_SR.get().label;
        };
    }

    public static boolean isEnabled() {
        return MODE.get() != Quality.OFF;
    }
}
