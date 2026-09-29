package com.taolesi.dlssmc.compat;

import com.taolesi.dlssmc.DLSSMC;
import com.taolesi.dlssmc.config.DLSSConfig;
import com.taolesi.dlssmc.core.FGRuntime;
import net.caffeinemc.mods.sodium.api.config.ConfigEntryPoint;
import net.caffeinemc.mods.sodium.api.config.ConfigEntryPointForge;
import net.caffeinemc.mods.sodium.api.config.StorageEventHandler;
import net.caffeinemc.mods.sodium.api.config.option.Range;
import net.caffeinemc.mods.sodium.api.config.structure.BooleanOptionBuilder;
import net.caffeinemc.mods.sodium.api.config.structure.ConfigBuilder;
import net.caffeinemc.mods.sodium.api.config.structure.EnumOptionBuilder;
import net.caffeinemc.mods.sodium.api.config.structure.IntegerOptionBuilder;
import net.caffeinemc.mods.sodium.api.config.structure.OptionGroupBuilder;
import net.caffeinemc.mods.sodium.api.config.structure.OptionPageBuilder;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.ResourceLocation;
import net.neoforged.neoforge.common.ModConfigSpec;

/**
 * 全部选项注册进 Sodium 视频设置界面：段名「Frame Generation」，下面按后端分
 * 「DLSS 5」「FSR」「XeSS」三页。三个后端的档位互斥，开一个就把另外两个关掉。
 * 每页都列出「内置版本」（构建期打进 jar 的文件）与「实机版本」（运行时查出来的），
 * 两者可以不同：FSR 的 4.0.1 包在非 RDNA4 显卡上只会给到 3.1.6 的 provider。
 */
@ConfigEntryPointForge(DLSSMC.MOD_ID)
public class SodiumConfigIntegration implements ConfigEntryPoint {

    // Sodium 对每个可写选项都强制要求 storage handler，缺了会在启动时直接崩
    private static final StorageEventHandler FLUSH = DLSSConfig.SPEC::save;

