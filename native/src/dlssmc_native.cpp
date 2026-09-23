// DLSS for Minecraft - 超分通道（Vulkan 后端）
//
// 职责：
//   1. 初始化 NVIDIA Streamline（与帧生成那条共用一个实例，eVulkan，特性全集）
//   2. 走 VK_KHR_external_memory_win32 把 OpenGL 纹理经 D3D11 KMT 导入 Vulkan
//   3. 每帧在 Vulkan 命令缓冲上跑 DLSS-SR，输出到 outTex，再 blit 到 MC 主 target
//
// 旧版走 D3D11 调度，DLSS-SR 的 GPU 工作被 D3D11 串着，OpenGL ↔ Vulkan 互操作
// 又多一趟，叠加到 FG 上就比 FSR/XeSS 那条慢不少。改成 Vulkan 后，DLSS-SR 的
// GPU 工作全部在 Vulkan 上，跨设备只剩 KMT 句柄翻译这一步。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan.h>
#define VK_ENABLE_WIN32_CUSTOM_EXTENSIONS 1
#include <vulkan_win32.h>

#include <d3d11.h>
#include <d3d11_4.h>   // ID3D11Device5/Context4/Fence
#include <dxgi.h>
#include <wrl/client.h>

#include <jni.h>

#include <sl.h>
#include <sl_core_api.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
#include <sl_matrix_helpers.h>

#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// 常量
// ---------------------------------------------------------------------------

#define DLSSMC_APP_ID 231313132u
#define DLSSMC_GL_TEXTURE_2D 0x0DE1
#define WGL_ACCESS_READ_WRITE_NV 0x0001
#define WGL_ACCESS_READ_ONLY_NV  0x0002

// ---------------------------------------------------------------------------
// WGL_NV_DX_interop2 动态加载（仅 KMT 句柄翻译用）
// ---------------------------------------------------------------------------

using PFN_wglGetProcAddress = void*(WINAPI*)(LPCSTR);
using GLuint = unsigned int;
using GLenum = unsigned int;
using GLint = int;

using PFN_wglDXOpenDeviceNV  = HANDLE(WINAPI*)(void*);
using PFN_wglDXCloseDeviceNV = BOOL(WINAPI*)(HANDLE);
using PFN_wglDXRegisterObjectNV = HANDLE(WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
using PFN_wglDXUnregisterObjectNV = BOOL(WINAPI*)(HANDLE, HANDLE);
using PFN_wglDXLockObjectsNV = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);
using PFN_wglDXUnlockObjectsNV = BOOL(WINAPI*)(HANDLE, GLint, HANDLE*);

struct InteropApi {
    PFN_wglDXOpenDeviceNV      dxOpenDevice = nullptr;
    PFN_wglDXCloseDeviceNV     dxCloseDevice = nullptr;
    PFN_wglDXRegisterObjectNV  dxRegisterObject = nullptr;
    PFN_wglDXUnregisterObjectNV dxUnregisterObject = nullptr;
    PFN_wglDXLockObjectsNV     dxLockObjects = nullptr;
    PFN_wglDXUnlockObjectsNV   dxUnlockObjects = nullptr;
    bool loaded = false;
};
static InteropApi g_interop;

static bool loadInteropApi() {
    if (g_interop.loaded) return true;
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    if (!gl) return false;
    auto wglGetProcAddress =
        reinterpret_cast<PFN_wglGetProcAddress>(GetProcAddress(gl, "wglGetProcAddress"));
    if (!wglGetProcAddress) return false;
    g_interop.dxOpenDevice = reinterpret_cast<PFN_wglDXOpenDeviceNV>(
        wglGetProcAddress("wglDXOpenDeviceNV"));
    g_interop.dxCloseDevice = reinterpret_cast<PFN_wglDXCloseDeviceNV>(
        wglGetProcAddress("wglDXCloseDeviceNV"));
    g_interop.dxRegisterObject = reinterpret_cast<PFN_wglDXRegisterObjectNV>(
        wglGetProcAddress("wglDXRegisterObjectNV"));
    g_interop.dxUnregisterObject = reinterpret_cast<PFN_wglDXUnregisterObjectNV>(
        wglGetProcAddress("wglDXUnregisterObjectNV"));
    g_interop.dxLockObjects = reinterpret_cast<PFN_wglDXLockObjectsNV>(
        wglGetProcAddress("wglDXLockObjectsNV"));
    g_interop.dxUnlockObjects = reinterpret_cast<PFN_wglDXUnlockObjectsNV>(
        wglGetProcAddress("wglDXUnlockObjectsNV"));
    g_interop.loaded = g_interop.dxOpenDevice && g_interop.dxRegisterObject &&
                       g_interop.dxLockObjects && g_interop.dxUnlockObjects;
    return g_interop.loaded;
}

// ---------------------------------------------------------------------------
// 日志
// ---------------------------------------------------------------------------

static FILE* g_log = nullptr;
static std::wstring g_logPath;
static void logOpen(const wchar_t* path) {
    if (g_log) return;
    g_logPath = path;
    g_log = _wfopen(path, L"a");
}
static void logClose() {
    if (g_log) { fclose(g_log); g_log = nullptr; }
}
static void logf(const char* fmt, ...) {
    va_list a; va_start(a, fmt);
    if (g_log) { vfprintf(g_log, fmt, a); fflush(g_log); }
    va_end(a);
}

