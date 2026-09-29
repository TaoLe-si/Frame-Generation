package com.taolesi.dlssmc.compat;

import com.google.common.collect.ImmutableList;
import com.taolesi.dlssmc.DLSSMC;
import com.taolesi.dlssmc.config.DLSSConfig;
import com.taolesi.dlssmc.core.FGRuntime;
import me.jellysquid.mods.sodium.client.gui.options.Option;
import me.jellysquid.mods.sodium.client.gui.options.OptionFlag;
import me.jellysquid.mods.sodium.client.gui.options.OptionGroup;
import me.jellysquid.mods.sodium.client.gui.options.OptionImpl;
import me.jellysquid.mods.sodium.client.gui.options.OptionPage;
import me.jellysquid.mods.sodium.client.gui.options.control.ControlValueFormatter;
import me.jellysquid.mods.sodium.client.gui.options.control.CyclingControl;
import me.jellysquid.mods.sodium.client.gui.options.control.SliderControl;
import me.jellysquid.mods.sodium.client.gui.options.control.TickBoxControl;
import me.jellysquid.mods.sodium.client.gui.options.storage.OptionStorage;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.ResourceLocation;
import net.minecraftforge.common.ForgeConfigSpec;
import org.embeddedt.embeddium.api.OptionGUIConstructionEvent;
import org.embeddedt.embeddium.client.gui.options.OptionIdentifier;

import java.util.Locale;

/**
 * 全部选项注册进 Embeddium 视频设置界面：多出「Frame Generation」「FSR」「XeSS」三页。
 * 三个后端的档位互斥，开一个就把另外两个关掉。
 * 每页都列出「内置版本」（构建期打进 jar 的文件）与「实机版本」（运行时查出来的），
 * 两者可以不同：FSR 的 4.0.1 包在非 RDNA4 显卡上只会给到 3.1.6 的 provider。
 *
 * <p>1.20.1 这边用的是 Embeddium 0.3.31（Sodium 0.5 世代的旧配置 API）：扩展点是
 * {@link OptionGUIConstructionEvent}，SodiumOptionsGUI 每次构造时 post，我们往里塞
 * OptionPage。页面在每次 post 时重建 —— 选项的当前值是构造时从配置快照下来的，
 * 复用旧实例会显示过期值（互斥关掉另一个后端后就对不上了）。
 */
public final class SodiumConfigIntegration {

    /**
     * Embeddium 对每个选项都要求一个 storage，save() 在点「应用」时被调。
     * 我们的值真正存在 ForgeConfigSpec 里，binding 直接读写它，这里只负责落盘。
     */
    private static final OptionStorage<Void> STORAGE = new OptionStorage<>() {
        @Override
        public Void getData() {
            return null;
        }

        @Override
        public void save() {
            DLSSConfig.SPEC.save();
        }
    };

    private SodiumConfigIntegration() {}

    /** 由 {@link DLSSMC} 在构造时调用；Embeddium 不在就只是没有设置页，不该拖垮整个 mod。 */
    public static void register() {
        try {
            OptionGUIConstructionEvent.BUS.addListener(e -> {
                e.addPage(dlssPage());
                e.addPage(fsrPage());
                e.addPage(xessPage());
            });
        } catch (Throwable t) {
            org.apache.logging.log4j.LogManager.getLogger("dlssmc")
                    .warn("Embeddium 设置页注册失败，视频设置里不会出现帧生成选项", t);
        }
    }

    // ------------------------------------------------------------------ DLSS

