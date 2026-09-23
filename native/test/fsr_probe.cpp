// FSR 帧生成（amd_fidelityfx_framegeneration_dx12.dll）在本机的能力探针
//
// 要回答的窄问题：这份 38 MB 的 FG 4.0.1 DLL 在 Win10 19045 + RTX 4070 Laptop 上能不能用。
// loader 与 provider 导出的是同一组 5 个函数，所以两条路都测一遍，才能把
// 「provider 自己拒绝（OS/硬件）」和「loader 没找到 provider」分开：
//   A) 经 amd_fidelityfx_loader_dx12.dll
//   B) 直接 LoadLibrary provider 自己
// 每条都做：ffxQuery(GET_VERSIONS) -> ffxCreateContext(FG 效果) -> ffxCreateContext(代理 swapchain)
//
// 依赖：native/vendor/amd/{include,bin}，头文件按 FidelityFX-SDK v2.3.0 的官方目录层次放，
// 相对 #include 才能解析。运行时 LoadLibrary，不链接 import lib。
// 窗口 WS_POPUP 放屏幕外且不 ShowWindow。编译：native/test/build_fsr_probe.sh

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winver.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "api/include/ffx_api.h"
#include "api/include/dx12/ffx_api_dx12.h"
#include "framegeneration/include/ffx_framegeneration.h"
#include "framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"

using Microsoft::WRL::ComPtr;

static const uint32_t W = 1280, H = 720;

#define LOG(...)         \
    do {                  \
        printf(__VA_ARGS__); \
        fflush(stdout);   \
    } while (0)

