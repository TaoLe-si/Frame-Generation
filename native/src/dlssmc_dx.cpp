// 帧生成的 D3D12 后端 - 原生层
//
// 为什么另起一个模块：DLSS 帧生成走 GL -> D3D11(KMT) -> Vulkan 导入（dlssmc_fg.cpp）。
// FSR(SDK 2.3) 与 XeSS-FG(3.0.2) 只有 D3D12 前端，而本机实测（native/test/delivery_probe.exe）：
//   - 只有 MISC_SHARED(KMT) 那张能被 GL 注册，D3D12 直接打开它读回全 0；
//   - 可行做法是 D3D11 内部 CopyResource 到一张 SHARED|SHARED_NTHANDLE、BindFlags=0 的纹理，
//     再让 D3D12 OpenSharedHandle —— 一帧四个 1080p 输入约 0.32 ms，且必须 Flush + fence 同步。
// 本模块就是照这条路实现的。
//
// 已实测的厂商约束（native/test/xess_probe.exe、fsr_probe.exe）：
//   - XeSS：必须 GPU 空闲 -> xellSetSleepMode(bLowLatencyMode=1) -> 才 xefgSwapChainSetEnabled(1)，
//     顺序反了库会报 "Latency reduction is not enabled." 并整场只出 1 帧；
//     销毁代理 swapchain 上下文前必须先放掉它的引用；进程内不要 FreeLibrary 厂商 DLL。
//   - FSR：provider 必须自己 LoadLibrary 进进程（只载 loader 会 NO_PROVIDER）；
//     逐帧 Configure -> Prepare（录进打开的命令列表）-> Close/Execute -> Present。
//
// 编译：native/build_dx.sh

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3d11_3.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <jni.h>

#include <xefg_swapchain.h>
#include <xefg_swapchain_d3d12.h>
#include <xell.h>
#include <xell_d3d12.h>
#include <xess.h>
#include <xess_d3d12.h>

#include "api/include/ffx_api.h"
#include "api/include/dx12/ffx_api_dx12.h"
#include "framegeneration/include/ffx_framegeneration.h"
#include "framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"
#include "upscaling/include/ffx_upscale.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#define DLSSMC_GL_TEXTURE_2D 0x0DE1
#define WGL_ACCESS_READ_WRITE_NV 0x0001
#define kFramesInFlight 3

using PFN_wglGetProcAddress = void*(WINAPI*)(LPCSTR);
using PFN_wglDXOpenDeviceNV = HANDLE(WINAPI*)(void*);
using PFN_wglDXRegisterObjectNV = HANDLE(WINAPI*)(HANDLE, void*, unsigned, unsigned, unsigned);
using PFN_wglDXUnregisterObjectNV = BOOL(WINAPI*)(HANDLE, HANDLE);
using PFN_wglDXLockObjectsNV = BOOL(WINAPI*)(HANDLE, int, HANDLE*);
using PFN_wglDXUnlockObjectsNV = BOOL(WINAPI*)(HANDLE, int, HANDLE*);
using PFN_wglDXCloseDeviceNV = BOOL(WINAPI*)(HANDLE);

static PFN_wglDXOpenDeviceNV pOpenDevice;
static PFN_wglDXRegisterObjectNV pRegisterObject;
static PFN_wglDXUnregisterObjectNV pUnregisterObject;
static PFN_wglDXLockObjectsNV pLockObjects;
static PFN_wglDXUnlockObjectsNV pUnlockObjects;
static PFN_wglDXCloseDeviceNV pCloseDevice;

static FILE* g_log = nullptr;
static void logf(const char* fmt, ...) {
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fflush(g_log);
}

enum Backend { kBackendPassthrough = 0, kBackendXeSS = 1, kBackendFSR = 2 };

struct Input {
    unsigned glTex = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t w = 0, h = 0;            // 这一路自己的尺寸：颜色跟呈现分辨率，深度/MV/世界颜色跟渲染分辨率
    ComPtr<ID3D11Texture2D> kmt;      // 唯一能被 GL 注册的那张
    ComPtr<ID3D11Texture2D> nt;       // 跨 API 目标，GL 看不见它
    HANDLE reg = nullptr;             // wglDXRegisterObjectNV 的句柄
    HANDLE ntHandle = nullptr;        // 交给 D3D12 的 NT 句柄
    ComPtr<ID3D12Resource> d12;
};

struct DxCtx {
    uint32_t w = 0, h = 0;
    // 渲染（SR 输入）分辨率；SR 关时与 w,h 相同。只有深度/MV/世界颜色这一路跟它走
    // （Intel FG 指南：MV「优先低分辨率」、depth 与 MV 同尺寸即可；FSR 的 prepare 直接收 renderSize），
    // 交给 FG 的两张颜色和代理 swapchain 一律是 w,h —— 超分的输出已经在 nativeUpscale 里
    // 反向写回 GL 的主 target，FG 看到的颜色天然是呈现分辨率的。
    uint32_t rw = 0, rh = 0;
    int srQuality = -1;   // <0=关；否则是各家自己的档位枚举（见 nativeConfigureSR）
    bool srSharpen = false;
    float srSharpness = 0.f;
    ComPtr<ID3D12Resource> srOut;
    D3D12_RESOURCE_STATES srOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    HWND parent = nullptr;
    HWND child = nullptr;
    bool childVisible = false;
    Backend backend = kBackendPassthrough;

    // D3D11（只为拿 GL 互操作 + 跨 API 拷贝）
    ComPtr<ID3D11Device> d3d11;
    ComPtr<ID3D11DeviceContext> ctx11;
    ComPtr<ID3D11Device5> dev11_5;
    ComPtr<ID3D11DeviceContext4> ctx11_4;
    ComPtr<ID3D11Fence> fence11;
    HANDLE evt11 = nullptr;
    uint64_t fence11Value = 0;
    HANDLE interop = nullptr;

    // D3D12
    ComPtr<ID3D12Device> dev12;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc[kFramesInFlight];
    ComPtr<ID3D12GraphicsCommandList> cmd[kFramesInFlight];
    ComPtr<ID3D12Fence> fence12;
    HANDLE evt12 = nullptr;
    uint64_t fence12Value = 0;
    uint32_t slot = 0;

    ComPtr<IDXGIFactory2> factory;
    // 代理 swapchain 也是 IDXGISwapChain4，统一按 3 拿（要 GetCurrentBackBufferIndex）
    ComPtr<IDXGISwapChain3> swapchain;

    // 0=final(呈现) 1=hudless(呈现) 2=depth(渲染) 3=motion(渲染) 4=世界颜色(渲染，SR 的输入)
    // 5=SR 输出(呈现，方向反过来：D3D12 写、GL 读。这是我们自己的暂存纹理，Java 再 blit 进主 target)
    // SR 时按 2..4、0 分段交付；1 由 SR 直接写，避免 GL 旧内容覆盖它。
    Input in[6];
    int lockFrom = 0, lockCount = 0;   // 当前锁住的段，nativeDeliver 照它搬
    // HUD-less 的状态必须由实际屏障确定，不能只按厂商声明赋值。
    // NT 共享纹理初始为 COMMON，SIMULTANEOUS_ACCESS 纹理在每次 Execute 完成后也衰减为 COMMON。
    D3D12_RESOURCE_STATES in1State = D3D12_RESOURCE_STATE_COMMON;
    bool locked = false;
    bool outLocked = false;             // in[5] 正被 GL 读着（nativeUpscale 与 nativeUpscaleDone 之间）
    bool ready = false;
    bool haveInputs = false;

    // XeSS
    HMODULE hXell = nullptr, hXefg = nullptr;
    xell_context_handle_t xell = nullptr;
    xefg_swapchain_handle_t xefg = nullptr;
    decltype(&xellD3D12CreateContext) pXellCreate = nullptr;
    decltype(&xellDestroyContext) pXellDestroy = nullptr;
    decltype(&xellSetSleepMode) pXellSleepMode = nullptr;
    decltype(&xellSleep) pXellSleep = nullptr;
    decltype(&xellAddMarkerData) pXellMarker = nullptr;
    decltype(&xellGetVersion) pXellVersion = nullptr;
    decltype(&xefgSwapChainD3D12CreateContext) pXefgCreate = nullptr;
    decltype(&xefgSwapChainDestroy) pXefgDestroy = nullptr;
    decltype(&xefgSwapChainSetLatencyReduction) pXefgSetLl = nullptr;
    decltype(&xefgSwapChainD3D12GetProperties) pXefgProps = nullptr;
    decltype(&xefgSwapChainD3D12InitFromSwapChainDesc) pXefgInit = nullptr;
    decltype(&xefgSwapChainD3D12GetSwapChainPtr) pXefgGetSc = nullptr;
    decltype(&xefgSwapChainD3D12TagFrameResource) pXefgTagRes = nullptr;
    decltype(&xefgSwapChainTagFrameConstants) pXefgTagConst = nullptr;
    decltype(&xefgSwapChainSetEnabled) pXefgEnable = nullptr;
    decltype(&xefgSwapChainSetPresentId) pXefgPresentId = nullptr;
    decltype(&xefgSwapChainSetSceneChangeThreshold) pXefgSetThreshold = nullptr;
    decltype(&xefgSwapChainGetLastPresentStatus) pXefgStatus = nullptr;
    uint32_t maxInterpolations = 0;
    uint32_t presentId = 0;
    uint32_t lastFramesPresented = 0;
    std::string xellVersion, xefgProviderNote;

    // XeSS-SR（libxess.dll，与 FG 的 libxess_fg.dll 是两个模块，导出互不包含）
    HMODULE hXess = nullptr;
    xess_context_handle_t xess = nullptr;
    decltype(&xessD3D12CreateContext) pXessCreate = nullptr;
    decltype(&xessD3D12Init) pXessInit = nullptr;
    decltype(&xessD3D12Execute) pXessExec = nullptr;
    decltype(&xessDestroyContext) pXessDestroy = nullptr;
    decltype(&xessGetVersion) pXessVersion = nullptr;
    decltype(&xessSetVelocityScale) pXessVelScale = nullptr;
    decltype(&xessForceLegacyScaleFactors) pXessForceLegacy = nullptr;
    bool xessLegacyScale = false;   // 传统档位倍率（平衡 1.7x 等），影响下次 initXessSr
    decltype(&xessGetOptimalInputResolution) pXessOptimalInput = nullptr;
    uint32_t velRW = 0, velRH = 0;   // 已经生效的 MV 换算尺寸（变了才重设 velocityScale）
    bool srSizeLogged = false;
    std::string xessSrVersion;

    // FSR-SR（核心 upscaler provider，与 FG provider 并存于进程）
    HMODULE hFfxUpscaler = nullptr;
    ffxContext ffxUpscale = nullptr;
    uint64_t fsrUpscaleVersion = 0;
    std::string fsrUpscaleProvider;

