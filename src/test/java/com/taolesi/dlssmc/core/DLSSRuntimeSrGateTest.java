package com.taolesi.dlssmc.core;

import org.junit.jupiter.api.Test;

import java.nio.charset.StandardCharsets;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

class DLSSRuntimeSrGateTest {

    @Test
    void worldHooksDoNotReferenceTheSeparateD3d11Backend() throws Exception {
        for (String name : new String[]{"MixinGameRenderer", "MixinLevelRenderer", "MixinMinecraft"}) {
            try (var in = getClass().getResourceAsStream("/com/taolesi/dlssmc/mixin/" + name + ".class")) {
                assertNotNull(in);
                String constants = new String(in.readAllBytes(), StandardCharsets.ISO_8859_1);
                assertTrue(constants.contains("com/taolesi/dlssmc/core/FGRuntime"), name);
                assertFalse(constants.contains("com/taolesi/dlssmc/core/DLSSRuntime"),
                        name + " must not reference a second Streamline backend");
            }
        }
    }
}
