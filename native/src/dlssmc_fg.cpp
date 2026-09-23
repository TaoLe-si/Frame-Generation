// DLSS 帧生成 - 原生层
//
// Minecraft Java 在 Windows 上是 OpenGL，而 DLSS 帧生成（sl.dlss_g）只支持 D3D12 / Vulkan，
// 并且必须接管 swapchain 才能插帧。所以这里的做法是：
//
//   1. 在 MC 的专用子窗口上自建 Vulkan swapchain，接管上屏
//   2. GL 渲染出的纹理经 WGL_NV_DX_interop2 做成 D3D11 共享纹理（KMT 句柄），
//      再用 VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT 导入 Vulkan
//   3. 每帧把最终画面 blit 进 swapchain 图像，给 Streamline 打 Hudless/Depth/Mvec 标签
//   4. Streamline 在 present 时插入生成帧
//
// 三个已验证的关键点（都在 native/test/ 里有独立验证程序）：
//   - 只有 D3D11_RESOURCE_MISC_SHARED（KMT）能与 GL 互操作；D3D12 直接打开这张 KMT 读不到 GL 写的
//     内容，必须 D3D11 内部 CopyResource 到一张跨 API(SHARED_NTHANDLE) 纹理再交给 D3D12
//     （delivery_probe.exe 实测：一帧 4 个 1080p 输入约 0.3 ms，且必须 Flush + fence 同步）
//   - sl.dlss_g 依赖 sl.reflex，featuresToLoad 必须一起请求
//   - 链接 sl.interposer.lib 时 vk* 由它代理，不要再调 slSetVulkanInfo
//
// 编译：native/build_fg.sh

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <winver.h>
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

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------ GL / WGL
#define DLSSMC_GL_TEXTURE_2D 0x0DE1
#define WGL_ACCESS_READ_WRITE_NV 0x0001

using GLuint = unsigned int;
using GLenum = unsigned int;

using PFN_wglGetProcAddress = void*(WINAPI*)(LPCSTR);
using PFN_wglDXOpenDeviceNV = HANDLE(WINAPI*)(void*);
using PFN_wglDXRegisterObjectNV = HANDLE(WINAPI*)(HANDLE, void*, GLuint, GLenum, GLenum);
using PFN_wglDXUnregisterObjectNV = BOOL(WINAPI*)(HANDLE, HANDLE);
using PFN_wglDXLockObjectsNV = BOOL(WINAPI*)(HANDLE, int, HANDLE*);
using PFN_wglDXUnlockObjectsNV = BOOL(WINAPI*)(HANDLE, int, HANDLE*);
using PFN_wglDXCloseDeviceNV = BOOL(WINAPI*)(HANDLE);

static PFN_wglDXOpenDeviceNV pOpenDevice = nullptr;
static PFN_wglDXRegisterObjectNV pRegisterObject = nullptr;
static PFN_wglDXUnregisterObjectNV pUnregisterObject = nullptr;
static PFN_wglDXLockObjectsNV pLockObjects = nullptr;
static PFN_wglDXUnlockObjectsNV pUnlockObjects = nullptr;
static PFN_wglDXCloseDeviceNV pCloseDevice = nullptr;

// ------------------------------------------------------------------ 日志
static FILE* g_log = nullptr;

// 每帧路径上的日志会拖慢渲染（带 fflush 的磁盘 IO 在渲染线程上），
// 所以做成"变化时才记 + 每 N 帧记一次"
static int g_lastTagErr = -999;
static int g_lastStatus = -999;
static uint32_t g_logCounter = 0;

static void logf(const char* fmt, ...) {
    if (!g_log) return;
    va_list a;
    va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    fflush(g_log);
    va_end(a);
}

// 只在错误码变化、或每 600 帧一次时记录
#define LOG_FRAME_ERR(lastVar, code, fmt, ...)                       \
    do {                                                             \
        if ((code) != (lastVar) || (g_logCounter % 600) == 0) {      \
            (lastVar) = (code);                                      \
            logf(fmt, __VA_ARGS__);                                  \
        }                                                            \
    } while (0)

static std::wstring utf8ToWide(const char* s) {
    if (!s) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (len <= 0) return L"";
    std::wstring out((size_t)(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), len);
    return out;
}

// ------------------------------------------------------------------ 被导入的一张 GL 纹理
struct ImportedTexture {
    GLuint glTex = 0;
    ComPtr<ID3D11Texture2D> d3d11Tex;
    HANDLE kmt = nullptr;
    HANDLE registered = nullptr;

    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;

    uint32_t width = 0;
    uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageUsageFlags usage = 0;

    bool valid() const { return registered && image && memory && view; }
};

// ------------------------------------------------------------------ 上下文

// 多帧并行：每帧一个命令缓冲 + fence + 信号量槽位，避免每帧全量等待 GPU 卡住流水线
static const uint32_t kFramesInFlight = 3;

struct FgContext {
    bool slInited = false;
    bool ready = false;

    HWND hwnd = nullptr;
    HWND presentationWindow = nullptr;
    bool presentationVisible = false;
    uint32_t width = 0, height = 0;

    // D3D11 + 互操作
    ComPtr<ID3D11Device> d3d11;
    ComPtr<ID3D11DeviceContext> d3d11Ctx;
    HANDLE interop = nullptr;

    // Vulkan
    VkInstance inst = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapImages;
    VkFormat swapFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};

    VkCommandPool pool = VK_NULL_HANDLE;

    // 多帧并行：每帧一个命令缓冲 + fence 槽位，避免每帧全量等待 GPU
    VkCommandBuffer cmdRing[kFramesInFlight] = {};
    VkFence fenceRing[kFramesInFlight] = {};
    VkSemaphore renderDoneRing[kFramesInFlight] = {};
    VkSemaphore imageAvailRing[kFramesInFlight] = {};
    bool fencePending[kFramesInFlight] = {};
    uint32_t frameSlot = 0;

    // 导入的纹理
    ImportedTexture finalTex;    // 最终画面（世界+手持+HUD）
    ImportedTexture hudlessTex;  // 画 HUD 之前的画面
    ImportedTexture depthTex;
    ImportedTexture motionTex;
    ImportedTexture worldTex;
    ImportedTexture srOutTex;

    // SR shares the Vulkan device, viewport and frame token with FG.
    int srQuality = -1;
    float srSharpness = 0.0f;
    int srPreset = 0;
    bool srOptionsSet = false;
    uint32_t srOutputWidth = 0, srOutputHeight = 0;
    bool srResources = false;
    bool outLocked = false;
    bool constantsSet = false;
    bool srEvaluated = false;
    bool fgEnabled = true; // Requested state survives texture rebind/resize.
    VkResult (*commonPresent)(VkQueue, const VkPresentInfoKHR*, bool&) = nullptr;
    VkResult (*commonAfterPresent)() = nullptr;

    // DLSS-G
    sl::ViewportHandle viewport = sl::ViewportHandle(0u);
    sl::FrameToken* token = nullptr;
    VkFence lastSubmitFence = VK_NULL_HANDLE;
    VkSemaphore inputsFence = VK_NULL_HANDLE;
    uint64_t inputsFenceValue = 0;
    uint32_t frameIndex = 0;
    uint32_t framesToGenerate = 1;
    bool dlssGOn = false;
    // 已下发给 DLSS-G 的选项快照：SL 明确警告重复下发是冗余甚至与 Present 竞争，
    // 所以只在选项真的变化（档位 / flag / 尺寸 / 格式）时才再调一次
    bool optsSent = false;
    sl::DLSSGOptions sentOpt{};
    bool reflexBoost = true;
    uint32_t reflexFpsLimit = 0;

    // DLSS-G 底层开关，由 Java 侧配置逐帧下发
    bool fgShowOnlyInterpolated = false;
    bool fgRetainResources = true;
    bool fgMenuDetection = true;
    bool fgQueueParallelism = true;
    bool fgUiRecomposition = false;

    // Reflex 回报的实测延迟分段（毫秒）
    bool latValid = false;
    double latTotal = 0, latSim = 0, latSubmit = 0, latQueue = 0, latGpu = 0, latPresent = 0;
    uint64_t latRawFirst = 0;

    // 桥接路径自身的分段耗时（毫秒，CPU 侧墙钟）
    double tSlotWait = 0, tAcquire = 0, tSubmit = 0, tPresent = 0, tState = 0;
    double tWaitSubmit = 0, tWaitInputs = 0, tDxLock = 0;
    uint32_t frameCounter = 0;
    int blackFrames = 0;
    bool takeoverDisabled = false;
    bool locked = false;

    // 诊断用回读缓冲
    VkBuffer probeBuffer = VK_NULL_HANDLE;
    VkDeviceMemory probeMemory = VK_NULL_HANDLE;
    bool needReset = true;
    uint32_t lastPresentedCount = 0;

    int lastError = 0;
};

static FgContext g_ctx;

// 扩展函数要经 vkGetDeviceProcAddr 取，interposer 只导出核心入口
static PFN_vkGetMemoryWin32HandlePropertiesKHR pGetMemWin32HandleProps = nullptr;

static void logSl(sl::LogType type, const char* msg) {
    if (type == sl::LogType::eError || type == sl::LogType::eWarn) {
        logf("[SL/%s] %s\n", type == sl::LogType::eError ? "ERR" : "WRN", msg);
    }
}

// ------------------------------------------------------------------ Vulkan 小工具
#define VK_FAILED(x) ((x) != VK_SUCCESS)

static bool hasDeviceExt(VkPhysicalDevice phys, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(phys, nullptr, &n, exts.data());
    for (auto& e : exts)
        if (strcmp(e.extensionName, name) == 0) return true;
    return false;
}

// ------------------------------------------------------------------ 初始化
// 插件目录里那份模型 DLL 的文件版本，用于在界面上标出实际生效的 DLSS 版本
static std::wstring g_pluginDir;

static std::string fileVersionOf(const std::wstring& path) {
    DWORD size = GetFileVersionInfoSizeW(path.c_str(), nullptr);
    if (!size) return "取不到";
    std::vector<unsigned char> buf(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return "取不到";
    VS_FIXEDFILEINFO* ffi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&ffi), &len) || !len)
        return "取不到";
    char out[40];
    snprintf(out, sizeof(out), "%u.%u.%u.%u", HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
             HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
    return out;
}

// ------------------------------------------------------------------ MFG 解锁（实验）
//
// 官方多帧生成（3x 以上）在 NGX 里被 **两处** 架构判断挡住，两处都要改：
//
//  (A) 上报上限。写 DLSSG.MultiFrameCountMax 的那段：
//        cmp  ebp, <架构阈值>       ; 81 FD imm32
//        jl   .max_one              ; 0F 8C rel32
//        mov  edi, 5                ; 支持 MFG 的架构 → 上限 5（即 6x）
//      .max_one:
//        mov  edi, 1                ; BF 01 00 00 00
//      改掉它，slDLSSGGetState 才会把 numFramesToGenerateMax 报成 5，
//      slDLSSGSetOptions 才肯接受 >1 的请求。
//
//  (B) 设备能力标志。这个才是真正拦住执行的那道：
//        call <GetGPUArchitecture>
//        cmp  eax, 0x1b0            ; 3D imm32
//        setae al                   ; 0F 93 C0
//        mov  byte ptr [rdi+0x28], al   ; 88 47 28
//      该字节是「本设备是否支持多帧」，随后 EndpointCoreInputs::ComputeAndValidateTimeFactor
//      读它：为 0 且 numFramesToGenerate > 1 就直接返回 0xbad00005（InvalidParameter），
//      并在日志里说 "Multi frame is not supported on this device"。
//      只改 (A) 不改 (B)，就会看到「上限报 5、选项被接受、但每帧 evaluate 全失败」。
//
// 处理方式：把 (A) 的 jl 换成 NOP（放行高阈值分支），把 (B) 的
// `cmp eax, imm32` 换成 `cmp eax, eax` + NOP（eax 恒等于自身 → setae 得 1）。
// 只改进程内的映像，不动磁盘文件；未开启时连 LoadLibrary 都不做。
static bool g_mfgUnlock = false;
static bool g_mfgPatched = false;
static std::string g_mfgUnlockReport = "未启用";

