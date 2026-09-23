package com.taolesi.dlssmc.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import net.minecraft.client.renderer.GameRenderer;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * 世界阶段的开始与结束。
 *
 * 对照 GameRenderer.java（1.20.1 反编译）：
 *   894   public void render(float partialTick, long nanoTime, boolean renderLevel)
 *   1080  public void renderLevel(float partialTick, long nanoTime, PoseStack poseStack)
 *   1126      this.minecraft.levelRenderer.renderLevel(poseStack, ...)  <- 世界 + Iris final pass
 *   1131      renderItemInHand(...)                                     <- 手持物品，留在全分辨率
 *
 * 1.20.1 没有 DeltaTracker，时间以 (partialTick, nanoTime) 两个参数传下来。
 */
@Mixin(GameRenderer.class)
public class MixinGameRenderer {

    private static final String LEVEL_RENDER =
            "Lnet/minecraft/client/renderer/LevelRenderer;renderLevel("
            + "Lcom/mojang/blaze3d/vertex/PoseStack;"
            + "F"
            + "J"
            + "Z"
            + "Lnet/minecraft/client/Camera;"
            + "Lnet/minecraft/client/renderer/GameRenderer;"
            + "Lnet/minecraft/client/renderer/LightTexture;"
            + "Lorg/joml/Matrix4f;)V";

    @Inject(method = "render", at = @At("HEAD"))
    private void dlssmc$renderStart(float partialTick, long nanoTime, boolean renderLevel,
                                    CallbackInfo ci) {
        com.taolesi.dlssmc.core.FGRuntime.get().renderStart();
    }

    @Inject(method = "renderLevel", at = @At("HEAD"))
    private void dlssmc$beginWorldPhase(float partialTick, long nanoTime, PoseStack poseStack,
                                        CallbackInfo ci) {
        com.taolesi.dlssmc.core.FGRuntime.get().beginWorldPhase();
    }

    @Inject(method = "renderLevel",
            at = @At(value = "INVOKE", target = LEVEL_RENDER, shift = At.Shift.AFTER))
    private void dlssmc$endWorldPhase(float partialTick, long nanoTime, PoseStack poseStack,
                                      CallbackInfo ci) {
        // 必须早于 renderItemInHand 前的清深度，包括 Iris 禁用原版手的情况。
        com.taolesi.dlssmc.core.FGRuntime.get().captureHudless();
    }
}