    @Override
    public void registerConfigLate(ConfigBuilder b) {
        OptionGroupBuilder generation = b.createOptionGroup()
                .setName(Component.literal("常规"))
                .addOption(frameGeneration(b))
                .addOption(dlssgMode(b))
                .addOption(dlssgDynamicFps(b))
                .addOption(maxInputLatency(b))
                .addOption(bool(b, "skip_redundant_blit", "跳过 MC 的冗余全屏 blit",
                        "MC 每帧会把主目标整个 blit 到 GL 背缓冲，而我们接管上屏后画面来自自建的呈现窗口、"
                                + "背缓冲被盖住根本不会显示 —— 这次全屏 blit 是纯浪费，还排在交付拷贝之前"
                                + "白占关键路径的 GPU 时间。判据严格跟随接管状态，菜单/预检/交回 GL 之后照常 blit。"
                                + "只有在极端情况下（呈现窗口没盖住 MC 窗口）才会看到旧画面，那时关掉它即可。",
                        DLSSConfig.SKIP_REDUNDANT_BLIT))
                .addOption(bool(b, "dlssg_unlock_mfg", "解锁多帧生成（3x/4x）",
                        "官方把多帧生成限在 RTX 50，真正卡住它的是一处 GPU 架构门禁："
                                + "低于该架构的显卡只报上限 1（即 2x），3x/4x 请求会被 NGX 顶回。"
                                + "开启后会在 slInit 之前把这条门禁改成空操作（只改进程内存映像，不动磁盘文件），"
                                + "本机上限随之报成 5，多帧才真正生效。RTX 40 实测可用。"
                                + "改档后需重启游戏。注意：~100Hz 屏上 2x 已打满刷新率，更高倍数只在"
                                + "真实帧率较高或低刷新率场景才有意义。",
                        DLSSConfig.DLSSG_UNLOCK_MFG))
                .addOption(bool(b, "debug_overlay", "状态叠加层",
                        "在游戏画面左上角显示帧生成状态、真实渲染 / 呈现 FPS、后端版本和调试设置。",
                        DLSSConfig.DEBUG_OVERLAY))
                .addOption(bool(b, "reflex_boost", "超低延迟增强（Reflex Boost）",
                        "仅 DLSS 后端使用；关闭增强仍保留基础 Reflex。增强可能增加功耗、略降帧率。",
                        DLSSConfig.REFLEX_BOOST))
                .addOption(reflexFpsLimit(b))
                .addOption(bool(b, "fg_queue_parallelism", "队列并行（降延迟）",
                        "让帧生成不再阻塞我们的呈现队列（queueParallelismMode=eBlockNoClientQueues，目前只有 Vulkan 支持）。"
                                + "关掉可对比延迟差别。",
                        DLSSConfig.FG_QUEUE_PARALLELISM))
                .addOption(bool(b, "fg_retain_resources", "关闭时保留帧生成资源",
                        "关掉帧生成时不释放 DLSS-G 资源，避免下次开启时重建卡顿；代价是多占一些显存。",
                        DLSSConfig.FG_RETAIN_RESOURCES))
                .addOption(bool(b, "fg_menu_detection", "全屏菜单检测",
                        "允许帧生成自行检测全屏菜单/暂停界面；关闭时它可能继续为静止画面生成插帧。",
                        DLSSConfig.FG_MENU_DETECTION))
                .addOption(bool(b, "fg_ui_recomposition", "SDK UI 重合成（实验）",
                        "当前未提供独立 UI 透明度纹理，不满足 SDK 的完整输入要求，建议关闭。"
                                + "此开关不保证消除 HUD 或手部拖影；Iris 不透明手部的相机运动修正独立生效。",
                        DLSSConfig.FG_UI_RECOMPOSITION));

        OptionGroupBuilder superResolution = b.createOptionGroup()
                .setName(Component.literal("超分（SR）"))
                .addOption(srMode(b))
                .addOption(srModelPreset(b));

        OptionGroupBuilder diagnostics = b.createOptionGroup()
                .setName(Component.literal("调试"))
                .addOption(bool(b, "zero_motion_vectors", "运动向量置零",
                        "将输入运动向量置零但保留深度，用于对比运动补偿问题；会影响插帧质量，不用于正常游玩。",
                        DLSSConfig.ZERO_MOTION_VECTORS))
                .addOption(bool(b, "reset_history_every_frame", "每帧重置历史",
                        "每帧请求重置历史，用于定位时序残影；会减少甚至停止生成帧，不用于正常游玩。",
                        DLSSConfig.RESET_HISTORY_EVERY_FRAME))
                .addOption(statusLogInterval(b))
                .addOption(bool(b, "fg_show_only_interpolated", "只显示插帧",
                        "只把帧生成产出的那一帧上屏，真实帧不显示（eShowOnlyInterpolatedFrame）。"
                                + "用于确认插帧是否真在产出新画面，会明显影响观感和延迟，不用于正常游玩。",
                        DLSSConfig.FG_SHOW_ONLY_INTERPOLATED));

        OptionPageBuilder dlss = b.createOptionPage()
                // 本机实测：加载的是 DLSS 5 世代的模型（文件版本 310.9.1，见「实机版本」一项）
                .setName(Component.literal("DLSS 5"))
                .addOptionGroup(generation)
                .addOptionGroup(superResolution)
                .addOptionGroup(diagnostics)
                .addOptionGroup(versions(b, DLSSConfig.Backend.DLSS, "DLSS"));

        OptionPageBuilder fsr = b.createOptionPage()
                .setName(Component.literal("FSR"))
                .addOptionGroup(b.createOptionGroup()
                        .setName(Component.literal("常规"))
                        .addOption(fsrFrameGeneration(b)))
                .addOptionGroup(b.createOptionGroup()
                        .setName(Component.literal("调优"))
                        .addOption(bool(b, "fsr_fg_async", "异步计算（降负载）",
                                "让帧生成的生成工作跑异步计算队列（allowAsyncWorkloads，AMD 官方示例默认开）。"
                                        + "可与其他 GPU 工作重叠、降低开销；个别驱动上可能更抖，关掉可对比。",
                                DLSSConfig.FSR_FG_ASYNC))
                        .addOption(bool(b, "fsr_fg_only_generated", "只显示插帧",
                                "只呈现 FSR 生成出来的插帧（onlyPresentGenerated），真实帧不上屏。"
                                        + "用于确认插帧是否真的产出画面，会明显影响观感，不用于正常游玩。",
                                DLSSConfig.FSR_FG_ONLY_GENERATED)))
                .addOptionGroup(b.createOptionGroup()
                        .setName(Component.literal("超分（SR）"))
                        .addOption(fsrSrMode(b))
                        .addOption(bool(b, "fsr_sr_sharpen", "RCAS 锐化",
                                "仅在 FSR 帧生成后端运行且 FSR 超分开启时使用 RCAS 锐化。",
                                DLSSConfig.FSR_SR_SHARPEN))
                        .addOption(fsrSrSharpness(b)))
                .addOptionGroup(versions(b, DLSSConfig.Backend.FSR, "FSR"));

        OptionPageBuilder xess = b.createOptionPage()
                .setName(Component.literal("XeSS"))
                .addOptionGroup(b.createOptionGroup()
                        .setName(Component.literal("常规"))
                        .addOption(xessFrameGeneration(b)))
                .addOptionGroup(b.createOptionGroup()
                        .setName(Component.literal("调优"))
                        .addOption(xessSceneThreshold(b))
                        .addOption(bool(b, "xess_legacy_scale", "传统档位倍率",
                                "XeSS-SR 用传统倍率（平衡 1.7x/质量 1.5x 等）。关闭时 SDK 按本机 GPU 自选，"
                                        + "本机有时更激进（如这版 SDK 平衡档建议 50% 分辨率）。改值后下次 SR 重建生效。",
                                DLSSConfig.XESS_LEGACY_SCALE))
                        .addOption(xellFpsLimit(b)))
                .addOptionGroup(b.createOptionGroup()
                        .setName(Component.literal("超分（SR）"))
                        .addOption(xessSrMode(b)))
                .addOptionGroup(versions(b, DLSSConfig.Backend.XESS, "XeSS"));

        // 不显式设色时 Sodium 会按 configId 哈希挑预设配色。
        b.registerOwnModOptions()
                .setName("Frame Generation")
                .setColorTheme(b.createColorTheme().setBaseThemeRGB(0x76B900))
                .addPage(dlss)
                .addPage(fsr)
                .addPage(xess);
    }