// ---------------------------------------------------------------------------
// Vulkan 资源
// ---------------------------------------------------------------------------

struct VkTex {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t width = 0;
    uint32_t height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
};

// 从 KMT 句柄导入 GL 纹理为 Vulkan 图像（D3D11 中转）
struct ImportedTex {
    VkTex vk;                      // Vulkan 图像（来自 KMT 句柄导入）
    Microsoft::WRL::ComPtr<ID3D11Texture2D> dx;  // D3D11 中转纹理
    HANDLE kmtHandle = nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    bool valid() const { return vk.image != VK_NULL_HANDLE && kmtHandle != nullptr; }
};

// ---------------------------------------------------------------------------
// 全局上下文
// ---------------------------------------------------------------------------

#define SR_CHANNELS 3

struct DlssContext {
    bool slInited = false;
    bool devReady = false;

    // 极小 D3D11 设备：仅用于 GL↔Vulkan KMT 翻译，没有 GPU 工作负载
    Microsoft::WRL::ComPtr<ID3D11Device> d3dDev;
    Microsoft::WRL::ComPtr<ID3D11Device5> d3dDev5;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3dCtx;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext4> d3dCtx4;
    Microsoft::WRL::ComPtr<ID3D11Fence> fence;
    HANDLE fenceEvent = nullptr;
    HANDLE interopDevice = nullptr;
    uint64_t fenceValue = 0;
    bool outLocked = false;

    // Vulkan
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmdRing[SR_CHANNELS]{};
    VkFence fenceRing[SR_CHANNELS]{};
    VkSemaphore imageAvailRing[SR_CHANNELS]{};
    VkSemaphore renderDoneRing[SR_CHANNELS]{};
    uint32_t frameSlot = 0;
    VkPhysicalDeviceMemoryProperties memProps{};
    uint32_t memTypeIdx = UINT32_MAX;

    // 维度
    uint32_t displayW = 0, displayH = 0;
    uint32_t renderW = 0, renderH = 0;

    // 资源
    ImportedTex colorIn;   // 低分辨率颜色（来自 MC worldTarget）
    ImportedTex aux;       // 低分辨率 RGBA16F：R=深度, G/B=运动向量
    ImportedTex colorOut;  // 全分辨率输出（GL 互操作，从 Vulkan 导出）

    sl::ViewportHandle viewport{};
    uint32_t frameIndex = 0;

    sl::DLSSMode mode = sl::DLSSMode::eOff;
    float sharpness = 0.0f;
    bool hdr = false;

    bool setupDone = false;
};

static DlssContext g_ctx;
static std::wstring g_pluginDir;

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

static void toRowMajor(const float* src, sl::float4x4& dst) {
    for (int r = 0; r < 4; ++r) {
        dst.row[r].x = src[0 * 4 + r];
        dst.row[r].y = src[1 * 4 + r];
        dst.row[r].z = src[2 * 4 + r];
        dst.row[r].w = src[3 * 4 + r];
    }
}
static void invertMatrix(const sl::float4x4& in, sl::float4x4& out) { sl::matrixFullInvert(out, in); }
static void mulMatrix(const sl::float4x4& a, const sl::float4x4& b, sl::float4x4& out) { sl::matrixMul(out, a, b); }

static std::wstring utf8ToWide(const char* s) {
    if (!s) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), len);
    return out;
}

// 等 fence：Flush + Signal + 等落地（用真实 CPU-GPU 同步做锁纪律）
static void wait11() {
    if (!g_ctx.fence) return;
    g_ctx.d3dCtx->Flush();
    g_ctx.fenceValue++;
    g_ctx.d3dCtx4->Signal(g_ctx.fence.Get(), g_ctx.fenceValue);
    g_ctx.fence->SetEventOnCompletion(g_ctx.fenceValue, g_ctx.fenceEvent);
    WaitForSingleObject(g_ctx.fenceEvent, 5000);
}

static VkBool32 hasVkExt(VkPhysicalDevice phys, const char* name) {
    uint32_t n = 0; vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, exts.data());
    for (auto& e : exts) if (strcmp(e.extensionName, name) == 0) return VK_TRUE;
    return VK_FALSE;
}
static uint32_t findMemoryType(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g_ctx.memProps.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (g_ctx.memProps.memoryTypes[i].propertyFlags & want) == want) return i;
    }
    return UINT32_MAX;
}

// ---------------------------------------------------------------------------
// Vulkan 初始化
// ---------------------------------------------------------------------------