// Streamline 会从插件目录加载这份模型 DLL；先按模块名找，找不到再按路径加载，
// 保证改的是进程里真正会被 NGX 用到的那一份映像。
static HMODULE resolveNvngxDlssg() {
    if (HMODULE m = GetModuleHandleW(L"nvngx_dlssg.dll")) return m;
    if (g_pluginDir.empty()) return nullptr;
    std::wstring p = g_pluginDir + L"\\nvngx_dlssg.dll";
    return LoadLibraryExW(p.c_str(), nullptr,
                          LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
}

static bool protectAndWrite(void* addr, const unsigned char* bytes, size_t n, std::string& report) {
    DWORD old = 0;
    if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &old)) {
        report = "VirtualProtect 失败 " + std::to_string(GetLastError());
        return false;
    }
    memcpy(addr, bytes, n);
    VirtualProtect(addr, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, n);
    return true;
}

static bool patchMfgArchGate(HMODULE mod, std::string& report) {
    if (!mod) {
        report = "找不到 nvngx_dlssg.dll";
        return false;
    }
    auto base = reinterpret_cast<const unsigned char*>(mod);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        report = "不是有效 PE";
        return false;
    }
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        report = "不是有效 PE";
        return false;
    }

    // 逐节找 .text：只在这段里扫，避免误命中数据。
    // 先把两处都定位到，再一起改——只改 (A) 不改 (B) 会更糟：
    // 档位被接受、每帧却被 NGX 顶回，用户看到的是「开了 4x 但 FG 全废」。
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    struct Site { unsigned char* addr; size_t len; uint32_t arch; };
    std::vector<Site> sitesA;
    std::vector<Site> sitesB;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (strncmp(reinterpret_cast<const char*>(sec->Name), ".text", 5) != 0) continue;
        auto text = const_cast<unsigned char*>(base + sec->VirtualAddress);
        size_t size = sec->Misc.VirtualSize;
        if (size < 16) continue;

        for (size_t p = 0; p + 17 <= size; ++p) {
            // ---- (A) 上报上限：81 FD imm32 / 0F 8C rel32 → mov edi,1 ----
            if (text[p] == 0x81 && text[p + 1] == 0xFD) {
                uint32_t arch = 0;
                memcpy(&arch, text + p + 2, 4);
                if (arch >= 0x180 && arch <= 0x1FF &&
                    text[p + 6] == 0x0F && text[p + 7] == 0x8C) {
                    int32_t rel = 0;
                    memcpy(&rel, text + p + 8, 4);
                    if (rel > 0) {
                        size_t target = p + 12 + static_cast<size_t>(rel);
                        if (target + 5 <= size &&
                            text[target] == 0xBF && text[target + 1] == 0x01 &&
                            text[target + 2] == 0x00 && text[target + 3] == 0x00 &&
                            text[target + 4] == 0x00) {
                            sitesA.push_back({text + p + 6, 6, arch});
                        }
                    }
                }
            }

            // ---- (B) 设备能力字节：0F 93 C0 88 47 28（setae al ; mov [rdi+0x28], al）----
            // 往前退到 cmp 的 opcode（3D imm32 或 81 F8 imm32 两种编码都认）
            if (text[p] == 0x0F && text[p + 1] == 0x93 && text[p + 2] == 0xC0 &&
                text[p + 3] == 0x88 && text[p + 4] == 0x47 && text[p + 5] == 0x28) {
                if (p >= 5 && text[p - 5] == 0x3D) sitesB.push_back({text + p - 5, 5, 0});
                else if (p >= 6 && text[p - 6] == 0x81 && text[p - 5] == 0xF8)
                    sitesB.push_back({text + p - 6, 6, 0});
            }
        }
    }

    if (sitesA.empty() || sitesB.empty()) {
        report = "没找到多帧门禁（驱动或 SDK 版本可能变了），未做任何修改";
        return false;
    }

    // 6 字节 NOP：nop dword ptr [rax+rax*1+0] + nop
    const unsigned char nop6[6] = {0x0F, 0x1F, 0x44, 0x00, 0x00, 0x90};
    for (auto& s : sitesA) {
        if (!protectAndWrite(s.addr, nop6, s.len, report)) return false;
        logf("[dlssmc] MFG 解锁 A：上报上限门禁已改，阈值 0x%x\n", s.arch);
    }
    for (auto& s : sitesB) {
        // cmp eax, eax（39 C0）+ NOP：eax 恒等于自身，setae 必然得 1
        const unsigned char cmpSelf[6] = {0x39, 0xC0, 0x90, 0x90, 0x90, 0x90};
        if (!protectAndWrite(s.addr, cmpSelf, s.len, report)) return false;
        logf("[dlssmc] MFG 解锁 B：设备多帧能力字节已改\n");
    }
    report = "已解锁多帧（上报上限 " + std::to_string(sitesA.size()) + " 处 + 设备能力 "
            + std::to_string(sitesB.size()) + " 处）";
    return true;
}

static bool initStreamline(const std::wstring& pluginPath, const std::wstring& logPath) {
    if (g_ctx.slInited) return true;

    // 必须在 slInit 之前：那时 sl.common 才会去加载 nvngx_dlssg.dll。
    // 我们先把它拉进进程并改掉门禁，Streamline 随后拿到的是同一份映像。
    if (g_mfgUnlock && !g_mfgPatched) {
        HMODULE nvngx = resolveNvngxDlssg();
        g_mfgPatched = patchMfgArchGate(nvngx, g_mfgUnlockReport);
        logf("[dlssmc] MFG 解锁：%s\n", g_mfgUnlockReport.c_str());
    }

    sl::Preferences pref{};
    pref.showConsole = false;
    pref.renderAPI = sl::RenderAPI::eVulkan;
    pref.applicationId = 231313132u; // 占位，正式发布需换成 NVIDIA 发放的 ID
    pref.engine = sl::EngineType::eCustom;
    pref.logMessageCallback = logSl;
    // 必须带上 eUseFrameBasedResourceTagging，否则 slSetTagForFrame 会直接拒绝
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                 sl::PreferenceFlags::eAllowOTA |
                 sl::PreferenceFlags::eLoadDownloadedPlugins |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;

    const wchar_t* paths[1] = {pluginPath.c_str()};
    if (!pluginPath.empty()) {
        pref.pathsToPlugins = paths;
        pref.numPathsToPlugins = 1;
    }
    if (!logPath.empty()) pref.pathToLogsAndData = logPath.c_str();

    // DLSS-G requires Reflex. SR (including SR-only) uses this same Vulkan instance;
    // a Streamline instance initialized by the old D3D11 path cannot be reused.
    sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL,
                              sl::kFeatureDLSS};
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 4;

    auto r = slInit(pref, sl::kSDKVersion);
    logf("[dlssmc] slInit -> %d\n", (int)r);
    if (r != sl::Result::eOk) return false;
    g_ctx.slInited = true;
    return true;
}

static bool initInteropApi() {
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    if (!gl) return false;
    auto getProc = reinterpret_cast<PFN_wglGetProcAddress>(GetProcAddress(gl, "wglGetProcAddress"));
    if (!getProc) return false;
    pOpenDevice = reinterpret_cast<PFN_wglDXOpenDeviceNV>(getProc("wglDXOpenDeviceNV"));
    pRegisterObject = reinterpret_cast<PFN_wglDXRegisterObjectNV>(getProc("wglDXRegisterObjectNV"));
    pUnregisterObject =
        reinterpret_cast<PFN_wglDXUnregisterObjectNV>(getProc("wglDXUnregisterObjectNV"));
    pLockObjects = reinterpret_cast<PFN_wglDXLockObjectsNV>(getProc("wglDXLockObjectsNV"));
    pUnlockObjects = reinterpret_cast<PFN_wglDXUnlockObjectsNV>(getProc("wglDXUnlockObjectsNV"));
    pCloseDevice = reinterpret_cast<PFN_wglDXCloseDeviceNV>(getProc("wglDXCloseDeviceNV"));
    return pOpenDevice && pRegisterObject && pLockObjects && pUnlockObjects;
}

static bool createSwapchain();

static LRESULT CALLBACK presentationWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(window, message, wParam, lParam);
}

static bool createPresentationSurface() {
    // A Vulkan present leaves the parent HWND's GL output stale even after surface destruction.
    WNDCLASSW wc{};
    wc.lpfnWndProc = presentationWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"dlssmc-fg-presentation";
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    RECT client{};
    if (!GetClientRect(g_ctx.hwnd, &client)) return false;
    g_ctx.presentationWindow = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"",
        WS_CHILD, 0, 0, client.right, client.bottom,
        g_ctx.hwnd, nullptr, wc.hInstance, nullptr);
    if (!g_ctx.presentationWindow) return false;

    VkWin32SurfaceCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    sci.hinstance = wc.hInstance;
    sci.hwnd = g_ctx.presentationWindow;
    VkResult result = vkCreateWin32SurfaceKHR(g_ctx.inst, &sci, nullptr, &g_ctx.surface);
    if (result != VK_SUCCESS) {
        logf("[dlssmc] vkCreateWin32SurfaceKHR -> %d\n", (int)result);
        DestroyWindow(g_ctx.presentationWindow);
        g_ctx.presentationWindow = nullptr;
        return false;
    }
    return true;
}

static bool initVulkan() {
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "dlssmc";
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
    if (VK_FAILED(vkCreateInstance(&ci, nullptr, &g_ctx.inst))) {
        logf("[dlssmc] vkCreateInstance 失败\n");
        return false;
    }

    if (!createPresentationSurface()) return false;

    uint32_t gpuCount = 0;
    vkEnumeratePhysicalDevices(g_ctx.inst, &gpuCount, nullptr);
    if (gpuCount == 0) return false;
    std::vector<VkPhysicalDevice> gpus(gpuCount);
    vkEnumeratePhysicalDevices(g_ctx.inst, &gpuCount, gpus.data());
    g_ctx.phys = gpus[0];

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(g_ctx.phys, &props);
    logf("[dlssmc] Vulkan GPU: %s\n", props.deviceName);

    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g_ctx.phys, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(g_ctx.phys, &qfCount, qfs.data());

    int family = -1;
    for (uint32_t i = 0; i < qfCount; ++i) {
        VkBool32 present = VK_FALSE;
        vkGetPhysicalDeviceSurfaceSupportKHR(g_ctx.phys, i, g_ctx.surface, &present);
        if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
            family = (int)i;
            break;
        }
    }
    if (family < 0) {
        logf("[dlssmc] 找不到图形+呈现队列族\n");
        return false;
    }
    g_ctx.queueFamily = (uint32_t)family;

    const char* wantDev[] = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_MAINTENANCE_4_EXTENSION_NAME,
        VK_NV_OPTICAL_FLOW_EXTENSION_NAME,
        VK_NVX_BINARY_IMPORT_EXTENSION_NAME,
        VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME,
    };
    std::vector<const char*> devExts;
    for (auto n : wantDev) {
        if (hasDeviceExt(g_ctx.phys, n)) {
            devExts.push_back(n);
        } else {
            logf("[dlssmc] 设备缺少扩展 %s\n", n);
        }
    }

    uint32_t wantQueues = qfs[family].queueCount >= 3 ? 3 : qfs[family].queueCount;
    float prio[3] = {1.0f, 1.0f, 1.0f};
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g_ctx.queueFamily;
    qci.queueCount = wantQueues;
    qci.pQueuePriorities = prio;

    VkPhysicalDeviceVulkan12Features f12{};
    f12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    f12.timelineSemaphore = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13{};
    f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
    f13.maintenance4 = VK_TRUE;
    f12.pNext = &f13;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f12;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)devExts.size();
    dci.ppEnabledExtensionNames = devExts.data();
    if (VK_FAILED(vkCreateDevice(g_ctx.phys, &dci, nullptr, &g_ctx.dev))) {
        logf("[dlssmc] vkCreateDevice 失败\n");
        return false;
    }
    vkGetDeviceQueue(g_ctx.dev, g_ctx.queueFamily, 0, &g_ctx.queue);

    pGetMemWin32HandleProps = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
        vkGetDeviceProcAddr(g_ctx.dev, "vkGetMemoryWin32HandlePropertiesKHR"));
    if (!pGetMemWin32HandleProps) {
        logf("[dlssmc] 取不到 vkGetMemoryWin32HandlePropertiesKHR\n");
        return false;
    }

    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = g_ctx.queueFamily;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(g_ctx.dev, &cpi, nullptr, &g_ctx.pool) != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo cbai{};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = g_ctx.pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = kFramesInFlight;
    if (vkAllocateCommandBuffers(g_ctx.dev, &cbai, g_ctx.cmdRing) != VK_SUCCESS) return false;

    VkSemaphoreCreateInfo semi{};
    semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (vkCreateSemaphore(g_ctx.dev, &semi, nullptr, &g_ctx.renderDoneRing[i]) != VK_SUCCESS ||
            vkCreateSemaphore(g_ctx.dev, &semi, nullptr, &g_ctx.imageAvailRing[i]) != VK_SUCCESS ||
            vkCreateFence(g_ctx.dev, &fci, nullptr, &g_ctx.fenceRing[i]) != VK_SUCCESS) return false;
    }

    logf("[dlssmc] Vulkan 设备就绪（队列族 %u，%u 个队列）\n", g_ctx.queueFamily, wantQueues);
    return true;
}

