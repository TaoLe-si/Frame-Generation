// DLSS 帧生成骨架：Vulkan swapchain + Present 经 Streamline 代理
//
// 这是原生层的雏形，验证最后一道结构关卡：
//   1. 在「已挂 GL 上下文」的窗口上建 Vulkan swapchain（模拟 MC 的窗口）
//   2. 按 slGetFeatureRequirements 的要求创建设备（队列 / 扩展）
//   3. slSetVulkanInfo 把设备信息交给 Streamline
//   4. 循环 acquire -> clear -> vkQueuePresentKHR，确认 Present 被 interposer 接管
//
// 通过后，剩下的就是把 MC 的画面经 KMT 句柄导入 Vulkan 并打 tag 交给 DLSS-G。
//
// 编译：native/test/build_loop.sh

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <sl.h>
#include <sl_core_api.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_helpers_vk.h>

#include <cstdio>
#include <cstring>
#include <vector>

#define LOG(...)             \
    do {                     \
        printf(__VA_ARGS__); \
        fflush(stdout);      \
    } while (0)

#define VK_CHECK(x, msg)                                                    \
    do {                                                                    \
        VkResult _r = (x);                                                  \
        if (_r != VK_SUCCESS) {                                             \
            LOG("  [FAIL] %s (VkResult=%d)\n", msg, (int)_r);               \
            return 1;                                                       \
        }                                                                   \
        LOG("  [ok] %s\n", msg);                                            \
    } while (0)

static void logCallback(sl::LogType type, const char* msg) {
    if (type == sl::LogType::eError || type == sl::LogType::eWarn) {
        LOG("    [SL/%s] %s\n", type == sl::LogType::eError ? "ERR" : "WRN", msg);
    }
}

