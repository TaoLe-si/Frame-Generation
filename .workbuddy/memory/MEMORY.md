# 项目长期记忆：DLSS for Minecraft

## 目标
MC **1.21.1 / NeoForge** 模组：在不影响光影模组（Iris）的前提下接入 NVIDIA DLSS，并提供档位选择。

## 前置依赖（桃指定）
- **Sodium** `mc1.21.1-0.8.13-neoforge`：源码 `ref/sodium`（分支 `1.21.1/stable`），jar `libs/`
- **Iris** `1.8.14-beta.1+1.21.1-neoforge`：源码 `ref/iris`（分支 `multiloader-1.21`），jar `libs/`

## 版本基线
- MC `1.21.1`，NeoForge `21.1.228`，Parchment `2024.11.17`
- 构建插件 `net.neoforged.moddev` `2.0.141`，Gradle `9.4.1`（wrapper 从 ref/sodium 拷来）
- 编译 JDK 21：`D:\Java21`（gradle.properties 里 `org.gradle.java.home`）

## !! 需求修正（2026-09-17 桃指出）!!
**桃要的是 DLSS 帧生成（FG），不是超分（SR）。第一版按 SR 做的设计是错的。**
FG 的三条硬约束（SDK 2.14.1 已核实）：
1. **只支持 D3D12 / Vulkan**，`docs/ProgrammingGuideDLSS_G.md` 全文 0 次提到 D3D11 → 原生层的 D3D11 路线不合格
2. **必须接管 swapchain Present**："the plugin intercepts this present call with a Streamline proxy swap chain"；MC 用 GLFW `SwapBuffers`，Streamline 无法 hook
3. **输入变了**：Backbuffer(自动) + Depth + MotionVectors + **Hudless(画 HUD 前)** + **UI Alpha 或 UI Color&Alpha**；集成清单还要求 **Reflex 必须一起接**；需要开 HWS
→ 结论：必须自建 D3D12/Vulkan swapchain 接管上屏，并 hook 掉 GL 的 SwapBuffers

### 沿用的部分
- 世界阶段结束时的画面 = FG 要的 Hudless
- GUI 阶段可单独渲染到带 alpha 的 target = UI 层
- 两阶段架构天然能拆出这两份输入
- 深度/运动向量那一套照旧可用（SR 与 FG 对 depth/mvec 要求相同）

### 桥接路线验证结果（2026-09-17 实测，RTX 4070 Laptop / 610.74）
验证程序：`native/test/bridge_test.cpp`（独立程序，不依赖 MC）

| 路线 | 结论 | 证据 |
|---|---|---|
| A: WGL interop + D3D11On12 | **死** | `wglDXOpenDeviceNV` 直接拒绝 D3D11On12 设备 |
| B: GL_EXT_memory_object → Vulkan | **死** | NVIDIA GL 驱动 404 个扩展里没有 `GL_EXT_memory_object` / `GL_EXT_semaphore` |
| D: D3D11 → NT 共享句柄 → D3D12 | **死** | 见下方能力矩阵，WGL 只认 KMT，D3D12 只认 NT，互斥 |
| E: D3D11 KMT → Vulkan `D3D11_TEXTURE_KMT_BIT` | **✅ 通过** | `bridge_test_vk.cpp` 读回 (200,40,60,255)，与 GL 写入一致 |
| F: CUDA 中转（GL→CUDA→D3D12） | 不需要了 | E 已通过 |

**MiscFlags 能力矩阵（实测）**
| 组合 | 创建 | 注册 GL | KMT 句柄 | NT 句柄 |
|---|---|---|---|---|
| `SHARED` | ✅ | **✅** | ✅ | ❌ |
| `SHARED\|KEYEDMUTEX` | ❌ E_INVALIDARG | | | |
| `SHARED_NTHANDLE` | ❌ E_INVALIDARG | | | |
| `SHARED_NTHANDLE\|KEYEDMUTEX` | ✅ | ❌ | ❌ | ✅ |
| `SHARED\|NTHANDLE` | ✅ | ❌ | ❌ | ✅ |

**另一条已验证的好消息**：在**已经挂了 GL 上下文的 HWND** 上，D3D12 swapchain 能创建成功
→ "接管 MC 上屏"这条路是可行的。

