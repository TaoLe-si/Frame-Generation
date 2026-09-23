// GL 画面交付给 D3D12 的可行性探针（FSR / XeSS 的地基判据）
//
// 背景：帧生成现在走 GL -> D3D11(KMT) -> Vulkan 导入。FSR(SDK 2.3) 与 XeSS-FG(3.0.2)
// 只有 D3D12 前端，所以必须先回答：GL 画的像素能不能进 D3D12？
//
// bridge_test.exe 已实测：只有 MISC_SHARED(KMT) 能被 GL 注册，而 D3D12 直接打开同一张
// KMT 读回全 0；带 SHARED_NTHANDLE 的纹理拿得到 NT 句柄但 GL 注册失败。
// 本程序因此测「两步走」和一条兜底路径，每条都要 D3D12 读回 GL 写的目标色才算通过：
//
//   P0  GL -> D3D11(KMT)：D3D11 自己拷到 STAGING 读回，确认这一腿通
//   P1  D3D11 内部 CopyResource 到跨 API(NT) 纹理 + Flush + fence 等完 -> D3D12 打开 NT 句柄读
//   P2  与 P1 相同但不 Flush 不等待（对照组：同步到底必不可省）
//   P3  反方向：D3D12 建共享资源 -> D3D11 OpenSharedResource1 拷入 -> D3D12 读
//   P4  兜底：GL glGetTexImage 回读到 CPU 再上传 D3D12，量 1080p 每帧毫秒代价
//
// 窗口不显示、不抢前台。编译：native/test/build_delivery.sh
// 判定基准：GL 把纹理清成 (200,40,60,255)。

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

#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

#define GL_TEXTURE_2D 0x0DE1
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_RGBA8 0x8058
#define GL_R32F 0x822E
#define GL_RG16F 0x822F
#define GL_RED 0x1903
#define GL_RG 0x8227
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_FLOAT 0x1406
#define GL_COLOR_BUFFER_BIT 0x00004000
#define GL_PACK_ALIGNMENT 0x0D05
#define WGL_ACCESS_READ_WRITE_NV 0x0001

// 机箱内 d3d12.h 只到 SHARED / SHARED_CROSS_ADAPTER，NT 句柄那个标志来自 Agility 头；
// 这里按枚举值补上，运行时不支持就让它报错，照样是一条实测数据。
#ifndef D3D12_HEAP_FLAG_SHARED_ALLOW_NT_HANDLES
#define D3D12_HEAP_FLAG_SHARED_ALLOW_NT_HANDLES ((D3D12_HEAP_FLAGS)0x40)
#endif

static const int W = 1920, H = 1080;   // 与实测整合包窗口同量级，P4 的代价才有意义
static const int TR = 200, TG = 40, TB = 60;
static unsigned g_glTex = 0;

#define LOG(...)         \
    do {                 \
        printf(__VA_ARGS__); \
        fflush(stdout);   \
    } while (0)

[[noreturn]] static void fail(const char* msg) {
    LOG("\n[FATAL] %s\n", msg);
    ExitProcess(1);
}
static void checkHr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char buf[192];
        snprintf(buf, sizeof(buf), "%s 失败 hr=0x%08X", what, (unsigned)hr);
        fail(buf);
    }
}

// ------------------------------------------------------------------ GL 装载
using PFN_wglGetProcAddress = void*(WINAPI*)(LPCSTR);
using PFN_glGenTextures = void(*)(int, unsigned*);
using PFN_glDeleteTextures = void(*)(int, const unsigned*);
using PFN_glBindTexture = void(*)(unsigned, unsigned);
using PFN_glTexImage2D = void(*)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
using PFN_glGenFramebuffers = void(*)(int, unsigned*);
using PFN_glDeleteFramebuffers = void(*)(int, const unsigned*);
using PFN_glBindFramebuffer = void(*)(unsigned, unsigned);
using PFN_glFramebufferTexture2D = void(*)(unsigned, unsigned, unsigned, unsigned, int);
using PFN_glViewport = void(*)(int, int, int, int);
using PFN_glClearColor = void(*)(float, float, float, float);
using PFN_glClear = void(*)(unsigned);
using PFN_glFinish = void(*)();
using PFN_glGetTexImage = void(*)(unsigned, int, unsigned, unsigned, void*);
using PFN_glPixelStorei = void(*)(unsigned, int);