static HWND g_hwnd = nullptr;
static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) {
        DestroyWindow(h);
        g_hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static HWND makeWindow(HINSTANCE hInst) {
    const wchar_t* cls = L"DlssmcFgLoopCls";
    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, cls, L"DLSS-G skeleton", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) {
        LOG("[FAIL] 创建窗口失败\n");
        exit(1);
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return hwnd;
}

int main() {
    LOG("=== DLSS-G 骨架：Vulkan swapchain + Present 代理 ===\n\n");

    const uint32_t kWidth = 1280, kHeight = 720;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    g_hwnd = makeWindow(hInst);

    // ---- 0. 给窗口挂一个 GL 上下文，模拟 Minecraft 的环境 ----
    {
        HDC hdc = GetDC(g_hwnd);
        PIXELFORMATDESCRIPTOR pfd{};
        pfd.nSize = sizeof(pfd);
        pfd.nVersion = 1;
        pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
        pfd.iPixelType = PFD_TYPE_RGBA;
        pfd.cColorBits = 32;
        int pf = ChoosePixelFormat(hdc, &pfd);
        if (pf && SetPixelFormat(hdc, pf, &pfd)) {
            HGLRC ctx = wglCreateContext(hdc);
            if (ctx && wglMakeCurrent(hdc, ctx)) {
                LOG("  [ok] 窗口已挂 GL 上下文（模拟 MC）\n");
            }
        }
    }

    // ---- 1. slInit（必须在任何 Vulkan 调用之前）----
    LOG("\n  --- slInit ---\n");
    {
        sl::Preferences pref{};
        pref.showConsole = false;
        pref.renderAPI = sl::RenderAPI::eVulkan;
        pref.applicationId = 231313132u;
        pref.engine = sl::EngineType::eCustom;
        pref.engineVersion = "0.1.0";
        pref.logMessageCallback = logCallback;

        const wchar_t* paths[1] = {L"D:\\Backup\\Downloads\\streamline-sdk-v2.14.1\\bin\\x64"};
        pref.pathsToPlugins = paths;
        pref.numPathsToPlugins = 1;

        sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
        pref.featuresToLoad = features;
        pref.numFeaturesToLoad = 3;

        auto r = slInit(pref, sl::kSDKVersion);
        LOG("  %s slInit (%d)\n", r == sl::Result::eOk ? "[ok]" : "[FAIL]", (int)r);
        if (r != sl::Result::eOk) return 1;
    }

    // ---- 2. 实例 + Win32 surface ----
    LOG("\n  --- Vulkan 实例 / surface ---\n");
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "dlssmc-fg";
    appInfo.apiVersion = VK_API_VERSION_1_2;

    const char* instExts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
    };
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &appInfo;
    ci.enabledExtensionCount = 5;
    ci.ppEnabledExtensionNames = instExts;

    VkInstance inst = VK_NULL_HANDLE;
    VK_CHECK(vkCreateInstance(&ci, nullptr, &inst), "vkCreateInstance");

    VkWin32SurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance = hInst;
    sci.hwnd = g_hwnd;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VK_CHECK(vkCreateWin32SurfaceKHR(inst, &sci, nullptr, &surface), "vkCreateWin32SurfaceKHR");

    // ---- 3. 物理设备 + 队列族 ----
    LOG("\n  --- 物理设备 ---\n");
    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(inst, &gpuCount, nullptr);
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(inst, &gpuCount, gpus.data());
    VkPhysicalDevice phys = gpus[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys, &props);
    LOG("  [info] GPU: %s\n", props.deviceName);

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfs.data());

    // DLSS-G 要 1 图形 + 2 计算队列
    int gfxFamily = -1, ofaFamily = -1;
    for (uint32_t i = 0; i < qfCount; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surface, &present);
        if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present && gfxFamily < 0) {
            gfxFamily = (int)i;
        }
    }
    if (gfxFamily < 0) {
        LOG("  [FAIL] 找不到既能渲染又能 present 的队列族\n");
        return 1;
    }
    LOG("  [ok] 图形+呈现队列族 = %d（该族共 %u 个队列）\n", gfxFamily,
        qfs[gfxFamily].queueCount);

    // ---- 4. 设备：按 DLSS-G 要求开扩展，队列数要够 ----
    LOG("\n  --- 逻辑设备 ---\n");
    uint32_t devExtCount = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &devExtCount, nullptr);
    std::vector<VkExtensionProperties> devExts(devExtCount);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &devExtCount, devExts.data());
    auto hasDevExt = [&](const char* n) {
        for (auto& e : devExts)
            if (strcmp(e.extensionName, n) == 0) return true;
        return false;
    };

    const char* wantDev[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_4_EXTENSION_NAME,
        VK_NV_OPTICAL_FLOW_EXTENSION_NAME,
    };
    std::vector<const char*> devExtList;
    for (auto n : wantDev) {
        if (hasDevExt(n)) {
            devExtList.push_back(n);
        } else {
            LOG("  [warn] 设备不支持扩展 %s\n", n);
        }
    }

    // 图形队列族里要拿 3 个队列（1 图形 + 2 计算）
    uint32_t wantQueues = qfs[gfxFamily].queueCount >= 3 ? 3 : qfs[gfxFamily].queueCount;
    float prio[3] = {1.0f, 1.0f, 1.0f};
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = (uint32_t)gfxFamily;
    qci.queueCount = wantQueues;
    qci.pQueuePriorities = prio;

    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.pNext = nullptr;
    f13.maintenance4 = VK_TRUE;
    f12.pNext = &f13;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f12;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)devExtList.size();
    dci.ppEnabledExtensionNames = devExtList.data();

    VkDevice dev = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDevice(phys, &dci, nullptr, &dev), "vkCreateDevice");

    VkQueue gfxQueue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, (uint32_t)gfxFamily, 0, &gfxQueue);
    LOG("  [ok] 图形队列已获取（族 %d 上共要了 %u 个队列）\n", gfxFamily, wantQueues);

    // ---- 5. 不需要 slSetVulkanInfo ----
    // 我们是链接 sl.interposer.lib 的，vkCreateInstance / vkCreateDevice 走的都是
    // Streamline 的代理，插件已在设备创建时自动初始化。此时再调 slSetVulkanInfo
    // 会返回 eErrorInvalidState(19)。
    // 官方文档也写了：只有【不使用】SL 的 vkCreateDevice/vkCreateInstance 代理时才需要调它。
    LOG("\n  --- 设备已由 Streamline 代理接管（无需 slSetVulkanInfo）---\n");

    // ---- 6. swapchain ----
    LOG("\n  --- swapchain ---\n");
    VkSurfaceCapabilitiesKHR caps{};
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps), "查询 surface 能力");
    LOG("  [info] surface: minImageCount=%u 当前 %ux%u\n", caps.minImageCount, caps.currentExtent.width,
        caps.currentExtent.height);

    VkSurfaceFormatKHR wantFmt{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

    VkSwapchainCreateInfoKHR scci{};
    scci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    scci.surface = surface;
    scci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && scci.minImageCount > caps.maxImageCount)
        scci.minImageCount = caps.maxImageCount;
    scci.imageFormat = wantFmt.format;
    scci.imageColorSpace = wantFmt.colorSpace;
    scci.imageExtent = caps.currentExtent.width == UINT32_MAX
                           ? VkExtent2D{kWidth, kHeight}
                           : caps.currentExtent;
    scci.imageArrayLayers = 1;
    scci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    scci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    scci.preTransform = caps.currentTransform;
    scci.compositeAlpha = alpha;
    scci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR; // DLSS-G 要求关垂直同步
    scci.clipped = VK_TRUE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSwapchainKHR(dev, &scci, nullptr, &swapchain), "vkCreateSwapchainKHR");

    uint32_t imgCount = 0;
    vkGetSwapchainImagesKHR(dev, swapchain, &imgCount, nullptr);
    std::vector<VkImage> images(imgCount);
    vkGetSwapchainImagesKHR(dev, swapchain, &imgCount, images.data());
    LOG("  [ok] swapchain 有 %u 张图，尺寸 %ux%u\n", imgCount, scci.imageExtent.width,
        scci.imageExtent.height);

    // ---- 7. 渲染循环：clear + present（Present 走 interposer） ----
    LOG("\n  --- 渲染循环（60 帧，Present 经 Streamline 代理）---\n");
    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = (uint32_t)gfxFamily;
    VkCommandPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateCommandPool(dev, &cpi, nullptr, &pool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(dev, &cbai, &cmd);

    VkSemaphoreCreateInfo semi{};
    semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkSemaphore imageAvail = VK_NULL_HANDLE, renderDone = VK_NULL_HANDLE;
    vkCreateSemaphore(dev, &semi, nullptr, &imageAvail);
    vkCreateSemaphore(dev, &semi, nullptr, &renderDone);

    // 用 slGetNewFrameToken 拿到帧号，Present 时 SL 要靠它对齐
    sl::FrameToken* frameToken = nullptr;
    uint32_t frameIndex = 0;

    int presented = 0;
    for (int f = 0; f < 60; ++f) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_hwnd) break;

        uint32_t imageIndex = 0;
        VkResult ar = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, imageAvail, VK_NULL_HANDLE,
                                            &imageIndex);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) continue;
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
            LOG("  [FAIL] vkAcquireNextImageKHR -> %d\n", (int)ar);
            break;
        }

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);

        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = images[imageIndex];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);

        VkClearColorValue color{};
        color.float32[0] = 0.15f + 0.5f * (f / 60.0f);
        color.float32[1] = 0.25f;
        color.float32[2] = 0.45f;
        color.float32[3] = 1.0f;
        VkImageSubresourceRange rng{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, images[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color,
                             1, &rng);

        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

        vkEndCommandBuffer(cmd);

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &imageAvail;
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        si.pWaitDstStageMask = &waitStage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &renderDone;
        vkQueueSubmit(gfxQueue, 1, &si, VK_NULL_HANDLE);

        // 给 Streamline 一个帧号（FG 会用它对齐呈现的帧）
        slGetNewFrameToken(frameToken, &frameIndex);
        frameIndex++;

        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &renderDone;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &imageIndex;
        VkResult pr = vkQueuePresentKHR(gfxQueue, &pi);
        if (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR) {
            ++presented;
        } else {
            LOG("  [FAIL] vkQueuePresentKHR -> %d\n", (int)pr);
            break;
        }
        Sleep(16);
    }

    LOG("\n  [ok] 成功 present %d 帧\n", presented);

    vkDeviceWaitIdle(dev);
    LOG("\n=========================================\n");
    LOG("  %s\n", presented > 0 ? "通过：Vulkan swapchain + Present 在 GL 窗口上可用"
                                 : "不通过");
    LOG("=========================================\n");

    // 文档要求：slShutdown() 必须在销毁 Vulkan 对象【之前】调用
    slShutdown();

    vkDestroySwapchainKHR(dev, swapchain, nullptr);
    vkDestroySurfaceKHR(inst, surface, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    return presented > 0 ? 0 : 2;
}