### DLSS-G 环境验证结果（2026-09-17 实测，`native/test/fg_probe.cpp`）
本机环境**满足全部要求**：
- HWS（硬件加速 GPU 调度）`HwSchMode = 2` 已开启；Windows 10.0.19045（≥20H1）✓
- Streamline v2.14.1 Vulkan 后端初始化成功；`slIsFeatureSupported(kFeatureDLSS_G) = eOk` ✓
- 驱动 610.74，GPU 报 `Native VK OFA feature supported on this device!`（有光流加速器）

**必须记住的坑**：
- **`sl.dlss_g` 依赖 `sl.reflex`，`featuresToLoad` 里必须同时包含 `kFeatureReflex`**，
  否则日志会打 `Plugin 'sl.dlss_g' will be unloaded since it requires plugin 'sl.reflex'`，
  并且 `slIsFeatureSupported` 返回 `eErrorFeatureMissing`。建议连 `kFeaturePCL` 一起请求。
- `sl.interposer.dll` **导出全部 vk\* 入口点**（273 个符号，含 `vkCreateInstance`/`vkQueuePresentKHR`）
  → 链接 `sl.interposer.lib` 后直接调 `vk*` 即可，由 interposer 代理并 hook。这是 FG 拦截 Present 的正路。

**DLSS-G 的硬性技术要求（`slGetFeatureRequirements` 实测输出）**：
- flags = `0x1E` = `eD3D12Supported | eVulkanSupported | eVSyncOffRequired | eHardwareSchedulingRequired`
  （**没有 D3D11 位**，再次确认 FG 不支持 D3D11）
- 需要 **1 个图形队列 + 2 个计算队列**
- 需要设备扩展 7 个：`VK_NV_optical_flow`、`VK_KHR_external_semaphore`、`VK_KHR_timeline_semaphore`、
  `VK_KHR_maintenance4`、`VK_KHR_external_memory_win32`、`VK_KHR_external_memory`、
  `VK_KHR_external_semaphore_win32`
- 需要实例扩展 3 个：`VK_KHR_external_semaphore_capabilities`、
  `VK_KHR_get_physical_device_properties2`、`VK_KHR_external_memory_capabilities`

### 四个结构关卡全部通过（2026-09-17 实测）
| # | 关卡 | 验证程序 | 结果 |
|---|---|---|---|
| 1 | GL → D3D11(KMT) → Vulkan 内存导入 | `bridge_test_vk.cpp` | ✅ 像素一致 |
| 2 | Streamline Vulkan 后端初始化 | `fg_probe.cpp` | ✅ `slInit` eOk |
| 3 | DLSS-G 在本机受支持 | `fg_probe.cpp` | ✅ `slIsFeatureSupported` eOk |
| 4 | Vulkan swapchain + Present（GL 窗口上，经 SL 代理） | `fg_loop.cpp` | ✅ 60 帧全过 |

**又两个必须记住的坑**：
- **链接 `sl.interposer.lib` 时不要调 `slSetVulkanInfo`**！因为 `vkCreateInstance`/`vkCreateDevice`
  走的就是 SL 代理，插件已自动初始化，再调会返回 `eErrorInvalidState(19)`。
  官方文档原话：只有**不用** SL 的 vkCreateDevice/vkCreateInstance 代理时才需要调它。
- **`slShutdown()` 必须在销毁 Vulkan 对象之前调用**（swapchain/surface/device/instance 之后销毁），
  否则 Streamline 会抛异常并写 minidump。

### 实机接入踩到的坑（2026-09-17）
1. **D3D11 设备必须晚于 Vulkan 设备创建**。为互操作建的 D3D11 设备会被 Streamline 的 D3D11 钩子接住
   （日志：`d3d11.cpp[D3D11CreateDeviceAndSwapChain] Automatically assigning d3d11 device`），
   把它当成 D3D11 应用 → 之后建 Vulkan 时 `vkCreateDevice` 反复失败、`vkGetSwapchainImagesKHR` 返回 **0 张图**。
   顺序改成 slInit → Vulkan 实例/设备/swapchain → 再建 D3D11 + `wglDXOpenDeviceNV` 就正常了。