    private OptionGroupBuilder versions(ConfigBuilder b, DLSSConfig.Backend backend, String label) {
        return b.createOptionGroup()
                .setName(Component.literal("版本"))
                .addOption(readOnly(b, backend.name().toLowerCase() + "_builtin_version",
                        label + " 内置版本",
                        "构建期打进 jar 的厂商文件版本，与 native/vendor/MANIFEST.sha256 对应。",
                        () -> FGRuntime.builtinVersion(backend)))
                .addOption(readOnly(b, backend.name().toLowerCase() + "_runtime_version",
                        label + " 实机版本",
                        "运行时向厂商库查出来的版本；与内置版本不同说明库按本机能力降了级"
                                + "（例如 FSR 4.0.1 的包在非 RDNA4 显卡上只给 3.1.6）。",
                        () -> FGRuntime.get().runtimeVersion(backend)));
    }

    /**
     * 只读展示项。Sodium 没有「纯文本」控件，这里用一个禁用掉的整数项，
     * 把要显示的文字放进取值格式化器 —— 它是渲染时才求值的，所以能显示运行期查到的版本。
     */
    private static IntegerOptionBuilder readOnly(ConfigBuilder b, String path, String name,
                                                 String tooltip,
                                                 java.util.function.Supplier<String> text) {
        return b.createIntegerOption(id(path))
                .setName(Component.literal(name))
                .setTooltip(Component.literal(tooltip))
                .setEnabled(false)
                .setControlHiddenWhenDisabled(false)
                .setDefaultValue(0)
                .setRange(new Range(0, 1, 1))
                .setValueFormatter(v -> Component.literal(text.get()))
                .setBinding((Integer value) -> { }, () -> 0)
                .setStorageHandler(FLUSH);
    }

