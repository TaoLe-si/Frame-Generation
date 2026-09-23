#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#define VK_NO_PROTOTYPES
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include <sl.h>
#include <sl_dlss_g.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

static void check(VkResult result, const char* name) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(name) + ": " + std::to_string(result));
}
static void checkSl(sl::Result result, const char* name) {
    if (result != sl::Result::eOk) throw std::runtime_error(std::string(name) + ": " + std::to_string((int)result));
}
template<class T> static T require(T function, const char* name) {
    if (!function) throw std::runtime_error(std::string("Missing entry: ") + name);
    return function;
}

int main(int argc, char** argv) {
    if (argc != 4 || (strcmp(argv[1], "export") && strcmp(argv[1], "proc")) ||
        (strcmp(argv[2], "0") && strcmp(argv[2], "1")) ||
        (strcmp(argv[3], "0") && strcmp(argv[3], "1"))) {
        fprintf(stderr, "Usage: fg_probe.exe export|proc nvx:0|1 manual:0|1\n");
        return 2;
    }
    const bool proc = !strcmp(argv[1], "proc"), nvx = !strcmp(argv[2], "1"), manual = !strcmp(argv[3], "1");
    setvbuf(stdout, nullptr, _IONBF, 0);
    const wchar_t* pluginPath = L"D:\\Backup\\Downloads\\streamline-sdk-v2.14.1\\bin\\x64";
    const std::wstring dllPath = std::wstring(pluginPath) + L"\\sl.interposer.dll";
    HMODULE slModule = LoadLibraryExW(dllPath.c_str(), nullptr,
                                     LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!slModule) { fprintf(stderr, "LoadLibraryEx: %lu\n", GetLastError()); return 1; }
    HMODULE vkModule = nullptr;
    VkInstance inst = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    bool initialized = false;
    PFN_vkDestroyInstance destroyInstance = nullptr;
    PFN_vkDestroyDevice destroyDevice = nullptr;
    auto shutdown = reinterpret_cast<PFun_slShutdown*>(GetProcAddress(slModule, "slShutdown"));
    int exitCode = 0;
    try {
#define SL_LOAD(name) auto p##name = require(reinterpret_cast<PFun_##name*>(GetProcAddress(slModule, #name)), #name)
        SL_LOAD(slInit);
        SL_LOAD(slIsFeatureSupported);
        SL_LOAD(slGetFeatureRequirements);
        require(shutdown, "slShutdown");
        auto gipa = require(reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(slModule, "vkGetInstanceProcAddr")), "vkGetInstanceProcAddr");
        auto gdpa = require(reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(slModule, "vkGetDeviceProcAddr")), "vkGetDeviceProcAddr");
        sl::Preferences pref{};
        pref.renderAPI = sl::RenderAPI::eVulkan;
        pref.applicationId = 231313132u;
        pref.engine = sl::EngineType::eCustom;
        pref.engineVersion = "0.1.0";
        pref.showConsole = false;
        pref.logLevel = sl::LogLevel::eOff;
        pref.pathsToPlugins = &pluginPath;
        pref.numPathsToPlugins = 1;
        pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        if (manual) pref.flags |= sl::PreferenceFlags::eUseManualHooking;
        sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
        pref.featuresToLoad = features;
        pref.numFeaturesToLoad = 3;
        fprintf(stderr, "phase=slInit\n");
        checkSl(pslInit(pref, sl::kSDKVersion), "slInit");
        initialized = true;
        fprintf(stderr, "phase=instance\n");

