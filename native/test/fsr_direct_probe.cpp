// FSR 帧生成「直驱 effect」可行性判据探针
//
// 要回答的唯一问题：**绕开 AMD 的代理 swapchain、自己填 ffxDispatchDescFrameGeneration 时，
// 这个 effect 认不认 numGeneratedFrames > 1？**
//
// 背景（见工作区 REVERSE-ENGINEERING.md §4.1）：
//   - 公开的 ABI 允许：ffxDispatchDescFrameGeneration 有 outputs[4] + numGeneratedFrames
//   - 但代理 swapchain 内部把帧型写死成 Interpolated_1 / Real 两态，
//     且没有任何配置键能改生成张数 —— 走代理就永远 2x
//   - 所以 >2x 只能自己驱动 effect。能不能成，取决于 effect 本人。
//
// 判据设计（只看像素，不依赖任何遥测）：
//   输出先填成中灰 (127,127,127)，真实帧在纯红/纯蓝间交替。
//   - outputs[0] 不再是中灰              -> effect 写了第 1 张
//   - outputs[1] 不再是中灰且与 [0] 不同 -> effect 写了第 2 张（多帧成立）
//   - 另有一张**从不交给 effect** 的纹理作对照，它必须一直是中灰。
//     对照若被动过，说明是填充/读回通路坏了 —— 这一条先于一切结论。
//
// 结构上的三个关键点（都是踩过的坑，生产后端同样适用）：
//   1. 上传/读回缓冲必须活到 gpuIdle 之后。函数一返回就析构 = 命令列表引用已释放资源
//      -> DXGI_ERROR_DEVICE_HUNG，现象是后续读回全 0，看起来像「没被写入」。
//   2. 填充、FFX、读回走**三条独立命令列表**依次执行。FFX 会对它拿到的资源做内部状态转换，
//      把读回录进同一条列表会让 StateBefore 猜错，Close 直接 E_FAIL。
//   3. 启动时先做一次「填充 + 读回」自检。它不通的话后面所有「未写入」都不能算数。
//
// 依赖：native/vendor/amd/{include,bin}。运行时 LoadLibrary，不链接 import lib。
// 窗口 WS_POPUP 放屏幕外且不 ShowWindow。编译：native/test/build_fsr_direct_probe.sh

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "api/include/ffx_api.h"
#include "api/include/dx12/ffx_api_dx12.h"
#include "framegeneration/include/ffx_framegeneration.h"
#include "framegeneration/include/ffx_framegeneration_api_types.h"

using Microsoft::WRL::ComPtr;

static const uint32_t W = 1280, H = 720;
static const uint32_t kMaxOut = 4;   // outputs 数组上限，与头文件一致
static const float kGrey[4] = {0.5f, 0.5f, 0.5f, 1.0f};

#define LOG(...)             \
    do {                     \
        printf(__VA_ARGS__); \
        fflush(stdout);      \
    } while (0)

// 调试层：非法调用不装它就被静默吞掉，只剩「读回全 0」这种没有信息的现象
static ComPtr<ID3D12InfoQueue> g_info;
static void drainD3d(const char* where) {
    if (!g_info) return;
    const UINT64 n = g_info->GetNumStoredMessages();
    UINT32 shown = 0;
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (g_info->GetMessage(i, nullptr, &len) != S_OK || !len) continue;
        std::string buf(len, '\0');
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(&buf[0]);
        if (g_info->GetMessage(i, m, &len) != S_OK) continue;
        if (m->Severity == D3D12_MESSAGE_SEVERITY_INFO ||
            m->Severity == D3D12_MESSAGE_SEVERITY_MESSAGE) {
            continue;
        }
        if (shown++ < 6) {
            LOG("    [d3d12] %s: %.*s\n", where, (int)m->DescriptionByteLength, m->pDescription);
        }
    }
    if (n) g_info->ClearStoredMessages();
}

