import com.taolesi.dlssmc.compat.SodiumConfigIntegration;
import com.taolesi.dlssmc.config.DLSSConfig;
import net.caffeinemc.mods.sodium.api.config.option.Range;
import net.caffeinemc.mods.sodium.api.config.structure.BooleanOptionBuilder;
import net.caffeinemc.mods.sodium.api.config.structure.ConfigBuilder;
import net.caffeinemc.mods.sodium.api.config.structure.IntegerOptionBuilder;
import net.caffeinemc.mods.sodium.api.config.structure.ModOptionsBuilder;
import net.caffeinemc.mods.sodium.api.config.structure.OptionPageBuilder;
import net.minecraft.network.chat.Component;
import net.minecraft.resources.ResourceLocation;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Function;
import java.util.function.Supplier;

/**
 * 用动态代理假造 Sodium 的 ConfigBuilder，记录 SodiumConfigIntegration 注册了什么，
 * 检查钠视频设置界面里的页面 / 分组 / 选项，以及 id、默认值、取值范围是否和 DLSSConfig 对齐；
 * 再把注册期捕获的绑定真的调用一次，验三个后端的档位确实互斥。
 * 只走注册期调用 + 内存配置，绝不写玩家的配置文件。
 */
public class SodiumOptionsCheck {
    // 每个代理收到的调用
    static final Map<Object, List<Object[]>> calls = new IdentityHashMap<>();
    // 每个代理是哪个 create*/register* 调用产生的（含参数）
    static final Map<Object, Object[]> createdBy = new IdentityHashMap<>();

    static void check(boolean condition, String message) {
        if (!condition) throw new AssertionError(message);
    }

    static Object proxy(Class<?> type) {
        return Proxy.newProxyInstance(SodiumOptionsCheck.class.getClassLoader(), new Class<?>[]{type},
                (InvocationHandler) (self, method, args) -> {
                    switch (method.getName()) {
                        case "toString": return "proxy:" + type.getSimpleName();
                        case "hashCode": return System.identityHashCode(self);
                        case "equals": return self == args[0];
                    }
                    calls.computeIfAbsent(self, k -> new ArrayList<>()).add(new Object[]{method.getName(), args});
                    Class<?> result = method.getReturnType();
                    if (!result.isInterface()) return null;
                    if (result.isAssignableFrom(type)) return self; // 链式调用返回自身
                    Object child = proxy(result);
                    createdBy.put(child, new Object[]{method.getName(), args});
                    return child;
                });
    }

    static List<Object[]> of(Object self) {
        return calls.getOrDefault(self, List.of());
    }

    static int count(Object self, String methodName) {
        return (int) of(self).stream().filter(c -> c[0].equals(methodName)).count();
    }

    static Object first(Object self, String methodName, int index) {
        for (Object[] call : of(self)) {
            if (call[0].equals(methodName)) return ((Object[]) call[1])[index];
        }
        throw new AssertionError("no " + methodName + " on " + self);
    }

    static String name(Object self) {
        return ((Component) first(self, "setName", 0)).getString();
    }

    static List<Object> passedTo(Object self, String methodName) {
        List<Object> out = new ArrayList<>();
        for (Object[] call : of(self)) {
            if (call[0].equals(methodName)) out.add(((Object[]) call[1])[0]);
        }
        return out;
    }

    static Object created(ConfigBuilder builder, String methodName) {
        for (Map.Entry<Object, Object[]> entry : createdBy.entrySet()) {
            if (entry.getValue()[0].equals(methodName)) return entry.getKey();
        }
        throw new AssertionError("nothing created by " + methodName);
    }

    static Object[] argsOf(Object created) {
        return (Object[]) createdBy.get(created)[1];
    }

    static String path(Object option) {
        return ((ResourceLocation) argsOf(option)[0]).getPath();
    }

    // Sodium 的 OptionBuilderImpl / StatefulOptionBuilderImpl.validateData 会逐条强制这些字段，
    // 缺任何一条都会在游戏启动阶段直接崩溃（storage handler 缺失已经崩过一次）。
    static void expectOption(String id, Object option, Object defaultValue) {
        check(count(option, "setBinding") == 1, id + " must bind exactly once");
        check(count(option, "setStorageHandler") == 1, id + " must set a storage handler");
        Object tooltip = first(option, "setTooltip", 0);
        if (tooltip instanceof Component component) {
            check(!component.getString().isBlank(), id + " tooltip must not be blank");
        }
        check(name(option).length() > 1, id + " needs a name");
        check(first(option, "setDefaultValue", 0).equals(defaultValue), id + " default mismatch");
    }