#define INSTANCE_FN(name) auto name = require(reinterpret_cast<PFN_##name>(proc ? gipa(inst, #name) : reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(slModule, #name))), #name)
        INSTANCE_FN(vkCreateInstance);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "dlssmc-headless-probe";
        app.apiVersion = VK_API_VERSION_1_2;
        const char* instanceExts[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME};
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app;
        ci.enabledExtensionCount = 5;
        ci.ppEnabledExtensionNames = instanceExts;
        check(vkCreateInstance(&ci, nullptr, &inst), "vkCreateInstance");
        destroyInstance = require(reinterpret_cast<PFN_vkDestroyInstance>(gipa(inst, "vkDestroyInstance")), "vkDestroyInstance");
        INSTANCE_FN(vkEnumeratePhysicalDevices);
        INSTANCE_FN(vkGetPhysicalDeviceProperties);
        INSTANCE_FN(vkGetPhysicalDeviceProperties2);
        INSTANCE_FN(vkGetPhysicalDeviceQueueFamilyProperties);
        INSTANCE_FN(vkEnumerateDeviceExtensionProperties);
        INSTANCE_FN(vkCreateDevice);
        uint32_t count = 0;
        check(vkEnumeratePhysicalDevices(inst, &count, nullptr), "vkEnumeratePhysicalDevices");
        std::vector<VkPhysicalDevice> gpus(count);
        check(vkEnumeratePhysicalDevices(inst, &count, gpus.data()), "vkEnumeratePhysicalDevices");
        VkPhysicalDevice phys = VK_NULL_HANDLE;
        VkPhysicalDeviceProperties properties{};
        for (auto gpu : gpus) {
            vkGetPhysicalDeviceProperties(gpu, &properties);
            if (properties.vendorID == 0x10de) { phys = gpu; break; }
        }
        if (!phys) throw std::runtime_error("No NVIDIA GPU");
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties2.pNext = &id;
        vkGetPhysicalDeviceProperties2(phys, &properties2);
        sl::AdapterInfo adapter{};
        adapter.deviceLUID = id.deviceLUID;
        adapter.deviceLUIDSizeInBytes = VK_LUID_SIZE;
        checkSl(pslIsFeatureSupported(sl::kFeatureDLSS_G, adapter), "slIsFeatureSupported");
        sl::FeatureRequirements req{};
        checkSl(pslGetFeatureRequirements(sl::kFeatureDLSS_G, req), "slGetFeatureRequirements");
        bool declaresBinary = false, declaresImage = false;
        for (uint32_t i = 0; i < req.vkNumDeviceExtensions; ++i) {
            declaresBinary |= !strcmp(req.vkDeviceExtensions[i], VK_NVX_BINARY_IMPORT_EXTENSION_NAME);
            declaresImage |= !strcmp(req.vkDeviceExtensions[i], VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME);
        }
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &count, families.data());
        uint32_t family = 0;
        while (family < count && !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) ++family;
        if (family == count) throw std::runtime_error("No graphics queue");
        check(vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, nullptr), "vkEnumerateDeviceExtensionProperties");
        std::vector<VkExtensionProperties> available(count);
        check(vkEnumerateDeviceExtensionProperties(phys, nullptr, &count, available.data()), "vkEnumerateDeviceExtensionProperties");
        std::vector<const char*> extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
            VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
            VK_KHR_MAINTENANCE_4_EXTENSION_NAME, VK_NV_OPTICAL_FLOW_EXTENSION_NAME};
        if (nvx) {
            extensions.push_back(VK_NVX_BINARY_IMPORT_EXTENSION_NAME);
            extensions.push_back(VK_NVX_IMAGE_VIEW_HANDLE_EXTENSION_NAME);
        }
        for (auto ext : extensions) {
            bool found = false;
            for (const auto& candidate : available) found |= !strcmp(candidate.extensionName, ext);
            if (!found) throw std::runtime_error(std::string("Unsupported extension: ") + ext);
        }
        float priorities[] = {1, 1, 1};
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = std::min(3u, families[family].queueCount);
        queueInfo.pQueuePriorities = priorities;
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f12.timelineSemaphore = VK_TRUE;
        f13.maintenance4 = VK_TRUE;
        f12.pNext = &f13;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &f12;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &queueInfo;
        dci.enabledExtensionCount = (uint32_t)extensions.size();
        dci.ppEnabledExtensionNames = extensions.data();
        fprintf(stderr, "phase=device\n");
        check(vkCreateDevice(phys, &dci, nullptr, &dev), "vkCreateDevice");
        fprintf(stderr, "phase=entry-checks\n");
        destroyDevice = require(reinterpret_cast<PFN_vkDestroyDevice>(gipa(inst, "vkDestroyDevice")), "vkDestroyDevice");
        vkModule = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!vkModule) throw std::runtime_error("Cannot load system Vulkan loader");
        auto driverGdpa = require(reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(vkModule, "vkGetDeviceProcAddr")), "driver vkGetDeviceProcAddr");
        const char* nvxNames[] = {"vkCreateCuModuleNVX", "vkCreateCuFunctionNVX", "vkDestroyCuModuleNVX",
            "vkDestroyCuFunctionNVX", "vkCmdCuLaunchKernelNVX", "vkGetImageViewHandleNVX",
            "vkGetImageViewHandle64NVX", "vkGetImageViewAddressNVX"};
        unsigned slNvx = 0, driverNvx = 0, sameNvx = 0;
        for (auto name : nvxNames) {
            auto a = gdpa(dev, name), b = driverGdpa(dev, name);
            slNvx += a != nullptr;
            driverNvx += b != nullptr;
            sameNvx += a == b;
        }
        auto exportQueue = require(reinterpret_cast<PFN_vkGetDeviceQueue>(GetProcAddress(slModule, "vkGetDeviceQueue")), "export vkGetDeviceQueue");
        auto procQueue = require(reinterpret_cast<PFN_vkGetDeviceQueue>(gdpa(dev, "vkGetDeviceQueue")), "SL vkGetDeviceQueue");
        auto driverQueue = require(reinterpret_cast<PFN_vkGetDeviceQueue>(driverGdpa(dev, "vkGetDeviceQueue")), "driver vkGetDeviceQueue");
        VkQueue queues[3]{};
        exportQueue(dev, family, 0, &queues[0]);
        procQueue(dev, family, 0, &queues[1]);
        driverQueue(dev, family, 0, &queues[2]);
        if (!queues[0] || queues[0] != queues[1] || queues[0] != queues[2]) throw std::runtime_error("Queue handle mismatch");
        std::array<std::array<double, 7>, 3> samples{};
        PFN_vkGetDeviceQueue functions[] = {exportQueue, procQueue, driverQueue};
        LARGE_INTEGER frequency{}, begin{}, end{};
        QueryPerformanceFrequency(&frequency);
        constexpr int iterations = 100000;
        for (int round = -1; round < 7; ++round) {
            for (int j = 0; j < 3; ++j) {
                int index = (j + round + 1) % 3;
                QueryPerformanceCounter(&begin);
                for (int i = 0; i < iterations; ++i) functions[index](dev, family, 0, &queues[index]);
                QueryPerformanceCounter(&end);
                if (round >= 0) samples[index][round] = (end.QuadPart - begin.QuadPart) * 1e9 / frequency.QuadPart / iterations;
            }
        }
        for (auto& sample : samples) std::sort(sample.begin(), sample.end());
        unsigned hooksSame = 0;
        const char* hooks[] = {"vkQueuePresentKHR", "vkCreateSwapchainKHR", "vkAcquireNextImageKHR",
                              "vkBeginCommandBuffer", "vkCmdPipelineBarrier"};
        for (auto name : hooks) hooksSame += gdpa(dev, name) == reinterpret_cast<PFN_vkVoidFunction>(GetProcAddress(slModule, name));
        printf("{\"route\":\"%s\",\"nvx\":%d,\"manual\":%d,\"ota\":false,\"gpu\":\"%s\",\"declares_nvx\":[%d,%d],\"sl_nvx_entries\":%u,\"driver_nvx_entries\":%u,\"nvx_addresses_equal\":%u,\"hook_addresses_equal\":%u,\"queue_proc_equals_driver\":%d,\"queue_export_equals_proc\":%d,\"queue_ns_median\":[%.2f,%.2f,%.2f],\"queue_ns_min\":[%.2f,%.2f,%.2f],\"queue_ns_max\":[%.2f,%.2f,%.2f],\"allocation_attempted\":false,\"present_calls\":0}\n",
            argv[1], nvx, manual, properties.deviceName, declaresBinary, declaresImage,
            slNvx, driverNvx, sameNvx, hooksSame, procQueue == driverQueue, exportQueue == procQueue,
            samples[0][3], samples[1][3], samples[2][3], samples[0][0], samples[1][0], samples[2][0],
            samples[0][6], samples[1][6], samples[2][6]);
    } catch (const std::exception& error) {
        fprintf(stderr, "FAIL: %s\n", error.what());
        exitCode = 1;
    }
    fprintf(stderr, "phase=shutdown\n");
    if (initialized) {
        auto getFunction = reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(slModule, "slGetFeatureFunction"));
        void* function = nullptr;
        if (getFunction && getFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", function) == sl::Result::eOk && function) {
            sl::DLSSGOptions options{};
            options.mode = sl::DLSSGMode::eOff;
            reinterpret_cast<PFun_slDLSSGSetOptions*>(function)(sl::ViewportHandle(0u), options);
        }
        shutdown();
    }
    if (dev && destroyDevice) destroyDevice(dev, nullptr);
    if (inst && destroyInstance) destroyInstance(inst, nullptr);
    printf("{\"exit_code\":%d}\n", exitCode);
    if (vkModule) FreeLibrary(vkModule);
    FreeLibrary(slModule);
    return exitCode;
}