// ------------------------------------------------------------------ 厂商入口

using PfnCreate = ffxReturnCode_t (*)(ffxContext*, ffxCreateContextDescHeader*,
                                      const ffxAllocationCallbacks*);
using PfnDestroy = ffxReturnCode_t (*)(ffxContext*, const ffxAllocationCallbacks*);
using PfnConfigure = ffxReturnCode_t (*)(ffxContext*, ffxConfigureDescHeader*);
using PfnDispatch = ffxReturnCode_t (*)(ffxContext*, ffxDispatchDescHeader*);

struct Api {
    HMODULE mod = nullptr;
    PfnCreate create = nullptr;
    PfnDestroy destroy = nullptr;
    PfnConfigure configure = nullptr;
    PfnDispatch dispatch = nullptr;
};

// 直驱时 swapchain 参数不能给普通 DXGI swapchain（FFX 只认自己建的那条），
// 与 nullptr 搭配必须给 presentCallback，否则 configure 直接失败。
static ffxReturnCode_t fsrPresentCb(ffxCallbackDescFrameGenerationPresent* params, void* ctx) {
    (void)params;
    (void)ctx;
    return FFX_API_RETURN_OK;
}

static void ffxMessage(uint32_t type, const wchar_t* msg) {
    LOG("    [ffx %s] %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "错误" : "警告", msg);
}

static bool loadApi(Api& a, const wchar_t* dir) {
    std::wstring p = std::wstring(dir) + L"\\amd_fidelityfx_loader_dx12.dll";
    a.mod = LoadLibraryW(p.c_str());
    if (!a.mod) {
        LOG("[FATAL] LoadLibrary %ls 失败 %lu\n", p.c_str(), GetLastError());
        return false;
    }
    a.create = (PfnCreate)GetProcAddress(a.mod, "ffxCreateContext");
    a.destroy = (PfnDestroy)GetProcAddress(a.mod, "ffxDestroyContext");
    a.configure = (PfnConfigure)GetProcAddress(a.mod, "ffxConfigure");
    a.dispatch = (PfnDispatch)GetProcAddress(a.mod, "ffxDispatch");
    if (!a.create || !a.destroy || !a.configure || !a.dispatch) {
        LOG("[FATAL] ffx 导出不完整\n");
        return false;
    }
    LOG("[ok] 已加载 amd_fidelityfx_loader_dx12.dll\n");
    return true;
}

// ------------------------------------------------------------------ D3D12

struct List {
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;

    bool init(ID3D12Device* dev) {
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&alloc)))) {
            return false;
        }
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(),
                                          nullptr, IID_PPV_ARGS(&cmd)))) {
            return false;
        }
        return cmd->Close() == S_OK;
    }
    bool begin() {
        if (FAILED(alloc->Reset())) return false;
        return cmd->Reset(alloc.Get(), nullptr) == S_OK;
    }
    bool submit(ID3D12CommandQueue* q) {
        const HRESULT hr = cmd->Close();
        if (FAILED(hr)) {
            LOG("    [!!] 命令列表 Close 失败 hr=0x%08lx\n", (unsigned long)hr);
            return false;
        }
        ID3D12CommandList* lists[] = {cmd.Get()};
        q->ExecuteCommandLists(1, lists);
        return true;
    }
};

struct Ctx {
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGISwapChain3> swap;
    HWND hwnd = nullptr;
    HANDLE evt = nullptr;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue = 0;

    List fill, ffx, read;

    // 上传/读回缓冲要活到 gpuIdle 之后才能释放
    std::vector<ComPtr<ID3D12Resource>> pending;
};

static void gpuIdle(Ctx& c) {
    c.queue->Signal(c.fence.Get(), ++c.fenceValue);
    c.fence->SetEventOnCompletion(c.fenceValue, c.evt);
    WaitForSingleObject(c.evt, 5000);
    c.pending.clear();
}