    private static OptionPage dlssPage() {
        OptionGroup generation = group("dlss_generation", "常规")
                .add(frameGeneration())
                .add(enumOption("dlssg_mode", "工作模式",
                        "固定倍数：一直按档位插满，档位越高越顺但真实帧率被摊薄、输入延迟同步变高。"
                                + "动态：按目标帧率只插够用的张数，真实帧率掉下来时自动少插几张把延迟压住，"
                                + "有余量时才用满档位 —— 觉得多帧延迟高就先切这个。"
                                + "动态需要驱动支持动态多帧，不支持时会退回固定倍数并在日志里说明。",
                        DLSSConfig.DLSSG_MODE, DLSSConfig.DlssgMode.class, null))
                .add(dlssgDynamicFps())
                .add(bool("dlssg_unlock_mfg", "解锁多帧生成（3x/4x）",
                        "官方把多帧生成限在 RTX 50，真正卡住它的是一处 GPU 架构门禁："
                                + "低于该架构的显卡只报上限 1（即 2x），3x/4x 请求会被 NGX 顶回。"
                                + "开启后会在 slInit 之前把这条门禁改成空操作（只改进程内存映像，不动磁盘文件），"
                                + "本机上限随之报成 5，多帧才真正生效。RTX 40 实测可用。"
                                + "改档后需重启游戏。注意：~100Hz 屏上 2x 已打满刷新率，更高倍数只在"
                                + "真实帧率较高或低刷新率场景才有意义。",
                        DLSSConfig.DLSSG_UNLOCK_MFG, OptionFlag.REQUIRES_GAME_RESTART))
                .add(bool("debug_overlay", "状态叠加层",
                        "在游戏画面左上角显示帧生成状态、真实渲染 / 呈现 FPS、后端版本和调试设置。",
                        DLSSConfig.DEBUG_OVERLAY))
                .add(bool("reflex_boost", "超低延迟增强（Reflex Boost）",
                        "仅 DLSS 后端使用；关闭增强仍保留基础 Reflex。增强可能增加功耗、略降帧率。",
                        DLSSConfig.REFLEX_BOOST))
                .add(reflexFpsLimit())
                .add(bool("fg_queue_parallelism", "队列并行（降延迟）",
                        "让帧生成不再阻塞我们的呈现队列（queueParallelismMode=eBlockNoClientQueues，目前只有 Vulkan 支持）。"
                                + "关掉可对比延迟差别。",
                        DLSSConfig.FG_QUEUE_PARALLELISM))
                .add(bool("fg_retain_resources", "关闭时保留帧生成资源",
                        "关掉帧生成时不释放 DLSS-G 资源，避免下次开启时重建卡顿；代价是多占一些显存。",
                        DLSSConfig.FG_RETAIN_RESOURCES))
                .add(bool("fg_menu_detection", "全屏菜单检测",
                        "允许帧生成自行检测全屏菜单/暂停界面；关闭时它可能继续为静止画面生成插帧。",
                        DLSSConfig.FG_MENU_DETECTION))
                .add(bool("fg_ui_recomposition", "SDK UI 重合成（实验）",
                        "当前未提供独立 UI 透明度纹理，不满足 SDK 的完整输入要求，建议关闭。"
                                + "此开关不保证消除 HUD 或手部拖影；Oculus 不透明手部的相机运动修正独立生效。",
                        DLSSConfig.FG_UI_RECOMPOSITION))
                .build();

        OptionGroup superResolution = group("dlss_sr", "超分（SR）")
                .add(enumOption("sr_mode", "超分档位",
                        "超分会把世界渲染改成低分辨率再放大，与帧生成同时开启时真实帧率会更高。",
                        DLSSConfig.MODE, DLSSConfig.Quality.class, DLSSMC::refresh))
                .add(enumOption("sr_model_preset", "DLSS 模型预设",
                        "模型预设只作用于超分（SR），帧生成没有模型参数；超分档位为关闭时此项无效。"
                                + "自动：由 DLSS 按当前档位自选，通常是较新的 Transformer 模型。",
                        DLSSConfig.SR_MODEL_PRESET, DLSSConfig.SrPreset.class, DLSSMC::refresh))
                .build();

        OptionGroup diagnostics = group("dlss_debug", "调试")
                .add(bool("zero_motion_vectors", "运动向量置零",
                        "将输入运动向量置零但保留深度，用于对比运动补偿问题；会影响插帧质量，不用于正常游玩。",
                        DLSSConfig.ZERO_MOTION_VECTORS))
                .add(bool("reset_history_every_frame", "每帧重置历史",
                        "每帧请求重置历史，用于定位时序残影；会减少甚至停止生成帧，不用于正常游玩。",
                        DLSSConfig.RESET_HISTORY_EVERY_FRAME))
                .add(statusLogInterval())
                .add(bool("fg_show_only_interpolated", "只显示插帧",
                        "只把帧生成产出的那一帧上屏，真实帧不显示（eShowOnlyInterpolatedFrame）。"
                                + "用于确认插帧是否真在产出新画面，会明显影响观感和延迟，不用于正常游玩。",
                        DLSSConfig.FG_SHOW_ONLY_INTERPOLATED))
                .build();

        // 本机实测：加载的是 DLSS 5 世代的模型（文件版本 310.9.1，见「实机版本」一项）
        return page("dlss", "DLSS 5", generation, superResolution, diagnostics,
                versions(DLSSConfig.Backend.DLSS, "DLSS"));
    }