static sl::Result setReflexOptions(bool boost, uint32_t fpsLimit) {
    if (fpsLimit > 1000) return sl::Result::eErrorInvalidParameter;
    sl::ReflexOptions options{};
    // DLSS-G requires baseline Reflex even when Boost is disabled.
    options.mode = boost ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency;
    options.frameLimitUs = fpsLimit == 0 ? 0 : 1000000u / fpsLimit;
    auto result = slReflexSetOptions(options);
    logf("[dlssmc] slReflexSetOptions(%s, fpsLimit=%u, frameLimitUs=%u) -> %d\n",
         boost ? "lowLatencyWithBoost" : "lowLatency", fpsLimit, options.frameLimitUs, (int)result);
    if (result == sl::Result::eOk) {
        g_ctx.reflexBoost = boost;
        g_ctx.reflexFpsLimit = fpsLimit;
    }
    return result;
}

static bool createSwapchain() {
    if (!g_ctx.surface && !createPresentationSurface()) return false;
    // 子窗口必须跟着父窗口客户区走：它只创建时定过一次尺寸，之后改分辨率或
    // 切全屏/窗口都不会自动变。尺寸不一致时 surface 一直是 SUBOPTIMAL，
    // 而 present 把 SUBOPTIMAL 当成「要重建」返回 1，Java 侧就会每帧
    // 销毁重建交换链 + 重导入纹理 —— 表现为满屏卡顿。
    if (g_ctx.presentationWindow) {
        RECT client{};
        if (GetClientRect(g_ctx.hwnd, &client)) {
            int cw = client.right - client.left, ch = client.bottom - client.top;
            RECT cur{};
            if (GetClientRect(g_ctx.presentationWindow, &cur) &&
                ((cur.right - cur.left) != cw || (cur.bottom - cur.top) != ch)) {
                SetWindowPos(g_ctx.presentationWindow, nullptr, 0, 0, cw, ch,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_ctx.phys, g_ctx.surface, &caps);

    // 优先 R8G8B8A8，方便与导入的 GL RGBA8 图像直接 copy
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_ctx.phys, g_ctx.surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_ctx.phys, g_ctx.surface, &fmtCount, fmts.data());

    VkSurfaceFormatKHR chosen = fmts.empty()
                                    ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                                         VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                    : fmts[0];
    for (auto& f : fmts) {
        if (f.format == VK_FORMAT_R8G8B8A8_UNORM &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = f;
            break;
        }
    }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) extent = {g_ctx.width, g_ctx.height};

    VkSwapchainCreateInfoKHR sci{};
    sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    sci.surface = g_ctx.surface;
    sci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && sci.minImageCount > caps.maxImageCount)
        sci.minImageCount = caps.maxImageCount;
    sci.imageFormat = chosen.format;
    sci.imageColorSpace = chosen.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR; // DLSS-G 要求关垂直同步
    sci.clipped = VK_TRUE;

    if (VK_FAILED(vkCreateSwapchainKHR(g_ctx.dev, &sci, nullptr, &g_ctx.swapchain))) {
        logf("[dlssmc] vkCreateSwapchainKHR 失败\n");
        return false;
    }

    uint32_t n = 0;
    VkResult ir = vkGetSwapchainImagesKHR(g_ctx.dev, g_ctx.swapchain, &n, nullptr);
    if (ir != VK_SUCCESS || n == 0) {
        RECT rc{};
        GetClientRect(g_ctx.hwnd, &rc);
        logf("[dlssmc] vkGetSwapchainImagesKHR 第一问 -> %d，n=%u\n", (int)ir, n);
        logf("[dlssmc]   窗口客户区 %ldx%ld，请求 extent %ux%u，caps.currentExtent %ux%u "
             "(minImageCount=%u maxImageCount=%u)\n",
             rc.right - rc.left, rc.bottom - rc.top, extent.width, extent.height,
             caps.currentExtent.width, caps.currentExtent.height, caps.minImageCount,
             caps.maxImageCount);
        if (n == 0) {
            vkDestroySwapchainKHR(g_ctx.dev, g_ctx.swapchain, nullptr);
            g_ctx.swapchain = VK_NULL_HANDLE;
            return false;
        }
    }
    g_ctx.swapImages.resize(n);
    ir = vkGetSwapchainImagesKHR(g_ctx.dev, g_ctx.swapchain, &n, g_ctx.swapImages.data());
    if (ir != VK_SUCCESS || n == 0) {
        logf("[dlssmc] vkGetSwapchainImagesKHR 第二问 -> %d，n=%u\n", (int)ir, n);
        vkDestroySwapchainKHR(g_ctx.dev, g_ctx.swapchain, nullptr);
        g_ctx.swapchain = VK_NULL_HANDLE;
        return false;
    }
    g_ctx.swapImages.resize(n);

    g_ctx.swapFormat = chosen.format;
    g_ctx.extent = extent;
    g_ctx.width = extent.width;
    g_ctx.height = extent.height;
    logf("[dlssmc] swapchain %ux%u，格式 %d，%u 张图\n", extent.width, extent.height,
         (int)chosen.format, n);
    return setReflexOptions(g_ctx.reflexBoost, g_ctx.reflexFpsLimit) == sl::Result::eOk;
}

// ------------------------------------------------------------------ 纹理导入
static void releaseImported(ImportedTexture& t) {
    if (t.registered && g_ctx.interop) pUnregisterObject(g_ctx.interop, t.registered);
    t.registered = nullptr;
    if (t.view) vkDestroyImageView(g_ctx.dev, t.view, nullptr);
    t.view = VK_NULL_HANDLE;
    if (t.image) vkDestroyImage(g_ctx.dev, t.image, nullptr);
    t.image = VK_NULL_HANDLE;
    if (t.memory) vkFreeMemory(g_ctx.dev, t.memory, nullptr);
    t.memory = VK_NULL_HANDLE;
    t.d3d11Tex.Reset();
    t.kmt = nullptr;
    t.glTex = 0;
    t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

// GL 纹理 -> D3D11 共享纹理(KMT) -> Vulkan 图像
static bool importGlTexture(ImportedTexture& t, GLuint glTex, uint32_t w, uint32_t h,
                            VkFormat vkFormat, bool storage = false) {
    releaseImported(t);
    t.glTex = glTex;
    t.width = w;
    t.height = h;
    t.format = vkFormat;
    t.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              VK_IMAGE_USAGE_SAMPLED_BIT;
    if (storage) t.usage |= VK_IMAGE_USAGE_STORAGE_BIT;

    // 1) D3D11 共享纹理（只有 SHARED 这一组能与 GL 互操作）
    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = vkFormat == VK_FORMAT_R32_SFLOAT ? DXGI_FORMAT_R32_FLOAT :
                vkFormat == VK_FORMAT_R16G16_SFLOAT ? DXGI_FORMAT_R16G16_FLOAT :
                DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (storage) td.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(g_ctx.d3d11->CreateTexture2D(&td, nullptr, t.d3d11Tex.GetAddressOf()))) {
        logf("[dlssmc] CreateTexture2D 失败 (gl=%u %ux%u)\n", glTex, w, h);
        return false;
    }

    // 2) 注册给 GL（注意：注册后 GL 必须锁定了才能写）
    t.registered = pRegisterObject(g_ctx.interop, t.d3d11Tex.Get(), glTex, DLSSMC_GL_TEXTURE_2D,
                                   WGL_ACCESS_READ_WRITE_NV);
    if (!t.registered) {
        logf("[dlssmc] wglDXRegisterObjectNV 失败 (gl=%u)\n", glTex);
        return false;
    }

    // 3) 取 KMT 句柄
    ComPtr<IDXGIResource> dxgiRes;
    if (FAILED(t.d3d11Tex.As(&dxgiRes)) || FAILED(dxgiRes->GetSharedHandle(&t.kmt))) {
        logf("[dlssmc] 取 KMT 句柄失败\n");
        return false;
    }

    // 4) 在 Vulkan 侧建外部内存图像
    VkExternalMemoryImageCreateInfo extImg{};
    extImg.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;

    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &extImg;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = vkFormat;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = t.usage;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (VK_FAILED(vkCreateImage(g_ctx.dev, &ici, nullptr, &t.image))) {
        logf("[dlssmc] vkCreateImage 失败\n");
        return false;
    }

    VkMemoryRequirements memReq{};
    vkGetImageMemoryRequirements(g_ctx.dev, t.image, &memReq);

    VkMemoryWin32HandlePropertiesKHR hp{};
    hp.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
    if (VK_FAILED(pGetMemWin32HandleProps(
            g_ctx.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, t.kmt, &hp))) {
        logf("[dlssmc] vkGetMemoryWin32HandlePropertiesKHR 失败\n");
        return false;
    }

    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(g_ctx.phys, &mp);
    uint32_t allowed = memReq.memoryTypeBits & hp.memoryTypeBits;
    uint32_t idx = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if ((allowed & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            idx = i;
            break;
        }
    }
    if (idx == UINT32_MAX) {
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if (allowed & (1u << i)) {
                idx = i;
                break;
            }
        }
    }
    if (idx == UINT32_MAX) {
        logf("[dlssmc] 找不到兼容 memory type\n");
        return false;
    }

    VkImportMemoryWin32HandleInfoKHR imp{};
    imp.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT;
    imp.handle = t.kmt;

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &imp;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = idx;
    if (VK_FAILED(vkAllocateMemory(g_ctx.dev, &mai, nullptr, &t.memory)) ||
        VK_FAILED(vkBindImageMemory(g_ctx.dev, t.image, t.memory, 0))) {
        logf("[dlssmc] 导入 KMT 内存失败\n");
        return false;
    }

    VkImageViewCreateInfo vci{};
    vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vci.image = t.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = vkFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (VK_FAILED(vkCreateImageView(g_ctx.dev, &vci, nullptr, &t.view))) {
        logf("[dlssmc] vkCreateImageView 失败\n");
        return false;
    }

    logf("[dlssmc] 导入纹理 gl=%u %ux%u KMT=%p OK\n", glTex, w, h, t.kmt);
    return true;
}

// ------------------------------------------------------------------ 像素回读诊断
// 回读 finalTex / hudlessTex 的中心像素，确认 GL 画面到底有没有经桥传过来。
// 全 0 说明互操作那一步没生效；非 0 说明画面到了，问题在别处。
static void probeSources(VkCommandBuffer cmd) {
    if (g_ctx.probeBuffer == VK_NULL_HANDLE) {
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = 8; // 两张图各 4 字节
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (VK_FAILED(vkCreateBuffer(g_ctx.dev, &bci, nullptr, &g_ctx.probeBuffer))) return;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(g_ctx.dev, g_ctx.probeBuffer, &req);
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(g_ctx.phys, &mp);
        uint32_t idx = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i)) &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
                idx = i;
                break;
            }
        }
        if (idx == UINT32_MAX) return;
        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = idx;
        if (VK_FAILED(vkAllocateMemory(g_ctx.dev, &mai, nullptr, &g_ctx.probeMemory))) return;
        vkBindBufferMemory(g_ctx.dev, g_ctx.probeBuffer, g_ctx.probeMemory, 0);
    }

    if (g_ctx.finalTex.valid()) {
        VkBufferImageCopy rg{};
        rg.bufferOffset = 0;
        rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rg.imageExtent = {1, 1, 1};
        rg.imageOffset = {(int32_t)(g_ctx.width / 2), (int32_t)(g_ctx.height / 2), 0};
        vkCmdCopyImageToBuffer(cmd, g_ctx.finalTex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               g_ctx.probeBuffer, 1, &rg);
    }
    if (g_ctx.hudlessTex.valid()) {
        VkBufferImageCopy rg{};
        rg.bufferOffset = 4;
        rg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rg.imageExtent = {1, 1, 1};
        rg.imageOffset = {(int32_t)(g_ctx.width / 2), (int32_t)(g_ctx.height / 2), 0};
        vkCmdCopyImageToBuffer(cmd, g_ctx.hudlessTex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               g_ctx.probeBuffer, 1, &rg);
    }
}

