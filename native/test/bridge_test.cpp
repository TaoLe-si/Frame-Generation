// GL -> D3D12 桥接最小可行性验证
//
// 为什么需要它：DLSS 帧生成（sl.dlss_g）只支持 D3D12 / Vulkan，
// 而 Minecraft Java 在 Windows 上是 OpenGL。所以必须能把 GL 画出来的画面搬进 D3D12。
//
// 已验证的结论：
//   - 路线 A（WGL_NV_DX_interop2 + D3D11On12）：失败，wglDXOpenDeviceNV 拒绝 D3D11On12 设备
//   - 在已挂 GL 上下文的 HWND 上建 D3D12 swapchain：成功（接管上屏可行）
//
// 本程序验证路线 D：
//   GL --WGL_NV_DX_interop2--> 普通 D3D11 纹理 --KMT 或 NT 共享句柄--> D3D12::OpenSharedHandle
//
// 判定标准：GL 把纹理清成 (200,40,60)，D3D12 侧读回来也是 (200,40,60)。
//
// 2026-09-22 在本机（RTX 4070 Laptop，驱动 610.74，GL 4.6）实测结论：
//   - 只有 MISC_SHARED（KMT）那组能被 GL 注册；带 SHARED_NTHANDLE 的两组创建成功但注册 GL 失败
//   - 只有 SHARED|NTHANDLE / SHARED_NTHANDLE|KEYEDMUTEX 能拿到 NT 句柄，而这两组 GL 注册不了
//   - D3D12 能 OpenSharedHandle(KMT)，但锁内+Flush 与解锁+Flush 两种时序读回都是 0
//   - D3D11 与 D3D12 的默认适配器 LUID 相同（0_91618），排除集显干扰
//   => 路线 D 不成立：**D3D12 直接打开 GL 注册的那张 KMT 纹理读不到 GL 写的内容**。
//      但 delivery_probe.exe 实测出可行做法：D3D11 内部 CopyResource 到一张跨 API(NT) 纹理，
//      再让 D3D12 打开那张 NT 纹理，像素正确（一帧四个 1080p 输入约 0.3 ms）。
//   （早先一次运行把「重复注册同一个 GL 纹理」的失败误记成了 MiscFlags 的限制，
//     现在每组的探测注册都会立即注销，矩阵才是可信的。）
//
// 编译：native/test/build.sh

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <d3d11on12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

// ------------------------------------------------------------------ GL 常量
#define GL_TEXTURE_2D 0x0DE1
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_RGBA8 0x8058
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_COLOR_BUFFER_BIT 0x00004000

#define WGL_ACCESS_READ_WRITE_NV 0x0001

#define LOG(...)            \
    do {                    \
        printf(__VA_ARGS__); \
        fflush(stdout);     \
    } while (0)

static void fail(const char* msg) {
    LOG("\n[FAIL] %s\n>>> 本条路线不可行\n", msg);
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

// ------------------------------------------------------------------ GL 函数
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

static PFN_glGenTextures glGenTextures_;
static PFN_glBindTexture glBindTexture_;
static PFN_glTexImage2D glTexImage2D_;
static PFN_glGenFramebuffers glGenFramebuffers_;
static PFN_glBindFramebuffer glBindFramebuffer_;
static PFN_glFramebufferTexture2D glFramebufferTexture2D_;
static PFN_glViewport glViewport_;
static PFN_glClearColor glClearColor_;
static PFN_glClear glClear_;
static PFN_glFinish glFinish_;

template <typename T>
static T loadGl(const char* name) {
    auto get = reinterpret_cast<PFN_wglGetProcAddress>(
        GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglGetProcAddress"));
    void* p = get ? get(name) : nullptr;
    if (!p) {
        // GL 1.1 的函数由 opengl32.dll 直接导出，不走 wglGetProcAddress
        p = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"opengl32.dll"), name));
    }
    if (!p) {
        char buf[128];
        snprintf(buf, sizeof(buf), "取不到 GL 函数 %s", name);
        fail(buf);
    }
    return reinterpret_cast<T>(p);
}