static PFN_glGenTextures glGenTextures_;
static PFN_glDeleteTextures glDeleteTextures_;
static PFN_glBindTexture glBindTexture_;
static PFN_glTexImage2D glTexImage2D_;
static PFN_glGenFramebuffers glGenFramebuffers_;
static PFN_glDeleteFramebuffers glDeleteFramebuffers_;
static PFN_glBindFramebuffer glBindFramebuffer_;
static PFN_glFramebufferTexture2D glFramebufferTexture2D_;
static PFN_glViewport glViewport_;
static PFN_glClearColor glClearColor_;
static PFN_glClear glClear_;
static PFN_glFinish glFinish_;
static PFN_glGetTexImage glGetTexImage_;
static PFN_glPixelStorei glPixelStorei_;

template <typename T>
static T loadGl(const char* name) {
    auto get = reinterpret_cast<PFN_wglGetProcAddress>(
        GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglGetProcAddress"));
    void* p = get ? get(name) : nullptr;
    // wglGetProcAddress 对 GL 1.0/1.1 的函数会返回 NULL 或 GDI 的错误码（1..6、(void*)-1），
    // 那些值当函数指针用会直接段错误，所以只有落在正常地址范围才算取到。
    if (p != nullptr && (reinterpret_cast<uintptr_t>(p) <= 0xFFFF || p == reinterpret_cast<void*>(-1)))
        p = nullptr;
    if (!p) p = GetProcAddress(GetModuleHandleW(L"opengl32.dll"), name);
    if (!p) fail(name);
    return reinterpret_cast<T>(p);
}

using PFN_wglDXOpenDeviceNV = HANDLE(WINAPI*)(void*);
using PFN_wglDXRegisterObjectNV = HANDLE(WINAPI*)(HANDLE, void*, unsigned, unsigned, unsigned);
using PFN_wglDXLockObjectsNV = BOOL(WINAPI*)(HANDLE, int, HANDLE*);
using PFN_wglDXUnlockObjectsNV = BOOL(WINAPI*)(HANDLE, int, HANDLE*);
using PFN_wglDXCloseDeviceNV = BOOL(WINAPI*)(HANDLE);

static PFN_wglDXOpenDeviceNV pOpenDevice;
static PFN_wglDXRegisterObjectNV pRegisterObject;
static PFN_wglDXLockObjectsNV pLockObjects;
static PFN_wglDXUnlockObjectsNV pUnlockObjects;
static PFN_wglDXCloseDeviceNV pCloseDevice;

static double qpcMs(LARGE_INTEGER from) {
    static double perMs = 0.0;
    if (perMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        perMs = (double)f.QuadPart / 1000.0;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - from.QuadPart) / perMs;
}
static LARGE_INTEGER nowTick() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t;
}

// ------------------------------------------------------------------ 隐藏窗口 + GL 上下文
static HWND makeHiddenWindow(HINSTANCE inst) {
    const wchar_t* cls = L"DlssmcDeliveryProbeCls";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, cls, L"GL->D3D12 delivery probe",
                                WS_POPUP, -32000, -32000, 640, 480, nullptr, nullptr, inst, nullptr);
    if (!hwnd) fail("创建窗口失败");
    return hwnd;
}

static void makeGlContext(HWND hwnd) {
    HDC hdc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    int pf = ChoosePixelFormat(hdc, &pfd);
    if (!pf || !SetPixelFormat(hdc, pf, &pfd)) fail("像素格式设置失败");
    HGLRC gl = wglCreateContext(hdc);
    if (!gl) fail("wglCreateContext 失败");
    if (!wglMakeCurrent(hdc, gl)) fail("wglMakeCurrent 失败");
}

// ------------------------------------------------------------------ D3D12 侧公共件
struct D3D12Side {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;
    HANDLE evt = nullptr;
    UINT64 rowPitch = 0;
};
static D3D12Side g_d12;

static void initD3D12(ID3D12Device* dev) {
    g_d12.dev = dev;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    checkHr(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_d12.queue)), "D3D12 命令队列");
    checkHr(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_d12.alloc)),
            "D3D12 命令分配器");
    checkHr(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_d12.alloc.Get(), nullptr,
                                   IID_PPV_ARGS(&g_d12.cmd)), "D3D12 命令列表");
    checkHr(g_d12.cmd->Close(), "D3D12 命令列表初始 Close");
    checkHr(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_d12.fence)), "D3D12 fence");
    g_d12.evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    g_d12.rowPitch = ((UINT64)W * 4 + 255) & ~255ULL;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = g_d12.rowPitch * H;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    checkHr(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                                         D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                         IID_PPV_ARGS(&g_d12.readback)), "D3D12 回读缓冲");
}

