import com.electronwill.nightconfig.core.CommentedConfig;
import com.taolesi.dlssmc.compat.SodiumConfigIntegration;
import com.taolesi.dlssmc.config.DLSSConfig;
import me.jellysquid.mods.sodium.client.gui.options.Option;
import me.jellysquid.mods.sodium.client.gui.options.OptionGroup;
import me.jellysquid.mods.sodium.client.gui.options.OptionPage;
import me.jellysquid.mods.sodium.client.gui.options.control.Control;
import me.jellysquid.mods.sodium.client.gui.options.control.ControlValueFormatter;
import me.jellysquid.mods.sodium.client.gui.options.control.CyclingControl;
import me.jellysquid.mods.sodium.client.gui.options.control.SliderControl;
import me.jellysquid.mods.sodium.client.gui.options.control.TickBoxControl;
import org.embeddedt.embeddium.api.OptionGUIConstructionEvent;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * 对着 Embeddium 0.3.31 的真实选项 API 跑一遍 SodiumConfigIntegration：
 * 注册监听、post 一次 GUI 构造事件、把收到的三页拆开逐项核对。
 *
 * <p>跑在生产 classpath（SRG 名）上，所以取 Component 文本要走反射 —— 编译期那份
 * Component 只有 SRG 方法名（m_7360_ 等）。
 *
 * <p>只读内存配置，不碰玩家存档里的配置文件。
 */
public class EmbeddiumPageCheck {

    static final Map<String, Option<?>> byId = new HashMap<>();
    static int failures = 0;

    static void check(boolean condition, String message) {
        if (!condition) {
            failures++;
            System.out.println("FAIL: " + message);
        }
    }

    /** Component 文本。生产 jar 里是 SRG 名，两种都试。 */
    static String text(Object component) {
        if (component == null) return null;
        for (String name : new String[]{"getString", "m_7360_"}) {
            try {
                Method m = component.getClass().getMethod(name);
                return (String) m.invoke(component);
            } catch (ReflectiveOperationException ignored) {
                // 换下一个名字
            }
        }
        return String.valueOf(component);
    }

    @SuppressWarnings("unchecked")
    static <T> T field(Object target, String name) {
        try {
            Field f = target.getClass().getDeclaredField(name);
            f.setAccessible(true);
            return (T) f.get(target);
        } catch (ReflectiveOperationException e) {
            throw new AssertionError("读不到字段 " + name + "（Embeddium 内部结构变了）", e);
        }
    }

    static String label(Option<?> o) {
        return text(o.getName());
    }

    /** OptionIdentifier 的 path，就是注册时给的 ResourceLocation 路径。 */
    static String idOf(Object holder) {
        Object id;
        try {
            id = holder.getClass().getMethod("getId").invoke(holder);
        } catch (ReflectiveOperationException e) {
            return null;
        }
        if (id == null) return null;
        try {
            return (String) id.getClass().getMethod("getPath").invoke(id);
        } catch (ReflectiveOperationException e) {
            return String.valueOf(id);
        }
    }