// ------------------------------------------------------------------ WGL interop
using PFN_wglDXOpenDeviceNV = HANDLE(WINAPI*)(void*);
using PFN_wglDXRegisterObjectNV = HANDLE(WINAPI*)(HANDLE, void*, unsigned, unsigned, unsigned);
using PFN_wglDXLockObjectsNV = BOOL(WINAPI*)(HANDLE, int, HANDLE*);
using PFN_wglDXUnlockObjectsNV = BOOL(WINAPI*)(HANDLE, int, HANDLE*);
using PFN_wglDXCloseDeviceNV = BOOL(WINAPI*)(HANDLE);
using PFN_wglDXUnregisterObjectNV = void(WINAPI*)(HANDLE, HANDLE);

static PFN_wglDXOpenDeviceNV pOpenDevice;
static PFN_wglDXRegisterObjectNV pRegisterObject;
static PFN_wglDXLockObjectsNV pLockObjects;
static PFN_wglDXUnlockObjectsNV pUnlockObjects;
static PFN_wglDXCloseDeviceNV pCloseDevice;
static PFN_wglDXUnregisterObjectNV pUnregisterObject;

// ------------------------------------------------------------------ 窗口 / 上下文
static HWND makeWindow(HINSTANCE hInst) {
    const wchar_t* cls = L"DlssmcBridgeTestCls";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = hInst;
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, cls, L"GL->D3D12 bridge test", WS_OVERLAPPEDWINDOW,
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

    typedef const char* (*PFN_glGetString)(unsigned);
    auto glGetString = reinterpret_cast<PFN_glGetString>(
        GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "glGetString"));
    if (glGetString) {
        LOG("  [info] GL_VERSION = %s\n", glGetString(0x1F02));
    }

    auto getProc = reinterpret_cast<PFN_wglGetProcAddress>(
        GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglGetProcAddress"));
    auto createAttribs = reinterpret_cast<PFN_wglCreateContextAttribs>(
        getProc("wglCreateContextAttribsARB"));

    if (createAttribs) {
        int versions[][2] = {{4, 6}, {4, 5}, {4, 4}, {4, 3}, {3, 3}, {3, 2}};
        for (auto& v : versions) {
            // WGL_CONTEXT_MAJOR=0x2091 MINOR=0x2092 PROFILE_MASK=0x9126 CORE_BIT=1
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
    LOG("  [info] 退回临时（compat）上下文\n");
    if (!wglMakeCurrent(hdc, temp)) fail("wglMakeCurrent 失败");
    return temp;
}

// ------------------------------------------------------------------ 主流程
int main() {
    LOG("=== GL -> D3D12 桥接验证（路线 D: WGL interop -> D3D11 -> NT 共享句柄 -> D3D12）===\n\n");

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HWND hwnd = makeWindow(hInst);
    LOG("  [ok] 窗口创建\n");

    // ---------- 1. GL 侧渲染 ----------
    HGLRC glCtx = makeGlContext(hwnd);

    glGenTextures_ = loadGl<PFN_glGenTextures>("glGenTextures");
    glBindTexture_ = loadGl<PFN_glBindTexture>("glBindTexture");
    glTexImage2D_ = loadGl<PFN_glTexImage2D>("glTexImage2D");
    glGenFramebuffers_ = loadGl<PFN_glGenFramebuffers>("glGenFramebuffers");
    glBindFramebuffer_ = loadGl<PFN_glBindFramebuffer>("glBindFramebuffer");
    glFramebufferTexture2D_ = loadGl<PFN_glFramebufferTexture2D>("glFramebufferTexture2D");
    glViewport_ = loadGl<PFN_glViewport>("glViewport");
    glClearColor_ = loadGl<PFN_glClearColor>("glClearColor");
    glClear_ = loadGl<PFN_glClear>("glClear");
    glFinish_ = loadGl<PFN_glFinish>("glFinish");

    const int W = 256, H = 256;
    unsigned glTex = 0;
    glGenTextures_(1, &glTex);
    glBindTexture_(GL_TEXTURE_2D, glTex);
    glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    unsigned fbo = 0;
    glGenFramebuffers_(1, &fbo);
    glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, glTex, 0);
    glViewport_(0, 0, W, H);
    glClearColor_(200.0f / 255.0f, 40.0f / 255.0f, 60.0f / 255.0f, 1.0f);
    glClear_(GL_COLOR_BUFFER_BIT);
    glFinish_();
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    LOG("  [ok] GL 渲染到纹理 %ux%u（目标色 200,40,60）\n", W, H);

    // GL 扩展探测（路线 B 的可行性参考）
    {
        typedef const char* (*PFN_glGetStringi)(unsigned, unsigned);
        typedef void (*PFN_glGetIntegerv)(unsigned, int*);
        auto glGetStringi = reinterpret_cast<PFN_glGetStringi>(
            GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "glGetStringi"));
        auto glGetIntegerv = reinterpret_cast<PFN_glGetIntegerv>(
            GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "glGetIntegerv"));
        int n = 0;
        if (glGetIntegerv) glGetIntegerv(0x821D, &n);
        bool memObj = false, sem = false;
        for (int i = 0; i < n && glGetStringi; ++i) {
            const char* e = glGetStringi(0x1F03, (unsigned)i);
            if (!e) continue;
            if (strstr(e, "GL_EXT_memory_object")) memObj = true;
            if (strstr(e, "GL_EXT_semaphore")) sem = true;
        }
        LOG("  [info] GL_EXT_memory_object=%s GL_EXT_semaphore=%s（路线 B 参考，%d 个扩展）\n",
            memObj ? "有" : "无", sem ? "有" : "无", n);
    }

    // ---------- 2. D3D12 ----------
    ComPtr<ID3D12Device> d3d12;
    checkHr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12)),
            "D3D12CreateDevice");

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    checkHr(d3d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "创建 D3D12 命令队列");

    // ---------- 3. 在带 GL 上下文的 HWND 上建 D3D12 swapchain ----------
    {
        ComPtr<IDXGIFactory4> factory;
        if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
            DXGI_SWAP_CHAIN_DESC1 scd{};
            scd.Width = 640;
            scd.Height = 480;
            scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            scd.SampleDesc.Count = 1;
            scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scd.BufferCount = 2;
            scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            ComPtr<IDXGISwapChain1> sc;
            HRESULT hr = factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &scd, nullptr, nullptr,
                                                         sc.GetAddressOf());
            LOG("  [%s] 在带 GL 上下文的 HWND 上建 D3D12 swapchain\n",
                SUCCEEDED(hr) ? "ok" : "!!");
        }
    }

    // ---------- 4. 普通 D3D11 设备（不是 On12） ----------
    ComPtr<ID3D11Device> d3d11;
    ComPtr<ID3D11DeviceContext> d3d11Ctx;
    checkHr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                              D3D11_SDK_VERSION, d3d11.GetAddressOf(), nullptr,
                              d3d11Ctx.GetAddressOf()), "D3D11CreateDevice");

    // ---------- 4b. 确认两台设备是不是同一块卡（本机还有 Radeon 780M 集显）----------
    ComPtr<IDXGIDevice> dxgiDev11;
    ComPtr<IDXGIAdapter> legacy11;
    ComPtr<IDXGIAdapter1> adapter11;
    checkHr(d3d11.As(&dxgiDev11), "D3D11 取 IDXGIDevice");
    checkHr(dxgiDev11->GetAdapter(&legacy11), "取 D3D11 适配器");
    legacy11.As(&adapter11);
    DXGI_ADAPTER_DESC desc11{};
    legacy11->GetDesc(&desc11);
    ComPtr<ID3D12Device1> d3d12Dev1;
    checkHr(d3d12.As(&d3d12Dev1), "D3D12 取 ID3D12Device1");
    LUID luid12 = d3d12Dev1->GetAdapterLuid();
    LOG("  [info] D3D11 适配器 = %ls vendor=0x%04X LUID=%ld_%lu\n", desc11.Description,
        desc11.VendorId, (long)desc11.AdapterLuid.HighPart,
        (unsigned long)desc11.AdapterLuid.LowPart);
    LOG("  [info] D3D12 默认   = LUID=%ld_%lu\n", (long)luid12.HighPart,
        (unsigned long)luid12.LowPart);
    if (desc11.AdapterLuid.HighPart != luid12.HighPart ||
        desc11.AdapterLuid.LowPart != luid12.LowPart) {
        LOG("  [!!] 两块卡不是同一个适配器，改用 D3D11 那块重建 D3D12\n");
        checkHr(D3D12CreateDevice(adapter11.Get(), D3D_FEATURE_LEVEL_11_0,
                                  IID_PPV_ARGS(&d3d12)), "在 D3D11 适配器上建 D3D12");
        checkHr(d3d12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "重建 D3D12 命令队列");
    } else {
        LOG("  [ok] D3D11/D3D12 同一个适配器\n");
    }

    // ---------- 5. WGL 互操作 ----------
    auto getProc = reinterpret_cast<PFN_wglGetProcAddress>(
        GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglGetProcAddress"));
    pOpenDevice = reinterpret_cast<PFN_wglDXOpenDeviceNV>(getProc("wglDXOpenDeviceNV"));
    pRegisterObject = reinterpret_cast<PFN_wglDXRegisterObjectNV>(getProc("wglDXRegisterObjectNV"));
    pLockObjects = reinterpret_cast<PFN_wglDXLockObjectsNV>(getProc("wglDXLockObjectsNV"));
    pUnlockObjects = reinterpret_cast<PFN_wglDXUnlockObjectsNV>(getProc("wglDXUnlockObjectsNV"));
    pCloseDevice = reinterpret_cast<PFN_wglDXCloseDeviceNV>(getProc("wglDXCloseDeviceNV"));
    pUnregisterObject = reinterpret_cast<PFN_wglDXUnregisterObjectNV>(getProc("wglDXUnregisterObjectNV"));
    if (!pOpenDevice || !pRegisterObject) fail("驱动不支持 WGL_NV_DX_interop2");

    HANDLE interop = pOpenDevice(d3d11.Get());
    if (!interop) fail("wglDXOpenDeviceNV 连普通 D3D11 设备都拒绝了");
    LOG("  [ok] wglDXOpenDeviceNV 接受普通 D3D11 设备\n");

    // ---------- 6. 建可跨 API 共享的 D3D11 纹理 ----------
    // 试几组 MiscFlags，跑出能力矩阵：能否创建 / 能否注册 GL / 能否拿到 NT 句柄
    struct Combo { UINT flags; const char* name; };
    const Combo combos[] = {
        {D3D11_RESOURCE_MISC_SHARED, "SHARED"},
        {D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, "SHARED|KEYEDMUTEX"},
        {D3D11_RESOURCE_MISC_SHARED_NTHANDLE, "SHARED_NTHANDLE"},
        {D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX,
         "SHARED_NTHANDLE|KEYEDMUTEX"},
        {D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE, "SHARED|NTHANDLE"},
    };

    LOG("\n  --- MiscFlags 能力矩阵 ---\n");
    LOG("  %-30s %-8s %-10s %-12s %s\n", "组合", "创建", "注册GL", "KMT句柄", "NT句柄");

    // 逐组探测时必须在下一组之前注销：同一个 GL 纹理只能挂在一个 D3D11 资源上，
    // 否则后面几组的「注册失败」是重复注册造成的，不是 MiscFlags 的限制。
    auto makeTexture = [&](UINT flags, ComPtr<ID3D11Texture2D>& out) -> bool {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = W;
        td.Height = H;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        td.MiscFlags = flags;
        return SUCCEEDED(d3d11->CreateTexture2D(&td, nullptr, out.GetAddressOf()));
    };

    struct Probe { ComPtr<ID3D11Texture2D> tex; HANDLE reg; HANDLE kmt; HANDLE nt; };
    Probe viable[8];
    int n = 0;
    for (const auto& c : combos) {
        ComPtr<ID3D11Texture2D> t;
        if (!makeTexture(c.flags, t)) {
            LOG("  %-30s %-8s 0x%08X\n", c.name, "失败", (unsigned)E_FAIL);
            continue;
        }

        HANDLE r = pRegisterObject(interop, t.Get(), glTex, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
        const char* regStr = r ? "成功" : "失败";

        HANDLE kmt = nullptr;
        ComPtr<IDXGIResource> dxgi0;
        if (SUCCEEDED(t.As(&dxgi0))) dxgi0->GetSharedHandle(&kmt);

        HANDLE nt = nullptr;
        ComPtr<IDXGIResource1> dxgi1;
        if (SUCCEEDED(t.As(&dxgi1))) {
            if (FAILED(dxgi1->CreateSharedHandle(
                    nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                    nullptr, &nt))) {
                nt = nullptr;
            }
        }

        LOG("  %-30s %-8s %-10s %-12s %s\n", c.name, "成功", regStr,
            kmt ? "有" : "无", nt ? "有" : "无");

        // 探测用的注册必须在下一组之前注销（同一个 GL 纹理只能挂一个资源），
        // 真正测试时再重新注册一次
        if (r && (nt || kmt) && n < 8) {
            viable[n++] = {t, nullptr, kmt, nt};
        } else if (nt) {
            CloseHandle(nt);
        }
        if (r) pUnregisterObject(interop, r);
    }
    LOG("  -------------------------\n\n");

    if (!n) fail("没有任何一组 MiscFlags 能同时满足「注册 GL」和「拿到跨 API 句柄」");

    const UINT64 rowPitch = ((UINT64)W * 4 + 255) & ~255ULL;
    const UINT64 readbackSize = rowPitch * H;
    ComPtr<ID3D12CommandAllocator> alloc;
    checkHr(d3d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)),
            "创建命令分配器");
    ComPtr<ID3D12GraphicsCommandList> cmd;
    checkHr(d3d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                     IID_PPV_ARGS(&cmd)), "创建命令列表");
    ComPtr<ID3D12Fence> fence;
    d3d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    D3D12_RESOURCE_DESC rbd{};
    rbd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rbd.Width = readbackSize;
    rbd.Height = 1;
    rbd.DepthOrArraySize = 1;
    rbd.MipLevels = 1;
    rbd.SampleDesc.Count = 1;
    rbd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES rbHeap{};
    rbHeap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    checkHr(d3d12->CreateCommittedResource(&rbHeap, D3D12_HEAP_FLAG_NONE, &rbd,
                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                           IID_PPV_ARGS(&readback)), "创建回读缓冲");

    // GL 侧把 glTex 清成 (200,40,60)；D3D12 能从共享纹理读回同一颜色才算桥接可用
    auto verifyInD3D12 = [&](ID3D12Resource* shared, const char* via) -> bool {
        alloc->Reset();
        cmd->Reset(alloc.Get(), nullptr);
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = shared;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        dst.PlacedFootprint.Footprint.Width = W;
        dst.PlacedFootprint.Footprint.Height = H;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = rowPitch;

        D3D12_RESOURCE_BARRIER bar{};
        bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bar.Transition.pResource = shared;
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &bar);
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        cmd->ResourceBarrier(1, &bar);
        checkHr(cmd->Close(), "关闭命令列表");

        ID3D12CommandList* lists[] = {cmd.Get()};
        queue->ExecuteCommandLists(1, lists);
        static UINT64 fenceValue = 0;
        queue->Signal(fence.Get(), ++fenceValue);
        fence->SetEventOnCompletion(fenceValue, evt);
        DWORD waited = WaitForSingleObject(evt, 10000);
        if (waited != WAIT_OBJECT_0) {
            LOG("  [!!] %s：D3D12 拷贝没有在 10s 内完成（waited=%lu）\n", via, waited);
            return false;
        }

        unsigned char* mapped = nullptr;
        D3D12_RANGE range{0, (SIZE_T)readbackSize};
        checkHr(readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "Map 回读缓冲");
        // DXGI_FORMAT_R8G8B8A8_UNORM 的字节序就是 R,G,B,A（BGRA 是 B8G8R8A8 那一族）
        unsigned char* px = mapped + 100 * rowPitch + 100 * 4;
        unsigned char r = px[0], g = px[1], b = px[2], a = px[3];
        readback->Unmap(0, nullptr);
        bool ok = (r == 200 && g == 40 && b == 60);
        LOG("  [%s] %s：D3D12 读回 (100,100) = R=%u G=%u B=%u A=%u，GL 写的是 200/40/60/255 → %s\n",
            ok ? "ok" : "!!", via, r, g, b, a, ok ? "颜色一致" : "颜色不符");
        return ok;
    };

    bool anyOk = false;
    for (int i = 0; i < n; ++i) {
        Probe& p = viable[i];
        p.reg = pRegisterObject(interop, p.tex.Get(), glTex, GL_TEXTURE_2D, WGL_ACCESS_READ_WRITE_NV);
        if (!p.reg) {
            LOG("  [!!] 重新注册 GL 失败\n");
            continue;
        }
        if (!pLockObjects(interop, 1, &p.reg)) fail("wglDXLockObjectsNV 失败");
        glFinish_();
        bool unlocked = false;

        ComPtr<ID3D12Resource> d3d12Tex;
        HRESULT hr = E_FAIL;
        const char* via = nullptr;
        if (p.nt) {
            hr = d3d12->OpenSharedHandle(p.nt, IID_PPV_ARGS(d3d12Tex.GetAddressOf()));
            if (SUCCEEDED(hr)) via = "NT";
            else d3d12Tex.Reset();
        }
        if (FAILED(hr) && p.kmt) {
            hr = d3d12->OpenSharedHandle(p.kmt, IID_PPV_ARGS(d3d12Tex.GetAddressOf()));
            if (SUCCEEDED(hr)) via = "KMT";
            else d3d12Tex.Reset();
        }
        if (FAILED(hr)) {
            LOG("  [!!] D3D12 打不开该组合的共享句柄 (NT/KMT, hr=0x%08X)\n", (unsigned)hr);
        } else {
            // 两种锁序都测：跨设备共享通常要 D3D11 先 Flush 才对 D3D12 可见
            d3d11Ctx->Flush();
            anyOk |= verifyInD3D12(d3d12Tex.Get(), "锁内+Flush");
            pUnlockObjects(interop, 1, &p.reg);
            unlocked = true;
            d3d11Ctx->Flush();
            anyOk |= verifyInD3D12(d3d12Tex.Get(), "解锁+Flush");
        }
        if (!unlocked) pUnlockObjects(interop, 1, &p.reg);
        pUnregisterObject(interop, p.reg);
        p.reg = nullptr;
    }

    LOG("\n=========================================\n");
    LOG("  %s\n", anyOk ? "通过：GL -> D3D12 桥接可用" : "不通过：没有一条共享路径能把 GL 的画面搬进 D3D12");
    LOG("=========================================\n");

    for (int i = 0; i < n; ++i) {
        if (viable[i].nt) CloseHandle(viable[i].nt);
    }
    if (interop) pCloseDevice(interop);
    (void)glCtx;

    return anyOk ? 0 : 2;
}