static void fail(const char* msg) {
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
static const char* rcName(ffxReturnCode_t r) {
    switch (r) {
        case FFX_API_RETURN_OK: return "OK";
        case FFX_API_RETURN_ERROR: return "ERROR";
        case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "UNKNOWN_DESCTYPE";
        case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "RUNTIME_ERROR";
        case FFX_API_RETURN_NO_PROVIDER: return "NO_PROVIDER";
        case FFX_API_RETURN_ERROR_MEMORY: return "ERROR_MEMORY";
        case FFX_API_RETURN_ERROR_PARAMETER: return "ERROR_PARAMETER";
        case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "PROVIDER_NO_SUPPORT_NEW_DESCTYPE";
        default: return "其他";
    }
}

struct Api {
    HMODULE mod = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxConfigure configure = nullptr;
};

static bool loadApi(Api& a, const std::string& dir, const char* file) {
    a.mod = LoadLibraryA((dir + file).c_str());
    if (!a.mod) {
        LOG("  [!!] LoadLibrary(%s) 失败 GLE=%lu\n", file, GetLastError());
        return false;
    }
    a.create = (PfnFfxCreateContext)GetProcAddress(a.mod, "ffxCreateContext");
    a.destroy = (PfnFfxDestroyContext)GetProcAddress(a.mod, "ffxDestroyContext");
    a.query = (PfnFfxQuery)GetProcAddress(a.mod, "ffxQuery");
    a.configure = (PfnFfxConfigure)GetProcAddress(a.mod, "ffxConfigure");
    if (!a.create || !a.destroy || !a.query || !a.configure) {
        LOG("  [!!] %s 缺少 ffx* 导出\n", file);
        return false;
    }
    LOG("  [ok] %s：5 个导出齐\n", file);
    return true;
}

// 一条路（loader 或 provider 直连）走一遍：问版本 -> 建效果上下文 -> 建代理 swapchain
static bool attempt(const Api& a, const char* via, ID3D12Device* dev, HWND hwnd,
                    ID3D12CommandQueue* queue, IDXGIFactory* factory) {
    bool any = false;
    LOG("\n  --- %s ---\n", via);

    ffxReturnCode_t r = FFX_API_RETURN_OK;
    for (int pass = 0; pass < 2; ++pass) {
        // pass 0 带设备（文档说这条查询就是按设备/驱动筛可用版本），pass 1 不带设备做对照
        uint64_t count = 0;
        ffxQueryDescGetVersions q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        q.device = pass == 0 ? (void*)dev : nullptr;
        q.outputCount = &count;
        r = a.query(nullptr, (ffxQueryDescHeader*)&q);
        LOG("  [%s] GET_VERSIONS(%s) -> %d(%s) 可用版本数=%llu\n",
            r == FFX_API_RETURN_OK ? "ok" : "!!", pass == 0 ? "带设备" : "不带设备", (int)r,
            rcName(r), (unsigned long long)count);
        if (r == FFX_API_RETURN_OK && count > 0 && count < 32) {
            std::vector<uint64_t> ids((size_t)count);
            std::vector<const char*> names((size_t)count, nullptr);
            uint64_t cap = count;
            ffxQueryDescGetVersions q2 = q;
            q2.outputCount = &cap;
            q2.versionIds = ids.data();
            q2.versionNames = names.data();
            if (a.query(nullptr, (ffxQueryDescHeader*)&q2) == FFX_API_RETURN_OK)
                for (uint64_t i = 0; i < cap; ++i)
                    LOG("      [%llu] id=0x%llX name=%s\n", (unsigned long long)i,
                        (unsigned long long)ids[i], names[i] ? names[i] : "(null)");
        }
    }

    ffxContext fg = nullptr;
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = dev;
    ffxCreateContextDescFrameGenerationVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    ver.header.pNext = (ffxCreateContextDescHeader*)&backend;
    ver.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateContextDescFrameGeneration fgd{};
    fgd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    fgd.header.pNext = (ffxCreateContextDescHeader*)&ver;
    fgd.displaySize = {W, H};
    fgd.maxRenderSize = {W, H};
    fgd.backBufferFormat = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM;
    r = a.create(&fg, (ffxCreateContextDescHeader*)&fgd, nullptr);
    LOG("  [%s] ffxCreateContext(FG 效果) -> %d(%s)\n", r == FFX_API_RETURN_OK ? "ok" : "!!",
        (int)r, rcName(r));
    if (r == FFX_API_RETURN_OK) {
        any = true;
        ffxQueryGetProviderVersion pv{};
        pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (a.query(&fg, (ffxQueryDescHeader*)&pv) == FFX_API_RETURN_OK)
            LOG("      实际 provider：%s (id=0x%llX)\n", pv.versionName ? pv.versionName : "(null)",
                (unsigned long long)pv.versionId);
        a.destroy(&fg, nullptr);
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = W;
    scd.Height = H;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferCount = 3;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain4* proxy = nullptr;
    ffxContext sc = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 s{};
    s.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    s.header.pNext = (ffxCreateContextDescHeader*)&backend;
    s.swapchain = &proxy;
    s.hwnd = hwnd;
    s.desc = &scd;
    s.dxgiFactory = factory;
    s.gameQueue = queue;
    r = a.create(&sc, (ffxCreateContextDescHeader*)&s, nullptr);
    LOG("  [%s] ffxCreateContext(代理 swapchain) -> %d(%s)，swapchain=%p\n",
        r == FFX_API_RETURN_OK ? "ok" : "!!", (int)r, rcName(r), (void*)proxy);
    if (r == FFX_API_RETURN_OK) {
        any = true;
        if (sc) a.destroy(&sc, nullptr);
    }
    if (proxy) proxy->Release();
    return any;
}

int main() {
    LOG("=== FSR 帧生成能力探针（%ux%u，窗口不显示，头里的 FG 版本 %d.%d.%d）===\n\n", W, H,
        FFX_FRAMEGENERATION_VERSION_MAJOR, FFX_FRAMEGENERATION_VERSION_MINOR,
        FFX_FRAMEGENERATION_VERSION_PATCH);

    char base[MAX_PATH];
    GetModuleFileNameA(nullptr, base, sizeof(base));
    std::string dir = std::string(base).substr(0, std::string(base).find_last_of('\\')) +
                      "\\..\\..\\vendor\\amd\\bin\\";

    // 先说清楚 DLL 自己声称是哪个版本：这跟「本机可用哪个版本」是两件事
    {
        std::string p = dir + "amd_fidelityfx_framegeneration_dx12.dll";
        DWORD fixed = GetFileVersionInfoSizeA(p.c_str(), nullptr);
        if (fixed) {
            std::vector<unsigned char> buf(fixed);
            if (GetFileVersionInfoA(p.c_str(), 0, fixed, buf.data())) {
                VS_FIXEDFILEINFO* ffi = nullptr;
                UINT len = 0;
                if (VerQueryValueA(buf.data(), "\\", (LPVOID*)&ffi, &len) && len)
                    LOG("  [info] provider DLL 文件版本 %u.%u.%u.%u\n",
                        HIWORD(ffi->dwFileVersionMS), LOWORD(ffi->dwFileVersionMS),
                        HIWORD(ffi->dwFileVersionLS), LOWORD(ffi->dwFileVersionLS));
            }
        }
    }

    HINSTANCE inst = GetModuleHandleW(nullptr);
    const wchar_t* cls = L"FsrProbeCls";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, cls, L"FSR probe", WS_POPUP, -32000, -32000,
                                (int)W, (int)H, nullptr, nullptr, inst, nullptr);
    if (!hwnd) fail("创建窗口失败");

    ComPtr<ID3D12Device> dev;
    checkHr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)),
            "D3D12CreateDevice");
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    checkHr(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "命令队列");
    ComPtr<IDXGIFactory2> f2;
    checkHr(CreateDXGIFactory2(0, IID_PPV_ARGS(&f2)), "DXGI factory");
    ComPtr<IDXGIFactory> factory;
    checkHr(f2.As(&factory), "转 IDXGIFactory");

    Api loader, provider;
    bool gotLoader = loadApi(loader, dir, "amd_fidelityfx_loader_dx12.dll");
    bool gotProvider = loadApi(provider, dir, "amd_fidelityfx_framegeneration_dx12.dll");

    bool okLoader = gotLoader && attempt(loader, "A) 经 loader", dev.Get(), hwnd, queue.Get(),
                                         factory.Get());
    bool okProvider = gotProvider && attempt(provider, "B) 直连 provider", dev.Get(), hwnd,
                                             queue.Get(), factory.Get());

    LOG("\n=========================================\n");
    LOG("  经 loader：%s\n  直连 provider：%s\n", okLoader ? "可用" : "不可用",
        okProvider ? "可用" : "不可用");
    LOG("  判定：%s\n",
        (okLoader || okProvider)
            ? "这份 FSR FG 在本机能建上下文，钠上 FSR 页可以给真选项"
            : "这份 FSR FG 在本机建不起上下文（provider 自己拒绝），FSR 页只能显示不支持原因");
    LOG("=========================================\n");
    ExitProcess((okLoader || okProvider) ? 0 : 2);
    return 0;
}