    private static EnumOptionBuilder<DLSSConfig.Quality> srMode(ConfigBuilder b) {
        ModConfigSpec.EnumValue<DLSSConfig.Quality> value = DLSSConfig.MODE;
        return b.createEnumOption(id("sr_mode"), DLSSConfig.Quality.class)
                .setName(Component.literal("超分档位"))
                .setTooltip(q -> Component.literal(q.desc + "。超分会把世界渲染改成低分辨率再放大，"
                        + "与帧生成同时开启时真实帧率会更高。"))
                .setDefaultValue(value.getDefault())
                .setElementNameProvider(q -> Component.literal(q.label))
                .setBinding((DLSSConfig.Quality v) -> {
                    value.set(v);
                    DLSSMC.refresh();
                }, value::get)
                .setStorageHandler(FLUSH);
    }

    private static EnumOptionBuilder<DLSSConfig.FsrSr> fsrSrMode(ConfigBuilder b) {
        ModConfigSpec.EnumValue<DLSSConfig.FsrSr> value = DLSSConfig.FSR_SR;
        return b.createEnumOption(id("fsr_sr_mode"), DLSSConfig.FsrSr.class)
                .setName(Component.literal("FSR 超分档位"))
                .setTooltip(q -> Component.literal(q.desc + "。仅在 FSR 帧生成后端运行时使用；"
                        + "实际渲染尺寸向 FSR 厂商接口查询。原版手部与 HUD 保持全分辨率，"
                        + "Iris 手部可能作为世界的一部分参与超分。"))
                .setDefaultValue(value.getDefault())
                .setElementNameProvider(q -> Component.literal(q.label))
                .setBinding((DLSSConfig.FsrSr v) -> {
                    value.set(v);
                    DLSSMC.refresh();
                }, value::get)
                .setStorageHandler(FLUSH);
    }

    private static IntegerOptionBuilder fsrSrSharpness(ConfigBuilder b) {
        ModConfigSpec.DoubleValue value = DLSSConfig.FSR_SR_SHARPNESS;
        return b.createIntegerOption(id("fsr_sr_sharpness"))
                .setName(Component.literal("RCAS 锐化强度"))
                .setTooltip(Component.literal("仅在 FSR 帧生成后端运行、FSR 超分与 RCAS 锐化均开启时生效。"
                        + "越高越锐，也越容易放大噪点和锯齿。"))
                .setDefaultValue((int) Math.round(value.getDefault() * 100.0D))
                .setRange(new Range(0, 100, 5))
                .setValueFormatter(v -> Component.literal(v + "%"))
                .setBinding((Integer v) -> value.set(v / 100.0D),
                        () -> (int) Math.round(value.get() * 100.0D))
                .setStorageHandler(FLUSH);
    }