    // FSR
    HMODULE hFfxLoader = nullptr, hFfxProvider = nullptr;
    PfnFfxCreateContext pFfxCreate = nullptr;
    PfnFfxDestroyContext pFfxDestroy = nullptr;
    PfnFfxQuery pFfxQuery = nullptr;
    PfnFfxConfigure pFfxConfigure = nullptr;
    PfnFfxDispatch pFfxDispatch = nullptr;
    ffxContext ffxEffect = nullptr;
    ffxContext ffxSwap = nullptr;
    uint64_t ffxFrameId = 0;
    LARGE_INTEGER ffxLastFrame{}, ffxLastUpscale{};
    LARGE_INTEGER xessLastFrame{};   // XeSS-FG 的真实帧间隔（frameRenderTime 用）
    uint32_t ffxGenerated = 0;   // 成功 dispatch 的生成数，不是显示器实际呈现遥测
    // FSR 调优（Java 经 nativeSetFsrTuning 下发）：生成工作跑异步计算 / 只呈现插帧
    bool ffxAsyncWorkloads = false;
    bool ffxOnlyGenerated = false;
    std::wstring fsrDir;
    std::string fsrProvider, fsrBuiltinDll, xessBuiltinDll;

    // 桥接分段计时（诊断卡顿用）：各阶段毫秒累计，窗口平均后定期写日志。
    // 锁等 = nativeLock 里 D3D12 排水；交付 = nativeDeliver 的 D3D11 拷贝+落地；
    // 超分 = nativeUpscale 里的两次等待；呈现 = nativePresent 全程（含 vsync 阻塞）。
    double accLockWait = 0, accDeliver = 0, accUpscale = 0, accPresent = 0;
    uint32_t accFrames = 0;
};

static DxCtx g;

static std::wstring utf8ToWide(const char* s) {
    if (!s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

// ------------------------------------------------------------------ 呈现子窗口
// 与 Vulkan 那条路同样的做法：贴在 MC 窗口上的 WS_CHILD，不吃鼠标也不抢焦点。
static LRESULT CALLBACK childProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

static bool ensureChildWindow(HWND parent, uint32_t w, uint32_t h) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = childProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"dlssmc_dx_present";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        registered = true;
    }
    if (g.child) {
        SetWindowPos(g.child, nullptr, 0, 0, (int)w, (int)h, SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
    }
    g.child = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, L"dlssmc_dx_present", L"",
                              WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, (int)w, (int)h,
                              parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    return g.child != nullptr;
}

// ------------------------------------------------------------------ 交付路径
// GL 写的 KMT -> D3D11 内部 CopyResource -> 跨 API NT 纹理 -> D3D12 OpenSharedHandle
// 反向（SR 输出）走同一条链的相反方向：D3D12 写 d12 -> 栅栏 -> D3D11 CopyResource(kmt<-nt)
//   -> Flush -> 解锁，之后 GL 才能读。DLSS 那条 Streamline 路已经在本机验证过这个方向。
static bool makeInputChain(Input& t, unsigned glTex, DXGI_FORMAT fmt, uint32_t w, uint32_t h) {
    D3D11_TEXTURE2D_DESC base{};
    base.Width = w;
    base.Height = h;
    base.MipLevels = 1;
    base.ArraySize = 1;
    base.Format = fmt;
    base.SampleDesc.Count = 1;
    base.Usage = D3D11_USAGE_DEFAULT;

    base.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    base.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    if (FAILED(g.d3d11->CreateTexture2D(&base, nullptr, &t.kmt))) return false;
    t.reg = pRegisterObject(g.interop, t.kmt.Get(), glTex, DLSSMC_GL_TEXTURE_2D,
                            WGL_ACCESS_READ_WRITE_NV);
    if (!t.reg) return false;

    // 跨 API 共享的硬要求：BindFlags=0、CPUAccessFlags=0、单采样
    base.BindFlags = 0;
    base.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if (FAILED(g.d3d11->CreateTexture2D(&base, nullptr, &t.nt))) return false;
    ComPtr<IDXGIResource1> res1;
    if (FAILED(t.nt.As(&res1))) return false;
    if (FAILED(res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ |
                                                DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                        &t.ntHandle))) {
        return false;
    }
    if (FAILED(g.dev12->OpenSharedHandle(t.ntHandle, IID_PPV_ARGS(t.d12.GetAddressOf())))) return false;
    t.glTex = glTex;
    t.fmt = fmt;
    t.w = w;
    t.h = h;
    return true;
}

static void releaseInput(Input& t) {
    t.d12.Reset();
    if (t.ntHandle) {
        CloseHandle(t.ntHandle);
        t.ntHandle = nullptr;
    }
    if (t.reg && g.interop) {
        pUnregisterObject(g.interop, t.reg);
        t.reg = nullptr;
    }
    t.nt.Reset();
    t.kmt.Reset();
    t.glTex = 0;
    t.fmt = DXGI_FORMAT_UNKNOWN;
    t.w = t.h = 0;
}

static void wait11() {
    if (!g.fence11) return;
    g.ctx11->Flush();
    // 本 SDK 头里 ID3D11Fence 没有 Signal，触发要经 ID3D11DeviceContext4
    g.ctx11_4->Signal(g.fence11.Get(), ++g.fence11Value);
    g.fence11->SetEventOnCompletion(g.fence11Value, g.evt11);
    // 不等到就等于让 D3D12 读一个还没落地的拷贝（实测会读到 0）
    WaitForSingleObject(g.evt11, 5000);
}

static void wait12() {
    if (!g.fence12) return;
    g.queue->Signal(g.fence12.Get(), ++g.fence12Value);
    g.fence12->SetEventOnCompletion(g.fence12Value, g.evt12);
    WaitForSingleObject(g.evt12, 5000);
}

// ------------------------------------------------------------------ 桥接计时
// 诊断卡顿：全量 CPU-GPU 同步每帧有四次（锁等/交付/超分/呈现），
// 哪一段在吃时间必须靠数据说话。窗口平均后定期写一行日志。
static double qpcSinceMs(LARGE_INTEGER since) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return (now.QuadPart - since.QuadPart) * 1000.0 / freq.QuadPart;
}

static void logBridgeTimingsMaybe() {
    if (++g.accFrames < 600) return;
    // SR 关着时超分段恒为 0（那段没跑），平均值照常输出即可读
    logf("[dlssmc-dx] 桥接均值ms(%u帧)：锁等 %.2f 交付 %.2f 超分 %.2f 呈现 %.2f\n",
         g.accFrames, g.accLockWait / g.accFrames, g.accDeliver / g.accFrames,
         g.accUpscale / g.accFrames, g.accPresent / g.accFrames);
    g.accFrames = 0;
    g.accLockWait = g.accDeliver = g.accUpscale = g.accPresent = 0;
}

// ------------------------------------------------------------------ XeSS
static bool loadXeSS(const std::wstring& dir) {
    g.hXell = LoadLibraryW((dir + L"\\libxell.dll").c_str());
    DWORD e1 = GetLastError();
    g.hXefg = LoadLibraryW((dir + L"\\libxess_fg.dll").c_str());
    DWORD e2 = GetLastError();
    if (!g.hXell || !g.hXefg) {
        // 路径与错误码都要留：126=依赖缺失，2=路径不对，两者修法完全不同
        logf("[dlssmc-dx] 厂商 DLL 加载失败 dir=%ls xell=%p(%lu) xefg=%p(%lu)\n", dir.c_str(),
             (void*)g.hXell, e1, (void*)g.hXefg, e2);
        return false;
    }
    auto both = [&](const char* name) {
        void* p = (void*)GetProcAddress(g.hXefg, name);
        if (!p) p = (void*)GetProcAddress(g.hXell, name);
        return p;
    };
    g.pXellCreate = (decltype(g.pXellCreate))both("xellD3D12CreateContext");
    g.pXellDestroy = (decltype(g.pXellDestroy))both("xellDestroyContext");
    g.pXellSleepMode = (decltype(g.pXellSleepMode))both("xellSetSleepMode");
    g.pXellSleep = (decltype(g.pXellSleep))both("xellSleep");
    g.pXellMarker = (decltype(g.pXellMarker))both("xellAddMarkerData");
    g.pXellVersion = (decltype(g.pXellVersion))both("xellGetVersion");
    g.pXefgCreate = (decltype(g.pXefgCreate))both("xefgSwapChainD3D12CreateContext");
    g.pXefgDestroy = (decltype(g.pXefgDestroy))both("xefgSwapChainDestroy");
    g.pXefgSetLl = (decltype(g.pXefgSetLl))both("xefgSwapChainSetLatencyReduction");
    g.pXefgProps = (decltype(g.pXefgProps))both("xefgSwapChainD3D12GetProperties");
    g.pXefgInit = (decltype(g.pXefgInit))both("xefgSwapChainD3D12InitFromSwapChainDesc");
    g.pXefgGetSc = (decltype(g.pXefgGetSc))both("xefgSwapChainD3D12GetSwapChainPtr");
    g.pXefgTagRes = (decltype(g.pXefgTagRes))both("xefgSwapChainD3D12TagFrameResource");
    g.pXefgTagConst = (decltype(g.pXefgTagConst))both("xefgSwapChainTagFrameConstants");
    g.pXefgEnable = (decltype(g.pXefgEnable))both("xefgSwapChainSetEnabled");
    g.pXefgPresentId = (decltype(g.pXefgPresentId))both("xefgSwapChainSetPresentId");
    // 可选导出：旧版 libxess_fg.dll 没有，缺了只是没有这个调优点
    g.pXefgSetThreshold =
        (decltype(g.pXefgSetThreshold))GetProcAddress(g.hXefg, "xefgSwapChainSetSceneChangeThreshold");
    g.pXefgStatus = (decltype(g.pXefgStatus))both("xefgSwapChainGetLastPresentStatus");
    const bool complete = g.pXellCreate && g.pXellSleepMode && g.pXellSleep && g.pXellMarker &&
                          g.pXefgCreate && g.pXefgSetLl && g.pXefgProps && g.pXefgInit &&
                          g.pXefgGetSc && g.pXefgTagRes && g.pXefgTagConst && g.pXefgEnable &&
                          g.pXefgPresentId && g.pXefgStatus;
    if (!complete) logf("[dlssmc-dx] XeSS 导出缺失\n");

    // 超分是另一个模块：libxess_fg.dll 里一个 SR 导出都没有（dumpbin 实测），必须单独载 libxess.dll。
    // 载不到只是没有超分，插帧照常，所以不并进上面的 complete。
    g.hXess = LoadLibraryW((dir + L"\\libxess.dll").c_str());
    if (!g.hXess) {
        logf("[dlssmc-dx] libxess.dll 加载失败(%lu)，超分不可用\n", GetLastError());
    } else {
        g.pXessCreate = (decltype(g.pXessCreate))GetProcAddress(g.hXess, "xessD3D12CreateContext");
        g.pXessInit = (decltype(g.pXessInit))GetProcAddress(g.hXess, "xessD3D12Init");
        g.pXessExec = (decltype(g.pXessExec))GetProcAddress(g.hXess, "xessD3D12Execute");
        g.pXessDestroy = (decltype(g.pXessDestroy))GetProcAddress(g.hXess, "xessDestroyContext");
        g.pXessVersion = (decltype(g.pXessVersion))GetProcAddress(g.hXess, "xessGetVersion");
        g.pXessVelScale = (decltype(g.pXessVelScale))GetProcAddress(g.hXess, "xessSetVelocityScale");
        g.pXessForceLegacy =
            (decltype(g.pXessForceLegacy))GetProcAddress(g.hXess, "xessForceLegacyScaleFactors");
        g.pXessOptimalInput =
            (decltype(g.pXessOptimalInput))GetProcAddress(g.hXess, "xessGetOptimalInputResolution");
        if (!(g.pXessCreate && g.pXessInit && g.pXessExec && g.pXessDestroy)) {
            logf("[dlssmc-dx] libxess.dll 超分导出缺失，超分不可用\n");
            g.hXess = nullptr;
        } else if (g.pXessVersion) {
            xess_version_t v{};
            if (g.pXessVersion(&v) == XESS_RESULT_SUCCESS) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%u.%u.%u", v.major, v.minor, v.patch);
                g.xessSrVersion = buf;
            }
        }
    }
    return complete;
}

