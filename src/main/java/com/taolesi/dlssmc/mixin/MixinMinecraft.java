package com.taolesi.dlssmc.mixin;

import com.mojang.blaze3d.pipeline.RenderTarget;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.Redirect;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfoReturnable;

/**
 * 在世界渲染阶段把「主渲染目标」换成低分辨率的 worldTarget。
 *
 * 这里改的是 getMainRenderTarget() 的实现本身而不是各个调用点，
 * 所以 Iris 内部（它大量使用 Minecraft.getInstance().getMainRenderTarget()）
 * 会跟着一起降到低分辨率，光影管线整体缩放，不需要我们碰它任何逻辑。
 */
@Mixin(Minecraft.class)
public class MixinMinecraft {

    @Inject(method = "runTick", at = @At("HEAD"))
    private void dlssmc$beginFrame(boolean renderLevel, CallbackInfo ci) {
        com.taolesi.dlssmc.core.FGRuntime.get().beginFrame();
    }

    @Inject(method = "close", at = @At("HEAD"))
    private void dlssmc$shutdown(CallbackInfo ci) {
        com.taolesi.dlssmc.core.FGRuntime.get().shutdown();
    }

    /**
     * 接管上屏时跳过 MC 那次全屏 blit。
     *
     * runTick 里 `mainRenderTarget.blitToScreen(w, h)` 是把主目标整个 blit 到 GL 背缓冲。
     * 我们接管后画面来自自己那条 Vulkan swapchain（呈现窗口盖在 MC 窗口上），
     * 背缓冲永远不会显示 —— 这一 blit 是纯浪费。而且它排在交付拷贝之前，
     * 白占关键路径的 GPU 时间，等于把上屏往后推。
     *
     * 跳过判据在 FGRuntime.ownsPresent()：严格照抄 presentFrame 的前置条件，
     * 不成立时老实调原方法，所以菜单、预检期、交回 GL 之后都照常 blit。
     */
    @Redirect(method = "runTick",
            at = @At(value = "INVOKE",
                    target = "Lcom/mojang/blaze3d/pipeline/RenderTarget;blitToScreen(II)V"))
    private void dlssmc$skipRedundantBlit(RenderTarget target, int width, int height) {
        if (!com.taolesi.dlssmc.config.DLSSConfig.SKIP_REDUNDANT_BLIT.get()
                || !com.taolesi.dlssmc.core.FGRuntime.get().ownsPresent()) {
            target.blitToScreen(width, height);
        }
    }

    @Inject(method = "getMainRenderTarget", at = @At("HEAD"), cancellable = true)
    private void dlssmc$swapMainTarget(CallbackInfoReturnable<RenderTarget> cir) {
        RenderTarget swapped = com.taolesi.dlssmc.core.FGRuntime.get().getActiveMainTarget();
        if (swapped != null) {
            cir.setReturnValue(swapped);
        }
    }
}