static ComPtr<ID3D12Resource> makeTex(Ctx& c, DXGI_FORMAT fmt, D3D12_RESOURCE_STATES st) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = W;
    d.Height = H;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> r;
    if (FAILED(c.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr,
                                              IID_PPV_ARGS(&r)))) {
        return nullptr;
    }
    return r;
}

// 用上传缓冲把整张纹理填成固定值（不走 RTV/clear：少一个状态机、少一个描述符堆）
static bool fillConst(List& L, Ctx& c, ID3D12Resource* tex, DXGI_FORMAT fmt,
                      D3D12_RESOURCE_STATES before, const float rgba[4]) {
    const uint32_t rowPitch = (W * 4 + 255) & ~255u;
    const uint64_t total = (uint64_t)rowPitch * H;

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    ComPtr<ID3D12Resource> up;
    if (FAILED(c.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up)))) {
        return false;
    }
    void* p = nullptr;
    if (FAILED(up->Map(0, nullptr, &p))) return false;
    if (fmt == DXGI_FORMAT_R32_FLOAT) {
        float* f = (float*)p;
        for (uint32_t i = 0; i < W * H; ++i) f[i] = rgba[0];
    } else if (fmt == DXGI_FORMAT_R16G16_FLOAT) {
        memset(p, 0, (size_t)total);   // 只填 0（运动向量置零），0 在任何精度下都是 0
    } else {
        unsigned char* px = (unsigned char*)p;
        const unsigned char r = (unsigned char)(rgba[0] * 255.0f + 0.5f);
        const unsigned char g = (unsigned char)(rgba[1] * 255.0f + 0.5f);
        const unsigned char b = (unsigned char)(rgba[2] * 255.0f + 0.5f);
        for (uint32_t y = 0; y < H; ++y) {
            unsigned char* row = px + (size_t)y * rowPitch;
            for (uint32_t x = 0; x < W; ++x) {
                row[x * 4 + 0] = r;
                row[x * 4 + 1] = g;
                row[x * 4 + 2] = b;
                row[x * 4 + 3] = 255;
            }
        }
    }
    up->Unmap(0, nullptr);
    c.pending.push_back(up);   // gpuIdle 之后才允许释放

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    L.cmd->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = up.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Footprint.Format = fmt;
    src.PlacedFootprint.Footprint.Width = W;
    src.PlacedFootprint.Footprint.Height = H;
    src.PlacedFootprint.Footprint.Depth = 1;
    src.PlacedFootprint.Footprint.RowPitch = rowPitch;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    L.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = before;
    L.cmd->ResourceBarrier(1, &b);
    return true;
}