    // ------------------------------------------------------------------ FSR

    private static OptionPage fsrPage() {
        OptionGroup generation = group("fsr_generation", "常规")
                .add(enumOption("fsr_frame_generation", "FSR 帧生成档位",
                        "FSR 帧生成走 D3D12 呈现，分析式实现每张真实帧只生成 1 帧，没有 3x/4x。"
                                + "三个后端互斥：选非关闭会同时把 DLSS 与 XeSS 关掉。",
                        DLSSConfig.FSR_FRAME_GEN, DLSSConfig.FsrFrameGen.class,
                        () -> DLSSConfig.makeExclusive(DLSSConfig.Backend.FSR)))
                .build();

        OptionGroup tuning = group("fsr_tuning", "调优")
                .add(bool("fsr_fg_async", "异步计算（降负载）",
                        "让帧生成的生成工作跑异步计算队列（allowAsyncWorkloads，AMD 官方示例默认开）。"
                                + "可与其他 GPU 工作重叠、降低开销；个别驱动上可能更抖，关掉可对比。",
                        DLSSConfig.FSR_FG_ASYNC))
                .add(bool("fsr_fg_only_generated", "只显示插帧",
                        "只呈现 FSR 生成出来的插帧（onlyPresentGenerated），真实帧不上屏。"
                                + "用于确认插帧是否真的产出画面，会明显影响观感，不用于正常游玩。",
                        DLSSConfig.FSR_FG_ONLY_GENERATED))
                .build();

        OptionGroup superResolution = group("fsr_sr", "超分（SR）")
                .add(enumOption("fsr_sr_mode", "FSR 超分档位",
                        "仅在 FSR 帧生成后端运行时使用；实际渲染尺寸向 FSR 厂商接口查询。"
                                + "原版手部与 HUD 保持全分辨率，Oculus 手部可能作为世界的一部分参与超分。",
                        DLSSConfig.FSR_SR, DLSSConfig.FsrSr.class, DLSSMC::refresh))
                .add(bool("fsr_sr_sharpen", "RCAS 锐化",
                        "仅在 FSR 帧生成后端运行且 FSR 超分开启时使用 RCAS 锐化。",
                        DLSSConfig.FSR_SR_SHARPEN))
                .add(percent("fsr_sr_sharpness", "RCAS 锐化强度",
                        "仅在 FSR 帧生成后端运行、FSR 超分与 RCAS 锐化均开启时生效。"
                                + "越高越锐，也越容易放大噪点和锯齿。",
                        DLSSConfig.FSR_SR_SHARPNESS))
                .build();

        return page("fsr", "FSR", generation, tuning, superResolution,
                versions(DLSSConfig.Backend.FSR, "FSR"));
    }

    // ----------------------------------------------------------------- XeSS

