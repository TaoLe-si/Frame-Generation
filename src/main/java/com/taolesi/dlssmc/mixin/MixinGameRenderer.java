package com.taolesi.dlssmc.mixin;

import net.minecraft.client.DeltaTracker;
import net.minecraft.client.renderer.GameRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * 世界阶段的开始与结束。
 *
 * 对照 GameRenderer.java（反编译）：
 *   1231  public void renderLevel(DeltaTracker)
 *   1277      levelRenderer.renderLevel(...)   <- 世界 + Iris final pass
 *   1281      renderItemInHand(...)            <- 手持物品，必须留在全分辨率
 */
@Mixin(GameRenderer.class)
public class MixinGameRenderer {

    private static final String LEVEL_RENDER =
            "Lnet/minecraft/client/renderer/LevelRenderer;renderLevel("
            + "Lnet/minecraft/client/DeltaTracker;"
            + "Z"
            + "Lnet/minecraft/client/Camera;"
            + "Lnet/minecraft/client/renderer/GameRenderer;"
            + "Lnet/minecraft/client/renderer/LightTexture;"
            + "Lorg/joml/Matrix4f;"
            + "Lorg/joml/Matrix4f;)V";

    @Inject(method = "render", at = @At("HEAD"))
    private void dlssmc$renderStart(DeltaTracker deltaTracker, boolean renderLevel, CallbackInfo ci) {
        com.taolesi.dlssmc.core.FGRuntime.get().renderStart();
    }

    @Inject(method = "renderLevel", at = @At("HEAD"))
    private void dlssmc$beginWorldPhase(DeltaTracker deltaTracker, CallbackInfo ci) {
        com.taolesi.dlssmc.core.FGRuntime.get().beginWorldPhase();
    }

    @Inject(method = "renderLevel",
            at = @At(value = "INVOKE", target = LEVEL_RENDER, shift = At.Shift.AFTER))
    private void dlssmc$endWorldPhase(DeltaTracker deltaTracker, CallbackInfo ci) {
        // 必须早于 renderItemInHand 前的清深度，包括 Iris 禁用原版手的情况。
        com.taolesi.dlssmc.core.FGRuntime.get().captureHudless();
    }
}
