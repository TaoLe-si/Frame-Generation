package com.taolesi.dlssmc.mixin;

import net.irisshaders.iris.pipeline.IrisRenderingPipeline;
import net.irisshaders.iris.targets.RenderTargets;
import org.spongepowered.asm.mixin.Mixin;
import org.spongepowered.asm.mixin.gen.Accessor;

@Mixin(value = IrisRenderingPipeline.class, remap = false)
public interface IrisPipelineAccessor {
    @Accessor("renderTargets")
    RenderTargets dlssmc$getRenderTargets();

    @Accessor("colorSpaceConverter")
    net.irisshaders.iris.pathways.colorspace.ColorSpaceConverter dlssmc$getColorSpaceConverter();
}