static bool initVulkan() {
    if (g_ctx.instance) return true;

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "dlssmc-sr";
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ic{};
    ic.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ic.pApplicationInfo = &app;
    VkResult r = vkCreateInstance(&ic, nullptr, &g_ctx.instance);
    if (r != VK_SUCCESS) { logf("[dlssmc] vkCreateInstance -> %d\n", (int)r); return false; }

    uint32_t physCount = 0;
    vkEnumeratePhysicalDevices(g_ctx.instance, &physCount, nullptr);
    if (physCount == 0) return false;
    std::vector<VkPhysicalDevice> physList(physCount);
    vkEnumeratePhysicalDevices(g_ctx.instance, &physCount, physList.data());

    // 选块设备：VK_KHR_external_memory_win32 必须支持
    for (auto p : physList) {
        if (hasVkExt(p, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME)) { g_ctx.phys = p; break; }
    }
    if (!g_ctx.phys) {
        logf("[dlssmc] no Vulkan device with VK_KHR_external_memory_win32\n");
        return false;
    }

    vkGetPhysicalDeviceMemoryProperties(g_ctx.phys, &g_ctx.memProps);

    // 找支持图形+传输的队列族
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_ctx.phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(g_ctx.phys, &qfCount, qfs.data());
    for (uint32_t i = 0; i < qfCount; i++) {
        if ((qfs[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT)) {
            g_ctx.queueFamily = i; break;
        }
    }
    if (g_ctx.queueFamily == UINT32_MAX) return false;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g_ctx.queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    const char* devExts[] = { VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME };
    VkDeviceCreateInfo dcci{};
    dcci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dcci.queueCreateInfoCount = 1;
    dcci.pQueueCreateInfos = &qci;
    dcci.enabledExtensionCount = 1;
    dcci.ppEnabledExtensionNames = devExts;

    r = vkCreateDevice(g_ctx.phys, &dcci, nullptr, &g_ctx.device);
    if (r != VK_SUCCESS) { logf("[dlssmc] vkCreateDevice -> %d\n", (int)r); return false; }

    vkGetDeviceQueue(g_ctx.device, g_ctx.queueFamily, 0, &g_ctx.queue);

    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = g_ctx.queueFamily;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    r = vkCreateCommandPool(g_ctx.device, &cpi, nullptr, &g_ctx.cmdPool);
    if (r != VK_SUCCESS) return false;

    for (uint32_t i = 0; i < SR_CHANNELS; i++) {
        VkCommandBufferAllocateInfo cbai{};
        cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool = g_ctx.cmdPool;
        cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        vkAllocateCommandBuffers(g_ctx.device, &cbai, &g_ctx.cmdRing[i]);

        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        vkCreateFence(g_ctx.device, &fci, nullptr, &g_ctx.fenceRing[i]);

        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(g_ctx.device, &sci, nullptr, &g_ctx.imageAvailRing[i]);
        vkCreateSemaphore(g_ctx.device, &sci, nullptr, &g_ctx.renderDoneRing[i]);
    }

    // Vulkan 设备专属内存：DEVICE_LOCAL 优先（GPU 端），不够再考虑 HOST_VISIBLE
    g_ctx.memTypeIdx = findMemoryType(0xFFFFFFFFu,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (g_ctx.memTypeIdx == UINT32_MAX) {
        g_ctx.memTypeIdx = findMemoryType(0xFFFFFFFFu,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    if (g_ctx.memTypeIdx == UINT32_MAX) {
        logf("[dlssmc] no suitable Vulkan memory type\n");
        return false;
    }

    logf("[dlssmc] Vulkan ready (qf=%u, memType=%u)\n", g_ctx.queueFamily, g_ctx.memTypeIdx);
    return true;
}

// ---------------------------------------------------------------------------
// Streamline 初始化（与帧生成那条共用一个 eVulkan 实例）
// ---------------------------------------------------------------------------

static sl::Result ensureStreamline(const wchar_t* pluginPath, const wchar_t* logPath) {
    if (g_ctx.slInited) return sl::Result::eOk;

    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.renderAPI = sl::RenderAPI::eVulkan;
    pref.applicationId = DLSSMC_APP_ID;
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "0.1.0";
    pref.projectId = "8a0f7c1e-5b2d-4c9a-9f3e-1d7b6a4c2e80";
    pref.flags = sl::PreferenceFlags::eUseFrameBasedResourceTagging;

    // 特性全集：Streamline 是进程级单例，插件集由第一次 slInit 决定。
    sl::Feature features[] = {sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex,
                              sl::kFeaturePCL};
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 4;

    const wchar_t* paths[1] = {pluginPath};
    if (pluginPath && pluginPath[0]) {
        pref.pathsToPlugins = paths;
        pref.numPathsToPlugins = 1;
    }
    if (logPath && logPath[0]) {
        pref.pathToLogsAndData = logPath;
    }

    auto res = slInit(pref, sl::kSDKVersion);
    if (res == sl::Result::eErrorInitNotCalled) {
        // 帧生成那条路已经初始化过 Streamline（也是 eVulkan），二次 slInit 被拒——直接复用
        logf("[dlssmc] slInit -> 23：Streamline 已由帧生成通道初始化，复用该实例\n");
        g_ctx.slInited = true;
        return sl::Result::eOk;
    }
    if (res != sl::Result::eOk) {
        logf("[dlssmc] slInit failed: %d\n", (int)res);
        return res;
    }
    g_ctx.slInited = true;
    logf("[dlssmc] slInit ok (sdk %u)\n", (unsigned)sl::kSDKVersion);
    return sl::Result::eOk;
}

// 小 D3D11 设备：仅用于 KMT 句柄翻译，没有 GPU 工作负载
static sl::Result ensureDevice() {
    if (g_ctx.devReady) return sl::Result::eOk;
    if (!initVulkan()) return sl::Result::eErrorNotInitialized;

    UINT flags = 0;
#ifndef NDEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL outLevel{};
    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, 1, D3D11_SDK_VERSION,
        g_ctx.d3dDev.GetAddressOf(), &outLevel, g_ctx.d3dCtx.GetAddressOf());
    if (FAILED(hr)) {
        logf("[dlssmc] D3D11CreateDevice failed hr=0x%08X\n", (unsigned)hr);
        return sl::Result::eErrorNotInitialized;
    }

    if (FAILED(g_ctx.d3dDev.As(&g_ctx.d3dDev5)) ||
        FAILED(g_ctx.d3dCtx.As(&g_ctx.d3dCtx4)) ||
        FAILED(g_ctx.d3dDev5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(g_ctx.fence.GetAddressOf())))) {
        logf("[dlssmc] D3D11 fence 不可用\n");
        return sl::Result::eErrorFeatureNotSupported;
    }
    g_ctx.fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_ctx.fenceEvent) return sl::Result::eErrorFeatureNotSupported;

    if (!loadInteropApi()) {
        logf("[dlssmc] WGL_NV_DX_interop2 unavailable\n");
        return sl::Result::eErrorFeatureNotSupported;
    }
    g_ctx.interopDevice = g_interop.dxOpenDevice(g_ctx.d3dDev.Get());
    if (!g_ctx.interopDevice) return sl::Result::eErrorFeatureNotSupported;

    g_ctx.devReady = true;
    logf("[dlssmc] Vulkan device + D3D11 KMT interop ready\n");
    return sl::Result::eOk;
}

// ---------------------------------------------------------------------------
// Vulkan 图像：从 GL 纹理（经 D3D11 KMT）导入
// ---------------------------------------------------------------------------

static void vkDestroy(VkTex& t) {
    if (t.view)      vkDestroyImageView(g_ctx.device, t.view, nullptr);
    if (t.image)     vkDestroyImage(g_ctx.device, t.image, nullptr);
    if (t.memory)    vkFreeMemory(g_ctx.device, t.memory, nullptr);
    t = {};
}

static void releaseImported(ImportedTex& t) {
    if (t.kmtHandle && g_ctx.interopDevice) {
        g_interop.dxUnregisterObject(g_ctx.interopDevice, t.kmtHandle);
        t.kmtHandle = nullptr;
    }
    t.dx.Reset();
    vkDestroy(t.vk);
}

// D3D11 共享贴图（MiscFlags=D3D11_RESOURCE_MISC_SHARED）+ KMT 句柄
static bool createDxShared(ImportedTex& t, GLuint gl, uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    releaseImported(t);
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = fmt; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    HRESULT hr = g_ctx.d3dDev->CreateTexture2D(&d, nullptr, t.dx.GetAddressOf());
    if (FAILED(hr)) { logf("[dlssmc] D3D11CreateTexture2D 0x%08X\n", (unsigned)hr); return false; }

    Microsoft::WRL::ComPtr<IDXGIResource> res;
    if (FAILED(t.dx.As(&res)) ||
        FAILED(res->GetSharedHandle(&t.kmtHandle)) || !t.kmtHandle) {
        logf("[dlssmc] GetSharedHandle failed\n"); t.dx.Reset(); return false;
    }

    t.kmtHandle = g_interop.dxRegisterObject(
        g_ctx.interopDevice, t.dx.Get(), gl, DLSSMC_GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
    if (!t.kmtHandle) {
        logf("[dlssmc] wglDXRegisterObjectNV failed gl=%u\n", gl);
        t.dx.Reset(); return false;
    }
    t.vk.width = w; t.vk.height = h; t.vk.format = VK_FORMAT_R8G8B8A8_UNORM;
    return true;
}

static bool createVkImage(ImportedTex& t, VkFormat vkFmt, VkImageUsageFlags usage) {
    vkDestroy(t.vk);
    t.vk.width = t.width; t.vk.height = t.height; t.vk.format = vkFmt;
    t.vk.extent.width = t.width; t.vk.extent.height = t.height;

    VkExternalMemoryImageCreateInfo emi{};
    emi.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

    VkImageCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.pNext = &emi;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = vkFmt;
    ci.extent = {t.width, t.height, 1};
    ci.mipLevels = 1; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(g_ctx.device, &ci, nullptr, &t.vk.image) != VK_SUCCESS) return false;

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(g_ctx.device, t.vk.image, &req);

    VkImportMemoryWin32HandleInfoKHR imp{};
    imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
    imp.handle = t.kmtHandle;
    imp.name = nullptr;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &imp;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = g_ctx.memTypeIdx;
    if (vkAllocateMemory(g_ctx.device, &mai, nullptr, &t.vk.memory) != VK_SUCCESS) {
        vkDestroyImage(g_ctx.device, t.vk.image, nullptr); t.vk = {}; return false;
    }
    if (vkBindImageMemory(g_ctx.device, t.vk.image, t.vk.memory, 0) != VK_SUCCESS) {
        vkFreeMemory(g_ctx.device, t.vk.memory, nullptr);
        vkDestroyImage(g_ctx.device, t.vk.image, nullptr);
        t.vk = {}; return false;
    }

    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t.vk.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = vkFmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(g_ctx.device, &vi, nullptr, &t.vk.view) != VK_SUCCESS) {
        vkDestroyImage(g_ctx.device, t.vk.image, nullptr); t.vk = {}; return false;
    }
    t.vk.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

static bool importGl(ImportedTex& t, GLuint gl, uint32_t w, uint32_t h, DXGI_FORMAT fmt,
                    VkImageUsageFlags usage) {
    VkFormat vkFmt = VK_FORMAT_R8G8B8A8_UNORM;  // 颜色和 RGBA16F 都先按 8 位走，DLSS 不在意位深对 tag
    if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) vkFmt = VK_FORMAT_R16G16B16A16_SFLOAT;
    if (!createDxShared(t, gl, w, h, fmt)) return false;
    return createVkImage(t, vkFmt, usage);
}