    private static OptionPage xessPage() {
        OptionGroup generation = group("xess_generation", "常规")
                .add(enumOption("xess_frame_generation", "XeSS 帧生成档位",
                        "XeSS 帧生成走 D3D12 呈现并强制使用 XeLL，非 Intel 显卡的上限是 2x，"
                                + "选高了会按本机能力自动降档（见「实机版本」）。"
                                + "三个后端互斥：选非关闭会同时把 DLSS 与 FSR 关掉。",
                        DLSSConfig.XESS_FRAME_GEN, DLSSConfig.FrameGen.class,
                        () -> DLSSConfig.makeExclusive(DLSSConfig.Backend.XESS)))
                .build();

        OptionGroup tuning = group("xess_tuning", "调优")
                .add(percent("xess_scene_threshold", "场景变化检测",
                        "XeSS 判定场景突变并暂停插帧的阈值（SDK 默认 70%）。"
                                + "调高：切场景/开背包不糊但插帧更常停；调低：更坚持插帧更流畅，"
                                + "突变瞬间可能糊一下。",
                        DLSSConfig.XESS_SCENE_THRESHOLD, 70))
                .add(bool("xess_legacy_scale", "传统档位倍率",
                        "XeSS-SR 用传统倍率（平衡 1.7x/质量 1.5x 等）。关闭时 SDK 按本机 GPU 自选，"
                                + "本机有时更激进（如这版 SDK 平衡档建议 50% 分辨率）。改值后下次 SR 重建生效。",
                        DLSSConfig.XESS_LEGACY_SCALE))
                .add(xellFpsLimit())
                .build();

        OptionGroup superResolution = group("xess_sr", "超分（SR）")
                .add(enumOption("xess_sr_mode", "XeSS 超分档位",
                        "仅在 XeSS 帧生成后端运行时使用；实际渲染尺寸向 XeSS 厂商接口查询。"
                                + "原版手部与 HUD 保持全分辨率，Oculus 手部可能作为世界的一部分参与超分。",
                        DLSSConfig.XESS_SR, DLSSConfig.XessSr.class, DLSSMC::refresh))
                .build();

        return page("xess", "XeSS", generation, tuning, superResolution,
                versions(DLSSConfig.Backend.XESS, "XeSS"));
    }

    // -------------------------------------------------------------- 构造辅助

    private static OptionPage page(String id, String name, OptionGroup... groups) {
        // 显式给 id：Embeddium 的 OptionPage 无 id 时会去猜，猜不到就去查模组列表，
        // 在模组列表还没就绪的时机（构造期）会直接抛异常。自己起名最稳。
        return new OptionPage(OptionIdentifier.create(id("page_" + id)),
                Component.literal(name), ImmutableList.copyOf(groups));
    }

    private static OptionGroup.Builder group(String id, String name) {
        return OptionGroup.createBuilder().setId(id(id)).add(header(id, name));
    }

    /** Embeddium 的分组只画一个框，不画标题，所以用一条禁用的只读项当小标题。 */
    private static Option<?> header(String id, String name) {
        return OptionImpl.createBuilder(Integer.class, STORAGE)
                .setId(id("header_" + id))
                .setName(Component.literal(name))
                .setTooltip(Component.literal(name))
                .setControl(o -> new SliderControl(o, 0, 1, 1, v -> Component.literal("")))
                .setBinding((Void ignored, Integer v) -> { }, ignored -> 0)
                .setEnabled(false)
                .build();
    }

    private static OptionGroup versions(DLSSConfig.Backend backend, String label) {
        return OptionGroup.createBuilder()
                .add(readOnly(backend.name().toLowerCase(Locale.ROOT) + "_builtin_version",
                        label + " 内置版本",
                        "构建期打进 jar 的厂商文件版本，与 native/vendor/MANIFEST.sha256 对应。",
                        () -> FGRuntime.builtinVersion(backend)))
                .add(readOnly(backend.name().toLowerCase(Locale.ROOT) + "_runtime_version",
                        label + " 实机版本",
                        "运行时向厂商库查出来的版本；与内置版本不同说明库按本机能力降了级"
                                + "（例如 FSR 4.0.1 的包在非 RDNA4 显卡上只给 3.1.6）。",
                        () -> FGRuntime.get().runtimeVersion(backend)))
                .setId(id(backend.name().toLowerCase(Locale.ROOT) + "_versions"))
                .build();
    }

