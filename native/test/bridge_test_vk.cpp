// GL -> Vulkan 桥接验证（路线 E）
//
// 背景：DLSS 帧生成只支持 D3D12 / Vulkan，而 MC 是 OpenGL。
// 路线 A（D3D11On12）、B（GL_EXT_memory_object）、D（NT 共享句柄 -> D3D12）实测全部失败，
// 关键矛盾是：WGL 互操作只认老的 D3D11 KMT 句柄，而 D3D12 只认 NT 句柄。
//
// Vulkan 是唯一原生支持导入 D3D11 KMT 句柄的 API：
//     VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT
//
// 本程序验证：GL 渲染 -> D3D11 共享纹理(KMT) -> Vulkan 导入 -> 读回像素
//
// Vulkan 采用运行时动态加载 vulkan-1.dll，不依赖 SDK 导入库（本机没装 Vulkan SDK）。
//
// 编译：native/test/build_vk.sh

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------ GL
#define GL_TEXTURE_2D 0x0DE1
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_RGBA8 0x8058
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_COLOR_BUFFER_BIT 0x00004000
#define WGL_ACCESS_READ_WRITE_NV 0x0001

#define LOG(...)             \
    do {                     \
        printf(__VA_ARGS__); \
        fflush(stdout);      \
    } while (0)

static void fail(const char* msg) {
    LOG("\n[FAIL] %s\n>>> 路线 E 不可行\n", msg);
    exit(1);
}

static void checkHr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s 失败 (hr=0x%08X)", what, (unsigned)hr);
        fail(buf);
    }
    LOG("  [ok] %s\n", what);
}

static void checkVk(VkResult r, const char* what) {
    if (r != VK_SUCCESS) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s 失败 (VkResult=%d)", what, (int)r);
        fail(buf);
    }
    LOG("  [ok] %s\n", what);
}

using PFN_wglGetProcAddress = void* (WINAPI*)(LPCSTR);
using PFN_wglCreateContextAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using PFN_glGenTextures = void (*)(int, unsigned*);
using PFN_glBindTexture = void (*)(unsigned, unsigned);
using PFN_glTexImage2D = void (*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
using PFN_glGenFramebuffers = void (*)(int, unsigned*);
using PFN_glBindFramebuffer = void (*)(unsigned, unsigned);
using PFN_glFramebufferTexture2D = void (*)(unsigned, unsigned, unsigned, unsigned, int);
using PFN_glViewport = void (*)(int, int, int, int);
using PFN_glClearColor = void (*)(float, float, float, float);
using PFN_glClear = void (*)(unsigned);
using PFN_glFinish = void (*)();

template <typename T>
static T loadGl(const char* name) {
    auto get = reinterpret_cast<PFN_wglGetProcAddress>(
        GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglGetProcAddress"));
    void* p = get ? get(name) : nullptr;
    if (!p) p = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"opengl32.dll"), name));
    if (!p) {
        char buf[128];
        snprintf(buf, sizeof(buf), "取不到 GL 函数 %s", name);
        fail(buf);
    }
    return reinterpret_cast<T>(p);
}

using PFN_wglDXOpenDeviceNV = HANDLE(WINAPI*)(void*);
using PFN_wglDXRegisterObjectNV = HANDLE(WINAPI*)(HANDLE, void*, unsigned, unsigned, unsigned);
using PFN_wglDXLockObjectsNV = BOOL(WINAPI*)(HANDLE, int, HANDLE*);
using PFN_wglDXUnlockObjectsNV = BOOL(WINAPI*)(HANDLE, int, HANDLE*);
using PFN_wglDXCloseDeviceNV = BOOL(WINAPI*)(HANDLE);

static HWND makeWindow(HINSTANCE hInst) {
    const wchar_t* cls = L"DlssmcVkBridgeCls";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, cls, L"GL->Vulkan bridge test", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
                                nullptr, nullptr, hInst, nullptr);
    if (!hwnd) fail("创建窗口失败");
    ShowWindow(hwnd, SW_SHOW);
    return hwnd;
}

