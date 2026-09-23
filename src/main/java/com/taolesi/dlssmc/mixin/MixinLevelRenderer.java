package com.taolesi.dlssmc.mixin;

import com.mojang.blaze3d.vertex.PoseStack;
import net.minecraft.client.Camera;
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
 * LevelRenderer.renderLevel 的参数里就带着投影矩阵，相机矩阵要从 PoseStack 的栈顶拿
 * （1.20.1 的 renderLevel 签名是 PoseStack + partialTick + nanoTime，没有直接给 modelView）。
 * modelView 只含相机旋转，平移部分要自己补上 -camera.getPosition()（与 MC 内部做法一致）。
 */
@Mixin(LevelRenderer.class)
public class MixinLevelRenderer {

    @Inject(method = "renderLevel", at = @At("HEAD"))
    private void dlssmc$captureMatrices(
            PoseStack poseStack, float partialTick, long nanoTime, boolean renderBlockOutline,
            Camera camera, GameRenderer gameRenderer, LightTexture lightTexture,
            Matrix4f projection, CallbackInfo ci) {
        var renderer = (LevelRenderer) (Object) this;
        var main = net.minecraft.client.Minecraft.getInstance().getMainRenderTarget();
        var outline = renderer.entityTarget();
        var items = renderer.getItemEntityTarget();
        if ((outline != null && (outline.width != main.width || outline.height != main.height))
                || (items != null && (items.width != main.width || items.height != main.height))) {
            renderer.resize(main.width, main.height);
            main.bindWrite(true);
        }
        // 相机矩阵：PoseStack 栈顶就是本帧的 view 旋转（GameRenderer 在此之前已把
        // 投影与相机旋转都乘进去，投影部分由 projection 参数单独给出，所以这里取栈顶的
        // 旋转分量即可 —— 与 1.21.1 用 modelView 参数等价）。
        com.taolesi.dlssmc.core.FGRuntime.get()
                .captureMatrices(poseStack.last().pose(), projection, camera);
    }
}