// 返回 false 表示源画面是黑的（互操作没生效）
static bool readProbeBuffer() {
    if (g_ctx.probeBuffer == VK_NULL_HANDLE || g_ctx.probeMemory == VK_NULL_HANDLE) return true;
    void* p = nullptr;
    if (VK_FAILED(vkMapMemory(g_ctx.dev, g_ctx.probeMemory, 0, 8, 0, &p)) || !p) return true;
    auto b = static_cast<unsigned char*>(p);
    logf("[dlssmc] 回读 final=(%u,%u,%u,%u) hudless=(%u,%u,%u,%u)\n", b[0], b[1], b[2], b[3], b[4],
         b[5], b[6], b[7]);
    bool blackFinal = (b[0] + b[1] + b[2] + b[3]) == 0;
    bool blackHudless = (b[4] + b[5] + b[6] + b[7]) == 0;
    vkUnmapMemory(g_ctx.dev, g_ctx.probeMemory);

    if (blackFinal && blackHudless) {
        g_ctx.blackFrames++;
    } else {
        g_ctx.blackFrames = 0;
    }
    return !(blackFinal && blackHudless);
}

static void releaseSwapchainAndSurface() {
    if (g_ctx.dev) vkDeviceWaitIdle(g_ctx.dev);
    if (g_ctx.swapchain) {
        vkDestroySwapchainKHR(g_ctx.dev, g_ctx.swapchain, nullptr);
        g_ctx.swapchain = VK_NULL_HANDLE;
    }
    g_ctx.swapImages.clear();
    if (g_ctx.surface) {
        vkDestroySurfaceKHR(g_ctx.inst, g_ctx.surface, nullptr);
        g_ctx.surface = VK_NULL_HANDLE;
    }
    if (g_ctx.presentationWindow) DestroyWindow(g_ctx.presentationWindow);
    g_ctx.presentationWindow = nullptr;
    g_ctx.presentationVisible = false;
}

static bool srActive() {
    return g_ctx.srQuality >= 1 && g_ctx.srOptionsSet &&
           g_ctx.srOutputWidth == g_ctx.width && g_ctx.srOutputHeight == g_ctx.height;
}

static bool inputsValid() {
    auto displaySized = [](const ImportedTexture& t) {
        return t.valid() && t.width == g_ctx.width && t.height == g_ctx.height;
    };
    if (!displaySized(g_ctx.finalTex) || !displaySized(g_ctx.hudlessTex) ||
        !g_ctx.depthTex.valid() || !g_ctx.motionTex.valid() ||
        g_ctx.depthTex.width != g_ctx.motionTex.width ||
        g_ctx.depthTex.height != g_ctx.motionTex.height) return false;
    return g_ctx.srQuality < 0 ||
        (srActive() && displaySized(g_ctx.srOutTex) && g_ctx.worldTex.valid() &&
         g_ctx.worldTex.width == g_ctx.depthTex.width &&
         g_ctx.worldTex.height == g_ctx.depthTex.height);
}

static void updateReady() {
    g_ctx.ready = inputsValid() && (srActive() || (g_ctx.fgEnabled && g_ctx.swapchain));
}

static bool waitForInputs(uint64_t timeout) {
    if (g_ctx.lastSubmitFence &&
        vkWaitForFences(g_ctx.dev, 1, &g_ctx.lastSubmitFence, VK_TRUE, timeout) != VK_SUCCESS)
        return false;
    if (g_ctx.inputsFence && g_ctx.inputsFenceValue) {
        VkSemaphoreWaitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &g_ctx.inputsFence;
        wait.pValues = &g_ctx.inputsFenceValue;
        if (vkWaitSemaphores(g_ctx.dev, &wait, timeout) != VK_SUCCESS) return false;
    }
    return true;
}

static bool waitForGpuAndFg() {
    return waitForInputs(UINT64_MAX) && vkDeviceWaitIdle(g_ctx.dev) == VK_SUCCESS;
}

static bool unlockOutput() {
    if (!g_ctx.outLocked) return true;
    if (!pUnlockObjects(g_ctx.interop, 1, &g_ctx.srOutTex.registered)) return false;
    g_ctx.outLocked = false;
    return true;
}

static bool unlockInputs() {
    if (!g_ctx.locked) return true;
    HANDLE objs[] = {g_ctx.finalTex.registered, g_ctx.hudlessTex.registered,
                     g_ctx.depthTex.registered, g_ctx.motionTex.registered,
                     g_ctx.worldTex.registered};
    if (!pUnlockObjects(g_ctx.interop, g_ctx.worldTex.valid() ? 5 : 4, objs)) return false;
    g_ctx.locked = false;
    return true;
}

static bool freeSrResources() {
    if (g_ctx.srResources) {
        auto r = slFreeResources(sl::kFeatureDLSS, g_ctx.viewport);
        // The SDK returns InvalidParameter if a failed evaluation never created the viewport.
        if (r != sl::Result::eOk && r != sl::Result::eErrorInvalidParameter) {
            logf("[dlssmc] slFreeResources(DLSS) -> %d\n", (int)r);
            return false;
        }
        g_ctx.srResources = false;
        g_ctx.srOptionsSet = false;
    }
    g_ctx.token = nullptr;
    g_ctx.constantsSet = false;
    g_ctx.srEvaluated = false;
    g_ctx.needReset = true;
    return true;
}

// Wait before eOff: the plugin may destroy the completion semaphore when disabled.
static bool stopFg() {
    if (!waitForGpuAndFg()) return false;
    if (g_ctx.dlssGOn || g_ctx.optsSent) {
        sl::DLSSGOptions off{};
        off.mode = sl::DLSSGMode::eOff;
        if (slDLSSGSetOptions(g_ctx.viewport, off) != sl::Result::eOk) return false;
    }
    releaseSwapchainAndSurface();
    g_ctx.dlssGOn = false;
    g_ctx.optsSent = false;
    g_ctx.inputsFence = VK_NULL_HANDLE;
    g_ctx.inputsFenceValue = 0;
    g_ctx.lastPresentedCount = 0;
    return true;
}

static bool releaseInputs() {
    g_ctx.ready = false;
    if (!waitForGpuAndFg() || !unlockOutput() || !unlockInputs() || !freeSrResources())
        return false;
    ImportedTexture* textures[] = {&g_ctx.finalTex, &g_ctx.hudlessTex, &g_ctx.depthTex,
                                  &g_ctx.motionTex, &g_ctx.worldTex, &g_ctx.srOutTex};
    for (auto t : textures) releaseImported(*t);
    g_ctx.lastSubmitFence = VK_NULL_HANDLE;
    return true;
}

static bool waitForSlot(uint32_t slot) {
    // Only submitted fences are waited: a failed submit leaves a reset fence unsignaled.
    if (g_ctx.fencePending[slot] &&
        vkWaitForFences(g_ctx.dev, 1, &g_ctx.fenceRing[slot], VK_TRUE, UINT64_MAX) != VK_SUCCESS)
        return false;
    g_ctx.fencePending[slot] = false;
    if (g_ctx.lastSubmitFence == g_ctx.fenceRing[slot]) g_ctx.lastSubmitFence = VK_NULL_HANDLE;
    return true;
}

static bool beginCommands(uint32_t slot) {
    if (!waitForSlot(slot)) return false;
    VkCommandBuffer cmd = g_ctx.cmdRing[slot];
    if (vkResetCommandBuffer(cmd, 0) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(cmd, &bi) == VK_SUCCESS;
}

static bool submitAndWait(uint32_t slot) {
    VkCommandBuffer cmd = g_ctx.cmdRing[slot];
    if (vkEndCommandBuffer(cmd) != VK_SUCCESS ||
        vkResetFences(g_ctx.dev, 1, &g_ctx.fenceRing[slot]) != VK_SUCCESS) return false;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (vkQueueSubmit(g_ctx.queue, 1, &si, g_ctx.fenceRing[slot]) != VK_SUCCESS) return false;
    g_ctx.fencePending[slot] = true;
    g_ctx.lastSubmitFence = g_ctx.fenceRing[slot];
    return waitForSlot(slot);
}

static void transitionImage(VkCommandBuffer cmd, ImportedTexture& t, VkImageLayout layout,
                            VkAccessFlags access) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = t.layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = t.layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 :
                     VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = access;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    t.layout = layout;
}

static sl::Resource makeResource(const ImportedTexture& t) {
    sl::Resource r{sl::ResourceType::eTex2d, t.image, t.memory, t.view, (uint32_t)t.layout};
    r.width = t.width;
    r.height = t.height;
    r.nativeFormat = (uint32_t)t.format;
    r.mipLevels = r.arrayLayers = 1;
    r.usage = t.usage;
    return r;
}

static sl::DLSSOptions srOptions(int quality, float sharpness, int preset) {
    sl::DLSSOptions opt{};
    opt.mode = quality < 0 ? sl::DLSSMode::eOff : static_cast<sl::DLSSMode>(quality);
    opt.outputWidth = g_ctx.width;
    opt.outputHeight = g_ctx.height;
    opt.sharpness = sharpness; // SDK 2.14 accepts this deprecated field but no longer sharpens.
    opt.colorBuffersHDR = sl::Boolean::eFalse;
    opt.useAutoExposure = sl::Boolean::eFalse;
    opt.dlaaPreset = opt.qualityPreset = opt.balancedPreset = opt.performancePreset =
        opt.ultraPerformancePreset = opt.ultraQualityPreset = static_cast<sl::DLSSPreset>(preset);
    return opt;
}

