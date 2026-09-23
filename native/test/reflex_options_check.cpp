#include <sl.h>
#include <sl_core_api.h>
#include <sl_reflex.h>
#include <cassert>

struct TestToken : sl::FrameToken {
    operator uint32_t() const override { return 0; }
};

static TestToken testToken;
static sl::ReflexMode mode = sl::ReflexMode::eOff;
static sl::Result optionsResult = sl::Result::eOk;
static int optionCalls = 0;
static int sleeps = 0;
static int markers = 0;
static uint32_t frameLimitUs = 0;
static sl::ReflexState reflexState;

static sl::Result testGetState(sl::ReflexState& state) {
    state = reflexState;
    return sl::Result::eOk;
}

static sl::Result testSetOptions(const sl::ReflexOptions& options) {
    ++optionCalls;
    mode = options.mode;
    frameLimitUs = options.frameLimitUs;
    return optionsResult;
}

static sl::Result testGetToken(sl::FrameToken*& token, const uint32_t*) {
    token = &testToken;
    return sl::Result::eOk;
}

static sl::Result testSleep(const sl::FrameToken&) {
    ++sleeps;
    return sl::Result::eOk;
}

static sl::Result testMarker(sl::PCLMarker marker, const sl::FrameToken&) {
    assert(marker == sl::PCLMarker::eSimulationStart);
    ++markers;
    return sl::Result::eOk;
}

#define slReflexSetOptions testSetOptions
#define slReflexGetState testGetState
#define slGetNewFrameToken testGetToken
#define slReflexSleep testSleep
#define slPCLSetMarker testMarker
#include "../src/dlssmc_fg.cpp"
#undef slReflexSetOptions
#undef slReflexGetState
#undef slGetNewFrameToken
#undef slReflexSleep
#undef slPCLSetMarker

int main() {
    auto begin = Java_com_taolesi_dlssmc_nativebridge_DLSSFGNative_nativeBeginFrame;
    assert(g_ctx.reflexBoost && g_ctx.reflexFpsLimit == 0);
    assert(begin(nullptr, nullptr, JNI_FALSE, 0) == 1);
    assert(optionCalls == 0);

    assert(setReflexOptions(g_ctx.reflexBoost, g_ctx.reflexFpsLimit) == sl::Result::eOk);
    assert(mode == sl::ReflexMode::eLowLatencyWithBoost && frameLimitUs == 0);
    g_ctx.ready = true;
    g_ctx.width = g_ctx.height = 16;
    for (auto t : {&g_ctx.finalTex, &g_ctx.hudlessTex, &g_ctx.depthTex, &g_ctx.motionTex}) {
        t->registered = reinterpret_cast<HANDLE>(1);
        t->image = reinterpret_cast<VkImage>(1);
        t->memory = reinterpret_cast<VkDeviceMemory>(1);
        t->view = reinterpret_cast<VkImageView>(1);
        t->width = t->height = 16;
    }
    g_ctx.swapchain = reinterpret_cast<VkSwapchainKHR>(1);
    assert(begin(nullptr, nullptr, JNI_TRUE, 0) == 0);
    assert(optionCalls == 1);
    assert(begin(nullptr, nullptr, JNI_FALSE, 60) == 0);
    assert(!g_ctx.reflexBoost && mode == sl::ReflexMode::eLowLatency);
    assert(g_ctx.reflexFpsLimit == 60 && frameLimitUs == 16666);
    assert(optionCalls == 2);
    assert(begin(nullptr, nullptr, JNI_FALSE, 60) == 0);
    assert(optionCalls == 2);
    assert(sleeps == 3 && markers == 3);

    assert(setReflexOptions(g_ctx.reflexBoost, g_ctx.reflexFpsLimit) == sl::Result::eOk);
    assert(mode == sl::ReflexMode::eLowLatency && frameLimitUs == 16666);
    optionsResult = sl::Result::eErrorInvalidParameter;
    assert(begin(nullptr, nullptr, JNI_TRUE, 120) == -3);
    assert(!g_ctx.reflexBoost && g_ctx.reflexFpsLimit == 60 && !g_ctx.token);
    assert(sleeps == 3 && markers == 3);

    optionsResult = sl::Result::eOk;
    assert(begin(nullptr, nullptr, JNI_TRUE, 120) == 0);
    assert(g_ctx.reflexBoost && mode == sl::ReflexMode::eLowLatencyWithBoost);
    assert(g_ctx.reflexFpsLimit == 120 && frameLimitUs == 8333);
    assert(sleeps == 4 && markers == 4);
    assert(begin(nullptr, nullptr, JNI_TRUE, 1000) == 0);
    assert(frameLimitUs == 1000);
    int calls = optionCalls;
    assert(begin(nullptr, nullptr, JNI_TRUE, -1) == -3);
    assert(begin(nullptr, nullptr, JNI_TRUE, 1001) == -3);
    assert(optionCalls == calls && g_ctx.reflexFpsLimit == 1000);
    assert(begin(nullptr, nullptr, JNI_TRUE, 0) == 0);
    assert(frameLimitUs == 0 && g_ctx.reflexFpsLimit == 0);
    assert(begin(nullptr, nullptr, JNI_FALSE, 0) == 0);
    assert(frameLimitUs == 0 && mode == sl::ReflexMode::eLowLatency);
    g_ctx.swapchain = VK_NULL_HANDLE;
    puts("PASS: Boost/FPS cap toggles, bounds, unchanged frames, reapply, failed update, Reflex markers preserved (mock SDK, no GPU)");

    reflexState.latencyReportAvailable = true;
    auto& report = reflexState.frameReport[0];
    report.frameID = 8;
    report.simStartTime = 1000000;
    report.simEndTime = 1003000;
    report.renderSubmitStartTime = 1003000;
    report.renderSubmitEndTime = 1019000;
    report.presentStartTime = 1019000;
    report.presentEndTime = 1023000;
    report.osRenderQueueStartTime = 1005000;
    report.osRenderQueueEndTime = 1007000;
    report.gpuRenderStartTime = 1007000;
    report.gpuRenderEndTime = 1020000;
    reflexState.frameReport[1].frameID = 9;
    readReflexLatency();
    assert(g_ctx.latValid && "Vulkan reports remain usable without deprecated inputSampleTime");
    assert(g_ctx.latTotal == 23.0 && "Vulkan Reflex timestamps are microseconds, not QPC ticks");
    assert(g_ctx.latSim == 3.0 && g_ctx.latSubmit == 16.0 && g_ctx.latQueue == 2.0);
    assert(g_ctx.latGpu == 13.0 && g_ctx.latPresent == 4.0);
    report.inputSampleTime = 999000;
    readReflexLatency();
    assert(g_ctx.latTotal == 23.0 && "The displayed metric starts at simulation, not an optional input timestamp");
    report.presentEndTime = 0;
    readReflexLatency();
    assert(!g_ctx.latValid);
    reflexState.latencyReportAvailable = false;
    readReflexLatency();
    assert(!g_ctx.latValid);
    puts("PASS: Vulkan Reflex microseconds, missing input sample, incomplete newest frame and unavailable reports");
}