// 把「读回左上角 4x1」录进读回列表
static ComPtr<ID3D12Resource> queueReadback(List& L, Ctx& c, ID3D12Resource* tex,
                                            D3D12_RESOURCE_STATES before) {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 256;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> rb;
    if (FAILED(c.dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) {
        return nullptr;
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    L.cmd->ResourceBarrier(1, &b);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = tex;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = rb.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width = 4;
    dst.PlacedFootprint.Footprint.Height = 1;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = 256;
    // pSrcBox 必须给：为 null 表示「拷整张子资源」，和 4x1 的 footprint 不匹配，
    // 调试层会判非法、拷贝被丢掉 —— 表现就是读回永远 0。
    const D3D12_BOX box{0, 0, 0, 4, 1, 1};
    L.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = before;
    L.cmd->ResourceBarrier(1, &b);
    c.pending.push_back(rb);
    return rb;
}

static bool mapReadback(ID3D12Resource* rb, float out[3]) {
    if (!rb) return false;
    void* p = nullptr;
    if (FAILED(rb->Map(0, nullptr, &p))) return false;
    const unsigned char* px = (const unsigned char*)p;
    out[0] = px[0]; out[1] = px[1]; out[2] = px[2];
    rb->Unmap(0, nullptr);
    return true;
}

static bool isGrey(const float v[3]) {
    for (int i = 0; i < 3; ++i) {
        if (v[i] < 118.0f || v[i] > 136.0f) return false;
    }
    return true;
}

// ------------------------------------------------------------------ main

int main(int argc, char** argv) {
    wchar_t amdPath[MAX_PATH] = {};
    {
        // 按可执行文件定位：<native>/test/build/xxx.exe -> <native>/vendor/amd/bin
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        for (int i = 0; i < 3; ++i) {
            wchar_t* slash = wcsrchr(exe, L'\\');
            if (slash) *slash = L'\0';
        }
        _snwprintf_s(amdPath, MAX_PATH, _TRUNCATE, L"%s\\vendor\\amd\\bin", exe);
    }
    if (argc > 1) MultiByteToWideChar(CP_ACP, 0, argv[1], -1, amdPath, MAX_PATH);
    LOG("AMD 库目录: %ls\n", amdPath);

    Ctx c;
    Api api;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"fsrDirectProbe";
    RegisterClassExW(&wc);
    c.hwnd = CreateWindowExW(0, L"fsrDirectProbe", L"fsr direct", WS_POPUP,
                             -32000, -32000, W, H, nullptr, nullptr, wc.hInstance, nullptr);
    if (!c.hwnd) { LOG("[FATAL] 窗口创建失败\n"); return 2; }

    {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            LOG("[info] D3D12 调试层已开\n");
        }
    }
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&c.factory)))) {
        LOG("[FATAL] DXGI\n");
        return 2;
    }
    ComPtr<IDXGIAdapter1> ad;
    c.factory->EnumAdapters1(0, &ad);
    DXGI_ADAPTER_DESC1 desc{};
    ad->GetDesc1(&desc);
    LOG("GPU: %ls\n", desc.Description);
    if (FAILED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&c.dev)))) {
        LOG("[FATAL] D3D12 设备\n");
        return 2;
    }
    c.dev.As(&g_info);
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    c.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&c.queue));
    if (!c.fill.init(c.dev.Get()) || !c.ffx.init(c.dev.Get()) || !c.read.init(c.dev.Get())) {
        LOG("[FATAL] 命令列表创建失败\n");
        return 2;
    }
    c.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence));
    c.evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // 真 swapchain（不是 FFX 代理）。configure 要求给一个 swapchain，
    // 传 nullptr 会直接失败（探针实测返回 3）—— 这正是代理路径之外必须自备的一环。
    {
        DXGI_SWAP_CHAIN_DESC1 scd{};
        scd.Width = W;
        scd.Height = H;
        scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        scd.SampleDesc.Count = 1;
        scd.BufferCount = kMaxOut + 2;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(c.factory->CreateSwapChainForHwnd(c.queue.Get(), c.hwnd, &scd, nullptr,
                                                     nullptr, &sc1)) ||
            FAILED(sc1.As(&c.swap))) {
            LOG("[FATAL] 真 swapchain 创建失败\n");
            return 2;
        }
        LOG("[ok] 真 swapchain（BufferCount=%u，非代理）\n", scd.BufferCount);
    }

    if (!loadApi(api, amdPath)) return 2;
    {
        std::wstring prov = std::wstring(amdPath) + L"\\amd_fidelityfx_framegeneration_dx12.dll";
        if (!LoadLibraryW(prov.c_str())) {
            LOG("[FATAL] provider 加载失败\n");
            return 2;
        }
    }

    // --- FG 效果（不建代理 swapchain）---
    ffxCreateBackendDX12Desc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = c.dev.Get();
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
    ffxContext effect = nullptr;
    if (api.create(&effect, (ffxCreateContextDescHeader*)&fgd, nullptr) != FFX_API_RETURN_OK) {
        LOG("[FATAL] FG 效果上下文创建失败\n");
        return 2;
    }
    LOG("[ok] FG 效果上下文（无代理 swapchain）\n");
    {
        // 让 runtime 把错误吐出来，否则 configure 失败只有一个数字
        ffxConfigureDescGlobalDebug1 dbg{};
        dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
        dbg.fpMessage = ffxMessage;
        dbg.debugLevel = 1;
        api.configure(&effect, (ffxConfigureDescHeader*)&dbg);
    }

    // --- 资源 ---
    ComPtr<ID3D12Resource> present = makeTex(c, DXGI_FORMAT_R8G8B8A8_UNORM,
                                             D3D12_RESOURCE_STATE_COMMON);
    ComPtr<ID3D12Resource> hudless = makeTex(c, DXGI_FORMAT_R8G8B8A8_UNORM,
                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ComPtr<ID3D12Resource> depth = makeTex(c, DXGI_FORMAT_R32_FLOAT,
                                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ComPtr<ID3D12Resource> mvec = makeTex(c, DXGI_FORMAT_R16G16_FLOAT,
                                          D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ComPtr<ID3D12Resource> outs[kMaxOut];
    for (uint32_t i = 0; i < kMaxOut; ++i) {
        outs[i] = makeTex(c, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_STATE_COMMON);
    }
    if (!present || !hudless || !depth || !mvec) { LOG("[FATAL] 资源创建失败\n"); return 2; }
    for (uint32_t i = 0; i < kMaxOut; ++i) {
        if (!outs[i]) { LOG("[FATAL] 输出纹理创建失败\n"); return 2; }
    }

    // --- 自检：先证明「填充 + 读回」这条通路是通的，否则后面所有读数都没有意义 ---
    {
        c.fill.begin();
        const float dv[4] = {0.5f, 0.0f, 0.0f, 0.0f};
        const float mv0[4] = {0, 0, 0, 0};
        const float q[4] = {0.25f, 0.25f, 0.25f, 1.0f};
        fillConst(c.fill, c, depth.Get(), DXGI_FORMAT_R32_FLOAT,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, dv);
        fillConst(c.fill, c, mvec.Get(), DXGI_FORMAT_R16G16_FLOAT,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, mv0);
        fillConst(c.fill, c, present.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                  D3D12_RESOURCE_STATE_COMMON, q);
        fillConst(c.fill, c, hudless.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, q);
        c.fill.submit(c.queue.Get());

        c.read.begin();
        ComPtr<ID3D12Resource> selfRb = queueReadback(c.read, c, present.Get(),
                                                      D3D12_RESOURCE_STATE_COMMON);
        c.read.submit(c.queue.Get());
        gpuIdle(c);

        float self[3] = {0, 0, 0};
        const bool ok = mapReadback(selfRb.Get(), self);
        drainD3d("自检");
        LOG("  [自检] present 填 0.25 后读回 RGB=(%.0f,%.0f,%.0f)，期望≈64 -> %s\n",
            self[0], self[1], self[2], (ok && self[0] > 40 && self[0] < 90) ? "通" : "不通");
        if (!ok || self[0] < 40 || self[0] > 90) {
            LOG("[FATAL] 填充/读回通路本身不通，后续读数不可信\n");
            return 2;
        }
    }

    // --- 逐档位试 numGeneratedFrames ---
    struct Step {
        uint32_t n = 0;
        ffxReturnCode_t configureRc = FFX_API_RETURN_ERROR;
        ffxReturnCode_t prepareRc = FFX_API_RETURN_ERROR;
        ffxReturnCode_t generateRc = FFX_API_RETURN_ERROR;
        float first[3] = {0, 0, 0};
        float second[3] = {0, 0, 0};
        float control[3] = {0, 0, 0};
        bool gotFirst = false;
        bool gotSecond = false;
    };
    Step steps[kMaxOut + 1];

    for (uint32_t n = 0; n <= kMaxOut; ++n) {
        Step& st = steps[n];
        st.n = n;
        const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
        const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};

        ComPtr<ID3D12Resource> rbFirst, rbSecond, rbControl;

        for (int f = 0; f < 8; ++f) {
            const float* col = (f & 1) ? blue : red;
            const uint64_t id = (uint64_t)f + 1 + n * 100;
            const bool last = (f == 7);

            // 1) 填充：真实帧红蓝交替，所有输出先归中灰
            c.fill.begin();
            fillConst(c.fill, c, present.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                      D3D12_RESOURCE_STATE_COMMON, col);
            fillConst(c.fill, c, hudless.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                      D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, col);
            for (uint32_t i = 0; i < kMaxOut; ++i) {
                fillConst(c.fill, c, outs[i].Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                          D3D12_RESOURCE_STATE_COMMON, kGrey);
            }
            c.fill.submit(c.queue.Get());

            // 2) FFX：configure -> prepare -> generate，全在自己的命令列表里
            c.ffx.begin();
            ffxConfigureDescFrameGeneration cfg{};
            cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
            cfg.swapChain = nullptr;              // 直驱：不给 swapchain（普通 DXGI 那条 FFX 不认）
            cfg.presentCallback = fsrPresentCb;   // 去掉它 configure 直接失败
            cfg.frameGenerationEnabled = true;
            cfg.HUDLessColor = ffxApiGetResourceDX12(hudless.Get(),
                                                     FFX_API_RESOURCE_STATE_COMPUTE_READ);
            cfg.frameID = id;

            ffxDispatchDescFrameGenerationPrepareV2 prep{};
            prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
            prep.frameID = id;
            prep.commandList = c.ffx.cmd.Get();
            prep.renderSize = {W, H};
            prep.motionVectorScale = {1.0f, -1.0f};
            prep.frameTimeDelta = 16.0f;
            prep.reset = (f == 0);
            prep.cameraNear = 0.1f;
            prep.cameraFar = 1000.0f;
            prep.cameraFovAngleVertical = 1.0f;
            prep.depth = ffxApiGetResourceDX12(depth.Get(), FFX_API_RESOURCE_STATE_COMPUTE_READ);
            prep.motionVectors = ffxApiGetResourceDX12(mvec.Get(),
                                                       FFX_API_RESOURCE_STATE_COMPUTE_READ);

            ffxDispatchDescFrameGeneration gen{};
            gen.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
            gen.commandList = c.ffx.cmd.Get();
            gen.presentColor = ffxApiGetResourceDX12(present.Get(),
                                                     FFX_API_RESOURCE_STATE_COMPUTE_READ);
            for (uint32_t i = 0; i < kMaxOut; ++i) {
                gen.outputs[i] = ffxApiGetResourceDX12(outs[i].Get(),
                                                       FFX_API_RESOURCE_STATE_COMMON);
            }
            gen.numGeneratedFrames = n;
            gen.reset = (f == 0);
            gen.frameID = id;

            const ffxReturnCode_t crc = api.configure(&effect, (ffxConfigureDescHeader*)&cfg);
            const ffxReturnCode_t prc = api.dispatch(&effect, (ffxDispatchDescHeader*)&prep.header);
            const ffxReturnCode_t grc = api.dispatch(&effect, (ffxDispatchDescHeader*)&gen.header);
            const bool ffxOk = c.ffx.submit(c.queue.Get());

            // 3) 读回：另一条列表，在 FFX 之后执行，不共享任何状态
            if (last && ffxOk) {
                c.read.begin();
                rbControl = queueReadback(c.read, c, outs[kMaxOut - 1].Get(),
                                          D3D12_RESOURCE_STATE_COMMON);
                if (n >= 1) {
                    rbFirst = queueReadback(c.read, c, outs[0].Get(),
                                            D3D12_RESOURCE_STATE_COMMON);
                }
                if (n >= 2) {
                    rbSecond = queueReadback(c.read, c, outs[1].Get(),
                                             D3D12_RESOURCE_STATE_COMMON);
                }
                c.read.submit(c.queue.Get());
            }
            gpuIdle(c);

            if (last) {
                st.configureRc = crc;
                st.prepareRc = prc;
                st.generateRc = grc;
                LOG("  N=%u 末轮：configure=%d prepare=%d generate=%d\n",
                    n, (int)crc, (int)prc, (int)grc);
                drainD3d("末轮");
                mapReadback(rbControl.Get(), st.control);
                if (rbFirst) mapReadback(rbFirst.Get(), st.first);
                if (rbSecond) mapReadback(rbSecond.Get(), st.second);
            }
        }

        st.gotFirst = !isGrey(st.first);
        st.gotSecond = !isGrey(st.second);
        LOG("  N=%u 对照（从不交给 effect）=RGB(%.0f,%.0f,%.0f) %s\n", n, st.control[0],
            st.control[1], st.control[2], isGrey(st.control) ? "仍是中灰 ✓" : "被改动了（异常）");
        if (n >= 1) {
            LOG("  N=%u outputs[0]=RGB(%.0f,%.0f,%.0f) %s\n", n, st.first[0], st.first[1],
                st.first[2], st.gotFirst ? "被写入 ✓" : "仍是中灰");
        }
        if (n >= 2) {
            const float d = fabsf(st.first[0] - st.second[0]) + fabsf(st.first[1] - st.second[1]) +
                            fabsf(st.first[2] - st.second[2]);
            LOG("  N=%u outputs[1]=RGB(%.0f,%.0f,%.0f) %s（与 [0] 差 %.1f）\n", n, st.second[0],
                st.second[1], st.second[2], st.gotSecond ? "被写入 ✓" : "仍是中灰", d);
        }
    }

    LOG("\n=========================================\n");
    LOG("  numGeneratedFrames -> configure/prepare/generate 返回码 / 输出是否被写\n");
    for (uint32_t n = 0; n <= kMaxOut; ++n) {
        const Step& st = steps[n];
        LOG("    N=%u  %d/%d/%d  outputs[0]=%s outputs[1]=%s\n", n, (int)st.configureRc,
            (int)st.prepareRc, (int)st.generateRc, n == 0 ? "-" : (st.gotFirst ? "写入" : "未写"),
            n < 2 ? "-" : (st.gotSecond ? "写入" : "未写"));
    }
    // 判定必须先过对照：对照纹理从不交给 effect，它被动过就说明「填充/读回」通路不可信，
    // 这时候所有 outputs 读数（哪怕是 0=「未写入」）都不能当作 effect 的结论。
    const bool controlHeld = isGrey(steps[2].control);
    if (!controlHeld) {
        LOG("  判定：本次测量无效 —— 对照组（从不交给 effect 的纹理）被动过，\n");
        LOG("        说明填充/读回通路本身不可信，不能据此对 numGeneratedFrames 下任何结论。\n");
        LOG("        已知线索：本探针里 ffxConfigure 每次都返回 %d（非 0 = 失败）。\n",
            (int)steps[2].configureRc);
        LOG("=========================================\n");
        ExitProcess(4);
    }
    const bool multiOk = steps[2].generateRc == FFX_API_RETURN_OK && steps[2].gotFirst;
    LOG("  判定：%s\n", multiOk
            ? "effect 接受 numGeneratedFrames>1 并写出了输出 —— 直驱路径可行"
            : "effect 没有按 >1 写出输出 —— 直驱路径不可行，FSR 只能停在 2x");
    LOG("  （先看自检与对照组：它们不通的话，上面的「未写」不能算数）\n");
    LOG("=========================================\n");

    if (effect) api.destroy(&effect, nullptr);
    CloseHandle(c.evt);
    DestroyWindow(c.hwnd);
    // 不 FreeLibrary：卸载厂商 DLL 会在库自己的线程/静态状态上踩空，交给进程退出回收。
    ExitProcess(multiOk ? 0 : 3);
    return 0;
}