static bool initXeSSSwapchain() {
    if (g.pXellCreate(g.dev12.Get(), &g.xell) != XELL_RESULT_SUCCESS) {
        logf("[dlssmc-dx] xellD3D12CreateContext 失败\n");
        return false;
    }
    if (g.pXefgCreate(g.dev12.Get(), &g.xefg) != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        logf("[dlssmc-dx] xefgSwapChainD3D12CreateContext 失败\n");
        return false;
    }
    g.pXefgSetLl(g.xefg, g.xell);

    xefg_swapchain_d3d12_init_params_t params{};
    params.pApplicationSwapChain = nullptr;
    params.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
    xefg_swapchain_properties_t props{};
    if (g.pXefgProps(g.xefg, &params, g.w, g.h, DXGI_FORMAT_R8G8B8A8_UNORM, &props) !=
        XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        logf("[dlssmc-dx] GetProperties 失败\n");
        return false;
    }
    g.maxInterpolations = props.maxSupportedInterpolations;
    logf("[dlssmc-dx] maxSupportedInterpolations=%u 临时纹理堆=%llu 缓冲堆=%llu 描述符=%u\n",
         g.maxInterpolations, (unsigned long long)props.tempTextureHeapSize,
         (unsigned long long)props.tempBufferHeapSize, props.requiredDescriptorCount);

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = g.w;
    scd.Height = g.h;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferCount = 3;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (g.pXefgInit(g.xefg, g.child, &scd, nullptr, g.queue.Get(), g.factory.Get(), &params) !=
        XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        logf("[dlssmc-dx] InitFromSwapChainDesc 失败\n");
        return false;
    }
    g.swapchain.Reset();
    if (FAILED(g.pXefgGetSc(g.xefg, IID_PPV_ARGS(g.swapchain.GetAddressOf())))) {
        logf("[dlssmc-dx] GetSwapChainPtr 失败\n");
        return false;
    }

    // 实测过的顺序：GPU 空闲 -> 打开 XeLL 低延迟 -> 才允许开 FG，
    // 反过来会拿到 "Latency reduction is not enabled." 然后永远只出 1 帧。
    wait12();
    xell_sleep_params_t sp{};
    sp.bLowLatencyMode = 1;
    sp.minimumIntervalUs = 0;
    if (g.pXellSleepMode(g.xell, &sp) != XELL_RESULT_SUCCESS) {
        logf("[dlssmc-dx] xellSetSleepMode 失败\n");
        return false;
    }
    g.pXefgEnable(g.xefg, 1);
    return true;
}

// ------------------------------------------------------------------ FSR
// 代理 swapchain 请求生成时回调。只累计成功派发返回的生成数；它不证明帧已实际显示。
static ffxReturnCode_t fsrGenerateCb(ffxDispatchDescFrameGeneration* params, void* ctx) {
    const ffxReturnCode_t rc = g.pFfxDispatch((ffxContext*)ctx, &params->header);
    if (rc == FFX_API_RETURN_OK) g.ffxGenerated += params->numGeneratedFrames;
    return rc;
}

