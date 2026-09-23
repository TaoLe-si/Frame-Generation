// Standalone production-function regression; no game, WGL context or vendor DLLs.
// build_fsr_frame_check.sh [optional absolute production source snapshot]
#include <windows.h>
#include <d3d12sdklayers.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>

static LONGLONG testTicks = 1000000;
static BOOL WINAPI testCounter(LARGE_INTEGER* v) { v->QuadPart = testTicks; return TRUE; }
static BOOL WINAPI testFrequency(LARGE_INTEGER* v) { v->QuadPart = 1000000; return TRUE; }
#define QueryPerformanceCounter testCounter
#define QueryPerformanceFrequency testFrequency
#ifndef DLSSMC_DX_SOURCE
#define DLSSMC_DX_SOURCE "../src/dlssmc_dx.cpp"
#endif
#include DLSSMC_DX_SOURCE
#undef QueryPerformanceCounter
#undef QueryPerformanceFrequency

static int failures = 0, prepares = 0, configures = 0;
static uint64_t configured = 0;
static float frameMs = -1, upscaleMs = -1;
static bool configureError = false, prepareError = false, closeInPrepare = false;
static ffxReturnCode_t generateResult = FFX_API_RETURN_OK;
static uint32_t generatedOutput = 0;
static ComPtr<ID3D12InfoQueue> info;
static ComPtr<ID3D12Resource> markerUpload, markerReadback;
static bool expectStates = false;
static D3D12_MESSAGE_ID expectedDebugError = D3D12_MESSAGE_ID_UNKNOWN;
static bool frameReset = false;

