package com.taolesi.dlssmc;

import com.taolesi.dlssmc.config.DLSSConfig;
import net.minecraft.client.Minecraft;
import net.neoforged.bus.api.IEventBus;
import net.neoforged.fml.ModContainer;
import net.neoforged.fml.common.Mod;
import net.neoforged.neoforge.common.NeoForge;

import java.nio.file.Path;

/**
 * DLSS for Minecraft
 *
 * 目标：在不改动光影模组（Iris/Sodium）任何逻辑的前提下接入 NVIDIA DLSS，
 * 并提供档位选择。
 *
 * 核心思路：Iris 的整条光影管线尺寸都跟随 Minecraft.getMainRenderTarget()，
 * 所以只要在世界渲染阶段把这个 target 换成一个低分辨率的 worldTarget，
 * 光影就会整体按低分辨率跑；渲染完再用 DLSS 放大写回全分辨率主 target，
 * 手持物品和 HUD 则完全不受影响。
 */
@Mod(DLSSMC.MOD_ID)
public class DLSSMC {

    public static final String MOD_ID = "dlssmc";

    private static final Path NATIVES_DIR =
            Path.of(System.getProperty("java.io.tmpdir"), "dlssmc-natives");

    public DLSSMC(IEventBus modBus, ModContainer container) {
        container.registerConfig(net.neoforged.fml.config.ModConfig.Type.CLIENT, DLSSConfig.SPEC);

        NeoForge.EVENT_BUS.addListener(this::onLoggingIn);
        NeoForge.EVENT_BUS.addListener(this::onLoggingOut);
        NeoForge.EVENT_BUS.addListener(this::onRenderGui);
    }

    private void onRenderGui(net.neoforged.neoforge.client.event.RenderGuiEvent.Post event) {
        Minecraft mc = Minecraft.getInstance();
        if (!DLSSConfig.DEBUG_OVERLAY.get() || mc.level == null || mc.screen != null || mc.options.hideGui) return;
        var fg = com.taolesi.dlssmc.core.FGRuntime.get();
        var gui = event.getGuiGraphics();
        gui.drawString(mc.font, "FG/SR：" + (fg.isEnabled() ? fg.getStatus() : "关闭"), 6, 6, 0xFFFFFF);
        gui.drawString(mc.font, fg.getFpsText(), 6, 18, 0xFFFFFF);
        if (fg.isEnabled() && fg.isReady()) {
            gui.drawString(mc.font, "后端 " + DLSSConfig.activeBackendLabel()
                    + " 请求 " + DLSSConfig.activeTierLabel()
                    + " / 最近 SDK 计数 " + fg.getLastPresented() + " 帧", 6, 30, 0xFFFFFF);
        }
        String latency = fg.getLatencyText();
        if (latency != null) gui.drawString(mc.font, latency, 6, 42, 0xFFCC66);
        gui.drawString(mc.font, "配置：Boost=" + DLSSConfig.REFLEX_BOOST.get()
                + " 限帧=" + DLSSConfig.REFLEX_FPS_LIMIT.get()
                + " 队列并行=" + DLSSConfig.FG_QUEUE_PARALLELISM.get()
                + " MV置零=" + DLSSConfig.ZERO_MOTION_VECTORS.get()
                + " 重置=" + DLSSConfig.RESET_HISTORY_EVERY_FRAME.get(), 6, 54, 0xAAAAAA);
        if (DLSSConfig.DLSSG_UNLOCK_MFG.get() || DLSSConfig.FRAME_GEN.get().framesToGenerate > 1) {
            gui.drawString(mc.font, "多帧解锁：" + fg.getMfgUnlockText(), 6, 114, 0xFFAA55);
        }
        String bridge = fg.getBridgeText();
        if (bridge != null) gui.drawString(mc.font, bridge, 6, 66, 0x88CCFF);
        String stages = fg.getGpuStageText();
        if (stages != null) gui.drawString(mc.font, stages, 6, 78, 0x88CCFF);
        String model = fg.getModelText();
        if (model != null) gui.drawString(mc.font, model, 6, 90, 0x888888);
        if (DLSSConfig.activeSrQuality() >= 0) {
            gui.drawString(mc.font, fg.getSrText(), 6, 102, 0x88FF88);
        }
    }

    private void onLoggingIn(net.neoforged.neoforge.client.event.ClientPlayerNetworkEvent.LoggingIn event) {
        com.taolesi.dlssmc.core.FGRuntime.get().setWorldActive(true);
    }

    private void onLoggingOut(net.neoforged.neoforge.client.event.ClientPlayerNetworkEvent.LoggingOut event) {
        // SL 插件卸载会使进程内缓存的函数地址失效；设备只在客户端关闭时销毁。
        com.taolesi.dlssmc.core.FGRuntime.get().setWorldActive(false);
    }

    public static Path nativesDir() {
        return NATIVES_DIR;
    }

    /** 档位改变后调用，强制下一次渲染重建资源 */
    public static void refresh() {
        com.taolesi.dlssmc.core.FGRuntime.get().invalidate();
        com.taolesi.dlssmc.core.FGRuntime.get().resetFailureLatch();
    }
}