    /**
     * 只读展示项。Embeddium 没有「纯文本」控件，这里用一个禁用掉的整数滑块：
     * 禁用的选项不响应点击和拖动（ControlElement 里都先查 isAvailable），
     * 滑块上那行文字走取值格式化器 —— 它渲染时才求值，所以能显示运行期查到的版本。
     */
    private static OptionImpl<Void, Integer> readOnly(String path, String name, String tooltip,
                                                      java.util.function.Supplier<String> text) {
        return OptionImpl.createBuilder(Integer.class, STORAGE)
                .setId(id(path))
                .setName(Component.literal(name))
                .setTooltip(Component.literal(tooltip))
                .setControl(o -> new SliderControl(o, 0, 1, 1,
                        v -> Component.literal(text.get())))
                // 选项已禁用，binding 不会被写；给个恒 0 的读数即可
                .setBinding((Void ignored, Integer v) -> { }, ignored -> 0)
                .setEnabled(false)
                .build();
    }

    private static OptionImpl<Void, Boolean> bool(String path, String name, String tooltip,
                                                  ForgeConfigSpec.BooleanValue value,
                                                  OptionFlag... flags) {
        OptionImpl.Builder<Void, Boolean> b = OptionImpl.createBuilder(Boolean.class, STORAGE)
                .setId(id(path))
                .setName(Component.literal(name))
                .setTooltip(Component.literal(tooltip))
                .setBinding((Void ignored, Boolean v) -> value.set(v), ignored -> value.get())
                .setControl(TickBoxControl::new);
        if (flags.length > 0) b.setFlags(flags);
        return b.build();
    }

    private static OptionImpl<Void, Integer> integer(String path, String name, String tooltip,
                                                     ForgeConfigSpec.IntValue value,
                                                     int min, int max, int step,
                                                     ControlValueFormatter formatter) {
        return OptionImpl.createBuilder(Integer.class, STORAGE)
                .setId(id(path))
                .setName(Component.literal(name))
                .setTooltip(Component.literal(tooltip))
                .setBinding((Void ignored, Integer v) -> value.set(v), ignored -> value.get())
                .setControl(o -> new SliderControl(o, min, max, step, formatter))
                .build();
    }

    /** 0~1 的 double 配置项，界面按百分比整数显示 */
    private static OptionImpl<Void, Integer> percent(String path, String name, String tooltip,
                                                     ForgeConfigSpec.DoubleValue value) {
        return percent(path, name, tooltip, value, -1);
    }

    private static OptionImpl<Void, Integer> percent(String path, String name, String tooltip,
                                                     ForgeConfigSpec.DoubleValue value,
                                                     int defaultValue) {
        return OptionImpl.createBuilder(Integer.class, STORAGE)
                .setId(id(path))
                .setName(Component.literal(name))
                .setTooltip(Component.literal(tooltip))
                .setControl(o -> new SliderControl(o, 0, 100, 5,
                        v -> Component.literal(defaultValue >= 0 && v == defaultValue
                                ? v + "%（SDK 默认）" : v + "%")))
                .setBinding((Void ignored, Integer v) -> value.set(v / 100.0D),
                        ignored -> (int) Math.round(value.get() * 100.0D))
                .build();
    }

    private static <T extends Enum<T>> OptionImpl<Void, T> enumOption(String path, String name,
                                                                     String tooltip,
                                                                     ForgeConfigSpec.EnumValue<T> value,
                                                                     Class<T> type,
                                                                     Runnable afterChange) {
        T[] universe = type.getEnumConstants();
        Component[] names = new Component[universe.length];
        StringBuilder full = new StringBuilder(tooltip);
        for (int i = 0; i < universe.length; ++i) {
            names[i] = label(universe[i]);
            full.append("\n").append(names[i].getString()).append("：").append(desc(universe[i]));
        }
        return OptionImpl.createBuilder(type, STORAGE)
                .setId(id(path))
                .setName(Component.literal(name))
                .setTooltip(Component.literal(full.toString()))
                .setControl(o -> new CyclingControl<>(o, type, names))
                .setBinding((Void ignored, T v) -> {
                    value.set(v);
                    if (afterChange != null) afterChange.run();
                }, ignored -> value.get())
                .build();
    }