    public static void main(String[] args) {
        // 配置默认值装进内存配置：选项构造时会立刻读一次当前值
        CommentedConfig cfg = CommentedConfig.inMemory();
        DLSSConfig.SPEC.correct(cfg);
        DLSSConfig.SPEC.setConfig(cfg);

        SodiumConfigIntegration.register();

        List<OptionPage> pages = new ArrayList<>();
        // post 的返回值表示「有处理器取消了事件」，我们只是加页，false 是正常的
        OptionGUIConstructionEvent.BUS.post(new OptionGUIConstructionEvent(pages));
        check(pages.size() == 3, "应当往视频设置里加 3 页，实际 " + pages.size());

        Map<String, OptionPage> pageByName = new HashMap<>();
        for (OptionPage p : pages) pageByName.put(text(p.getName()), p);
        check(pageByName.containsKey("DLSS 5"), "缺 DLSS 页：" + pageByName.keySet());
        check(pageByName.containsKey("FSR"), "缺 FSR 页：" + pageByName.keySet());
        check(pageByName.containsKey("XeSS"), "缺 XeSS 页：" + pageByName.keySet());

        Set<String> ids = new HashSet<>();
        int optionCount = 0;
        for (OptionPage page : pages) {
            String pageName = text(page.getName());
            for (OptionGroup group : page.getGroups()) {
                check(idOf(group) != null, pageName + " 有分组没有 id");
                for (Option<?> option : group.getOptions()) {
                    optionCount++;
                    String id = idOf(option);
                    check(id != null, pageName + " 有选项没有 id：" + label(option));
                    if (id == null) continue;
                    check(ids.add(id), "选项 id 重复：" + id);
                    byId.put(id, option);
                    check(option.getName() != null, id + " 没有名字");
                    check(option.getTooltip() != null && !text(option.getTooltip()).isEmpty(),
                            id + " 没有 tooltip");
                    check(option.getControl() != null, id + " 没有控件");
                    check(option.getStorage() != null, id + " 没有 storage（Embeddium 会崩）");
                    // 档位说明是拼进 tooltip 的，至少得列出每个档位的名字
                    check(option.getFlags() != null, id + " 没有 flags 集合");
                }
            }
        }
        System.out.println("页面：" + pageByName.keySet() + "，共 " + optionCount + " 项");

        // 控件类型：枚举走 Cycling、开关走 TickBox、数值走 Slider
        expectControl("frame_generation", CyclingControl.class);
        expectControl("fsr_frame_generation", CyclingControl.class);
        expectControl("xess_frame_generation", CyclingControl.class);
        expectControl("sr_mode", CyclingControl.class);
        expectControl("sr_model_preset", CyclingControl.class);
        expectControl("fsr_sr_mode", CyclingControl.class);
        expectControl("xess_sr_mode", CyclingControl.class);
        expectControl("debug_overlay", TickBoxControl.class);
        expectControl("reflex_boost", TickBoxControl.class);
        expectControl("dlssg_unlock_mfg", TickBoxControl.class);
        expectControl("reflex_fps_limit", SliderControl.class);
        expectControl("xell_fps_limit", SliderControl.class);
        expectControl("status_log_interval", SliderControl.class);
        expectControl("fsr_sr_sharpness", SliderControl.class);
        expectControl("xess_scene_threshold", SliderControl.class);

        // 滑块范围要和配置语义对上，不然界面会把值卡在错误区间
        expectRange("reflex_fps_limit", 0, 1000, 5);
        expectRange("xell_fps_limit", 0, 1000, 5);
        expectRange("status_log_interval", 0, 36000, 30);
        expectRange("fsr_sr_sharpness", 0, 100, 5);
        expectRange("xess_scene_threshold", 0, 100, 5);

        // 整数档位格式化
        check(text(formatter("reflex_fps_limit").format(0)).equals("不额外限帧"),
                "Reflex 限帧 0 应当显示「不额外限帧」");
        check(text(formatter("reflex_fps_limit").format(120)).equals("120 FPS"),
                "Reflex 限帧标签不对：" + text(formatter("reflex_fps_limit").format(120)));
        check(text(formatter("status_log_interval").format(0)).equals("仅状态变化"),
                "状态日志间隔 0 应当显示「仅状态变化」");
        check(text(formatter("status_log_interval").format(600)).equals("每 600 帧"),
                "状态日志间隔标签不对：" + text(formatter("status_log_interval").format(600)));
        check(text(formatter("xess_scene_threshold").format(70)).contains("SDK 默认"),
                "场景变化检测 70 应当标出 SDK 默认");

        // 只读的版本项：必须不可用，且文本来自运行期查询
        for (String id : new String[]{"dlss_builtin_version", "dlss_runtime_version",
                "fsr_builtin_version", "fsr_runtime_version",
                "xess_builtin_version", "xess_runtime_version"}) {
            Option<?> o = byId.get(id);
            check(o != null, "缺只读版本项：" + id);
            if (o == null) continue;
            check(!o.isAvailable(), id + " 应当是不可用的只读项");
            String shown = text(formatter(id).format(0));
            check(shown != null && !shown.isEmpty(), id + " 没显示出内容");
            if (id.equals("dlss_builtin_version")) {
                check(shown.contains("Streamline"), "DLSS 内置版本要说清来源，got " + shown);
            }
            if (id.endsWith("_runtime_version")) {
                check(shown.contains("未运行"), id + " 没在跑时不能拿内置值冒充，got " + shown);
            }
        }

        // 档位互斥：界面上把 DLSS 选起来，另外两个后端必须被写回关闭
        apply("frame_generation", DLSSConfig.FrameGen.X2);
        check(DLSSConfig.FRAME_GEN.get() == DLSSConfig.FrameGen.X2, "DLSS 档位没写进配置");
        check(DLSSConfig.XESS_FRAME_GEN.get() == DLSSConfig.FrameGen.OFF,
                "选 DLSS 之后 XeSS 必须回到关闭");
        check(DLSSConfig.FSR_FRAME_GEN.get() == DLSSConfig.FsrFrameGen.OFF,
                "选 DLSS 之后 FSR 必须回到关闭");
        check(DLSSConfig.activeBackend() == DLSSConfig.Backend.DLSS, "后端应当是 DLSS");
        check(DLSSConfig.activeFramesToGenerate() == 1, "DLSS 2x 就是每张真实帧再生成 1 帧");

        apply("xess_frame_generation", DLSSConfig.FrameGen.X4);
        check(DLSSConfig.XESS_FRAME_GEN.get() == DLSSConfig.FrameGen.X4, "XeSS 档位没写进配置");
        check(DLSSConfig.FRAME_GEN.get() == DLSSConfig.FrameGen.OFF, "选 XeSS 之后 DLSS 必须回到关闭");
        check(DLSSConfig.activeBackend() == DLSSConfig.Backend.XESS, "后端应当是 XeSS");
        check(DLSSConfig.activeFramesToGenerate() == 3, "XeSS 4x 就是每张真实帧再生成 3 帧");

        apply("fsr_frame_generation", DLSSConfig.FsrFrameGen.X2);
        check(DLSSConfig.FSR_FRAME_GEN.get() == DLSSConfig.FsrFrameGen.X2, "FSR 档位没写进配置");
        check(DLSSConfig.XESS_FRAME_GEN.get() == DLSSConfig.FrameGen.OFF, "选 FSR 之后 XeSS 必须回到关闭");
        check(DLSSConfig.activeBackend() == DLSSConfig.Backend.FSR, "后端应当是 FSR");

        // 百分比项存的是 0~1 的小数，界面给的是整数
        apply("fsr_sr_sharpness", 35);
        check(Double.compare(DLSSConfig.FSR_SR_SHARPNESS.get(), 0.35D) == 0,
                "锐化强度 35% 应当存成 0.35，实际 " + DLSSConfig.FSR_SR_SHARPNESS.get());
        check(read("fsr_sr_sharpness").equals(35), "锐化强度回读不是 35");
        apply("xess_scene_threshold", 55);
        check(Double.compare(DLSSConfig.XESS_SCENE_THRESHOLD.get(), 0.55D) == 0,
                "场景阈值 55% 应当存成 0.55，实际 " + DLSSConfig.XESS_SCENE_THRESHOLD.get());

        // 布尔项
        apply("debug_overlay", true);
        check(DLSSConfig.DEBUG_OVERLAY.get(), "状态叠加层开关没写进配置");
        apply("debug_overlay", false);
        check(!DLSSConfig.DEBUG_OVERLAY.get(), "状态叠加层关不掉");

        // 枚举项：档位文字必须和枚举自带的 label 一致（界面靠它显示）
        check(text(((CyclingControl<?>) byId.get("sr_mode").getControl()).getNames()[0]) != null,
                "SR 档位控件没有名字数组");
        // 超分档位取的是「当前后端」的值，先把后端切回 DLSS，否则读到的是 FSR 的档位
        apply("frame_generation", DLSSConfig.FrameGen.X2);
        check(DLSSConfig.activeBackend() == DLSSConfig.Backend.DLSS, "后端应当是 DLSS");
        apply("sr_mode", DLSSConfig.Quality.DLAA);
        check(DLSSConfig.MODE.get() == DLSSConfig.Quality.DLAA, "SR 档位没写进配置");
        check(DLSSConfig.activeSrQuality() == DLSSConfig.Quality.DLAA.slValue,
                "SR 档位没传到原生层取值，实际 " + DLSSConfig.activeSrQuality());
        apply("sr_mode", DLSSConfig.Quality.OFF);
        check(!DLSSConfig.isEnabled(), "SR 关掉后不该还启用");

        apply("sr_model_preset", DLSSConfig.SrPreset.K);
        check(DLSSConfig.SR_MODEL_PRESET.get() == DLSSConfig.SrPreset.K, "模型预设没写进配置");

        // 每页的档位页签数量对得上枚举（漏了档位界面上就选不到）
        expectCycleCount("frame_generation", DLSSConfig.FrameGen.values().length);
        expectCycleCount("sr_mode", DLSSConfig.Quality.values().length);
        expectCycleCount("sr_model_preset", DLSSConfig.SrPreset.values().length);
        expectCycleCount("fsr_sr_mode", DLSSConfig.FsrSr.values().length);
        expectCycleCount("xess_sr_mode", DLSSConfig.XessSr.values().length);
        expectCycleCount("fsr_frame_generation", DLSSConfig.FsrFrameGen.values().length);

        if (failures == 0) {
            System.out.println("PASS: Embeddium 三页 " + optionCount + " 项全部注册，"
                    + "控件类型 / 滑块范围 / 只读版本项 / 三后端互斥 / 百分比与枚举往返验证");
        } else {
            System.out.println("FAILED: " + failures + " 项不通过");
            System.exit(1);
        }
    }

