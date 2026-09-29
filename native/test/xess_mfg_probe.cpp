// XeSS-FG / XeLL 在本机的能力探针（接后端前必须先量出事实）
//
// 要回答的问题，按 Intel 官方 3.0.2 头的顺序：
//   1) xellD3D12CreateContext 在这块 RTX 4070 Laptop 上能不能建成（XeLL 是强制依赖）
//   2) xefgSwapChainD3D12GetProperties 报的 maxSupportedInterpolations 是多少（非 Intel 官方说最多 1）
//   3) 建链后逐个试 xefgSwapChainSetNumInterpolatedFrames(1..5)：库到底接不接受 >1，
//      接受了会不会真的多上屏（判据：每帧读回 xefgSwapChainGetLastPresentStatus().framesPresented）
//
// 这个探针是「解锁 XeSS 多帧」的判决器：我们的 DLL 把请求夹到 maxSupportedInterpolations，
// 所以这里绕开那层夹取，直接问库本人接不接受更大的值。
//
// 依赖：native/vendor/intel/{bin,inc}（MANIFEST.sha256 有哈希）。运行时按绝对路径 LoadLibrary，
// 不链接厂商 import lib——否则加载器会去 PATH 找 DLL 而不是我们解出来的目录，报「找不到模块」。
// 导出名写错时 sym() 会带着函数名一起失败。
// 窗口用 WS_POPUP 放在屏幕外、不 ShowWindow：不抢前台、不显示给用户。
//
// 编译：native/test/build_xess_probe.sh   运行：native/test/build/xess_probe.exe

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <inc/xess_fg/xefg_swapchain.h>
#include <inc/xess_fg/xefg_swapchain_d3d12.h>
#include <inc/xell/xell.h>
#include <inc/xell/xell_d3d12.h>

using Microsoft::WRL::ComPtr;

static const uint32_t W = 1280, H = 720;

