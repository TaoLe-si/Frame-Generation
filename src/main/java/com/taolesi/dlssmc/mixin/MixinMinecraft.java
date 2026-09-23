package com.taolesi.dlssmc.mixin;

import com.mojang.blaze3d.pipeline.RenderTarget;
import net.minecraft.client.Minecraft;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
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

    @Inject(method = "getMainRenderTarget", at = @At("HEAD"), cancellable = true)
    private void dlssmc$swapMainTarget(CallbackInfoReturnable<RenderTarget> cir) {
        RenderTarget swapped = com.taolesi.dlssmc.core.FGRuntime.get().getActiveMainTarget();
        if (swapped != null) {
            cir.setReturnValue(swapped);
        }
    }
}
