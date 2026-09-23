package com.taolesi.dlssmc.mixin;

import com.mojang.blaze3d.systems.RenderSystem;
import com.taolesi.dlssmc.core.FGRuntime;
import org.lwjgl.glfw.GLFW;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.injection.At;
import org.spongepowered.asm.mixin.injection.Redirect;

@Mixin(RenderSystem.class)
public class MixinWindow {

    // flipFrame 的事件轮询和渲染队列清理必须保留。
    @Redirect(method = "flipFrame", remap = false,
            at = @At(value = "INVOKE", target = "Lorg/lwjgl/glfw/GLFW;glfwSwapBuffers(J)V"))
    private static void dlssmc$swapBuffers(long window) {
        FGRuntime fg = FGRuntime.get();
        boolean takeover = fg.presentFrame();
        if (!takeover) {
            GLFW.glfwSwapBuffers(window);
            fg.afterGlSwap();
        }
        fg.recordPresentation(System.nanoTime(), takeover ? fg.getLastPresented() : 1, takeover);
    }
}