    static void expectRange(String id, Object option, int specMax, int defaultValue) {
        Range range = (Range) first(option, "setRange", 0);
        check(range.min() == 0, id + " min must stay 0");
        check(range.max() == specMax, id + " max " + range.max() + " != config spec " + specMax);
        check(range.step() > 0 && (defaultValue - range.min()) % range.step() == 0,
                id + " default " + defaultValue + " not reachable with step " + range.step());
        check(count(option, "setValueFormatter") == 1, id + " needs a value formatter");
    }

    /** 把注册期捕获的 setter 真的调一次，验互斥是不是真写进了配置 */
    static void applyTier(Object option, Object value) throws Exception {
        Object setter = first(option, "setBinding", 0);
        // 绑定是别的包里的 lambda，直接拿它的类反射会被模块挡；走接口的 accept 方法
        for (Class<?> iface : setter.getClass().getInterfaces()) {
            for (Method m : iface.getMethods()) {
                if (m.getName().equals("accept") && m.getParameterCount() == 1) {
                    m.invoke(setter, value);
                    return;
                }
            }
        }
        throw new AssertionError("setBinding 的 setter 没有 accept(...)");
    }

    static void loadInMemoryConfig() throws Exception {
        var config = com.electronwill.nightconfig.core.CommentedConfig.inMemory();
        DLSSConfig.SPEC.correct(config);
        var constructor = Class.forName("net.neoforged.fml.config.LoadedConfig").getDeclaredConstructor(
                com.electronwill.nightconfig.core.CommentedConfig.class, Path.class,
                net.neoforged.fml.config.ModConfig.class);
        constructor.setAccessible(true);
        DLSSConfig.SPEC.acceptConfig((net.neoforged.fml.config.IConfigSpec.ILoadedConfig)
                constructor.newInstance(config, null, null));
    }