static void d12Wait(UINT64 value) {
    g_d12.fence->SetEventOnCompletion(value, g_d12.evt);
    if (WaitForSingleObject(g_d12.evt, 20000) != WAIT_OBJECT_0) fail("D3D12 fence 等待超时");
}

// 共享纹理按 COMMON 前提读回并核对目标色
static bool d12Verify(ID3D12Resource* res, const char* label) {
    g_d12.alloc->Reset();
    checkHr(g_d12.cmd->Reset(g_d12.alloc.Get(), nullptr), "D3D12 Reset");
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = g_d12.readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = W;
    dst.PlacedFootprint.Footprint.Height = H;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = g_d12.rowPitch;

    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = res;
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_d12.cmd->ResourceBarrier(1, &bar);
    g_d12.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    g_d12.cmd->ResourceBarrier(1, &bar);
    checkHr(g_d12.cmd->Close(), "D3D12 Close");
    ID3D12CommandList* lists[] = {g_d12.cmd.Get()};
    g_d12.queue->ExecuteCommandLists(1, lists);
    g_d12.queue->Signal(g_d12.fence.Get(), ++g_d12.fenceValue);
    d12Wait(g_d12.fenceValue);

    unsigned char* mapped = nullptr;
    D3D12_RANGE range{0, (SIZE_T)(g_d12.rowPitch * H)};
    checkHr(g_d12.readback->Map(0, &range, reinterpret_cast<void**>(&mapped)), "D3D12 Map 回读");
    unsigned char* px = mapped + (H / 2) * g_d12.rowPitch + (W / 2) * 4;
    // DXGI_FORMAT_R8G8B8A8_UNORM 的字节序就是 R,G,B,A（BGRA 是 B8G8R8A8 那族格式）
    bool ok = px[0] == TR && px[1] == TG && px[2] == TB && px[3] == 255;
    LOG("  [%s] %s：D3D12 读回 R=%u G=%u B=%u A=%u（应为 %d/%d/%d/255）\n", ok ? "ok" : "!!",
        label, px[0], px[1], px[2], px[3], TR, TG, TB);
    g_d12.readback->Unmap(0, nullptr);
    return ok;
}

static bool d11Verify(ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* srcTex,
                      const char* label) {
    D3D11_TEXTURE2D_DESC sd{};
    srcTex->GetDesc(&sd);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, &staging))) {
        LOG("  [!!] %s：建 STAGING 失败\n", label);
        return false;
    }
    ctx->CopyResource(staging.Get(), srcTex);
    ctx->Flush();
    D3D11_MAPPED_SUBRESOURCE map{};
    checkHr(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map), "D3D11 Map STAGING");
    unsigned char* px = (unsigned char*)map.pData + (H / 2) * map.RowPitch + (W / 2) * 4;
    bool ok = px[0] == TR && px[1] == TG && px[2] == TB;
    LOG("  [%s] %s：D3D11 读回 R=%u G=%u B=%u A=%u\n", ok ? "ok" : "!!", label, px[0], px[1],
        px[2], px[3]);
    ctx->Unmap(staging.Get(), 0);
    return ok;
}