    private static Component label(Object enumConstant) {
        return Component.literal(read(enumConstant, "label"));
    }

    private static String desc(Object enumConstant) {
        return read(enumConstant, "desc");
    }

    /** 档位枚举都带 label/desc 两个 public 字段，这里反射取；取不到就退回枚举名。 */
    private static String read(Object enumConstant, String field) {
        try {
            Object v = enumConstant.getClass().getField(field).get(enumConstant);
            return v == null ? enumConstant.toString() : v.toString();
        } catch (ReflectiveOperationException e) {
            return enumConstant.toString();
        }
    }

    private static OptionImpl<Void, Integer> dlssgDynamicFps() {
        return integer("dlssg_dynamic_fps", "动态模式目标帧率",
                "动态模式希望达到的每秒帧数（插帧后）。0 表示由 DLSS-G 自动取显示器刷新率。"
                        + "调低会在真实帧率不足时插得更少、延迟更低；只在动态模式下生效。",
                DLSSConfig.DLSSG_DYNAMIC_FPS, 0, 1000, 5,
                v -> Component.literal(v == 0 ? "跟显示器刷新率" : v + " FPS"));
    }

    private static OptionImpl<Void, Integer> reflexFpsLimit() {
        return integer("reflex_fps_limit", "Reflex 限帧",
                "限制真实帧而非插帧后 FPS，仅在帧生成运行时生效；0 不额外限帧，游戏自身限帧仍有效。",
                DLSSConfig.REFLEX_FPS_LIMIT, 0, 1000, 5,
                v -> Component.literal(v == 0 ? "不额外限帧" : v + " FPS"));
    }

    private static OptionImpl<Void, Integer> xellFpsLimit() {
        return integer("xell_fps_limit", "XeLL 限帧",
                "XeLL 原生限帧，仅在 FSR / XeSS 后端运行有效。Reflex 限帧只控 DLSS-G；"
                        + "这两条路要限制真实帧必须调 XeLL 自己的接口。",
                DLSSConfig.XELL_FPS_LIMIT, 0, 1000, 5,
                v -> Component.literal(v == 0 ? "不额外限帧" : v + " FPS"));
    }

    private static OptionImpl<Void, Integer> statusLogInterval() {
        return integer("status_log_interval", "状态日志间隔",
                "控制 dlssmc_fg.log 的 FG 状态采样条数（按真实帧计）；状态变化和错误仍会记录，逐帧写盘会影响性能。",
                DLSSConfig.STATUS_LOG_INTERVAL, 0, 36000, 30,
                v -> Component.literal(v == 0 ? "仅状态变化" : "每 " + v + " 帧"));
    }

    private static OptionImpl<Void, DLSSConfig.FrameGen> frameGeneration() {
        return enumOption("frame_generation", "DLSS 帧生成档位",
                "需要硬件加速 GPU 调度已开启；官方限 RTX 50 才有多帧，本机上限不足时会自动降档。"
                        + "3x 以上要先开「解锁多帧生成」并重启游戏，否则会被夹回 2x。"
                        + "倍数是拿真实帧换呈现帧：N 倍下真实帧率上限 = 刷新率 / N，"
                        + "100Hz 屏上 6x 只有约 16.7，操作会明显变钝。"
                        + "三个后端互斥：选非关闭会同时把 FSR 与 XeSS 关掉。",
                DLSSConfig.FRAME_GEN, DLSSConfig.FrameGen.class,
                () -> DLSSConfig.makeExclusive(DLSSConfig.Backend.DLSS));
    }

    private static ResourceLocation id(String path) {
        return new ResourceLocation(DLSSMC.MOD_ID, path);
    }
}