static std::string dllVersionOf(const std::wstring& path) {
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

static bool loadFfx(const std::wstring& dir) {
    g.hFfxLoader = LoadLibraryW((dir + L"\\amd_fidelityfx_loader_dx12.dll").c_str());
    DWORD e1 = GetLastError();
    // provider 必须自己载进进程：只载 loader 时 GET_VERSIONS 回 NO_PROVIDER（fsr_probe 实测）
    g.hFfxProvider = LoadLibraryW((dir + L"\\amd_fidelityfx_framegeneration_dx12.dll").c_str());
    DWORD e2 = GetLastError();
    if (!g.hFfxLoader || !g.hFfxProvider) {
        logf("[dlssmc-dx] FFX DLL 加载失败 dir=%ls loader=%p(%lu) provider=%p(%lu)\n", dir.c_str(),
             (void*)g.hFfxLoader, e1, (void*)g.hFfxProvider, e2);
        return false;
    }
    g.pFfxCreate = (PfnFfxCreateContext)GetProcAddress(g.hFfxLoader, "ffxCreateContext");
    g.pFfxDestroy = (PfnFfxDestroyContext)GetProcAddress(g.hFfxLoader, "ffxDestroyContext");
    g.pFfxQuery = (PfnFfxQuery)GetProcAddress(g.hFfxLoader, "ffxQuery");
    g.pFfxConfigure = (PfnFfxConfigure)GetProcAddress(g.hFfxLoader, "ffxConfigure");
    g.pFfxDispatch = (PfnFfxDispatch)GetProcAddress(g.hFfxLoader, "ffxDispatch");
    const bool complete = g.pFfxCreate && g.pFfxDestroy && g.pFfxQuery && g.pFfxConfigure &&
                          g.pFfxDispatch;
    if (!complete) logf("[dlssmc-dx] FFX loader 导出缺失\n");
    // 超分的 provider 也是单独一个 DLL，和 FG provider 并存；同样只影响超分可用性
    g.hFfxUpscaler = LoadLibraryW((dir + L"\\amd_fidelityfx_upscaler_dx12.dll").c_str());
    if (!g.hFfxUpscaler)
        logf("[dlssmc-dx] amd_fidelityfx_upscaler_dx12.dll 加载失败(%lu)，超分不可用\n",
             GetLastError());
    return complete;
}

// provider 版本必须问库自己（本机 FG provider 就与包头常量对不上），upscaler 同理。
// 需要 D3D12 设备，所以在建完设备后调。
static void queryFfxUpscaleVersion() {
    if (!g.hFfxUpscaler || !g.pFfxQuery || !g.dev12) return;
    uint64_t count = 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = g.dev12.Get();
    q.outputCount = &count;
    if (g.pFfxQuery(nullptr, (ffxQueryDescHeader*)&q) != FFX_API_RETURN_OK || count == 0 || count >= 8)
        return;
    std::vector<uint64_t> ids((size_t)count);
    std::vector<const char*> names((size_t)count, nullptr);
    uint64_t cap = count;
    q.outputCount = &cap;
    q.versionIds = ids.data();
    q.versionNames = names.data();
    if (g.pFfxQuery(nullptr, (ffxQueryDescHeader*)&q) != FFX_API_RETURN_OK || cap == 0) return;
    g.fsrUpscaleVersion = ids[0];
    if (names[0]) g.fsrUpscaleProvider = names[0];
    logf("[dlssmc-dx] FSR 超分 provider %s（id=%llu）\n",
         g.fsrUpscaleProvider.empty() ? "无名" : g.fsrUpscaleProvider.c_str(),
         (unsigned long long)ids[0]);
}

static bool initFfx() {
    // 内置版本 = DLL 自己的文件版本；实机版本 = provider 在本机枚举出来的那个
    g.fsrBuiltinDll = dllVersionOf(g.fsrDir + L"\\amd_fidelityfx_framegeneration_dx12.dll");
    {
        uint64_t count = 0;
        ffxQueryDescGetVersions q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        q.device = g.dev12.Get();
        q.outputCount = &count;
        if (g.pFfxQuery(nullptr, (ffxQueryDescHeader*)&q) == FFX_API_RETURN_OK && count > 0 &&
            count < 8) {
            std::vector<uint64_t> ids((size_t)count);
            std::vector<const char*> names((size_t)count, nullptr);
            uint64_t cap = count;
            q.outputCount = &cap;
            q.versionIds = ids.data();
            q.versionNames = names.data();
            if (g.pFfxQuery(nullptr, (ffxQueryDescHeader*)&q) == FFX_API_RETURN_OK && cap > 0 &&
                names[0]) {
                g.fsrProvider = names[0];
            }
        }
        logf("[dlssmc-dx] FSR 内置 DLL %s，本机 provider %s\n", g.fsrBuiltinDll.c_str(),
             g.fsrProvider.empty() ? "无" : g.fsrProvider.c_str());
    }
    if (g.fsrProvider.empty()) return false;   // 这台机器上一个 FG provider 都没有

    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = g.dev12.Get();
    ffxCreateContextDescFrameGenerationVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver.header.pNext = (ffxCreateContextDescHeader*)&backend;
    ver.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateContextDescFrameGeneration fg{};
    fg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    fg.header.pNext = (ffxCreateContextDescHeader*)&ver;
    fg.displaySize = {g.w, g.h};
    fg.maxRenderSize = {g.w, g.h};
    fg.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    if (g.pFfxCreate(&g.ffxEffect, (ffxCreateContextDescHeader*)&fg, nullptr) !=
        FFX_API_RETURN_OK) {
        logf("[dlssmc-dx] FSR 效果上下文创建失败\n");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = g.w;
    scd.Height = g.h;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferCount = 3;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGIFactory> f1;
    if (FAILED(g.factory.As(&f1))) return false;
    IDXGISwapChain4* proxy = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 s{};
    s.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    s.header.pNext = (ffxCreateContextDescHeader*)&backend;
    s.swapchain = &proxy;
    s.hwnd = g.child;
    s.desc = &scd;
    s.dxgiFactory = f1.Get();
    s.gameQueue = g.queue.Get();
    if (g.pFfxCreate(&g.ffxSwap, (ffxCreateContextDescHeader*)&s, nullptr) != FFX_API_RETURN_OK ||
        !proxy) {
        logf("[dlssmc-dx] FSR 代理 swapchain 创建失败\n");
        return false;
    }
    if (FAILED(proxy->QueryInterface(IID_PPV_ARGS(&g.swapchain)))) return false;
    proxy->Release();   // g.swapchain 已经持有引用

    ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = g.swapchain.Get();
    // presentCallback 留空：代理自己做「当前画面 -> 真 backbuffer」的搬运。
    // 我们没自己实现是因为 FFX 的 FfxApiResource.state 位域与 D3D12_RESOURCE_STATES 不同值，
    // 头文件里没有公开的转换入口，硬猜状态只会换来不稳定画面。
    cfg.presentCallback = nullptr;
    cfg.frameGenerationCallback = fsrGenerateCb;
    cfg.frameGenerationCallbackUserContext = &g.ffxEffect;
    cfg.frameGenerationEnabled = true;
    // 调优项：异步计算（与其他工作重叠，降开销）/ 只呈现插帧（诊断，对应 DLSS 的同款开关）
    cfg.allowAsyncWorkloads = g.ffxAsyncWorkloads;
    cfg.onlyPresentGenerated = g.ffxOnlyGenerated;
    cfg.allowAsyncWorkloads = false;
    if (g.pFfxConfigure(&g.ffxEffect, (ffxConfigureDescHeader*)&cfg) != FFX_API_RETURN_OK) {
        logf("[dlssmc-dx] FSR 配置失败\n");
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ 超分（SR）
// SR 不在呈现路径上，它在世界阶段收尾时跑（nativeUpscale）：
//   in[4] 世界颜色(渲染) + in[2] 深度 + in[3] MV -> 厂商超分 -> srOut(呈现)
//   -> 一份拷进 in[1]（FG 要的 HUD-less 颜色就是超分结果本身）
//   -> 一份反向交付进 in[5]，Java 把它 blit 进 MC 主 target 的颜色纹理。
// 之后 MC 在主 target 上接着画手持物品和 HUD，所以 GUI 天然是呈现分辨率、不经过超分；
// FG 拿到的 final 也是呈现分辨率，只有深度/MV 留在渲染档
// （Intel FG 指南明说 MV「优先低分辨率」、depth 与 MV 同尺寸即可；FSR 的 prepare 直接收 renderSize）。
static bool srActive() { return g.srQuality >= 0 && g.srOut && (g.xess || g.ffxUpscale); }

// 三处呈现分支共用的 barrier 记录：同状态或空资源直接跳过，避免无意义的屏障
static void trans(ID3D12GraphicsCommandList* cmd, D3D12_RESOURCE_BARRIER* b, int& n,
                  ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    (void)cmd;
    if (!r || from == to) return;
    b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[n].Transition.pResource = r;
    b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b[n].Transition.StateBefore = from;
    b[n].Transition.StateAfter = to;
    ++n;
}

static bool createSrOutput() {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = g.w;
    d.Height = g.h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    // 输出必须与输入同格式同色彩空间（XeSS 指南「Output」一节）：我们交的是 RGBA8 的 LDR 画面
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr = g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                  IID_PPV_ARGS(&g.srOut));
    if (FAILED(hr)) {
        logf("[dlssmc-dx] 超分输出纹理创建失败 %ux%u hr=0x%08lx\n", g.w, g.h, (unsigned long)hr);
        g.srOut.Reset();
        return false;
    }
    g.srOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    return true;
}

static bool initXessSr(int quality) {
    if (!g.pXessCreate) return false;
    if (g.pXessCreate(g.dev12.Get(), &g.xess) != XESS_RESULT_SUCCESS) {
        logf("[dlssmc-dx] xessD3D12CreateContext 失败\n");
        g.xess = nullptr;
        return false;
    }
    // 传统档位倍率（平衡=1.7x/质量=1.5x 等）替代 SDK 自选——本机按 GPU 算出的建议值有时太激
    // 进（如平衡档直接 50%）。必须在 BuildPipelines 之前调用，新版 SDK 会失效。
    if (g.pXessForceLegacy) {
        const xess_result_t fr = g.pXessForceLegacy(g.xess, g.xessLegacyScale ? 1u : 0u);
        logf("[dlssmc-dx] xessForceLegacyScaleFactors(%d) 结果=%d\n", (int)g.xessLegacyScale, (int)fr);
    }
    xess_d3d12_init_params_t ip{};
    ip.outputResolution = {g.w, g.h};
    ip.qualitySetting = (xess_quality_settings_t)quality;
    // 必须声明 LDR：否则库按 scRGB HDR 解读我们的 RGBA8，还会去做内部色调映射
    ip.initFlags = XESS_INIT_FLAG_LDR_INPUT_COLOR;
    const ULONGLONG t0 = GetTickCount64();
    const xess_result_t r = g.pXessInit(g.xess, &ip);   // 内部 JIT 编译内核，首次可能要几秒
    logf("[dlssmc-dx] xessD3D12Init 输出=%ux%u 档位=%d LDR 结果=%d 耗时=%llums\n", g.w, g.h, quality,
         (int)r, (unsigned long long)(GetTickCount64() - t0));
    if (r != XESS_RESULT_SUCCESS) {
        if (g.pXessDestroy) g.pXessDestroy(g.xess);
        g.xess = nullptr;
        return false;
    }
    g.velRW = g.velRH = 0;   // 逼 srPass 重设一次 MV 换算
    return true;
}

static bool initFfxUpscale() {
    if (!g.pFfxCreate || !g.hFfxUpscaler) return false;
    // 版本用库自己报的：包头常量是 4.1.1，本机 provider 未必给这个（FG 那边就已经对不上）
    const uint64_t ver = g.fsrUpscaleVersion ? g.fsrUpscaleVersion : FFX_UPSCALER_VERSION;
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = g.dev12.Get();
    ffxCreateContextDescUpscaleVersion vdesc{};
    vdesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    vdesc.header.pNext = (ffxCreateContextDescHeader*)&backend;
    vdesc.version = ver;
    ffxCreateContextDescUpscale up{};
    up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    up.header.pNext = (ffxCreateContextDescHeader*)&vdesc;
    // 我们的颜色是 gamma 编码的 RGBA8，不是线性 HDR
    up.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    // 渲染分辨率永远不超过呈现分辨率，所以上界就填呈现分辨率（超分开关切换不用重建上下文）
    up.maxRenderSize = {g.w, g.h};
    up.maxUpscaleSize = {g.w, g.h};
    const ffxReturnCode_t rc = g.pFfxCreate(&g.ffxUpscale, (ffxCreateContextDescHeader*)&up, nullptr);
    logf("[dlssmc-dx] FSR 超分上下文 版本=%llu 上界=%ux%u 结果=%d\n", (unsigned long long)ver, g.w,
         g.h, (int)rc);
    if (rc != FFX_API_RETURN_OK) {
        g.ffxUpscale = nullptr;
        return false;
    }
    return true;
}

// XeSS 的档位是「一段允许的缩放区间」（1.3 起改成动态区间），Java 按厂商倍率算出的渲染分辨率
// 必须落在里面。把库自己的建议值记下来：实机画面不对时第一个要看的就是这行。
static void logXessSrInputRange() {
    if (g.srSizeLogged || !g.xess || !g.pXessOptimalInput) return;
    g.srSizeLogged = true;
    xess_2d_t out{g.w, g.h}, opt{}, mn{}, mx{};
    if (g.pXessOptimalInput(g.xess, &out, (xess_quality_settings_t)g.srQuality, &opt, &mn, &mx) !=
        XESS_RESULT_SUCCESS)
        return;
    const bool inRange = g.rw >= mn.x && g.rw <= mx.x && g.rh >= mn.y && g.rh <= mx.y;
    logf("[dlssmc-dx] XeSS 超分输入：实交 %ux%u，库建议 %ux%u，允许 %ux%u..%ux%u%s\n", g.rw, g.rh,
         opt.x, opt.y, mn.x, mn.y, mx.x, mx.y, inRange ? "" : " ← 超出允许区间");
}

// 一帧的超分。调用前命令列表已 Reset 且处于打开状态，结束后：
//   in[4] 回到 PIXEL_SHADER_RESOURCE（帧首约定）、in[2]/in[3] 留在 NON_PIXEL_SHADER_RESOURCE
//   （FG 声明的 incomingState 就是它）、srOut 留在 COPY_SOURCE 等着拷进反向交付纹理。
static bool srPass(ID3D12GraphicsCommandList* cmd, bool reset, const float* p) {
    D3D12_RESOURCE_BARRIER bars[4] = {};
    int n = 0;
    // SR 只吃这三张：4=世界颜色 2=深度 3=运动矢量（0/1 是给 FG 的呈现分辨率颜色，SR 不碰）
    for (int i : {2, 3, 4})
        trans(cmd, bars, n, g.in[i].d12.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    trans(cmd, bars, n, g.srOut.Get(), g.srOutState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (n) cmd->ResourceBarrier(n, bars);

    bool ok = false;
    if (g.xess) {
        if (g.pXessVelScale && (g.velRW != g.rw || g.velRH != g.rh)) {
            // 我们的 MV 是 prev-current 的 NDC 差，XeSS 要像素、+Y 向下
            const xess_result_t vr =
                g.pXessVelScale(g.xess, 0.5f * (float)g.rw, -0.5f * (float)g.rh);
            logf("[dlssmc-dx] XeSS velocityScale=(%.1f,%.1f) 结果=%d\n", 0.5f * (float)g.rw,
                 -0.5f * (float)g.rh, (int)vr);
            g.velRW = g.rw;
            g.velRH = g.rh;
        }
        logXessSrInputRange();
        xess_d3d12_execute_params_t ep{};
        ep.pColorTexture = g.in[4].d12.Get();
        ep.pVelocityTexture = g.in[3].d12.Get();
        ep.pDepthTexture = g.in[2].d12.Get();
        ep.pOutputTexture = g.srOut.Get();
        ep.jitterOffsetX = 0.0f;   // MC 这边没有相机抖动
        ep.jitterOffsetY = 0.0f;
        ep.exposureScale = 1.0f;   // LDR 输入按指南建议固定 1.0，且不开自动曝光
        ep.resetHistory = reset ? 1u : 0u;
        ep.inputWidth = g.rw;
        ep.inputHeight = g.rh;
        const xess_result_t r = g.pXessExec(g.xess, cmd, &ep);
        if (r != XESS_RESULT_SUCCESS) logf("[dlssmc-dx] xessD3D12Execute 失败 %d\n", (int)r);
        ok = (r == XESS_RESULT_SUCCESS);
    } else if (g.ffxUpscale) {
        ffxDispatchDescUpscale d{};
        d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        d.commandList = cmd;   // FFX 把命令录进这个列表，由我们统一 Close+Execute
        d.color = ffxApiGetResourceDX12(g.in[4].d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.depth = ffxApiGetResourceDX12(g.in[2].d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors =
            ffxApiGetResourceDX12(g.in[3].d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
        d.output = ffxApiGetResourceDX12(g.srOut.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        d.jitterOffset = {0.0f, 0.0f};
        // MV 是 (prev-current) 的 NDC 差、+Y 向上；FFX 语义 mv_pixel = raw × scale
        // （头文件：像素空间配 (1,1)，UV 空间配 renderSize 正值），像素空间 +Y 向下：
        // x×W/2、y×-H/2。与 XeSS-SR 的 velocityScale 同一套 —— 三家消费同一张 MV 纹理。
        // 原 (-W/2,+H/2) 两轴全反：时序重投影朝反方向取样，历史永远对不上，画面糊。
        d.motionVectorScale = {0.5f * (float)g.rw, -0.5f * (float)g.rh};
        d.renderSize = {g.rw, g.rh};
        d.upscaleSize = {g.w, g.h};
        d.enableSharpening = g.srSharpen;
        d.sharpness = g.srSharpness;
        LARGE_INTEGER now, frequency;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        d.frameTimeDelta = g.ffxLastUpscale.QuadPart
            ? (float)((now.QuadPart - g.ffxLastUpscale.QuadPart) * 1000.0 / frequency.QuadPart) : 0.0f;
        d.preExposure = 1.0f;
        d.reset = reset || !g.ffxLastUpscale.QuadPart;
        g.ffxLastUpscale = now;
        d.cameraNear = p[0];
        d.cameraFar = p[1];
        d.cameraFovAngleVertical = p[2];
        d.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
        const ffxReturnCode_t rc = g.pFfxDispatch(&g.ffxUpscale, (ffxDispatchDescHeader*)&d);
        if (rc != FFX_API_RETURN_OK) logf("[dlssmc-dx] FSR 超分派发失败 %d\n", (int)rc);
        ok = (rc == FFX_API_RETURN_OK);
    }

    n = 0;
    // 世界颜色放回帧首状态；srOut 交给后面的反向交付拷贝当 COPY_SOURCE 用
    trans(cmd, bars, n, g.in[4].d12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    trans(cmd, bars, n, g.srOut.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
          D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (n) cmd->ResourceBarrier(n, bars);
    g.srOutState = D3D12_RESOURCE_STATE_COPY_SOURCE;
    return ok;
}

// 把这一帧要呈现的颜色拷进 backbuffer。in[0] 是 GL 交付的最终画面 —— 超分开着时它里面已经
// 是「SR 放大的世界 + 原生手持物品 + 原生 HUD」，所以这里不用再分超分开关。
// 两家 FG 都以 backbuffer 为颜色源，漏掉这步就是「计数全绿、屏幕全黑」（实机黑屏即此）。
static bool copyColorToBackbuffer(ID3D12GraphicsCommandList* cmd) {
    const UINT idx = g.swapchain->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> bb;
    if (FAILED(g.swapchain->GetBuffer(idx, IID_PPV_ARGS(&bb)))) return false;
    ID3D12Resource* src = g.in[0].d12.Get();
    D3D12_RESOURCE_BARRIER bars[3] = {};
    int n = 0;
    trans(cmd, bars, n, src, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
          D3D12_RESOURCE_STATE_COPY_SOURCE);
    trans(cmd, bars, n, bb.Get(), D3D12_RESOURCE_STATE_PRESENT,
          D3D12_RESOURCE_STATE_COPY_DEST);
    if (n) cmd->ResourceBarrier(n, bars);
    cmd->CopyResource(bb.Get(), src);
    n = 0;
    trans(cmd, bars, n, bb.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
          D3D12_RESOURCE_STATE_PRESENT);
    // 放回帧首状态：下一帧还要按 PIXEL_SHADER_RESOURCE 读它（before 写错，屏障就等于没做）
    trans(cmd, bars, n, src, D3D12_RESOURCE_STATE_COPY_SOURCE,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (n) cmd->ResourceBarrier(n, bars);
    return true;
}

static bool ffxFrame(const float* m, const float* p, bool reset, uint32_t slot) {
    const uint64_t id = ++g.ffxFrameId;
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    const float frameMs = g.ffxLastFrame.QuadPart
        ? (float)((now.QuadPart - g.ffxLastFrame.QuadPart) * 1000.0 / frequency.QuadPart) : 0.0f;
    reset = reset || !g.ffxLastFrame.QuadPart;
    g.ffxLastFrame = now;

    HRESULT hr = g.alloc[slot]->Reset();
    if (FAILED(hr)) {
        logf("[dlssmc-dx] FSR allocator Reset 失败 hr=0x%08lx\n", (unsigned long)hr);
        return false;
    }
    auto* cmd = g.cmd[slot].Get();
    hr = cmd->Reset(g.alloc[slot].Get(), nullptr);
    if (FAILED(hr)) {
        logf("[dlssmc-dx] FSR command list Reset 失败 hr=0x%08lx\n", (unsigned long)hr);
        return false;
    }
    // 用 frameGenerationCallback 时 FG 的颜色取自 backbuffer，所以先把 GL 交付的最终画面拷进去
    bool ok = copyColorToBackbuffer(cmd);
    D3D12_RESOURCE_BARRIER bars[3] = {};
    int n = 0;
    trans(cmd, bars, n, g.in[1].d12.Get(), g.in1State,
          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    for (int i : {2, 3}) {
        // SR OFF 没有 srPass 的 PSR -> NPSR 屏障，不能只给 FFX 填 COMPUTE_READ。
        // 实际 NT 输入带 SIMULTANEOUS_ACCESS，SR 的独立 Execute 后也已衰减为 COMMON；
        // 这里显式转到 NPSR，不把上一个命令列表的状态误当成持久状态。
        const auto before = (g.in[i].d12->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)
            ? D3D12_RESOURCE_STATE_COMMON : (srActive() ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                       : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        trans(cmd, bars, n, g.in[i].d12.Get(), before, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    if (n) cmd->ResourceBarrier(n, bars);

    ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = g.swapchain.Get();
    cfg.frameGenerationCallback = fsrGenerateCb;
    cfg.frameGenerationCallbackUserContext = &g.ffxEffect;
    cfg.frameGenerationEnabled = true;
    // 调优项：异步计算（与其他工作重叠，降开销）/ 只呈现插帧（诊断，对应 DLSS 的同款开关）
    cfg.allowAsyncWorkloads = g.ffxAsyncWorkloads;
    cfg.onlyPresentGenerated = g.ffxOnlyGenerated;
    cfg.HUDLessColor =
        ffxApiGetResourceDX12(g.in[1].d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    cfg.frameID = id;
    // AMD API 要求逐帧 Configure 成功后才能 Prepare，且 Prepare 向打开的列表录命令。
    if (ok && g.pFfxConfigure(&g.ffxEffect, &cfg.header) != FFX_API_RETURN_OK) {
        logf("[dlssmc-dx] FSR configure 失败 id=%llu\n", (unsigned long long)id);
        ok = false;
    }

    ffxDispatchDescFrameGenerationPrepareV2 prep{};
    prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prep.frameID = id;
    prep.commandList = cmd;
    prep.renderSize = {g.rw, g.rh};
    prep.jitterOffset = {0.0f, 0.0f};
    // MV 是 (prev-current) 的 NDC 差、+Y 向上；FFX 语义 mv_pixel = raw × scale
    // （头文件：像素空间配 (1,1)、UV 空间配 renderSize 正值），像素空间 +Y 向下：
    // x×W/2、y×-H/2。与 XeSS-SR/FG、FSR-SR 同一套。原 (-W/2,+H/2) 两轴反向，
    // 插帧朝反方向取样 —— FSR 动态画面拖影糊片的根源。
    prep.motionVectorScale = {0.5f * (float)g.rw, -0.5f * (float)g.rh};
    prep.frameTimeDelta = frameMs;
    prep.reset = reset;
    prep.cameraNear = p[0];
    prep.cameraFar = p[1];
    prep.cameraFovAngleVertical = p[2];
    prep.depth = ffxApiGetResourceDX12(g.in[2].d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    prep.motionVectors =
        ffxApiGetResourceDX12(g.in[3].d12.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
    // 相机基向量取自 view 矩阵（列主序 m[16..31]）：right/up/forward 是旋转部分的行，
    // 位置 = -R^T * t
    const float* v = m + 16;
    for (int i = 0; i < 3; ++i) {
        prep.cameraRight[i] = v[i];
        prep.cameraUp[i] = v[4 + i];
        prep.cameraForward[i] = v[8 + i];
        prep.cameraPosition[i] = -(v[12] * prep.cameraRight[i] + v[13] * prep.cameraUp[i] +
                                   v[14] * prep.cameraForward[i]);
    }
    if (ok && g.pFfxDispatch(&g.ffxEffect, &prep.header) != FFX_API_RETURN_OK) {
        logf("[dlssmc-dx] FSR prepare 派发失败 id=%llu\n", (unsigned long long)id);
        ok = false;
    }
    // 失败也要关闭以便下次 Reset，但不得执行半帧命令或 Present。
    hr = cmd->Close();
    if (FAILED(hr)) {
        logf("[dlssmc-dx] FSR command list Close 失败 hr=0x%08lx\n", (unsigned long)hr);
        return false;
    }
    if (!ok) return false;
    ID3D12CommandList* lists[] = {cmd};
    g.queue->ExecuteCommandLists(1, lists);
    g.in1State = (g.in[1].d12->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)
        ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    return g.swapchain->Present(1, 0) == S_OK;
}

// ------------------------------------------------------------------ JNI
extern "C" {

// vendorRoot 下按 native/vendor 的层次放：intel/bin 与 amd/bin
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeInit(JNIEnv* env, jclass, jstring vendorRoot,
                                                             jstring logDir) {
    const char* v = vendorRoot ? env->GetStringUTFChars(vendorRoot, nullptr) : nullptr;
    const char* l = logDir ? env->GetStringUTFChars(logDir, nullptr) : nullptr;
    std::wstring root = utf8ToWide(v), wl = utf8ToWide(l);
    if (v) env->ReleaseStringUTFChars(vendorRoot, v);
    if (l) env->ReleaseStringUTFChars(logDir, l);
    // 首开用 "w" 清掉上一次的；nativeShutdown 会 fclose，同进程再 init 必须追加，
    // 否则换后端时新初始化会把上一段后端的证据整段冲掉（实机诊断被这个坑过一次）
    if (!g_log && !wl.empty()) {
        static bool opened = false;
        g_log = _wfopen((wl + L"\\dlssmc_dx.log").c_str(), opened ? L"a" : L"w");
        opened = true;
    }
    logf("[dlssmc] ---- DX nativeInit vendor=%ls log=%ls ----\n", root.c_str(), wl.c_str());

    auto get = reinterpret_cast<PFN_wglGetProcAddress>(
        GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglGetProcAddress"));
    pOpenDevice = (PFN_wglDXOpenDeviceNV)get("wglDXOpenDeviceNV");
    pRegisterObject = (PFN_wglDXRegisterObjectNV)get("wglDXRegisterObjectNV");
    pUnregisterObject = (PFN_wglDXUnregisterObjectNV)get("wglDXUnregisterObjectNV");
    pLockObjects = (PFN_wglDXLockObjectsNV)get("wglDXLockObjectsNV");
    pUnlockObjects = (PFN_wglDXUnlockObjectsNV)get("wglDXUnlockObjectsNV");
    pCloseDevice = (PFN_wglDXCloseDeviceNV)get("wglDXCloseDeviceNV");
    if (!pOpenDevice || !pRegisterObject || !pLockObjects || !pUnlockObjects) {
        logf("[dlssmc] WGL_NV_DX_interop2 不可用\n");
        return -1;
    }
    std::wstring intel = root + L"\\intel\\bin", amd = root + L"\\amd\\bin";
    g.fsrDir = amd;
    if (!loadXeSS(intel)) return -2;
    // FSR 加载失败不致命：能力查询会报「不可用」，后端选择时再拦
    if (!loadFfx(amd)) logf("[dlssmc-dx] FSR 未加载（缺文件或导出），该后端不可用\n");
    g.xessBuiltinDll = dllVersionOf(intel + L"\\libxess_fg.dll");
    g.fsrBuiltinDll = dllVersionOf(amd + L"\\amd_fidelityfx_framegeneration_dx12.dll");
    xell_version_t xv{};
    if (g.pXellVersion && g.pXellVersion(&xv) == XELL_RESULT_SUCCESS) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u.%u.%u", xv.major, xv.minor, xv.patch);
        g.xellVersion = buf;
    }
    return 0;
}

// [XeLL 版本, 本机最大插帧数, FSR provider 版本, FSR 内置 DLL 版本, XeSS 内置 DLL 版本,
//  FSR 是否可用, XeSS 超分, FSR 超分]
// 第 2 项要建完设备才知道（GetProperties 需要 D3D12 设备与 xefg 上下文），未建好时为 0。
// 第 8 项的 provider 版本同样要建设备后才查得到，之前只显示「可用」。
JNIEXPORT jobjectArray JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeGetCapabilities(JNIEnv* env, jclass) {
    char interp[16];
    snprintf(interp, sizeof(interp), "%u", g.maxInterpolations);
    std::string xessSr = !g.hXess   ? "不可用（缺 libxess.dll）"
                         : g.xessSrVersion.empty() ? "可用" : "可用 " + g.xessSrVersion;
    std::string fsrSr = !g.hFfxUpscaler ? "不可用（缺 upscaler provider）"
                        : g.fsrUpscaleProvider.empty() ? "可用" : g.fsrUpscaleProvider;
    const char* vals[8] = {g.xellVersion.empty() ? "未初始化" : g.xellVersion.c_str(), interp,
                           g.fsrProvider.empty() ? "未查询" : g.fsrProvider.c_str(),
                           g.fsrBuiltinDll.empty() ? "未加载" : g.fsrBuiltinDll.c_str(),
                           g.xessBuiltinDll.empty() ? "未加载" : g.xessBuiltinDll.c_str(),
                           (g.pFfxCreate && g.pFfxDispatch) ? "是" : "否", xessSr.c_str(),
                           fsrSr.c_str()};
    jclass str = env->FindClass("java/lang/String");
    jobjectArray out = env->NewObjectArray(8, str, nullptr);
    for (int i = 0; i < 8; ++i) {
        jstring s = env->NewStringUTF(vals[i]);
        env->SetObjectArrayElement(out, i, s);
        env->DeleteLocalRef(s);
    }
    return out;
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeCreateDevice(JNIEnv*, jclass, jlong parent,
                                                                     jint width, jint height,
                                                                     jint backend) {
    if (g.dev12) return 0;
    g.parent = reinterpret_cast<HWND>((intptr_t)parent);
    g.w = (uint32_t)width;
    g.h = (uint32_t)height;
    g.rw = g.w;   // 超分没配之前，渲染分辨率就是呈现分辨率
    g.rh = g.h;
    g.backend = (Backend)backend;

    if (!ensureChildWindow(g.parent, g.w, g.h)) return -1;

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.dev12)))) return -2;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(g.dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&g.queue)))) return -2;
    for (int i = 0; i < kFramesInFlight; ++i) {
        if (FAILED(g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&g.alloc[i])))) return -2;
        if (FAILED(g.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[i].Get(),
                                              nullptr, IID_PPV_ARGS(&g.cmd[i])))) return -2;
        g.cmd[i]->Close();
    }
    if (FAILED(g.dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence12)))) return -2;
    g.evt12 = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                 D3D11_SDK_VERSION, &g.d3d11, nullptr, &g.ctx11))) return -3;
    if (FAILED(g.d3d11.As(&g.dev11_5)) || FAILED(g.ctx11.As(&g.ctx11_4))) return -3;
    if (FAILED(g.dev11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.fence11))))
        return -3;
    g.evt11 = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    g.interop = pOpenDevice(g.d3d11.Get());
    if (!g.interop) return -3;

    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&g.factory)))) return -4;
    queryFfxUpscaleVersion();   // 超分 provider 版本要问库，且需要 D3D12 设备

    if (g.backend == kBackendXeSS) {
        if (!initXeSSSwapchain()) return -6;
    } else if (g.backend == kBackendFSR) {
        if (!initFfx()) return -7;
    } else {
        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = g.w;
        scd.Height = g.h;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferCount = 3;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> plain;
        if (FAILED(g.factory->CreateSwapChainForHwnd(g.queue.Get(), g.child, &scd, nullptr, nullptr,
                                                     &plain))) {
            return -4;
        }
        if (FAILED(plain.As(&g.swapchain))) return -4;
    }
    logf("[dlssmc] DX 设备就绪 backend=%d %ux%u xell=%s\n", (int)g.backend, g.w, g.h,
         g.xellVersion.c_str());
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeBindTextures(JNIEnv*, jclass, jint finalGl,
                                                                     jint hudlessGl, jint depthGl,
                                                                     jint motionGl, jint worldGl,
                                                                     jint srOutGl, jint renderWidth,
                                                                     jint renderHeight) {
    if (!g.dev12) return -1;
    // 超分的输入和输出必须成对：只有世界颜色没有反向输出，SR 的结果就落不回 GL，画面会停在
    // 「计数全绿、内容不更新」这个最难查的形态上，所以这里直接拒。
    if ((worldGl == 0) != (srOutGl == 0)) {
        logf("[dlssmc] 超分输入/输出必须成对绑定 world=%d srOut=%d\n", (int)worldGl, (int)srOutGl);
        return -8;
    }
    // 渲染分辨率由交进来的世界颜色/深度/MV 纹理决定；呈现分辨率只由 nativeCreateDevice 决定。
    // 超分关着时两者本该相等，所以不相等就说明窗口/主 target 改了分辨率。
    g.rw = (uint32_t)renderWidth;
    g.rh = (uint32_t)renderHeight;
    if (g.srQuality < 0 && (g.rw != g.w || g.rh != g.h)) {
        g.w = g.rw;
        g.h = g.rh;
        ensureChildWindow(g.parent, g.w, g.h);
    }
    g.velRW = g.velRH = 0;   // MV 换算跟着渲染分辨率走，逼 srPass 重设一次
    const unsigned gls[6] = {(unsigned)finalGl, (unsigned)hudlessGl, (unsigned)depthGl,
                             (unsigned)motionGl, (unsigned)worldGl, (unsigned)srOutGl};
    const DXGI_FORMAT fmts[6] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
                                 DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT,
                                 DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
    // 交给 FG 的两张颜色和 SR 的反向输出跟呈现分辨率，深度/MV/世界颜色跟渲染分辨率
    const uint32_t ws[6] = {g.w, g.w, g.rw, g.rw, g.rw, g.w};
    const uint32_t hs[6] = {g.h, g.h, g.rh, g.rh, g.rh, g.h};
    // OpenSharedHandle 的初始状态是 COMMON，不是后端希望读取时的状态。
    g.in1State = D3D12_RESOURCE_STATE_COMMON;
    for (int i = 0; i < 6; ++i) {
        releaseInput(g.in[i]);
        if (!gls[i]) continue;
        if (!makeInputChain(g.in[i], gls[i], fmts[i], ws[i], hs[i])) {
            logf("[dlssmc] 输入 %d 建立跨 API 链失败 (gl=%u %ux%u)\n", i, gls[i], ws[i], hs[i]);
            return -2 - i;
        }
    }
    logf("[dlssmc] 绑定输入 渲染=%ux%u 呈现=%ux%u 超分档位=%d 反向输出=%s\n", g.rw, g.rh, g.w, g.h,
         g.srQuality, srOutGl ? "有" : "无");
    g.haveInputs = true;
    g.ready = true;
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeUnbindTextures(JNIEnv*, jclass) {
    if (g.locked || g.outLocked) return -1;
    wait12();
    for (auto& t : g.in) releaseInput(t);
    g.haveInputs = false;
    return 0;
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeLock(JNIEnv*, jclass, jint from, jint count) {
    if (!g.ready || !g.haveInputs || g.locked || g.outLocked) return -1;
    if (from < 0 || from >= 5 || count < 1 || count > 5 - from) return -1;
    LARGE_INTEGER lockMark;
    QueryPerformanceCounter(&lockMark);
    wait12();   // 上一帧的 D3D12 读取必须结束，GL 才能重写
    g.accLockWait += qpcSinceMs(lockMark);
    HANDLE objs[6];
    for (int i = 0; i < count; ++i) {
        objs[i] = g.in[from + i].reg;
        if (!objs[i]) return -1;
    }
    if (!pLockObjects(g.interop, count, objs)) return -2;
    g.locked = true;
    g.lockFrom = from;
    g.lockCount = count;
    return 0;
}

// 锁内 GL 已经画完（Java 侧 glFinish 过）：把这一段拷进跨 API 纹理并等 D3D11 落地，然后解锁
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeDeliver(JNIEnv*, jclass) {
    if (!g.locked) return -1;
    LARGE_INTEGER deliverMark;
    QueryPerformanceCounter(&deliverMark);
    const int from = g.lockFrom, count = g.lockCount;
    for (int i = 0; i < count; ++i)
        g.ctx11->CopyResource(g.in[from + i].nt.Get(), g.in[from + i].kmt.Get());
    wait11();
    g.accDeliver += qpcSinceMs(deliverMark);
    HANDLE objs[6];
    for (int i = 0; i < count; ++i) objs[i] = g.in[from + i].reg;
    g.locked = false;
    if (!pUnlockObjects(g.interop, count, objs)) return -2;
    return 0;
}

// 世界阶段收尾：渲染分辨率的世界颜色/深度/MV -> 厂商超分 -> 反向写回 in[5] 那张 GL 纹理。
// 反向交付就是正向的镜像：锁 -> D3D12 写 d12 -> 栅栏 -> D3D11 CopyResource(kmt<-nt) -> Flush，
// 然后**把 in[5] 留在锁内**返回 —— wglDX 互操作的硬约束是「注册过的对象只有锁着才能被 GL 用」
// （锁外连 FBO 都挂不上，DXToggleCheck 里正反两面都验了）。GL 把它贴回主 target 后必须调
// nativeUpscaleDone 解锁，手持物品与 HUD 于是天然是呈现分辨率、不经过超分。
// 调用前提：GL 已经画完并 glFinish，且不在 nativeLock/nativeDeliver 的锁内。
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeUpscale(JNIEnv* env, jclass,
                                                                jfloatArray params,
                                                                jboolean reset) {
    if (!g.ready || !g.haveInputs) return -1;
    if (g.locked) return -7;
    if (!srActive() || !g.in[4].reg || !g.in[5].reg) return -6;
    float p[8] = {0, 0, 0, 0, 0, 0, 1, 1};
    if (params && env->GetArrayLength(params) >= 8) env->GetFloatArrayRegion(params, 0, 8, p);

    if (g.outLocked) return -7;

    // 输入已经由 nativeDeliver 交付，不能在这里再搬一次。
    // 反向那一路要在 D3D12 动手之前就锁住，否则 GL 可能还在读上一帧的内容
    if (!pLockObjects(g.interop, 1, &g.in[5].reg)) return -3;
    const uint32_t slot = g.slot;
    g.slot = (g.slot + 1) % kFramesInFlight;
    g.alloc[slot]->Reset();
    g.cmd[slot]->Reset(g.alloc[slot].Get(), nullptr);
    bool ok = srPass(g.cmd[slot].Get(), reset != JNI_FALSE, p);
    if (ok) {
        ID3D12GraphicsCommandList* cmd = g.cmd[slot].Get();
        // hudless 就是超分结果本身：世界放大完、手持物品与 HUD 还没画。在 D3D12 里多拷一份，
        // 省掉「SR 输出 -> GL -> 再交付回 D3D12」这个来回（一次呈现分辨率 blit + 一次阻塞等待）。
        ID3D12Resource* hudless = g.in[1].d12.Get();
        D3D12_RESOURCE_BARRIER bars[3] = {};
        int n = 0;
        // srPass 结束时 srOut 就在 COPY_SOURCE
        trans(cmd, bars, n, g.in[5].d12.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
              D3D12_RESOURCE_STATE_COPY_DEST);
        trans(cmd, bars, n, hudless, g.in1State, D3D12_RESOURCE_STATE_COPY_DEST);
        if (n) cmd->ResourceBarrier(n, bars);
        cmd->CopyResource(g.in[5].d12.Get(), g.srOut.Get());
        if (hudless) cmd->CopyResource(hudless, g.srOut.Get());
        n = 0;
        trans(cmd, bars, n, g.in[5].d12.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        trans(cmd, bars, n, hudless, D3D12_RESOURCE_STATE_COPY_DEST, g.in1State);
        trans(cmd, bars, n, g.srOut.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (n) cmd->ResourceBarrier(n, bars);
        g.srOutState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    }
    g.cmd[slot]->Close();
    ID3D12CommandList* lists[] = {g.cmd[slot].Get()};
    g.queue->ExecuteCommandLists(1, lists);
    LARGE_INTEGER upMark;
    QueryPerformanceCounter(&upMark);
    wait12();
    if (ok) {
        g.ctx11->CopyResource(g.in[5].kmt.Get(), g.in[5].nt.Get());
        wait11();
    }
    g.accUpscale += qpcSinceMs(upMark);
    if (!ok) {
        // 失败就别把锁留着：GL 还要继续画这一帧
        pUnlockObjects(g.interop, 1, &g.in[5].reg);
        return -4;
    }
    g.outLocked = true;
    return 0;
}

// GL 已经把超分结果贴回主 target：解锁，把这张纹理还给 D3D 侧
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeUpscaleDone(JNIEnv*, jclass) {
    if (!g.outLocked) return -1;
    g.outLocked = false;
    return pUnlockObjects(g.interop, 1, &g.in[5].reg) ? 0 : -2;
}

// 预检：D3D12 侧能不能看到 GL 画的非黑画面
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeProbe(JNIEnv*, jclass) {
    if (!g.ready || !g.haveInputs) return 0;
    // 读的是 in[0]，也就是交给 FG 的最终画面，尺寸恒等于呈现分辨率
    const uint32_t pw = g.in[0].w, ph = g.in[0].h;
    const UINT64 pitch = ((UINT64)pw * 4 + 255) & ~255ULL;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = pitch * ph;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    if (FAILED(g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&readback)))) return 0;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    g.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                               IID_PPV_ARGS(&cmd));
    alloc->Reset();
    cmd->Reset(alloc.Get(), nullptr);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = g.in[0].d12.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = pw;
    dst.PlacedFootprint.Footprint.Height = ph;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = pitch;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    cmd->Close();
    ID3D12CommandList* lists[] = {cmd.Get()};
    g.queue->ExecuteCommandLists(1, lists);
    wait12();
    unsigned char* mapped = nullptr;
    D3D12_RANGE range{0, (SIZE_T)(pitch * ph)};
    if (FAILED(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)))) return 0;
    int nonBlack = 0;
    for (uint32_t y = 0; y < ph; y += 7) {
        const unsigned char* row = mapped + (size_t)y * pitch;
        for (uint32_t x = 0; x < pw; x += 7) {
            if (row[x * 4] || row[x * 4 + 1] || row[x * 4 + 2]) {
                ++nonBlack;
                if (nonBlack > 8) break;
            }
        }
        if (nonBlack > 8) break;
    }
    readback->Unmap(0, nullptr);
    return nonBlack > 8 ? 1 : 0;
}