int main() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    LOG("=== GL -> D3D12 交付路径探针（%dx%d RGBA8，目标色 %d/%d/%d）===\n\n", W, H, TR, TG, TB);

    HWND hwnd = makeHiddenWindow(inst);
    makeGlContext(hwnd);
    glGenTextures_ = loadGl<PFN_glGenTextures>("glGenTextures");
    glDeleteTextures_ = loadGl<PFN_glDeleteTextures>("glDeleteTextures");
    glBindTexture_ = loadGl<PFN_glBindTexture>("glBindTexture");
    glTexImage2D_ = loadGl<PFN_glTexImage2D>("glTexImage2D");
    glGenFramebuffers_ = loadGl<PFN_glGenFramebuffers>("glGenFramebuffers");
    glDeleteFramebuffers_ = loadGl<PFN_glDeleteFramebuffers>("glDeleteFramebuffers");
    glBindFramebuffer_ = loadGl<PFN_glBindFramebuffer>("glBindFramebuffer");
    glFramebufferTexture2D_ = loadGl<PFN_glFramebufferTexture2D>("glFramebufferTexture2D");
    glViewport_ = loadGl<PFN_glViewport>("glViewport");
    glClearColor_ = loadGl<PFN_glClearColor>("glClearColor");
    glClear_ = loadGl<PFN_glClear>("glClear");
    glFinish_ = loadGl<PFN_glFinish>("glFinish");
    glGetTexImage_ = loadGl<PFN_glGetTexImage>("glGetTexImage");
    glPixelStorei_ = loadGl<PFN_glPixelStorei>("glPixelStorei");
    auto glGetString = reinterpret_cast<const unsigned char* (*)(unsigned)>(
        GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "glGetString"));
    LOG("  [info] GL_RENDERER = %s\n", glGetString ? (const char*)glGetString(0x1F01) : "?");

    auto getProc = reinterpret_cast<PFN_wglGetProcAddress>(
        GetProcAddress(GetModuleHandleW(L"opengl32.dll"), "wglGetProcAddress"));
    pOpenDevice = reinterpret_cast<PFN_wglDXOpenDeviceNV>(getProc("wglDXOpenDeviceNV"));
    pRegisterObject = reinterpret_cast<PFN_wglDXRegisterObjectNV>(getProc("wglDXRegisterObjectNV"));
    pLockObjects = reinterpret_cast<PFN_wglDXLockObjectsNV>(getProc("wglDXLockObjectsNV"));
    pUnlockObjects = reinterpret_cast<PFN_wglDXUnlockObjectsNV>(getProc("wglDXUnlockObjectsNV"));
    pCloseDevice = reinterpret_cast<PFN_wglDXCloseDeviceNV>(getProc("wglDXCloseDeviceNV"));
    if (!pOpenDevice || !pRegisterObject) fail("驱动不支持 WGL_NV_DX_interop2");

    ComPtr<ID3D11Device> d3d11;
    ComPtr<ID3D11DeviceContext> d3d11Ctx;
    checkHr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                              D3D11_SDK_VERSION, &d3d11, nullptr, &d3d11Ctx), "D3D11CreateDevice");
    ComPtr<ID3D11DeviceContext4> ctx4;
    HRESULT q4 = d3d11Ctx.As(&ctx4);   // 本 SDK 里 fence 由 ID3D11DeviceContext4::Signal 触发
    ComPtr<ID3D11Device5> d3d11Dev5;
    HRESULT q5 = d3d11.As(&d3d11Dev5);
    LOG("  [info] ID3D11Device5（fence）= %s\n", SUCCEEDED(q5) ? "可用" : "不可用");

    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    checkHr(d3d11.As(&dxgiDev), "D3D11 取 IDXGIDevice");
    checkHr(dxgiDev->GetAdapter(&adapter), "取 D3D11 适配器");
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);

    ComPtr<ID3D12Device> d3d12;
    checkHr(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12)),
            "在同一适配器上建 D3D12");
    initD3D12(d3d12.Get());
    LOG("  [info] 适配器 = %ls LUID=%ld_%lu\n\n", ad.Description, (long)ad.AdapterLuid.HighPart,
        (unsigned long)ad.AdapterLuid.LowPart);

    HANDLE interop = pOpenDevice(d3d11.Get());
    if (!interop) fail("wglDXOpenDeviceNV 拒绝 D3D11 设备");

    glGenTextures_(1, &g_glTex);
    glBindTexture_(GL_TEXTURE_2D, g_glTex);
    glTexImage2D_(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindTexture_(GL_TEXTURE_2D, 0);

    D3D11_TEXTURE2D_DESC kmt{};
    kmt.Width = W;
    kmt.Height = H;
    kmt.MipLevels = 1;
    kmt.ArraySize = 1;
    kmt.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    kmt.SampleDesc.Count = 1;
    kmt.Usage = D3D11_USAGE_DEFAULT;
    kmt.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    kmt.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    ComPtr<ID3D11Texture2D> texKmt;
    checkHr(d3d11->CreateTexture2D(&kmt, nullptr, &texKmt), "建 KMT 纹理");
    HANDLE regKmt = pRegisterObject(interop, texKmt.Get(), g_glTex, GL_TEXTURE_2D,
                                    WGL_ACCESS_READ_WRITE_NV);
    if (!regKmt) fail("KMT 纹理注册给 GL 失败（与生产路径不一致）");
    HANDLE objs[1] = {regKmt};
    if (!pLockObjects(interop, 1, objs)) fail("wglDXLockObjectsNV 失败");

    unsigned fbo = 0;
    glGenFramebuffers_(1, &fbo);
    // GL_FRAMEBUFFER 同时绑到 READ|DRAW：glClear 走 DRAW，glGetTexImage 走 READ
    glBindFramebuffer_(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_glTex, 0);
    glViewport_(0, 0, W, H);
    glClearColor_(TR / 255.0f, TG / 255.0f, TB / 255.0f, 1.0f);
    glClear_(GL_COLOR_BUFFER_BIT);
    glFinish_();

    bool p0 = d11Verify(d3d11.Get(), d3d11Ctx.Get(), texKmt.Get(), "P0 GL->D3D11(KMT)");

    // 跨 API 共享纹理：按文档必须 BindFlags=0 / CPUAccessFlags=0 / 单采样
    D3D11_TEXTURE2D_DESC ntd = kmt;
    ntd.BindFlags = 0;
    ntd.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ComPtr<ID3D11Texture2D> texNt;
    ComPtr<ID3D11Fence> fence11;
    if (SUCCEEDED(q5) && SUCCEEDED(q4))
        checkHr(d3d11Dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11)),
                "建 D3D11 fence");
    HANDLE evt11 = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 f11 = 0;
    // 等 D3D11 侧的拷贝真正落到 GPU 上，D3D12 才允许读
    auto waitForD3D11 = [&]() {
        d3d11Ctx->Flush();
        if (fence11) {
            checkHr(ctx4->Signal(fence11.Get(), ++f11), "D3D11 fence Signal");
            fence11->SetEventOnCompletion(f11, evt11);
            if (WaitForSingleObject(evt11, 20000) != WAIT_OBJECT_0) fail("D3D11 fence 等待超时");
        }
    };

    bool p1 = false, p2 = false, p3 = false, p4 = false, p5 = false;
    LOG("\n  --- P1/P2：D3D11 拷进跨 API(NT) 纹理 -> D3D12 打开 NT 句柄 ---\n");
    HRESULT cr = d3d11->CreateTexture2D(&ntd, nullptr, &texNt);
    if (FAILED(cr)) {
        LOG("  [!!] 建跨 API 纹理失败 hr=0x%08X\n", (unsigned)cr);
    } else {
        ComPtr<IDXGIResource1> res1;
        checkHr(texNt.As(&res1), "取 IDXGIResource1");
        HANDLE nt = nullptr;
        checkHr(res1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ |
                                                 DXGI_SHARED_RESOURCE_WRITE, nullptr, &nt),
                "D3D11 CreateSharedHandle");
        ComPtr<ID3D12Resource> d12tex;
        checkHr(d3d12->OpenSharedHandle(nt, IID_PPV_ARGS(d12tex.GetAddressOf())),
                "D3D12 打开 D3D11 的 NT 句柄");

        // P2 对照组：拷完立刻读，不 Flush 不等待
        d3d11Ctx->CopyResource(texNt.Get(), texKmt.Get());
        p2 = d12Verify(d12tex.Get(), "P2 不 Flush 不等待（对照）");

        // P1：Flush + D3D11 fence 等完再读
        d3d11Ctx->CopyResource(texNt.Get(), texKmt.Get());
        waitForD3D11();
        p1 = d12Verify(d12tex.Get(), "P1 Flush + fence 同步");
        CloseHandle(nt);
    }

    LOG("\n  --- P3：D3D12 建共享资源 -> D3D11 OpenSharedResource1 拷入 ---\n");
    {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        rd.Width = W;
        rd.Height = H;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> owned;
        // D3D11 的 OpenSharedResource1 只认 NT 句柄：D3D12 侧必须用 SHARED_ALLOW_NT_HANDLES，
        // 普通 SHARED 出来的句柄 CreateSharedHandle 会直接 E_INVALIDARG
        HRESULT hr = d3d12->CreateCommittedResource(
            &hp, D3D12_HEAP_FLAG_SHARED_ALLOW_NT_HANDLES, &rd, D3D12_RESOURCE_STATE_COMMON,
            nullptr, IID_PPV_ARGS(owned.GetAddressOf()));
        if (FAILED(hr)) {
            LOG("  [!!] D3D12 建共享纹理失败 hr=0x%08X\n", (unsigned)hr);
        } else {
            HANDLE shared = nullptr;
            hr = d3d12->CreateSharedHandle(owned.Get(), nullptr,
                                           DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                           nullptr, &shared);
            if (FAILED(hr)) {
                LOG("  [!!] D3D12 CreateSharedHandle 失败 hr=0x%08X\n", (unsigned)hr);
            } else if (FAILED(q5)) {
                LOG("  [!!] 这台机器没有 ID3D11Device5，无法 OpenSharedResource1\n");
                CloseHandle(shared);
            } else {
                ComPtr<ID3D11Texture2D> opened;
                hr = d3d11Dev5->OpenSharedResource1(shared, IID_PPV_ARGS(opened.GetAddressOf()));
                CloseHandle(shared);
                if (FAILED(hr)) {
                    LOG("  [!!] D3D11 OpenSharedResource1 失败 hr=0x%08X\n", (unsigned)hr);
                } else {
                    d3d11Ctx->CopyResource(opened.Get(), texKmt.Get());
                    waitForD3D11();
                    p3 = d12Verify(owned.Get(), "P3 D3D12 拥有 / D3D11 写入");
                }
            }
        }
    }

    // 注意：GL 纹理还注册在 WGL 互操作里，解锁之后再 glGetTexImage 会直接段错误（实测），
    // 所以 P4 也在锁窗口内跑。
    glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers_(1, &fbo);
    fbo = 0;

    LOG("\n  --- P4：GL 回读到 CPU 再上传 D3D12（兜底路径，量每帧代价）---\n");
    {
        const size_t bytes = (size_t)W * H * 4;
        std::vector<unsigned char> cpu(bytes);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot{};
        UINT rows = 0;
        UINT64 rowBytes = 0, total = 0;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        rd.Width = W;
        rd.Height = H;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rd.SampleDesc.Count = 1;
        d3d12->GetCopyableFootprints(&rd, 0, 1, 0, &foot, &rows, &rowBytes, &total);

        D3D12_HEAP_PROPERTIES upHp{};
        upHp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES defHp{};
        defHp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> upload, target;
        bool built =
            SUCCEEDED(d3d12->CreateCommittedResource(&upHp, D3D12_HEAP_FLAG_NONE, &bd,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                     IID_PPV_ARGS(upload.GetAddressOf()))) &&
            SUCCEEDED(d3d12->CreateCommittedResource(&defHp, D3D12_HEAP_FLAG_NONE, &rd,
                                                     D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                     IID_PPV_ARGS(target.GetAddressOf())));
        if (!built) {
            LOG("  [!!] 上传缓冲 / 目标纹理创建失败\n");
        } else {
            unsigned char* mapped = nullptr;
            checkHr(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map 上传缓冲");
            LOG("  [info] P4 可拷贝性：rows=%u 源行字节=%llu 对齐行=%llu 总=%llu mapped=%p\n", rows,
                (unsigned long long)rowBytes, (unsigned long long)foot.Footprint.RowPitch,
                (unsigned long long)total, (void*)mapped);
            glPixelStorei_(GL_PACK_ALIGNMENT, 4);
            glBindTexture_(GL_TEXTURE_2D, g_glTex);
            LOG("  [info] P4 准备回读 glGetTexImage(%p)\n", (void*)glGetTexImage_);
            double best = 1e9, sum = 0;
            const int iterations = 20;
            for (int i = 0; i < iterations; ++i) {
                if (i == 0) LOG("  [info] P4 第 0 帧：进 glGetTexImage\n");
                LARGE_INTEGER t0 = nowTick();
                glGetTexImage_(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, cpu.data());
                glFinish_();
                for (UINT y = 0; y < rows; ++y) {
                    memcpy(mapped + foot.Offset + (size_t)y * foot.Footprint.RowPitch,
                           cpu.data() + (size_t)y * rowBytes, (size_t)rowBytes);
                }
                g_d12.alloc->Reset();
                g_d12.cmd->Reset(g_d12.alloc.Get(), nullptr);
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = target.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = upload.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint = foot;
                D3D12_RESOURCE_BARRIER bar{};
                bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bar.Transition.pResource = target.Get();
                bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                g_d12.cmd->ResourceBarrier(1, &bar);
                g_d12.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                g_d12.cmd->ResourceBarrier(1, &bar);
                g_d12.cmd->Close();
                ID3D12CommandList* lists[] = {g_d12.cmd.Get()};
                g_d12.queue->ExecuteCommandLists(1, lists);
                g_d12.queue->Signal(g_d12.fence.Get(), ++g_d12.fenceValue);
                d12Wait(g_d12.fenceValue);
                double ms = qpcMs(t0);
                sum += ms;
                if (ms < best) best = ms;
            }
            glBindTexture_(GL_TEXTURE_2D, 0);
            upload->Unmap(0, nullptr);
            LOG("  [info] P4 每帧（回读+页对齐+提交+等完成）：均值 %.2f ms，最好 %.2f ms，数据量 %.1f MB\n",
                sum / iterations, best, bytes / 1048576.0);
            p4 = d12Verify(target.Get(), "P4 CPU 上传");
        }
    }

    pUnlockObjects(interop, 1, objs);

    // ---------- P5：按生产的一帧四个输入，量 P1 那条路的整帧代价 ----------
    // final + hudless（RGBA8）、depth（R32F）、motion（RG16F），三种格式都算过一遍
    LOG("\n  --- P5：一帧四个输入的交付代价（P1 那条路，逐格式验正确性）---\n");
    {
        struct Input { const char* name; unsigned glFmt; DXGI_FORMAT dxgi; unsigned glComponents; };
        const Input inputs[4] = {
            {"final(RGBA8)", GL_RGBA8, DXGI_FORMAT_R8G8B8A8_UNORM, GL_RGBA},
            {"hudless(RGBA8)", GL_RGBA8, DXGI_FORMAT_R8G8B8A8_UNORM, GL_RGBA},
            {"depth(R32F)", GL_R32F, DXGI_FORMAT_R32_FLOAT, GL_RED},
            {"motion(RG16F)", GL_RG16F, DXGI_FORMAT_R16G16_FLOAT, GL_RG},
        };
        HANDLE regs[4] = {};
        unsigned glTex[4] = {};
        ComPtr<ID3D11Texture2D> src[4], dst[4];
        ComPtr<ID3D12Resource> d12dst[4];
        bool ready = true;
        for (int i = 0; i < 4 && ready; ++i) {
            glGenTextures_(1, &glTex[i]);
            glBindTexture_(GL_TEXTURE_2D, glTex[i]);
            glTexImage2D_(GL_TEXTURE_2D, 0, inputs[i].glFmt, W, H, 0, inputs[i].glComponents,
                          GL_FLOAT, nullptr);
            glBindTexture_(GL_TEXTURE_2D, 0);

            D3D11_TEXTURE2D_DESC sd = kmt;
            sd.Format = inputs[i].dxgi;
            if (FAILED(d3d11->CreateTexture2D(&sd, nullptr, &src[i]))) ready = false;
            sd.BindFlags = 0;
            sd.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            if (FAILED(d3d11->CreateTexture2D(&sd, nullptr, &dst[i]))) ready = false;
            regs[i] = pRegisterObject(interop, src[i].Get(), glTex[i], GL_TEXTURE_2D,
                                      WGL_ACCESS_READ_WRITE_NV);
            if (!regs[i]) ready = false;
            ComPtr<IDXGIResource1> res1;
            HANDLE nt = nullptr;
            if (ready && (FAILED(dst[i].As(&res1)) ||
                          FAILED(res1->CreateSharedHandle(
                              nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                              nullptr, &nt)))) {
                ready = false;
            }
            if (ready) {
                HRESULT hrOpen =
                    d3d12->OpenSharedHandle(nt, IID_PPV_ARGS(d12dst[i].GetAddressOf()));
                CloseHandle(nt);
                if (FAILED(hrOpen)) {
                    LOG("  [!!] %s：D3D12 打开失败 hr=0x%08X\n", inputs[i].name, (unsigned)hrOpen);
                    ready = false;
                }
            }
        }

        if (!ready) {
            LOG("  [!!] 四个输入没能全部建起来，P5 未测\n");
        } else {
            if (!pLockObjects(interop, 4, regs)) fail("P5 锁定失败");
            unsigned fb = 0;
            glGenFramebuffers_(1, &fb);
            glBindFramebuffer_(GL_FRAMEBUFFER, fb);
            glViewport_(0, 0, W, H);
            // 每个输入清成不同值，读回能对上就说明格式/stride 没问题
            const float vals[4][4] = {{0.784f, 0.157f, 0.235f, 1.0f},
                                      {0.157f, 0.784f, 0.235f, 1.0f},
                                      {0.25f, 0, 0, 0},
                                      {0.5f, -0.25f, 0, 0}};
            for (int i = 0; i < 4; ++i) {
                glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                        glTex[i], 0);
                glClearColor_(vals[i][0], vals[i][1], vals[i][2], vals[i][3]);
                glClear_(GL_COLOR_BUFFER_BIT);
            }
            glFinish_();

            const int iterations = 30;
            double sum = 0, best = 1e9;
            for (int it = 0; it < iterations; ++it) {
                LARGE_INTEGER t0 = nowTick();
                for (int i = 0; i < 4; ++i) d3d11Ctx->CopyResource(dst[i].Get(), src[i].Get());
                waitForD3D11();
                double ms = qpcMs(t0);
                sum += ms;
                if (ms < best) best = ms;
            }
            LOG("  [info] 一帧 4 输入（各 %dx%d，共 %.1f MB）：%s 等完 均值 %.2f ms，单帧最好 %.2f ms\n",
                W, H, 4.0 * W * H * 4 / 1048576.0, "4 次 CopyResource + Flush + fence",
                sum / iterations, best);

            // 逐个读回核对
            bool allOk = true;
            for (int i = 0; i < 4; ++i) {
                D3D12_RESOURCE_DESC rd = d12dst[i]->GetDesc();
                D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot{};
                UINT rows = 0;
                UINT64 rowBytes = 0, total = 0;
                d3d12->GetCopyableFootprints(&rd, 0, 1, 0, &foot, &rows, &rowBytes, &total);
                g_d12.alloc->Reset();
                g_d12.cmd->Reset(g_d12.alloc.Get(), nullptr);
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = d12dst[i].Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION dstc{};
                dstc.pResource = g_d12.readback.Get();
                dstc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dstc.PlacedFootprint = foot;
                D3D12_RESOURCE_BARRIER bar{};
                bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                bar.Transition.pResource = d12dst[i].Get();
                bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
                bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                g_d12.cmd->ResourceBarrier(1, &bar);
                g_d12.cmd->CopyTextureRegion(&dstc, 0, 0, 0, &src, nullptr);
                bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
                g_d12.cmd->ResourceBarrier(1, &bar);
                g_d12.cmd->Close();
                ID3D12CommandList* lists[] = {g_d12.cmd.Get()};
                g_d12.queue->ExecuteCommandLists(1, lists);
                g_d12.queue->Signal(g_d12.fence.Get(), ++g_d12.fenceValue);
                d12Wait(g_d12.fenceValue);
                unsigned char* mapped = nullptr;
                D3D12_RANGE range{0, (SIZE_T)(g_d12.rowPitch * H)};
                g_d12.readback->Map(0, &range, reinterpret_cast<void**>(&mapped));
                unsigned char* px = mapped + (size_t)(H / 2) * foot.Footprint.RowPitch +
                                    (size_t)(W / 2) * 4;
                bool ok;
                if (i < 2) {
                    ok = px[0] == (unsigned char)(vals[i][0] * 255.0f + 0.5f) &&
                         px[1] == (unsigned char)(vals[i][1] * 255.0f + 0.5f) &&
                         px[2] == (unsigned char)(vals[i][2] * 255.0f + 0.5f) && px[3] == 255;
                    LOG("  [%s] %s：D3D12 读回 %u/%u/%u/%u\n", ok ? "ok" : "!!", inputs[i].name,
                        px[0], px[1], px[2], px[3]);
                } else if (i == 2) {
                    float v = *(float*)px;
                    ok = v > 0.249f && v < 0.251f;
                    LOG("  [%s] %s：D3D12 读回 %.4f（应为 0.25）\n", ok ? "ok" : "!!",
                        inputs[i].name, v);
                } else {
                    // 半精度的期望位模式：0.5 -> 0x3800（指数 13+... 即 2^-1），-0.25 -> 0xB400
                    unsigned short h0 = *(unsigned short*)px, h1 = *(unsigned short*)(px + 2);
                    ok = h0 == 0x3800 && h1 == 0xB400;
                    LOG("  [%s] %s：D3D12 读回半精度位模式 x=0x%04X y=0x%04X（应为 0x3800 / 0xB400）\n",
                        ok ? "ok" : "!!", inputs[i].name, h0, h1);
                }
                allOk = allOk && ok;
            }
            glBindFramebuffer_(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers_(1, &fb);
            pUnlockObjects(interop, 4, regs);
            LOG("  [%s] P5：%s\n", allOk ? "ok" : "!!",
                allOk ? "四个输入都按各自格式交付到了 D3D12" : "有输入没读对");
            p5 = allOk;
        }
        for (int i = 0; i < 4; ++i) {
            if (glTex[i]) glDeleteTextures_(1, &glTex[i]);
        }
    }

    LOG("\n=========================================\n");
    LOG("  P0  GL->D3D11(KMT)        ：%s\n", p0 ? "通" : "不通");
    LOG("  P1  跨 API(NT) + 同步     ：%s\n", p1 ? "通" : "不通");
    LOG("  P2  同 P1 但不 Flush 不等待：%s%s\n", p2 ? "通" : "不通",
        p2 && p1 ? "（这条不能单独当结论：颜色是常量，看不出漏帧）" : "");
    LOG("  P3  D3D12 拥有 + D3D11 写 ：%s\n", p3 ? "通" : "不通");
    LOG("  P4  CPU 回读上传          ：%s（代价见上面的均值）\n", p4 ? "通" : "不通");
    LOG("  P5  一帧 4 输入按格式交付 ：%s\n", p5 ? "通" : "不通");
    LOG("=========================================\n");

    if (interop) pCloseDevice(interop);
    if (evt11) CloseHandle(evt11);
    DestroyWindow(hwnd);
    return (p1 && p5) ? 0 : 2;
}