#define LOG(...)         \
    do {                 \
        printf(__VA_ARGS__); \
        fflush(stdout);  \
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
static void checkXe(const char* what, xefg_swapchain_result_t r) {
    LOG("  [%s] %s -> %d\n", r == XEFG_SWAPCHAIN_RESULT_SUCCESS ? "ok" : "!!", what, (int)r);
}

static HMODULE g_fg = nullptr, g_ll = nullptr;
static decltype(&xellD3D12CreateContext) pXellCreate;
static decltype(&xellSetSleepMode) pXellSleepMode;
static decltype(&xellSleep) pXellSleep;
static decltype(&xellAddMarkerData) pXellMarker;
static decltype(&xellGetVersion) pXellVersion;
static decltype(&xefgSwapChainD3D12CreateContext) pCreate;
static decltype(&xefgSwapChainSetLatencyReduction) pSetLl;
static decltype(&xefgSwapChainD3D12GetProperties) pProps;
static decltype(&xefgSwapChainD3D12InitFromSwapChainDesc) pInit;
static decltype(&xefgSwapChainD3D12GetSwapChainPtr) pGetSc;
static decltype(&xefgSwapChainD3D12TagFrameResource) pTagRes;
static decltype(&xefgSwapChainTagFrameConstants) pTagConst;
static decltype(&xefgSwapChainSetEnabled) pEnable;
static decltype(&xefgSwapChainSetNumInterpolatedFrames) pNumFrames;
static decltype(&xefgSwapChainSetPresentId) pPresentId;
static decltype(&xefgSwapChainGetLastPresentStatus) pStatus;
static decltype(&xefgSwapChainGetProperties) pGetProps;
static decltype(&xefgSwapChainSetLoggingCallback) pLog;
static decltype(&xefgSwapChainDestroy) pDestroy;
static decltype(&xellDestroyContext) pXellDestroy;

static void xefgLog(const char* msg, xefg_swapchain_logging_level_t lvl, void*) {
    // 只放 WARNING 及以上，否则 DEBUG 一行帧就把输出淹了
    if (msg && lvl >= XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING) LOG("    [xefg L%d] %s\n", (int)lvl, msg);
}

static void* sym(const char* n) {
    void* p = (void*)GetProcAddress(g_fg, n);
    if (!p) p = (void*)GetProcAddress(g_ll, n);
    if (!p) fail(n);
    return p;
}

static void loadVendors() {
    char base[MAX_PATH];
    GetModuleFileNameA(nullptr, base, sizeof(base));
    std::string dir = base;
    dir = dir.substr(0, dir.find_last_of('\\')) + "\\..\\..\\vendor\\intel\\bin\\";
    if (!GetModuleHandleA("libxell.dll")) g_ll = LoadLibraryA((dir + "libxell.dll").c_str());
    else g_ll = GetModuleHandleA("libxell.dll");
    if (!g_ll) g_ll = LoadLibraryA("libxell.dll");   // 工作目录兜底
    if (!g_ll) fail("加载 libxell.dll 失败");
    if (!GetModuleHandleA("libxess_fg.dll")) g_fg = LoadLibraryA((dir + "libxess_fg.dll").c_str());
    else g_fg = GetModuleHandleA("libxess_fg.dll");
    if (!g_fg) g_fg = LoadLibraryA("libxess_fg.dll");
    if (!g_fg) fail("加载 libxess_fg.dll 失败");
    LOG("  [ok] 已加载 %s 与 %s\n", (dir + "libxell.dll").c_str(), (dir + "libxess_fg.dll").c_str());

    pXellCreate = (decltype(pXellCreate))sym("xellD3D12CreateContext");
    pXellSleepMode = (decltype(pXellSleepMode))sym("xellSetSleepMode");
    pXellSleep = (decltype(pXellSleep))sym("xellSleep");
    pXellMarker = (decltype(pXellMarker))sym("xellAddMarkerData");
    pXellVersion = (decltype(pXellVersion))sym("xellGetVersion");
    pCreate = (decltype(pCreate))sym("xefgSwapChainD3D12CreateContext");
    pSetLl = (decltype(pSetLl))sym("xefgSwapChainSetLatencyReduction");
    pProps = (decltype(pProps))sym("xefgSwapChainD3D12GetProperties");
    pInit = (decltype(pInit))sym("xefgSwapChainD3D12InitFromSwapChainDesc");
    pGetSc = (decltype(pGetSc))sym("xefgSwapChainD3D12GetSwapChainPtr");
    pTagRes = (decltype(pTagRes))sym("xefgSwapChainD3D12TagFrameResource");
    pTagConst = (decltype(pTagConst))sym("xefgSwapChainTagFrameConstants");
    pEnable = (decltype(pEnable))sym("xefgSwapChainSetEnabled");
    pNumFrames = (decltype(pNumFrames))sym("xefgSwapChainSetNumInterpolatedFrames");
    pPresentId = (decltype(pPresentId))sym("xefgSwapChainSetPresentId");
    pStatus = (decltype(pStatus))sym("xefgSwapChainGetLastPresentStatus");
    pGetProps = (decltype(pGetProps))sym("xefgSwapChainGetProperties");
    pLog = (decltype(pLog))sym("xefgSwapChainSetLoggingCallback");
    pDestroy = (decltype(pDestroy))sym("xefgSwapChainDestroy");
    pXellDestroy = (decltype(pXellDestroy))sym("xellDestroyContext");
}

static ID3D12Resource* makeTex(ID3D12Device* dev, DXGI_FORMAT fmt, D3D12_RESOURCE_STATES state,
                               D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    rd.Width = W;
    rd.Height = H;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = fmt;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* out = nullptr;
    checkHr(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                         IID_PPV_ARGS(&out)), "CreateCommittedResource");
    return out;
}

static unsigned short toHalf(float f) {
    unsigned u = *(unsigned*)&f;
    unsigned s = (u >> 16) & 0x8000u;
    int e = (int)((u >> 23) & 0xFFu) - 127 + 15;
    unsigned m = u & 0x7FFFFFu;
    if (e <= 0) return (unsigned short)(s | (m ? 1u : 0u));
    if (e >= 31) return (unsigned short)(s | 0x7C00u);
    return (unsigned short)(s | ((unsigned)e << 10) | (m >> 13));
}