2. **`pref.flags` 必须带 `sl::PreferenceFlags::eUseFrameBasedResourceTagging`**，
   否则每帧 `slSetTagForFrame` 返回 19，SL 日志直说："flag is not set!"。
3. **初始化失败绝不能每帧重试**。第一版没有熔断，导致每帧重建整个 Vulkan+D3D11 设备，卡到没法玩。
   现在有熔断（3 次失败后停手 + 5 秒冷却）和 present 失败兜底（连续 5 次失败就交回 GL 上屏，避免黑屏）。
4. **每帧日志要走限流**。带 `fflush` 的磁盘 IO 在渲染线程上本身就是卡顿源（第一版每帧写 3 行）。
5. 同步要用 **3 帧并行的 fence/semaphore 环**，不能每帧 `vkWaitForFences(UINT64_MAX)` 等到底；
   present 前要 `slReflexSleep` 做帧率节流。

### 桥接打通（2026-09-17 关键突破）
**GL 只有在互操作对象【锁定】期间写入的内容，D3D11/Vulkan 才看得到。**
锁之前画的读回来全是 0 —— 独立测试和 MC 实机两次都验证了。
→ 所有写进互操作纹理的 GL 操作都必须夹在 `wglDXLockObjectsNV` / `wglDXUnlockObjectsNV` 之间。
→ 因此不要直接注册 MC 主 target 的颜色纹理，而是自己建普通纹理、在锁窗口内用
   `glCopyImageSubData` 把画面拷进去。
打通后回读：`final=(25,21,15,64) hudless=(182,209,255,0)`，是真实游戏画面。

### 当前唯一卡点：NGX PlatformError
- 实测 `NGX create feature failed 0xBAD00002`。
  **注意别查错表**：按 `external/ngx-sdk/include/nvsdk_ngx_defs.h`，
  `NVSDK_NGX_Result_Fail = 0xBAD00000`，`FAIL_PlatformError = Fail | 2`，
  所以 0xBAD00002 是 **PlatformError**，不是 OutOfRange。
- 线索：SL 日志一直打
  `Hook sl.common:Vulkan:CmdBindPipeline is NOT supported, plugin will not function properly`
  （CmdBindPipeline / CmdBindDescriptorSets / BeginCommandBuffer 都挂不上）→ 插件没法工作。
- **原因**：Vulkan 下**不应该静态链接 `sl.interposer.lib`**。官方 manual hooking 文档明确说：
  *"When using Vulkan linking the sl.interposer.lib would result in additional CPU overhead so the
  best approach is to dynamically load sl.interposer.dll instead of vulkan-1.dll and use
  vkGetDeviceProcAddr and vkGetInstanceProcAddr provided by the SL."*
  （静态链接是 DirectX 下的推荐做法，Vulkan 下正好相反。）
- **待做**：原生层改成动态加载 `sl.interposer.dll`，所有 Vulkan 入口点经它的
  `vkGetInstanceProcAddr` / `vkGetDeviceProcAddr` 获取。

### 路线 E 的实现要点（已实测）
- Vulkan 走**运行时动态加载** `vulkan-1.dll` + `vkGetInstanceProcAddr`，不需要装 Vulkan SDK / 导入库
- 头文件从 `KhronosGroup/Vulkan-Headers` 抓，放在 `ref/vkheaders`
- `VK_KHR_external_memory_win32` 是**设备级**扩展，塞进实例级会直接 `VK_ERROR_EXTENSION_NOT_PRESENT`(-7)
- `external_memory` 系列在 Vulkan 1.1 已提升为核心，建实例时 `apiVersion` 要给 `VK_API_VERSION_1_2`，
  启用扩展前先按实际可用列表过滤
- 关键流程：D3D11 `SHARED` 纹理 → `IDXGIResource::GetSharedHandle`(KMT) →
  `VkExternalMemoryImageCreateInfo{handleTypes=D3D11_TEXTURE_KMT_BIT}` →
  `vkGetMemoryWin32HandlePropertiesKHR` 取 memoryTypeBits →
  `VkImportMemoryWin32HandleInfoKHR` + `vkAllocateMemory` → `vkBindImageMemory`