static void expect(bool ok, const char* msg) {
    if (!ok) { ++failures; printf("FAIL: %s\n", msg); }
}
static void hr(HRESULT value, const char* msg) {
    if (FAILED(value)) { printf("FATAL: %s hr=0x%08lx\n", msg, (unsigned long)value); exit(2); }
}
static void debugMessages() {
    bool sawExpected = false;
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T bytes = 0;
        info->GetMessage(i, nullptr, &bytes);
        std::vector<unsigned char> storage(bytes);
        auto* m = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        hr(info->GetMessage(i, m, &bytes), "debug message");
        if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            if (m->ID == expectedDebugError) {
                sawExpected = true;
                printf("Expected D3D12 error %d from failure injection\n", (int)m->ID);
            } else {
                ++failures;
                printf("D3D12 ERROR %d: %s\n", (int)m->ID, m->pDescription);
            }
        }
    }
    if (expectedDebugError != D3D12_MESSAGE_ID_UNKNOWN) expect(sawExpected, "failure injection was exercised");
    info->ClearStoredMessages();
}
static void assertState(ID3D12GraphicsCommandList* cmd, ID3D12Resource* res,
                        D3D12_RESOURCE_STATES state) {
    ComPtr<ID3D12DebugCommandList> debug;
    hr(cmd->QueryInterface(IID_PPV_ARGS(&debug)), "debug command list");
    expect(debug->AssertResourceState(res, 0, state) != FALSE, "actual command-list resource state");
}
static ffxReturnCode_t configureStub(ffxContext*, const ffxConfigureDescHeader* h) {
    auto* cfg = reinterpret_cast<const ffxConfigureDescFrameGeneration*>(h);
    ++configures;
    if (configureError) return FFX_API_RETURN_ERROR;
    configured = cfg->frameID;
    expect(cfg->frameGenerationCallback == fsrGenerateCb, "production generation callback");
    expect(!cfg->allowAsyncWorkloads, "async setting unchanged");
    expect(cfg->HUDLessColor.state == FFX_API_RESOURCE_STATE_COMPUTE_READ, "hudless declared compute read");
    return FFX_API_RETURN_OK;
}
static ffxReturnCode_t dispatchStub(ffxContext*, const ffxDispatchDescHeader* h) {
    if (h->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION) {
        auto* fg = const_cast<ffxDispatchDescFrameGeneration*>(
            reinterpret_cast<const ffxDispatchDescFrameGeneration*>(h));
        fg->numGeneratedFrames = generatedOutput;
        return generateResult;
    }
    if (h->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        auto* up = reinterpret_cast<const ffxDispatchDescUpscale*>(h);
        upscaleMs = up->frameTimeDelta;
        return FFX_API_RETURN_OK;
    }
    expect(h->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2, "prepare V2");
    auto* prep = reinterpret_cast<const ffxDispatchDescFrameGenerationPrepareV2*>(h);
    ++prepares;
    expect(configured == prep->frameID, "Configure must succeed before Prepare for same frameID");
    frameMs = prep->frameTimeDelta;
    frameReset = prep->reset;
    auto* cmd = static_cast<ID3D12GraphicsCommandList*>(prep->commandList);
    // Real D3D12 recording: a closed list reports COMMAND_LIST_CLOSED, and a
    // list submitted before this call cannot copy the marker to readback.
    cmd->CopyBufferRegion(markerReadback.Get(), 0, markerUpload.Get(), 0, 4);
    if (expectStates) {
        assertState(cmd, g.in[1].d12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        assertState(cmd, g.in[2].d12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        assertState(cmd, g.in[3].d12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // AssertResourceState alone is conservative when the list has no prior
        // state record. Real transitions also validate StateBefore at Execute.
        for (int i : {1, 2, 3}) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = g.in[i].d12.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cmd->ResourceBarrier(1, &b);
            std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
            cmd->ResourceBarrier(1, &b);
        }
    }
    if (closeInPrepare) hr(cmd->Close(), "injected early Close");
    return prepareError ? FFX_API_RETURN_ERROR : FFX_API_RETURN_OK;
}
static HANDLE WINAPI registerStub(HANDLE, void*, unsigned, unsigned, unsigned) { return (HANDLE)1; }
static BOOL WINAPI objectsStub(HANDLE, int, HANDLE*) { return TRUE; }
static BOOL WINAPI unregisterStub(HANDLE, HANDLE) { return TRUE; }
static ComPtr<ID3D12Resource> buffer(D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 256; desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> out;
    hr(g.dev12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                       nullptr, IID_PPV_ARGS(&out)), "buffer");
    return out;
}
static void init(bool shared) {
    ComPtr<ID3D12Debug> debug;
    hr(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)), "D3D12 debug layer required");
    debug->EnableDebugLayer();
    hr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g.dev12)), "device");
    hr(g.dev12.As(&info), "info queue");
    D3D12_COMMAND_QUEUE_DESC q{};
    hr(g.dev12->CreateCommandQueue(&q, IID_PPV_ARGS(&g.queue)), "queue");
    for (int i = 0; i < kFramesInFlight; ++i) {
        hr(g.dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g.alloc[i])), "allocator");
        hr(g.dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.alloc[i].Get(), nullptr,
                                     IID_PPV_ARGS(&g.cmd[i])), "command list");
        hr(g.cmd[i]->Close(), "initial Close");
    }
    hr(g.dev12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.fence12)), "fence");
    g.evt12 = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g.w = g.rw = 64; g.h = g.rh = 64; g.backend = kBackendFSR;
    hr(CreateDXGIFactory2(0, IID_PPV_ARGS(&g.factory)), "factory");
    g.child = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"FSR regression",
                              WS_POPUP, -32000, -32000, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    expect(g.child && !IsWindowVisible(g.child), "test window remains hidden");
    DXGI_SWAP_CHAIN_DESC1 sc{};
    sc.Width = g.w; sc.Height = g.h; sc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sc.SampleDesc.Count = 1; sc.BufferCount = 3;
    sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swap;
    hr(g.factory->CreateSwapChainForHwnd(g.queue.Get(), g.child, &sc, nullptr, nullptr, &swap), "hidden swapchain");
    hr(swap.As(&g.swapchain), "swapchain3");
    const DXGI_FORMAT formats[] = {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM};
    if (shared) {
        hr(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                            D3D11_SDK_VERSION, &g.d3d11, nullptr, &g.ctx11), "D3D11 device");
        hr(g.d3d11.As(&g.dev11_5), "D3D11 device5");
        hr(g.ctx11.As(&g.ctx11_4), "D3D11 context4");
        hr(g.dev11_5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.fence11)), "D3D11 fence");
        g.evt11 = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        pRegisterObject = registerStub; pLockObjects = pUnlockObjects = objectsStub;
        pUnregisterObject = unregisterStub;
        expect(Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeBindTextures(
            nullptr, nullptr, 1, 2, 3, 4, 5, 6, g.rw, g.rh) == 0, "production input chain");
        for (int i = 0; i < 6; ++i)
            printf("shared input[%d] D3D12 flags=0x%x\n", i, (unsigned)g.in[i].d12->GetDesc().Flags);
    } else {
        for (int i = 0; i < 4; ++i) {
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = g.w; d.Height = g.h; d.DepthOrArraySize = d.MipLevels = 1;
            d.SampleDesc.Count = 1; d.Format = formats[i];
            hr(g.dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g.in[i].d12)), "PSR texture");
        }
        g.in1State = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    g.pFfxConfigure = configureStub; g.pFfxDispatch = dispatchStub;
    markerUpload = buffer(D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    markerReadback = buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    void* p = nullptr;
    hr(markerUpload->Map(0, nullptr, &p), "map upload");
    *static_cast<uint32_t*>(p) = 0;
    hr(g.cmd[0]->Reset(g.alloc[0].Get(), nullptr), "initialize marker");
    g.cmd[0]->CopyBufferRegion(markerReadback.Get(), 0, markerUpload.Get(), 0, 4);
    hr(g.cmd[0]->Close(), "marker Close");
    ID3D12CommandList* lists[] = {g.cmd[0].Get()};
    g.queue->ExecuteCommandLists(1, lists);
    wait12();
    *static_cast<uint32_t*>(p) = 0x1234abcd; markerUpload->Unmap(0, nullptr);
    g.ready = g.haveInputs = true;
}
static bool frame(uint32_t slot = 0) {
    float m[64]{}; for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) m[i * 16 + j * 5] = 1;
    float p[8] = {0.1f, 1000.f, 1.f, 1.f, 0, 0, 1, 1};
    bool ok = ffxFrame(m, p, g.ffxFrameId == 0, slot);
    wait12();
    expect(g.fence12->GetCompletedValue() >= g.fence12Value, "GPU completed test work");
    return ok;
}
static void verifyMarker(uint32_t expected = 0x1234abcd) {
    void* p = nullptr;
    hr(markerReadback->Map(0, nullptr, &p), "map marker");
    expect(*static_cast<uint32_t*>(p) == expected, "only successful frame executes Prepare commands");
    markerReadback->Unmap(0, nullptr);
}
static void deliver(bool sr) {
    expect(Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeLock(
        nullptr, nullptr, sr ? 2 : 0, sr ? 3 : 4) == 0, "lock input segment (WGL stub)");
    expect(Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeDeliver(nullptr, nullptr) == 0,
           "real D3D11 CopyResource + Flush/fence delivery");
}
int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "order";
    if (mode == "callback") {
        g.pFfxDispatch = dispatchStub;
        ffxDispatchDescFrameGeneration p{}; p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        for (auto count : {0u, 1u, 3u}) {
            generatedOutput = count; p.numGeneratedFrames = 99;
            const uint32_t before = g.ffxGenerated;
            expect(fsrGenerateCb(&p, &g.ffxEffect) == FFX_API_RETURN_OK, "callback returns success");
            expect(g.ffxGenerated - before == count, "count successful returned numGeneratedFrames, not callback count");
        }
        generateResult = FFX_API_RETURN_ERROR; generatedOutput = 5;
        const uint32_t before = g.ffxGenerated;
        expect(fsrGenerateCb(&p, &g.ffxEffect) == FFX_API_RETURN_ERROR, "callback preserves failure");
        expect(g.ffxGenerated == before, "failed dispatch not counted");
    } else {
        const bool shared = mode == "shared" || mode == "sr" || mode == "clock";
        init(shared);
        expectStates = mode == "states" || shared;
        if (mode == "configure-error") configureError = true;
        if (mode == "prepare-error") prepareError = true;
        if (mode == "close-error") {
            closeInPrepare = true;
            expectedDebugError = D3D12_MESSAGE_ID_COMMAND_LIST_CLOSED;
        }
        if (mode == "reset-error" || mode == "list-reset-error") {
            hr(g.cmd[0]->Reset(g.alloc[mode == "reset-error" ? 0 : 2].Get(), nullptr), "leave list open");
            expectedDebugError = mode == "reset-error" ? D3D12_MESSAGE_ID_COMMAND_ALLOCATOR_CANNOT_RESET
                                                       : D3D12_MESSAGE_ID_COMMAND_LIST_OPEN;
        }
        if (shared) deliver(mode == "sr");
        if (mode == "sr") {
            g.ffxUpscale = reinterpret_cast<ffxContext>(1); g.srQuality = 0;
            expect(createSrOutput(), "SR output");
            expect(Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeUpscale(nullptr, nullptr, nullptr, JNI_TRUE) == 0, "production SR before FG");
            expect(Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeUpscaleDone(nullptr, nullptr) == 0, "SR unlock");
        }
        const bool error = mode.find("-error") != std::string::npos;
        const auto inputStateBefore = g.in1State;
        const bool ok = frame();
        if (error) {
            expect(!ok, "failed stage aborts frame");
            expect(g.in1State == inputStateBefore, "unsubmitted barriers do not change input tracking");
            if (configureError || mode.find("reset-error") != std::string::npos)
                expect(prepares == 0 && (configureError || configures == 0), "failed setup does not Prepare");
            UINT presents = 0;
            hr(g.swapchain->GetLastPresentCount(&presents), "present count");
            expect(presents == 0, "failure does not Present");
            verifyMarker(0);
            if (configureError || prepareError) {
                configureError = prepareError = false;
                expect(frame(), "retry after provider failure can Reset and submit");
                verifyMarker();
            }
        } else {
            expect(ok, "hidden frame succeeds");
            expect(frameMs == 0 && frameReset, "first frame has no invented interval and resets history");
            verifyMarker();
            if (mode == "clock" || shared) {
                for (auto delta : {7, 37}) {
                    testTicks += delta * 1000;
                    if (shared) deliver(mode == "sr");
                    if (mode == "sr") {
                        expect(Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeUpscale(nullptr, nullptr, nullptr, JNI_FALSE) == 0, "next SR");
                        expect(std::fabs(upscaleMs - delta) < 0.01f, "SR uses real elapsed milliseconds");
                        Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeUpscaleDone(nullptr, nullptr);
                    }
                    expect(frame(1), "next hidden frame");
                    expect(std::fabs(frameMs - delta) < 0.01f, "FG uses real elapsed milliseconds");
                }
            }
        }
        debugMessages();
        markerReadback.Reset(); markerUpload.Reset(); info.Reset();
        Java_com_taolesi_dlssmc_nativebridge_DLSSDXNative_nativeShutdown(nullptr, nullptr);
    }
    printf("%s: %s (%d failures)\n", failures ? "FAIL" : "PASS", mode.c_str(), failures);
    return failures ? 1 : 0;
}