// matrices: 列主序 4x4 x4 = [proj, view, prevProj, prevView]；params: [near, far, fov, aspect, jx, jy, mvx, mvy]
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativePresent(JNIEnv* env, jclass,
                                                               jfloatArray matrices,
                                                               jfloatArray params, jboolean reset) {
    if (!g.ready || !g.swapchain) return -1;
    LARGE_INTEGER presentMark;
    QueryPerformanceCounter(&presentMark);
    auto presentDone = [&]() {
        g.accPresent += qpcSinceMs(presentMark);
        logBridgeTimingsMaybe();
    };
    float m[64] = {0};
    if (matrices && env->GetArrayLength(matrices) >= 64) env->GetFloatArrayRegion(matrices, 0, 64, m);
    float p[8] = {0, 0, 0, 0, 0, 0, 1, 1};
    if (params && env->GetArrayLength(params) >= 8) env->GetFloatArrayRegion(params, 0, 8, p);

    const uint32_t slot = g.slot;
    g.slot = (g.slot + 1) % kFramesInFlight;
    const uint32_t id = ++g.presentId;

    if (g.backend == kBackendFSR) {
        const uint32_t before = g.ffxGenerated;
        if (!ffxFrame(m, p, reset != JNI_FALSE, slot)) {
            presentDone();
            return -5;
        }
        // 兼容现有接口：1 个真实帧 + 本次成功派发的生成数，并非实际 display telemetry。
        g.lastFramesPresented = 1 + (g.ffxGenerated - before);
        presentDone();
        return 0;
    }

    if (g.backend == kBackendXeSS) {
        g.pXellMarker(g.xell, id, XELL_INPUT_SAMPLE);
        g.pXellSleep(g.xell, id);
        g.pXellMarker(g.xell, id, XELL_SIMULATION_START);
        g.pXellMarker(g.xell, id, XELL_SIMULATION_END);
        g.pXellMarker(g.xell, id, XELL_RENDERSUBMIT_START);

        g.alloc[slot]->Reset();
        g.cmd[slot]->Reset(g.alloc[slot].Get(), nullptr);
        // XeSS-FG 的颜色源就是代理 swapchain 的 backbuffer（Intel 样例里应用直接往里面渲染）。
        // 不先把这一帧的颜色拷进去，呈现出去的就是没写过的黑 backbuffer：计数与预检全绿、屏幕
        // 全黑，实机黑屏即此。
        if (!copyColorToBackbuffer(g.cmd[slot].Get())) {
            presentDone();
            return -3;
        }
        // 我们交的是「最终画面」，XeSS 的 HUDLessColor 槽位就放它（没有独立 UI alpha）。
        // Intel FG 开发指南：HUD-less 颜色必须跟 backbuffer 同一档 —— 交渲染尺寸的会让库静默跳过
        // 插帧、原样呈现上一帧（实测 framesPresented 恒为 1）；MV/depth 才允许留在渲染档
        // （"Preferred to be low-resolution"，depth 只要和 MV 同尺寸）。
        xefg_swapchain_d3d12_resource_data_t res[3] = {};
        res[0].type = XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
        res[0].validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
        res[0].resourceBase = {0, 0};
        res[0].resourceSize = {g.in[1].w, g.in[1].h};
        res[0].pResource = g.in[1].d12.Get();
        res[0].incomingState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        g.in1State = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        res[1].type = XEFG_SWAPCHAIN_RES_DEPTH;
        res[1].validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
        res[1].resourceBase = {0, 0};
        res[1].resourceSize = {g.in[2].w, g.in[2].h};
        res[1].pResource = g.in[2].d12.Get();
        res[1].incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        res[2].type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
        res[2].validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
        res[2].resourceBase = {0, 0};
        res[2].resourceSize = {g.in[3].w, g.in[3].h};
        res[2].pResource = g.in[3].d12.Get();
        res[2].incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        for (auto& r : res) g.pXefgTagRes(g.xefg, g.cmd[slot].Get(), id, &r);

        xefg_swapchain_frame_constant_data_t cst{};
        // Java 交来的是 joml 列主序；XeSS 要行主序，所以 dst[r*4+c] = src[c*4+r]
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) {
                cst.viewMatrix[r * 4 + c] = m[16 + c * 4 + r];
                cst.projectionMatrix[r * 4 + c] = m[c * 4 + r];
            }
        // 我们的 MV 是 prev-current 的 NDC 差，MV 纹理跟场景一样是渲染分辨率的。
        // XeSS 要像素、+Y 向下：x×W/2、y×-H/2 —— 与 SR 上下文的 velocityScale
        // （xessSetVelocityScale 处）同一套换算，两者消费的是同一张 MV 纹理。
        // 不能把 Java 传来的 mvecScale(0.5,-0.5) 也乘进来：那会得到 W/4、+H/4，
        // 幅度减半且 Y 反向，插帧运动补偿整体错位（动态画面糊成一片）。
        cst.motionVectorScaleX = 0.5f * (float)g.rw;
        cst.motionVectorScaleY = -0.5f * (float)g.rh;
        cst.resetHistory = reset ? 1u : 0u;
        // frameRenderTime 喂实测帧间隔（FSR 路径的 frameTimeDelta 同款做法）：
        // 写死 16ms 会让插帧节奏与真实帧率脱节，表现为节奏性卡顿。
        {
            LARGE_INTEGER now, frequency;
            QueryPerformanceCounter(&now);
            QueryPerformanceFrequency(&frequency);
            float ms = g.xessLastFrame.QuadPart
                ? (float)((now.QuadPart - g.xessLastFrame.QuadPart) * 1000.0
                          / frequency.QuadPart)
                : 16.0f;
            // 世界加载、拖窗口这类长停顿与异常短毛刺都不进滤波
            cst.frameRenderTime = (ms >= 2.0f && ms <= 200.0f) ? ms : 16.0f;
            g.xessLastFrame = now;
        }
        g.pXefgTagConst(g.xefg, id, &cst);
        g.cmd[slot]->Close();
        ID3D12CommandList* lists[] = {g.cmd[slot].Get()};
        g.queue->ExecuteCommandLists(1, lists);   // 打标签的命令列表必须在 Present 前提交

        g.pXellMarker(g.xell, id, XELL_RENDERSUBMIT_END);
        g.pXellMarker(g.xell, id, XELL_PRESENT_START);
        g.pXefgPresentId(g.xefg, id);
        HRESULT hr = g.swapchain->Present(1, 0);
        g.pXellMarker(g.xell, id, XELL_PRESENT_END);

        xefg_swapchain_present_status_t st{};
        if (g.pXefgStatus(g.xefg, &st) == XEFG_SWAPCHAIN_RESULT_SUCCESS)
            g.lastFramesPresented = st.framesPresented;
        presentDone();
        return hr == S_OK ? 0 : (hr == DXGI_ERROR_INVALID_CALL ? -3 : -4);
    }

    // passthrough：把这一帧的颜色拷进 backbuffer 再呈现，用来单验桥本身（不开厂商插帧）
    {
        g.alloc[slot]->Reset();
        g.cmd[slot]->Reset(g.alloc[slot].Get(), nullptr);
        if (!copyColorToBackbuffer(g.cmd[slot].Get())) {
            presentDone();
            return -3;
        }
        g.cmd[slot]->Close();
        ID3D12CommandList* lists[] = {g.cmd[slot].Get()};
        g.queue->ExecuteCommandLists(1, lists);
        g.lastFramesPresented = 1;
        presentDone();
        return g.swapchain->Present(1, 0) == S_OK ? 0 : -4;
    }
}

JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeGetPresentedCount(JNIEnv*, jclass) {
    return (jint)g.lastFramesPresented;
}

// 请求每张真实帧后面生成几张插帧。XeSS 会夹到本机 maxSupportedInterpolations；
// FSR 的生成张数由库自己决定，这里明确回 -1（不支持），不假装接受。
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeSetInterpolatedFrames(JNIEnv*, jclass,
                                                                              jint extra) {
    if (g.backend != kBackendXeSS || !g.xefg) return -1;
    uint32_t want = extra < 1 ? 1u : (uint32_t)extra;
    uint32_t eff = g.maxInterpolations ? (want < g.maxInterpolations ? want : g.maxInterpolations)
                                       : 1u;
    auto setNum = (decltype(&xefgSwapChainSetNumInterpolatedFrames))
        (g.pXefgStatus ? GetProcAddress(g.hXefg, "xefgSwapChainSetNumInterpolatedFrames") : nullptr);
    if (!setNum || setNum(g.xefg, eff) != XEFG_SWAPCHAIN_RESULT_SUCCESS) return -1;
    logf("[dlssmc-dx] 插帧数请求=%u 生效=%u（本机上限=%u）\n", want, eff, g.maxInterpolations);
    return (jint)eff;
}

// FSR 调优：生成工作跑异步计算队列（allowAsyncWorkloads）与「只呈现生成帧」诊断开关。
// 逐帧 configure 会照 g 里的这两个值下发，这里只改值。
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeSetFsrTuning(JNIEnv*, jclass,
                                                                     jboolean asyncWorkloads,
                                                                     jboolean onlyPresentGenerated) {
    const bool a = asyncWorkloads == JNI_TRUE, o = onlyPresentGenerated == JNI_TRUE;
    if (a == g.ffxAsyncWorkloads && o == g.ffxOnlyGenerated) return 0;
    g.ffxAsyncWorkloads = a;
    g.ffxOnlyGenerated = o;
    logf("[dlssmc-dx] FSR 调优：异步计算=%d 只显示插帧=%d\n", (int)a, (int)o);
    return 0;
}