    private static EnumOptionBuilder<DLSSConfig.XessSr> xessSrMode(ConfigBuilder b) {
        ModConfigSpec.EnumValue<DLSSConfig.XessSr> value = DLSSConfig.XESS_SR;
        return b.createEnumOption(id("xess_sr_mode"), DLSSConfig.XessSr.class)
                .setName(Component.literal("XeSS 超分档位"))
                .setTooltip(q -> Component.literal(q.desc + "。仅在 XeSS 帧生成后端运行时使用；"
                        + "实际渲染尺寸向 XeSS 厂商接口查询。原版手部与 HUD 保持全分辨率，"
                        + "Iris 手部可能作为世界的一部分参与超分。"))
                .setDefaultValue(value.getDefault())
                .setElementNameProvider(q -> Component.literal(q.label))
                .setBinding((DLSSConfig.XessSr v) -> {
                    value.set(v);
                    DLSSMC.refresh();
                }, value::get)
                .setStorageHandler(FLUSH);
    }

    private static IntegerOptionBuilder xessSceneThreshold(ConfigBuilder b) {
        ModConfigSpec.DoubleValue value = DLSSConfig.XESS_SCENE_THRESHOLD;
        return b.createIntegerOption(id("xess_scene_threshold"))
                .setName(Component.literal("场景变化检测"))
                .setTooltip(Component.literal("XeSS 判定场景突变并暂停插帧的阈值（SDK 默认 70%）。"
                        + "调高：切场景/开背包不糊但插帧更常停；调低：更坚持插帧更流畅，"
                        + "突变瞬间可能糊一下。"))
                .setDefaultValue((int) Math.round(value.getDefault() * 100.0D))
                .setRange(new Range(0, 100, 5))
                .setValueFormatter(v -> Component.literal(v == 70 ? "70%（SDK 默认）" : v + "%"))
                .setBinding((Integer v) -> value.set(v / 100.0D),
                        () -> (int) Math.round(value.get() * 100.0D))
                .setStorageHandler(FLUSH);
    }

    private static EnumOptionBuilder<DLSSConfig.SrPreset> srModelPreset(ConfigBuilder b) {
        ModConfigSpec.EnumValue<DLSSConfig.SrPreset> value = DLSSConfig.SR_MODEL_PRESET;
        return b.createEnumOption(id("sr_model_preset"), DLSSConfig.SrPreset.class)
                .setName(Component.literal("DLSS 模型预设"))
                .setTooltip(p -> Component.literal(p.desc + "。模型预设只作用于超分（SR），"
                        + "帧生成没有模型参数；超分档位为关闭时此项无效。"))
                .setDefaultValue(value.getDefault())
                .setElementNameProvider(p -> Component.literal(p.label))
                .setBinding((DLSSConfig.SrPreset v) -> {
                    value.set(v);
                    DLSSMC.refresh();
                }, value::get)
                .setStorageHandler(FLUSH);
    }

    private static EnumOptionBuilder<DLSSConfig.FrameGen> frameGeneration(ConfigBuilder b) {
        ModConfigSpec.EnumValue<DLSSConfig.FrameGen> value = DLSSConfig.FRAME_GEN;
        return b.createEnumOption(id("frame_generation"), DLSSConfig.FrameGen.class)
                .setName(Component.literal("DLSS 帧生成档位"))
                .setTooltip(f -> Component.literal(f.desc + "。"
                        + "需要硬件加速 GPU 调度已开启；官方限 RTX 50 才有多帧，本机上限不足时会自动降档。"
                        + "3x 以上要先开「解锁多帧生成」并重启游戏，否则会被夹回 2x。"
                        + "倍数是拿真实帧换呈现帧：N 倍下真实帧率上限 = 刷新率 / N，"
                        + "100Hz 屏上 6x 只有约 16.7，操作会明显变钝。"
                        + "三个后端互斥：选非关闭会同时把 FSR 与 XeSS 关掉。"))
                .setDefaultValue(value.getDefault())
                .setElementNameProvider(f -> Component.literal(f.label))
                .setBinding((DLSSConfig.FrameGen v) -> {
                    value.set(v);
                    if (v != DLSSConfig.FrameGen.OFF) DLSSConfig.makeExclusive(DLSSConfig.Backend.DLSS);
                    DLSSMC.refresh();
                }, value::get)
                .setStorageHandler(FLUSH);
    }