static HGLRC makeGlContext(HWND hwnd) {
    HDC hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(hdc, &pfd);
    if (!pf) fail("ChoosePixelFormat 失败");
    if (!SetPixelFormat(hdc, pf, &pfd)) fail("SetPixelFormat 失败");

    HGLRC temp = wglCreateContext(hdc);
    if (!temp) fail("wglCreateContext 失败");
    wglMakeCurrent(hdc, temp);

    auto getProc = reinterpret_cast<PFN_wglGetProcAddress>(
        GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglGetProcAddress"));
    auto createAttribs = reinterpret_cast<PFN_wglCreateContextAttribs>(
        getProc("wglCreateContextAttribsARB"));
    if (createAttribs) {
        int versions[][2] = {{4, 6}, {4, 5}, {4, 4}, {4, 3}, {3, 3}, {3, 2}};
        for (auto& v : versions) {
            int attribs[] = {0x2091, v[0], 0x2092, v[1], 0x9126, 0x00000001, 0};
            HGLRC c = createAttribs(hdc, nullptr, attribs);
            if (c) {
                LOG("  [ok] GL %d.%d core 上下文\n", v[0], v[1]);
                wglMakeCurrent(nullptr, nullptr);
                wglDeleteContext(temp);
                if (!wglMakeCurrent(hdc, c)) fail("wglMakeCurrent 失败");
                return c;
            }
        }
    }
    if (!wglMakeCurrent(hdc, temp)) fail("wglMakeCurrent 失败");
    return temp;
}

// ------------------------------------------------------------------ Vulkan 动态加载
using PFN_vkGetInstanceProcAddr = PFN_vkVoidFunction(VKAPI_PTR*)(VkInstance, const char*);
using PFN_vkGetDeviceProcAddr = PFN_vkVoidFunction(VKAPI_PTR*)(VkDevice, const char*);

static PFN_vkGetInstanceProcAddr gGetInstanceProcAddr;
static PFN_vkGetDeviceProcAddr gGetDeviceProcAddr;

#define DEF_VK_GLOBAL(fn) PFN_##fn fn = reinterpret_cast<PFN_##fn>(gGetInstanceProcAddr(nullptr, #fn))
#define DEF_VK_INST(fn) \
    PFN_##fn fn = reinterpret_cast<PFN_##fn>(gGetInstanceProcAddr(inst, #fn))
#define DEF_VK_DEV(fn)                                                          \
    PFN_##fn fn = reinterpret_cast<PFN_##fn>(gGetDeviceProcAddr(dev, #fn));     \
    if (!fn) {                                                                  \
        LOG("  [FAIL] 取不到设备级 Vulkan 函数 %s\n", #fn);                      \
        exit(1);                                                                \
    }

int main() {
    LOG("=== GL -> Vulkan 桥接验证（路线 E: D3D11 KMT 句柄 -> VK_EXTERNAL_MEMORY）===\n\n");

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HWND hwnd = makeWindow(hInst);
    LOG("  [ok] 窗口创建\n");

    // ---------- 1. GL 渲染到纹理 ----------
    HGLRC glCtx = makeGlContext(hwnd);
    auto glGenTextures_ = loadGl<PFN_glGenTextures>("glGenTextures");
    auto glBindTexture_ = loadGl<PFN_glBindTexture>("glBindTexture");
    auto glTexImage2D_ = loadGl<PFN_glTexImage2D>("glTexImage2D");
    auto glGenFramebuffers_ = loadGl<PFN_glGenFramebuffers>("glGenFramebuffers");
    auto glBindFramebuffer_ = loadGl<PFN_glBindFramebuffer>("glBindFramebuffer");
    auto glFramebufferTexture2D_ = loadGl<PFN_glFramebufferTexture2D>("glFramebufferTexture2D");
    auto glViewport_ = loadGl<PFN_glViewport>("glViewport");
    auto glClearColor_ = loadGl<PFN_glClearColor>("glClearColor");
    auto glClear_ = loadGl<PFN_glClear>("glClear");
    auto glFinish_ = loadGl<PFN_glFinish>("glFinish");

    const uint32_t W = 256, H = 256;
    unsigned glTex = 0;
    glGenTextures_(1, &glTex);
    glBindTexture_(GL_TEXTURE_2D, glTex);
    // 先只分配存储（注册互操作要求纹理已存在），实际渲染放到锁定之后
    glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    unsigned fbo = 0;
    glGenFramebuffers_(1, &fbo);
    glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTex, 0);
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    LOG("  [ok] GL 纹理已分配 %ux%u（目标色 200,40,60）\n", W, H);

    // ---------- 2. D3D11 + WGL 互操作，拿 KMT 句柄 ----------
    ComPtr<ID3D11Device> d3d11;
    ComPtr<ID3D11DeviceContext> d3d11Ctx;
    checkHr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                              D3D11_SDK_VERSION, d3d11.GetAddressOf(), nullptr,
                              d3d11Ctx.GetAddressOf()), "D3D11CreateDevice");

    auto getProc = reinterpret_cast<PFN_wglGetProcAddress>(
        GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglGetProcAddress"));
    auto pOpenDevice = reinterpret_cast<PFN_wglDXOpenDeviceNV>(getProc("wglDXOpenDeviceNV"));
    auto pRegisterObject =
        reinterpret_cast<PFN_wglDXRegisterObjectNV>(getProc("wglDXRegisterObjectNV"));
    auto pLockObjects = reinterpret_cast<PFN_wglDXLockObjectsNV>(getProc("wglDXLockObjectsNV"));
    auto pUnlockObjects = reinterpret_cast<PFN_wglDXUnlockObjectsNV>(getProc("wglDXUnlockObjectsNV"));
    if (!pOpenDevice || !pRegisterObject) fail("驱动不支持 WGL_NV_DX_interop2");

    HANDLE interop = pOpenDevice(d3d11.Get());
    if (!interop) fail("wglDXOpenDeviceNV 失败");

    D3D11_TEXTURE2D_DESC td{};
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // 只有这个组合能与 GL 互操作

    ComPtr<ID3D11Texture2D> d3d11Tex;
    checkHr(d3d11->CreateTexture2D(&td, nullptr, d3d11Tex.GetAddressOf()), "创建 D3D11 SHARED 纹理");

    HANDLE reg = pRegisterObject(interop, d3d11Tex.Get(), glTex, GL_TEXTURE_2D,
                                 WGL_ACCESS_READ_WRITE_NV);
    if (!reg) fail("wglDXRegisterObjectNV 失败");
    if (!pLockObjects(interop, 1, &reg)) fail("wglDXLockObjectsNV 失败");
    LOG("  [ok] GL 纹理 <-> D3D11 共享纹理已关联\n");

    // 锁定之后再渲染，确保 GL 的写入落在共享内存上
    glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
    glViewport_(0, 0, W, H);
    glClearColor_(200.0f / 255.0f, 40.0f / 255.0f, 60.0f / 255.0f, 1.0f);
    glClear_(GL_COLOR_BUFFER_BIT);
    glFinish_();
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    LOG("  [ok] GL 已在锁定状态下渲染\n");

    // ---- 对照：先用 D3D11 自己读回，确认 GL -> D3D11 这一步到底有没有通 ----
    {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = W;
        sd.Height = H;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (SUCCEEDED(d3d11->CreateTexture2D(&sd, nullptr, staging.GetAddressOf()))) {
            d3d11Ctx->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, d3d11Tex.Get(), 0, nullptr);
            D3D11_MAPPED_SUBRESOURCE ms{};
            if (SUCCEEDED(d3d11Ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &ms))) {
                auto p = static_cast<unsigned char*>(ms.pData) + 100 * ms.RowPitch + 100 * 4;
                LOG("  [对照] D3D11 读回像素 = R=%u G=%u B=%u A=%u\n", p[0], p[1], p[2], p[3]);
                d3d11Ctx->Unmap(staging.Get(), 0);
            }
        }
    }

    ComPtr<IDXGIResource> dxgiRes;
    checkHr(d3d11Tex.As(&dxgiRes), "获取 IDXGIResource");
    HANDLE kmt = nullptr;
    checkHr(dxgiRes->GetSharedHandle(&kmt), "取 KMT 共享句柄");
    LOG("  [ok] KMT 句柄 = %p\n", kmt);

    // ---------- 3. 加载 Vulkan ----------
    HMODULE vklib = LoadLibraryW(L"vulkan-1.dll");
    if (!vklib) fail("找不到 vulkan-1.dll");
    gGetInstanceProcAddr =
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vklib, "vkGetInstanceProcAddr"));
    if (!gGetInstanceProcAddr) fail("取不到 vkGetInstanceProcAddr");
    LOG("  [ok] vulkan-1.dll 已加载\n");

    DEF_VK_GLOBAL(vkCreateInstance);
    DEF_VK_GLOBAL(vkEnumerateInstanceExtensionProperties);

    // 注意：external_memory_win32 是【设备级】扩展，放实例级会直接 EXTENSION_NOT_PRESENT(-7)
    auto listInstanceExt = [&]() {
        uint32_t n = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> e(n);
        vkEnumerateInstanceExtensionProperties(nullptr, &n, e.data());
        std::vector<const char*> names;
        for (auto& p : e) names.push_back(p.extensionName);
        return names;
    };
    auto hasExt = [](const std::vector<const char*>& list, const char* want) {
        for (auto n : list)
            if (strcmp(n, want) == 0) return true;
        return false;
    };

    std::vector<const char*> availInst = listInstanceExt();
    LOG("  [info] 实例级扩展 %zu 个\n", availInst.size());

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.apiVersion = VK_API_VERSION_1_2; // external memory 在 1.1 已被提升为核心
    appInfo.pApplicationName = "dlssmc-bridge-test";

    VkInstance inst = VK_NULL_HANDLE;
    {
        std::vector<const char*> ext;
        if (hasExt(availInst, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME))
            ext.push_back(VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME);

        VkInstanceCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ci.pApplicationInfo = &appInfo;
        ci.enabledExtensionCount = (uint32_t)ext.size();
        ci.ppEnabledExtensionNames = ext.empty() ? nullptr : ext.data();
        checkVk(vkCreateInstance(&ci, nullptr, &inst), "vkCreateInstance");
    }

    gGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        gGetInstanceProcAddr(inst, "vkGetDeviceProcAddr"));
    if (!gGetDeviceProcAddr) fail("取不到 vkGetDeviceProcAddr");
    LOG("  [ok] vkCreateInstance\n");

    DEF_VK_INST(vkEnumeratePhysicalDevices);
    DEF_VK_INST(vkGetPhysicalDeviceProperties);
    DEF_VK_INST(vkGetPhysicalDeviceQueueFamilyProperties);
    DEF_VK_INST(vkCreateDevice);
    DEF_VK_INST(vkGetPhysicalDeviceMemoryProperties);
    DEF_VK_INST(vkDestroyInstance);
    DEF_VK_INST(vkGetPhysicalDeviceImageFormatProperties2);
    DEF_VK_INST(vkEnumerateDeviceExtensionProperties);

    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(inst, &gpuCount, nullptr);
    if (gpuCount == 0) fail("没有 Vulkan 物理设备");
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(inst, &gpuCount, gpus.data());

    VkPhysicalDevice phys = gpus[0];
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(gpus[0], &props);
    LOG("  [info] 使用 GPU: %s\n", props.deviceName);

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qfCount, qfs.data());
    uint32_t qf = 0;
    for (uint32_t i = 0; i < qfCount; ++i) {
        if (qfs[i].queueFlags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT)) {
            qf = i;
            break;
        }
    }

    VkDevice dev = VK_NULL_HANDLE;
    {
        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = qf;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;

        // 只启用设备确实报告的扩展（提升为核心的那些可能不再列出）
        uint32_t devExtCount = 0;
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &devExtCount, nullptr);
        std::vector<VkExtensionProperties> devExts(devExtCount);
        vkEnumerateDeviceExtensionProperties(phys, nullptr, &devExtCount, devExts.data());
        std::vector<const char*> availDev;
        for (auto& p : devExts) availDev.push_back(p.extensionName);

        std::vector<const char*> dext;
        if (hasExt(availDev, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME))
            dext.push_back(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
        if (hasExt(availDev, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME))
            dext.push_back(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
        LOG("  [info] 设备级扩展 %zu 个，启用 external_memory=%s win32=%s\n", availDev.size(),
            hasExt(availDev, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) ? "y" : "n",
            hasExt(availDev, VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME) ? "y" : "n");

        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = (uint32_t)dext.size();
        dci.ppEnabledExtensionNames = dext.empty() ? nullptr : dext.data();
        checkVk(vkCreateDevice(phys, &dci, nullptr, &dev), "vkCreateDevice");
    }

    DEF_VK_DEV(vkGetDeviceQueue);
    DEF_VK_DEV(vkCreateImage);
    DEF_VK_DEV(vkDestroyImage);
    DEF_VK_DEV(vkGetImageMemoryRequirements);
    DEF_VK_DEV(vkAllocateMemory);
    DEF_VK_DEV(vkFreeMemory);
    DEF_VK_DEV(vkBindImageMemory);
    DEF_VK_DEV(vkBindBufferMemory);
    DEF_VK_DEV(vkCreateBuffer);
    DEF_VK_DEV(vkDestroyBuffer);
    DEF_VK_DEV(vkGetBufferMemoryRequirements);
    DEF_VK_DEV(vkCreateCommandPool);
    DEF_VK_DEV(vkAllocateCommandBuffers);
    DEF_VK_DEV(vkBeginCommandBuffer);
    DEF_VK_DEV(vkCmdPipelineBarrier);
    DEF_VK_DEV(vkCmdCopyImageToBuffer);
    DEF_VK_DEV(vkEndCommandBuffer);
    DEF_VK_DEV(vkQueueSubmit);
    DEF_VK_DEV(vkQueueWaitIdle);
    DEF_VK_DEV(vkMapMemory);
    DEF_VK_DEV(vkUnmapMemory);
    DEF_VK_DEV(vkDestroyDevice);
    DEF_VK_DEV(vkGetMemoryWin32HandlePropertiesKHR);

    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, qf, 0, &queue);

    // ---------- 4. 创建外部内存 VkImage 并导入 KMT 句柄 ----------
    VkExternalMemoryImageCreateInfo extImg{};
    extImg.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

    VkImageCreateInfo imgInfo{};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.pNext = &extImg;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent = {W, H, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage vkImg = VK_NULL_HANDLE;
    VkResult r = vkCreateImage(dev, &imgInfo, nullptr, &vkImg);
    if (r != VK_SUCCESS) {
        LOG("  [!!] OPTIMAL tiling 建图失败 (%d)，改试 LINEAR\n", (int)r);
        imgInfo.tiling = VK_IMAGE_TILING_LINEAR;
        checkVk(vkCreateImage(dev, &imgInfo, nullptr, &vkImg), "vkCreateImage (LINEAR)");
    } else {
        LOG("  [ok] vkCreateImage (OPTIMAL)\n");
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(dev, vkImg, &memReq);

    VkMemoryWin32HandlePropertiesKHR handleProps{};
    handleProps.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
    r = vkGetMemoryWin32HandlePropertiesKHR(
        dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, kmt, &handleProps);
    checkVk(r, "vkGetMemoryWin32HandlePropertiesKHR");
    LOG("  [ok] 句柄可用于 memoryTypeBits=0x%X\n", handleProps.memoryTypeBits);

    VkPhysicalDeviceMemoryProperties memProps{};
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

    uint32_t memIdx = UINT32_MAX;
    uint32_t allowed = memReq.memoryTypeBits & handleProps.memoryTypeBits;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((allowed & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memIdx = i;
            break;
        }
    }
    if (memIdx == UINT32_MAX) {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if (allowed & (1u << i)) {
                memIdx = i;
                break;
            }
        }
    }
    if (memIdx == UINT32_MAX) fail("找不到兼容的 memory type");

    VkImportMemoryWin32HandleInfoKHR importInfo{};
    importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
    importInfo.handle = kmt;

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.pNext = &importInfo;
    allocInfo.allocationSize = memReq.size;
    allocInfo.memoryTypeIndex = memIdx;

    VkDeviceMemory vkMem = VK_NULL_HANDLE;
    checkVk(vkAllocateMemory(dev, &allocInfo, nullptr, &vkMem), "vkAllocateMemory（导入 KMT）");
    checkVk(vkBindImageMemory(dev, vkImg, vkMem, 0), "vkBindImageMemory");
    LOG("  [ok] KMT 句柄已导入 Vulkan\n");

    // ---------- 5. 拷到 buffer 读回 ----------
    VkDeviceSize bufSize = (VkDeviceSize)W * H * 4;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bufSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer vkBuf = VK_NULL_HANDLE;
    checkVk(vkCreateBuffer(dev, &bci, nullptr, &vkBuf), "vkCreateBuffer");

    VkMemoryRequirements bufReq{};
    vkGetBufferMemoryRequirements(dev, vkBuf, &bufReq);
    uint32_t bufMemIdx = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((bufReq.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            bufMemIdx = i;
            break;
        }
    }
    if (bufMemIdx == UINT32_MAX) fail("找不到 host visible 的 buffer memory type");

    VkMemoryAllocateInfo bai{};
    bai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    bai.allocationSize = bufReq.size;
    bai.memoryTypeIndex = bufMemIdx;
    VkDeviceMemory bufMem = VK_NULL_HANDLE;
    checkVk(vkAllocateMemory(dev, &bai, nullptr, &bufMem), "分配 buffer 内存");
    vkBindBufferMemory(dev, vkBuf, bufMem, 0);

    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = qf;
    VkCommandPool pool = VK_NULL_HANDLE;
    checkVk(vkCreateCommandPool(dev, &cpi, nullptr, &pool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    checkVk(vkAllocateCommandBuffers(dev, &cai, &cmd), "分配命令缓冲");

    VkCommandBufferBeginInfo bbi{};
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bbi);

    VkImageMemoryBarrier toSrc{};
    toSrc.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image = vkImg;
    toSrc.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toSrc.subresourceRange.levelCount = 1;
    toSrc.subresourceRange.layerCount = 1;
    toSrc.srcAccessMask = 0;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toSrc);

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = W;
    region.bufferImageHeight = H;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent = {W, H, 1};
    vkCmdCopyImageToBuffer(cmd, vkImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vkBuf, 1, &region);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    checkVk(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
    vkQueueWaitIdle(queue);
    LOG("  [ok] Vulkan 拷贝完成\n");

    pUnlockObjects(interop, 1, &reg);

    void* data = nullptr;
    checkVk(vkMapMemory(dev, bufMem, 0, bufSize, 0, &data), "vkMapMemory");
    auto px = static_cast<unsigned char*>(data) + (100 * W + 100) * 4;
    unsigned char r_ = px[0], g_ = px[1], b_ = px[2], a_ = px[3];
    LOG("\n  Vulkan 读回像素 (100,100) = R=%u G=%u B=%u A=%u\n", r_, g_, b_, a_);
    LOG("  GL 写入的颜色            = R=200 G=40 B=60 A=255\n\n");

    bool ok = (r_ == 200 && g_ == 40 && b_ == 60);
    LOG("=========================================\n");
    LOG("  %s\n", ok ? "通过：GL -> Vulkan 桥接可用" : "不通过：颜色对不上");
    LOG("=========================================\n");

    vkUnmapMemory(dev, bufMem);
    vkDestroyBuffer(dev, vkBuf, nullptr);
    vkFreeMemory(dev, bufMem, nullptr);
    vkFreeMemory(dev, vkMem, nullptr);
    vkDestroyImage(dev, vkImg, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroyInstance(inst, nullptr);
    (void)glCtx;
    (void)vkEnumerateInstanceExtensionProperties;
    (void)vkGetPhysicalDeviceImageFormatProperties2;
    (void)vkDestroyBuffer;
    (void)vkDestroyImage;
    (void)vkDestroyDevice;

    return ok ? 0 : 2;
}