// XeSS 场景变化检测阈值：0=最不敏感（更敢插帧），1=最敏感（更容易停插防糊）。SDK 默认 0.7
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeSetSceneChangeThreshold(JNIEnv*, jclass,
                                                                                jfloat threshold) {
    if (!g.xefg || !g.pXefgSetThreshold) return -1;
    if (threshold < 0.0f) threshold = 0.0f;
    if (threshold > 1.0f) threshold = 1.0f;
    const xefg_swapchain_result_t r = g.pXefgSetThreshold(g.xefg, threshold);
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS) {
        logf("[dlssmc-dx] 场景变化阈值 %.2f 设置失败 %d\n", threshold, (int)r);
        return -2;
    }
    logf("[dlssmc-dx] 场景变化阈值=%.2f\n", threshold);
    return 0;
}

// XeSS-SR 传统档位倍率开关：true=平衡=1.7x/质量=1.5x 等；false=SDK 按 GPU 自选（本机有时更激进）。
// 只在下一次 nativeConfigureSR 时生效，所以这里只更新标志 + 立刻作废 SR 上下文。
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeSetXessLegacyScale(JNIEnv*, jclass,
                                                                          jboolean enable) {
    const bool want = enable == JNI_TRUE;
    if (want == g.xessLegacyScale) return 0;
    g.xessLegacyScale = want;
    logf("[dlssmc-dx] XeSS 传统档位倍率=%d，下一次 SR 上下文生效\n", (int)want);
    return 0;
}