static void transition(VkCommandBuffer cmd, VkImage img,
                      VkImageLayout oldL, VkAccessFlags oldA,
                      VkImageLayout newL, VkAccessFlags newA) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = oldA;
    b.dstAccessMask = newA;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
        0, nullptr, 0, nullptr, 1, &b);
}

// ---------------------------------------------------------------------------
// JNI
// ---------------------------------------------------------------------------

extern "C" {

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSNative_nativeInit(JNIEnv* env, jclass, jstring pluginPath, jstring logPath) {
    const char* p = pluginPath ? env->GetStringUTFChars(pluginPath, nullptr) : nullptr;
    const char* l = logPath ? env->GetStringUTFChars(logPath, nullptr) : nullptr;
    std::wstring wp = utf8ToWide(p);
    std::wstring wl = utf8ToWide(l);
    if (p) env->ReleaseStringUTFChars(pluginPath, p);
    if (l) env->ReleaseStringUTFChars(logPath, l);

    g_pluginDir = wp;
    if (!wl.empty()) logOpen((wl + L"\\dlssmc_native.log").c_str());
    logf("[dlssmc] ---- nativeInit (Vulkan) ----\n");

    auto r = ensureStreamline(wp.c_str(), wl.c_str());
    if (r != sl::Result::eOk) return (jint)r;
    r = ensureDevice();
    return (jint)r;
}

JNIEXPORT void JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSNative_nativeShutdown(JNIEnv*, jclass) {
    logf("[dlssmc] nativeShutdown\n");
    if (g_ctx.outLocked && g_ctx.interopDevice) {
        HANDLE h = g_ctx.colorOut.kmtHandle;
        g_interop.dxUnlockObjects(g_ctx.interopDevice, 1, &h);
        g_ctx.outLocked = false;
    }
    releaseImported(g_ctx.colorIn);
    releaseImported(g_ctx.aux);
    releaseImported(g_ctx.colorOut);
    if (g_ctx.interopDevice) { g_interop.dxCloseDevice(g_ctx.interopDevice); g_ctx.interopDevice = nullptr; }
    if (g_ctx.fenceEvent) { CloseHandle(g_ctx.fenceEvent); g_ctx.fenceEvent = nullptr; }
    g_ctx.fenceValue = 0;
    g_ctx.fence.Reset(); g_ctx.d3dDev5.Reset(); g_ctx.d3dCtx4.Reset(); g_ctx.d3dCtx.Reset(); g_ctx.d3dDev.Reset();
    if (g_ctx.device) {
        for (uint32_t i = 0; i < SR_CHANNELS; i++) {
            if (g_ctx.fenceRing[i])    vkDestroyFence(g_ctx.device, g_ctx.fenceRing[i], nullptr);
            if (g_ctx.imageAvailRing[i]) vkDestroySemaphore(g_ctx.device, g_ctx.imageAvailRing[i], nullptr);
            if (g_ctx.renderDoneRing[i]) vkDestroySemaphore(g_ctx.device, g_ctx.renderDoneRing[i], nullptr);
        }
        if (g_ctx.cmdPool) vkDestroyCommandPool(g_ctx.device, g_ctx.cmdPool, nullptr);
        vkDestroyDevice(g_ctx.device, nullptr);
    }
    if (g_ctx.instance) vkDestroyInstance(g_ctx.instance, nullptr);
    g_ctx.device = VK_NULL_HANDLE;
    g_ctx.instance = VK_NULL_HANDLE;
    g_ctx.phys = VK_NULL_HANDLE;
    g_ctx.queue = VK_NULL_HANDLE;
    g_ctx.cmdPool = VK_NULL_HANDLE;
    for (auto& f : g_ctx.fenceRing) f = VK_NULL_HANDLE;
    for (auto& s : g_ctx.imageAvailRing) s = VK_NULL_HANDLE;
    for (auto& s : g_ctx.renderDoneRing) s = VK_NULL_HANDLE;
    g_ctx.devReady = false;

    if (g_ctx.slInited) { slShutdown(); g_ctx.slInited = false; }
    g_ctx.setupDone = false;
    logClose();
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSNative_nativeIsDlssSupported(JNIEnv*, jclass) {
    if (!g_ctx.slInited || !g_ctx.devReady) return 0;
    VkPhysicalDeviceProperties2 phys{}; phys.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    vkGetPhysicalDeviceProperties2(g_ctx.phys, &phys);
    VkPhysicalDeviceIDProperties id{};
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    (void)id;

    sl::AdapterInfo info{};
    info.deviceLUID = reinterpret_cast<uint8_t*>(&phys.properties.deviceID);
    info.deviceLUIDSizeInBytes = sizeof(phys.properties.deviceID);
    auto r = slIsFeatureSupported(sl::kFeatureDLSS, info);
    logf("[dlssmc] slIsFeatureSupported -> %d\n", (int)r);
    return r == sl::Result::eOk ? 1 : 0;
}

JNIEXPORT jintArray JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSNative_nativeGetOptimalSettings(
    JNIEnv* env, jclass, jint displayW, jint displayH, jint mode) {
    jintArray fallback = env->NewIntArray(7);
    jint zero[7] = {0, 0, 0, 0, 0, 0, 0};
    env->SetIntArrayRegion(fallback, 0, 7, zero);

    if (!g_ctx.slInited || !g_ctx.devReady) return fallback;

    sl::DLSSOptions opt{};
    opt.mode = static_cast<sl::DLSSMode>(mode);
    opt.outputWidth = (uint32_t)displayW;
    opt.outputHeight = (uint32_t)displayH;

    sl::DLSSOptimalSettings out{};
    auto r = slDLSSGetOptimalSettings(opt, out);
    if (r != sl::Result::eOk) return fallback;

    jint vals[7] = {
        (jint)out.optimalRenderWidth, (jint)out.optimalRenderHeight,
        (jint)out.renderWidthMin, (jint)out.renderHeightMin,
        (jint)out.renderWidthMax, (jint)out.renderHeightMax,
        (jint)(out.optimalSharpness * 1000.0f)};
    env->SetIntArrayRegion(fallback, 0, 7, vals);
    return fallback;
}

static void applyPreset(sl::DLSSOptions& opt, sl::DLSSMode mode, sl::DLSSPreset preset) {
    switch (mode) {
        case sl::DLSSMode::eDLAA:             opt.dlaaPreset = preset; break;
        case sl::DLSSMode::eMaxQuality:       opt.qualityPreset = preset; break;
        case sl::DLSSMode::eBalanced:         opt.balancedPreset = preset; break;
        case sl::DLSSMode::eMaxPerformance:   opt.performancePreset = preset; break;
        case sl::DLSSMode::eUltraPerformance: opt.ultraPerformancePreset = preset; break;
        case sl::DLSSMode::eUltraQuality:     opt.ultraQualityPreset = preset; break;
        default: break;
    }
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSNative_nativeSetup(
    JNIEnv*, jclass, jint displayW, jint displayH, jint renderW, jint renderH,
    jint mode, jfloat sharpness, jboolean hdr, jint preset,
    jint colorGl, jint auxGl, jint outGl) {
    if (!g_ctx.slInited || !g_ctx.devReady) return (jint)sl::Result::eErrorNotInitialized;

    g_ctx.displayW = (uint32_t)displayW;
    g_ctx.displayH = (uint32_t)displayH;
    g_ctx.renderW = (uint32_t)renderW;
    g_ctx.renderH = (uint32_t)renderH;
    g_ctx.mode = static_cast<sl::DLSSMode>(mode);
    g_ctx.sharpness = sharpness;
    g_ctx.hdr = (hdr == JNI_TRUE);

    VkImageUsageFlags inUsage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageUsageFlags outUsage =
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    if (!importGl(g_ctx.colorIn, (GLuint)colorGl, g_ctx.renderW, g_ctx.renderH,
                  DXGI_FORMAT_R8G8B8A8_UNORM, inUsage)) return (jint)sl::Result::eErrorNotInitialized;
    if (!importGl(g_ctx.aux, (GLuint)auxGl, g_ctx.renderW, g_ctx.renderH,
                  DXGI_FORMAT_R16G16B16A16_FLOAT, inUsage)) return (jint)sl::Result::eErrorNotInitialized;
    if (!importGl(g_ctx.colorOut, (GLuint)outGl, g_ctx.displayW, g_ctx.displayH,
                  DXGI_FORMAT_R8G8B8A8_UNORM, outUsage)) return (jint)sl::Result::eErrorNotInitialized;

    sl::DLSSOptions opt{};
    opt.mode = g_ctx.mode;
    opt.outputWidth = g_ctx.displayW;
    opt.outputHeight = g_ctx.displayH;
    opt.sharpness = g_ctx.sharpness;
    opt.colorBuffersHDR = g_ctx.hdr ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    opt.useAutoExposure = sl::Boolean::eFalse;
    applyPreset(opt, g_ctx.mode, (sl::DLSSPreset)preset);

    auto r = slDLSSSetOptions(g_ctx.viewport, opt);
    if (r != sl::Result::eOk) return (jint)r;

    g_ctx.setupDone = true;
    logf("[dlssmc] setup (Vulkan): render %ux%u -> display %ux%u mode=%d\n",
         g_ctx.renderW, g_ctx.renderH, g_ctx.displayW, g_ctx.displayH, (int)mode);
    return (jint)sl::Result::eOk;
}

JNIEXPORT void JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSNative_nativeReleaseResources(JNIEnv*, jclass) {
    if (g_ctx.setupDone) slFreeResources(sl::kFeatureDLSS, g_ctx.viewport);
    if (g_ctx.outLocked && g_ctx.interopDevice) {
        HANDLE h = g_ctx.colorOut.kmtHandle;
        g_interop.dxUnlockObjects(g_ctx.interopDevice, 1, &h);
        g_ctx.outLocked = false;
    }
    releaseImported(g_ctx.colorIn);
    releaseImported(g_ctx.aux);
    releaseImported(g_ctx.colorOut);
    g_ctx.setupDone = false;
}

// 输入段锁：先把 D3D11 排空（GL→D3D 的 barrier），锁后 GL 才能写
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSNative_nativeLockInputs(JNIEnv*, jclass) {
    if (!g_ctx.setupDone || !g_ctx.colorIn.valid() || !g_ctx.aux.valid()) return -1;
    // 不再 wait11：DLSS-SR 已跑在 Vulkan 上，D3D11 队列本来就空，
    // 每帧 Flush+Signal+Wait 纯属白等一次全管线停顿。GL↔D3D 的可见性
    // 由 wglDXLockObjectsNV 自己保证。
    HANDLE objs[2] = {g_ctx.colorIn.kmtHandle, g_ctx.aux.kmtHandle};
    if (!g_interop.dxLockObjects(g_ctx.interopDevice, 2, objs)) return -2;
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSNative_nativeUnlockInputs(JNIEnv*, jclass) {
    if (!g_ctx.colorIn.kmtHandle || !g_ctx.aux.kmtHandle) return -1;
    HANDLE objs[2] = {g_ctx.colorIn.kmtHandle, g_ctx.aux.kmtHandle};
    if (!g_interop.dxUnlockObjects(g_ctx.interopDevice, 2, objs)) return -2;
    return 0;
}

// 每帧评估：纯 Vulkan 命令缓冲
// matrices: 列主序 4x4 —— [0..15] proj, [16..31] view, [32..47] prevProj, [48..63] prevView
// params:   [0] near [1] far [2] fov(rad) [3] aspect [4] jitterX [5] jitterY [6] mvecScaleX [7] mvecScaleY
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSNative_nativeEvaluate(
    JNIEnv* env, jclass, jfloatArray matrices, jfloatArray params, jboolean reset) {
    if (!g_ctx.setupDone || !g_ctx.colorIn.valid() || !g_ctx.aux.valid() || !g_ctx.colorOut.valid()) {
        return (jint)sl::Result::eErrorNotInitialized;
    }
    if (!matrices || env->GetArrayLength(matrices) < 64) {
        return (jint)sl::Result::eErrorInvalidParameter;
    }

    float m[64]; env->GetFloatArrayRegion(matrices, 0, 64, m);
    float p[8] = {0, 0, 0, 0, 0, 0, 1.0f, 1.0f};
    if (params && env->GetArrayLength(params) >= 8) env->GetFloatArrayRegion(params, 0, 8, p);

    sl::float4x4 proj, view, prevProj, prevView;
    toRowMajor(m + 0, proj);
    toRowMajor(m + 16, view);
    toRowMajor(m + 32, prevProj);
    toRowMajor(m + 48, prevView);

    sl::float4x4 clipToCameraView, viewToWorld, prevViewProj, clipToPrevClip, prevClipToClip;
    invertMatrix(proj, clipToCameraView);
    invertMatrix(view, viewToWorld);
    mulMatrix(prevProj, prevView, prevViewProj);
    { sl::float4x4 tmp; mulMatrix(clipToCameraView, viewToWorld, tmp); mulMatrix(tmp, prevViewProj, clipToPrevClip); }
    invertMatrix(clipToPrevClip, prevClipToClip);

    // 输出锁住直到 GL 读走——DLSS 在 Vulkan 上写 output image，跨 GL 的 D3D11 中转不可见
    if (!g_interop.dxLockObjects(g_ctx.interopDevice, 1, &g_ctx.colorOut.kmtHandle)) {
        return (jint)sl::Result::eErrorNotInitialized;
    }
    g_ctx.outLocked = true;
    auto unlockOut = [&]{
        HANDLE h = g_ctx.colorOut.kmtHandle;
        g_interop.dxUnlockObjects(g_ctx.interopDevice, 1, &h);
        g_ctx.outLocked = false;
    };

    sl::FrameToken* token = nullptr;
    uint32_t frameIndex = g_ctx.frameIndex++;
    auto r = slGetNewFrameToken(token, &frameIndex);
    if (r != sl::Result::eOk) { unlockOut(); return (jint)r; }

    // 资源封装：传给 Streamline 的 sl::Resource 用 VkImage 指针（不是 ID3D11Texture2D*）
    auto makeResource = [](VkTex& t, uint32_t w, uint32_t h) {
        sl::Resource res(sl::ResourceType::eTex2d, (void*)t.image, t.memory, t.view,
                        (uint32_t)t.layout);
        return res;
    };
    sl::Resource resColorIn  = makeResource(g_ctx.colorIn.vk,  g_ctx.renderW, g_ctx.renderH);
    sl::Resource resColorOut = makeResource(g_ctx.colorOut.vk, g_ctx.displayW, g_ctx.displayH);
    sl::Resource resDepth    = makeResource(g_ctx.aux.vk,      g_ctx.renderW, g_ctx.renderH);
    sl::Resource resMvec    = makeResource(g_ctx.aux.vk,      g_ctx.renderW, g_ctx.renderH);

    sl::Extent renderExtent{0, 0, g_ctx.renderW, g_ctx.renderH};
    sl::Extent outExtent   {0, 0, g_ctx.displayW, g_ctx.displayH};

    sl::ResourceTag tags[] = {
        sl::ResourceTag{&resColorIn,  sl::kBufferTypeScalingInputColor, sl::eValidUntilEvaluate, &renderExtent},
        sl::ResourceTag{&resColorOut, sl::kBufferTypeScalingOutputColor, sl::eValidUntilEvaluate, &outExtent},
        sl::ResourceTag{&resDepth,    sl::kBufferTypeDepth,            sl::eValidUntilEvaluate, &renderExtent},
        sl::ResourceTag{&resMvec,    sl::kBufferTypeMotionVectors,    sl::eValidUntilEvaluate, &renderExtent},
    };

    const uint32_t slot = g_ctx.frameSlot;
    g_ctx.frameSlot = (slot + 1) % SR_CHANNELS;

    vkWaitForFences(g_ctx.device, 1, &g_ctx.fenceRing[slot], VK_TRUE, UINT64_MAX);
    vkResetFences(g_ctx.device, 1, &g_ctx.fenceRing[slot]);

    VkCommandBuffer cmd = g_ctx.cmdRing[slot];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    // 把三张导入图都迁到 SHADER_READ_ONLY（DLSS 评估要读它们）
    transition(cmd, g_ctx.colorIn.vk.image,  VK_IMAGE_LAYOUT_UNDEFINED, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT);
    transition(cmd, g_ctx.aux.vk.image,      VK_IMAGE_LAYOUT_UNDEFINED, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT);
    transition(cmd, g_ctx.colorOut.vk.image, VK_IMAGE_LAYOUT_UNDEFINED, 0, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT);

    r = slSetTagForFrame(*token, g_ctx.viewport, tags, 4, cmd);
    if (r != sl::Result::eOk) { vkEndCommandBuffer(cmd); unlockOut(); return (jint)r; }

    sl::Constants consts{};
    consts.cameraViewToClip = proj;
    consts.clipToCameraView = clipToCameraView;
    consts.clipToPrevClip = clipToPrevClip;
    consts.prevClipToClip = prevClipToClip;
    consts.jitterOffset = {p[4], p[5]};
    consts.mvecScale = {p[6], p[7]};
    consts.cameraPos = {0.0f, 0.0f, 0.0f};
    consts.cameraNear = p[0];
    consts.cameraFar = p[1];
    consts.cameraFOV = p[2];
    consts.cameraAspectRatio = p[3];
    consts.cameraUp = {0.0f, 1.0f, 0.0f};
    consts.cameraRight = {1.0f, 0.0f, 0.0f};
    consts.cameraFwd = {0.0f, 0.0f, -1.0f};
    consts.depthInverted = sl::Boolean::eFalse;
    consts.cameraMotionIncluded = sl::Boolean::eTrue;
    consts.motionVectors3D = sl::Boolean::eFalse;
    consts.orthographicProjection = sl::Boolean::eFalse;
    consts.motionVectorsDilated = sl::Boolean::eFalse;
    consts.reset = reset == JNI_TRUE ? sl::Boolean::eTrue : sl::Boolean::eFalse;

    r = slSetConstants(consts, *token, g_ctx.viewport);
    if (r != sl::Result::eOk) { vkEndCommandBuffer(cmd); unlockOut(); return (jint)r; }

    const sl::BaseStructure* inputs[] = {&g_ctx.viewport};
    r = slEvaluateFeature(sl::kFeatureDLSS, *token, inputs, 1, cmd);
    if (r != sl::Result::eOk) { vkEndCommandBuffer(cmd); unlockOut(); return (jint)r; }

    // DLSS-SR 写完 output：迁到 TRANSFER_SRC（可被 blit 到主 target）
    transition(cmd, g_ctx.colorOut.vk.image, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(g_ctx.queue, 1, &si, g_ctx.fenceRing[slot]) != VK_SUCCESS) {
        unlockOut(); return -4;
    }

    // 等的就是这次提交的 fence——GL 马上要读 colorOut。
    // 原来这里 wait11() 等的是 D3D11 队列（空的，立刻返回），
    // Vulkan 的 DLSS-SR 工作根本没等，读回全靠运气。
    vkWaitForFences(g_ctx.device, 1, &g_ctx.fenceRing[slot], VK_TRUE, UINT64_MAX);

    return (jint)sl::Result::eOk;
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSNative_nativeEvaluateDone(JNIEnv*, jclass) {
    if (!g_ctx.outLocked) return -1;
    g_ctx.outLocked = false;
    HANDLE h = g_ctx.colorOut.kmtHandle;
    if (!g_interop.dxUnlockObjects(g_ctx.interopDevice, 1, &h)) return -2;
    return 0;
}

} // extern "C"