// ------------------------------------------------------------------ JNI
extern "C" {

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeInit(JNIEnv* env, jclass, jstring pluginPath,
                                                            jstring logPath, jboolean mfgUnlock) {
    const char* p = pluginPath ? env->GetStringUTFChars(pluginPath, nullptr) : nullptr;
    const char* l = logPath ? env->GetStringUTFChars(logPath, nullptr) : nullptr;
    std::wstring wp = utf8ToWide(p);
    std::wstring wl = utf8ToWide(l);
    if (p) env->ReleaseStringUTFChars(pluginPath, p);
    if (l) env->ReleaseStringUTFChars(logPath, l);

    // 一个进程只开一次：换后端回来会再调 nativeInit，别把前面的日志冲掉
    if (!wl.empty() && !g_log) {
        std::wstring f = wl + L"\\dlssmc_fg.log";
        g_log = _wfopen(f.c_str(), L"w");
    }
    logf("[dlssmc] ---- FG nativeInit ----\n");
    g_pluginDir = wp;
    g_mfgUnlock = mfgUnlock == JNI_TRUE;

    if (!initStreamline(wp, wl)) return -1;
    return 0;
}

/** MFG 解锁的结果说明，给叠加层/日志用 */
JNIEXPORT jstring JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeGetMfgUnlockReport(JNIEnv* env, jclass) {
    return env->NewStringUTF(g_mfgUnlockReport.c_str());
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeIsSupported(JNIEnv*, jclass) {
    if (!g_ctx.slInited) return 0;
    if (!g_ctx.phys) return 0; // 需要先建设备才知道适配器

    VkPhysicalDeviceIDProperties idProps{};
    idProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    VkPhysicalDeviceProperties2 p2{};
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &idProps;
    vkGetPhysicalDeviceProperties2(g_ctx.phys, &p2);

    sl::AdapterInfo ai{};
    ai.deviceLUID = idProps.deviceLUID;
    ai.deviceLUIDSizeInBytes = VK_LUID_SIZE;
    auto r = slIsFeatureSupported(sl::kFeatureDLSS_G, ai);
    logf("[dlssmc] slIsFeatureSupported(DLSS_G) -> %d\n", (int)r);
    return r == sl::Result::eOk ? 1 : 0;
}

// 设备创建失败时把已建的对象清干净，避免每次重试都泄漏一套
static void teardownVulkanOnFailure() {
    releaseSwapchainAndSurface();
    if (g_ctx.dev) {
        for (uint32_t i = 0; i < kFramesInFlight; ++i) {
            if (g_ctx.fenceRing[i]) vkDestroyFence(g_ctx.dev, g_ctx.fenceRing[i], nullptr);
            if (g_ctx.renderDoneRing[i]) vkDestroySemaphore(g_ctx.dev, g_ctx.renderDoneRing[i], nullptr);
            if (g_ctx.imageAvailRing[i]) vkDestroySemaphore(g_ctx.dev, g_ctx.imageAvailRing[i], nullptr);
        }
        if (g_ctx.pool) vkDestroyCommandPool(g_ctx.dev, g_ctx.pool, nullptr);
        vkDestroyDevice(g_ctx.dev, nullptr);
    }
    if (g_ctx.inst) vkDestroyInstance(g_ctx.inst, nullptr);
    g_ctx.swapchain = VK_NULL_HANDLE;
    g_ctx.surface = VK_NULL_HANDLE;
    g_ctx.dev = VK_NULL_HANDLE;
    g_ctx.inst = VK_NULL_HANDLE;
    g_ctx.phys = VK_NULL_HANDLE;
    g_ctx.queue = VK_NULL_HANDLE;
    g_ctx.pool = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        g_ctx.cmdRing[i] = VK_NULL_HANDLE;
        g_ctx.fenceRing[i] = VK_NULL_HANDLE;
        g_ctx.renderDoneRing[i] = VK_NULL_HANDLE;
        g_ctx.imageAvailRing[i] = VK_NULL_HANDLE;
    }
    g_ctx.frameSlot = 0;
    g_ctx.swapImages.clear();
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeCreateDevice(JNIEnv*, jclass, jlong hwnd,
                                                                    jint width, jint height, jboolean reflexBoost,
                                                                    jint reflexFpsLimit) {
    if (!g_ctx.slInited || !hwnd || width <= 0 || height <= 0 ||
        reflexFpsLimit < 0 || reflexFpsLimit > 1000) return -4;
    if (g_ctx.dev) {
        HWND window = reinterpret_cast<HWND>((intptr_t)hwnd);
        if (window != g_ctx.hwnd || (uint32_t)width != g_ctx.width || (uint32_t)height != g_ctx.height) {
            if (!stopFg() || !releaseInputs()) return -2;
            g_ctx.hwnd = window;
            g_ctx.width = (uint32_t)width;
            g_ctx.height = (uint32_t)height;
            g_ctx.srOptionsSet = false;
        }
        return setReflexOptions(reflexBoost != JNI_FALSE, (uint32_t)reflexFpsLimit) == sl::Result::eOk
                   ? 0 : -3;
    }

    g_ctx.hwnd = reinterpret_cast<HWND>((intptr_t)hwnd);
    g_ctx.width = (uint32_t)width;
    g_ctx.height = (uint32_t)height;
    g_ctx.reflexBoost = reflexBoost != JNI_FALSE;
    g_ctx.reflexFpsLimit = (uint32_t)reflexFpsLimit;

    if (!initInteropApi()) {
        logf("[dlssmc] WGL_NV_DX_interop2 不可用\n");
        return -1;
    }

    // 先把 Vulkan 建起来，让 Streamline 先认到 Vulkan 后端；
    // 否则先建 D3D11（那是为了互操作）会被 SL 的 D3D11 钩子接住，把它当成 D3D11 应用。
    if (!initVulkan()) {
        teardownVulkanOnFailure();
        return -1;
    }
    if (!createSwapchain()) {
        logf("[dlssmc] swapchain 创建失败，回滚\n");
        teardownVulkanOnFailure();
        return -1;
    }

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, dev.GetAddressOf(), nullptr,
                                 ctx.GetAddressOf()))) {
        logf("[dlssmc] D3D11CreateDevice 失败\n");
        teardownVulkanOnFailure();
        return -1;
    }
    g_ctx.d3d11 = dev;
    g_ctx.d3d11Ctx = ctx;

    g_ctx.interop = pOpenDevice(g_ctx.d3d11.Get());
    if (!g_ctx.interop) {
        logf("[dlssmc] wglDXOpenDeviceNV 失败\n");
        teardownVulkanOnFailure();
        return -1;
    }
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeConfigureSR(JNIEnv*, jclass, jint quality,
                                                                 jfloat sharpness, jint preset) {
    if (!g_ctx.slInited || !g_ctx.dev || !g_ctx.width || !g_ctx.height) return -1;
    if ((quality != -1 && (quality < 1 || quality > 6)) ||
        !(sharpness >= 0.0f && sharpness <= 1.0f) ||
        (preset != 0 && (preset < 5 || preset >= (int)sl::DLSSPreset::eCount))) return -4;
    if (quality > 0 && (!g_ctx.commonPresent || !g_ctx.commonAfterPresent)) {
        if (slGetFeatureFunction(sl::kFeatureCommon, "slHookVkPresent",
                reinterpret_cast<void*&>(g_ctx.commonPresent)) != sl::Result::eOk ||
            slGetFeatureFunction(sl::kFeatureCommon, "slHookVkAfterPresent",
                reinterpret_cast<void*&>(g_ctx.commonAfterPresent)) != sl::Result::eOk) return -5;
    }
    if (g_ctx.srOptionsSet && quality == g_ctx.srQuality && sharpness == g_ctx.srSharpness &&
        preset == g_ctx.srPreset && g_ctx.srOutputWidth == g_ctx.width &&
        g_ctx.srOutputHeight == g_ctx.height) return 0;
    if (!waitForGpuAndFg() || !unlockOutput() || !unlockInputs() || !freeSrResources()) return -2;
    auto opt = srOptions(quality, sharpness, preset);
    auto r = slDLSSSetOptions(g_ctx.viewport, opt);
    if (r != sl::Result::eOk) {
        logf("[dlssmc] slDLSSSetOptions -> %d\n", (int)r);
        g_ctx.srOptionsSet = false;
        updateReady();
        return -3;
    }
    g_ctx.srQuality = quality;
    g_ctx.srSharpness = sharpness;
    g_ctx.srPreset = preset;
    g_ctx.srOptionsSet = true;
    g_ctx.srOutputWidth = g_ctx.width;
    g_ctx.srOutputHeight = g_ctx.height;
    updateReady();
    return 0;
}