// XeLL 原生限帧（minimumIntervalUs 微秒，0=不限）。与 Reflex 限帧独立：
// Reflex 仅控 DLSS-G；XeSS / FSR 这条路走 XeLL，要走自己的限帧接口。
// bLowLatencyMode 保持 1（XeSS-FG 必须在低延迟模式下工作），不能关。
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeSetXellFpsLimit(JNIEnv*, jclass,
                                                                        jint fpsLimit) {
    if (!g.xell || !g.pXellSleepMode) return -1;
    if (fpsLimit < 0) fpsLimit = 0;
    const uint32_t intervalUs = fpsLimit == 0 ? 0u : (uint32_t)(1'000'000 / fpsLimit);
    xell_sleep_params_t sp{};
    sp.minimumIntervalUs = intervalUs;
    sp.bLowLatencyMode = 1;   // 不能关，关了 FG 出 1 帧
    sp.bLowLatencyBoost = 0;  // SDK 暂不支持
    const xell_result_t r = g.pXellSleepMode(g.xell, &sp);
    if (r != XELL_RESULT_SUCCESS) {
        logf("[dlssmc-dx] XeLL 限帧 %u us (%d FPS) 失败 %d\n", intervalUs, fpsLimit, (int)r);
        return -2;
    }
    logf("[dlssmc-dx] XeLL 限帧=%dFPS (%uus)\n", fpsLimit, intervalUs);
    return 0;
}

// 配置超分。quality<0 关闭；否则是各家自己的档位枚举（XeSS 100..106 / FSR 0..4），由 Java 侧按
// 后端翻译。渲染分辨率不在这里给：它由 nativeBindTextures 交进来的纹理尺寸决定，呈现分辨率由
// nativeCreateDevice 决定 —— 两边都定了，超分才有输入和输出。
// 锐化只有 FSR 用得上：XeSS-SR 的导出表里根本没有锐化接口（dumpbin 实测）。
JNIEXPORT jint JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeConfigureSR(JNIEnv*, jclass, jint quality,
                                                                    jboolean sharpen,
                                                                    jfloat sharpness) {
    if (!g.dev12 || g.locked || g.outLocked) return -1;
    wait12();
    if (quality < 0) {
        g.srQuality = -1;
        g.ffxLastUpscale = {};
        logf("[dlssmc-dx] 超分关闭（上下文与输出纹理保留，切回来不用重建）\n");
        return 0;
    }
    if (g.backend == kBackendPassthrough) {
        logf("[dlssmc-dx] passthrough 后端没有超分\n");
        return -2;
    }
    if (g.backend == kBackendXeSS) {
        if (!g.hXess) return -2;
        if (quality < XESS_QUALITY_SETTING_ULTRA_PERFORMANCE || quality > XESS_QUALITY_SETTING_AA)
            return -4;
    } else if (quality > FFX_UPSCALE_QUALITY_MODE_ULTRA_PERFORMANCE) {
        return -4;
    }
    g.srSharpen = sharpen != JNI_FALSE;
    g.srSharpness = sharpness;
    if (g.backend == kBackendXeSS && g.srSharpen)
        logf("[dlssmc-dx] XeSS-SR 没有锐化接口，锐化=%.2f 被忽略\n", g.srSharpness);
    if (!g.srOut && !createSrOutput()) return -5;
    if (g.backend == kBackendXeSS) {
        // 档位是 xessD3D12Init 时定死的，换档必须重建上下文
        if (g.xess && g.srQuality != quality) {
            if (g.pXessDestroy) g.pXessDestroy(g.xess);
            g.xess = nullptr;
        }
        if (!g.xess && !initXessSr(quality)) return -3;
    } else if (!g.ffxUpscale && !initFfxUpscale()) {
        return -3;
    }
    g.srQuality = quality;
    g.srSizeLogged = false;
    logf("[dlssmc-dx] 超分就绪 后端=%d 档位=%d 锐化=%d(%.2f) 呈现=%ux%u\n", (int)g.backend, quality,
         (int)g.srSharpen, g.srSharpness, g.w, g.h);
    return 0;
}

// 这个档位该按多大分辨率渲染 —— 问厂商，不自己列倍率表（倍率表是近似值，档位区间还会随版本变）。
// XeSS 回的是该档位的建议输入分辨率，FSR 回的是该档位对应的渲染分辨率。
// 调用时机：nativeConfigureSR 之后（XeSS 的查询要上下文）、nativeBindTextures 之前。
// 查不到回 [0, 0]，Java 侧必须拒绝无效尺寸。
JNIEXPORT jintArray JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeGetRenderSize(JNIEnv* env, jclass,
                                                                      jint quality) {
    jint v[2] = {0, 0};
    if (g.dev12 && quality >= 0) {
        if (g.backend == kBackendXeSS) {
            if (g.xess && g.pXessOptimalInput) {
                const xess_2d_t dst{g.w, g.h};
                xess_2d_t opt{}, mn{}, mx{};
                if (g.pXessOptimalInput(g.xess, &dst, (xess_quality_settings_t)quality, &opt, &mn,
                                        &mx) == XESS_RESULT_SUCCESS) {
                    v[0] = (jint)opt.x;
                    v[1] = (jint)opt.y;
                }
            }
        } else if (g.pFfxQuery) {
            ffxQueryDescUpscaleGetRenderResolutionFromQualityMode q{};
            q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETRENDERRESOLUTIONFROMQUALITYMODE;
            q.displayWidth = g.w;
            q.displayHeight = g.h;
            q.qualityMode = (uint32_t)quality;
            uint32_t rw = 0, rh = 0;
            q.pOutRenderWidth = &rw;
            q.pOutRenderHeight = &rh;
            if (g.pFfxQuery(nullptr, (ffxQueryDescHeader*)&q) == FFX_API_RETURN_OK) {
                v[0] = (jint)rw;
                v[1] = (jint)rh;
            }
        }
    }
    logf("[dlssmc-dx] 档位=%d 呈现=%ux%u 厂商建议渲染=%dx%d\n", (int)quality, g.w, g.h, v[0], v[1]);
    jintArray out = env->NewIntArray(2);
    env->SetIntArrayRegion(out, 0, 2, v);
    return out;
}

JNIEXPORT void JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeShowPresentation(JNIEnv*, jclass) {
    if (g.child && !g.childVisible) {
        ShowWindow(g.child, SW_SHOWNOACTIVATE);
        g.childVisible = true;
    }
}

JNIEXPORT void JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeHidePresentation(JNIEnv*, jclass) {
    if (g.child && g.childVisible) {
        ShowWindow(g.child, SW_HIDE);
        g.childVisible = false;
    }
}

JNIEXPORT void JNICALL
Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeShutdown(JNIEnv*, jclass) {
    logf("[dlssmc] DX shutdown\n");
    wait12();
    // 代理 swapchain 的引用必须先放掉，否则 XeSS 报 CRITICAL Integration issue
    g.swapchain.Reset();
    if (g.ffxSwap && g.pFfxDestroy) g.pFfxDestroy(&g.ffxSwap, nullptr);
    if (g.ffxEffect && g.pFfxDestroy) g.pFfxDestroy(&g.ffxEffect, nullptr);
    g.ffxSwap = g.ffxEffect = nullptr;
    // 超分上下文要在输入纹理与设备之前销毁：它内部还引用着这些资源
    if (g.ffxUpscale && g.pFfxDestroy) g.pFfxDestroy(&g.ffxUpscale, nullptr);
    g.ffxUpscale = nullptr;
    if (g.xess && g.pXessDestroy) g.pXessDestroy(g.xess);
    g.xess = nullptr;
    g.srOut.Reset();
    g.srQuality = -1;
    // 互操作对象必须先解锁才能注销。in[5] 的锁是跨 Java 调用持有的（upscale -> 贴回主 target ->
    // upscaleDone），中间真出了异常就会留在这儿，所以关机时兜一次。
    if (g.outLocked && g.interop) pUnlockObjects(g.interop, 1, &g.in[5].reg);
    g.outLocked = false;
    for (auto& t : g.in) releaseInput(t);
    g.lockFrom = g.lockCount = 0;
    if (g.xefg && g.pXefgDestroy) g.pXefgDestroy(g.xefg);
    if (g.xell && g.pXellDestroy) g.pXellDestroy(g.xell);
    g.xefg = nullptr;
    g.xell = nullptr;
    if (g.interop && pCloseDevice) pCloseDevice(g.interop);
    g.interop = nullptr;
    if (g.evt11) CloseHandle(g.evt11);
    if (g.evt12) CloseHandle(g.evt12);
    g.evt11 = g.evt12 = nullptr;
    for (int i = 0; i < kFramesInFlight; ++i) {
        g.cmd[i].Reset();
        g.alloc[i].Reset();
    }
    g.fence11.Reset();
    g.fence12.Reset();
    g.factory.Reset();
    g.dev11_5.Reset();
    g.ctx11_4.Reset();
    g.ctx11.Reset();
    g.d3d11.Reset();
    g.queue.Reset();
    g.dev12.Reset();
    if (g.child) DestroyWindow(g.child);
    g.child = nullptr;
    g.childVisible = false;
    g.ready = false;
    g.haveInputs = false;
    g.locked = false;
    // 厂商 DLL 不 FreeLibrary：它们带线程/静态状态，卸载会踩空（探针实测段错误）
    if (g_log) {
        fclose(g_log);
        g_log = nullptr;
    }
    g = DxCtx{};
}

} // extern "C"