int main(int argc, char** argv) {
    LOG("=== XeSS-FG / XeLL 能力探针（%ux%u，窗口不显示）===\n\n", W, H);
    loadVendors();

    xell_version_t xv{};
    if (pXellVersion(&xv) == XELL_RESULT_SUCCESS)
        LOG("  [info] XeLL 版本 %u.%u.%u\n", xv.major, xv.minor, xv.patch);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    const wchar_t* cls = L"XessProbeCls";
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = cls;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, cls, L"XeSS probe", WS_POPUP, -32000, -32000,
                                (int)W, (int)H, nullptr, nullptr, inst, nullptr);
    if (!hwnd) fail("创建窗口失败");

    ComPtr<ID3D12Device> dev;
    checkHr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)),
            "D3D12CreateDevice");
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    checkHr(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)), "命令队列");
    ComPtr<IDXGIFactory2> factory;
    checkHr(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");

    // ---- 1) XeLL ----
    LOG("\n  --- 1) XeLL 上下文（XeSS-FG 的强制依赖）---\n");
    xell_context_handle_t ll = nullptr;
    xell_result_t lr = pXellCreate(dev.Get(), &ll);
    LOG("  [%s] xellD3D12CreateContext -> %d%s\n", lr == XELL_RESULT_SUCCESS ? "ok" : "!!", (int)lr,
        lr == XELL_RESULT_ERROR_UNSUPPORTED_DEVICE ? "（不支持的设备/显示未直连独显）" : "");
    if (lr != XELL_RESULT_SUCCESS) {
        LOG("\n=========================================\n");
        LOG("  XeLL 都建不起来，这台机器上 XeSS-FG 无法接入\n");
        LOG("=========================================\n");
        return 2;
    }

    // ---- 2) FG 上下文 + 能力 ----
    LOG("\n  --- 2) XeSS-FG 上下文与能力 ---\n");
    xefg_swapchain_handle_t fg = nullptr;
    checkXe("xefgSwapChainD3D12CreateContext", pCreate(dev.Get(), &fg));
    pLog(fg, XEFG_SWAPCHAIN_LOGGING_LEVEL_DEBUG, xefgLog, nullptr);
    checkXe("xefgSwapChainSetLatencyReduction", pSetLl(fg, ll));

    // ---- 2b) 能力门 patch（--patch）----
    // libxess_fg.dll 的建链校验里有这么一段：
    //     cmp byte ptr [rsp+0x48], 0   ; 来自全局对象的 word [obj+0x48]
    //     je  only_1_allowed           ; 为 0 -> params->maxInterpolatedFrames 必须是 1
    // 非 0 时才走「1..max」那条路，而 max 由存取器 0x1A5170 给出：
    //     ebx = 3; if (obj[0x168]) ebx = max(3, obj[0x16c]); return ebx
    // 也就是说这个 DLL 内置的有效上限本来就是 3（4x），只是被那个字节挡住了。
    // 与 DLSS-G 那处架构门禁同理：只改进程内存映像，不动磁盘文件。
    if (argc > 1 && strcmp(argv[1], "--patch") == 0) {
        HMODULE mod = GetModuleHandleW(L"libxess_fg.dll");
        if (!mod) {
            LOG("  [!!] --patch：libxess_fg.dll 未加载\n");
        } else {
            auto base = reinterpret_cast<uint8_t*>(mod);
            // 0x37F0 = 全局（TLS）配置对象的取值函数
            using GetGlobalFn = void* (*)();
            auto getGlobal = reinterpret_cast<GetGlobalFn>(base + 0x37F0);
            auto* obj = reinterpret_cast<uint8_t*>(getGlobal());
            LOG("  [info] 全局对象 %p：word[+0x48]=0x%04X byte[+0x168]=%u dword[+0x16c]=%u\n",
                obj, *(uint16_t*)(obj + 0x48), *(uint8_t*)(obj + 0x168), *(uint32_t*)(obj + 0x16c));
            *(uint16_t*)(obj + 0x48) = 1;      // 放开「>1 张插帧」这条校验
            *(uint8_t*)(obj + 0x168) = 1;      // 标成「有覆盖值」
            *(uint32_t*)(obj + 0x16c) = 3;     // 覆盖值 = 3 张插帧（4x）
            LOG("  [info] patch 后：word[+0x48]=0x%04X byte[+0x168]=%u dword[+0x16c]=%u\n",
                *(uint16_t*)(obj + 0x48), *(uint8_t*)(obj + 0x168), *(uint32_t*)(obj + 0x16c));
        }
    }

    xefg_swapchain_d3d12_init_params_t params{};
    params.pApplicationSwapChain = nullptr;
    // 命令行给个整数就用它，否则用「库自己报的上限」。用来测库到底把哪个值当作
    // 「provided during initialization」——SetNumInterpolatedFrames 的报错就指着它。
    const uint32_t initMax = (argc > 1 && argv[1][0] != '-') ? (uint32_t)atoi(argv[1])
                                     : XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
    params.maxInterpolatedFrames = initMax;
    LOG("  [info] 建链时 maxInterpolatedFrames = %s\n",
        initMax == XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES
            ? "USE_MAX_SUPPORTED" : std::to_string(initMax).c_str());
    xefg_swapchain_properties_t props{};
    checkXe("xefgSwapChainD3D12GetProperties(初始化前)",
            pProps(fg, &params, W, H, DXGI_FORMAT_R8G8B8A8_UNORM, &props));
    LOG("  [info] maxSupportedInterpolations=%u 需要描述符=%u 临时纹理堆=%.1f MB 临时缓冲堆=%.2f MB\n",
        props.maxSupportedInterpolations, props.requiredDescriptorCount,
        props.tempTextureHeapSize / 1048576.0, props.tempBufferHeapSize / 1048576.0);

    // ---- 3) 代理 swapchain + 逐帧打标签 + Present ----
    LOG("\n  --- 3) 代理 swapchain 与真实呈现 ---\n");
    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = W;
    scd.Height = H;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferCount = 2;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    checkXe("xefgSwapChainD3D12InitFromSwapChainDesc",
            pInit(fg, hwnd, &scd, nullptr, queue.Get(), factory.Get(), &params));
    ComPtr<IDXGISwapChain1> sc;
    checkHr(pGetSc(fg, IID_PPV_ARGS(sc.GetAddressOf())), "GetSwapChainPtr");

    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> cmd;
    checkHr(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)), "alloc");
    checkHr(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr,
                                   IID_PPV_ARGS(&cmd)), "cmdlist");
    ComPtr<ID3D12Fence> fence;
    checkHr(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    HANDLE evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fv = 0;
    auto gpuIdle = [&]() {
        queue->Signal(fence.Get(), ++fv);
        fence->SetEventOnCompletion(fv, evt);
        if (WaitForSingleObject(evt, 20000) != WAIT_OBJECT_0) fail("GPU 等待超时");
    };

    ID3D12Resource* color = makeTex(dev.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* depth = makeTex(dev.Get(), DXGI_FORMAT_R32_FLOAT,
                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12Resource* motion = makeTex(dev.Get(), DXGI_FORMAT_R16G16_FLOAT,
                                     D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // 三张输入必须填成有意义的值：CreateCommittedResource 之后内容是未定义的，
    // 拿 NaN / 垃圾去喂 FG 会被它自己关掉
    {
        const UINT64 pitch = ((UINT64)W * 4 + 255) & ~255ULL;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = pitch * H;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES up{};
        up.Type = D3D12_HEAP_TYPE_UPLOAD;
        ComPtr<ID3D12Resource> upload;
        checkHr(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&upload)), "探针上传缓冲");
        unsigned char* mapped = nullptr;
        checkHr(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "Map 上传缓冲");
        ID3D12Resource* dsts[3] = {color, depth, motion};
        const DXGI_FORMAT fmts[3] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R32_FLOAT,
                                     DXGI_FORMAT_R16G16_FLOAT};
        const D3D12_RESOURCE_STATES finals[3] = {D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
        const unsigned short mv[2] = {toHalf(0.004f), toHalf(-0.002f)};
        for (int i = 0; i < 3; ++i) {
            for (uint32_t y = 0; y < H; ++y) {
                unsigned char* row = mapped + (size_t)y * pitch;
                for (uint32_t x = 0; x < W; ++x) {
                    if (i == 0) {
                        row[x * 4 + 0] = 102; row[x * 4 + 1] = 77;
                        row[x * 4 + 2] = 153; row[x * 4 + 3] = 255;
                    } else if (i == 1) {
                        *(float*)(row + x * 4) = 0.5f;
                    } else {
                        *(unsigned short*)(row + x * 4 + 0) = mv[0];
                        *(unsigned short*)(row + x * 4 + 2) = mv[1];
                    }
                }
            }
            alloc->Reset();
            cmd->Reset(alloc.Get(), nullptr);
            D3D12_RESOURCE_BARRIER bar{};
            bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            bar.Transition.pResource = dsts[i];
            bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            bar.Transition.StateBefore = finals[i];
            bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            cmd->ResourceBarrier(1, &bar);
            D3D12_TEXTURE_COPY_LOCATION d{};
            d.pResource = dsts[i];
            d.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION s{};
            s.pResource = upload.Get();
            s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            s.PlacedFootprint.Footprint.Format = fmts[i];
            s.PlacedFootprint.Footprint.Width = W;
            s.PlacedFootprint.Footprint.Height = H;
            s.PlacedFootprint.Footprint.Depth = 1;
            s.PlacedFootprint.Footprint.RowPitch = pitch;
            cmd->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
            bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            bar.Transition.StateAfter = finals[i];
            cmd->ResourceBarrier(1, &bar);
            cmd->Close();
            ID3D12CommandList* lists[] = {cmd.Get()};
            queue->ExecuteCommandLists(1, lists);
            gpuIdle();
        }
        upload->Unmap(0, nullptr);
        LOG("  [ok] 三张输入纹理已填初值（颜色 102/77/153，深度 0.5，MV 0.004/-0.002 像素）\n");
    }

    // 官方顺序：GPU 空闲 -> xellSetSleepMode(bLowLatencyMode) -> 才允许开 FG。
    // 反过来先 SetEnabled 的话，库会报 "Latency reduction is not enabled." 并整场不插帧。
    gpuIdle();
    xell_sleep_params_t sp{};
    sp.bLowLatencyMode = 1;
    sp.minimumIntervalUs = 0;
    xell_result_t smr = pXellSleepMode(ll, &sp);
    LOG("  [%s] xellSetSleepMode(bLowLatencyMode=1) -> %d\n",
        smr == XELL_RESULT_SUCCESS ? "ok" : "!!", (int)smr);
    pEnable(fg, 1);

    // 建链后再问一次，拿全字段（初始化前只填 maxSupportedInterpolations）
    xefg_swapchain_properties_t after{};
    checkXe("xefgSwapChainGetProperties(初始化后)", pGetProps(fg, &after));
    LOG("  [info] 初始化后属性：描述符=%u 恒定缓冲=%llu 最大插帧=%u\n",
        after.requiredDescriptorCount,
        (unsigned long long)after.constantBufferSize, after.maxSupportedInterpolations);

    xefg_swapchain_frame_constant_data_t cst{};
    for (int i = 0; i < 16; ++i) {
        cst.viewMatrix[i] = (i % 5) == 0 ? 1.0f : 0.0f;   // 单位阵
        cst.projectionMatrix[i] = (i % 5) == 0 ? 1.0f : 0.0f;
    }
    cst.motionVectorScaleX = 1.0f / W;
    cst.motionVectorScaleY = 1.0f / H;
    cst.frameRenderTime = 16.0f;

    struct Step {
        uint32_t want;
        xefg_swapchain_result_t setRc;
        uint32_t maxPresented;
        uint32_t sum;
        uint32_t frames;
    };
    Step steps[5] = {};
    uint32_t nextId = 1;

    // 每一步都换插帧数重跑：先 20 帧预热（重建历史），再统计 30 帧
    for (int s = 0; s < 5; ++s) {
        const uint32_t want = (uint32_t)(s + 1);
        steps[s].want = want;
        steps[s].setRc = pNumFrames(fg, want);
        LOG("\n== 请求插帧数 %u -> SetNumInterpolatedFrames 返回 %d（0 = 库接受）\n",
            want, (int)steps[s].setRc);

        for (uint32_t k = 0; k < 50; ++k, ++nextId) {
            pXellMarker(ll, nextId, XELL_INPUT_SAMPLE);
            pXellSleep(ll, nextId);
            pXellMarker(ll, nextId, XELL_SIMULATION_START);
            pXellMarker(ll, nextId, XELL_SIMULATION_END);
            pXellMarker(ll, nextId, XELL_RENDERSUBMIT_START);

            alloc->Reset();
            cmd->Reset(alloc.Get(), nullptr);
            xefg_swapchain_d3d12_resource_data_t res[3] = {};
            res[0].type = XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
            res[0].validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
            res[0].resourceBase = {0, 0};
            res[0].resourceSize = {W, H};
            res[0].pResource = color;
            res[0].incomingState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            res[1].type = XEFG_SWAPCHAIN_RES_DEPTH;
            res[1].validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
            res[1].resourceBase = {0, 0};
            res[1].resourceSize = {W, H};
            res[1].pResource = depth;
            res[1].incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            res[2].type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
            res[2].validity = XEFG_SWAPCHAIN_RV_ONLY_NOW;
            res[2].resourceBase = {0, 0};
            res[2].resourceSize = {W, H};
            res[2].pResource = motion;
            res[2].incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            for (auto& r : res) pTagRes(fg, cmd.Get(), nextId, &r);
            pTagConst(fg, nextId, &cst);
            cmd->Close();
            ID3D12CommandList* lists[] = {cmd.Get()};
            queue->ExecuteCommandLists(1, lists);
            pXellMarker(ll, nextId, XELL_RENDERSUBMIT_END);
            pXellMarker(ll, nextId, XELL_PRESENT_START);
            pPresentId(fg, nextId);
            HRESULT phr = sc->Present(1, 0);
            pXellMarker(ll, nextId, XELL_PRESENT_END);
            if (phr != S_OK && k <= 2) LOG("  [!!] Present -> 0x%08X\n", (unsigned)phr);

            xefg_swapchain_present_status_t st{};
            xefg_swapchain_result_t sr = pStatus(fg, &st);
            if (k >= 20) {
                ++steps[s].frames;
                steps[s].sum += st.framesPresented;
                if (st.framesPresented > steps[s].maxPresented)
                    steps[s].maxPresented = st.framesPresented;
                if (k == 20 || k == 49)
                    LOG("  [info] 帧 %u：status=%d framesPresented=%u 生成中=%u result=%d\n",
                        nextId, (int)sr, st.framesPresented, st.isFrameGenEnabled,
                        (int)st.frameGenResult);
            }
            gpuIdle();
        }
    }

    LOG("\n=========================================\n");
    LOG("  建链前报的 maxSupportedInterpolations = %u（=> 最多 %ux 输出）\n",
        props.maxSupportedInterpolations, props.maxSupportedInterpolations + 1);
    LOG("  建链后报的 maxSupportedInterpolations = %u\n", after.maxSupportedInterpolations);
    LOG("  请求倍数 / 请求插帧数 / SetNumInterpolatedFrames 返回 / 稳定期单帧最多呈现 / 平均\n");
    for (int s = 0; s < 5; ++s) {
        const Step& t = steps[s];
        LOG("    %ux  插帧=%u  返回=%d  最多呈现=%u  平均=%.2f（%u 帧）\n",
            t.want + 1, t.want, (int)t.setRc, t.maxPresented,
            t.frames ? (double)t.sum / t.frames : 0.0, t.frames);
    }
    LOG("=========================================\n");

    sc.Reset();   // 代理 swapchain 的引用必须先放掉，否则库报 CRITICAL Integration issue
    pDestroy(fg);
    pXellDestroy(ll);
    color->Release();
    depth->Release();
    motion->Release();
    CloseHandle(evt);
    DestroyWindow(hwnd);
    // 不 FreeLibrary：卸载厂商 DLL 会在库自身的线程/静态状态上踩空（探针实测退出码 139），
    // 进程本来就要结束了，交给系统回收。
    ExitProcess(0);
    return 0;
}