JNIEXPORT jintArray JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeGetRenderSize(JNIEnv* env, jclass,
                                                                   jint quality) {
    if (!g_ctx.slInited || !g_ctx.dev || !g_ctx.width || !g_ctx.height ||
        quality < 1 || quality > 6) return nullptr;
    auto opt = srOptions(quality, g_ctx.srSharpness, g_ctx.srPreset);
    sl::DLSSOptimalSettings settings{};
    auto r = slDLSSGetOptimalSettings(opt, settings);
    if (r != sl::Result::eOk || !settings.optimalRenderWidth || !settings.optimalRenderHeight ||
        settings.optimalRenderWidth > g_ctx.width || settings.optimalRenderHeight > g_ctx.height) {
        logf("[dlssmc] slDLSSGetOptimalSettings -> %d (%ux%u)\n", (int)r,
             settings.optimalRenderWidth, settings.optimalRenderHeight);
        return nullptr;
    }
    jint sizes[] = {(jint)settings.optimalRenderWidth, (jint)settings.optimalRenderHeight};
    jintArray out = env->NewIntArray(2);
    if (out) env->SetIntArrayRegion(out, 0, 2, sizes);
    return out;
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeBindTextures(JNIEnv*, jclass, jint finalGl,
                                                                    jint hudlessGl, jint depthGl,
                                                                    jint motionGl, jint worldGl,
                                                                    jint outGl, jint renderW, jint renderH) {
    if (!g_ctx.dev || !g_ctx.interop) return -1;
    if (renderW <= 0 || renderH <= 0 || (uint32_t)renderW > g_ctx.width ||
        (uint32_t)renderH > g_ctx.height || !finalGl || !hudlessGl || !depthGl || !motionGl ||
        (g_ctx.srQuality >= 1 ? (!worldGl || !outGl) : (worldGl || outGl))) return -8;
    const GLuint gls[] = {(GLuint)finalGl, (GLuint)hudlessGl, (GLuint)depthGl,
                          (GLuint)motionGl, (GLuint)worldGl, (GLuint)outGl};
    for (int i = 0; i < 6; ++i)
        for (int j = 0; j < i; ++j)
            if (gls[i] && gls[i] == gls[j]) return -8;
    // Stop old FG work before replacing any resources; keep the requested FG mode.
    if (g_ctx.dlssGOn && !stopFg()) return -9;
    if (!releaseInputs()) return -9;
    if (g_ctx.fgEnabled && !g_ctx.swapchain && !createSwapchain()) return -10;
    if (g_ctx.srQuality >= 1 &&
        Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeConfigureSR(
            nullptr, nullptr, g_ctx.srQuality, g_ctx.srSharpness, g_ctx.srPreset) != 0) return -11;

    ImportedTexture* textures[] = {&g_ctx.finalTex, &g_ctx.hudlessTex, &g_ctx.depthTex,
                                  &g_ctx.motionTex, &g_ctx.worldTex, &g_ctx.srOutTex};
    for (int i = 0; i < 6; ++i) {
        if (!gls[i]) continue;
        bool lowRes = i >= 2 && i <= 4;
        VkFormat format = i == 2 ? VK_FORMAT_R32_SFLOAT :
                          i == 3 ? VK_FORMAT_R16G16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
        if (!importGlTexture(*textures[i], gls[i], lowRes ? (uint32_t)renderW : g_ctx.width,
                             lowRes ? (uint32_t)renderH : g_ctx.height, format, i == 5)) {
            releaseInputs();
            return -2 - i;
        }
    }
    const uint32_t slot = g_ctx.frameSlot;
    if (!beginCommands(slot)) return -9;
    for (auto t : textures) {
        if (t->valid()) transitionImage(g_ctx.cmdRing[slot], *t, VK_IMAGE_LAYOUT_GENERAL,
                                       VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    }
    if (!submitAndWait(slot)) return -9;
    g_ctx.needReset = true;
    g_ctx.optsSent = false;
    updateReady();
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeSetMode(JNIEnv*, jclass, jint framesToGenerate,
                                                                jboolean forceMultiFrame) {
    sl::DLSSGState state{};
    auto r = slDLSSGGetState(g_ctx.viewport, state, nullptr);
    if (r != sl::Result::eOk || state.numFramesToGenerateMax == 0) return -1;
    uint32_t requested = framesToGenerate <= 0 ? 1 : (uint32_t)framesToGenerate;
    if (forceMultiFrame == JNI_TRUE) {
        // 探针用：跳过本地夹值，把请求原样交给 slDLSSGSetOptions / NGX。
        // 生产路径不开——解锁生效后 numFramesToGenerateMax 本身就是 5，夹值不会压档。
        g_ctx.framesToGenerate = requested;
    } else {
        g_ctx.framesToGenerate = requested < state.numFramesToGenerateMax ? requested : state.numFramesToGenerateMax;
    }
    logf("[dlssmc] FG requested=%u set=%u sdkMax=%u forced=%d minDimension=%u\n",
         requested, g_ctx.framesToGenerate, state.numFramesToGenerateMax,
         (int)(forceMultiFrame == JNI_TRUE), state.minWidthOrHeight);
    g_ctx.needReset = true;
    // 回生效张数：本机 numFramesToGenerateMax 可能小于用户选的档位，Java 侧要能说出「已降档」
    return (jint)g_ctx.framesToGenerate;
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeBeginFrame(JNIEnv*, jclass, jboolean reflexBoost,
                                                                 jint reflexFpsLimit) {
    if (g_ctx.locked || g_ctx.outLocked) return -4;
    g_ctx.token = nullptr;
    g_ctx.constantsSet = false;
    g_ctx.srEvaluated = false;
    if (!g_ctx.ready || !inputsValid() || (!g_ctx.swapchain && !srActive())) return 1;
    bool boost = reflexBoost != JNI_FALSE;
    uint32_t fpsLimit = (uint32_t)reflexFpsLimit;
    if ((boost != g_ctx.reflexBoost || fpsLimit != g_ctx.reflexFpsLimit) &&
        setReflexOptions(boost, fpsLimit) != sl::Result::eOk) return -3;
    uint32_t fi = g_ctx.frameIndex++;
    auto r = slGetNewFrameToken(g_ctx.token, &fi);
    if (r != sl::Result::eOk) return -1;
    r = slReflexSleep(*g_ctx.token);
    if (r != sl::Result::eOk) {
        g_ctx.token = nullptr;
        return -2;
    }
    slPCLSetMarker(sl::PCLMarker::eSimulationStart, *g_ctx.token);
    return 0;
}

JNIEXPORT void JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeRenderStart(JNIEnv*, jclass) {
    if (!g_ctx.token) return;
    slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *g_ctx.token);
    slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *g_ctx.token);
}

// 桥接路径分段计时用的墙钟
static double g_qpcPerMs = 0.0;
static LARGE_INTEGER qpcNow() {
    if (g_qpcPerMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcPerMs = (double)f.QuadPart / 1000.0;
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t;
}
static double qpcMs(const LARGE_INTEGER& from) {
    LARGE_INTEGER to = qpcNow();
    return (double)(to.QuadPart - from.QuadPart) / g_qpcPerMs;
}

// DLSS-G 内部调用 DXGI/Vulkan 出错时的回调（present / acquireNextImage）
static void onApiError(const sl::APIError& err) {
    logf("[dlssmc] DLSS-G 底层 API 错误，代码 0x%08x\n", (unsigned)err.hres);
}

// Reflex 的逐帧延迟回报：取最新一帧，把各阶段时间差换算成毫秒
static void readReflexLatency() {
    sl::ReflexState state{};
    if (slReflexGetState(state) != sl::Result::eOk || !state.latencyReportAvailable) {
        g_ctx.latValid = false;
        return;
    }
    const sl::ReflexReport* best = nullptr;
    for (const sl::ReflexReport& r : state.frameReport) {
        if (r.simStartTime && r.presentEndTime > r.simStartTime &&
            (best == nullptr || r.frameID > best->frameID)) best = &r;
    }
    if (best == nullptr) { g_ctx.latValid = false; return; }
    if (g_ctx.latRawFirst == 0) {
        g_ctx.latRawFirst = best->frameID;
        logf("[dlssmc] Reflex timestamps(us) simulation=%llu presentEnd=%llu gpuFrameTimeUs=%u\n",
             (unsigned long long)best->simStartTime, (unsigned long long)best->presentEndTime,
             best->gpuFrameTimeUs);
    }
    auto ms = [](uint64_t from, uint64_t to) { return (double)(to - from) / 1000.0; };
    g_ctx.latTotal = ms(best->simStartTime, best->presentEndTime);
    g_ctx.latSim = best->simEndTime > best->simStartTime ? ms(best->simStartTime, best->simEndTime) : 0;
    g_ctx.latSubmit = best->renderSubmitEndTime > best->renderSubmitStartTime
                      ? ms(best->renderSubmitStartTime, best->renderSubmitEndTime) : 0;
    g_ctx.latQueue = best->osRenderQueueEndTime > best->osRenderQueueStartTime
                     ? ms(best->osRenderQueueStartTime, best->osRenderQueueEndTime) : 0;
    g_ctx.latGpu = best->gpuRenderEndTime > best->gpuRenderStartTime
                   ? ms(best->gpuRenderStartTime, best->gpuRenderEndTime) : 0;
    g_ctx.latPresent = best->presentEndTime > best->presentStartTime
                       ? ms(best->presentStartTime, best->presentEndTime) : 0;
    g_ctx.latValid = true;
}

// Shared, immutable constants for this token: SR runs first, FG must not overwrite its reset/jitter.
static sl::Result setFrameConstants(JNIEnv* env, jfloatArray matrices, jfloatArray params,
                                    jboolean reset) {
    if (g_ctx.constantsSet) return sl::Result::eOk;
    if (!g_ctx.token || !matrices || !params || env->GetArrayLength(matrices) < 64 ||
        env->GetArrayLength(params) < 8) return sl::Result::eErrorInvalidParameter;
    float m[64], p[8];
    env->GetFloatArrayRegion(matrices, 0, 64, m);
    env->GetFloatArrayRegion(params, 0, 8, p);
    if (env->ExceptionCheck()) return sl::Result::eErrorInvalidParameter;

    sl::float4x4 proj, view, prevProj, prevView;
    auto toRow = [](const float* s, sl::float4x4& d) {
        std::memcpy(&d.row[0], s, 16 * sizeof(float));
    };
    toRow(m + 0, proj);
    toRow(m + 16, view);
    toRow(m + 32, prevProj);
    toRow(m + 48, prevView);
    for (int i = 0; i < 4; ++i) {
        proj.row[i].z = 0.5f * (proj.row[i].z + proj.row[i].w);
        prevProj.row[i].z = 0.5f * (prevProj.row[i].z + prevProj.row[i].w);
    }

    sl::float4x4 clipToCameraView, viewToWorld, prevViewProj, clipToPrevClip, prevClipToClip, tmp;
    sl::matrixFullInvert(clipToCameraView, proj);
    sl::matrixFullInvert(viewToWorld, view);
    sl::matrixMul(prevViewProj, prevView, prevProj);
    sl::matrixMul(tmp, clipToCameraView, viewToWorld);
    sl::matrixMul(clipToPrevClip, tmp, prevViewProj);
    sl::matrixFullInvert(prevClipToClip, clipToPrevClip);

    sl::Constants consts{};
    consts.cameraViewToClip = proj;
    consts.clipToCameraView = clipToCameraView;
    consts.clipToPrevClip = clipToPrevClip;
    consts.prevClipToClip = prevClipToClip;
    consts.jitterOffset = {p[4], p[5]};
    consts.mvecScale = {p[6], p[7]};
    consts.cameraPinholeOffset = {0.0f, 0.0f};
    consts.cameraPos = {viewToWorld.row[3].x, viewToWorld.row[3].y, viewToWorld.row[3].z};
    consts.cameraUp = {viewToWorld.row[1].x, viewToWorld.row[1].y, viewToWorld.row[1].z};
    consts.cameraRight = {viewToWorld.row[0].x, viewToWorld.row[0].y, viewToWorld.row[0].z};
    consts.cameraFwd = {-viewToWorld.row[2].x, -viewToWorld.row[2].y, -viewToWorld.row[2].z};
    consts.cameraNear = p[0];
    consts.cameraFar = p[1];
    consts.cameraFOV = p[2];
    consts.cameraAspectRatio = p[3];
    consts.depthInverted = sl::Boolean::eFalse;
    consts.cameraMotionIncluded = sl::Boolean::eTrue;
    consts.motionVectors3D = sl::Boolean::eFalse;
    consts.orthographicProjection = sl::Boolean::eFalse;
    consts.motionVectorsDilated = sl::Boolean::eFalse;
    consts.reset = (reset == JNI_TRUE || g_ctx.needReset) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    auto r = slSetConstants(consts, *g_ctx.token, g_ctx.viewport);
    if (r == sl::Result::eOk) {
        g_ctx.constantsSet = true;
        g_ctx.needReset = false;
    } else {
        logf("[dlssmc] slSetConstants -> %d\n", (int)r);
    }
    return r;
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeUpscale(JNIEnv* env, jclass,
                                                               jfloatArray matrices,
                                                               jfloatArray params, jboolean reset) {
    if (!g_ctx.ready || !srActive() || !inputsValid()) return -1;
    if (g_ctx.locked || g_ctx.outLocked || g_ctx.srEvaluated) return -2;
    if (!g_ctx.token) return -3; // Never mint/consume a token here: BeginFrame owns it.
    if (setFrameConstants(env, matrices, params, reset) != sl::Result::eOk) return -4;
    if (!waitForInputs(UINT64_MAX)) return -6;
    const uint32_t slot = g_ctx.frameSlot;
    if (!beginCommands(slot)) return -6;
    VkCommandBuffer cmd = g_ctx.cmdRing[slot];
    transitionImage(cmd, g_ctx.worldTex, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT);
    transitionImage(cmd, g_ctx.depthTex, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT);
    transitionImage(cmd, g_ctx.motionTex, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT);
    transitionImage(cmd, g_ctx.srOutTex, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT);

    sl::Resource world = makeResource(g_ctx.worldTex), output = makeResource(g_ctx.srOutTex);
    sl::Resource depth = makeResource(g_ctx.depthTex), motion = makeResource(g_ctx.motionTex);
    sl::Extent renderExtent{0, 0, world.width, world.height};
    sl::Extent fullExtent{0, 0, output.width, output.height};
    sl::ResourceTag tags[] = {
        {&world, sl::kBufferTypeScalingInputColor, sl::eValidUntilEvaluate, &renderExtent},
        {&output, sl::kBufferTypeScalingOutputColor, sl::eValidUntilEvaluate, &fullExtent},
        {&depth, sl::kBufferTypeDepth, sl::eValidUntilPresent, &renderExtent},
        {&motion, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &renderExtent},
    };
    auto r = slSetTagForFrame(*g_ctx.token, g_ctx.viewport, tags, 4, cmd);
    if (r == sl::Result::eOk) {
        const sl::BaseStructure* inputs[] = {&g_ctx.viewport};
        // Evaluate can allocate before reporting an error; release those resources on reconfigure too.
        g_ctx.srResources = true;
        g_ctx.srEvaluated = true;
        r = slEvaluateFeature(sl::kFeatureDLSS, *g_ctx.token, inputs, 1, cmd);
    }
    if (r == sl::Result::eOk) {
        // SL restores the tagged layouts. Copy in Vulkan coordinates: no additional Y flip.
        transitionImage(cmd, g_ctx.srOutTex, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_ACCESS_TRANSFER_READ_BIT);
        transitionImage(cmd, g_ctx.hudlessTex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_ACCESS_TRANSFER_WRITE_BIT);
        VkImageCopy copy{};
        copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {g_ctx.width, g_ctx.height, 1};
        vkCmdCopyImage(cmd, g_ctx.srOutTex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       g_ctx.hudlessTex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transitionImage(cmd, g_ctx.srOutTex, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_MEMORY_READ_BIT);
        transitionImage(cmd, g_ctx.hudlessTex, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT);
    }
    // Finish even a partially recorded evaluation before allowing GL access or resource release.
    if (!submitAndWait(slot)) {
        g_ctx.ready = false;
        g_ctx.needReset = true;
        return -6;
    }
    if (r != sl::Result::eOk) {
        logf("[dlssmc] DLSS SR tag/evaluate -> %d\n", (int)r);
        g_ctx.needReset = true;
        return -5;
    }
    // WGL lock grants GL ownership, so it must happen AFTER all Vulkan writes have completed.
    if (!pLockObjects(g_ctx.interop, 1, &g_ctx.srOutTex.registered)) return -7;
    g_ctx.outLocked = true;
    if (!g_ctx.fgEnabled) slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *g_ctx.token);
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeUpscaleDone(JNIEnv*, jclass) {
    if (!g_ctx.outLocked) return 0;
    if (!unlockOutput()) return -1;
    if (!g_ctx.fgEnabled) {
        // GL presents SR-only frames; advance SL garbage collection after its GPU work is complete.
        bool skip = false;
        if (g_ctx.commonPresent(g_ctx.queue, nullptr, skip) != VK_SUCCESS ||
            g_ctx.commonAfterPresent() != VK_SUCCESS) return -2;
        g_ctx.token = nullptr;
    }
    return 0;
}

// 每帧调用：把最终画面搬进 swapchain 图像，交给 DLSS-G 插帧后 present
// matrices: 列主序 4x4 x4 = [proj, view, prevProj, prevView]
// params:   [near, far, fov, aspect, jitterX, jitterY, mvecScaleX, mvecScaleY]
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativePresent(JNIEnv* env, jclass,
                                                                jfloatArray matrices,
                                                                jfloatArray params,
                                                                jboolean reset, jint statusLogInterval) {
    if (!g_ctx.ready || !inputsValid()) return -1;
    if (g_ctx.takeoverDisabled) return -1;
    if (!g_ctx.fgEnabled || !g_ctx.swapchain) return -1;
    if (g_ctx.locked || g_ctx.outLocked) return -2;
    if (!g_ctx.token) return 2;
    sl::Result r = setFrameConstants(env, matrices, params, reset);
    if (r != sl::Result::eOk) return -4;
    sl::FrameToken* token = g_ctx.token;
    g_ctx.token = nullptr;
    ++g_logCounter;

    const uint32_t slot = g_ctx.frameSlot;
    g_ctx.frameSlot = (slot + 1) % kFramesInFlight;

    // 只等这个槽位上「上一轮」的帧，不阻塞当前流水线
    LARGE_INTEGER mark = qpcNow();
    if (!waitForSlot(slot)) return -5;
    g_ctx.tSlotWait = qpcMs(mark);

    uint32_t imageIndex = 0;
    mark = qpcNow();
    // 不用无限等待：present 队列满时宁可这一帧交回 GL，也不要卡死主线程
    VkResult ar = vkAcquireNextImageKHR(g_ctx.dev, g_ctx.swapchain, 1000000000ull,
                                        g_ctx.imageAvailRing[slot], VK_NULL_HANDLE, &imageIndex);
    g_ctx.tAcquire = qpcMs(mark);
    if (ar == VK_ERROR_OUT_OF_DATE_KHR) return 1;
    if (ar == VK_TIMEOUT || ar == VK_NOT_READY) return 2;
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) return -3;

    VkCommandBuffer cmd = g_ctx.cmdRing[slot];
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    // ---- 把最终画面 blit 进 swapchain 图像 ----
    VkImageMemoryBarrier bars[2] = {};
    bars[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bars[0].oldLayout = g_ctx.finalTex.layout;
    bars[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    bars[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bars[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bars[0].image = g_ctx.finalTex.image;
    bars[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bars[0].srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    bars[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

    bars[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bars[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bars[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bars[1].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bars[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bars[1].image = g_ctx.swapImages[imageIndex];
    bars[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    bars[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 2, bars);
    g_ctx.finalTex.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {g_ctx.width, g_ctx.height, 1};

    if (g_ctx.swapFormat == g_ctx.finalTex.format) {
        vkCmdCopyImage(cmd, g_ctx.finalTex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       g_ctx.swapImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &region);
    } else {
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {(int32_t)g_ctx.width, (int32_t)g_ctx.height, 1};
        blit.dstOffsets[1] = {(int32_t)g_ctx.width, (int32_t)g_ctx.height, 1};
        vkCmdBlitImage(cmd, g_ctx.finalTex.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       g_ctx.swapImages[imageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_NEAREST);
    }

    if (g_ctx.hudlessTex.layout != VK_IMAGE_LAYOUT_GENERAL) {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.oldLayout = g_ctx.hudlessTex.layout;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = g_ctx.hudlessTex.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &b);
        g_ctx.hudlessTex.layout = VK_IMAGE_LAYOUT_GENERAL;
    }

    // Vulkan descriptors must describe the imported image, not the display resolution.
    sl::Resource resHudless = makeResource(g_ctx.hudlessTex);
    sl::Resource resDepth = makeResource(g_ctx.depthTex);
    sl::Resource resMvec = makeResource(g_ctx.motionTex);
    sl::Extent fullExtent{0, 0, g_ctx.width, g_ctx.height};
    sl::Extent depthExtent{0, 0, resDepth.width, resDepth.height};
    sl::Extent motionExtent{0, 0, resMvec.width, resMvec.height};

    // DLSS-G 需要 backbuffer 的 tag —— 不是为了传资源指针（SL 自己代理 swapchain），
    // 而是要把尺寸 / 格式 / 布局告诉它。少了它 SL 会报
    // "Invalid backbuffer resource extent (0 x 0)"，然后 NGX 建特性失败。
    sl::Resource resBackbuffer{sl::ResourceType::eTex2d, g_ctx.swapImages[imageIndex], nullptr,
                               nullptr, (uint32_t)VK_IMAGE_LAYOUT_PRESENT_SRC_KHR};
    resBackbuffer.width = g_ctx.width;
    resBackbuffer.height = g_ctx.height;
    resBackbuffer.nativeFormat = (uint32_t)g_ctx.swapFormat;
    resBackbuffer.mipLevels = 1;
    resBackbuffer.arrayLayers = 1;
    resBackbuffer.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    // 未提供 UIAlpha/UIColorAndAlpha；仅下发重合成开关不满足 SDK 的完整输入要求。
    sl::ResourceTag tags[4] = {
        sl::ResourceTag{&resHudless, sl::kBufferTypeHUDLessColor, sl::eValidUntilPresent, &fullExtent},
        sl::ResourceTag{&resDepth, sl::kBufferTypeDepth, sl::eValidUntilPresent, &depthExtent},
        sl::ResourceTag{&resMvec, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &motionExtent},
        sl::ResourceTag{&resBackbuffer, sl::kBufferTypeBackbuffer, sl::eValidUntilPresent,
                        &fullExtent},
    };
    uint32_t tagCount = 4u;
    bool uiActive = g_ctx.fgUiRecomposition;

    // ---- DLSS-G 选项 ----
    sl::DLSSGOptions opt{};
    opt.mode = sl::DLSSGMode::eOn;
    opt.numFramesToGenerate = g_ctx.framesToGenerate;
    opt.numBackBuffers = (uint32_t)g_ctx.swapImages.size();
    opt.mvecDepthWidth = g_ctx.depthTex.width;
    opt.mvecDepthHeight = g_ctx.depthTex.height;
    opt.colorWidth = g_ctx.width;
    opt.colorHeight = g_ctx.height;
    opt.colorBufferFormat = (uint32_t)g_ctx.swapFormat;
    opt.mvecBufferFormat = (uint32_t)VK_FORMAT_R16G16_SFLOAT;
    opt.depthBufferFormat = (uint32_t)VK_FORMAT_R32_SFLOAT;
    opt.hudLessBufferFormat = (uint32_t)VK_FORMAT_R8G8B8A8_UNORM;
    opt.enableUserInterfaceRecomposition = uiActive ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    uint32_t flags = 0;
    if (g_ctx.fgShowOnlyInterpolated) flags |= (uint32_t)sl::DLSSGFlags::eShowOnlyInterpolatedFrame;
    if (g_ctx.fgRetainResources)      flags |= (uint32_t)sl::DLSSGFlags::eRetainResourcesWhenOff;
    if (g_ctx.fgMenuDetection)        flags |= (uint32_t)sl::DLSSGFlags::eEnableFullscreenMenuDetection;
    opt.flags = static_cast<sl::DLSSGFlags>(flags);
    // eBlockNoClientQueues 目前只有 Vulkan 支持：FG 不再阻塞我们的呈现队列，
    // 代价是改/销毁输入纹理前必须等 inputsProcessingCompletionFence（nativeLock 里已经等）
    opt.queueParallelismMode = g_ctx.fgQueueParallelism
        ? sl::DLSSGQueueParallelismMode::eBlockNoClientQueues
        : sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
    opt.onErrorCallback = &onApiError;

    static const auto& sent = g_ctx.sentOpt;
    bool unchanged = g_ctx.optsSent
            && opt.mode == sent.mode
            && opt.numFramesToGenerate == sent.numFramesToGenerate
            && opt.flags == sent.flags
            && opt.queueParallelismMode == sent.queueParallelismMode
            && opt.enableUserInterfaceRecomposition == sent.enableUserInterfaceRecomposition
            && opt.numBackBuffers == sent.numBackBuffers
            && opt.colorWidth == sent.colorWidth && opt.colorHeight == sent.colorHeight
            && opt.mvecDepthWidth == sent.mvecDepthWidth && opt.mvecDepthHeight == sent.mvecDepthHeight
            && opt.colorBufferFormat == sent.colorBufferFormat
            && opt.mvecBufferFormat == sent.mvecBufferFormat
            && opt.depthBufferFormat == sent.depthBufferFormat
            && opt.hudLessBufferFormat == sent.hudLessBufferFormat
            && opt.uiBufferFormat == sent.uiBufferFormat;
    if (unchanged) {
        g_ctx.dlssGOn = true;
    } else {
        r = slDLSSGSetOptions(g_ctx.viewport, opt);
        if (r != sl::Result::eOk) {
            logf("[dlssmc] slDLSSGSetOptions -> %d\n", (int)r);
        } else {
            logf("[dlssmc] slDLSSGSetOptions 下发: frames=%u flags=0x%x queue=%d ui=%d %ux%u buffers=%u\n",
                 opt.numFramesToGenerate, (uint32_t)opt.flags, (int)opt.queueParallelismMode,
                 (int)opt.enableUserInterfaceRecomposition, opt.colorWidth, opt.colorHeight,
                 opt.numBackBuffers);
            g_ctx.sentOpt = opt;
            g_ctx.optsSent = true;
            g_ctx.dlssGOn = true;
        }
    }

    g_ctx.frameCounter++;

    // ---- swapchain 图像转到 present 布局 ----
    VkImageMemoryBarrier toPresent{};
    toPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toPresent.image = g_ctx.swapImages[imageIndex];
    toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &toPresent);

    r = slSetTagForFrame(*token, g_ctx.viewport, tags, tagCount, cmd);
    if (r != sl::Result::eOk) {
        LOG_FRAME_ERR(g_lastTagErr, (int)r, "[dlssmc] slSetTagForFrame -> %d\n", (int)r);
    }
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &g_ctx.imageAvailRing[slot];
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &g_ctx.renderDoneRing[slot];
    vkResetFences(g_ctx.dev, 1, &g_ctx.fenceRing[slot]);
    LARGE_INTEGER submitMark = qpcNow();
    if (vkQueueSubmit(g_ctx.queue, 1, &si, g_ctx.fenceRing[slot]) != VK_SUCCESS) {
        g_ctx.ready = false;
        return -5;
    }
    g_ctx.tSubmit = qpcMs(submitMark);
    g_ctx.fencePending[slot] = true;
    g_ctx.lastSubmitFence = g_ctx.fenceRing[slot];
    slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *token);

    slPCLSetMarker(sl::PCLMarker::ePresentStart, *token);
    submitMark = qpcNow();

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &g_ctx.renderDoneRing[slot];
    pi.swapchainCount = 1;
    pi.pSwapchains = &g_ctx.swapchain;
    pi.pImageIndices = &imageIndex;
    VkResult pr = vkQueuePresentKHR(g_ctx.queue, &pi);
    g_ctx.tPresent = qpcMs(submitMark);

    slPCLSetMarker(sl::PCLMarker::ePresentEnd, *token);

    // 问一下 DLSS-G 实际呈现了几帧（插帧是否真的生效）
    LARGE_INTEGER stateMark = qpcNow();
    sl::DLSSGState state{};
    r = slDLSSGGetState(g_ctx.viewport, state, nullptr);
    g_ctx.lastPresentedCount = r == sl::Result::eOk ? state.numFramesActuallyPresented : 0;
    readReflexLatency();
    g_ctx.tState = qpcMs(stateMark);
    if (r == sl::Result::eOk) {
        g_ctx.inputsFence = reinterpret_cast<VkSemaphore>(state.inputsProcessingCompletionFence);
        g_ctx.inputsFenceValue = state.lastPresentInputsProcessingCompletionFenceValue;
    } else {
        logf("[dlssmc] slDLSSGGetState failed -> %d; stopping input reuse\n", (int)r);
        sl::DLSSGOptions off{};
        off.mode = sl::DLSSGMode::eOff;
        slDLSSGSetOptions(g_ctx.viewport, off);
        releaseSwapchainAndSurface();
        g_ctx.inputsFence = VK_NULL_HANDLE;
        g_ctx.inputsFenceValue = 0;
        g_ctx.dlssGOn = false;
        g_ctx.optsSent = false;
        g_ctx.fgEnabled = false;
        g_ctx.takeoverDisabled = true;
        updateReady(); // Losing FG must not disable an otherwise usable SR path.
        return -6;
    }
    int status = (int)state.status;
    if (status != g_lastStatus || (statusLogInterval > 0 && (g_ctx.frameCounter % (uint32_t)statusLogInterval) == 0)) {
        g_lastStatus = status;
        logf("[dlssmc] FG frame=%u stateResult=%d status=0x%X presented=%u generatedMax=%u\n",
             g_ctx.frameCounter, (int)r, (uint32_t)status, g_ctx.lastPresentedCount, state.numFramesToGenerateMax);
    }

    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR || ar == VK_SUBOPTIMAL_KHR) return 1;
    if (pr == VK_SUCCESS && !g_ctx.presentationVisible) {
        ShowWindow(g_ctx.presentationWindow, SW_SHOWNOACTIVATE);
        g_ctx.presentationVisible = true;
    }
    return pr == VK_SUCCESS ? 0 : -5;
}

// 锁定/解锁互操作对象。
// 实测结论：GL 只有在对象【锁定】期间写入的内容，才能被 D3D11/Vulkan 看到；
// 锁之前画的读回来全是 0。所以所有写进这三张纹理的 GL 操作都必须夹在 lock/unlock 之间。
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeLock(JNIEnv*, jclass) {
    if (!g_ctx.ready || !g_ctx.interop || !inputsValid()) return -1;
    if (g_ctx.outLocked) return -2;
    if (g_ctx.locked) return 0;
    LARGE_INTEGER mark = qpcNow();
    if (g_ctx.lastSubmitFence &&
        vkWaitForFences(g_ctx.dev, 1, &g_ctx.lastSubmitFence, VK_TRUE, 1000000000ull) != VK_SUCCESS)
        return -3;
    g_ctx.tWaitSubmit = qpcMs(mark);
    if (g_ctx.inputsFence && g_ctx.inputsFenceValue) {
        mark = qpcNow();
        VkSemaphoreWaitInfo wait{};
        wait.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        wait.semaphoreCount = 1;
        wait.pSemaphores = &g_ctx.inputsFence;
        wait.pValues = &g_ctx.inputsFenceValue;
        if (vkWaitSemaphores(g_ctx.dev, &wait, 1000000000ull) != VK_SUCCESS) return -4;
        g_ctx.tWaitInputs = qpcMs(mark);
    } else {
        g_ctx.tWaitInputs = 0;
    }
    mark = qpcNow();
    HANDLE objs[] = {g_ctx.finalTex.registered, g_ctx.hudlessTex.registered,
                     g_ctx.depthTex.registered, g_ctx.motionTex.registered,
                     g_ctx.worldTex.registered};
    if (!pLockObjects(g_ctx.interop, g_ctx.worldTex.valid() ? 5 : 4, objs)) return -2;
    g_ctx.tDxLock = qpcMs(mark);
    g_ctx.locked = true;
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeUnlock(JNIEnv*, jclass) {
    if (!unlockInputs()) {
        logf("[dlssmc] nativeUnlock 失败\n");
        return -1;
    }
    return 0;
}

// 轻量预检：只做一次 1 像素回读，判断画面有没有真的经桥传过来。
// 返回 1 = 有内容（可以接管上屏），0 = 还是黑的（绝不能接管，否则就是黑屏）
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeProbe(JNIEnv*, jclass) {
    if (!g_ctx.dev || !g_ctx.ready || !inputsValid()) return 0;
    if (g_ctx.takeoverDisabled || g_ctx.locked || g_ctx.outLocked) return 0;
    const uint32_t slot = g_ctx.frameSlot;
    if (!waitForInputs(UINT64_MAX) || !beginCommands(slot)) return 0;
    VkCommandBuffer cmd = g_ctx.cmdRing[slot];

    // hudless 布局先转过来才能拷
    if (g_ctx.hudlessTex.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        VkImageMemoryBarrier hb{};
        hb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        hb.oldLayout = g_ctx.hudlessTex.layout;
        hb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        hb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        hb.image = g_ctx.hudlessTex.image;
        hb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        hb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        hb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &hb);
        g_ctx.hudlessTex.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    if (g_ctx.finalTex.layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
        VkImageMemoryBarrier fb{};
        fb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        fb.oldLayout = g_ctx.finalTex.layout;
        fb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        fb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fb.image = g_ctx.finalTex.image;
        fb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        fb.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        fb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &fb);
        g_ctx.finalTex.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }

    probeSources(cmd);
    if (!submitAndWait(slot)) {
        g_ctx.ready = false;
        return 0;
    }

    g_ctx.blackFrames = 0;
    return readProbeBuffer() ? 1 : 0;
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeSetEnabled(JNIEnv*, jclass, jint on) {
    if (!g_ctx.dev) return -1;
    if (on == 0) {
        if (!stopFg()) return -2;
        g_ctx.fgEnabled = false;
        // SR keeps its token and textures even without a presentation swapchain.
        if (!srActive()) g_ctx.token = nullptr;
    } else {
        if (!g_ctx.swapchain && !createSwapchain()) return -2;
        g_ctx.fgEnabled = true;
        g_ctx.takeoverDisabled = false;
        g_ctx.needReset = true;
    }
    updateReady();
    logf("[dlssmc] nativeSetEnabled(%d)\n", (int)on);
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeGetPresentedCount(JNIEnv*, jclass) {
    return (jint)g_ctx.lastPresentedCount;
}

// [DLSS-G 模型, DLSS-SR 模型, Streamline SDK]，取自插件目录里那份 DLL 自己的文件版本
JNIEXPORT jobjectArray JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeGetModelVersions(JNIEnv* env, jclass) {
    auto labelled = [](const char* label, const wchar_t* file) -> std::string {
        if (g_pluginDir.empty()) return std::string(label) + " 未初始化";
        return std::string(label) + " " + fileVersionOf(g_pluginDir + L"\\" + file);
    };
    std::string fg = labelled("DLSS-G", L"nvngx_dlssg.dll");
    std::string sr = labelled("DLSS-SR", L"nvngx_dlss.dll");
    char slv[40];
    snprintf(slv, sizeof(slv), "Streamline %d.%d.%d", SL_VERSION_MAJOR, SL_VERSION_MINOR,
             SL_VERSION_PATCH);

    jclass str = env->FindClass("java/lang/String");
    jobjectArray out = env->NewObjectArray(3, str, nullptr);
    const char* vals[3] = {fg.c_str(), sr.c_str(), slv};
    for (int i = 0; i < 3; ++i) {
        jstring s = env->NewStringUTF(vals[i]);
        env->SetObjectArrayElement(out, i, s);
        env->DeleteLocalRef(s);
    }
    return out;
}

// DLSS-G 底层开关，逐帧下发；下一帧重建 DLSSGOptions 时生效
JNIEXPORT void JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeSetTuning(JNIEnv*, jclass, jboolean showOnly,
                                                                jboolean retainResources,
                                                                jboolean menuDetection,
                                                                jboolean queueParallelism,
                                                                jboolean uiRecomposition) {
    g_ctx.fgShowOnlyInterpolated = showOnly != JNI_FALSE;
    g_ctx.fgRetainResources = retainResources != JNI_FALSE;
    g_ctx.fgMenuDetection = menuDetection != JNI_FALSE;
    g_ctx.fgQueueParallelism = queueParallelism != JNI_FALSE;
    bool ui = uiRecomposition != JNI_FALSE;
    if (ui != g_ctx.fgUiRecomposition) {
        g_ctx.fgUiRecomposition = ui;
        g_ctx.needReset = true;   // 换 UI 重合成要丢历史，避免残影
    }
}

// 桥接路径自身的分段耗时 [等上一轮帧, acquire, 提交, 呈现, 取状态, 锁-提交fence, 锁-输入fence, 锁-DX]
JNIEXPORT jdoubleArray JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeGetBridgeTimings(JNIEnv* env, jclass) {
    double v[8] = {g_ctx.tSlotWait, g_ctx.tAcquire, g_ctx.tSubmit, g_ctx.tPresent,
                   g_ctx.tState, g_ctx.tWaitSubmit, g_ctx.tWaitInputs, g_ctx.tDxLock};
    jdoubleArray out = env->NewDoubleArray(8);
    env->SetDoubleArrayRegion(out, 0, 8, v);
    return out;
}

// [模拟→Present, 模拟, 渲染提交, 驱动队列, GPU 渲染, Present, 是否有效]，毫秒，非屏幕延迟
JNIEXPORT jdoubleArray JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeGetLatency(JNIEnv* env, jclass) {
    double v[7] = {
        g_ctx.latValid ? g_ctx.latTotal : -1.0,
        g_ctx.latValid ? g_ctx.latSim : 0.0,
        g_ctx.latValid ? g_ctx.latSubmit : 0.0,
        g_ctx.latValid ? g_ctx.latQueue : 0.0,
        g_ctx.latValid ? g_ctx.latGpu : 0.0,
        g_ctx.latValid ? g_ctx.latPresent : 0.0,
        g_ctx.latValid ? 1.0 : 0.0};
    jdoubleArray out = env->NewDoubleArray(7);
    env->SetDoubleArrayRegion(out, 0, 7, v);
    return out;
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeResize(JNIEnv*, jclass, jint width,
                                                              jint height) {
    if (!g_ctx.dev) return -1;
    if (width <= 0 || height <= 0) return -4;
    g_ctx.ready = false;
    if (!stopFg() || !releaseInputs()) return -3;
    g_ctx.width = (uint32_t)width;
    g_ctx.height = (uint32_t)height;
    // Retain quality/preset/sharpness, but resend options with the new display size.
    g_ctx.srOptionsSet = false;
    if (g_ctx.fgEnabled && !createSwapchain()) return -2;
    g_ctx.needReset = true;
    return 0;
}

// 解绑所有输入和 SR 输出，保留 Streamline 与 Vulkan 设备。
// 切到 FSR / XeSS 时要用它：进程内再 slInit 会踩失效的插件函数指针（跨存档崩溃的根因），
// 所以那条路只能「不用」而不能「关掉重建」。
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeUnbindTextures(JNIEnv*, jclass) {
    if (!g_ctx.dev) return -1;
    g_ctx.ready = false;
    if (!stopFg() || !releaseInputs()) return -2;
    logf("[dlssmc] 输入纹理已解绑（设备与 Streamline 保留）\n");
    return 0;
}

JNIEXPORT void JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeHidePresentation(JNIEnv*, jclass) {
    if (g_ctx.presentationVisible) {
        ShowWindow(g_ctx.presentationWindow, SW_HIDE);
        g_ctx.presentationVisible = false;
    }
}

JNIEXPORT void JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeShutdown(JNIEnv*, jclass) {
    logf("[dlssmc] FG shutdown\n");
    g_ctx.fgEnabled = false;
    if (g_ctx.dev &&
        Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeUnbindTextures(nullptr, nullptr) != 0) {
        logf("[dlssmc] shutdown: resource synchronization/release failed\n");
        return;
    }
    if (g_ctx.probeBuffer) vkDestroyBuffer(g_ctx.dev, g_ctx.probeBuffer, nullptr);
    if (g_ctx.probeMemory) vkFreeMemory(g_ctx.dev, g_ctx.probeMemory, nullptr);
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        if (g_ctx.fenceRing[i]) vkDestroyFence(g_ctx.dev, g_ctx.fenceRing[i], nullptr);
        if (g_ctx.renderDoneRing[i]) vkDestroySemaphore(g_ctx.dev, g_ctx.renderDoneRing[i], nullptr);
        if (g_ctx.imageAvailRing[i]) vkDestroySemaphore(g_ctx.dev, g_ctx.imageAvailRing[i], nullptr);
    }
    if (g_ctx.pool) vkDestroyCommandPool(g_ctx.dev, g_ctx.pool, nullptr);
    if (g_ctx.interop && pCloseDevice) pCloseDevice(g_ctx.interop);
    // Final client shutdown only: unloading SL invalidates its cached plugin function pointers.
    if (g_ctx.slInited) slShutdown();
    if (g_ctx.dev) vkDestroyDevice(g_ctx.dev, nullptr);
    if (g_ctx.inst) vkDestroyInstance(g_ctx.inst, nullptr);
    g_ctx = FgContext{};
    g_lastStatus = -999;
    g_lastTagErr = -999;
    g_logCounter = 0;
    if (g_log) {
        fclose(g_log);
        g_log = nullptr;
    }
}

} // extern "C"