    static void expectControl(String id, Class<?> type) {
        Option<?> o = byId.get(id);
        check(o != null, "缺选项：" + id);
        if (o == null) return;
        check(type.isInstance(o.getControl()),
                id + " 的控件类型应当是 " + type.getSimpleName() + "，实际 "
                        + (o.getControl() == null ? "null" : o.getControl().getClass().getSimpleName()));
    }

    static void expectRange(String id, int min, int max, int step) {
        Option<?> o = byId.get(id);
        if (o == null) return;
        Control<?> c = o.getControl();
        check(c instanceof SliderControl, id + " 不是滑块");
        if (!(c instanceof SliderControl)) return;
        check(field(c, "min").equals(min), id + " 下限应当是 " + min + "，实际 " + field(c, "min"));
        check(field(c, "max").equals(max), id + " 上限应当是 " + max + "，实际 " + field(c, "max"));
        check(field(c, "interval").equals(step), id + " 步长应当是 " + step + "，实际 " + field(c, "interval"));
    }

    static void expectCycleCount(String id, int count) {
        Option<?> o = byId.get(id);
        if (o == null) return;
        Control<?> c = o.getControl();
        check(c instanceof CyclingControl, id + " 不是循环档位控件");
        if (!(c instanceof CyclingControl)) return;
        check(((CyclingControl<?>) c).getNames().length == count,
                id + " 的档位数量应当是 " + count + "，实际 " + ((CyclingControl<?>) c).getNames().length);
    }

    static ControlValueFormatter formatter(String id) {
        Option<?> o = byId.get(id);
        if (o == null) throw new AssertionError("缺选项：" + id);
        Control<?> c = o.getControl();
        if (c instanceof SliderControl) return field(c, "mode");
        throw new AssertionError(id + " 没有取值格式化器（控件是 " + c.getClass().getSimpleName() + "）");
    }

    /**
     * 走界面上「改档位 → 点应用」的同一条路：setValue 只改待应用值，
     * 真正落进配置的是 applyChanges（SodiumOptionsGUI 在点应用时对每个改过的选项调它）。
     */
    @SuppressWarnings("unchecked")
    static <T> void apply(String id, T value) {
        Option<T> o = (Option<T>) byId.get(id);
        o.setValue(value);
        o.applyChanges();
    }

    @SuppressWarnings("unchecked")
    static <T> T read(String id) {
        return (T) byId.get(id).getValue();
    }
}