    private static EnumOptionBuilder<DLSSConfig.FsrFrameGen> fsrFrameGeneration(ConfigBuilder b) {
        ModConfigSpec.EnumValue<DLSSConfig.FsrFrameGen> value = DLSSConfig.FSR_FRAME_GEN;
        return b.createEnumOption(id("fsr_frame_generation"), DLSSConfig.FsrFrameGen.class)
                .setName(Component.literal("FSR 帧生成档位"))
                .setTooltip(f -> Component.literal(f.desc + "。FSR 帧生成走 D3D12 呈现，"
                        + "分析式实现每张真实帧只生成 1 帧，没有 3x/4x。"
                        + "三个后端互斥：选非关闭会同时把 DLSS 与 XeSS 关掉。"))
                .setDefaultValue(value.getDefault())
                .setElementNameProvider(f -> Component.literal(f.label))
                .setBinding((DLSSConfig.FsrFrameGen v) -> {
                    value.set(v);
                    if (v != DLSSConfig.FsrFrameGen.OFF) DLSSConfig.makeExclusive(DLSSConfig.Backend.FSR);
                    DLSSMC.refresh();
                }, value::get)
                .setStorageHandler(FLUSH);
    }

    private static EnumOptionBuilder<DLSSConfig.FrameGen> xessFrameGeneration(ConfigBuilder b) {
        ModConfigSpec.EnumValue<DLSSConfig.FrameGen> value = DLSSConfig.XESS_FRAME_GEN;
        return b.createEnumOption(id("xess_frame_generation"), DLSSConfig.FrameGen.class)
                .setName(Component.literal("XeSS 帧生成档位"))
                .setTooltip(f -> Component.literal(f.desc + "。XeSS 帧生成走 D3D12 呈现并强制使用 XeLL，"
                        + "非 Intel 显卡的上限是 2x，选高了会按本机能力自动降档（见「实机版本」）。"
                        + "三个后端互斥：选非关闭会同时把 DLSS 与 FSR 关掉。"))
                .setDefaultValue(value.getDefault())
                .setElementNameProvider(f -> Component.literal(f.label))
                .setBinding((DLSSConfig.FrameGen v) -> {
                    value.set(v);
                    if (v != DLSSConfig.FrameGen.OFF)
                        DLSSConfig.makeExclusive(DLSSConfig.Backend.XESS);
                    DLSSMC.refresh();
                }, value::get)
                .setStorageHandler(FLUSH);
    }

    private static EnumOptionBuilder<DLSSConfig.DlssgMode> dlssgMode(ConfigBuilder b) {
        ModConfigSpec.EnumValue<DLSSConfig.DlssgMode> value = DLSSConfig.DLSSG_MODE;
        return b.createEnumOption(id("dlssg_mode"), DLSSConfig.DlssgMode.class)
                .setName(Component.literal("工作模式"))
                .setTooltip(q -> Component.literal("固定倍数：一直按档位插满，档位越高越顺但真实帧率被摊薄、"
                        + "输入延迟同步变高。动态：按目标帧率只插够用的张数，真实帧率掉下来时自动少插几张"
                        + "把延迟压住，有余量时才用满档位 —— 觉得多帧延迟高就先切这个。"
                        + "动态需要驱动支持动态多帧，不支持时会退回固定倍数并在日志里说明。"))
                .setDefaultValue(value.getDefault())
                .setElementNameProvider(q -> Component.literal(q.label))
                .setBinding(value::set, value::get)
                .setStorageHandler(FLUSH);
    }