    @SuppressWarnings("unchecked")
    public static void main(String[] args) throws Exception {
        loadInMemoryConfig();
        ConfigBuilder builder = (ConfigBuilder) proxy(ConfigBuilder.class);
        new SodiumConfigIntegration().registerConfigLate(builder);

        check(count(builder, "createExternalButtonOption") == 0, "must not offer a separate settings screen");
        ModOptionsBuilder mod = (ModOptionsBuilder) created(builder, "registerOwnModOptions");
        check(count(mod, "setColorTheme") == 1, "must set an explicit colour theme instead of the hashed preset");
        List<Object> pages = passedTo(mod, "addPage");
        check(pages.size() == 3, "expected DLSS/FSR/XeSS pages, got " + pages.size());
        check(name(pages.get(0)).equals("DLSS 5"), "first page must be DLSS 5, got " + name(pages.get(0)));
        check(name(pages.get(1)).equals("FSR"), "second page must be FSR, got " + name(pages.get(1)));
        check(name(pages.get(2)).equals("XeSS"), "third page must be XeSS, got " + name(pages.get(2)));
        String section = (String) first(mod, "setName", 0);
        check(section.equals("Frame Generation"), "mod section must be titled Frame Generation, got " + section);
        for (Object entry : pages) check(!section.equals(name(entry)), "section and page must not share a title");

        List<Object> groups = passedTo(pages.get(0), "addOptionGroup");
        check(groups.size() == 4, "expected 4 groups on the DLSS page, got " + groups.size());
        List<Object> options = new ArrayList<>();
        for (Object group : groups) {
            check(name(group).length() > 1, "group without a name");
            options.addAll(passedTo(group, "addOption"));
        }
        check(groups.stream().anyMatch(g -> name(g).equals("常规")), "basic group missing");
        check(groups.stream().anyMatch(g -> name(g).contains("超分")), "super-resolution group missing");
        check(groups.stream().anyMatch(g -> name(g).contains("调试")), "diagnostics group missing");
        check(groups.stream().anyMatch(g -> name(g).equals("版本")), "version group missing");
        check(options.size() == 17, "expected 17 options on the DLSS page, got " + options.size());

        Map<String, Object> byId = new java.util.LinkedHashMap<>();
        for (Object option : options) byId.put(path(option), option);
        check(byId.size() == options.size(), "duplicate option ids: " + options.size() + " vs " + byId.size());

        Object frameGen = byId.get("frame_generation");
        expectOption("frame_generation", frameGen, DLSSConfig.FrameGen.OFF);
        check(argsOf(frameGen)[1] == DLSSConfig.FrameGen.class, "frame gen must use the FrameGen enum");
        expectOption("dlssg_unlock_mfg", byId.get("dlssg_unlock_mfg"), DLSSConfig.DLSSG_UNLOCK_MFG.getDefault());
        expectOption("debug_overlay", byId.get("debug_overlay"), DLSSConfig.DEBUG_OVERLAY.getDefault());
        expectOption("reflex_boost", byId.get("reflex_boost"), DLSSConfig.REFLEX_BOOST.getDefault());
        expectOption("sr_mode", byId.get("sr_mode"), DLSSConfig.MODE.getDefault());
        expectOption("sr_model_preset", byId.get("sr_model_preset"), DLSSConfig.SR_MODEL_PRESET.getDefault());
        for (String enumPath : List.of("frame_generation", "sr_mode", "sr_model_preset")) {
            check(count(byId.get(enumPath), "setElementNameProvider") == 1, enumPath + " needs its own labels");
        }
        expectOption("zero_motion_vectors", byId.get("zero_motion_vectors"),
                DLSSConfig.ZERO_MOTION_VECTORS.getDefault());
        expectOption("reset_history_every_frame", byId.get("reset_history_every_frame"),
                DLSSConfig.RESET_HISTORY_EVERY_FRAME.getDefault());
        expectOption("reflex_fps_limit", byId.get("reflex_fps_limit"), DLSSConfig.REFLEX_FPS_LIMIT.getDefault());
        expectOption("status_log_interval", byId.get("status_log_interval"),
                DLSSConfig.STATUS_LOG_INTERVAL.getDefault());
        expectOption("fg_queue_parallelism", byId.get("fg_queue_parallelism"),
                DLSSConfig.FG_QUEUE_PARALLELISM.getDefault());
        expectOption("fg_retain_resources", byId.get("fg_retain_resources"),
                DLSSConfig.FG_RETAIN_RESOURCES.getDefault());
        expectOption("fg_menu_detection", byId.get("fg_menu_detection"),
                DLSSConfig.FG_MENU_DETECTION.getDefault());
        expectOption("fg_show_only_interpolated", byId.get("fg_show_only_interpolated"),
                DLSSConfig.FG_SHOW_ONLY_INTERPOLATED.getDefault());
        expectOption("fg_ui_recomposition", byId.get("fg_ui_recomposition"),
                DLSSConfig.FG_UI_RECOMPOSITION.getDefault());
        expectRange("reflex_fps_limit", byId.get("reflex_fps_limit"), 1000,
                DLSSConfig.REFLEX_FPS_LIMIT.getDefault());
        expectRange("status_log_interval", byId.get("status_log_interval"), 36000,
                DLSSConfig.STATUS_LOG_INTERVAL.getDefault());
        check(!byId.containsKey("details"), "the entry that opened our own screen must be gone");

        // ---- 三个后端的档位与版本项 ----
        Map<String, Object> fsr = new java.util.LinkedHashMap<>();
        for (Object group : passedTo(pages.get(1), "addOptionGroup"))
            for (Object option : passedTo(group, "addOption")) fsr.put(path(option), option);
        check(fsr.size() == 8, "FSR page must hold 8 options including async and generated-only, got " + fsr.size());
        expectOption("fsr_fg_async", fsr.get("fsr_fg_async"), DLSSConfig.FSR_FG_ASYNC.getDefault());
        expectOption("fsr_fg_only_generated", fsr.get("fsr_fg_only_generated"), DLSSConfig.FSR_FG_ONLY_GENERATED.getDefault());
        check(!fsr.containsKey("fsr_enabled"), "the old disabled FSR placeholder must be gone");
        Object fsrTier = fsr.get("fsr_frame_generation");
        expectOption("fsr_frame_generation", fsrTier, DLSSConfig.FsrFrameGen.OFF);
        check(argsOf(fsrTier)[1] == DLSSConfig.FsrFrameGen.class,
                "FSR must offer its own enum: analytical FSR3 has no 3x/4x");
        Object fsrSr = fsr.get("fsr_sr_mode");
        expectOption("fsr_sr_mode", fsrSr, DLSSConfig.FSR_SR.getDefault());
        check(argsOf(fsrSr)[1] == DLSSConfig.FsrSr.class, "FSR SR must use the FsrSr enum");
        check(name(fsrSr).equals("FSR 超分档位"), "FSR SR name mismatch");
        check(count(fsrSr, "setElementNameProvider") == 1, "fsr_sr_mode needs its own labels");
        Function<DLSSConfig.FsrSr, Component> fsrSrLabels =
                (Function<DLSSConfig.FsrSr, Component>) first(fsrSr, "setElementNameProvider", 0);
        Object fsrSharpen = fsr.get("fsr_sr_sharpen");
        expectOption("fsr_sr_sharpen", fsrSharpen, DLSSConfig.FSR_SR_SHARPEN.getDefault());
        check(fsrSharpen instanceof BooleanOptionBuilder, "FSR sharpening must be a boolean option");
        check(name(fsrSharpen).equals("RCAS 锐化"), "FSR sharpening name mismatch");
        Object fsrSharpness = fsr.get("fsr_sr_sharpness");
        int sharpnessDefault = (int) Math.round(DLSSConfig.FSR_SR_SHARPNESS.getDefault() * 100.0D);
        expectOption("fsr_sr_sharpness", fsrSharpness, sharpnessDefault);
        check(fsrSharpness instanceof IntegerOptionBuilder, "FSR sharpness must be an integer option");
        check(name(fsrSharpness).equals("RCAS 锐化强度"), "FSR sharpness name mismatch");
        expectRange("fsr_sr_sharpness", fsrSharpness, 100, sharpnessDefault);
        check(((Range) first(fsrSharpness, "setRange", 0)).step() == 5, "FSR sharpness step must be 5");
        Supplier<?> sharpnessGetter = (Supplier<?>) first(fsrSharpness, "setBinding", 1);
        check(sharpnessGetter.get().equals(sharpnessDefault), "FSR sharpness getter default mismatch");

        List<Object[]> fsrSrCalls = calls.get(fsrSr);
        Object[] storageCall = fsrSrCalls.stream().filter(c -> c[0].equals("setStorageHandler"))
                .findFirst().orElseThrow();
        int storageIndex = fsrSrCalls.indexOf(storageCall);
        fsrSrCalls.remove(storageIndex);
        boolean missingStorageRejected = false;
        try {
            expectOption("fsr_sr_mode", fsrSr, DLSSConfig.FSR_SR.getDefault());
        } catch (AssertionError expected) {
            missingStorageRejected = "fsr_sr_mode must set a storage handler".equals(expected.getMessage());
        } finally {
            fsrSrCalls.add(storageIndex, storageCall);
        }
        check(missingStorageRejected, "expectOption must reject a new SR option without its storage handler");
        expectOption("fsr_sr_mode", fsrSr, DLSSConfig.FSR_SR.getDefault());

        Map<String, Object> xess = new java.util.LinkedHashMap<>();
        for (Object group : passedTo(pages.get(2), "addOptionGroup"))
            for (Object option : passedTo(group, "addOption")) xess.put(path(option), option);
        check(xess.size() == 7, "XeSS page must hold 7 options including scene threshold, scale and XeLL limit, got " + xess.size());
        expectOption("xess_legacy_scale", xess.get("xess_legacy_scale"), DLSSConfig.XESS_LEGACY_SCALE.getDefault());
        expectOption("xess_scene_threshold", xess.get("xess_scene_threshold"), 70);
        expectRange("xess_scene_threshold", xess.get("xess_scene_threshold"), 100, 70);
        expectOption("xell_fps_limit", xess.get("xell_fps_limit"), DLSSConfig.XELL_FPS_LIMIT.getDefault());
        expectRange("xell_fps_limit", xess.get("xell_fps_limit"), 1000, DLSSConfig.XELL_FPS_LIMIT.getDefault());
        Object xessTier = xess.get("xess_frame_generation");
        expectOption("xess_frame_generation", xessTier, DLSSConfig.FrameGen.OFF);
        Object xessSr = xess.get("xess_sr_mode");
        expectOption("xess_sr_mode", xessSr, DLSSConfig.XESS_SR.getDefault());
        check(argsOf(xessSr)[1] == DLSSConfig.XessSr.class, "XeSS SR must use the XessSr enum");
        check(name(xessSr).equals("XeSS 超分档位"), "XeSS SR name mismatch");
        check(count(xessSr, "setElementNameProvider") == 1, "xess_sr_mode needs its own labels");
        Function<DLSSConfig.XessSr, Component> xessSrLabels =
                (Function<DLSSConfig.XessSr, Component>) first(xessSr, "setElementNameProvider", 0);

        Map<String, Object> versionOptions = new java.util.LinkedHashMap<>();
        versionOptions.put("dlss_builtin_version", byId.get("dlss_builtin_version"));
        versionOptions.put("dlss_runtime_version", byId.get("dlss_runtime_version"));
        versionOptions.put("fsr_builtin_version", fsr.get("fsr_builtin_version"));
        versionOptions.put("fsr_runtime_version", fsr.get("fsr_runtime_version"));
        versionOptions.put("xess_builtin_version", xess.get("xess_builtin_version"));
        versionOptions.put("xess_runtime_version", xess.get("xess_runtime_version"));
        check(versionOptions.values().stream().allMatch(java.util.Objects::nonNull),
                "缺版本项：DLSS=" + byId.keySet() + " FSR=" + fsr.keySet() + " XeSS=" + xess.keySet());
        for (Map.Entry<String, Object> entry : versionOptions.entrySet()) {
            Object option = entry.getValue();
            expectOption(entry.getKey(), option, 0);
            check(count(option, "setEnabled") == 1, entry.getKey() + " must be read-only");
            check(first(option, "setEnabled", 0).equals(false), entry.getKey() + " must stay disabled");
            check(count(option, "setControlHiddenWhenDisabled") == 1,
                    entry.getKey() + " must keep its control visible when disabled");
            check(first(option, "setControlHiddenWhenDisabled", 0).equals(false),
                    entry.getKey() + " control must not be hidden");
            expectRange(entry.getKey(), option, 1, 0);
            check(entry.getKey().endsWith("_version"), "version ids must stay named *_version");
        }

        // ---- 互斥：调用注册期捕获的绑定，看另外两个是不是真被关掉 ----
        check(DLSSConfig.activeBackend() == null, "nothing should run by default");
        applyTier(fsrTier, DLSSConfig.FsrFrameGen.X2);
        check(DLSSConfig.activeBackend() == DLSSConfig.Backend.FSR, "FSR 档位应当把后端切成 FSR");
        check(DLSSConfig.FRAME_GEN.get() == DLSSConfig.FrameGen.OFF
                        && DLSSConfig.XESS_FRAME_GEN.get() == DLSSConfig.FrameGen.OFF,
                "选 FSR 之后 DLSS 与 XeSS 必须都是关闭");
        check(DLSSConfig.activeFramesToGenerate() == 1, "FSR 每张真实帧生成 1 帧");
        for (DLSSConfig.FsrSr mode : DLSSConfig.FsrSr.values()) {
            check(fsrSrLabels.apply(mode).getString().equals(mode.label), "FSR SR label mismatch: " + mode);
            applyTier(fsrSr, mode);
            check(DLSSConfig.FSR_SR.get() == mode, "FSR SR setter did not store " + mode);
            check(DLSSConfig.activeBackend() == DLSSConfig.Backend.FSR, "FSR SR must not change the backend");
            check(DLSSConfig.activeSrQuality() == mode.nativeValue, "FSR SR native quality mismatch: " + mode);
            for (boolean sharpen : new boolean[]{false, true}) {
                applyTier(fsrSharpen, sharpen);
                check(DLSSConfig.FSR_SR_SHARPEN.get() == sharpen, "FSR sharpening setter mismatch");
                check(DLSSConfig.activeSrSharpen() == sharpen, "FSR active sharpening mismatch: " + mode);
            }
        }
        for (int percent : new int[]{0, 35, 100}) {
            applyTier(fsrSharpness, percent);
            double fraction = percent / 100.0D;
            check(Double.compare(DLSSConfig.FSR_SR_SHARPNESS.get(), fraction) == 0,
                    "FSR sharpness must store " + fraction + " for UI value " + percent);
            check(Float.compare(DLSSConfig.activeSrSharpness(), (float) fraction) == 0,
                    "FSR active sharpness mismatch: " + fraction);
            check(sharpnessGetter.get().equals(percent), "FSR sharpness UI round-trip mismatch: " + percent);
            String formatted = ((net.caffeinemc.mods.sodium.api.config.option.ControlValueFormatter)
                    first(fsrSharpness, "setValueFormatter", 0)).format(percent).getString();
            check(formatted.equals(percent + "%"), "FSR sharpness percentage label mismatch: " + formatted);
        }

        applyTier(xessTier, DLSSConfig.FrameGen.X4);
        check(DLSSConfig.activeBackend() == DLSSConfig.Backend.XESS, "XeSS 档位应当把后端切成 XeSS");
        check(DLSSConfig.FSR_FRAME_GEN.get() == DLSSConfig.FsrFrameGen.OFF,
                "选 XeSS 之后 FSR 必须回到关闭");
        check(DLSSConfig.activeFramesToGenerate() == 3, "XeSS 4x 就是每张真实帧再生成 3 帧");
        for (DLSSConfig.XessSr mode : DLSSConfig.XessSr.values()) {
            check(xessSrLabels.apply(mode).getString().equals(mode.label), "XeSS SR label mismatch: " + mode);
            applyTier(xessSr, mode);
            check(DLSSConfig.XESS_SR.get() == mode, "XeSS SR setter did not store " + mode);
            check(DLSSConfig.activeBackend() == DLSSConfig.Backend.XESS, "XeSS SR must not change the backend");
            check(DLSSConfig.activeSrQuality() == mode.nativeValue, "XeSS SR native quality mismatch: " + mode);
            check(DLSSConfig.FSR_SR_SHARPEN.get() && DLSSConfig.FSR_SR_SHARPNESS.get() == 1.0D,
                    "FSR sharpening knobs must remain set while testing XeSS isolation");
            check(!DLSSConfig.activeSrSharpen(), "FSR sharpening must never leak into XeSS: " + mode);
            check(DLSSConfig.activeSrSharpness() == 0f, "FSR sharpness must never leak into XeSS: " + mode);
        }

        applyTier(frameGen, DLSSConfig.FrameGen.X2);
        check(DLSSConfig.activeBackend() == DLSSConfig.Backend.DLSS, "DLSS 档位应当把后端切成 DLSS");
        check(DLSSConfig.XESS_FRAME_GEN.get() == DLSSConfig.FrameGen.OFF,
                "选 DLSS 之后 XeSS 必须回到关闭");

        applyTier(fsrSr, DLSSConfig.FsrSr.OFF);
        applyTier(xessSr, DLSSConfig.XessSr.OFF);
        check(DLSSConfig.FSR_SR.get() == DLSSConfig.FsrSr.OFF
                        && DLSSConfig.XESS_SR.get() == DLSSConfig.XessSr.OFF,
                "FSR and XeSS SR must be restored to OFF before disabling frame generation");
        applyTier(frameGen, DLSSConfig.FrameGen.OFF);
        check(DLSSConfig.activeBackend() == null, "全部关掉后不该还有后端在跑");

        // 版本文本：内置与实机必须来自不同来源，实机没运行时要说明而不是抄内置
        String builtin = ((net.caffeinemc.mods.sodium.api.config.option.ControlValueFormatter)
                first(byId.get("dlss_builtin_version"), "setValueFormatter", 0)).format(0).getString();
        String runtime = ((net.caffeinemc.mods.sodium.api.config.option.ControlValueFormatter)
                first(byId.get("dlss_runtime_version"), "setValueFormatter", 0)).format(0).getString();
        check(builtin.contains("Streamline"), "DLSS 内置版本要说清来源，got " + builtin);
        check(runtime.contains("未运行"), "没在跑时实机版本不能拿内置值冒充，got " + runtime);

        System.out.println("PASS: Frame Generation 段 3 页（DLSS 5 共 17 项 / FSR 8 项 / XeSS 7 项）、"
                + "SR 默认值/类型/标签/范围与 storage handler（含缺失必拒绝）验证、"
                + "三后端档位互斥与 FSR/XeSS 全部 SR 档位（含 OFF）经真实绑定验证、"
                + "FSR 锐化开关与强度 0/0.35/1 往返及 XeSS 隔离验证、"
                + "内置与实机版本分开显示（内存配置 + mock Sodium builder）");
    }
}