- **坑：GL 必须在 `wglDXLockObjectsNV` 之后才渲染**，锁之前画的内容读回来是全 0
  （第一次跑就是这个原因失败，不是桥接本身的问题）

## 已确定的架构（第一版，SR 用；FG 需重构）
1. **两 target 方案**：主 target 保持全分辨率；只在世界渲染阶段把 `Minecraft.getMainRenderTarget()` 重定向到低分辨率 `worldTarget`（`new MainTarget(renderW, renderH)`，RGBA8 + DEPTH_COMPONENT32F）。
2. **降分辨率靠 Iris 自适应**：`IrisRenderingPipeline.java:922-944` 会用 `main.width/height` 自动重建内部 target，所以不需要碰光影任何逻辑。
3. **DLSS 输出直接写回主 target 的颜色纹理**（省掉一次全屏 blit）。
4. **深度 + 相机运动向量**：`DepthMotionPass` 在 GL 侧把 DEPTH_COMPONENT32F 转成 RGBA16F（R=深度，GB=运动向量），因为深度格式没法做 GL-D3D11 互操作。

## MC 1.21.1 关键源码位置（反编译，ref/mcsrc）
- `Minecraft.java:1191` 主 target bindWrite → `:1201` gameRenderer.render → `:1216` blitToScreen（唯一上屏点）
- `GameRenderer.java:1231` `renderLevel(DeltaTracker)`；`:1277` 调 `levelRenderer.renderLevel`；`:1281` 画手
- `GameRenderer.java:1040` 重新 bind 主 target；`:1079` GUI 渲染，**投影用 window 尺寸** → 主 target 一缩小 HUD 就错位
- `LevelRenderer.renderLevel(DeltaTracker, boolean, Camera, GameRenderer, LightTexture, Matrix4f modelView, Matrix4f projection)` — 矩阵是现成参数
- `RenderTarget.java:108/117` 颜色=GL_RGBA8，深度=DEPTH_COMPONENT32F（stencil 开时 DEPTH32F_STENCIL8）
- `com.mojang.blaze3d.pipeline.MainTarget` 有 public 构造 `MainTarget(int,int)`

## Iris 关键位置
- `FinalPassRenderer.renderFinalPass()` 写进 `main.getColorTextureId()`，靠 `iris$getColorBufferVersion()` 检测换 target → **换 target 不会崩**
- `MixinLevelRenderer.java:116-121`：final pass 在 `LevelRenderer.renderLevel` RETURN 前跑
- Iris 复用主帧缓冲的深度缓冲（FinalPassRenderer 注释 203-215）→ 深度拿得到

## 设置界面接入
用 **Sodium 官方第三方配置 API**，不做 mixin：
- `net.caffeinemc.mods.sodium.api.config.ConfigEntryPoint` + `@ConfigEntryPointForge("dlssmc")`
- 由 `ConfigLoaderForge.collectConfigEntryPoints()` 扫描 NeoForge 注解发现
- 文档：`ref/sodium/common/src/api/java/.../config/USAGE.md`
- 注意：Maven 上**没有** 1.21.1 的 `sodium-neoforge-api`，要把 Sodium jar 当 `compileOnly`；
  外层 jar 是 jarjar 包装，真正的类在 `META-INF/jarjar/*-mod.jar` 里（已解到 `libs/sodium-neoforge-0.8.13-mod.jar`）

## 原生层
- `native/src/dlssmc_native.cpp` + `native/build.sh`（cl.exe 直编，无 cmake）
- 顺序：`slInit` → `D3D11CreateDevice` → `slSetD3DDevice` → `wglDXOpenDeviceNV`
- 每个 GL 纹理配一个 `D3D11_RESOURCE_MISC_SHARED` 纹理，用 `wglDXRegisterObjectNV` 注册
- Java 加载时必须**先** `System.load(sl.interposer.dll)` 再 `System.load(dlssmc_native.dll)`

## 已知问题 / 待办
- `sl::Preferences::applicationId` 用的是占位值 231313132，正式发布要换成 NVIDIA 发放的 ID
- 还没实机验证：互操作是否成功、DLSS 实际出图、档位切换是否平滑
- `libs/*-mod.jar` 不能放进 `run/mods`（会重复加载 Sodium）