    private static IntegerOptionBuilder maxInputLatency(ConfigBuilder b) {
        return integer(b, "max_input_latency", "输入延迟预算",
                "插帧会让一个真实帧占掉 N+1 个刷新周期，所以 输入到光子 ≈ (N+1)/刷新率 + 呈现延迟："
                        + "倍数越高画面越顺、输入延迟也线性变高，这是插帧的固有代价。"
                        + "设了预算后模组会按这条公式自动把倍数压到放得下的档位（至少保留 2x），"
                        + "并在状态叠加层里写明降档原因。0 = 不限制。"
                        + "觉得手感迟钝就往下调（例如 35）。",
                DLSSConfig.MAX_INPUT_LATENCY, new Range(0, 500, 5),
                v -> Component.literal(v == 0 ? "不限制" : v + " ms"));
    }

    private static IntegerOptionBuilder dlssgDynamicFps(ConfigBuilder b) {
        return integer(b, "dlssg_dynamic_fps", "动态模式目标帧率",
                "动态模式希望达到的每秒帧数（插帧后）。0 表示由 DLSS-G 自动取显示器刷新率。"
                        + "调低会在真实帧率不足时插得更少、延迟更低；只在动态模式下生效。",
                DLSSConfig.DLSSG_DYNAMIC_FPS, new Range(0, 1000, 5),
                v -> Component.literal(v == 0 ? "跟显示器刷新率" : v + " FPS"));
    }

    private static IntegerOptionBuilder reflexFpsLimit(ConfigBuilder b) {
        return integer(b, "reflex_fps_limit", "Reflex 限帧",
                "限制真实帧而非插帧后 FPS，仅在帧生成运行时生效；0 不额外限帧，游戏自身限帧仍有效。",
                DLSSConfig.REFLEX_FPS_LIMIT, new Range(0, 1000, 5),
                v -> Component.literal(v == 0 ? "不额外限帧" : v + " FPS"));
    }

    private static IntegerOptionBuilder xellFpsLimit(ConfigBuilder b) {
        return integer(b, "xell_fps_limit", "XeLL 限帧",
                "XeLL 原生限帧，仅在 FSR / XeSS 后端运行有效。Reflex 限帧只控 DLSS-G；"
                        + "这两条路要限制真实帧必须调 XeLL 自己的接口。",
                DLSSConfig.XELL_FPS_LIMIT, new Range(0, 1000, 5),
                v -> Component.literal(v == 0 ? "不额外限帧" : v + " FPS"));
    }

    private static IntegerOptionBuilder statusLogInterval(ConfigBuilder b) {
        return integer(b, "status_log_interval", "状态日志间隔",
                "控制 dlssmc_fg.log 的 FG 状态采样条数（按真实帧计）；状态变化和错误仍会记录，逐帧写盘会影响性能。",
                DLSSConfig.STATUS_LOG_INTERVAL, new Range(0, 36000, 30),
                v -> Component.literal(v == 0 ? "仅状态变化" : "每 " + v + " 帧"));
    }

    private static IntegerOptionBuilder integer(ConfigBuilder b, String path, String name, String tooltip,
                                                 ModConfigSpec.IntValue value, Range range,
                                                 net.caffeinemc.mods.sodium.api.config.option.ControlValueFormatter formatter) {
        return b.createIntegerOption(id(path))
                .setName(Component.literal(name))
                .setTooltip(Component.literal(tooltip))
                .setDefaultValue(value.getDefault())
                .setRange(range)
                .setValueFormatter(formatter)
                .setBinding(value::set, value::get)
                .setStorageHandler(FLUSH);
    }

    private static BooleanOptionBuilder bool(ConfigBuilder b, String path, String name, String tooltip,
                                             ModConfigSpec.BooleanValue value) {
        return b.createBooleanOption(id(path))
                .setName(Component.literal(name))
                .setTooltip(Component.literal(tooltip))
                .setDefaultValue(value.getDefault())
                .setBinding(value::set, value::get)
                .setStorageHandler(FLUSH);
    }

    private static ResourceLocation id(String path) {
        return ResourceLocation.fromNamespaceAndPath(DLSSMC.MOD_ID, path);
    }
}
