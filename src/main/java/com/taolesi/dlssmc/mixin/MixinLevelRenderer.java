package com.taolesi.dlssmc.mixin;

import net.minecraft.client.Camera;
import net.minecraft.client.DeltaTracker;
import net.minecraft.client.renderer.GameRenderer;
import net.minecraft.client.renderer.LevelRenderer;
import net.minecraft.client.renderer.LightTexture;
import org.joml.Matrix4f;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Inject;
import org.spongepowered.asm.mixin.injection.callback.CallbackInfo;

/**
 * 抓每帧的 modelView / projection。
 * LevelRenderer.renderLevel 的参数里就带着这两个矩阵，不用自己去 RenderSystem 里掏。
 * modelView 只含相机旋转，平移部分要自己补上 -camera.getPosition()（与 MC 内部做法一致）。
 */
@Mixin(LevelRenderer.class)
public class MixinLevelRenderer {

    @Inject(method = "renderLevel", at = @At("HEAD"))
    private void dlssmc$captureMatrices(
            DeltaTracker deltaTracker, boolean renderBlockOutline,
            Camera camera, GameRenderer gameRenderer, LightTexture lightTexture,
            Matrix4f modelView, Matrix4f projection, CallbackInfo ci) {
        var renderer = (LevelRenderer) (Object) this;
        var main = net.minecraft.client.Minecraft.getInstance().getMainRenderTarget();
        var outline = renderer.entityTarget();
        var items = renderer.getItemEntityTarget();
        if ((outline != null && (outline.width != main.width || outline.height != main.height))
                || (items != null && (items.width != main.width || items.height != main.height))) {
            renderer.resize(main.width, main.height);
            main.bindWrite(true);
        }
        com.taolesi.dlssmc.core.FGRuntime.get().captureMatrices(modelView, projection, camera);
    }
}
