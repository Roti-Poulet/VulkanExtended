// Vulkan wrapper, to emulate new vulkan versions with older drivers and unsupported GPUs

// Supported GPUs: 
// Minimum: Kepler (GTX 6xx)
// Recommended: Maxwell 2.0

// Compilation command: x86_64-w64-mingw32-g++ -shared -o vulkan-1.dll vulkan_wrapper.cpp -static -static-libgcc -static-libstdc++ -I "C:\VulkanSDK\1.4.350.0\Include"
// Installing Vulkan SDK is required in order to compile the source code

#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cstdarg>

// ---------------------------------------------------------------------------
// Logging: vk_emu.log file (next to the DLL, otherwise in %TEMP%) + OutputDebugString.
// stderr is unusable: a game has no console, so nothing would be visible.
//
// DISABLED BY DEFAULT. Can be re-enabled with environment variables (read only once):
//   set VkExtLogging=1       -> enables vk_emu.log + OutputDebugString
//   set VkExtShadersDump=1   -> enables the SPIR-V shader dump (vkdiag_<hash>.spv)
// Values treated as "disabled": unset, empty, "0", "false", "off", "no".
// ---------------------------------------------------------------------------
static HMODULE g_selfModule = nullptr;

static bool VkEmuEnvFlag(const char* name) {
    char v[16] = {0};
    DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    if (n == 0 || n >= sizeof(v)) return n >= sizeof(v);   // unset/empty -> false ; value too long -> true
    return _stricmp(v, "0") != 0 && _stricmp(v, "false") != 0 &&
           _stricmp(v, "off") != 0 && _stricmp(v, "no") != 0;
}

static bool VkEmuLoggingEnabled() {
    static const bool e = VkEmuEnvFlag("VkExtLogging");
    return e;
}

static bool VkEmuShadersDumpEnabled() {
    static const bool e = VkEmuEnvFlag("VkExtShadersDump");
    return e;
}

static void VkEmuLog(const char* fmt, ...) {
    if (!VkEmuLoggingEnabled()) return;   // logging disabled: zero cost (no vsnprintf, no file access)
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return;

    OutputDebugStringA(buf);   // visible in a debugger / DebugView

    static SRWLOCK lock = SRWLOCK_INIT;
    static FILE* f = nullptr;
    static bool tried = false;

    AcquireSRWLockExclusive(&lock);
    if (!tried) {
        tried = true;
        char path[MAX_PATH] = {0};
        if (g_selfModule && GetModuleFileNameA(g_selfModule, path, MAX_PATH)) {
            char* slash = strrchr(path, '\\');
            if (slash) {
                slash[1] = 0;
                strncat(path, "vk_emu.log", MAX_PATH - strlen(path) - 1);
                f = fopen(path, "a");
            }
        }
        if (!f) {
            char tmp[MAX_PATH] = {0};
            DWORD l = GetTempPathA(MAX_PATH, tmp);
            if (l > 0 && l < MAX_PATH - 16) {
                strncat(tmp, "vk_emu.log", MAX_PATH - strlen(tmp) - 1);
                f = fopen(tmp, "a");
            }
        }
        if (f) {
            fprintf(f, "=== vk-emu log started, pid=%lu ===\n", (unsigned long)GetCurrentProcessId());
            fflush(f);
        }
    }
    if (f) { fputs(buf, f); fflush(f); }
    ReleaseSRWLockExclusive(&lock);
}

// Vulkan version spoof. Set it to whatever you want but the default is the latest (VK 1.4.350)
static constexpr uint32_t VKEMU_SPOOFED_API_VERSION = VK_MAKE_API_VERSION(0, 1, 4, 350);

// device state

// We dont always overwrite the driver extensions
static const char* const g_fakedDeviceExtensions[] = {
    VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME,
    VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
    VK_EXT_NON_SEAMLESS_CUBE_MAP_EXTENSION_NAME,
    VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
    VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME,
    VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME,
    VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
	VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
    VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_4_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_5_EXTENSION_NAME,
    VK_KHR_MAINTENANCE_6_EXTENSION_NAME,
    VK_KHR_PRESENT_ID_EXTENSION_NAME,
    VK_KHR_PRESENT_WAIT_EXTENSION_NAME,
    VK_KHR_SHADER_NON_SEMANTIC_INFO_EXTENSION_NAME,
    VK_KHR_SHADER_TERMINATE_INVOCATION_EXTENSION_NAME,
    VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME,
        VK_KHR_ZERO_INITIALIZE_WORKGROUP_MEMORY_EXTENSION_NAME,
    VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME,
    VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME,
    VK_KHR_MAP_MEMORY_2_EXTENSION_NAME,
    VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
    VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME,
    VK_EXT_PAGEABLE_DEVICE_LOCAL_MEMORY_EXTENSION_NAME,
    // Optional ray tracing (user request) -- pure capability facade:
    // IMPORTANT: do NOT advertise the ray tracing extensions
    // (VK_KHR_deferred_host_operations, acceleration_structure,
    // ray_tracing_pipeline, ray_query) on a GPU without hardware RT (e.g.
    // GM107/GTX 750 Ti). Unlike EDS3/maintenance4/push_descriptor,
    // there is no reasonable software fallback for hardware RT
    // (BVH, acceleration structures, RT pipelines). The game engine
    // already treats RT as optional and cleanly ignores it AS LONG AS these
    // extensions are not advertised -- listing them here without implementing
    // anything behind them turns this optional check (gracefully
    // ignored) into a hard ErrorFeatureNotPresent failure, since the feature bits
    // (rayTracingPipeline, rayQuery, accelerationStructure...) stay at
    // FALSE. And even when forcing them to TRUE, the first real call to
    // vkCreateAccelerationStructureKHR/vkCreateRayTracingPipelinesKHR
    // would crash, for lack of an implementation.
};
static constexpr uint32_t g_fakedDeviceExtensionCount =
    (uint32_t)(sizeof(g_fakedDeviceExtensions) / sizeof(g_fakedDeviceExtensions[0]));

static const char* const g_blockedDeviceExtensions[] = {
    VK_AMD_BUFFER_MARKER_EXTENSION_NAME,
};
static constexpr uint32_t g_blockedDeviceExtensionCount =
    (uint32_t)(sizeof(g_blockedDeviceExtensions) / sizeof(g_blockedDeviceExtensions[0]));
static bool IsBlockedDeviceExtension(const char* name) {
    for (uint32_t i = 0; i < g_blockedDeviceExtensionCount; i++)
        if (strcmp(name, g_blockedDeviceExtensions[i]) == 0) return true;
    return false;
}

// Device state to know what we emulate
struct DeviceState {
    bool emu_sync2 = false;
    bool emu_push_desc = false;
    bool emu_desc_buffer = false;
    bool emu_shader_object = false;
    bool sync2_native = false;
    PFN_vkCmdPipelineBarrier2 fn_CmdPipelineBarrier2 = nullptr;
    PFN_vkQueueSubmit2        fn_QueueSubmit2        = nullptr;
    PFN_vkCmdWriteTimestamp2  fn_CmdWriteTimestamp2  = nullptr;
    PFN_vkCmdSetEvent2        fn_CmdSetEvent2        = nullptr;
    PFN_vkCmdResetEvent2      fn_CmdResetEvent2      = nullptr;
    PFN_vkCmdWaitEvents2      fn_CmdWaitEvents2      = nullptr;

    bool push_descriptor_native = false;
    PFN_vkCmdPushDescriptorSetKHR fn_CmdPushDescriptorSetKHR = nullptr;

    // VK_KHR_create_renderpass2 -- real state, checked via vkGetDeviceProcAddr (not vkGetInstanceProcAddr,
    // which returns a non-null trampoline even when the driver does not implement it -- see CreateRenderPassFromKey)
    bool rp2_native = false;
    PFN_vkCreateRenderPass2   fn_CreateRenderPass2   = nullptr;
    PFN_vkCmdBeginRenderPass2 fn_CmdBeginRenderPass2 = nullptr;
    PFN_vkCmdNextSubpass2     fn_CmdNextSubpass2     = nullptr;
    PFN_vkCmdEndRenderPass2   fn_CmdEndRenderPass2   = nullptr;
    // legacy core 1.0 fallback, always resolved (guaranteed present on every Vulkan driver)
    PFN_vkCreateRenderPass    fn_CreateRenderPassLegacy    = nullptr;
    PFN_vkCmdBeginRenderPass  fn_CmdBeginRenderPassLegacy  = nullptr;
    PFN_vkCmdNextSubpass      fn_CmdNextSubpassLegacy      = nullptr;
    PFN_vkCmdEndRenderPass    fn_CmdEndRenderPassLegacy    = nullptr;

    // VK_KHR_imageless_framebuffer -- if absent, a real framebuffer must be created, explicitly bound
    // to the VkImageViews (no VkRenderPassAttachmentBeginInfo at begin time)
    bool imageless_fb_native = false;

    // legacy fallback
    PFN_vkCmdPipelineBarrier fn_CmdPipelineBarrier = nullptr;
    PFN_vkQueueSubmit        fn_QueueSubmit        = nullptr;
    PFN_vkCmdWriteTimestamp  fn_CmdWriteTimestamp  = nullptr;
    PFN_vkCmdSetEvent        fn_CmdSetEvent        = nullptr;
    PFN_vkCmdResetEvent      fn_CmdResetEvent      = nullptr;
    PFN_vkCmdWaitEvents      fn_CmdWaitEvents      = nullptr;
};
static std::mutex g_deviceStateMutex;
static std::unordered_map<VkDevice, DeviceState> g_deviceState;

// AMD support test (which doesnt work bruh)
static std::mutex g_deviceMutex;
static std::unordered_map<VkDevice, PFN_vkGetDeviceProcAddr> g_realDeviceProcAddr;

static bool GetDeviceState(VkDevice device, DeviceState* out) {
    std::lock_guard<std::mutex> lock(g_deviceStateMutex);
    auto it = g_deviceState.find(device);
    if (it == g_deviceState.end()) return false;
    *out = it->second;
    return true;
}

static std::mutex g_queueDeviceMutex;
static std::unordered_map<VkQueue, VkDevice> g_queueDevice;
static VkDevice GetDeviceForQueue(VkQueue queue) {
    std::lock_guard<std::mutex> lock(g_queueDeviceMutex);
    auto it = g_queueDevice.find(queue);
    return it != g_queueDevice.end() ? it->second : VK_NULL_HANDLE;
}
static void TrackQueueDevice(VkQueue queue, VkDevice device) {
    std::lock_guard<std::mutex> lock(g_queueDeviceMutex);
    g_queueDevice[queue] = device;
}

//VK_EXT_descriptor_buffer
static constexpr uint32_t kDescSlotMagic = 0x44455343; // "DESC"

struct DescSlot {
    uint32_t magic = kDescSlotMagic;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    VkSampler sampler = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkDeviceAddress bufferAddress = 0;
    VkDeviceSize bufferRange = 0;
    VkFormat texelBufferFormat = VK_FORMAT_UNDEFINED;
};

struct BufferAddressInfo {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress baseAddress = 0;
    VkDeviceSize size = 0;
};
static std::mutex g_bufferAddressMutex;
static std::unordered_map<VkDeviceAddress, BufferAddressInfo> g_bufferAddressByBase;
static std::vector<BufferAddressInfo> g_bufferAddressSorted; // kept sorted
static bool g_bufferAddressSortedDirty = false;

static void RegisterBufferAddress(VkBuffer buffer, VkDeviceAddress addr, VkDeviceSize size) {
    std::lock_guard<std::mutex> lock(g_bufferAddressMutex);
    g_bufferAddressByBase[addr] = BufferAddressInfo{buffer, addr, size};
    g_bufferAddressSortedDirty = true;
}

static void UnregisterBufferAddress(VkDeviceAddress addr) {
    std::lock_guard<std::mutex> lock(g_bufferAddressMutex);
    g_bufferAddressByBase.erase(addr);
    g_bufferAddressSortedDirty = true;
}

static VkBuffer ResolveBufferFromAddress(VkDeviceAddress addr, VkDeviceSize* outOffset) {
    std::lock_guard<std::mutex> lock(g_bufferAddressMutex);
    if (g_bufferAddressSortedDirty) {
        g_bufferAddressSorted.clear();
        g_bufferAddressSorted.reserve(g_bufferAddressByBase.size());
        for (auto& kv : g_bufferAddressByBase) g_bufferAddressSorted.push_back(kv.second);
        std::sort(g_bufferAddressSorted.begin(), g_bufferAddressSorted.end(),
                  [](const BufferAddressInfo& a, const BufferAddressInfo& b) { return a.baseAddress < b.baseAddress; });
        g_bufferAddressSortedDirty = false;
    }
    // Binary search for the largest baseAddress <= addr
    auto it = std::upper_bound(g_bufferAddressSorted.begin(), g_bufferAddressSorted.end(), addr,
        [](VkDeviceAddress a, const BufferAddressInfo& info) { return a < info.baseAddress; });
    if (it == g_bufferAddressSorted.begin()) return VK_NULL_HANDLE;
    --it;
    if (addr >= it->baseAddress && addr < it->baseAddress + it->size) {
        if (outOffset) *outOffset = addr - it->baseAddress;
        return it->buffer;
    }
    return VK_NULL_HANDLE;
}


struct BoundDescBuffer {
    VkDeviceAddress address = 0;
};
static std::mutex g_cmdDescBufMutex;
static std::unordered_map<VkCommandBuffer, std::vector<BoundDescBuffer>> g_cmdBoundDescBuffers;

struct EmuDescSetKey {
    VkDescriptorSetLayout layout;
    size_t contentHash;
    bool operator==(const EmuDescSetKey& o) const { return layout == o.layout && contentHash == o.contentHash; }
};
struct EmuDescSetKeyHash {
    size_t operator()(const EmuDescSetKey& k) const {
        return std::hash<void*>()(k.layout) ^ (k.contentHash + 0x9e3779b9);
    }
};
struct EmuDescBufferPool {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    uint32_t allocatedCount = 0;
    static constexpr uint32_t kMaxSetsPerPool = 4096;
};
static std::mutex g_descBufPoolMutex;
static std::unordered_map<VkDevice, std::vector<EmuDescBufferPool>> g_descBufPools;
static std::unordered_map<VkDevice, std::unordered_map<EmuDescSetKey, VkDescriptorSet, EmuDescSetKeyHash>> g_descBufSetCache;

// Cache of the supported extensions
static std::mutex g_physDeviceMutex;
static std::unordered_map<VkPhysicalDevice, std::unordered_set<std::string>> g_realDeviceExtensions;

// 1. Logging (once for each function)
static std::mutex g_logMutex;
static std::unordered_set<std::string> g_alreadyLoggedStub;

static void LogStubCall(const char* name) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_alreadyLoggedStub.insert(name).second) {
        VkEmuLog("[vk-emu] STUB CALLED: %s -- check whether the game really needs it\n", name);
        /* log flushed in VkEmuLog */
    }
}

static void LogFatal(const char* name) {
    VkEmuLog("[vk-emu] FATAL ERROR: the real driver does not expose %s (expected in core 1.2)\n", name);
    /* log flushed in VkEmuLog */
}

// load of the real system loader (vulkan-1.dll)

struct RealLoader {
    HMODULE module = nullptr;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
};
static RealLoader g_real;

static std::atomic<VkInstance> g_lastInstance{VK_NULL_HANDLE};
// All live VkInstances (an app/vkd3d may create several, e.g. a probe device, then destroy only one of them):
// destroying one instance must not leave instance-level resolution without a valid instance.
static std::mutex g_liveInstancesMutex;
static std::vector<VkInstance> g_liveInstances;
// Last VkDevice created: used to check the REAL presence of device functions (see LazyResolve)
static std::atomic<VkDevice> g_lastDevice{VK_NULL_HANDLE};
// VK_EXT_shader_viewport_index_layer enabled on the real device (see DowngradeSpirvTo15)
static std::atomic<bool> g_layerExtEnabled{false};

// Diagnostic switches: environment variable VkExtFix (hex), 0 by default.
//   0x01 : DISABLES the removal of ZeroExtend/SignExtend image operands from the SPIR-V (enabled by default)
//   0x02 : removes VkPipelineRenderingCreateInfo from the pNext of emulated graphics pipelines (renderPass != NULL)
//   0x04 : forces depthClampEnable = FALSE in graphics pipelines
//   0x08 : replaces any fragment shader that reads gl_Layer with a trivial FS (writes 0 to output 0, uint)
static uint32_t VkEmuFixFlags() {
    static uint32_t f = [] { const char* e = getenv("VkExtFix"); return e ? (uint32_t)strtoul(e, nullptr, 16) : 0u; }();
    return f;
}

static void EnsureRealLoaderLoaded() {
    if (g_real.module) return;
    
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    std::string path = std::string(sysDir) + "\\vulkan-1.dll";
    
    g_real.module = LoadLibraryA(path.c_str());
    if (!g_real.module) {
        VkEmuLog("[vk-emu] FATAL ERROR: Impossible to load the real driver, this may result in a crash %s\n", path.c_str());
        return;
    }
    g_real.GetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)GetProcAddress(g_real.module, "vkGetInstanceProcAddr");
}

static std::mutex g_resolveMutex;
static std::unordered_map<std::string, PFN_vkVoidFunction> g_resolvedCache;

struct InstanceAllocs {
    VkInstanceCreateInfo* ci = nullptr;
    VkApplicationInfo* appInfo = nullptr;
    std::vector<const char*>* enabledExts = nullptr;
    std::vector<char*> strdupedStrings; // pApplicationName, pEngineName, extensions
};
static std::mutex g_instanceAllocsMutex;
static std::unordered_map<VkInstance, InstanceAllocs> g_instanceAllocs;
template <typename PFN>
static PFN ResolveGlobal(const char* name) {
    EnsureRealLoaderLoaded();
    PFN_vkVoidFunction fn = nullptr;
    if (g_real.GetInstanceProcAddr)
        fn = g_real.GetInstanceProcAddr(nullptr, name);
    if (!fn && g_real.module)
        fn = (PFN_vkVoidFunction)GetProcAddress(g_real.module, name);
    return (PFN)fn;
}

// Instance-level functions: do NOT validate them via vkGetDeviceProcAddr.
static bool IsInstanceLevelName(const char* n) {
    return strncmp(n, "vkGetPhysicalDevice", 19) == 0 ||
           strncmp(n, "vkEnumerate", 11) == 0 ||
           strcmp(n, "vkCreateInstance") == 0 ||
           strcmp(n, "vkDestroyInstance") == 0 ||
           strcmp(n, "vkCreateDevice") == 0 ||
           strcmp(n, "vkCreateWin32SurfaceKHR") == 0 ||
           strcmp(n, "vkDestroySurfaceKHR") == 0 ||
           strncmp(n, "vkGetInstanceProcAddr", 21) == 0;
}

// FIX call-0x0: vkGetInstanceProcAddr (system loader) returns a NON-NULL trampoline even for a
// DEVICE function the driver does not implement (e.g. vkGetDeviceBufferMemoryRequirements on a
// 1.2 driver); the call then jumps into a null dispatch table => EXCEPTION_ACCESS_VIOLATION
// at address 0 (DEP). So every device function is validated against the REAL vkGetDeviceProcAddr of the
// last device: if it returns NULL, the function does not exist and LazyResolve returns nullptr
// (callers then fall back to their emulation / stub).
template <typename PFN>
static PFN LazyResolve(const char* name) {
    std::lock_guard<std::mutex> lock(g_resolveMutex);
    auto it = g_resolvedCache.find(name);
    if (it != g_resolvedCache.end())
        return (PFN)it->second;

    EnsureRealLoaderLoaded();
    PFN_vkVoidFunction fn = nullptr;
    VkInstance inst = g_lastInstance.load(std::memory_order_acquire);
    if (g_real.GetInstanceProcAddr && inst != VK_NULL_HANDLE)
        fn = g_real.GetInstanceProcAddr(inst, name);

    if (fn && !IsInstanceLevelName(name)) {
        VkDevice dev = g_lastDevice.load(std::memory_order_acquire);
        if (dev == VK_NULL_HANDLE) {
            // No device yet: cannot validate, so we do not cache.
            return (PFN)fn;
        }
        PFN_vkGetDeviceProcAddr getter = nullptr;
        {
            std::lock_guard<std::mutex> dl(g_deviceMutex);
            auto d = g_realDeviceProcAddr.find(dev);
            if (d != g_realDeviceProcAddr.end()) getter = d->second;
        }
        if (getter && !getter(dev, name)) {
            VkEmuLog("[vk-emu] %s missing from the real driver -> emulation / stub\n", name);
            fn = nullptr;
            g_resolvedCache[name] = nullptr;   // negative cache
            return nullptr;
        }
    }

    if (fn)
        g_resolvedCache[name] = fn;
    return (PFN)fn;
}

static bool RealDeviceHasExtension(VkPhysicalDevice physDevice, const char* extName) {
    std::lock_guard<std::mutex> lock(g_physDeviceMutex);
    auto it = g_realDeviceExtensions.find(physDevice);
    if (it == g_realDeviceExtensions.end()) {
        auto real = LazyResolve<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
        if (!real) return false;
        uint32_t count = 0;
        real(physDevice, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> props(count);
        real(physDevice, nullptr, &count, props.data());
        std::unordered_set<std::string> exts;
        for (auto& p : props) exts.insert(p.extensionName);
        g_realDeviceExtensions[physDevice] = std::move(exts);
        it = g_realDeviceExtensions.find(physDevice);
    }
    return it->second.count(extName) > 0;
}

static VkDescriptorPool GetOrCreateEmuDescPool(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_descBufPoolMutex);
    auto& pools = g_descBufPools[device];
    if (!pools.empty() && pools.back().allocatedCount < EmuDescBufferPool::kMaxSetsPerPool)
        return pools.back().pool;

    auto real_createPool = LazyResolve<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool");
    if (!real_createPool) return VK_NULL_HANDLE;

    VkDescriptorPoolSize sizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 4096 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4096 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4096 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4096 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2048 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 2048 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4096 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 512 },
    };
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pci.maxSets = EmuDescBufferPool::kMaxSetsPerPool;
    pci.poolSizeCount = (uint32_t)(sizeof(sizes) / sizeof(sizes[0]));
    pci.pPoolSizes = sizes;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (real_createPool(device, &pci, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    pools.push_back(EmuDescBufferPool{pool, 0});
    return pool;
}



// --- VkImage -> number of samples ---
static std::mutex g_imageMutex;
static std::unordered_map<VkImage, VkSampleCountFlagBits> g_imageSamples;

// --- VkImageView -> (format, samples) ---
// --- VkImage -> info (samples, dimensions, usage) ---
struct ImageInfo {
    VkSampleCountFlagBits samples;
    uint32_t width;
    uint32_t height;
    uint32_t arrayLayers;
    VkImageUsageFlags usage;
};
static std::unordered_map<VkImage, ImageInfo> g_imageInfo;

// --- VkSwapchainKHR -> info + images (swapchain images do NOT go through vkCreateImage:
// without this tracking, their VkImageViews were seen as 1x1 / usage 0 and the emulated framebuffer of the
// presentation blit was 1x1 => black window) ---
struct SwapchainInfo {
    uint32_t width, height, layers;
    VkImageUsageFlags usage;
    VkFormat format;
    std::vector<VkImage> images;
};
static std::unordered_map<VkSwapchainKHR, SwapchainInfo> g_swapchainInfo;

// --- VkImageView -> complete info ---
struct ImageViewInfo {
    VkFormat format;
    VkSampleCountFlagBits samples;
    uint32_t width;
    uint32_t height;
    uint32_t layerCount;
    VkImageUsageFlags usage;
};
static std::mutex g_imageViewMutex;
static std::unordered_map<VkImageView, ImageViewInfo> g_imageViewInfo;

// --- VkCommandBuffer -> VkDevice ---
static std::mutex g_cmdBufferMutex;
static std::unordered_map<VkCommandBuffer, VkDevice> g_cmdBufferDevice;

static ImageViewInfo LookupImageViewInfo(VkImageView view) {
    std::lock_guard<std::mutex> lock(g_imageViewMutex);
    auto it = g_imageViewInfo.find(view);
    if (it != g_imageViewInfo.end()) return it->second;
    return ImageViewInfo{ VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT, 1, 1, 1, 0 };
}

static VkFormat LookupImageViewFormat(VkImageView view) {
    return LookupImageViewInfo(view).format;
}

static VkSampleCountFlagBits LookupImageViewSamples(VkImageView view) {
    return LookupImageViewInfo(view).samples;
}

static VkDevice GetDeviceForCommandBuffer(VkCommandBuffer cmd) {
    std::lock_guard<std::mutex> lock(g_cmdBufferMutex);
    auto it = g_cmdBufferDevice.find(cmd);
    if (it != g_cmdBufferDevice.end()) return it->second;
    LogStubCall("GetDeviceForCommandBuffer:unknown_cmd");
    return VK_NULL_HANDLE;
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreateImage(VkDevice device, const VkImageCreateInfo* pCreateInfo,
              const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
    auto real = LazyResolve<PFN_vkCreateImage>("vkCreateImage");
    if (!real) { LogFatal("vkCreateImage"); return VK_ERROR_INITIALIZATION_FAILED; }
    VkResult r = real(device, pCreateInfo, pAllocator, pImage);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_imageMutex);
        g_imageInfo[*pImage] = {
            pCreateInfo->samples,
            pCreateInfo->extent.width,
            pCreateInfo->extent.height,
            pCreateInfo->arrayLayers,
            pCreateInfo->usage
        };
    }
    return r;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkDestroyImage(VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator) {
    {
        std::lock_guard<std::mutex> lock(g_imageMutex);
        g_imageInfo.erase(image);
    }
    auto real = LazyResolve<PFN_vkDestroyImage>("vkDestroyImage");
    if (real) real(device, image, pAllocator);
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreateImageView(VkDevice device, const VkImageViewCreateInfo* pCreateInfo,
                  const VkAllocationCallbacks* pAllocator, VkImageView* pView) {
    auto real = LazyResolve<PFN_vkCreateImageView>("vkCreateImageView");
    if (!real) { LogFatal("vkCreateImageView"); return VK_ERROR_INITIALIZATION_FAILED; }
    VkResult r = real(device, pCreateInfo, pAllocator, pView);
    if (r == VK_SUCCESS) {
        ImageViewInfo info{};
        info.format = pCreateInfo->format;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.width = 1; info.height = 1; info.layerCount = 1; info.usage = 0;
        
        {
            std::lock_guard<std::mutex> lock(g_imageMutex);
            auto it = g_imageInfo.find(pCreateInfo->image);
            if (it != g_imageInfo.end()) {
                info.samples = it->second.samples;
                info.width = it->second.width;
                info.height = it->second.height;
                info.usage = it->second.usage;
                if (pCreateInfo->subresourceRange.layerCount == VK_REMAINING_ARRAY_LAYERS)
                    info.layerCount = it->second.arrayLayers - pCreateInfo->subresourceRange.baseArrayLayer;
                else
                    info.layerCount = pCreateInfo->subresourceRange.layerCount;
            }
        }
        std::lock_guard<std::mutex> lock(g_imageViewMutex);
        g_imageViewInfo[*pView] = info;
    }
    return r;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkDestroyImageView(VkDevice device, VkImageView imageView, const VkAllocationCallbacks* pAllocator) {
    {
        std::lock_guard<std::mutex> lock(g_imageViewMutex);
        g_imageViewInfo.erase(imageView);
    }
    auto real = LazyResolve<PFN_vkDestroyImageView>("vkDestroyImageView");
    if (real) real(device, imageView, pAllocator);
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                     const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    auto real = LazyResolve<PFN_vkCreateSwapchainKHR>("vkCreateSwapchainKHR");
    if (!real) { LogFatal("vkCreateSwapchainKHR"); return VK_ERROR_INITIALIZATION_FAILED; }
    VkResult r = real(device, pCreateInfo, pAllocator, pSwapchain);
    if (r == VK_SUCCESS && pSwapchain) {
        SwapchainInfo si{};
        si.width = pCreateInfo->imageExtent.width;
        si.height = pCreateInfo->imageExtent.height;
        si.layers = pCreateInfo->imageArrayLayers ? pCreateInfo->imageArrayLayers : 1;
        si.usage = pCreateInfo->imageUsage;
        si.format = pCreateInfo->imageFormat;
        {
            std::lock_guard<std::mutex> lock(g_imageMutex);
            g_swapchainInfo[*pSwapchain] = si;
        }
        VkEmuLog("[vk-emu] swapchain %p created: %ux%u layers=%u usage=0x%x format=%d\n",
                 (void*)*pSwapchain, si.width, si.height, si.layers, (unsigned)si.usage, (int)si.format);
    }
    return r;
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pCount, VkImage* pImages) {
    auto real = LazyResolve<PFN_vkGetSwapchainImagesKHR>("vkGetSwapchainImagesKHR");
    if (!real) { LogFatal("vkGetSwapchainImagesKHR"); return VK_ERROR_INITIALIZATION_FAILED; }
    VkResult r = real(device, swapchain, pCount, pImages);
    if ((r == VK_SUCCESS || r == VK_INCOMPLETE) && pImages && pCount) {
        std::lock_guard<std::mutex> lock(g_imageMutex);
        auto it = g_swapchainInfo.find(swapchain);
        if (it != g_swapchainInfo.end()) {
            for (uint32_t i = 0; i < *pCount; i++) {
                g_imageInfo[pImages[i]] = { VK_SAMPLE_COUNT_1_BIT, it->second.width, it->second.height,
                                            it->second.layers, it->second.usage };
                bool known = false;
                for (VkImage im : it->second.images) if (im == pImages[i]) { known = true; break; }
                if (!known) it->second.images.push_back(pImages[i]);
            }
        }
    }
    return r;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator) {
    {
        std::lock_guard<std::mutex> lock(g_imageMutex);
        auto it = g_swapchainInfo.find(swapchain);
        if (it != g_swapchainInfo.end()) {
            for (VkImage im : it->second.images) g_imageInfo.erase(im);
            g_swapchainInfo.erase(it);
        }
    }
    auto real = LazyResolve<PFN_vkDestroySwapchainKHR>("vkDestroySwapchainKHR");
    if (real) real(device, swapchain, pAllocator);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue) {
    auto real = LazyResolve<PFN_vkGetDeviceQueue>("vkGetDeviceQueue");
    if (!real) { LogFatal("vkGetDeviceQueue"); return; }
    real(device, queueFamilyIndex, queueIndex, pQueue);
    if (pQueue && *pQueue != VK_NULL_HANDLE) TrackQueueDevice(*pQueue, device);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetDeviceQueue2(VkDevice device, const VkDeviceQueueInfo2* pQueueInfo, VkQueue* pQueue) {
    auto real = LazyResolve<PFN_vkGetDeviceQueue2>("vkGetDeviceQueue2");
    if (!real) { LogFatal("vkGetDeviceQueue2"); return; }
    real(device, pQueueInfo, pQueue);
    if (pQueue && *pQueue != VK_NULL_HANDLE) TrackQueueDevice(*pQueue, device);
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkAllocateCommandBuffers(VkDevice device, const VkCommandBufferAllocateInfo* pAllocateInfo,
                         VkCommandBuffer* pCommandBuffers) {
    auto real = LazyResolve<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers");
    if (!real) { LogFatal("vkAllocateCommandBuffers"); return VK_ERROR_INITIALIZATION_FAILED; }
    VkResult r = real(device, pAllocateInfo, pCommandBuffers);
    if (r == VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_cmdBufferMutex);
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; i++)
            g_cmdBufferDevice[pCommandBuffers[i]] = device;
    }
    return r;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool,
                     uint32_t commandBufferCount, const VkCommandBuffer* pCommandBuffers) {
    {
        std::lock_guard<std::mutex> lock(g_cmdBufferMutex);
        for (uint32_t i = 0; i < commandBufferCount; i++)
            g_cmdBufferDevice.erase(pCommandBuffers[i]);
    }
    auto real = LazyResolve<PFN_vkFreeCommandBuffers>("vkFreeCommandBuffers");
    if (real) real(device, commandPool, commandBufferCount, pCommandBuffers);
}

struct AttachmentDesc {
    VkFormat format;
    VkSampleCountFlagBits samples;
    VkAttachmentLoadOp loadOp;
    VkAttachmentStoreOp storeOp;
    VkImageLayout layout;      // layout while the render pass (= imageLayout gives a BeginRendering)
    bool hasResolve;
    VkFormat resolveFormat;    // if resolve used

    bool operator==(const AttachmentDesc& o) const {
        return std::memcmp(this, &o, sizeof(AttachmentDesc)) == 0;
    }
};

struct RenderPassKey {
    std::vector<AttachmentDesc> colorAttachments;
    bool hasDepth = false;
    bool hasStencil = false;
    AttachmentDesc depth{};
    AttachmentDesc stencil{};
    uint32_t viewMask = 0;   // multiview
    VkDevice device = VK_NULL_HANDLE;   // PATCH: per-device cache (otherwise a stale VkRenderPass is reused after vkDestroyDevice)

    bool operator==(const RenderPassKey& o) const {
        return viewMask == o.viewMask && device == o.device &&
               hasDepth == o.hasDepth && hasStencil == o.hasStencil &&
               colorAttachments.size() == o.colorAttachments.size() &&
               std::equal(colorAttachments.begin(), colorAttachments.end(), o.colorAttachments.begin()) &&
               (!hasDepth   || depth   == o.depth) &&
               (!hasStencil || stencil == o.stencil);
    }
};

struct RenderPassKeyHash {
    size_t operator()(const RenderPassKey& k) const {
        size_t h = std::hash<uint32_t>()(k.viewMask) ^ std::hash<void*>()((void*)k.device);
        for (auto& a : k.colorAttachments)
            h ^= std::hash<uint64_t>()((uint64_t)a.format << 32 | a.loadOp) + 0x9e3779b9 + (h << 6) + (h >> 2);
        if (k.hasDepth)
            h ^= std::hash<uint32_t>()(k.depth.format) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct FramebufferKey {
    VkRenderPass renderPass;
    VkDevice device = VK_NULL_HANDLE;   // PATCH: per-device cache
    uint32_t width, height, layers;
    // Empty in imageless mode (the framebuffer is not bound to specific views).
    // Filled in legacy (non-imageless) mode: a legacy framebuffer IS bound to
    // specific views, so two different view combinations = two different framebuffers.
    std::vector<VkImageView> views;
    bool operator==(const FramebufferKey& o) const {
        return renderPass == o.renderPass && device == o.device && width == o.width &&
               height == o.height && layers == o.layers && views == o.views;
    }
};
struct FramebufferKeyHash {
    size_t operator()(const FramebufferKey& k) const {
        size_t h = std::hash<void*>()(k.renderPass) ^ std::hash<void*>()((void*)k.device) ^ (k.width << 1) ^ (k.height << 2) ^ (k.layers << 3);
        for (auto v : k.views)
            h ^= std::hash<void*>()((void*)v) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

static std::mutex g_cacheMutex;
static std::unordered_map<RenderPassKey, VkRenderPass, RenderPassKeyHash> g_renderPassCache;
static std::unordered_map<FramebufferKey, VkFramebuffer, FramebufferKeyHash> g_framebufferCache;

static VkFramebuffer GetOrCreateFramebuffer(VkDevice device, VkRenderPass rp,
                                             const std::vector<VkImageView>& views,
                                             bool useImageless, const VkRect2D& renderArea) {
    uint32_t w = 1, h = 1, layers = 1;
    if (!views.empty()) {
        auto info = LookupImageViewInfo(views[0]);
        w = info.width;
        h = info.height;
        layers = info.layerCount;
        // Image unknown to the wrapper (usage==0): never guess 1x1, the framebuffer must at least
        // cover the renderArea (which is always contained in the real image).
        if (info.usage == 0) {
            w = (uint32_t)renderArea.offset.x + renderArea.extent.width;
            h = (uint32_t)renderArea.offset.y + renderArea.extent.height;
            layers = 1;
        }
    }

    FramebufferKey key;
    key.renderPass = rp; key.device = device; key.width = w; key.height = h; key.layers = layers;
    if (!useImageless) key.views = views; // legacy: bound to specific views, must be part of the key

    std::lock_guard<std::mutex> lock(g_cacheMutex);
    auto it = g_framebufferCache.find(key);
    if (it != g_framebufferCache.end())
        return it->second;

    auto real_vkCreateFramebuffer = LazyResolve<PFN_vkCreateFramebuffer>("vkCreateFramebuffer");
    if (!real_vkCreateFramebuffer) {
        LogFatal("vkCreateFramebuffer");
        return VK_NULL_HANDLE;
    }

    VkFramebuffer fb = VK_NULL_HANDLE;

    if (useImageless) {
        std::vector<VkFormat> formatsStorage(views.size());
        std::vector<VkFramebufferAttachmentImageInfo> imgInfos;
        imgInfos.reserve(views.size());

        for (size_t i = 0; i < views.size(); i++) {
            auto info = LookupImageViewInfo(views[i]);
            formatsStorage[i] = info.format;

            VkFramebufferAttachmentImageInfo imgInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO};
            imgInfo.usage = info.usage;
            if (imgInfo.usage == 0) imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; // safe fallback
            imgInfo.width = info.width;
            imgInfo.height = info.height;
            imgInfo.layerCount = info.layerCount;
            imgInfo.viewFormatCount = 1;
            imgInfo.pViewFormats = &formatsStorage[i];
            imgInfos.push_back(imgInfo);
        }

        VkFramebufferAttachmentsCreateInfo attCreateInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO};
        attCreateInfo.attachmentImageInfoCount = (uint32_t)imgInfos.size();
        attCreateInfo.pAttachmentImageInfos = imgInfos.data();

        VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbci.pNext = &attCreateInfo;
        fbci.flags = VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;
        fbci.renderPass = rp;
        fbci.attachmentCount = (uint32_t)imgInfos.size();
        fbci.width = w; fbci.height = h; fbci.layers = layers;

        real_vkCreateFramebuffer(device, &fbci, nullptr, &fb);
    } else {
        // Legacy fallback (VK_KHR_imageless_framebuffer absent, e.g. Kepler): the framebuffer
        // is explicitly bound to the VkImageViews passed here, no rebind possible at begin.
        VkFramebufferCreateInfo fbci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbci.renderPass = rp;
        fbci.attachmentCount = (uint32_t)views.size();
        fbci.pAttachments = views.empty() ? nullptr : views.data();
        fbci.width = w; fbci.height = h; fbci.layers = layers;

        real_vkCreateFramebuffer(device, &fbci, nullptr, &fb);
    }

    g_framebufferCache[key] = fb;
    return fb;
}

static VkRenderPass CreateRenderPassFromKey(VkDevice device, const RenderPassKey& keyIn) {
    RenderPassKey key = keyIn;
    key.device = device;   // PATCH: one cache per VkDevice
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    auto it = g_renderPassCache.find(key);
    if (it != g_renderPassCache.end())
        return it->second;

    std::vector<VkAttachmentDescription2> attachments;
    std::vector<VkAttachmentReference2> colorRefs;

    for (uint32_t i = 0; i < key.colorAttachments.size(); i++) {
        const auto& d = key.colorAttachments[i];
        VkAttachmentDescription2 ad{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
        ad.format = d.format;
        ad.samples = d.samples;
        ad.loadOp = d.loadOp;
        ad.storeOp = d.storeOp;
        ad.initialLayout = d.layout;
        ad.finalLayout = d.layout;
        attachments.push_back(ad);

        VkAttachmentReference2 ref{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
        ref.attachment = i;
        ref.layout = d.layout;
        colorRefs.push_back(ref);
    }

    VkAttachmentReference2 depthStencilRef{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
    VkAttachmentDescription2 depthDesc{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
    bool hasDS = key.hasDepth || key.hasStencil;
    if (hasDS) {
        uint32_t dsIndex = (uint32_t)attachments.size();
        depthDesc.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        depthDesc.format = key.hasDepth ? key.depth.format : key.stencil.format;
        depthDesc.samples = key.hasDepth ? key.depth.samples : key.stencil.samples;
        depthDesc.loadOp = key.hasDepth ? key.depth.loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthDesc.storeOp = key.hasDepth ? key.depth.storeOp : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthDesc.stencilLoadOp = key.hasStencil ? key.stencil.loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthDesc.stencilStoreOp = key.hasStencil ? key.stencil.storeOp : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthDesc.initialLayout = key.hasDepth ? key.depth.layout : key.stencil.layout;
        depthDesc.finalLayout = depthDesc.initialLayout;
        attachments.push_back(depthDesc);

        depthStencilRef.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
        depthStencilRef.attachment = dsIndex;
        depthStencilRef.layout = depthDesc.initialLayout;
    }

    VkSubpassDescription2 subpass{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = (uint32_t)colorRefs.size();
    subpass.pColorAttachments = colorRefs.data();
    subpass.pDepthStencilAttachment = hasDS ? &depthStencilRef : nullptr;
    subpass.viewMask = key.viewMask;

    VkSubpassDependency2 depIn{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
    depIn.srcSubpass = VK_SUBPASS_EXTERNAL;
    depIn.dstSubpass = 0;
    depIn.srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    depIn.dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    depIn.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    depIn.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    depIn.dependencyFlags = 0;

    VkSubpassDependency2 depOut{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2};
    depOut.srcSubpass = 0;
    depOut.dstSubpass = VK_SUBPASS_EXTERNAL;
    depOut.srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    depOut.dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    depOut.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    depOut.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    depOut.dependencyFlags = 0;

    VkSubpassDependency2 deps[2] = { depIn, depOut };

    VkRenderPassCreateInfo2 rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    rpci.attachmentCount = (uint32_t)attachments.size();
    rpci.pAttachments = attachments.data();
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;
    rpci.dependencyCount = 2;
    rpci.pDependencies = deps;

    DeviceState ds;
    bool haveState = GetDeviceState(device, &ds);
    VkRenderPass rp = VK_NULL_HANDLE;

    // IMPORTANT: we NEVER trust a non-null pointer returned by LazyResolve()
    // (vkGetInstanceProcAddr) to decide whether vkCreateRenderPass2 is really supported:
    // the Vulkan loader returns a valid trampoline for this name even if the driver does not
    // implement it, and calling it then crashes with pc=0x0 inside vulkan-1.dll
    // (null per-device dispatch table). Only a real vkGetDeviceProcAddr (see DeviceState,
    // filled in vkCreateDevice) tells the truth.
    if (haveState && ds.rp2_native) {
        ds.fn_CreateRenderPass2(device, &rpci, nullptr, &rp);
    } else {
        // Legacy fallback (core 1.0), used on GPUs without VK_KHR_create_renderpass2 (e.g. Kepler/K420)
        std::vector<VkAttachmentDescription> attachmentsLegacy;
        std::vector<VkAttachmentReference> colorRefsLegacy;
        for (uint32_t i = 0; i < key.colorAttachments.size(); i++) {
            const auto& d = key.colorAttachments[i];
            VkAttachmentDescription ad{};
            ad.format = d.format;
            ad.samples = d.samples;
            ad.loadOp = d.loadOp;
            ad.storeOp = d.storeOp;
            ad.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            ad.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            ad.initialLayout = d.layout;
            ad.finalLayout = d.layout;
            attachmentsLegacy.push_back(ad);

            VkAttachmentReference ref{};
            ref.attachment = i;
            ref.layout = d.layout;
            colorRefsLegacy.push_back(ref);
        }

        VkAttachmentReference depthStencilRefLegacy{};
        if (hasDS) {
            uint32_t dsIndex = (uint32_t)attachmentsLegacy.size();
            VkAttachmentDescription ad{};
            ad.format = key.hasDepth ? key.depth.format : key.stencil.format;
            ad.samples = key.hasDepth ? key.depth.samples : key.stencil.samples;
            ad.loadOp = key.hasDepth ? key.depth.loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            ad.storeOp = key.hasDepth ? key.depth.storeOp : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            ad.stencilLoadOp = key.hasStencil ? key.stencil.loadOp : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            ad.stencilStoreOp = key.hasStencil ? key.stencil.storeOp : VK_ATTACHMENT_STORE_OP_DONT_CARE;
            ad.initialLayout = key.hasDepth ? key.depth.layout : key.stencil.layout;
            ad.finalLayout = ad.initialLayout;
            attachmentsLegacy.push_back(ad);

            depthStencilRefLegacy.attachment = dsIndex;
            depthStencilRefLegacy.layout = ad.initialLayout;
        }

        VkSubpassDescription subpassLegacy{};
        subpassLegacy.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpassLegacy.colorAttachmentCount = (uint32_t)colorRefsLegacy.size();
        subpassLegacy.pColorAttachments = colorRefsLegacy.data();
        subpassLegacy.pDepthStencilAttachment = hasDS ? &depthStencilRefLegacy : nullptr;

        VkSubpassDependency depInLegacy{};
        depInLegacy.srcSubpass = VK_SUBPASS_EXTERNAL;
        depInLegacy.dstSubpass = 0;
        depInLegacy.srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        depInLegacy.dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        depInLegacy.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        depInLegacy.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

        VkSubpassDependency depOutLegacy{};
        depOutLegacy.srcSubpass = 0;
        depOutLegacy.dstSubpass = VK_SUBPASS_EXTERNAL;
        depOutLegacy.srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        depOutLegacy.dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        depOutLegacy.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        depOutLegacy.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

        VkSubpassDependency depsLegacy[2] = { depInLegacy, depOutLegacy };

        VkRenderPassCreateInfo rpciLegacy{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpciLegacy.attachmentCount = (uint32_t)attachmentsLegacy.size();
        rpciLegacy.pAttachments = attachmentsLegacy.data();
        rpciLegacy.subpassCount = 1;
        rpciLegacy.pSubpasses = &subpassLegacy;
        rpciLegacy.dependencyCount = 2;
        rpciLegacy.pDependencies = depsLegacy;

        // Multiview (VR/instanced-view) is core 1.1, independent of create_renderpass2:
        // we keep it via VkRenderPassMultiviewCreateInfo if the game uses it.
        VkRenderPassMultiviewCreateInfo multiview{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
        uint32_t correlationMask = key.viewMask;
        if (key.viewMask != 0) {
            multiview.subpassCount = 1;
            multiview.pViewMasks = &key.viewMask;
            multiview.correlationMaskCount = 1;
            multiview.pCorrelationMasks = &correlationMask;
            rpciLegacy.pNext = &multiview;
        }

        if (!haveState || !ds.fn_CreateRenderPassLegacy) {
            LogFatal("vkCreateRenderPass (legacy)");
            return VK_NULL_HANDLE;
        }
        ds.fn_CreateRenderPassLegacy(device, &rpciLegacy, nullptr, &rp);
    }

    g_renderPassCache[key] = rp;
    return rp;
}

static VkRenderPass GetOrCreateRenderPass(VkDevice device, const VkRenderingInfo* info) {
    RenderPassKey key;
    key.viewMask = info->viewMask;

    for (uint32_t i = 0; i < info->colorAttachmentCount; i++) {
        const auto& ca = info->pColorAttachments[i];
        AttachmentDesc d{};
        d.format   = LookupImageViewFormat(ca.imageView);
        d.samples  = LookupImageViewSamples(ca.imageView);
        d.loadOp   = ca.loadOp;
        d.storeOp  = ca.storeOp;
        d.layout   = ca.imageLayout;
        d.hasResolve = (ca.resolveMode != VK_RESOLVE_MODE_NONE);
        key.colorAttachments.push_back(d);
    }
    if (info->pDepthAttachment) {
        key.hasDepth = true;
        key.depth.format  = LookupImageViewFormat(info->pDepthAttachment->imageView);
        key.depth.samples = LookupImageViewSamples(info->pDepthAttachment->imageView);
        key.depth.loadOp  = info->pDepthAttachment->loadOp;
        key.depth.storeOp = info->pDepthAttachment->storeOp;
        key.depth.layout  = info->pDepthAttachment->imageLayout;
    }
    if (info->pStencilAttachment) {
        key.hasStencil = true;
        key.stencil.format  = LookupImageViewFormat(info->pStencilAttachment->imageView);
        key.stencil.samples = LookupImageViewSamples(info->pStencilAttachment->imageView);
        key.stencil.loadOp  = info->pStencilAttachment->loadOp;
        key.stencil.storeOp = info->pStencilAttachment->storeOp;
        key.stencil.layout  = info->pStencilAttachment->imageLayout;
    }

    return CreateRenderPassFromKey(device, key);
}

static VkRenderPass GetOrCreatePipelineRenderPass(VkDevice device, const VkPipelineRenderingCreateInfo* renderingInfo, VkSampleCountFlagBits samples) {
    RenderPassKey key;
    key.viewMask = renderingInfo->viewMask;

    for (uint32_t i = 0; i < renderingInfo->colorAttachmentCount; i++) {
        AttachmentDesc d{};
        d.format = renderingInfo->pColorAttachmentFormats[i];
        d.samples = samples;
        d.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        d.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        d.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        d.hasResolve = false;
        key.colorAttachments.push_back(d);
    }
    if (renderingInfo->depthAttachmentFormat != VK_FORMAT_UNDEFINED) {
        key.hasDepth = true;
        key.depth.format = renderingInfo->depthAttachmentFormat;
        key.depth.samples = samples;
        key.depth.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        key.depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        key.depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
    if (renderingInfo->stencilAttachmentFormat != VK_FORMAT_UNDEFINED) {
        key.hasStencil = true;
        key.stencil.format = renderingInfo->stencilAttachmentFormat;
        key.stencil.samples = samples;
        key.stencil.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        key.stencil.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        key.stencil.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    return CreateRenderPassFromKey(device, key);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* info) {
    VkDevice device = GetDeviceForCommandBuffer(cmd);

    DeviceState ds;
    bool haveState = GetDeviceState(device, &ds);
    bool useImageless = haveState && ds.imageless_fb_native;
    bool useRp2 = haveState && ds.rp2_native;

    VkRenderPass rp = GetOrCreateRenderPass(device, info);

    std::vector<VkFormat> formats;
    std::vector<VkImageView> views;
    for (uint32_t i = 0; i < info->colorAttachmentCount; i++) {
        views.push_back(info->pColorAttachments[i].imageView);
    }
    if (info->pDepthAttachment) {
        views.push_back(info->pDepthAttachment->imageView);
    }
    if (info->pStencilAttachment) {
        VkImageView depthView = info->pDepthAttachment ? info->pDepthAttachment->imageView : VK_NULL_HANDLE;
        if (info->pStencilAttachment->imageView != depthView) {
            views.push_back(info->pStencilAttachment->imageView);
        }
    }

    VkFramebuffer fb = GetOrCreateFramebuffer(device, rp, views, useImageless, info->renderArea);
    {   // trace of the first renders: checks that the views are known (usage != 0) and their dimensions
        static std::atomic<int> s_nBegin{0};
        if (s_nBegin.fetch_add(1) < 40) {
            ImageViewInfo vi = views.empty() ? ImageViewInfo{} : LookupImageViewInfo(views[0]);
            VkEmuLog("[vk-diag] vkCmdBeginRendering views=%zu area=%ux%u+%d+%d view0: fmt=%d %ux%u layers=%u usage=0x%x%s imageless=%d\n",
                     views.size(), info->renderArea.extent.width, info->renderArea.extent.height,
                     info->renderArea.offset.x, info->renderArea.offset.y,
                     (int)vi.format, vi.width, vi.height, vi.layerCount, (unsigned)vi.usage,
                     vi.usage == 0 ? " (UNKNOWN IMAGE)" : "", (int)useImageless);
        }
    }

    std::vector<VkClearValue> clears;
    for (uint32_t i = 0; i < info->colorAttachmentCount; i++)
        clears.push_back(info->pColorAttachments[i].clearValue);
    if (info->pDepthAttachment) clears.push_back(info->pDepthAttachment->clearValue);

    VkRenderPassAttachmentBeginInfo rpabi{VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO};
    rpabi.attachmentCount = (uint32_t)views.size();
    rpabi.pAttachments = views.data();

    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    // Chaining VkRenderPassAttachmentBeginInfo only makes sense with an imageless framebuffer:
    // in legacy mode the framebuffer is already bound to the views, a pNext here would be ignored/invalid.
    rpbi.pNext = useImageless ? (const void*)&rpabi : nullptr;
    rpbi.renderPass = rp;
    rpbi.framebuffer = fb;
    rpbi.renderArea = info->renderArea;
    rpbi.clearValueCount = (uint32_t)clears.size();
    rpbi.pClearValues = clears.data();

    VkSubpassContents contents = (info->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT)
                       ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
                       : VK_SUBPASS_CONTENTS_INLINE;

    if (useRp2) {
        VkSubpassBeginInfo sbi{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO};
        sbi.contents = contents;
        ds.fn_CmdBeginRenderPass2(cmd, &rpbi, &sbi);
    } else {
        // Legacy fallback (core 1.0) -- used on GPUs without VK_KHR_create_renderpass2 (e.g. Kepler/K420).
        // This is the same pointer that is resolved and checked non-null in vkCreateDevice.
        if (!haveState || !ds.fn_CmdBeginRenderPassLegacy) {
            LogFatal("vkCmdBeginRenderPass (legacy)");
            return;
        }
        ds.fn_CmdBeginRenderPassLegacy(cmd, &rpbi, contents);
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdEndRendering(VkCommandBuffer cmd) {
    VkDevice device = GetDeviceForCommandBuffer(cmd);
    DeviceState ds;
    bool haveState = GetDeviceState(device, &ds);

    if (haveState && ds.rp2_native) {
        VkSubpassEndInfo sei{VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
        ds.fn_CmdEndRenderPass2(cmd, &sei);
    } else {
        if (!haveState || !ds.fn_CmdEndRenderPassLegacy) {
            LogFatal("vkCmdEndRenderPass (legacy)");
            return;
        }
        ds.fn_CmdEndRenderPassLegacy(cmd);
    }
}



// =====================================================================================
// DIAG (b1_24_diag): trace of every shader module / pipeline BEFORE the call to the driver.
// The last log line printed before a crash in nvoglv64 points to the faulty object.
// =====================================================================================
static const char* DiagCapName(uint32_t c) {
    switch (c) {
        case 0: return "Matrix"; case 1: return "Shader"; case 2: return "Geometry"; case 3: return "Tessellation";
        case 9: return "Float16"; case 10: return "Float64"; case 11: return "Int64"; case 22: return "Int16";
        case 25: return "ImageGatherExtended"; case 27: return "StorageImageMultisample";
        case 28: return "UniformBufferArrayDynamicIndexing"; case 29: return "SampledImageArrayDynamicIndexing";
        case 30: return "StorageBufferArrayDynamicIndexing"; case 31: return "StorageImageArrayDynamicIndexing";
        case 32: return "ClipDistance"; case 33: return "CullDistance"; case 34: return "ImageCubeArray";
        case 35: return "SampleRateShading"; case 39: return "Int8"; case 40: return "InputAttachment";
        case 41: return "SparseResidency"; case 42: return "MinLod"; case 43: return "Sampled1D"; case 44: return "Image1D";
        case 45: return "SampledCubeArray"; case 46: return "SampledBuffer"; case 47: return "ImageBuffer";
        case 48: return "ImageMSArray"; case 49: return "StorageImageExtendedFormats"; case 50: return "ImageQuery";
        case 51: return "DerivativeControl"; case 52: return "InterpolationFunction"; case 53: return "TransformFeedback";
        case 54: return "GeometryStreams"; case 55: return "StorageImageReadWithoutFormat";
        case 56: return "StorageImageWriteWithoutFormat"; case 57: return "MultiViewport";
        case 61: return "GroupNonUniform"; case 62: return "GroupNonUniformVote"; case 63: return "GroupNonUniformArithmetic";
        case 64: return "GroupNonUniformBallot"; case 65: return "GroupNonUniformShuffle";
        case 66: return "GroupNonUniformShuffleRelative"; case 67: return "GroupNonUniformClustered";
        case 68: return "GroupNonUniformQuad"; case 69: return "ShaderLayer"; case 70: return "ShaderViewportIndex";
        case 4427: return "ShaderViewportIndexLayerEXT"; case 4437: return "DeviceGroup"; case 4439: return "MultiView";
        case 4441: return "VariablePointersStorageBuffer"; case 4442: return "VariablePointers";
        case 4448: return "StorageBuffer16BitAccess"; case 4449: return "UniformAndStorageBuffer16BitAccess";
        case 4450: return "StoragePushConstant16"; case 5301: return "ShaderNonUniform";
        case 5302: return "RuntimeDescriptorArray"; case 5345: return "VulkanMemoryModel";
        case 5347: return "PhysicalStorageBufferAddresses"; case 5379: return "DemoteToHelperInvocation";
        case 6022: return "DotProduct";
        default: return "?";
    }
}


// =====================================================================================
// FIX SPIR-V 1.6 -> 1.5 (VKD3D >= 2.11 compiles its internal shaders with --target-env vulkan1.3).
// The Vulkan 1.2 driver only accepts SPIR-V <= 1.5 and crashes (read 0x8 in nvoglv64) on:
//   - version 1.6 in the header
//   - OpExecutionModeId LocalSizeId / LocalSizeHintId (Vulkan 1.3 / maintenance4)
// We rewrite the module: version -> 1.5, LocalSizeId(constants) -> LocalSize(literals)
// (same word count, in-place modification), and we add the OpExtensions that SPIR-V 1.6
// makes implicit (demote_to_helper_invocation, terminate_invocation).
// =====================================================================================
static bool DowngradeSpirvTo15(const uint32_t* in, size_t words, std::vector<uint32_t>& out, std::string& report, bool remapLayerCap, bool stripImageExt) {
    if (words < 5 || in[0] != 0x07230203u) return false;
    const uint32_t version = in[1];
    out.assign(in, in + words);
    bool changed = false;

    std::unordered_map<uint32_t, uint32_t> constVal;      // id -> literal value (32-bit OpConstant / default of an OpSpecConstant)
    std::unordered_map<uint32_t, uint32_t> constType;     // id -> type id
    std::unordered_set<uint32_t> specIds;                 // ids defined by OpSpecConstant
    std::vector<std::pair<uint32_t, uint32_t>> vec3Types; // (OpTypeVector id, component type id), 3 components
    uint32_t uintTypeId = 0; size_t firstFuncIdx = 0; bool hasWGSizeBuiltin = false;
    // LocalSizeId whose operands are spec constants: needs BuiltIn WorkgroupSize on an OpSpecConstantComposite (SPIR-V <= 1.5 form)
    struct WgFix { uint32_t x, y, z; };
    std::vector<WgFix> wgFixes;
    bool usesDemote = false, usesTerminate = false, hasDemoteExt = false, hasTermExt = false;
    bool hasLayerExt = false, remappedLayer = false;
    for (size_t i = 5; i < words;) {
        uint32_t op = out[i] & 0xFFFF, wc = out[i] >> 16;
        if (wc == 0 || i + wc > words) break;
        if (op == 43 && wc >= 4) { constVal[out[i + 2]] = out[i + 3]; constType[out[i + 2]] = out[i + 1]; }   // OpConstant
        else if (op == 50 && wc >= 4) { constVal[out[i + 2]] = out[i + 3]; constType[out[i + 2]] = out[i + 1]; specIds.insert(out[i + 2]); } // OpSpecConstant (default value)
        else if (op == 21 && wc == 4 && out[i + 2] == 32 && out[i + 3] == 0) uintTypeId = out[i + 1];       // OpTypeInt 32 unsigned
        else if (op == 23 && wc == 4 && out[i + 3] == 3) vec3Types.push_back({out[i + 1], out[i + 2]});    // OpTypeVector (result, component) x3
        else if (op == 71 && wc >= 4 && out[i + 2] == 11 && out[i + 3] == 25) hasWGSizeBuiltin = true;     // OpDecorate BuiltIn WorkgroupSize
        else if (op == 54 && firstFuncIdx == 0) firstFuncIdx = i;                                          // first OpFunction
        else if (op == 5380) usesDemote = true;                              // OpDemoteToHelperInvocation
        else if (op == 4416) usesTerminate = true;                           // OpTerminateInvocation
        else if (op == 17 && wc >= 2 && out[i + 1] == 5379) usesDemote = true; // Capability DemoteToHelperInvocation
        else if (op == 10) {
            const char* e = (const char*)&out[i + 1];
            if (!strcmp(e, "SPV_EXT_demote_to_helper_invocation")) hasDemoteExt = true;
            if (!strcmp(e, "SPV_KHR_terminate_invocation")) hasTermExt = true;
            if (!strcmp(e, "SPV_EXT_shader_viewport_index_layer")) hasLayerExt = true;
        }
        i += wc;
    }

    for (size_t i = 5; i < words;) {
        uint32_t op = out[i] & 0xFFFF, wc = out[i] >> 16;
        if (wc == 0 || i + wc > words) break;
        if (op == 331 && wc == 6 && (out[i + 2] == 38 || out[i + 2] == 39)) {   // OpExecutionModeId LocalSizeId / LocalSizeHintId
            auto a = constVal.find(out[i + 3]), b = constVal.find(out[i + 4]), c = constVal.find(out[i + 5]);
            if (a != constVal.end() && b != constVal.end() && c != constVal.end()) {
                bool hint = (out[i + 2] == 39);
                if (!hint && (specIds.count(out[i + 3]) || specIds.count(out[i + 4]) || specIds.count(out[i + 5]))) {
                    if (!hasWGSizeBuiltin && uintTypeId &&
                        constType[out[i + 3]] == uintTypeId && constType[out[i + 4]] == uintTypeId && constType[out[i + 5]] == uintTypeId) {
                        wgFixes.push_back({ out[i + 3], out[i + 4], out[i + 5] });
                        report += "LocalSizeId(spec)->LocalSize(default)+WorkgroupSize(SpecConstantComposite) ";
                    } else if (!hasWGSizeBuiltin) {
                        report += "LocalSizeId(spec): WorkgroupSize NOT added (unexpected types) ";
                    }
                }
                out[i]     = (6u << 16) | 16u;                 // OpExecutionMode
                out[i + 2] = hint ? 18u : 17u;                 // LocalSizeHint / LocalSize
                out[i + 3] = a->second; out[i + 4] = b->second; out[i + 5] = c->second;
                char b2[96]; snprintf(b2, sizeof(b2), "%s(%u,%u,%u) ", hint ? "LocalSizeHintId->LocalSizeHint" : "LocalSizeId->LocalSize",
                                      a->second, b->second, c->second);
                report += b2; changed = true;
            } else {
                report += "LocalSizeId with spec-constants: NOT converted ";
            }
        }
        i += wc;
    }

    if (!wgFixes.empty() && firstFuncIdx) {
        uint32_t bound = out[3];
        uint32_t vec3T = 0;
        for (auto& v : vec3Types) if (v.second == uintTypeId) { vec3T = v.first; break; }
        std::vector<uint32_t> addTypes, addDecor;
        if (!vec3T) {
            vec3T = bound++;
            addTypes.insert(addTypes.end(), { (4u << 16) | 23u, vec3T, uintTypeId, 3u });   // OpTypeVector %uint 3
        }
        // only the first entry point's LocalSizeId matters for a single-entry-point compute module
        const WgFix& w = wgFixes[0];
        uint32_t comp = bound++;
        addTypes.insert(addTypes.end(), { (6u << 16) | 51u, vec3T, comp, w.x, w.y, w.z });  // OpSpecConstantComposite
        addDecor.insert(addDecor.end(), { (4u << 16) | 71u, comp, 11u, 25u });              // OpDecorate %comp BuiltIn WorkgroupSize
        // annotation insert point: just before the first type declaration (op 19..39), it comes BEFORE firstFuncIdx
        size_t decIdx = 5;
        for (size_t i = 5; i < firstFuncIdx;) {
            uint32_t op = out[i] & 0xFFFF, wc = out[i] >> 16;
            if (wc == 0) break;
            if (op >= 19 && op <= 39) { decIdx = i; break; }
            i += wc;
        }
        out.insert(out.begin() + firstFuncIdx, addTypes.begin(), addTypes.end());   // later position first
        out.insert(out.begin() + decIdx, addDecor.begin(), addDecor.end());
        out[3] = bound;
        changed = true;
    }

    // ZeroExtend(0x2000)/SignExtend(0x1000) image operands (SPIR-V 1.4+, glslang 11 with vulkan1.3): no effect for
    // 32-bit components, but the driver 472.12 compiler does not always know them (OpImageGather...).
    if (stripImageExt) {
        unsigned stripped = 0;
        for (size_t i = 5; i < words;) {
            uint32_t op = out[i] & 0xFFFF, wc = out[i] >> 16;
            if (wc == 0 || i + wc > words) break;
            int mi = -1;
            switch (op) {
                case 89: case 90: case 93: case 94: case 96: case 97: mi = 6; break;  // SampleDref*, ProjDref*, Gather, DrefGather
                case 87: case 88: case 91: case 92: case 95: case 98: mi = 5; break;  // Sample*Lod, SampleProj*, Fetch, Read
                case 99: mi = 4; break;                              // Write
            }
            if (mi > 0 && wc > (uint32_t)mi && (out[i + mi] & 0x3000u)) {
                out[i + mi] &= ~0x3000u; stripped++; changed = true;
            }
            i += wc;
        }
        if (stripped) { char b4[64]; snprintf(b4, sizeof(b4), "ImageOperands Zero/SignExtend removed x%u ", stripped); report += b4; }
    }

    // ShaderLayer(69) / ShaderViewportIndex(70) (SPIR-V 1.5+) -> ShaderViewportIndexLayerEXT(5254) + OpExtension,
    // this is exactly the form VKD3D <= 2.10 emitted (SPIR-V 1.3) and which driver 472.12 compiles.
    if (remapLayerCap) {
        bool seen5254 = false;
        for (size_t i = 5; i < words;) {
            uint32_t op = out[i] & 0xFFFF, wc = out[i] >> 16;
            if (wc == 0 || i + wc > words) break;
            if (op == 17 && wc >= 2) {
                if (out[i + 1] == 5254) seen5254 = true;
                else if (out[i + 1] == 69 || out[i + 1] == 70) {
                    out[i + 1] = 5254; remappedLayer = true; changed = true;
                    report += "ShaderLayer->ShaderViewportIndexLayerEXT ";
                }
            }
            i += wc;
        }
        (void)seen5254;
    }

    // Missing OpExtensions (implicit in 1.6): inserted after the OpCapability/OpExtension block
    std::vector<const char*> need;
    if (usesDemote && !hasDemoteExt && version >= 0x00010600) need.push_back("SPV_EXT_demote_to_helper_invocation");
    if (usesTerminate && !hasTermExt && version >= 0x00010600) need.push_back("SPV_KHR_terminate_invocation");
    if (remappedLayer && !hasLayerExt) need.push_back("SPV_EXT_shader_viewport_index_layer");
    if (!need.empty()) {
        size_t ins = 5;
        for (size_t i = 5; i < out.size();) {
            uint32_t op = out[i] & 0xFFFF, wc = out[i] >> 16;
            if (wc == 0) break;
            if (op != 17 && op != 10) { ins = i; break; }
            i += wc; ins = i;
        }
        std::vector<uint32_t> add;
        for (const char* n : need) {
            size_t len = strlen(n) + 1, nw = (len + 3) / 4;
            size_t at = add.size();
            add.resize(at + 1 + nw, 0);
            add[at] = (uint32_t)((1 + nw) << 16) | 10u;
            memcpy(&add[at + 1], n, len);
            report += std::string("+OpExtension ") + n + " ";
        }
        out.insert(out.begin() + ins, add.begin(), add.end());
        changed = true;
    }

    if (version > 0x00010500u) {
        out[1] = 0x00010500u;
        char b3[48]; snprintf(b3, sizeof(b3), "version %u.%u->1.5 ", (version >> 16) & 0xFF, (version >> 8) & 0xFF);
        report += b3; changed = true;
    }
    return changed;
}

static bool SpirvIsFragmentReadingLayer(const uint32_t* code, size_t words) {
    bool frag = false, layer = false;
    for (size_t i = 5; i < words;) {
        uint32_t op = code[i] & 0xFFFF, wc = code[i] >> 16;
        if (wc == 0 || i + wc > words) break;
        if (op == 15 && wc >= 2 && code[i + 1] == 4) frag = true;                       // OpEntryPoint Fragment
        if (op == 71 && wc >= 4 && code[i + 2] == 11 && code[i + 3] == 9) layer = true; // OpDecorate BuiltIn Layer
        i += wc;
    }
    return frag && layer;
}
static std::unordered_map<uint64_t, bool> g_diagFragLayer;   // VkShaderModule -> FS that reads gl_Layer
static const uint32_t g_stubFsSpirv[] = { 0x07230203, 0x00010500, 0x00070000, 0x00000009, 0x00000000, 0x00020011, 0x00000001, 0x0003000e, 0x00000000, 0x00000001, 0x0006000f, 0x00000004, 0x00000001, 0x6e69616d, 0x00000000, 0x00000002, 0x00030010, 0x00000001, 0x00000007, 0x00040047, 0x00000002, 0x0000001e, 0x00000000, 0x00020013, 0x00000003, 0x00030021, 0x00000004, 0x00000003, 0x00040015, 0x00000005, 0x00000020, 0x00000000, 0x00040020, 0x00000006, 0x00000003, 0x00000005, 0x0004003b, 0x00000006, 0x00000002, 0x00000003, 0x0004002b, 0x00000005, 0x00000007, 0x00000000, 0x00050036, 0x00000003, 0x00000001, 0x00000000, 0x00000004, 0x000200f8, 0x00000008, 0x0003003e, 0x00000002, 0x00000007, 0x000100fd, 0x00010038 };
static VkShaderModule GetStubFragmentModule(VkDevice device) {
    static std::mutex m; static VkShaderModule mod = VK_NULL_HANDLE;
    std::lock_guard<std::mutex> l(m);
    if (mod == VK_NULL_HANDLE) {
        auto real = LazyResolve<PFN_vkCreateShaderModule>("vkCreateShaderModule");
        VkShaderModuleCreateInfo ci{ VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        ci.codeSize = sizeof(g_stubFsSpirv); ci.pCode = g_stubFsSpirv;
        if (real) real(device, &ci, nullptr, &mod);
    }
    return mod;
}

static std::mutex g_diagMutex;
static std::unordered_map<uint64_t, uint32_t> g_diagModuleHash;   // VkShaderModule -> hash

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo,
                     const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule) {
    auto real = LazyResolve<PFN_vkCreateShaderModule>("vkCreateShaderModule");
    if (!real) { LogFatal("vkCreateShaderModule"); return VK_ERROR_INITIALIZATION_FAILED; }

    const uint32_t* code = pCreateInfo->pCode;
    size_t words = pCreateInfo->codeSize / 4;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < words; i++) { h ^= code[i]; h *= 16777619u; }

    std::string caps, exts;
    size_t i = 5;
    while (i < words) {
        uint32_t w = code[i];
        uint32_t op = w & 0xFFFF, wc = w >> 16;
        if (wc == 0) break;
        if (op == 17 && i + 1 < words) {          // OpCapability
            char b[64]; snprintf(b, sizeof(b), "%s(%u) ", DiagCapName(code[i + 1]), code[i + 1]);
            caps += b;
        } else if (op == 10) {                     // OpExtension
            exts += (const char*)&code[i + 1]; exts += " ";
        } else if (op == 14) break;                // OpMemoryModel: end of the header
        i += wc;
    }

    if (VkEmuShadersDumpEnabled()) {   // dump the SPIR-V next to the DLL: vkdiag_<hash>.spv (open with spirv-dis) -- requires VkExtShadersDump=1
        char path[MAX_PATH] = {0};
        if (g_selfModule && GetModuleFileNameA(g_selfModule, path, MAX_PATH)) {
            char* slash = strrchr(path, '\\');
            if (slash) {
                snprintf(slash + 1, MAX_PATH - (size_t)(slash + 1 - path), "vkdiag_%08x.spv", h);
                if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
                    FILE* f = fopen(path, "wb");
                    if (f) { fwrite(code, 1, pCreateInfo->codeSize, f); fclose(f); }
                }
            }
        }
    }
    std::vector<uint32_t> patched;
    std::string spvReport;
    VkShaderModuleCreateInfo patchedCi = *pCreateInfo;
    const VkShaderModuleCreateInfo* ciToUse = pCreateInfo;
    if (pCreateInfo->codeSize % 4 == 0 && DowngradeSpirvTo15(code, words, patched, spvReport, g_layerExtEnabled.load(std::memory_order_acquire), (VkEmuFixFlags() & 0x01) == 0)) {
        patchedCi.pCode = patched.data();
        patchedCi.codeSize = patched.size() * 4;
        ciToUse = &patchedCi;
        VkEmuLog("[vk-spirv] hash=%08x rewritten for Vulkan 1.2: %s\n", h, spvReport.c_str());
    }
    VkResult r = real(device, ciToUse, pAllocator, pShaderModule);
    if (r == VK_SUCCESS && pShaderModule) {
        std::lock_guard<std::mutex> l(g_diagMutex);
        g_diagModuleHash[(uint64_t)*pShaderModule] = h;
        if (SpirvIsFragmentReadingLayer(code, words)) g_diagFragLayer[(uint64_t)*pShaderModule] = true;
    }
    VkEmuLog("[vk-diag] vkCreateShaderModule hash=%08x size=%zu -> %d module=%p\n"
             "[vk-diag]    caps: %s\n[vk-diag]    ext: %s\n",
             h, pCreateInfo->codeSize, (int)r, pShaderModule ? (void*)*pShaderModule : nullptr,
             caps.c_str(), exts.c_str());
    return r;
}

static uint32_t DiagModuleHash(VkShaderModule m) {
    std::lock_guard<std::mutex> l(g_diagMutex);
    auto it = g_diagModuleHash.find((uint64_t)m);
    return it == g_diagModuleHash.end() ? 0 : it->second;
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
                         const VkComputePipelineCreateInfo* pCreateInfos,
                         const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
    auto real = LazyResolve<PFN_vkCreateComputePipelines>("vkCreateComputePipelines");
    if (!real) { LogFatal("vkCreateComputePipelines"); return VK_ERROR_INITIALIZATION_FAILED; }
    for (uint32_t i = 0; i < createInfoCount; i++) {
        const auto& ci = pCreateInfos[i];
        VkEmuLog("[vk-diag] vkCreateComputePipelines[%u/%u] flags=0x%x layout=%p shaderHash=%08x spec=%s\n",
                 i, createInfoCount, (unsigned)ci.flags, (void*)ci.layout, DiagModuleHash(ci.stage.module),
                 ci.stage.pSpecializationInfo ? "yes" : "no");
    }
    VkResult r = real(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
    VkEmuLog("[vk-diag]    -> %d\n", (int)r);
    return r;
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
                           const VkGraphicsPipelineCreateInfo* pCreateInfos,
                           const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines) {
    auto real = LazyResolve<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
    if (!real) { LogFatal("vkCreateGraphicsPipelines"); return VK_ERROR_INITIALIZATION_FAILED; }

    std::vector<VkGraphicsPipelineCreateInfo> modifiedInfos;
    bool modified = false;

    for (uint32_t i = 0; i < createInfoCount; i++) {
        const auto& ci = pCreateInfos[i];
        if (ci.renderPass == VK_NULL_HANDLE) {
            const VkPipelineRenderingCreateInfo* renderingInfo = nullptr;
            const VkBaseInStructure* pNext = (const VkBaseInStructure*)ci.pNext;
            while (pNext) {
                if (pNext->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
                    renderingInfo = (const VkPipelineRenderingCreateInfo*)pNext;
                    break;
                }
                pNext = pNext->pNext;
            }

            if (renderingInfo) {
                if (!modified) {
                    modifiedInfos.assign(pCreateInfos, pCreateInfos + createInfoCount);
                    modified = true;
                }

                VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
                if (ci.pMultisampleState) {
                    samples = ci.pMultisampleState->rasterizationSamples;
                }

                VkRenderPass rp = GetOrCreatePipelineRenderPass(device, renderingInfo, samples);
                modifiedInfos[i].renderPass = rp;
            }
        }
    }

    // --- diagnostic switches (VkExtFix) ---
    std::vector<VkPipelineRasterizationStateCreateInfo> rsCopies;
    std::vector<std::vector<VkPipelineShaderStageCreateInfo>> stageCopies(createInfoCount);
    rsCopies.reserve(createInfoCount);
    const uint32_t fx = VkEmuFixFlags();
    static std::atomic<bool> s_fxLogged{false};
    if (fx && !s_fxLogged.exchange(true)) VkEmuLog("[vk-emu] VkExtFix=0x%x active\n", fx);
    if (fx & 0x0E) {
        if (!modified) { modifiedInfos.assign(pCreateInfos, pCreateInfos + createInfoCount); modified = true; }
        for (uint32_t i = 0; i < createInfoCount; i++) {
            auto& mi = modifiedInfos[i];
            if ((fx & 0x02) && mi.renderPass != VK_NULL_HANDLE && mi.pNext &&
                ((const VkBaseInStructure*)mi.pNext)->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
                mi.pNext = ((const VkBaseInStructure*)mi.pNext)->pNext;
                VkEmuLog("[vk-emu] FIX 0x02: VkPipelineRenderingCreateInfo removed from pNext\n");
            }
            if ((fx & 0x04) && mi.pRasterizationState && mi.pRasterizationState->depthClampEnable) {
                rsCopies.push_back(*mi.pRasterizationState);
                rsCopies.back().depthClampEnable = VK_FALSE;
                mi.pRasterizationState = &rsCopies.back();
                VkEmuLog("[vk-emu] FIX 0x04: depthClampEnable forced to FALSE\n");
            }
            if (fx & 0x08) {
                for (uint32_t k = 0; k < mi.stageCount; k++) {
                    if (mi.pStages[k].stage != VK_SHADER_STAGE_FRAGMENT_BIT) continue;
                    bool isLayerFs;
                    { std::lock_guard<std::mutex> l(g_diagMutex); isLayerFs = g_diagFragLayer.count((uint64_t)mi.pStages[k].module) != 0; }
                    if (!isLayerFs) continue;
                    VkShaderModule stub = GetStubFragmentModule(device);
                    if (stub == VK_NULL_HANDLE) continue;
                    const VkShaderModule origMod = mi.pStages[k].module;
                    stageCopies[i].assign(mi.pStages, mi.pStages + mi.stageCount);
                    stageCopies[i][k].module = stub;
                    stageCopies[i][k].pSpecializationInfo = nullptr;
                    mi.pStages = stageCopies[i].data();
                    VkEmuLog("[vk-emu] FIX 0x08: FS reading gl_Layer (hash=%08x) replaced by a trivial FS\n", DiagModuleHash(origMod));
                    break;
                }
            }
        }
    }

    const VkGraphicsPipelineCreateInfo* finalInfos = modified ? modifiedInfos.data() : pCreateInfos;
    for (uint32_t i = 0; i < createInfoCount; i++) {
        const auto& ci = finalInfos[i];
        std::string stages;
        for (uint32_t k = 0; k < ci.stageCount; k++) {
            char b[64]; snprintf(b, sizeof(b), "[stage=0x%x hash=%08x] ", (unsigned)ci.pStages[k].stage, DiagModuleHash(ci.pStages[k].module));
            stages += b;
        }
        const VkPipelineRenderingCreateInfo* ri = nullptr;
        for (const VkBaseInStructure* n = (const VkBaseInStructure*)pCreateInfos[i].pNext; n; n = n->pNext)
            if (n->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) { ri = (const VkPipelineRenderingCreateInfo*)n; break; }
        VkEmuLog("[vk-diag] vkCreateGraphicsPipelines[%u/%u] flags=0x%x renderPass=%p(orig=%p) layout=%p cb=%p ds=%p rs=%p ms=%p dyn=%p %s\n"
                 "[vk-diag]    rendering: colorCount=%u fmt0=%d depth=%d stencil=%d\n",
                 i, createInfoCount, (unsigned)ci.flags, (void*)ci.renderPass, (void*)pCreateInfos[i].renderPass,
                 (void*)ci.layout, (const void*)ci.pColorBlendState, (const void*)ci.pDepthStencilState,
                 (const void*)ci.pRasterizationState, (const void*)ci.pMultisampleState, (const void*)ci.pDynamicState,
                 stages.c_str(),
                 ri ? ri->colorAttachmentCount : 0u,
                 (ri && ri->colorAttachmentCount && ri->pColorAttachmentFormats) ? (int)ri->pColorAttachmentFormats[0] : -1,
                 ri ? (int)ri->depthAttachmentFormat : -1, ri ? (int)ri->stencilAttachmentFormat : -1);
    }
    VkResult gr = real(device, pipelineCache, createInfoCount, finalInfos, pAllocator, pPipelines);
    VkEmuLog("[vk-diag]    -> %d\n", (int)gr);
    return gr;
}

// 5. vkCreateInstance and vkEnumerateInstanceVersion
extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                  const VkAllocationCallbacks* pAllocator,
                  VkInstance* pInstance)
{
    EnsureRealLoaderLoaded();
    auto real_vkCreateInstance =
        (PFN_vkCreateInstance)g_real.GetInstanceProcAddr(nullptr, "vkCreateInstance");

    auto* ci = new VkInstanceCreateInfo(*pCreateInfo);
    auto* appInfo = new VkApplicationInfo{};
    appInfo->sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    if (ci->pApplicationInfo) *appInfo = *ci->pApplicationInfo;

    std::vector<char*> strdupedStrings;
    if (appInfo->pApplicationName) { appInfo->pApplicationName = _strdup(appInfo->pApplicationName); strdupedStrings.push_back((char*)appInfo->pApplicationName); }
    if (appInfo->pEngineName)      { appInfo->pEngineName      = _strdup(appInfo->pEngineName);      strdupedStrings.push_back((char*)appInfo->pEngineName); }

    uint32_t major = VK_API_VERSION_MAJOR(appInfo->apiVersion);
    uint32_t minor = VK_API_VERSION_MINOR(appInfo->apiVersion);
    if (major > 1 || (major == 1 && minor > 2))
        appInfo->apiVersion = VK_API_VERSION_1_2;
    ci->pApplicationInfo = appInfo;

    auto* enabledExts = new std::vector<const char*>();
    if (ci->enabledExtensionCount > 0) {
        enabledExts->assign(ci->ppEnabledExtensionNames, ci->ppEnabledExtensionNames + ci->enabledExtensionCount);
    }

    for (auto& ext : *enabledExts) {
        ext = _strdup(ext);
        strdupedStrings.push_back((char*)ext);
    }
    bool has_gpdp2 = false;
    for (auto& ext : *enabledExts) {
        if (strcmp(ext, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0) { has_gpdp2 = true; break; }
    }
    if (!has_gpdp2) {
        enabledExts->push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    }
    ci->enabledExtensionCount = (uint32_t)enabledExts->size();
    ci->ppEnabledExtensionNames = enabledExts->data();

    VkResult result = real_vkCreateInstance(ci, pAllocator, pInstance);
    if (result == VK_SUCCESS) {
        g_lastInstance.store(*pInstance, std::memory_order_release);
        {
            std::lock_guard<std::mutex> liveLock(g_liveInstancesMutex);
            g_liveInstances.push_back(*pInstance);
        }

        {
            std::lock_guard<std::mutex> allocLock(g_instanceAllocsMutex);
            g_instanceAllocs[*pInstance] = InstanceAllocs{ ci, appInfo, enabledExts, std::move(strdupedStrings) };
        }

        std::lock_guard<std::mutex> lock(g_resolveMutex);
        for (const char* fname : { "vkGetPhysicalDeviceProperties", "vkGetPhysicalDeviceProperties2" }) {
            if (!g_resolvedCache.count(fname)) {
                PFN_vkVoidFunction fn = g_real.GetInstanceProcAddr(*pInstance, fname);
                if (fn) g_resolvedCache[fname] = fn;
            }
        }
    } else {

        for (char* s : strdupedStrings) free(s);
        delete enabledExts;
        delete appInfo;
        delete ci;
    }
    return result;
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkEnumerateInstanceVersion(uint32_t* pApiVersion) {
    *pApiVersion = VKEMU_SPOOFED_API_VERSION;
    return VK_SUCCESS;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    EnsureRealLoaderLoaded();
    auto real_vkDestroyInstance =
        (PFN_vkDestroyInstance)g_real.GetInstanceProcAddr(instance, "vkDestroyInstance");
    if (real_vkDestroyInstance) {
        real_vkDestroyInstance(instance, pAllocator);
    } else {
        LogFatal("vkDestroyInstance");
    }

    {
        // Forget this instance; if it was the "last" one, fall back to another instance that is still alive
        // (instead of NULL, which made every instance-level call fail with "real driver does not expose ...").
        std::lock_guard<std::mutex> liveLock(g_liveInstancesMutex);
        g_liveInstances.erase(std::remove(g_liveInstances.begin(), g_liveInstances.end(), instance), g_liveInstances.end());
        if (g_lastInstance.load(std::memory_order_acquire) == instance) {
            g_lastInstance.store(g_liveInstances.empty() ? VK_NULL_HANDLE : g_liveInstances.back(),
                                 std::memory_order_release);
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_resolveMutex);
        g_resolvedCache.clear();
    }

    {
        std::lock_guard<std::mutex> lock(g_instanceAllocsMutex);
        auto it = g_instanceAllocs.find(instance);
        if (it != g_instanceAllocs.end()) {
            for (char* s : it->second.strdupedStrings) free(s);
            delete it->second.enabledExts;
            delete it->second.appInfo;
            delete it->second.ci;
            g_instanceAllocs.erase(it);
        }
    }
}


extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
                         const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface) {
    auto real = LazyResolve<PFN_vkCreateWin32SurfaceKHR>("vkCreateWin32SurfaceKHR");
    if (!real) { LogFatal("vkCreateWin32SurfaceKHR"); return VK_ERROR_INITIALIZATION_FAILED; }
    return real(instance, pCreateInfo, pAllocator, pSurface);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    auto real = LazyResolve<PFN_vkDestroyDevice>("vkDestroyDevice");
    if (!real) { LogFatal("vkDestroyDevice"); return; }
    {   // PATCH: purge the render pass / framebuffer caches of THIS device. Without this, a 2nd device
        // (VKD3D 2.11 creates two) receives a VkRenderPass from the already-destroyed 1st device -> driver crash
        // (nvoglv64.dll read 0xBF8 in vkCreateGraphicsPipelines).
        auto dFb = LazyResolve<PFN_vkDestroyFramebuffer>("vkDestroyFramebuffer");
        auto dRp = LazyResolve<PFN_vkDestroyRenderPass>("vkDestroyRenderPass");
        std::lock_guard<std::mutex> lk(g_cacheMutex);
        for (auto it = g_framebufferCache.begin(); it != g_framebufferCache.end();) {
            if (it->first.device == device) { if (dFb && it->second) dFb(device, it->second, nullptr); it = g_framebufferCache.erase(it); }
            else ++it;
        }
        for (auto it = g_renderPassCache.begin(); it != g_renderPassCache.end();) {
            if (it->first.device == device) { if (dRp && it->second) dRp(device, it->second, nullptr); it = g_renderPassCache.erase(it); }
            else ++it;
        }
    }
    real(device, pAllocator);
    {
        std::lock_guard<std::mutex> dl(g_deviceMutex);
        g_realDeviceProcAddr.erase(device);
    }
    VkDevice expected = device;
    g_lastDevice.compare_exchange_strong(expected, VK_NULL_HANDLE);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* pAllocator) {
    auto real = LazyResolve<PFN_vkDestroySurfaceKHR>("vkDestroySurfaceKHR");
    if (!real) { LogFatal("vkDestroySurfaceKHR"); return; }
    real(instance, surface, pAllocator);
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkEnumerateDeviceLayerProperties(VkPhysicalDevice physicalDevice, uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    auto real = LazyResolve<PFN_vkEnumerateDeviceLayerProperties>("vkEnumerateDeviceLayerProperties");
    if (!real) { LogFatal("vkEnumerateDeviceLayerProperties"); return VK_ERROR_INITIALIZATION_FAILED; }
    return real(physicalDevice, pPropertyCount, pProperties);
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkEnumerateInstanceExtensionProperties(const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    auto real = ResolveGlobal<PFN_vkEnumerateInstanceExtensionProperties>("vkEnumerateInstanceExtensionProperties");
    if (!real) { LogFatal("vkEnumerateInstanceExtensionProperties"); return VK_ERROR_INITIALIZATION_FAILED; }
    return real(pLayerName, pPropertyCount, pProperties);
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkEnumerateInstanceLayerProperties(uint32_t* pPropertyCount, VkLayerProperties* pProperties) {
    auto real = ResolveGlobal<PFN_vkEnumerateInstanceLayerProperties>("vkEnumerateInstanceLayerProperties");
    if (!real) { LogFatal("vkEnumerateInstanceLayerProperties"); return VK_ERROR_INITIALIZATION_FAILED; }
    return real(pPropertyCount, pProperties);
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount, VkPhysicalDevice* pPhysicalDevices) {
    auto real = LazyResolve<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
    if (!real) { LogFatal("vkEnumeratePhysicalDevices"); return VK_ERROR_INITIALIZATION_FAILED; }
    return real(instance, pPhysicalDeviceCount, pPhysicalDevices);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures* pFeatures) {
    auto real = LazyResolve<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures");
    if (!real) { LogFatal("vkGetPhysicalDeviceFeatures"); return; }
    real(physicalDevice, pFeatures);
    if (pFeatures) {
        // BeamNG (0.39+) requires sparseResidencyBuffer during its own compatibility
        // check (before vkCreateDevice). The GM107 (Maxwell 1.0, GTX 750 Ti) does NOT
        // really support this feature -- this is a pure facade lie to get past that
        // check. The bit is then explicitly set back to VK_FALSE in
        // vkCreateDevice before calling the real driver (see the related note there),
        // otherwise vkCreateDevice fails with VK_ERROR_FEATURE_NOT_PRESENT (the Nvidia
        // driver re-validates every requested feature against the real hardware).
        pFeatures->sparseResidencyBuffer = VK_TRUE;
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties* pFormatProperties) {
    auto real = LazyResolve<PFN_vkGetPhysicalDeviceFormatProperties>("vkGetPhysicalDeviceFormatProperties");
    if (!real) { LogFatal("vkGetPhysicalDeviceFormatProperties"); return; }
    real(physicalDevice, format, pFormatProperties);
}

// PATCH VKD3D 2.11: VkFormatProperties3 (VK_KHR_format_feature_flags2, core 1.3) is unknown to the
// 472.12 driver: the struct stayed zeroed -> vkd3d believed no format supported
// COLOR_ATTACHMENT etc. ("no format in cast list supports it" -> E_INVALIDARG on the swapchain,
// and "Mapping D24_UNORM_S8_UINT to D32_SFLOAT_S8_UINT"). We emulate it from VkFormatProperties.
static void EmulateFormatProperties3(VkFormat format, VkFormatProperties2* p) {
    for (VkBaseOutStructure* n = (VkBaseOutStructure*)p->pNext; n; n = n->pNext) {
        if (n->sType != VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) continue;
        auto* f3 = (VkFormatProperties3*)n;
        if (f3->linearTilingFeatures || f3->optimalTilingFeatures || f3->bufferFeatures) continue;  // natively filled
        const bool depth = (format >= VK_FORMAT_D16_UNORM && format <= VK_FORMAT_D32_SFLOAT) ||
                           (format >= VK_FORMAT_D16_UNORM_S8_UINT && format <= VK_FORMAT_D32_SFLOAT_S8_UINT);
        auto widen = [&](VkFormatFeatureFlags f) -> VkFormatFeatureFlags2 {
            VkFormatFeatureFlags2 r = (VkFormatFeatureFlags2)f;                       // bits 0..30 identical
            if (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)                               // READ/WRITE_WITHOUT_FORMAT
                r |= (VkFormatFeatureFlags2)0x80000000ull | (VkFormatFeatureFlags2)0x100000000ull;
            if (depth && (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))                    // SAMPLED_IMAGE_DEPTH_COMPARISON
                r |= (VkFormatFeatureFlags2)0x200000000ull;
            return r;
        };
        f3->linearTilingFeatures  = widen(p->formatProperties.linearTilingFeatures);
        f3->optimalTilingFeatures = widen(p->formatProperties.optimalTilingFeatures);
        f3->bufferFeatures        = (VkFormatFeatureFlags2)p->formatProperties.bufferFeatures;
        static bool logged = false;
        if (!logged) { logged = true; VkEmuLog("[vk-emu] VkFormatProperties3 emulated from VkFormatProperties (driver without VK_KHR_format_feature_flags2)\n"); }
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetPhysicalDeviceFormatProperties2(VkPhysicalDevice physicalDevice, VkFormat format, VkFormatProperties2* pFormatProperties) {
    auto real = LazyResolve<PFN_vkGetPhysicalDeviceFormatProperties2>("vkGetPhysicalDeviceFormatProperties2");
    if (!real) { LogFatal("vkGetPhysicalDeviceFormatProperties2"); return; }
    for (VkBaseOutStructure* n = (VkBaseOutStructure*)pFormatProperties->pNext; n; n = n->pNext)   // avoids reading uninitialized garbage
        if (n->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3) {
            auto* f3 = (VkFormatProperties3*)n;
            f3->linearTilingFeatures = 0; f3->optimalTilingFeatures = 0; f3->bufferFeatures = 0;
        }
    real(physicalDevice, format, pFormatProperties);
    EmulateFormatProperties3(format, pFormatProperties);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceMemoryProperties* pMemoryProperties) {
    auto real = LazyResolve<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties");
    if (!real) { LogFatal("vkGetPhysicalDeviceMemoryProperties"); return; }
    real(physicalDevice, pMemoryProperties);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount, VkQueueFamilyProperties* pQueueFamilyProperties) {
    auto real = LazyResolve<PFN_vkGetPhysicalDeviceQueueFamilyProperties>("vkGetPhysicalDeviceQueueFamilyProperties");
    if (!real) { LogFatal("vkGetPhysicalDeviceQueueFamilyProperties"); return; }
    real(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetPhysicalDeviceQueueFamilyProperties2(VkPhysicalDevice physicalDevice, uint32_t* pQueueFamilyPropertyCount, VkQueueFamilyProperties2* pQueueFamilyProperties) {
    auto real = LazyResolve<PFN_vkGetPhysicalDeviceQueueFamilyProperties2>("vkGetPhysicalDeviceQueueFamilyProperties2");
    if (!real) { LogFatal("vkGetPhysicalDeviceQueueFamilyProperties2"); return; }
    real(physicalDevice, pQueueFamilyPropertyCount, pQueueFamilyProperties);
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities) {
    auto real = LazyResolve<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>("vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    if (!real) { LogFatal("vkGetPhysicalDeviceSurfaceCapabilitiesKHR"); return VK_ERROR_INITIALIZATION_FAILED; }
    return real(physicalDevice, surface, pSurfaceCapabilities);
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats) {
    auto real = LazyResolve<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>("vkGetPhysicalDeviceSurfaceFormatsKHR");
    if (!real) { LogFatal("vkGetPhysicalDeviceSurfaceFormatsKHR"); return VK_ERROR_INITIALIZATION_FAILED; }
    return real(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pPresentModeCount, VkPresentModeKHR* pPresentModes) {
    auto real = LazyResolve<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>("vkGetPhysicalDeviceSurfacePresentModesKHR");
    if (!real) { LogFatal("vkGetPhysicalDeviceSurfacePresentModesKHR"); return VK_ERROR_INITIALIZATION_FAILED; }
    return real(physicalDevice, surface, pPresentModeCount, pPresentModes);
}

// --- 1. vkEnumerateDeviceExtensionProperties : we add our extensions to the supported list
extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName,
                                      uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    auto real = LazyResolve<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    if (!real) { LogFatal("vkEnumerateDeviceExtensionProperties"); return VK_ERROR_INITIALIZATION_FAILED; }

    if (pLayerName != nullptr)
        return real(physicalDevice, pLayerName, pPropertyCount, pProperties);

    uint32_t realCount = 0;
    VkResult res = real(physicalDevice, nullptr, &realCount, nullptr);
    if (res != VK_SUCCESS) return res;

    std::vector<VkExtensionProperties> realProps(realCount);
    res = real(physicalDevice, nullptr, &realCount, realProps.data());
    if (res != VK_SUCCESS && res != VK_INCOMPLETE) return res;

    realProps.erase(
        std::remove_if(realProps.begin(), realProps.end(),
            [](const VkExtensionProperties& p) { return IsBlockedDeviceExtension(p.extensionName); }),
        realProps.end());

    // We only overwrite the caps if the driver don't support them (our implementation may introduce bugs)
    std::vector<VkExtensionProperties> merged = realProps;
    for (uint32_t i = 0; i < g_fakedDeviceExtensionCount; i++) {
        bool already = false;
        for (auto& p : realProps)
            if (strcmp(p.extensionName, g_fakedDeviceExtensions[i]) == 0) { already = true; break; }
        if (already) continue;

        VkExtensionProperties fake{};
        strncpy(fake.extensionName, g_fakedDeviceExtensions[i], VK_MAX_EXTENSION_NAME_SIZE - 1);
        fake.specVersion = 1;
        merged.push_back(fake);
    }

    if (!pProperties) {
        *pPropertyCount = (uint32_t)merged.size();
        return VK_SUCCESS;
    }

    uint32_t toCopy = (*pPropertyCount < merged.size()) ? *pPropertyCount : (uint32_t)merged.size();
    for (uint32_t i = 0; i < toCopy; i++) pProperties[i] = merged[i];
    *pPropertyCount = toCopy;
    return (toCopy < merged.size()) ? VK_INCOMPLETE : VK_SUCCESS;
}

// --- 2. vkGetPhysicalDeviceFeatures2 : we force dynamicRendering=VK_TRUE in the pNext chain if the app ask for it
extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures) {
    auto real = LazyResolve<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
    VkEmuLog("[vk-emu] DIAGNOSTIC vkGetPhysicalDeviceFeatures2 CALLED, real=%p, pFeatures=%p\n",
            (void*)real, (void*)pFeatures);
    /* log flushed in VkEmuLog */
    if (!real) { LogFatal("vkGetPhysicalDeviceFeatures2"); return; }

    real(physicalDevice, pFeatures);

    VkEmuLog("[vk-emu] DIAGNOSTIC after real(): sparseResidencyBuffer=%d (before patch)\n",
            (int)pFeatures->features.sparseResidencyBuffer);
    /* log flushed in VkEmuLog */

    // BeamNG (0.39+) requires sparseResidencyBuffer on the base VkPhysicalDeviceFeatures
    // (same structure as vkGetPhysicalDeviceFeatures, reachable here via ->features).
    // Pure facade lie -- not supported by the GM107, set back to VK_FALSE before the real
    // vkCreateDevice (see the related note there).
    pFeatures->features.sparseResidencyBuffer = VK_TRUE;

    VkEmuLog("[vk-emu] DIAGNOSTIC after patch: sparseResidencyBuffer=%d (must be 1)\n",
            (int)pFeatures->features.sparseResidencyBuffer);
    /* log flushed in VkEmuLog */

    VkBaseOutStructure* cur = (VkBaseOutStructure*)pFeatures->pNext;
    while (cur) {
        if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* f = (VkPhysicalDeviceVulkan12Features*)cur;
            f->imagelessFramebuffer = VK_TRUE; // Needed for dynamic rendering
        } 
        else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR) {
            // Optional, requested by BeamNG. No simple hardware equivalent on Maxwell/Kepler,
            // but we advertise support: the engine does not necessarily depend heavily on it if it is optional.
            ((VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR*)cur)->fragmentShaderBarycentric = VK_TRUE;
        }
        else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT) {
            ((VkPhysicalDevicePageableDeviceLocalMemoryFeaturesEXT*)cur)->pageableDeviceLocalMemory = VK_TRUE;
        }
        else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            auto* f = (VkPhysicalDeviceVulkan13Features*)cur;
            f->robustImageAccess = VK_TRUE;                  
            f->inlineUniformBlock = VK_FALSE;                 
            f->descriptorBindingInlineUniformBlockUpdateAfterBind = VK_FALSE;
            f->pipelineCreationCacheControl = VK_TRUE;        
            f->privateData = VK_TRUE;                         
            f->shaderDemoteToHelperInvocation = VK_FALSE;     
            f->shaderTerminateInvocation = VK_TRUE;           
            f->subgroupSizeControl = VK_FALSE;                
            f->computeFullSubgroups = VK_FALSE;
            f->synchronization2 = VK_TRUE;
            f->textureCompressionASTC_HDR = VK_FALSE;         
            f->shaderZeroInitializeWorkgroupMemory = VK_TRUE; 
            f->dynamicRendering = VK_TRUE;
            f->shaderIntegerDotProduct = VK_TRUE;             
            f->maintenance4 = VK_TRUE;                        
        } 
        else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES) {
            auto* f = (VkPhysicalDeviceVulkan14Features*)cur;
            f->globalPriorityQuery = VK_FALSE;
            f->shaderSubgroupRotate = VK_FALSE;
            f->shaderSubgroupRotateClustered = VK_FALSE;
            f->shaderFloatControls2 = VK_FALSE;
            f->shaderExpectAssume = VK_FALSE;
            f->rectangularLines = VK_FALSE;
            f->bresenhamLines = VK_TRUE;
            f->smoothLines = VK_FALSE;
            f->stippledRectangularLines = VK_FALSE;
            f->stippledBresenhamLines = VK_TRUE;
            f->stippledSmoothLines = VK_FALSE;
            f->vertexAttributeInstanceRateDivisor = VK_FALSE;
            f->vertexAttributeInstanceRateZeroDivisor = VK_FALSE;
            f->indexTypeUint8 = VK_FALSE;
            f->dynamicRenderingLocalRead = VK_FALSE;
            f->maintenance5 = VK_TRUE;
            f->maintenance6 = VK_FALSE;
            f->pipelineProtectedAccess = VK_FALSE;
            f->pipelineRobustness = VK_FALSE;
            f->hostImageCopy = VK_TRUE;
            f->pushDescriptor = VK_TRUE;
        } 
        else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES) {
            ((VkPhysicalDeviceDynamicRenderingFeatures*)cur)->dynamicRendering = VK_TRUE;
        }
        else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES) {
            ((VkPhysicalDeviceSynchronization2Features*)cur)->synchronization2 = VK_TRUE;
        }
        else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_FEATURES) {
            ((VkPhysicalDeviceMaintenance4Features*)cur)->maintenance4 = VK_TRUE;
        }
        else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES) {
            ((VkPhysicalDeviceMaintenance5Features*)cur)->maintenance5 = VK_TRUE;
        }
        else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES) {
            ((VkPhysicalDeviceHostImageCopyFeatures*)cur)->hostImageCopy = VK_TRUE;
        }
        else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT) {
            auto* f = (VkPhysicalDeviceExtendedDynamicState3FeaturesEXT*)cur;
            size_t offset = offsetof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3TessellationDomainOrigin);
            memset((char*)f + offset, 0xFF, sizeof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT) - offset);
        }
        else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT) {
            ((VkPhysicalDeviceExtendedDynamicStateFeaturesEXT*)cur)->extendedDynamicState = VK_TRUE;
        }
        else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT) {
            auto* f = (VkPhysicalDeviceExtendedDynamicState2FeaturesEXT*)cur;
            f->extendedDynamicState2 = VK_TRUE;
            f->extendedDynamicState2LogicOp = VK_TRUE;
            f->extendedDynamicState2PatchControlPoints = VK_TRUE;
        }
        cur = cur->pNext;
    }
}

// 3. vkCreateDevice : we remove the wrong functions before forwarding them, otherwise the driver will give this error: VK_ERROR_EXTENSION_NOT_PRESENT
extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
               const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    auto real = LazyResolve<PFN_vkCreateDevice>("vkCreateDevice");
    if (!real) { LogFatal("vkCreateDevice"); return VK_ERROR_INITIALIZATION_FAILED; }

    VkDeviceCreateInfo ci = *pCreateInfo;
    DeviceState state;

    bool real_has_dr = RealDeviceHasExtension(physicalDevice, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    bool real_has_sync2 = RealDeviceHasExtension(physicalDevice, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    bool real_has_pd = RealDeviceHasExtension(physicalDevice, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    bool real_has_rp2 = RealDeviceHasExtension(physicalDevice, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
    bool real_has_imgless_fb = RealDeviceHasExtension(physicalDevice, VK_KHR_IMAGELESS_FRAMEBUFFER_EXTENSION_NAME);
    VkEmuLog("[vk-emu] DIAGNOSTIC push_descriptor: real_has_pd=%d, extensions requested by the app (%u):\n",
            (int)real_has_pd, pCreateInfo->enabledExtensionCount);
    for (uint32_t di = 0; di < pCreateInfo->enabledExtensionCount; di++) {
        VkEmuLog("  [app-requested] -> %s\n", pCreateInfo->ppEnabledExtensionNames[di]);
    }
    /* log flushed in VkEmuLog */
    bool real_has_eds = RealDeviceHasExtension(physicalDevice, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    bool real_has_eds2 = RealDeviceHasExtension(physicalDevice, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);

    bool real_has_bda = RealDeviceHasExtension(physicalDevice, VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    bool real_has_eds3 = RealDeviceHasExtension(physicalDevice, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    bool real_has_hic = RealDeviceHasExtension(physicalDevice, VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME);
    bool real_has_m5 = RealDeviceHasExtension(physicalDevice, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    bool real_has_vids = RealDeviceHasExtension(physicalDevice, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    bool real_has_mesh = RealDeviceHasExtension(physicalDevice, VK_EXT_MESH_SHADER_EXTENSION_NAME);

    VkPhysicalDeviceFeatures2 realFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features real12Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceDescriptorIndexingFeatures realDescIndexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    realFeatures.pNext = &real12Features;
    real12Features.pNext = &realDescIndexing;
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT realEds2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    if (real_has_eds2) realDescIndexing.pNext = (void*)&realEds2;
    
    auto realGetFeatures2 = LazyResolve<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
    if (realGetFeatures2) {
        realGetFeatures2(physicalDevice, &realFeatures);
    } else {
        auto realGetFeatures = LazyResolve<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures");
        if (realGetFeatures) realGetFeatures(physicalDevice, &realFeatures.features);
    }

    // IMPORTANT -- FIX: we NO LONGER force sparseResidencyBuffer=VK_TRUE here.
    // sparseResidencyBuffer requires hardware sparseBinding; the GM107 (GTX 750 Ti,
    // Maxwell 1.0) does not support it, and the Nvidia driver REFUSES vkCreateDevice
    // (VK_ERROR_FEATURE_NOT_PRESENT) as soon as it is asked to enable this bit, whatever we
    // return from vkGetPhysicalDeviceFeatures/Features2 upstream -- vkCreateDevice re-validates
    // every requested feature against the REAL HARDWARE, so that lie never "passes".
    // We therefore keep the spoof only for the QUERY phase (vkGetPhysicalDeviceFeatures/
    // Features2, shown earlier in this file) so that the engine believes the feature is
    // available and does not block its own upstream compatibility check -- then we
    // force the bit to VK_FALSE right before the real vkCreateDevice, whatever the game asked for,
    // so that creation really succeeds. If BeamNG crashes further on because it
    // actually uses sparse buffers, sparseResidencyBuffer will have to be emulated differently
    // (regular non-sparse buffers) rather than spoofed here.
    realFeatures.features.sparseResidencyBuffer = VK_FALSE;


    if (ci.pEnabledFeatures) {
        VkBool32* appF = (VkBool32*)ci.pEnabledFeatures;
        VkBool32* realF = (VkBool32*)&realFeatures.features;
        size_t featSize = sizeof(VkPhysicalDeviceFeatures);
        for (size_t i = 0; i < featSize / sizeof(VkBool32); i++) {
            if (appF[i] && !realF[i]) appF[i] = VK_FALSE;
        }
    }

    std::vector<const char*> filteredExt;
    for (uint32_t i = 0; i < ci.enabledExtensionCount; i++) {
        const char* ext = ci.ppEnabledExtensionNames[i];


        if (IsBlockedDeviceExtension(ext)) {
            VkEmuLog("[vk-emu] Extension explicitly masked, filtering: %s\n", ext);
            continue;
        }

        if (strcmp(ext, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0 && real_has_pd) {
            filteredExt.push_back(ext);
            continue;
        }

        bool is_faked = false;
        for (uint32_t j = 0; j < g_fakedDeviceExtensionCount; j++) {
            if (strcmp(ext, g_fakedDeviceExtensions[j]) == 0) {
                is_faked = true;
                break;
            }
        }
        if (is_faked) {
            // PATCH VKD3D 2.7: if the real driver natively supports EDS1/EDS2, we
            // enable it for real (otherwise vkGetDeviceProcAddr returns NULL for the
            // *EXT functions and the wrappers silently become no-ops).
            bool nativeOk =
                (strcmp(ext, VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME)   == 0 && real_has_eds)  ||
                (strcmp(ext, VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME) == 0 && real_has_eds2);
            if (nativeOk) {
                VkEmuLog("[vk-emu] Driver native extension enabled: %s\n", ext);
                filteredExt.push_back(ext);
                continue;
            }
            if (strcmp(ext, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0) state.emu_sync2 = true;
            if (strcmp(ext, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0) state.emu_push_desc = true;
            continue;
        }


        if (!RealDeviceHasExtension(physicalDevice, ext)) {
            VkEmuLog("[vk-emu] Extension not supported by the driver, filtering: %s\n", ext);
            continue;
        }

        filteredExt.push_back(ext);
    }
    // PATCH 2.9: a Vulkan 1.3 app (recent vkd3d-proton) no longer needs to list
    // VK_EXT_extended_dynamic_state{,2}: these functions are core 1.3. Since we advertise 1.3
    // but the real driver is 1.2, WE must enable the real extension,
    // otherwise vkGetDeviceProcAddr("vkCmdSet...EXT") returns NULL and the wrappers become no-ops.
    bool real_has_cc2 = RealDeviceHasExtension(physicalDevice, VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME);
    VkEmuLog("[vk-emu] DIAGNOSTIC EDS: real_has_eds=%d real_has_eds2=%d real_has_copy_commands2=%d\n",
             (int)real_has_eds, (int)real_has_eds2, (int)real_has_cc2);
    auto addExtIfMissing = [&](const char* name) {
        for (const char* e : filteredExt) if (strcmp(e, name) == 0) return;
        VkEmuLog("[vk-emu] Real extension force-enabled (emulated core 1.3): %s\n", name);
        filteredExt.push_back(name);
    };
    if (real_has_eds)  addExtIfMissing(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    if (real_has_eds2) addExtIfMissing(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
    if (real_has_cc2)  addExtIfMissing(VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME);

    ci.enabledExtensionCount = (uint32_t)filteredExt.size();
    ci.ppEnabledExtensionNames = filteredExt.empty() ? nullptr : filteredExt.data();


    auto stripFeature = [&](VkStructureType st, bool emu) {
        if (!emu) return;
        VkBaseOutStructure* p = (VkBaseOutStructure*)&ci;
        VkBaseOutStructure* c = (VkBaseOutStructure*)ci.pNext;
        while (c) {
            if (c->sType == st) {
                p->pNext = c->pNext;
                c = c->pNext;
            } else {
                p = c;
                c = c->pNext;
            }
        }
    };

    auto extEnabled = [&](const char* n) {
        for (const char* e : filteredExt) if (strcmp(e, n) == 0) return true;
        return false;
    };
    VkBaseOutStructure* newChainHead = nullptr;
    VkBaseOutStructure* newChainTail = nullptr;
    bool haveEdsStruct = false, haveEds2Struct = false;
    VkPhysicalDeviceExtendedDynamicStateFeaturesEXT  injEds{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT injEds2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
    
    VkBaseOutStructure* curr = (VkBaseOutStructure*)ci.pNext;
    while (curr) {
        VkBaseOutStructure* next = curr->pNext;
        bool keep = false;
        
         if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            auto* f = (VkPhysicalDeviceFeatures2*)curr;
            VkBool32* appF = (VkBool32*)&f->features;
            VkBool32* realF = (VkBool32*)&realFeatures.features;
            size_t featSize = sizeof(VkPhysicalDeviceFeatures);
            for (size_t i = 0; i < featSize / sizeof(VkBool32); i++) {
                if (appF[i] && !realF[i]) appF[i] = VK_FALSE;
            }
            // Explicit safety net: even if the game requests it, sparseResidencyBuffer
            // must NEVER reach the real vkCreateDevice on this GPU (see the note above,
            // realFeatures.features.sparseResidencyBuffer = VK_FALSE) -- the generic loop
            // above already handles it via realF, this documents/locks the intent.
            f->features.sparseResidencyBuffer = VK_FALSE;
            keep = true;
        }
        else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* f = (VkPhysicalDeviceVulkan12Features*)curr;
            size_t offset = offsetof(VkPhysicalDeviceVulkan12Features, imagelessFramebuffer);
            memcpy((char*)f + offset, (char*)&real12Features + offset, sizeof(VkPhysicalDeviceVulkan12Features) - offset);
            keep = true;
        } 
        else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
            auto* f = (VkPhysicalDeviceDescriptorIndexingFeatures*)curr;
            size_t offset = offsetof(VkPhysicalDeviceDescriptorIndexingFeatures, shaderInputAttachmentArrayDynamicIndexing);
            memcpy((char*)f + offset, (char*)&realDescIndexing + offset, sizeof(VkPhysicalDeviceDescriptorIndexingFeatures) - offset);
            keep = true;
        }
        else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES) {
            auto* f = (VkPhysicalDeviceBufferDeviceAddressFeatures*)curr;
            f->bufferDeviceAddress = real12Features.bufferDeviceAddress;
            f->bufferDeviceAddressCaptureReplay = real12Features.bufferDeviceAddressCaptureReplay;
            f->bufferDeviceAddressMultiDevice = real12Features.bufferDeviceAddressMultiDevice;
            keep = true;
        }
        else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES) {
            ((VkPhysicalDeviceImagelessFramebufferFeatures*)curr)->imagelessFramebuffer = real12Features.imagelessFramebuffer;
            keep = true;
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            ((VkPhysicalDeviceTimelineSemaphoreFeatures*)curr)->timelineSemaphore = real12Features.timelineSemaphore;
            keep = true;
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES) {
            keep = true; 
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES) {
            keep = true; 
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES) {
            keep = true; 
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES) {
            ((VkPhysicalDeviceScalarBlockLayoutFeatures*)curr)->scalarBlockLayout = real12Features.scalarBlockLayout;
            keep = true;
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            keep = false;
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES) {
            keep = false;
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES) {
            keep = real_has_sync2;
            if (keep) ((VkPhysicalDeviceSynchronization2Features*)curr)->synchronization2 = VK_TRUE;
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES) {
            keep = false;
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT) {
            keep = real_has_eds;
            haveEdsStruct = keep;
            if (keep) ((VkPhysicalDeviceExtendedDynamicStateFeaturesEXT*)curr)->extendedDynamicState = VK_TRUE;
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT) {
            keep = real_has_eds2;
            haveEds2Struct = keep;
            if (keep) {
                auto* f = (VkPhysicalDeviceExtendedDynamicState2FeaturesEXT*)curr;
                f->extendedDynamicState2 = VK_TRUE;
                f->extendedDynamicState2LogicOp = VK_TRUE;
                f->extendedDynamicState2PatchControlPoints = VK_TRUE;
            }
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT) {
            keep = real_has_eds3;
            if (keep) {
                auto* f = (VkPhysicalDeviceExtendedDynamicState3FeaturesEXT*)curr;
                size_t offset = offsetof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3TessellationDomainOrigin);
                memset((char*)f + offset, 0xFF, sizeof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT) - offset);
            }
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES) {
            keep = real_has_hic;
            if (keep) ((VkPhysicalDeviceHostImageCopyFeatures*)curr)->hostImageCopy = VK_TRUE;
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES) {
            keep = real_has_m5;
            if (keep) ((VkPhysicalDeviceMaintenance5Features*)curr)->maintenance5 = VK_TRUE;
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT) {
            keep = real_has_vids;
            if (keep) ((VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT*)curr)->vertexInputDynamicState = VK_TRUE;
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT) {
            keep = real_has_mesh;
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR) {
            // FIX: do NOT pass this feature to the real vkCreateDevice.
            // The GM107 (Maxwell 1.0, GTX 750 Ti) has no hardware support for
            // barycentric interpolation -- even if we advertise TRUE in
            // vkGetPhysicalDeviceFeatures2 to satisfy the engine's "optional" pre-check
            // (facade, see above), actually requesting this feature
            // here makes the REAL vkCreateDevice fail with VK_ERROR_FEATURE_NOT_PRESENT
            // (the Nvidia driver re-validates every feature against the real hardware, as
            // for sparseResidencyBuffer). So we drop the struct from the chain (keep
            // stays false) rather than forcing the bit to VK_FALSE: the corresponding
            // extension is already removed from ppEnabledExtensionNames
            // via g_fakedDeviceExtensions, so sending it would be inconsistent
            // with the real driver anyway.
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PAGEABLE_DEVICE_LOCAL_MEMORY_FEATURES_EXT) {
            // FIX: same reasoning as above for pageableDeviceLocalMemory.
        }
        // PATCH 2.10: these structs were dropped (keep=false by default) even though the real extension
        // is enabled -> robustness2/nullDescriptor, xfb, divisor... features were never enabled.
        // We keep them only if the matching extension is actually sent to the real driver.
        else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT) {
            keep = extEnabled(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT) {
            keep = extEnabled(VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME);
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_EXT) {
            keep = extEnabled(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT) {
            keep = extEnabled(VK_EXT_CONDITIONAL_RENDERING_EXTENSION_NAME);
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT) {
            keep = extEnabled(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
        }
        
        if (keep) {
            curr->pNext = nullptr;
            if (!newChainHead) {
                newChainHead = curr;
                newChainTail = curr;
            } else {
                newChainTail->pNext = curr;
                newChainTail = curr;
            }
        }
        curr = next;
    }
    auto appendFeat = [&](VkBaseOutStructure* st) {
        st->pNext = nullptr;
        if (!newChainHead) { newChainHead = st; newChainTail = st; }
        else { newChainTail->pNext = st; newChainTail = st; }
    };
    if (real_has_eds && !haveEdsStruct) {
        injEds.extendedDynamicState = VK_TRUE;
        appendFeat((VkBaseOutStructure*)&injEds);
        VkEmuLog("[vk-emu] Feature extendedDynamicState injected (missing from the app's chain)\n");
    }
    if (real_has_eds2 && !haveEds2Struct) {
        injEds2.extendedDynamicState2 = realEds2.extendedDynamicState2;
        injEds2.extendedDynamicState2LogicOp = realEds2.extendedDynamicState2LogicOp;
        injEds2.extendedDynamicState2PatchControlPoints = realEds2.extendedDynamicState2PatchControlPoints;
        appendFeat((VkBaseOutStructure*)&injEds2);
        VkEmuLog("[vk-emu] Feature extendedDynamicState2 injected (logicOp=%d patchCP=%d)\n",
                 (int)realEds2.extendedDynamicState2LogicOp, (int)realEds2.extendedDynamicState2PatchControlPoints);
    }
    ci.pNext = newChainHead;

    VkEmuLog("[vk-emu] Extensions sent to the driver:\n");
    for (uint32_t i = 0; i < ci.enabledExtensionCount; i++) {
        VkEmuLog("  -> %s\n", ci.ppEnabledExtensionNames[i]);
    }
    for (uint32_t i = 0; i < ci.enabledExtensionCount; i++)
        if (!strcmp(ci.ppEnabledExtensionNames[i], "VK_EXT_shader_viewport_index_layer"))
            g_layerExtEnabled.store(true, std::memory_order_release);
    /* log flushed in VkEmuLog */

    VkResult res = real(physicalDevice, &ci, pAllocator, pDevice);
    if (res != VK_SUCCESS) {
        VkEmuLog("[vk-emu] vkCreateDevice failed with error: %d\n", res);
        /* log flushed in VkEmuLog */
    } else {
        PFN_vkGetDeviceProcAddr realDevProcAddr =
            (PFN_vkGetDeviceProcAddr)g_real.GetInstanceProcAddr(g_lastInstance, "vkGetDeviceProcAddr");
        {
            std::lock_guard<std::mutex> lock(g_deviceMutex);
            g_realDeviceProcAddr[*pDevice] = realDevProcAddr;
        }
        g_lastDevice.store(*pDevice, std::memory_order_release);

        auto resolveDev = [&](const char* name) -> PFN_vkVoidFunction {
            if (!realDevProcAddr) return nullptr;
            return realDevProcAddr(*pDevice, name);
        };
        {   // a negative cache from a previous device (e.g. a test device) must not survive
            std::lock_guard<std::mutex> rl(g_resolveMutex);
            for (auto it = g_resolvedCache.begin(); it != g_resolvedCache.end();) {
                if (!it->second) it = g_resolvedCache.erase(it); else ++it;
            }
        }
        VkEmuLog("[vk-emu] DIAGNOSTIC EDS runtime: vkCmdSetViewportWithCountEXT=%p vkCmdBindVertexBuffers2EXT=%p vkCmdCopyBuffer2KHR=%p\n",
                 (void*)resolveDev("vkCmdSetViewportWithCountEXT"), (void*)resolveDev("vkCmdBindVertexBuffers2EXT"),
                 (void*)resolveDev("vkCmdCopyBuffer2KHR"));
        VkEmuLog("[vk-emu] DIAGNOSTIC sync2: real_has_sync2=%d\n", (int)real_has_sync2);
        if (real_has_sync2) {
            state.fn_CmdPipelineBarrier2 = (PFN_vkCmdPipelineBarrier2)resolveDev("vkCmdPipelineBarrier2");
            state.fn_QueueSubmit2        = (PFN_vkQueueSubmit2)resolveDev("vkQueueSubmit2");
            state.fn_CmdWriteTimestamp2  = (PFN_vkCmdWriteTimestamp2)resolveDev("vkCmdWriteTimestamp2");
            state.fn_CmdSetEvent2        = (PFN_vkCmdSetEvent2)resolveDev("vkCmdSetEvent2");
            state.fn_CmdResetEvent2      = (PFN_vkCmdResetEvent2)resolveDev("vkCmdResetEvent2");
            state.fn_CmdWaitEvents2      = (PFN_vkCmdWaitEvents2)resolveDev("vkCmdWaitEvents2");

            state.sync2_native = state.fn_CmdPipelineBarrier2 && state.fn_QueueSubmit2 &&
                                  state.fn_CmdWriteTimestamp2 && state.fn_CmdSetEvent2 &&
                                  state.fn_CmdResetEvent2 && state.fn_CmdWaitEvents2;

            VkEmuLog("[vk-emu] DIAGNOSTIC sync2: CmdPipelineBarrier2=%p QueueSubmit2=%p "
                            "CmdWriteTimestamp2=%p CmdSetEvent2=%p CmdResetEvent2=%p CmdWaitEvents2=%p "
                            "=> sync2_native=%d\n",
                            (void*)state.fn_CmdPipelineBarrier2, (void*)state.fn_QueueSubmit2,
                            (void*)state.fn_CmdWriteTimestamp2, (void*)state.fn_CmdSetEvent2,
                            (void*)state.fn_CmdResetEvent2, (void*)state.fn_CmdWaitEvents2,
                            (int)state.sync2_native);

            if (!state.sync2_native) {
                VkEmuLog("[vk-emu] WARNING: VK_KHR_synchronization2 advertised by the "
                                "driver but at least one function is missing -- fully switching to emulation.\n");
            }
        }
        /* log flushed in VkEmuLog */
        
        state.fn_CmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)resolveDev("vkCmdPipelineBarrier");
        state.fn_QueueSubmit        = (PFN_vkQueueSubmit)resolveDev("vkQueueSubmit");
        state.fn_CmdWriteTimestamp  = (PFN_vkCmdWriteTimestamp)resolveDev("vkCmdWriteTimestamp");
        state.fn_CmdSetEvent        = (PFN_vkCmdSetEvent)resolveDev("vkCmdSetEvent");
        state.fn_CmdResetEvent      = (PFN_vkCmdResetEvent)resolveDev("vkCmdResetEvent");
        state.fn_CmdWaitEvents      = (PFN_vkCmdWaitEvents)resolveDev("vkCmdWaitEvents");

        // --- VK_KHR_create_renderpass2 ---
        // We ALWAYS resolve the legacy fallback (core 1.0, guaranteed present) before deciding.
        state.fn_CreateRenderPassLegacy   = (PFN_vkCreateRenderPass)resolveDev("vkCreateRenderPass");
        state.fn_CmdBeginRenderPassLegacy = (PFN_vkCmdBeginRenderPass)resolveDev("vkCmdBeginRenderPass");
        state.fn_CmdNextSubpassLegacy     = (PFN_vkCmdNextSubpass)resolveDev("vkCmdNextSubpass");
        state.fn_CmdEndRenderPassLegacy   = (PFN_vkCmdEndRenderPass)resolveDev("vkCmdEndRenderPass");

        if (real_has_rp2) {
            state.fn_CreateRenderPass2   = (PFN_vkCreateRenderPass2)resolveDev("vkCreateRenderPass2");
            state.fn_CmdBeginRenderPass2 = (PFN_vkCmdBeginRenderPass2)resolveDev("vkCmdBeginRenderPass2");
            state.fn_CmdNextSubpass2     = (PFN_vkCmdNextSubpass2)resolveDev("vkCmdNextSubpass2");
            state.fn_CmdEndRenderPass2   = (PFN_vkCmdEndRenderPass2)resolveDev("vkCmdEndRenderPass2");

            state.rp2_native = state.fn_CreateRenderPass2 && state.fn_CmdBeginRenderPass2 &&
                                state.fn_CmdEndRenderPass2;

            if (!state.rp2_native) {
                VkEmuLog("[vk-emu] WARNING: VK_KHR_create_renderpass2 advertised by the "
                                "driver but function missing -- falling back to legacy renderpass (core 1.0).\n");
            }
        } else {
            VkEmuLog("[vk-emu] VK_KHR_create_renderpass2 not supported by this GPU -- "
                            "dynamic_rendering emulation switched to legacy renderpass (core 1.0).\n");
        }

        state.imageless_fb_native = real_has_imgless_fb;
        if (!state.imageless_fb_native) {
            VkEmuLog("[vk-emu] VK_KHR_imageless_framebuffer not supported by this GPU -- "
                            "framebuffers will be explicitly bound to the image views (no rebind at begin).\n");
        }
        /* log flushed in VkEmuLog */

        // --- push_descriptor ---
        if (real_has_pd) {
            state.fn_CmdPushDescriptorSetKHR =
                (PFN_vkCmdPushDescriptorSetKHR)resolveDev("vkCmdPushDescriptorSetKHR");
            state.push_descriptor_native = (state.fn_CmdPushDescriptorSetKHR != nullptr);

            if (!state.push_descriptor_native) {
                VkEmuLog("[vk-emu] WARNING: VK_KHR_push_descriptor advertised by the "
                                "driver but function missing -- falling back to emulation.\n");
            }
        }

        std::lock_guard<std::mutex> lock(g_deviceStateMutex);
        g_deviceState[*pDevice] = state;
    }
    return res;
}


// Stubs/experimental implementations

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdSetLineStipple(VkCommandBuffer commandBuffer, uint32_t lineStippleFactor, uint16_t lineStipplePattern) {
    using PFN = PFN_vkCmdSetLineStipple;
    if (PFN real = LazyResolve<PFN>("vkCmdSetLineStipple")) {
        real(commandBuffer, lineStippleFactor, lineStipplePattern);
        return;
    }
    // fallback
    using PFN_Ext = PFN_vkCmdSetLineStippleEXT;
    if (PFN_Ext ext = LazyResolve<PFN_Ext>("vkCmdSetLineStippleEXT")) {
        ext(commandBuffer, lineStippleFactor, lineStipplePattern);
        return;
    }
    LogStubCall("vkCmdSetLineStipple");

}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdBindIndexBuffer2(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                       VkDeviceSize size, VkIndexType indexType) {

    using PFN = PFN_vkCmdBindIndexBuffer2;
    if (PFN real = LazyResolve<PFN>("vkCmdBindIndexBuffer2")) {
        real(commandBuffer, buffer, offset, size, indexType);
        return;
    }
    using PFN_Khr = PFN_vkCmdBindIndexBuffer2KHR;
    if (PFN_Khr khr = LazyResolve<PFN_Khr>("vkCmdBindIndexBuffer2KHR")) {
        khr(commandBuffer, buffer, offset, size, indexType);
        return;
    }
    LogStubCall("vkCmdBindIndexBuffer2 (1.0 fallback without 'size' -- VK_KHR_maintenance5 missing from the driver)");
    using PFN_Legacy = PFN_vkCmdBindIndexBuffer;
    if (PFN_Legacy legacy = LazyResolve<PFN_Legacy>("vkCmdBindIndexBuffer"))
        legacy(commandBuffer, buffer, offset, indexType);
}

struct DescSetLayoutBindingInfo {
    uint32_t binding;
    VkDescriptorType descriptorType;
    uint32_t descriptorCount;
};

struct DescSetLayoutInfo {
    std::vector<DescSetLayoutBindingInfo> bindings;
};
static std::mutex g_descSetLayoutInfoMutex;
static std::unordered_map<VkDescriptorSetLayout, DescSetLayoutInfo> g_descSetLayoutInfo;

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
                             const VkAllocationCallbacks* pAllocator, VkDescriptorSetLayout* pSetLayout) {
    auto real = LazyResolve<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;

    VkDescriptorSetLayoutCreateInfo modifiedCi = *pCreateInfo;
    DeviceState devState;
    
    if (GetDeviceState(device, &devState) && !devState.push_descriptor_native) {
        if (modifiedCi.flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) {
            modifiedCi.flags &= ~VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        }
    }

    VkEmuLog("[vk-diag] vkCreateDescriptorSetLayout flags=0x%x bindings=%u pNext=%p\n",
             (unsigned)modifiedCi.flags, modifiedCi.bindingCount, (const void*)modifiedCi.pNext);
    for (uint32_t i = 0; i < modifiedCi.bindingCount; i++) {
        const auto& b = modifiedCi.pBindings[i];
        VkEmuLog("[vk-diag]    binding=%u type=%d count=%u stages=0x%x immutableSamplers=%p (sampler0=%p)\n",
                 b.binding, (int)b.descriptorType, b.descriptorCount, (unsigned)b.stageFlags,
                 (const void*)b.pImmutableSamplers,
                 b.pImmutableSamplers ? (void*)b.pImmutableSamplers[0] : nullptr);
    }
    VkResult res = real(device, &modifiedCi, pAllocator, pSetLayout);
    if (res == VK_SUCCESS) {
        DescSetLayoutInfo info;
        info.bindings.reserve(pCreateInfo->bindingCount);
        for (uint32_t i = 0; i < pCreateInfo->bindingCount; i++) {
            info.bindings.push_back({
                pCreateInfo->pBindings[i].binding,
                pCreateInfo->pBindings[i].descriptorType,
                pCreateInfo->pBindings[i].descriptorCount
            });
        }

        std::lock_guard<std::mutex> lock(g_descSetLayoutInfoMutex);
        g_descSetLayoutInfo[*pSetLayout] = std::move(info);
    }
    return res;
}

static std::mutex g_pipelineLayoutSetsMutex;
static std::unordered_map<VkPipelineLayout, std::vector<VkDescriptorSetLayout>> g_pipelineLayoutSets;

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* pCreateInfo,
                        const VkAllocationCallbacks* pAllocator, VkPipelineLayout* pPipelineLayout) {
    auto real = LazyResolve<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;

    VkEmuLog("[vk-diag] vkCreatePipelineLayout sets=%u pushRanges=%u\n", pCreateInfo->setLayoutCount, pCreateInfo->pushConstantRangeCount);
    for (uint32_t i = 0; i < pCreateInfo->setLayoutCount; i++)
        VkEmuLog("[vk-diag]    set[%u]=%p\n", i, (void*)pCreateInfo->pSetLayouts[i]);
    for (uint32_t i = 0; i < pCreateInfo->pushConstantRangeCount; i++)
        VkEmuLog("[vk-diag]    push[%u] stages=0x%x offset=%u size=%u\n", i,
                 (unsigned)pCreateInfo->pPushConstantRanges[i].stageFlags,
                 pCreateInfo->pPushConstantRanges[i].offset, pCreateInfo->pPushConstantRanges[i].size);
    VkResult res = real(device, pCreateInfo, pAllocator, pPipelineLayout);
    if (res == VK_SUCCESS) {
        std::vector<VkDescriptorSetLayout> sets(pCreateInfo->pSetLayouts,
            pCreateInfo->pSetLayouts + pCreateInfo->setLayoutCount);

        std::lock_guard<std::mutex> lock(g_pipelineLayoutSetsMutex);
        g_pipelineLayoutSets[*pPipelineLayout] = std::move(sets);
    }
    return res;
}

struct MemoryMapState { void* ptr; VkDeviceSize mappedOffset; };
static std::mutex g_memoryMapMutex;
static std::unordered_map<VkDeviceMemory, MemoryMapState> g_memoryMapState;

struct BufferBinding { VkDeviceMemory memory; VkDeviceSize offset; };
static std::mutex g_bufferBindingMutex;
static std::unordered_map<VkBuffer, BufferBinding> g_bufferBinding;
static std::unordered_multimap<VkDeviceMemory, VkBuffer> g_memoryToBuffers;

static std::mutex g_activeMappingsMutex;
static std::unordered_map<VkBuffer, void*> g_activeMappings;

static void RefreshActiveMappingsForMemory(VkDeviceMemory memory) {
    void* basePtr = nullptr;
    VkDeviceSize mappedOffset = 0;
    {
        std::lock_guard<std::mutex> lock(g_memoryMapMutex);
        auto it = g_memoryMapState.find(memory);
        if (it != g_memoryMapState.end()) { basePtr = it->second.ptr; mappedOffset = it->second.mappedOffset; }
    }

    std::lock_guard<std::mutex> lockBind(g_bufferBindingMutex);
    std::lock_guard<std::mutex> lockActive(g_activeMappingsMutex);
    auto range = g_memoryToBuffers.equal_range(memory);
    for (auto it = range.first; it != range.second; ++it) {
        VkBuffer buf = it->second;
        auto bindIt = g_bufferBinding.find(buf);
        if (bindIt == g_bufferBinding.end()) continue;

        if (basePtr) {
            VkDeviceSize relative = bindIt->second.offset - mappedOffset;
            g_activeMappings[buf] = (uint8_t*)basePtr + relative;
        } else {
            g_activeMappings.erase(buf);
        }
    }
}

static void TrackBufferBinding(VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize offset) {
    {
        std::lock_guard<std::mutex> lock(g_bufferBindingMutex);
        g_bufferBinding[buffer] = { memory, offset };
        g_memoryToBuffers.insert({ memory, buffer });
    }
    RefreshActiveMappingsForMemory(memory);
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkBindBufferMemory(VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    auto real = LazyResolve<PFN_vkBindBufferMemory>("vkBindBufferMemory");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = real(device, buffer, memory, memoryOffset);
    if (res == VK_SUCCESS) TrackBufferBinding(buffer, memory, memoryOffset);
    return res;
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkBindBufferMemory2(VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo* pBindInfos) {
    auto real = LazyResolve<PFN_vkBindBufferMemory2>("vkBindBufferMemory2");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = real(device, bindInfoCount, pBindInfos);
    if (res == VK_SUCCESS) {
        for (uint32_t i = 0; i < bindInfoCount; i++)
            TrackBufferBinding(pBindInfos[i].buffer, pBindInfos[i].memory, pBindInfos[i].memoryOffset);
    }
    return res;
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkMapMemory(VkDevice device, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
            VkMemoryMapFlags flags, void** ppData) {
    auto real = LazyResolve<PFN_vkMapMemory>("vkMapMemory");
    if (!real) return VK_ERROR_MEMORY_MAP_FAILED;
    VkResult res = real(device, memory, offset, size, flags, ppData);
    if (res == VK_SUCCESS) {
        {
            std::lock_guard<std::mutex> lock(g_memoryMapMutex);
            g_memoryMapState[memory] = { *ppData, offset };
        }
        RefreshActiveMappingsForMemory(memory);
    }
    return res;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    auto real = LazyResolve<PFN_vkUnmapMemory>("vkUnmapMemory");
    if (real) real(device, memory);
    {
        std::lock_guard<std::mutex> lock(g_memoryMapMutex);
        g_memoryMapState.erase(memory);
    }
    RefreshActiveMappingsForMemory(memory);
}


extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkMapMemory2(VkDevice device, const VkMemoryMapInfo* pMemoryMapInfo, void** ppData) {
    using PFN = PFN_vkMapMemory2;
    VkResult res;
    if (PFN real = LazyResolve<PFN>("vkMapMemory2")) {
        res = real(device, pMemoryMapInfo, ppData);
    } else {
        LogStubCall("vkMapMemory2");
        res = vkMapMemory(device, pMemoryMapInfo->memory, pMemoryMapInfo->offset,
                           pMemoryMapInfo->size, pMemoryMapInfo->flags, ppData);
        return res;
    }
    if (res == VK_SUCCESS) {
        {
            std::lock_guard<std::mutex> lock(g_memoryMapMutex);
            g_memoryMapState[pMemoryMapInfo->memory] = { *ppData, pMemoryMapInfo->offset };
        }
        RefreshActiveMappingsForMemory(pMemoryMapInfo->memory);
    }
    return res;
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkUnmapMemory2(VkDevice device, const VkMemoryUnmapInfo* pMemoryUnmapInfo) {
    using PFN = PFN_vkUnmapMemory2;
    if (PFN real = LazyResolve<PFN>("vkUnmapMemory2")) {
        VkResult res = real(device, pMemoryUnmapInfo);
        if (res == VK_SUCCESS) {
            {
                std::lock_guard<std::mutex> lock(g_memoryMapMutex);
                g_memoryMapState.erase(pMemoryUnmapInfo->memory);
            }
            RefreshActiveMappingsForMemory(pMemoryUnmapInfo->memory);
        }
        return res;
    }

    LogStubCall("vkUnmapMemory2");
    vkUnmapMemory(device, pMemoryUnmapInfo->memory);
    return VK_SUCCESS;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdSetRenderingAttachmentLocations(VkCommandBuffer commandBuffer, const VkRenderingAttachmentLocationInfo* pInfo) {
    using PFN = PFN_vkCmdSetRenderingAttachmentLocations;
    if (PFN real = LazyResolve<PFN>("vkCmdSetRenderingAttachmentLocations")) {
        real(commandBuffer, pInfo);
        return;
    }
    bool isIdentity = true;
    if (pInfo && pInfo->pColorAttachmentLocations) {
        for (uint32_t i = 0; i < pInfo->colorAttachmentCount; i++) {
            if (pInfo->pColorAttachmentLocations[i] != i && pInfo->pColorAttachmentLocations[i] != VK_ATTACHMENT_UNUSED) {
                isIdentity = false;
                break;
            }
        }
    }
    if (!isIdentity) {
        LogStubCall("vkCmdSetRenderingAttachmentLocations (non-identity mapping requested, "
                     "NOT honored -- driver without VK_KHR_dynamic_rendering_local_read/1.4; "
                     "rendering may be incorrect if the game depends on this remapping)");
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdSetRenderingInputAttachmentIndices(VkCommandBuffer commandBuffer, const VkRenderingInputAttachmentIndexInfo* pInfo) {
    using PFN = PFN_vkCmdSetRenderingInputAttachmentIndices;
    if (PFN real = LazyResolve<PFN>("vkCmdSetRenderingInputAttachmentIndices")) {
        real(commandBuffer, pInfo);
        return;
    }
    bool isIdentity = true;
    if (pInfo && pInfo->pColorAttachmentInputIndices) {
        for (uint32_t i = 0; i < pInfo->colorAttachmentCount; i++) {
            if (pInfo->pColorAttachmentInputIndices[i] != i && pInfo->pColorAttachmentInputIndices[i] != VK_ATTACHMENT_UNUSED) {
                isIdentity = false;
                break;
            }
        }
    }
    if (pInfo && pInfo->pDepthInputAttachmentIndex && *pInfo->pDepthInputAttachmentIndex != 0)
        isIdentity = false;
    if (pInfo && pInfo->pStencilInputAttachmentIndex && *pInfo->pStencilInputAttachmentIndex != 0)
        isIdentity = false;
    if (!isIdentity) {
        LogStubCall("vkCmdSetRenderingInputAttachmentIndices (non-identity mapping requested, "
                     "NOT honored -- driver without VK_KHR_dynamic_rendering_local_read/1.4)");
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetDeviceImageMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements* pInfo,
                                    VkMemoryRequirements2* pMemoryRequirements) {
    using PFN = PFN_vkGetDeviceImageMemoryRequirements;
    if (PFN real = LazyResolve<PFN>("vkGetDeviceImageMemoryRequirements")) {
        real(device, pInfo, pMemoryRequirements);
        return;
    }
    
    auto real_create = LazyResolve<PFN_vkCreateImage>("vkCreateImage");
    auto real_get = LazyResolve<PFN_vkGetImageMemoryRequirements2>("vkGetImageMemoryRequirements2");
    auto real_destroy = LazyResolve<PFN_vkDestroyImage>("vkDestroyImage");
    static std::atomic<int> s_imgCalls{0};
    const bool dbg = s_imgCalls.fetch_add(1) < 4;
    if (dbg) {
        const VkImageCreateInfo* ci = pInfo ? pInfo->pCreateInfo : nullptr;
        VkEmuLog("[vk-emu] emu GetDeviceImageMemoryRequirements: create=%p get=%p destroy=%p pInfo=%p ci=%p\n",
                 (void*)real_create, (void*)real_get, (void*)real_destroy, (const void*)pInfo, (const void*)ci);
        if (ci) VkEmuLog("[vk-emu]   image: type=%d fmt=%d flags=0x%x usage=0x%x tiling=%d samples=%d ext=%ux%ux%u mips=%u layers=%u pNext.sType=%d\n",
                 (int)ci->imageType, (int)ci->format, (unsigned)ci->flags, (unsigned)ci->usage, (int)ci->tiling, (int)ci->samples,
                 ci->extent.width, ci->extent.height, ci->extent.depth, ci->mipLevels, ci->arrayLayers,
                 ci->pNext ? (int)((const VkBaseInStructure*)ci->pNext)->sType : 0);
    }
    if (real_create && real_get && real_destroy && pInfo && pMemoryRequirements) {
        VkImage img = VK_NULL_HANDLE;
        VkResult cr = real_create(device, pInfo->pCreateInfo, nullptr, &img);
        if (dbg) VkEmuLog("[vk-emu]   temporary vkCreateImage -> %d (img=%p)\n", (int)cr, (void*)img);
        if (cr == VK_SUCCESS && img != VK_NULL_HANDLE) {
            VkImageMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
            info.image = img;
            real_get(device, &info, pMemoryRequirements);
            if (dbg) VkEmuLog("[vk-emu]   requirements: size=%llu align=%llu typeBits=0x%x\n",
                     (unsigned long long)pMemoryRequirements->memoryRequirements.size,
                     (unsigned long long)pMemoryRequirements->memoryRequirements.alignment,
                     pMemoryRequirements->memoryRequirements.memoryTypeBits);
            real_destroy(device, img, nullptr);
            return;
        }
    }
    
    pMemoryRequirements->memoryRequirements.size = 0;
    pMemoryRequirements->memoryRequirements.alignment = 1;
    pMemoryRequirements->memoryRequirements.memoryTypeBits = 0xFFFFFFFF;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetDeviceBufferMemoryRequirements(VkDevice device, const VkDeviceBufferMemoryRequirements* pInfo,
                                     VkMemoryRequirements2* pMemoryRequirements) {
    using PFN = PFN_vkGetDeviceBufferMemoryRequirements;
    if (PFN real = LazyResolve<PFN>("vkGetDeviceBufferMemoryRequirements")) {
        real(device, pInfo, pMemoryRequirements);
        return;
    }
    
    auto real_create = LazyResolve<PFN_vkCreateBuffer>("vkCreateBuffer");
    auto real_get = LazyResolve<PFN_vkGetBufferMemoryRequirements2>("vkGetBufferMemoryRequirements2");
    auto real_destroy = LazyResolve<PFN_vkDestroyBuffer>("vkDestroyBuffer");
    static std::atomic<int> s_bufCalls{0};
    const bool dbg = s_bufCalls.fetch_add(1) < 4;
    if (dbg) {
        const VkBufferCreateInfo* ci = pInfo ? pInfo->pCreateInfo : nullptr;
        VkEmuLog("[vk-emu] emu GetDeviceBufferMemoryRequirements: create=%p get=%p destroy=%p pInfo=%p ci=%p\n",
                 (void*)real_create, (void*)real_get, (void*)real_destroy, (const void*)pInfo, (const void*)ci);
        if (ci) VkEmuLog("[vk-emu]   buffer: size=%llu usage=0x%x flags=0x%x sharing=%d pNext.sType=%d\n",
                 (unsigned long long)ci->size, (unsigned)ci->usage, (unsigned)ci->flags, (int)ci->sharingMode,
                 ci->pNext ? (int)((const VkBaseInStructure*)ci->pNext)->sType : 0);
    }
    if (real_create && real_get && real_destroy && pInfo && pMemoryRequirements) {
        VkBuffer buf = VK_NULL_HANDLE;
        VkResult cr = real_create(device, pInfo->pCreateInfo, nullptr, &buf);
        if (dbg) VkEmuLog("[vk-emu]   temporary vkCreateBuffer -> %d (buf=%p)\n", (int)cr, (void*)buf);
        if (cr == VK_SUCCESS && buf != VK_NULL_HANDLE) {
            VkBufferMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
            info.buffer = buf;
            real_get(device, &info, pMemoryRequirements);
            if (dbg) VkEmuLog("[vk-emu]   requirements: size=%llu align=%llu typeBits=0x%x\n",
                     (unsigned long long)pMemoryRequirements->memoryRequirements.size,
                     (unsigned long long)pMemoryRequirements->memoryRequirements.alignment,
                     pMemoryRequirements->memoryRequirements.memoryTypeBits);
            real_destroy(device, buf, nullptr);
            return;
        }
    }
    
    pMemoryRequirements->memoryRequirements.size = 0;
    pMemoryRequirements->memoryRequirements.alignment = 1;
    pMemoryRequirements->memoryRequirements.memoryTypeBits = 0xFFFFFFFF;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetDeviceImageSparseMemoryRequirements(VkDevice device, const VkDeviceImageMemoryRequirements* pInfo,
                                          uint32_t* pSparseMemoryRequirementCount,
                                          VkSparseImageMemoryRequirements2* pSparseMemoryRequirements) {
    using PFN = PFN_vkGetDeviceImageSparseMemoryRequirements;
    if (PFN real = LazyResolve<PFN>("vkGetDeviceImageSparseMemoryRequirements")) {
        real(device, pInfo, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
        return;
    }
    auto real_create  = LazyResolve<PFN_vkCreateImage>("vkCreateImage");
    auto real_get     = LazyResolve<PFN_vkGetImageSparseMemoryRequirements2>("vkGetImageSparseMemoryRequirements2");
    auto real_destroy = LazyResolve<PFN_vkDestroyImage>("vkDestroyImage");
    if (real_create && real_get && real_destroy && pInfo && pSparseMemoryRequirementCount) {
        VkImage img;
        if (real_create(device, pInfo->pCreateInfo, nullptr, &img) == VK_SUCCESS) {
            VkImageSparseMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_IMAGE_SPARSE_MEMORY_REQUIREMENTS_INFO_2};
            info.image = img;
            real_get(device, &info, pSparseMemoryRequirementCount, pSparseMemoryRequirements);
            real_destroy(device, img, nullptr);
            return;
        }
    }

    LogStubCall("vkGetDeviceImageSparseMemoryRequirements (fallback via temporary image creation is not possible)");
    if (pSparseMemoryRequirementCount) *pSparseMemoryRequirementCount = 0;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetRenderingAreaGranularity(VkDevice device, const VkRenderingAreaInfo* pRenderingAreaInfo, VkExtent2D* pGranularity) {
    using PFN = PFN_vkGetRenderingAreaGranularity;
    if (PFN real = LazyResolve<PFN>("vkGetRenderingAreaGranularity")) {
        real(device, pRenderingAreaInfo, pGranularity);
        return;
    }
    LogStubCall("vkGetRenderingAreaGranularity (conservative fallback: 1x1 granularity)");
    if (pGranularity) { pGranularity->width = 1; pGranularity->height = 1; }
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCopyMemoryToImage(VkDevice device, const VkCopyMemoryToImageInfo* pInfo) {
    using PFN = PFN_vkCopyMemoryToImage;
    if (PFN real = LazyResolve<PFN>("vkCopyMemoryToImage"))
        return real(device, pInfo);
    LogStubCall("vkCopyMemoryToImage");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCopyImageToMemory(VkDevice device, const VkCopyImageToMemoryInfo* pInfo) {
    using PFN = PFN_vkCopyImageToMemory;
    if (PFN real = LazyResolve<PFN>("vkCopyImageToMemory"))
        return real(device, pInfo);
    LogStubCall("vkCopyImageToMemory");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCopyImageToImage(VkDevice device, const VkCopyImageToImageInfo* pInfo) {
    using PFN = PFN_vkCopyImageToImage;
    if (PFN real = LazyResolve<PFN>("vkCopyImageToImage"))
        return real(device, pInfo);
    LogStubCall("vkCopyImageToImage");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkTransitionImageLayout(VkDevice device, uint32_t transitionCount, const VkHostImageLayoutTransitionInfo* pTransitions) {
    using PFN = PFN_vkTransitionImageLayout;
    if (PFN real = LazyResolve<PFN>("vkTransitionImageLayout"))
        return real(device, transitionCount, pTransitions);
    LogStubCall("vkTransitionImageLayout");
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

static VkPipelineStageFlags LegacyStageMaskFromSync2(VkPipelineStageFlags2 mask) {
    VkPipelineStageFlags legacy = (VkPipelineStageFlags)(mask & 0xFFFFFFFFull);
    constexpr VkPipelineStageFlags2 kGranularTransfer =
        VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_RESOLVE_BIT |
        VK_PIPELINE_STAGE_2_BLIT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
    if (mask & kGranularTransfer) legacy |= VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (mask & VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT) legacy |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    if (mask & VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT) legacy |= VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
    if (mask & VK_PIPELINE_STAGE_2_PRE_RASTERIZATION_SHADERS_BIT) {
        legacy |= VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                  VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
                  VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
                  VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
    }
    return legacy;
}

static VkAccessFlags LegacyAccessMaskFromSync2(VkAccessFlags2 mask) {
    VkAccessFlags legacy = (VkAccessFlags)(mask & 0xFFFFFFFFull);
    if (mask & (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT))
        legacy |= VK_ACCESS_SHADER_READ_BIT;
    if (mask & VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT)
        legacy |= VK_ACCESS_SHADER_WRITE_BIT;
    return legacy;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo* pDependencyInfo) {
    VkDevice device = GetDeviceForCommandBuffer(commandBuffer);
    DeviceState state;
    if (!GetDeviceState(device, &state)) {

        VkEmuLog("[vk-emu] WARNING vkCmdPipelineBarrier2: untracked command buffer "
                        "(vkAllocateCommandBuffers missed this cmd?) -- trying to forward "
                        "directly to the real driver to avoid a hang.\n");
        auto rawReal = LazyResolve<PFN_vkCmdPipelineBarrier2>("vkCmdPipelineBarrier2");
        if (rawReal) { rawReal(commandBuffer, pDependencyInfo); return; }
        LogStubCall("vkCmdPipelineBarrier2 (unknown device, no fallback possible)");
        return;
    }

    if (state.sync2_native) {
        state.fn_CmdPipelineBarrier2(commandBuffer, pDependencyInfo);
        return;
    }
    if (!state.fn_CmdPipelineBarrier || !pDependencyInfo) return;

    VkPipelineStageFlags srcStageMask = 0, dstStageMask = 0;
    std::vector<VkMemoryBarrier> memBarriers;
    std::vector<VkBufferMemoryBarrier> bufBarriers;
    std::vector<VkImageMemoryBarrier> imgBarriers;

    for (uint32_t i = 0; i < pDependencyInfo->memoryBarrierCount; i++) {
        const auto& src = pDependencyInfo->pMemoryBarriers[i];
        srcStageMask |= LegacyStageMaskFromSync2(src.srcStageMask);
        dstStageMask |= LegacyStageMaskFromSync2(src.dstStageMask);
        VkMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = LegacyAccessMaskFromSync2(src.srcAccessMask);
        b.dstAccessMask = LegacyAccessMaskFromSync2(src.dstAccessMask);
        memBarriers.push_back(b);
    }
    for (uint32_t i = 0; i < pDependencyInfo->bufferMemoryBarrierCount; i++) {
        const auto& src = pDependencyInfo->pBufferMemoryBarriers[i];
        srcStageMask |= LegacyStageMaskFromSync2(src.srcStageMask);
        dstStageMask |= LegacyStageMaskFromSync2(src.dstStageMask);
        VkBufferMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.srcAccessMask = LegacyAccessMaskFromSync2(src.srcAccessMask);
        b.dstAccessMask = LegacyAccessMaskFromSync2(src.dstAccessMask);
        b.srcQueueFamilyIndex = src.srcQueueFamilyIndex; b.dstQueueFamilyIndex = src.dstQueueFamilyIndex;
        b.buffer = src.buffer; b.offset = src.offset; b.size = src.size;
        bufBarriers.push_back(b);
    }
    for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount; i++) {
        const auto& src = pDependencyInfo->pImageMemoryBarriers[i];
        srcStageMask |= LegacyStageMaskFromSync2(src.srcStageMask);
        dstStageMask |= LegacyStageMaskFromSync2(src.dstStageMask);
        VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = LegacyAccessMaskFromSync2(src.srcAccessMask);
        b.dstAccessMask = LegacyAccessMaskFromSync2(src.dstAccessMask);
        b.oldLayout = src.oldLayout; b.newLayout = src.newLayout;
        b.srcQueueFamilyIndex = src.srcQueueFamilyIndex; b.dstQueueFamilyIndex = src.dstQueueFamilyIndex;
        b.image = src.image; b.subresourceRange = src.subresourceRange;
        imgBarriers.push_back(b);
    }

    if (srcStageMask == 0) srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    if (dstStageMask == 0) dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

    state.fn_CmdPipelineBarrier(commandBuffer, srcStageMask, dstStageMask, pDependencyInfo->dependencyFlags,
           (uint32_t)memBarriers.size(), memBarriers.data(),
           (uint32_t)bufBarriers.size(), bufBarriers.data(),
           (uint32_t)imgBarriers.size(), imgBarriers.data());
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence) {
    VkDevice device = GetDeviceForQueue(queue);

    if (device == VK_NULL_HANDLE && submitCount > 0) {

        // the game went through OUR vkGetDeviceQueue/2).
        for (uint32_t i = 0; i < submitCount && device == VK_NULL_HANDLE; i++) {
            for (uint32_t j = 0; j < pSubmits[i].commandBufferInfoCount; j++) {
                VkDevice d = GetDeviceForCommandBuffer(pSubmits[i].pCommandBufferInfos[j].commandBuffer);
                if (d != VK_NULL_HANDLE) { device = d; break; }
            }
        }
        if (device != VK_NULL_HANDLE) {
            VkEmuLog("[vk-emu] vkQueueSubmit2: untracked VkQueue, device found via "
                            "a submitted command buffer -- queue tracking recovered.\n");
            TrackQueueDevice(queue, device);
        }
    }

    DeviceState state;
    bool haveState = (device != VK_NULL_HANDLE) && GetDeviceState(device, &state);

    if (!haveState) {
        VkEmuLog("[vk-emu] WARNING vkQueueSubmit2: no device found (neither via the "
                        "queue nor via the submitted command buffers) -- trying a direct forward "
                        "to the real driver to avoid a deadlock. To investigate: why this "
                        "VkQueue/these VkCommandBuffers escape tracking.\n");
        auto rawReal = LazyResolve<PFN_vkQueueSubmit2>("vkQueueSubmit2");
        if (rawReal) return rawReal(queue, submitCount, pSubmits, fence);

        auto rawLegacy = LazyResolve<PFN_vkQueueSubmit>("vkQueueSubmit");
        if (!rawLegacy) {
            VkEmuLog("[vk-emu] FATAL ERROR vkQueueSubmit2: no fallback possible, "
                            "the submission is lost -- expect a freeze/timeout.\n");
            return VK_ERROR_UNKNOWN;
        }

        std::vector<VkSubmitInfo> minimalSubmits(submitCount);
        std::vector<std::vector<VkCommandBuffer>> cmdBuffers(submitCount);
        std::vector<std::vector<VkSemaphore>> waitSem(submitCount), signalSem(submitCount);
        std::vector<std::vector<VkPipelineStageFlags>> waitStage(submitCount);
        for (uint32_t i = 0; i < submitCount; i++) {
            const auto& s2 = pSubmits[i];
            minimalSubmits[i].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            minimalSubmits[i].pNext = nullptr;
            cmdBuffers[i].resize(s2.commandBufferInfoCount);
            for (uint32_t j = 0; j < s2.commandBufferInfoCount; j++) cmdBuffers[i][j] = s2.pCommandBufferInfos[j].commandBuffer;
            minimalSubmits[i].commandBufferCount = s2.commandBufferInfoCount;
            minimalSubmits[i].pCommandBuffers = cmdBuffers[i].data();
            waitSem[i].resize(s2.waitSemaphoreInfoCount);
            waitStage[i].resize(s2.waitSemaphoreInfoCount);
            for (uint32_t j = 0; j < s2.waitSemaphoreInfoCount; j++) {
                waitSem[i][j] = s2.pWaitSemaphoreInfos[j].semaphore;
                VkPipelineStageFlags2 fbMask = s2.pWaitSemaphoreInfos[j].stageMask;
                VkPipelineStageFlags fbLegacy = LegacyStageMaskFromSync2(fbMask);
                waitStage[i][j] = (fbMask == 0 || fbLegacy == 0) ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : fbLegacy;
            }
            minimalSubmits[i].waitSemaphoreCount = s2.waitSemaphoreInfoCount;
            minimalSubmits[i].pWaitSemaphores = waitSem[i].data();
            minimalSubmits[i].pWaitDstStageMask = waitStage[i].data();
            signalSem[i].resize(s2.signalSemaphoreInfoCount);
            for (uint32_t j = 0; j < s2.signalSemaphoreInfoCount; j++) signalSem[i][j] = s2.pSignalSemaphoreInfos[j].semaphore;
            minimalSubmits[i].signalSemaphoreCount = s2.signalSemaphoreInfoCount;
            minimalSubmits[i].pSignalSemaphores = signalSem[i].data();
        }
        return rawLegacy(queue, submitCount, minimalSubmits.data(), fence);
    }

    if (state.sync2_native) return state.fn_QueueSubmit2(queue, submitCount, pSubmits, fence);
    if (!state.fn_QueueSubmit) return VK_ERROR_INITIALIZATION_FAILED;

    std::vector<VkSubmitInfo> legacySubmits(submitCount);
    std::vector<std::vector<VkSemaphore>> waitSemaphores(submitCount);
    std::vector<std::vector<VkPipelineStageFlags>> waitDstStageMasks(submitCount);
    std::vector<std::vector<VkCommandBuffer>> cmdBuffers(submitCount);
    std::vector<std::vector<VkSemaphore>> signalSemaphores(submitCount);
    std::vector<std::vector<uint64_t>> waitValues(submitCount);
    std::vector<std::vector<uint64_t>> signalValues(submitCount);
    std::vector<VkTimelineSemaphoreSubmitInfo> timelineInfos(submitCount);

    for (uint32_t i = 0; i < submitCount; i++) {
        const auto& s2 = pSubmits[i];
        legacySubmits[i].sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        legacySubmits[i].pNext = nullptr;

        waitSemaphores[i].resize(s2.waitSemaphoreInfoCount);
        waitDstStageMasks[i].resize(s2.waitSemaphoreInfoCount);
        waitValues[i].resize(s2.waitSemaphoreInfoCount);
        for (uint32_t j = 0; j < s2.waitSemaphoreInfoCount; j++) {
            waitSemaphores[i][j] = s2.pWaitSemaphoreInfos[j].semaphore;
            VkPipelineStageFlags2 mask = s2.pWaitSemaphoreInfos[j].stageMask;

            VkPipelineStageFlags legacyMask = LegacyStageMaskFromSync2(mask);
            if (mask == 0 || legacyMask == 0) legacyMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            waitDstStageMasks[i][j] = legacyMask;
            waitValues[i][j] = s2.pWaitSemaphoreInfos[j].value;
        }
        legacySubmits[i].waitSemaphoreCount = s2.waitSemaphoreInfoCount;
        legacySubmits[i].pWaitSemaphores = waitSemaphores[i].data();
        legacySubmits[i].pWaitDstStageMask = waitDstStageMasks[i].data();

        cmdBuffers[i].resize(s2.commandBufferInfoCount);
        for (uint32_t j = 0; j < s2.commandBufferInfoCount; j++) {
            cmdBuffers[i][j] = s2.pCommandBufferInfos[j].commandBuffer;
        }
        legacySubmits[i].commandBufferCount = s2.commandBufferInfoCount;
        legacySubmits[i].pCommandBuffers = cmdBuffers[i].data();

        signalSemaphores[i].resize(s2.signalSemaphoreInfoCount);
        signalValues[i].resize(s2.signalSemaphoreInfoCount);
        for (uint32_t j = 0; j < s2.signalSemaphoreInfoCount; j++) {
            signalSemaphores[i][j] = s2.pSignalSemaphoreInfos[j].semaphore;
            signalValues[i][j] = s2.pSignalSemaphoreInfos[j].value;
        }
        legacySubmits[i].signalSemaphoreCount = s2.signalSemaphoreInfoCount;
        legacySubmits[i].pSignalSemaphores = signalSemaphores[i].data();
        bool hasTimeline = false;
        for (auto v : waitValues[i]) if (v != 0) hasTimeline = true;
        for (auto v : signalValues[i]) if (v != 0) hasTimeline = true;

        if (hasTimeline) {
            timelineInfos[i].sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timelineInfos[i].waitSemaphoreValueCount = s2.waitSemaphoreInfoCount;
            timelineInfos[i].pWaitSemaphoreValues = waitValues[i].data();
            timelineInfos[i].signalSemaphoreValueCount = s2.signalSemaphoreInfoCount;
            timelineInfos[i].pSignalSemaphoreValues = signalValues[i].data();
            timelineInfos[i].pNext = legacySubmits[i].pNext;
            legacySubmits[i].pNext = &timelineInfos[i];
        }
    }
    return state.fn_QueueSubmit(queue, submitCount, legacySubmits.data(), fence);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdWriteTimestamp2(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage, VkQueryPool queryPool, uint32_t query) {
    VkDevice device = GetDeviceForCommandBuffer(commandBuffer);
    DeviceState state;
    if (!GetDeviceState(device, &state)) { LogStubCall("vkCmdWriteTimestamp2 (unknown device)"); return; }

    if (state.sync2_native) { state.fn_CmdWriteTimestamp2(commandBuffer, stage, queryPool, query); return; }
    if (state.fn_CmdWriteTimestamp) state.fn_CmdWriteTimestamp(commandBuffer, (VkPipelineStageFlagBits)stage, queryPool, query);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdSetEvent2(VkCommandBuffer commandBuffer, VkEvent event, const VkDependencyInfo* pDependencyInfo) {
    VkDevice device = GetDeviceForCommandBuffer(commandBuffer);
    DeviceState state;
    if (!GetDeviceState(device, &state)) { LogStubCall("vkCmdSetEvent2 (unknown device)"); return; }

    if (state.sync2_native) { state.fn_CmdSetEvent2(commandBuffer, event, pDependencyInfo); return; }
       if (state.fn_CmdSetEvent && pDependencyInfo) {
        VkPipelineStageFlags mask = 0;
        for (uint32_t i = 0; i < pDependencyInfo->memoryBarrierCount; i++) mask |= (VkPipelineStageFlags)pDependencyInfo->pMemoryBarriers[i].srcStageMask;
        for (uint32_t i = 0; i < pDependencyInfo->bufferMemoryBarrierCount; i++) mask |= (VkPipelineStageFlags)pDependencyInfo->pBufferMemoryBarriers[i].srcStageMask;
        for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount; i++) mask |= (VkPipelineStageFlags)pDependencyInfo->pImageMemoryBarriers[i].srcStageMask;
        if (mask == 0) mask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        state.fn_CmdSetEvent(commandBuffer, event, mask);
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdResetEvent2(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags2 stageMask) {
    VkDevice device = GetDeviceForCommandBuffer(commandBuffer);
    DeviceState state;
    if (!GetDeviceState(device, &state)) { LogStubCall("vkCmdResetEvent2 (unknown device)"); return; }

    if (state.sync2_native) { state.fn_CmdResetEvent2(commandBuffer, event, stageMask); return; }
    if (state.fn_CmdResetEvent) state.fn_CmdResetEvent(commandBuffer, event, (VkPipelineStageFlagBits)stageMask);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdWaitEvents2(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent* pEvents, const VkDependencyInfo* pDependencyInfos) {
    VkDevice device = GetDeviceForCommandBuffer(commandBuffer);
    DeviceState state;
    if (!GetDeviceState(device, &state)) { LogStubCall("vkCmdWaitEvents2 (unknown device)"); return; }

    if (state.sync2_native) { state.fn_CmdWaitEvents2(commandBuffer, eventCount, pEvents, pDependencyInfos); return; }
    if (!state.fn_CmdWaitEvents || !pDependencyInfos) return;

    VkPipelineStageFlags srcStageMask = 0, dstStageMask = 0;
    std::vector<VkMemoryBarrier> memBarriers;
    std::vector<VkBufferMemoryBarrier> bufBarriers;
    std::vector<VkImageMemoryBarrier> imgBarriers;

    for (uint32_t i = 0; i < eventCount; i++) {
        const auto& dep = pDependencyInfos[i];
        for (uint32_t j = 0; j < dep.memoryBarrierCount; j++) {
            const auto& src = dep.pMemoryBarriers[j];
            srcStageMask |= LegacyStageMaskFromSync2(src.srcStageMask);
            dstStageMask |= LegacyStageMaskFromSync2(src.dstStageMask);
            VkMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            b.srcAccessMask = LegacyAccessMaskFromSync2(src.srcAccessMask);
            b.dstAccessMask = LegacyAccessMaskFromSync2(src.dstAccessMask);
            memBarriers.push_back(b);
        }
        for (uint32_t j = 0; j < dep.bufferMemoryBarrierCount; j++) {
            const auto& src = dep.pBufferMemoryBarriers[j];
            srcStageMask |= LegacyStageMaskFromSync2(src.srcStageMask);
            dstStageMask |= LegacyStageMaskFromSync2(src.dstStageMask);
            VkBufferMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            b.srcAccessMask = LegacyAccessMaskFromSync2(src.srcAccessMask);
            b.dstAccessMask = LegacyAccessMaskFromSync2(src.dstAccessMask);
            b.srcQueueFamilyIndex = src.srcQueueFamilyIndex; b.dstQueueFamilyIndex = src.dstQueueFamilyIndex;
            b.buffer = src.buffer; b.offset = src.offset; b.size = src.size;
            bufBarriers.push_back(b);
        }
        for (uint32_t j = 0; j < dep.imageMemoryBarrierCount; j++) {
            const auto& src = dep.pImageMemoryBarriers[j];
            srcStageMask |= LegacyStageMaskFromSync2(src.srcStageMask);
            dstStageMask |= LegacyStageMaskFromSync2(src.dstStageMask);
            VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = LegacyAccessMaskFromSync2(src.srcAccessMask);
            b.dstAccessMask = LegacyAccessMaskFromSync2(src.dstAccessMask);
            b.oldLayout = src.oldLayout; b.newLayout = src.newLayout;
            b.srcQueueFamilyIndex = src.srcQueueFamilyIndex; b.dstQueueFamilyIndex = src.dstQueueFamilyIndex;
            b.image = src.image; b.subresourceRange = src.subresourceRange;
            imgBarriers.push_back(b);
        }
    }
    if (srcStageMask == 0) srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    if (dstStageMask == 0) dstStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    state.fn_CmdWaitEvents(commandBuffer, eventCount, pEvents, srcStageMask, dstStageMask,
           (uint32_t)memBarriers.size(), memBarriers.data(),
           (uint32_t)bufBarriers.size(), bufBarriers.data(),
           (uint32_t)imgBarriers.size(), imgBarriers.data());
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdBindDescriptorSets2(VkCommandBuffer commandBuffer, const VkBindDescriptorSetsInfo* pBindDescriptorSetsInfo) {
    auto real = LazyResolve<PFN_vkCmdBindDescriptorSets2>("vkCmdBindDescriptorSets2");
    if (real) { real(commandBuffer, pBindDescriptorSetsInfo); return; }
    
    LogStubCall("vkCmdBindDescriptorSets2");
    if (pBindDescriptorSetsInfo) {
        auto legacy = LazyResolve<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");
        if (legacy) {
            // fallback
            legacy(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pBindDescriptorSetsInfo->layout, 
                   pBindDescriptorSetsInfo->firstSet, pBindDescriptorSetsInfo->descriptorSetCount, 
                   pBindDescriptorSetsInfo->pDescriptorSets, pBindDescriptorSetsInfo->dynamicOffsetCount, 
                   pBindDescriptorSetsInfo->pDynamicOffsets);
        }
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdPushConstants2(VkCommandBuffer commandBuffer, const VkPushConstantsInfo* pPushConstantsInfo) {
    auto real = LazyResolve<PFN_vkCmdPushConstants2>("vkCmdPushConstants2");
    if (real) { real(commandBuffer, pPushConstantsInfo); return; }
    LogStubCall("vkCmdPushConstants2");
    if (pPushConstantsInfo) {
        auto legacy = LazyResolve<PFN_vkCmdPushConstants>("vkCmdPushConstants");
        if (legacy) legacy(commandBuffer, pPushConstantsInfo->layout, pPushConstantsInfo->stageFlags, pPushConstantsInfo->offset, pPushConstantsInfo->size, pPushConstantsInfo->pValues);
    }
}

static std::mutex g_templateBindPointMutex;
static std::unordered_map<VkDescriptorUpdateTemplate, VkPipelineBindPoint> g_templateBindPoint;

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreateDescriptorUpdateTemplate(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
                                  const VkAllocationCallbacks* pAllocator, VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {
    auto real = LazyResolve<PFN_vkCreateDescriptorUpdateTemplate>("vkCreateDescriptorUpdateTemplate");
    if (!real) { LogFatal("vkCreateDescriptorUpdateTemplate"); return VK_ERROR_INITIALIZATION_FAILED; }
    VkResult r = real(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
    if (r == VK_SUCCESS && pCreateInfo && pDescriptorUpdateTemplate) {
        std::lock_guard<std::mutex> lock(g_templateBindPointMutex);
        g_templateBindPoint[*pDescriptorUpdateTemplate] = pCreateInfo->pipelineBindPoint;
    }
    return r;
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreateDescriptorUpdateTemplateKHR(VkDevice device, const VkDescriptorUpdateTemplateCreateInfo* pCreateInfo,
                                     const VkAllocationCallbacks* pAllocator, VkDescriptorUpdateTemplate* pDescriptorUpdateTemplate) {
    return vkCreateDescriptorUpdateTemplate(device, pCreateInfo, pAllocator, pDescriptorUpdateTemplate);
}

static void EmuPushDescriptorSetWithTemplateRaw(VkCommandBuffer commandBuffer,
                                                 VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                                 VkPipelineLayout layout, uint32_t set, const void* pData) {
    if (!pData) return;
    VkDevice device = GetDeviceForCommandBuffer(commandBuffer);
    if (device == VK_NULL_HANDLE) { LogStubCall("PushDescriptorSetWithTemplate (unknown device)"); return; }

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(g_pipelineLayoutSetsMutex);
        auto it = g_pipelineLayoutSets.find(layout);
        if (it != g_pipelineLayoutSets.end() && set < it->second.size())
            setLayout = it->second[set];
    }
    if (setLayout == VK_NULL_HANDLE) {
        LogStubCall("PushDescriptorSetWithTemplate (set layout not found)");
        return;
    }

    VkDescriptorPool pool = GetOrCreateEmuDescPool(device);
    if (pool == VK_NULL_HANDLE) return;

    auto real_alloc      = LazyResolve<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets");
    auto real_updateTpl  = LazyResolve<PFN_vkUpdateDescriptorSetWithTemplate>("vkUpdateDescriptorSetWithTemplate");
    auto real_bind       = LazyResolve<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");
    if (!real_alloc || !real_updateTpl || !real_bind) {
        LogStubCall("PushDescriptorSetWithTemplate (core functions not found)");
        return;
    }

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = pool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &setLayout;

    VkDescriptorSet realSet = VK_NULL_HANDLE;
    VkResult ar = real_alloc(device, &dsai, &realSet);
    if (ar != VK_SUCCESS) {
        std::lock_guard<std::mutex> lock(g_descBufPoolMutex);
        auto& pools = g_descBufPools[device];
        if (!pools.empty()) pools.back().allocatedCount = EmuDescBufferPool::kMaxSetsPerPool;
        LogStubCall("PushDescriptorSetWithTemplate (descriptor set allocation failed)");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(g_descBufPoolMutex);
        auto& pools = g_descBufPools[device];
        if (!pools.empty()) pools.back().allocatedCount++;
    }

    real_updateTpl(device, realSet, descriptorUpdateTemplate, pData);

    VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    {
        std::lock_guard<std::mutex> lock(g_templateBindPointMutex);
        auto it = g_templateBindPoint.find(descriptorUpdateTemplate);
        if (it != g_templateBindPoint.end()) bindPoint = it->second;
    }

    real_bind(commandBuffer, bindPoint, layout, set, 1, &realSet, 0, nullptr);
}

static void EmuPushDescriptorSetWithTemplate(VkCommandBuffer commandBuffer, const VkPushDescriptorSetWithTemplateInfo* pInfo) {
    if (!pInfo) return;
    EmuPushDescriptorSetWithTemplateRaw(commandBuffer, pInfo->descriptorUpdateTemplate, pInfo->layout, pInfo->set, pInfo->pData);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer commandBuffer, VkDescriptorUpdateTemplate descriptorUpdateTemplate,
                                       VkPipelineLayout layout, uint32_t set, const void* pData) {
    DeviceState state;
    VkDevice device = GetDeviceForCommandBuffer(commandBuffer);
    if (GetDeviceState(device, &state) && state.push_descriptor_native) {
        auto real = LazyResolve<PFN_vkCmdPushDescriptorSetWithTemplateKHR>("vkCmdPushDescriptorSetWithTemplateKHR");
        if (real) { real(commandBuffer, descriptorUpdateTemplate, layout, set, pData); return; }
    }
    EmuPushDescriptorSetWithTemplateRaw(commandBuffer, descriptorUpdateTemplate, layout, set, pData);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdPushDescriptorSetWithTemplate2(VkCommandBuffer commandBuffer, const VkPushDescriptorSetWithTemplateInfo* pInfo) {
    auto real = LazyResolve<PFN_vkCmdPushDescriptorSetWithTemplate2>("vkCmdPushDescriptorSetWithTemplate2");
    if (real) { real(commandBuffer, pInfo); return; }

    auto legacy = LazyResolve<PFN_vkCmdPushDescriptorSetWithTemplateKHR>("vkCmdPushDescriptorSetWithTemplateKHR");
    if (legacy && pInfo) {
        legacy(commandBuffer, pInfo->descriptorUpdateTemplate, pInfo->layout, pInfo->set, pInfo->pData);
        return;
    }

    EmuPushDescriptorSetWithTemplate(commandBuffer, pInfo);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdPushDescriptorSetWithTemplate2KHR(VkCommandBuffer commandBuffer, const VkPushDescriptorSetWithTemplateInfo* pInfo) {
    auto real = LazyResolve<PFN_vkCmdPushDescriptorSetWithTemplate2KHR>("vkCmdPushDescriptorSetWithTemplate2KHR");
    if (real) { real(commandBuffer, pInfo); return; }

    auto legacy = LazyResolve<PFN_vkCmdPushDescriptorSetWithTemplateKHR>("vkCmdPushDescriptorSetWithTemplateKHR");
    if (legacy && pInfo) {
        legacy(commandBuffer, pInfo->descriptorUpdateTemplate, pInfo->layout, pInfo->set, pInfo->pData);
        return;
    }

    EmuPushDescriptorSetWithTemplate(commandBuffer, pInfo);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdPushDescriptorSetKHR(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
                          VkPipelineLayout layout, uint32_t set, uint32_t descriptorWriteCount,
                          const VkWriteDescriptorSet* pDescriptorWrites) {
    VkDevice device = GetDeviceForCommandBuffer(commandBuffer);
    DeviceState state;
    if (!GetDeviceState(device, &state)) {
        LogStubCall("vkCmdPushDescriptorSetKHR (unknown device)");
        return;
    }


    if (state.push_descriptor_native && state.fn_CmdPushDescriptorSetKHR) {
        state.fn_CmdPushDescriptorSetKHR(commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites);
        return;
    }


    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(g_pipelineLayoutSetsMutex);
        auto it = g_pipelineLayoutSets.find(layout);
        if (it != g_pipelineLayoutSets.end() && set < it->second.size()) {
            setLayout = it->second[set];
        }
    }
    if (setLayout == VK_NULL_HANDLE) {
        LogStubCall("vkCmdPushDescriptorSetKHR (set layout not found)");
        return;
    }

    size_t contentHash = 0;
    for (uint32_t i = 0; i < descriptorWriteCount; i++) {
        const auto& w = pDescriptorWrites[i];
        contentHash ^= std::hash<uint32_t>()(w.dstBinding) + 0x9e3779b9 + (contentHash << 6) + (contentHash >> 2);
        contentHash ^= std::hash<uint32_t>()(w.descriptorType) + 0x9e3779b9 + (contentHash << 6) + (contentHash >> 2);
        contentHash ^= std::hash<uint32_t>()(w.descriptorCount) + 0x9e3779b9 + (contentHash << 6) + (contentHash >> 2);

        if (w.pImageInfo) {
            for (uint32_t j = 0; j < w.descriptorCount; j++) {
                contentHash ^= std::hash<uint64_t>()((uint64_t)w.pImageInfo[j].sampler) + 0x9e3779b9;
                contentHash ^= std::hash<uint64_t>()((uint64_t)w.pImageInfo[j].imageView) + 0x9e3779b9;
                contentHash ^= std::hash<uint32_t>()(w.pImageInfo[j].imageLayout) + 0x9e3779b9;
            }
        } else if (w.pBufferInfo) {
            for (uint32_t j = 0; j < w.descriptorCount; j++) {
                contentHash ^= std::hash<uint64_t>()((uint64_t)w.pBufferInfo[j].buffer) + 0x9e3779b9;
                contentHash ^= std::hash<uint64_t>()(w.pBufferInfo[j].offset) + 0x9e3779b9;
                contentHash ^= std::hash<uint64_t>()(w.pBufferInfo[j].range) + 0x9e3779b9;
            }
        } else if (w.pTexelBufferView) {
            for (uint32_t j = 0; j < w.descriptorCount; j++) {
                contentHash ^= std::hash<uint64_t>()((uint64_t)w.pTexelBufferView[j]) + 0x9e3779b9;
            }
        }
    }

    EmuDescSetKey key{setLayout, contentHash};
    VkDescriptorSet realSet = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(g_descBufPoolMutex);
        auto& cache = g_descBufSetCache[device];
        auto cit = cache.find(key);
        if (cit != cache.end()) realSet = cit->second;
    }

    if (realSet == VK_NULL_HANDLE) {
        VkDescriptorPool pool = GetOrCreateEmuDescPool(device);
        if (pool == VK_NULL_HANDLE) return;

        auto real_alloc = LazyResolve<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets");
        if (!real_alloc) return;

        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = pool;
        dsai.descriptorSetCount = 1;
        dsai.pSetLayouts = &setLayout;
        VkResult ar = real_alloc(device, &dsai, &realSet);
        if (ar != VK_SUCCESS) {
            std::lock_guard<std::mutex> lock(g_descBufPoolMutex);
            auto& pools = g_descBufPools[device];
            if (!pools.empty()) pools.back().allocatedCount = EmuDescBufferPool::kMaxSetsPerPool;
            return;
        }
        {
            std::lock_guard<std::mutex> lock(g_descBufPoolMutex);
            auto& pools = g_descBufPools[device];
            if (!pools.empty()) pools.back().allocatedCount++;
        }

        auto real_update = LazyResolve<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets");
        if (real_update) {
            std::vector<VkWriteDescriptorSet> writes(descriptorWriteCount);
            for (uint32_t i = 0; i < descriptorWriteCount; i++) {
                writes[i] = pDescriptorWrites[i];
                writes[i].dstSet = realSet;
            }
            real_update(device, descriptorWriteCount, writes.data(), 0, nullptr);
        }

        std::lock_guard<std::mutex> lock(g_descBufPoolMutex);
        g_descBufSetCache[device][key] = realSet;
    }

    auto real_bind = LazyResolve<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");
    if (real_bind) real_bind(commandBuffer, pipelineBindPoint, layout, set, 1, &realSet, 0, nullptr);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdPushDescriptorSet2KHR(VkCommandBuffer commandBuffer, const VkPushDescriptorSetInfo* pPushDescriptorSetInfo) {
    auto real = LazyResolve<PFN_vkCmdPushDescriptorSet2KHR>("vkCmdPushDescriptorSet2KHR");
    if (real) { real(commandBuffer, pPushDescriptorSetInfo); return; }

    if (pPushDescriptorSetInfo) {
        VkPipelineBindPoint bindPoint =
            (pPushDescriptorSetInfo->stageFlags & VK_SHADER_STAGE_COMPUTE_BIT)
                ? VK_PIPELINE_BIND_POINT_COMPUTE
                : VK_PIPELINE_BIND_POINT_GRAPHICS;
        

        vkCmdPushDescriptorSetKHR(commandBuffer, bindPoint, pPushDescriptorSetInfo->layout, pPushDescriptorSetInfo->set,
                                  pPushDescriptorSetInfo->descriptorWriteCount, pPushDescriptorSetInfo->pDescriptorWrites);
    } else {
        LogStubCall("vkCmdPushDescriptorSet2KHR (pPushDescriptorSetInfo null)");
    }
}

// Vulkan 1.3 feature emulation

// PATCH: the *2 structs (VkBufferCopy2, VkImageCopy2, ...) have a sType/pNext header (16 bytes)
// and are NOT binary-compatible with the legacy structs. A simple cast corrupts the
// regions (the offsets/sizes read would be sType/pNext) -> they must be converted field by field.
static std::vector<VkBufferCopy> ConvRegions(const VkBufferCopy2* r, uint32_t n) {
    std::vector<VkBufferCopy> o(n);
    for (uint32_t i = 0; i < n; i++) { o[i].srcOffset = r[i].srcOffset; o[i].dstOffset = r[i].dstOffset; o[i].size = r[i].size; }
    return o;
}
static std::vector<VkImageCopy> ConvRegions(const VkImageCopy2* r, uint32_t n) {
    std::vector<VkImageCopy> o(n);
    for (uint32_t i = 0; i < n; i++) { o[i].srcSubresource = r[i].srcSubresource; o[i].srcOffset = r[i].srcOffset;
        o[i].dstSubresource = r[i].dstSubresource; o[i].dstOffset = r[i].dstOffset; o[i].extent = r[i].extent; }
    return o;
}
static std::vector<VkBufferImageCopy> ConvRegions(const VkBufferImageCopy2* r, uint32_t n) {
    std::vector<VkBufferImageCopy> o(n);
    for (uint32_t i = 0; i < n; i++) { o[i].bufferOffset = r[i].bufferOffset; o[i].bufferRowLength = r[i].bufferRowLength;
        o[i].bufferImageHeight = r[i].bufferImageHeight; o[i].imageSubresource = r[i].imageSubresource;
        o[i].imageOffset = r[i].imageOffset; o[i].imageExtent = r[i].imageExtent; }
    return o;
}
static std::vector<VkImageBlit> ConvRegions(const VkImageBlit2* r, uint32_t n) {
    std::vector<VkImageBlit> o(n);
    for (uint32_t i = 0; i < n; i++) { o[i].srcSubresource = r[i].srcSubresource; o[i].srcOffsets[0] = r[i].srcOffsets[0]; o[i].srcOffsets[1] = r[i].srcOffsets[1];
        o[i].dstSubresource = r[i].dstSubresource; o[i].dstOffsets[0] = r[i].dstOffsets[0]; o[i].dstOffsets[1] = r[i].dstOffsets[1]; }
    return o;
}
static std::vector<VkImageResolve> ConvRegions(const VkImageResolve2* r, uint32_t n) {
    std::vector<VkImageResolve> o(n);
    for (uint32_t i = 0; i < n; i++) { o[i].srcSubresource = r[i].srcSubresource; o[i].srcOffset = r[i].srcOffset;
        o[i].dstSubresource = r[i].dstSubresource; o[i].dstOffset = r[i].dstOffset; o[i].extent = r[i].extent; }
    return o;
}

// --- Copy Commands 2 (VK_KHR_copy_commands2) ---
extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdCopyBuffer2(VkCommandBuffer commandBuffer, const VkCopyBufferInfo2* pCopyBufferInfo) {
    auto real = LazyResolve<PFN_vkCmdCopyBuffer2>("vkCmdCopyBuffer2KHR");
    if (real) { real(commandBuffer, pCopyBufferInfo); return; }
    auto legacy = LazyResolve<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    if (legacy && pCopyBufferInfo) {
        // (explicit conversion: VkBufferCopy2 is NOT binary-compatible with VkBufferCopy)
        legacy(commandBuffer, pCopyBufferInfo->srcBuffer, pCopyBufferInfo->dstBuffer, pCopyBufferInfo->regionCount, ConvRegions(pCopyBufferInfo->pRegions, pCopyBufferInfo->regionCount).data());
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdCopyImage2(VkCommandBuffer commandBuffer, const VkCopyImageInfo2* pCopyImageInfo) {
    auto real = LazyResolve<PFN_vkCmdCopyImage2>("vkCmdCopyImage2KHR");
    if (real) { real(commandBuffer, pCopyImageInfo); return; }
    auto legacy = LazyResolve<PFN_vkCmdCopyImage>("vkCmdCopyImage");
    if (legacy && pCopyImageInfo) {
        legacy(commandBuffer, pCopyImageInfo->srcImage, pCopyImageInfo->srcImageLayout, pCopyImageInfo->dstImage, pCopyImageInfo->dstImageLayout, pCopyImageInfo->regionCount, ConvRegions(pCopyImageInfo->pRegions, pCopyImageInfo->regionCount).data());
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdCopyBufferToImage2(VkCommandBuffer commandBuffer, const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo) {
    auto real = LazyResolve<PFN_vkCmdCopyBufferToImage2>("vkCmdCopyBufferToImage2KHR");
    if (real) { real(commandBuffer, pCopyBufferToImageInfo); return; }
    auto legacy = LazyResolve<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
    if (legacy && pCopyBufferToImageInfo) {
        legacy(commandBuffer, pCopyBufferToImageInfo->srcBuffer, pCopyBufferToImageInfo->dstImage, pCopyBufferToImageInfo->dstImageLayout, pCopyBufferToImageInfo->regionCount, ConvRegions(pCopyBufferToImageInfo->pRegions, pCopyBufferToImageInfo->regionCount).data());
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdCopyImageToBuffer2(VkCommandBuffer commandBuffer, const VkCopyImageToBufferInfo2* pCopyImageToBufferInfo) {
    auto real = LazyResolve<PFN_vkCmdCopyImageToBuffer2>("vkCmdCopyImageToBuffer2KHR");
    if (real) { real(commandBuffer, pCopyImageToBufferInfo); return; }
    auto legacy = LazyResolve<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
    if (legacy && pCopyImageToBufferInfo) {
        legacy(commandBuffer, pCopyImageToBufferInfo->srcImage, pCopyImageToBufferInfo->srcImageLayout, pCopyImageToBufferInfo->dstBuffer, pCopyImageToBufferInfo->regionCount, ConvRegions(pCopyImageToBufferInfo->pRegions, pCopyImageToBufferInfo->regionCount).data());
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdBlitImage2(VkCommandBuffer commandBuffer, const VkBlitImageInfo2* pBlitImageInfo) {
    auto real = LazyResolve<PFN_vkCmdBlitImage2>("vkCmdBlitImage2KHR");
    if (real) { real(commandBuffer, pBlitImageInfo); return; }
    auto legacy = LazyResolve<PFN_vkCmdBlitImage>("vkCmdBlitImage");
    if (legacy && pBlitImageInfo) {
        legacy(commandBuffer, pBlitImageInfo->srcImage, pBlitImageInfo->srcImageLayout, pBlitImageInfo->dstImage, pBlitImageInfo->dstImageLayout, pBlitImageInfo->regionCount, ConvRegions(pBlitImageInfo->pRegions, pBlitImageInfo->regionCount).data(), pBlitImageInfo->filter);
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdResolveImage2(VkCommandBuffer commandBuffer, const VkResolveImageInfo2* pResolveImageInfo) {
    auto real = LazyResolve<PFN_vkCmdResolveImage2>("vkCmdResolveImage2KHR");
    if (real) { real(commandBuffer, pResolveImageInfo); return; }
    auto legacy = LazyResolve<PFN_vkCmdResolveImage>("vkCmdResolveImage");
    if (legacy && pResolveImageInfo) {
        legacy(commandBuffer, pResolveImageInfo->srcImage, pResolveImageInfo->srcImageLayout, pResolveImageInfo->dstImage, pResolveImageInfo->dstImageLayout, pResolveImageInfo->regionCount, ConvRegions(pResolveImageInfo->pRegions, pResolveImageInfo->regionCount).data());
    }
}

// --- Extended Dynamic State (VK_EXT_extended_dynamic_state) ---
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetCullMode(VkCommandBuffer cb, VkCullModeFlags m) {
    auto real = LazyResolve<PFN_vkCmdSetCullMode>("vkCmdSetCullModeEXT");
    if (real) real(cb, m);
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetFrontFace(VkCommandBuffer cb, VkFrontFace f) {
    auto real = LazyResolve<PFN_vkCmdSetFrontFace>("vkCmdSetFrontFaceEXT");
    if (real) real(cb, f);
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetPrimitiveTopology(VkCommandBuffer cb, VkPrimitiveTopology t) {
    auto real = LazyResolve<PFN_vkCmdSetPrimitiveTopology>("vkCmdSetPrimitiveTopologyEXT");
    if (!real) LogStubCall("vkCmdSetPrimitiveTopology (NO-OP !)");
    if (real) real(cb, t);
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetViewportWithCount(VkCommandBuffer cb, uint32_t c, const VkViewport* v) {
    auto real = LazyResolve<PFN_vkCmdSetViewportWithCount>("vkCmdSetViewportWithCountEXT");
    if (!real) LogStubCall("vkCmdSetViewportWithCount (NO-OP !)");
    if (real) real(cb, c, v);
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetScissorWithCount(VkCommandBuffer cb, uint32_t c, const VkRect2D* s) {
    auto real = LazyResolve<PFN_vkCmdSetScissorWithCount>("vkCmdSetScissorWithCountEXT");
    if (!real) LogStubCall("vkCmdSetScissorWithCount (NO-OP !)");
    if (real) real(cb, c, s);
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes, const VkDeviceSize* pStrides) {
    auto real = LazyResolve<PFN_vkCmdBindVertexBuffers2>("vkCmdBindVertexBuffers2EXT");
    if (!real) LogStubCall("vkCmdBindVertexBuffers2 (NO-OP !)");
    if (real) real(cb, firstBinding, bindingCount, pBuffers, pOffsets, pSizes, pStrides);
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetDepthTestEnable(VkCommandBuffer cb, VkBool32 e) {
    auto real = LazyResolve<PFN_vkCmdSetDepthTestEnable>("vkCmdSetDepthTestEnableEXT");
    if (real) real(cb, e);
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetDepthWriteEnable(VkCommandBuffer cb, VkBool32 e) {
    auto real = LazyResolve<PFN_vkCmdSetDepthWriteEnable>("vkCmdSetDepthWriteEnableEXT");
    if (real) real(cb, e);
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetDepthCompareOp(VkCommandBuffer cb, VkCompareOp o) {
    auto real = LazyResolve<PFN_vkCmdSetDepthCompareOp>("vkCmdSetDepthCompareOpEXT");
    if (real) real(cb, o);
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetDepthBoundsTestEnable(VkCommandBuffer cb, VkBool32 e) {
    auto real = LazyResolve<PFN_vkCmdSetDepthBoundsTestEnable>("vkCmdSetDepthBoundsTestEnableEXT");
    if (real) real(cb, e);
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetStencilTestEnable(VkCommandBuffer cb, VkBool32 e) {
    auto real = LazyResolve<PFN_vkCmdSetStencilTestEnable>("vkCmdSetStencilTestEnableEXT");
    if (real) real(cb, e);
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetStencilOp(VkCommandBuffer cb, VkStencilFaceFlags f, VkStencilOp ff, VkStencilOp dp, VkStencilOp df, VkCompareOp co) {
    auto real = LazyResolve<PFN_vkCmdSetStencilOp>("vkCmdSetStencilOpEXT");
    if (real) real(cb, f, ff, dp, df, co);
}

// --- Extended Dynamic State 2 (VK_EXT_extended_dynamic_state2) ---
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetDepthBiasEnable(VkCommandBuffer cb, VkBool32 e) {
    auto real = LazyResolve<PFN_vkCmdSetDepthBiasEnable>("vkCmdSetDepthBiasEnableEXT");
    if (real) real(cb, e);
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetPrimitiveRestartEnable(VkCommandBuffer cb, VkBool32 e) {
    auto real = LazyResolve<PFN_vkCmdSetPrimitiveRestartEnable>("vkCmdSetPrimitiveRestartEnableEXT");
    if (real) real(cb, e);
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetRasterizerDiscardEnable(VkCommandBuffer cb, VkBool32 e) {
    auto real = LazyResolve<PFN_vkCmdSetRasterizerDiscardEnable>("vkCmdSetRasterizerDiscardEnableEXT");
    if (real) real(cb, e);
}

extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetLogicOpEXT(VkCommandBuffer cb, VkLogicOp logicOp) {
    if (auto r = LazyResolve<PFN_vkCmdSetLogicOpEXT>("vkCmdSetLogicOpEXT")) r(cb, logicOp);
    else LogStubCall("vkCmdSetLogicOpEXT");
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetPatchControlPointsEXT(VkCommandBuffer cb, uint32_t patchControlPoints) {
    if (auto r = LazyResolve<PFN_vkCmdSetPatchControlPointsEXT>("vkCmdSetPatchControlPointsEXT")) r(cb, patchControlPoints);
    else LogStubCall("vkCmdSetPatchControlPointsEXT");
}

static PFN_vkVoidFunction FreshResolveAgainstCurrentInstance(const char* name) {
    EnsureRealLoaderLoaded();
    VkInstance inst = g_lastInstance.load(std::memory_order_acquire);
    if (!g_real.GetInstanceProcAddr || inst == VK_NULL_HANDLE) return nullptr;
    return g_real.GetInstanceProcAddr(inst, name);
}

// Bump API Version
extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* pProperties) {
    auto real = (PFN_vkGetPhysicalDeviceProperties)FreshResolveAgainstCurrentInstance("vkGetPhysicalDeviceProperties");
    if (!real) { LogFatal("vkGetPhysicalDeviceProperties"); return; }
    real(physicalDevice, pProperties);
    if (pProperties) {
        pProperties->apiVersion = VKEMU_SPOOFED_API_VERSION;
    }
}

// ---------------------------------------------------------------------------
// VkPhysicalDeviceVulkan13Properties does not exist on a 1.2 driver: the driver ignores the struct and
// it stays zeroed. vkd3d-proton 2.10 reads the texel buffer alignment (mandatory) and the
// subgroup sizes from it. We rebuild it from the 1.1 structs / extensions the driver knows.
// ---------------------------------------------------------------------------
static void FillVulkan13Properties(VkPhysicalDevice pd, PFN_vkGetPhysicalDeviceProperties2 real,
                                   VkPhysicalDeviceVulkan13Properties* out) {
    const size_t off = offsetof(VkPhysicalDeviceVulkan13Properties, minSubgroupSize);
    memset((char*)out + off, 0, sizeof(*out) - off);

    const bool has_ssc = RealDeviceHasExtension(pd, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    const bool has_iub = RealDeviceHasExtension(pd, VK_EXT_INLINE_UNIFORM_BLOCK_EXTENSION_NAME);
    const bool has_tba = RealDeviceHasExtension(pd, VK_EXT_TEXEL_BUFFER_ALIGNMENT_EXTENSION_NAME);

    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceSubgroupProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
    VkPhysicalDeviceMaintenance3Properties m3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES};
    VkPhysicalDeviceSubgroupSizeControlProperties ssc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    VkPhysicalDeviceInlineUniformBlockProperties iub{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_PROPERTIES};
    VkPhysicalDeviceTexelBufferAlignmentProperties tba{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TEXEL_BUFFER_ALIGNMENT_PROPERTIES};

    VkBaseOutStructure* last = (VkBaseOutStructure*)&p2;
    auto add = [&](void* st) { last->pNext = (VkBaseOutStructure*)st; last = (VkBaseOutStructure*)st; };
    add(&sg); add(&m3);
    if (has_ssc) add(&ssc);
    if (has_iub) add(&iub);
    if (has_tba) add(&tba);
    real(pd, &p2);

    // Subgroup: without subgroup_size_control, min = max = fixed subgroup size (32 on NVIDIA).
    out->minSubgroupSize = has_ssc ? ssc.minSubgroupSize : sg.subgroupSize;
    out->maxSubgroupSize = has_ssc ? ssc.maxSubgroupSize : sg.subgroupSize;
    if (has_ssc) {
        out->maxComputeWorkgroupSubgroups = ssc.maxComputeWorkgroupSubgroups;
        out->requiredSubgroupSizeStages   = ssc.requiredSubgroupSizeStages;
    }
    if (has_iub) {
        out->maxInlineUniformBlockSize = iub.maxInlineUniformBlockSize;
        out->maxPerStageDescriptorInlineUniformBlocks = iub.maxPerStageDescriptorInlineUniformBlocks;
        out->maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks = iub.maxPerStageDescriptorUpdateAfterBindInlineUniformBlocks;
        out->maxDescriptorSetInlineUniformBlocks = iub.maxDescriptorSetInlineUniformBlocks;
        out->maxDescriptorSetUpdateAfterBindInlineUniformBlocks = iub.maxDescriptorSetUpdateAfterBindInlineUniformBlocks;
    }
    // Texel buffers: REAL driver values (we do not lie about the alignment).
    if (has_tba) {
        out->storageTexelBufferOffsetAlignmentBytes       = tba.storageTexelBufferOffsetAlignmentBytes;
        out->storageTexelBufferOffsetSingleTexelAlignment = tba.storageTexelBufferOffsetSingleTexelAlignment;
        out->uniformTexelBufferOffsetAlignmentBytes       = tba.uniformTexelBufferOffsetAlignmentBytes;
        out->uniformTexelBufferOffsetSingleTexelAlignment = tba.uniformTexelBufferOffsetSingleTexelAlignment;
    } else {
        const VkDeviceSize a = p2.properties.limits.minTexelBufferOffsetAlignment;
        out->storageTexelBufferOffsetAlignmentBytes = a;
        out->uniformTexelBufferOffsetAlignmentBytes = a;
        out->storageTexelBufferOffsetSingleTexelAlignment = VK_FALSE;
        out->uniformTexelBufferOffsetSingleTexelAlignment = VK_FALSE;
    }
    out->maxBufferSize = m3.maxMemoryAllocationSize;   // emulated maintenance4: bounded by the allocation size

    VkEmuLog("[vk-emu] Vulkan13Properties rebuilt (ssc=%d iub=%d tba=%d): subgroup=%u..%u "
             "storageTexel{bytes=%llu single=%u} uniformTexel{bytes=%llu single=%u}\n",
             (int)has_ssc, (int)has_iub, (int)has_tba, out->minSubgroupSize, out->maxSubgroupSize,
             (unsigned long long)out->storageTexelBufferOffsetAlignmentBytes, (unsigned)out->storageTexelBufferOffsetSingleTexelAlignment,
             (unsigned long long)out->uniformTexelBufferOffsetAlignmentBytes, (unsigned)out->uniformTexelBufferOffsetSingleTexelAlignment);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties) {
    auto real = (PFN_vkGetPhysicalDeviceProperties2)FreshResolveAgainstCurrentInstance("vkGetPhysicalDeviceProperties2");
    if (!real) { LogFatal("vkGetPhysicalDeviceProperties2"); return; }
    real(physicalDevice, pProperties);
    if (pProperties) {
        pProperties->properties.apiVersion = VKEMU_SPOOFED_API_VERSION;
    }

    if (pProperties) {
        VkBaseOutStructure* cur = (VkBaseOutStructure*)pProperties->pNext;
        while (cur) {
            if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR) {
                auto* pd = (VkPhysicalDevicePushDescriptorPropertiesKHR*)cur;
                if (pd->maxPushDescriptors == 0 &&
                    !RealDeviceHasExtension(physicalDevice, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) {
                    pd->maxPushDescriptors = 32;
                }
            }
            else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES) {
                FillVulkan13Properties(physicalDevice, real, (VkPhysicalDeviceVulkan13Properties*)cur);
            }
            cur = cur->pNext;
        }
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetTessellationDomainOriginEXT(VkCommandBuffer cb, VkTessellationDomainOrigin v) { if (auto r = LazyResolve<PFN_vkCmdSetTessellationDomainOriginEXT>("vkCmdSetTessellationDomainOriginEXT")) r(cb, v); else LogStubCall("vkCmdSetTessellationDomainOriginEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetDepthClampEnableEXT(VkCommandBuffer cb, VkBool32 v) { if (auto r = LazyResolve<PFN_vkCmdSetDepthClampEnableEXT>("vkCmdSetDepthClampEnableEXT")) r(cb, v); else LogStubCall("vkCmdSetDepthClampEnableEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetPolygonModeEXT(VkCommandBuffer cb, VkPolygonMode v) { if (auto r = LazyResolve<PFN_vkCmdSetPolygonModeEXT>("vkCmdSetPolygonModeEXT")) r(cb, v); else LogStubCall("vkCmdSetPolygonModeEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetRasterizationSamplesEXT(VkCommandBuffer cb, VkSampleCountFlagBits v) { if (auto r = LazyResolve<PFN_vkCmdSetRasterizationSamplesEXT>("vkCmdSetRasterizationSamplesEXT")) r(cb, v); else LogStubCall("vkCmdSetRasterizationSamplesEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetSampleMaskEXT(VkCommandBuffer cb, VkSampleCountFlagBits s, const VkSampleMask* m) { if (auto r = LazyResolve<PFN_vkCmdSetSampleMaskEXT>("vkCmdSetSampleMaskEXT")) r(cb, s, m); else LogStubCall("vkCmdSetSampleMaskEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetAlphaToCoverageEnableEXT(VkCommandBuffer cb, VkBool32 v) { if (auto r = LazyResolve<PFN_vkCmdSetAlphaToCoverageEnableEXT>("vkCmdSetAlphaToCoverageEnableEXT")) r(cb, v); else LogStubCall("vkCmdSetAlphaToCoverageEnableEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetAlphaToOneEnableEXT(VkCommandBuffer cb, VkBool32 v) { if (auto r = LazyResolve<PFN_vkCmdSetAlphaToOneEnableEXT>("vkCmdSetAlphaToOneEnableEXT")) r(cb, v); else LogStubCall("vkCmdSetAlphaToOneEnableEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetLogicOpEnableEXT(VkCommandBuffer cb, VkBool32 v) { if (auto r = LazyResolve<PFN_vkCmdSetLogicOpEnableEXT>("vkCmdSetLogicOpEnableEXT")) r(cb, v); else LogStubCall("vkCmdSetLogicOpEnableEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetColorBlendEnableEXT(VkCommandBuffer cb, uint32_t firstAttachment, uint32_t attachmentCount, const VkBool32* pAttachmentEnables) { if (auto r = LazyResolve<PFN_vkCmdSetColorBlendEnableEXT>("vkCmdSetColorBlendEnableEXT")) r(cb, firstAttachment, attachmentCount, pAttachmentEnables); else LogStubCall("vkCmdSetColorBlendEnableEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetColorBlendEquationEXT(VkCommandBuffer cb, uint32_t firstAttachment, uint32_t attachmentCount, const VkColorBlendEquationEXT* pColorBlendEquations) { if (auto r = LazyResolve<PFN_vkCmdSetColorBlendEquationEXT>("vkCmdSetColorBlendEquationEXT")) r(cb, firstAttachment, attachmentCount, pColorBlendEquations); else LogStubCall("vkCmdSetColorBlendEquationEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetColorWriteMaskEXT(VkCommandBuffer cb, uint32_t firstAttachment, uint32_t attachmentCount, const VkColorComponentFlags* pColorWriteMasks) { if (auto r = LazyResolve<PFN_vkCmdSetColorWriteMaskEXT>("vkCmdSetColorWriteMaskEXT")) r(cb, firstAttachment, attachmentCount, pColorWriteMasks); else LogStubCall("vkCmdSetColorWriteMaskEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetRasterizationStreamEXT(VkCommandBuffer cb, uint32_t s) { if (auto r = LazyResolve<PFN_vkCmdSetRasterizationStreamEXT>("vkCmdSetRasterizationStreamEXT")) r(cb, s); else LogStubCall("vkCmdSetRasterizationStreamEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetConservativeRasterizationModeEXT(VkCommandBuffer cb, VkConservativeRasterizationModeEXT m) { if (auto r = LazyResolve<PFN_vkCmdSetConservativeRasterizationModeEXT>("vkCmdSetConservativeRasterizationModeEXT")) r(cb, m); else LogStubCall("vkCmdSetConservativeRasterizationModeEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetExtraPrimitiveOverestimationSizeEXT(VkCommandBuffer cb, float s) { if (auto r = LazyResolve<PFN_vkCmdSetExtraPrimitiveOverestimationSizeEXT>("vkCmdSetExtraPrimitiveOverestimationSizeEXT")) r(cb, s); else LogStubCall("vkCmdSetExtraPrimitiveOverestimationSizeEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetDepthClipEnableEXT(VkCommandBuffer cb, VkBool32 e) { if (auto r = LazyResolve<PFN_vkCmdSetDepthClipEnableEXT>("vkCmdSetDepthClipEnableEXT")) r(cb, e); else LogStubCall("vkCmdSetDepthClipEnableEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetSampleLocationsEnableEXT(VkCommandBuffer cb, VkBool32 e) { if (auto r = LazyResolve<PFN_vkCmdSetSampleLocationsEnableEXT>("vkCmdSetSampleLocationsEnableEXT")) r(cb, e); else LogStubCall("vkCmdSetSampleLocationsEnableEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetColorBlendAdvancedEXT(VkCommandBuffer cb, uint32_t firstAttachment, uint32_t attachmentCount, const VkColorBlendAdvancedEXT* pColorBlendAdvanced) { if (auto r = LazyResolve<PFN_vkCmdSetColorBlendAdvancedEXT>("vkCmdSetColorBlendAdvancedEXT")) r(cb, firstAttachment, attachmentCount, pColorBlendAdvanced); else LogStubCall("vkCmdSetColorBlendAdvancedEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetProvokingVertexModeEXT(VkCommandBuffer cb, VkProvokingVertexModeEXT m) { if (auto r = LazyResolve<PFN_vkCmdSetProvokingVertexModeEXT>("vkCmdSetProvokingVertexModeEXT")) r(cb, m); else LogStubCall("vkCmdSetProvokingVertexModeEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetLineRasterizationModeEXT(VkCommandBuffer cb, VkLineRasterizationModeEXT m) { if (auto r = LazyResolve<PFN_vkCmdSetLineRasterizationModeEXT>("vkCmdSetLineRasterizationModeEXT")) r(cb, m); else LogStubCall("vkCmdSetLineRasterizationModeEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetLineStippleEnableEXT(VkCommandBuffer cb, VkBool32 e) { if (auto r = LazyResolve<PFN_vkCmdSetLineStippleEnableEXT>("vkCmdSetLineStippleEnableEXT")) r(cb, e); else LogStubCall("vkCmdSetLineStippleEnableEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetDepthClipNegativeOneToOneEXT(VkCommandBuffer cb, VkBool32 e) { if (auto r = LazyResolve<PFN_vkCmdSetDepthClipNegativeOneToOneEXT>("vkCmdSetDepthClipNegativeOneToOneEXT")) r(cb, e); else LogStubCall("vkCmdSetDepthClipNegativeOneToOneEXT"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetViewportWScalingEnableNV(VkCommandBuffer cb, VkBool32 e) { if (auto r = LazyResolve<PFN_vkCmdSetViewportWScalingEnableNV>("vkCmdSetViewportWScalingEnableNV")) r(cb, e); else LogStubCall("vkCmdSetViewportWScalingEnableNV"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetViewportSwizzleNV(VkCommandBuffer cb, uint32_t firstViewport, uint32_t viewportCount, const VkViewportSwizzleNV* pViewportSwizzles) { if (auto r = LazyResolve<PFN_vkCmdSetViewportSwizzleNV>("vkCmdSetViewportSwizzleNV")) r(cb, firstViewport, viewportCount, pViewportSwizzles); else LogStubCall("vkCmdSetViewportSwizzleNV"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetCoverageToColorEnableNV(VkCommandBuffer cb, VkBool32 e) { if (auto r = LazyResolve<PFN_vkCmdSetCoverageToColorEnableNV>("vkCmdSetCoverageToColorEnableNV")) r(cb, e); else LogStubCall("vkCmdSetCoverageToColorEnableNV"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetCoverageToColorLocationNV(VkCommandBuffer cb, uint32_t l) { if (auto r = LazyResolve<PFN_vkCmdSetCoverageToColorLocationNV>("vkCmdSetCoverageToColorLocationNV")) r(cb, l); else LogStubCall("vkCmdSetCoverageToColorLocationNV"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetCoverageModulationModeNV(VkCommandBuffer cb, VkCoverageModulationModeNV m) { if (auto r = LazyResolve<PFN_vkCmdSetCoverageModulationModeNV>("vkCmdSetCoverageModulationModeNV")) r(cb, m); else LogStubCall("vkCmdSetCoverageModulationModeNV"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetCoverageModulationTableEnableNV(VkCommandBuffer cb, VkBool32 e) { if (auto r = LazyResolve<PFN_vkCmdSetCoverageModulationTableEnableNV>("vkCmdSetCoverageModulationTableEnableNV")) r(cb, e); else LogStubCall("vkCmdSetCoverageModulationTableEnableNV"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetCoverageModulationTableNV(VkCommandBuffer cb, uint32_t c, const float* t) { if (auto r = LazyResolve<PFN_vkCmdSetCoverageModulationTableNV>("vkCmdSetCoverageModulationTableNV")) r(cb, c, t); else LogStubCall("vkCmdSetCoverageModulationTableNV"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetShadingRateImageEnableNV(VkCommandBuffer cb, VkBool32 e) { if (auto r = LazyResolve<PFN_vkCmdSetShadingRateImageEnableNV>("vkCmdSetShadingRateImageEnableNV")) r(cb, e); else LogStubCall("vkCmdSetShadingRateImageEnableNV"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetRepresentativeFragmentTestEnableNV(VkCommandBuffer cb, VkBool32 e) { if (auto r = LazyResolve<PFN_vkCmdSetRepresentativeFragmentTestEnableNV>("vkCmdSetRepresentativeFragmentTestEnableNV")) r(cb, e); else LogStubCall("vkCmdSetRepresentativeFragmentTestEnableNV"); }
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetCoverageReductionModeNV(VkCommandBuffer cb, VkCoverageReductionModeNV m) { if (auto r = LazyResolve<PFN_vkCmdSetCoverageReductionModeNV>("vkCmdSetCoverageReductionModeNV")) r(cb, m); else LogStubCall("vkCmdSetCoverageReductionModeNV"); }


extern "C" __declspec(dllexport) VkDeviceAddress VKAPI_CALL
vkGetBufferDeviceAddress(VkDevice device, const VkBufferDeviceAddressInfo* pInfo) {
    auto real = LazyResolve<PFN_vkGetBufferDeviceAddress>("vkGetBufferDeviceAddress");
    VkDeviceAddress addr = 0;
    if (real) addr = real(device, pInfo);
    else {
        auto real_khr = LazyResolve<PFN_vkGetBufferDeviceAddressKHR>("vkGetBufferDeviceAddressKHR");
        if (real_khr) addr = real_khr(device, pInfo);
        else { LogFatal("vkGetBufferDeviceAddress"); return 0; }
    }
    if (addr != 0 && pInfo && pInfo->buffer != VK_NULL_HANDLE) {
        VkDeviceSize size = 0;
        auto real_req = LazyResolve<PFN_vkGetBufferMemoryRequirements2>("vkGetBufferMemoryRequirements2");
        if (real_req) {
            VkBufferMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
            info.buffer = pInfo->buffer;
            VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
            real_req(device, &info, &req);
            size = req.memoryRequirements.size;
        }
        if (size == 0) size = 1;
        RegisterBufferAddress(pInfo->buffer, addr, size);
    }
    return addr;
}

extern "C" __declspec(dllexport) VkDeviceAddress VKAPI_CALL
vkGetBufferDeviceAddressKHR(VkDevice device, const VkBufferDeviceAddressInfo* pInfo) {
    return vkGetBufferDeviceAddress(device, pInfo);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetDescriptorEXT(VkDevice device, const VkDescriptorGetInfoEXT* pDescriptorInfo,
                    size_t dataSize, void* pDescriptor) {
    if (!pDescriptorInfo || !pDescriptor) return;

    auto real = LazyResolve<PFN_vkGetDescriptorEXT>("vkGetDescriptorEXT");
    if (real) { real(device, pDescriptorInfo, dataSize, pDescriptor); return; }

    if (dataSize < sizeof(DescSlot)) {
        LogStubCall("vkGetDescriptorEXT (dataSize too small for the emulation -- check "
                     "vkGetDescriptorSetLayoutSizeEXT/BindingOffsetEXT on the caller side)");
        return;
    }

    DescSlot slot{};
    slot.type = pDescriptorInfo->type;
    switch (pDescriptorInfo->type) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            if (pDescriptorInfo->data.pSampler) slot.sampler = *pDescriptorInfo->data.pSampler;
            break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            if (pDescriptorInfo->data.pCombinedImageSampler) {
                slot.sampler = pDescriptorInfo->data.pCombinedImageSampler->sampler;
                slot.imageView = pDescriptorInfo->data.pCombinedImageSampler->imageView;
                slot.imageLayout = pDescriptorInfo->data.pCombinedImageSampler->imageLayout;
            }
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            if (pDescriptorInfo->data.pSampledImage) {
                slot.imageView = pDescriptorInfo->data.pSampledImage->imageView;
                slot.imageLayout = pDescriptorInfo->data.pSampledImage->imageLayout;
            }
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
            if (pDescriptorInfo->data.pStorageImage) {
                slot.imageView = pDescriptorInfo->data.pStorageImage->imageView;
                slot.imageLayout = pDescriptorInfo->data.pStorageImage->imageLayout;
            }
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            if (pDescriptorInfo->data.pUniformTexelBuffer) {
                slot.bufferAddress = pDescriptorInfo->data.pUniformTexelBuffer->address;
                slot.bufferRange = pDescriptorInfo->data.pUniformTexelBuffer->range;
                slot.texelBufferFormat = pDescriptorInfo->data.pUniformTexelBuffer->format;
            }
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            if (pDescriptorInfo->data.pStorageTexelBuffer) {
                slot.bufferAddress = pDescriptorInfo->data.pStorageTexelBuffer->address;
                slot.bufferRange = pDescriptorInfo->data.pStorageTexelBuffer->range;
                slot.texelBufferFormat = pDescriptorInfo->data.pStorageTexelBuffer->format;
            }
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            if (pDescriptorInfo->data.pUniformBuffer) {
                slot.bufferAddress = pDescriptorInfo->data.pUniformBuffer->address;
                slot.bufferRange = pDescriptorInfo->data.pUniformBuffer->range;
            }
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            if (pDescriptorInfo->data.pStorageBuffer) {
                slot.bufferAddress = pDescriptorInfo->data.pStorageBuffer->address;
                slot.bufferRange = pDescriptorInfo->data.pStorageBuffer->range;
            }
            break;
        default:
            LogStubCall("vkGetDescriptorEXT (descriptor type not emulated)");
            break;
    }
    std::memcpy(pDescriptor, &slot, sizeof(DescSlot));
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetDescriptorSetLayoutSizeEXT(VkDevice device, VkDescriptorSetLayout layout, VkDeviceSize* pLayoutSizeInBytes) {
    auto real = LazyResolve<PFN_vkGetDescriptorSetLayoutSizeEXT>("vkGetDescriptorSetLayoutSizeEXT");
    if (real) { real(device, layout, pLayoutSizeInBytes); return; }

    if (pLayoutSizeInBytes) *pLayoutSizeInBytes = 0; // filled dynamically, see note below
    LogStubCall("vkGetDescriptorSetLayoutSizeEXT (emulation -- size computed "
                "conservatively, see vkGetDescriptorSetLayoutBindingOffsetEXT)");

    std::lock_guard<std::mutex> lock(g_descSetLayoutInfoMutex);
    auto it = g_descSetLayoutInfo.find(layout);
    if (it != g_descSetLayoutInfo.end() && pLayoutSizeInBytes) {
        *pLayoutSizeInBytes = it->second.bindings.size() * sizeof(DescSlot);
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetDescriptorSetLayoutBindingOffsetEXT(VkDevice device, VkDescriptorSetLayout layout, uint32_t binding, VkDeviceSize* pOffset) {
    auto real = LazyResolve<PFN_vkGetDescriptorSetLayoutBindingOffsetEXT>("vkGetDescriptorSetLayoutBindingOffsetEXT");
    if (real) { real(device, layout, binding, pOffset); return; }

    std::lock_guard<std::mutex> lock(g_descSetLayoutInfoMutex);
    auto it = g_descSetLayoutInfo.find(layout);
    if (it == g_descSetLayoutInfo.end() || !pOffset) {
        LogStubCall("vkGetDescriptorSetLayoutBindingOffsetEXT (unknown layout -- "
                     "created before the descriptor_buffer emulation was enabled?)");
        if (pOffset) *pOffset = 0;
        return;
    }
        for (size_t i = 0; i < it->second.bindings.size(); i++) {
        if (it->second.bindings[i].binding == binding) {
            *pOffset = (VkDeviceSize)i * sizeof(DescSlot);
            return;
        }
    }
    *pOffset = 0;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdBindDescriptorBuffersEXT(VkCommandBuffer commandBuffer, uint32_t bufferCount,
                               const VkDescriptorBufferBindingInfoEXT* pBindingInfos) {
    auto real = LazyResolve<PFN_vkCmdBindDescriptorBuffersEXT>("vkCmdBindDescriptorBuffersEXT");
    if (real) { real(commandBuffer, bufferCount, pBindingInfos); return; }

    std::lock_guard<std::mutex> lock(g_cmdDescBufMutex);
    auto& bound = g_cmdBoundDescBuffers[commandBuffer];
    bound.clear();
    bound.resize(bufferCount);
    for (uint32_t i = 0; i < bufferCount; i++)
        bound[i].address = pBindingInfos[i].address;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdSetDescriptorBufferOffsetsEXT(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
                                    VkPipelineLayout layout, uint32_t firstSet, uint32_t setCount,
                                    const uint32_t* pBufferIndices, const VkDeviceSize* pOffsets) {
    auto real = LazyResolve<PFN_vkCmdSetDescriptorBufferOffsetsEXT>("vkCmdSetDescriptorBufferOffsetsEXT");
    if (real) { real(commandBuffer, pipelineBindPoint, layout, firstSet, setCount, pBufferIndices, pOffsets); return; }

    VkDevice device = GetDeviceForCommandBuffer(commandBuffer);
    if (device == VK_NULL_HANDLE) {
        LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (unknown device for this command buffer)");
        return;
    }

    std::vector<BoundDescBuffer> boundCopy;
    {
        std::lock_guard<std::mutex> lock(g_cmdDescBufMutex);
        auto it = g_cmdBoundDescBuffers.find(commandBuffer);
        if (it == g_cmdBoundDescBuffers.end()) {
            LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (no descriptor buffer bound -- "
                         "vkCmdBindDescriptorBuffersEXT missing before this call)");
            return;
        }
        boundCopy = it->second; // copy to avoid holding the lock during allocations
    }

    auto real_getLayoutSupport = LazyResolve<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout");
    (void)real_getLayoutSupport; // reserved for a future extension if validation is needed

    for (uint32_t s = 0; s < setCount; s++) {
        uint32_t bufIdx = pBufferIndices[s];
        if (bufIdx >= boundCopy.size()) {
            LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (descriptor buffer index out of bounds)");
            continue;
        }
        VkDeviceAddress bufBase = boundCopy[bufIdx].address;
        VkDeviceAddress slotsAddr = bufBase + pOffsets[s];

        VkDeviceSize relOffset = 0;
        VkBuffer realBuffer = ResolveBufferFromAddress(slotsAddr, &relOffset);
        if (realBuffer == VK_NULL_HANDLE) {
            LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (unable to resolve the address "
                        "of the descriptor buffer -- vkGetBufferDeviceAddress may not have "
                        "gone through this wrapper for this buffer)");
            continue;
        }

        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(g_pipelineLayoutSetsMutex);
            auto it = g_pipelineLayoutSets.find(layout);
            if (it != g_pipelineLayoutSets.end() && (firstSet + s) < it->second.size())
                setLayout = it->second[firstSet + s];
        }
        if (setLayout == VK_NULL_HANDLE) {
            LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (set layout not found -- "
                        "vkCreatePipelineLayout not tracked for this VkPipelineLayout)");
            continue;
        }

                std::vector<DescSetLayoutBindingInfo> bindingInfos;
        {
            std::lock_guard<std::mutex> lock(g_descSetLayoutInfoMutex);
            auto it = g_descSetLayoutInfo.find(setLayout);
            if (it == g_descSetLayoutInfo.end()) {
                LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (untracked VkDescriptorSetLayout)");
                continue;
            }
            bindingInfos = it->second.bindings;
        }

        void* mappedBase = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_activeMappingsMutex);
            auto it = g_activeMappings.find(realBuffer);
            if (it != g_activeMappings.end()) mappedBase = it->second;
        }
        if (!mappedBase) {
            LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (descriptor buffer not mapped "
                        "on the CPU side -- the game must keep this buffer permanently mapped, "
                        "as required by VK_EXT_descriptor_buffer)");
            continue;
        }

        const uint8_t* slotsBase = (const uint8_t*)mappedBase + relOffset;
        size_t contentHash = 0;
        std::vector<DescSlot> slots(bindingInfos.size());
        for (size_t i = 0; i < bindingInfos.size(); i++) {
            std::memcpy(&slots[i], slotsBase + i * sizeof(DescSlot), sizeof(DescSlot));

            const uint8_t* raw = (const uint8_t*)&slots[i];
            for (size_t b = 0; b < sizeof(DescSlot); b++)
                contentHash = contentHash * 1099511628211ull ^ raw[b];
        }

        EmuDescSetKey key{setLayout, contentHash};
        VkDescriptorSet realSet = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(g_descBufPoolMutex);
            auto& cache = g_descBufSetCache[device];
            auto cit = cache.find(key);
            if (cit != cache.end()) realSet = cit->second;
        }

        if (realSet == VK_NULL_HANDLE) {
            VkDescriptorPool pool = GetOrCreateEmuDescPool(device);
            if (pool == VK_NULL_HANDLE) {
                LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (internal pool creation failed)");
                continue;
            }
            auto real_alloc = LazyResolve<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets");
            auto real_update = LazyResolve<PFN_vkUpdateDescriptorSets>("vkUpdateDescriptorSets");
            if (!real_alloc || !real_update) continue;

            VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dsai.descriptorPool = pool;
            dsai.descriptorSetCount = 1;
            dsai.pSetLayouts = &setLayout;
            VkResult ar = real_alloc(device, &dsai, &realSet);
            if (ar != VK_SUCCESS) {

                std::lock_guard<std::mutex> lock(g_descBufPoolMutex);
                auto& pools = g_descBufPools[device];
                if (!pools.empty()) pools.back().allocatedCount = EmuDescBufferPool::kMaxSetsPerPool;
                LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (internal pool saturated, "
                            "a new pool will be allocated on the next call)");
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(g_descBufPoolMutex);
                auto& pools = g_descBufPools[device];
                if (!pools.empty()) pools.back().allocatedCount++;
            }
            std::vector<VkWriteDescriptorSet> writes;
            std::vector<VkDescriptorImageInfo> imageInfos(slots.size());
            std::vector<VkDescriptorBufferInfo> bufferInfos(slots.size());
            writes.reserve(slots.size());
            for (size_t i = 0; i < slots.size(); i++) {
                const DescSlot& slot = slots[i];
                if (slot.type == VK_DESCRIPTOR_TYPE_MAX_ENUM) continue;

                VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet = realSet;
                w.dstBinding = bindingInfos[i].binding;
                w.descriptorCount = 1;
                w.descriptorType = slot.type;

                switch (slot.type) {
                    case VK_DESCRIPTOR_TYPE_SAMPLER:
                        imageInfos[i].sampler = slot.sampler;
                        w.pImageInfo = &imageInfos[i];
                        break;
                    case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                    case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                    case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                    case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                        imageInfos[i].sampler = slot.sampler;
                        imageInfos[i].imageView = slot.imageView;
                        imageInfos[i].imageLayout = slot.imageLayout;
                        w.pImageInfo = &imageInfos[i];
                        break;
                    case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                    case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: {
                        VkDeviceSize relBufOffset = 0;
                        VkBuffer b = ResolveBufferFromAddress(slot.bufferAddress, &relBufOffset);
                        if (b == VK_NULL_HANDLE) continue;
                        bufferInfos[i].buffer = b;
                        bufferInfos[i].offset = relBufOffset;
                        bufferInfos[i].range = slot.bufferRange;
                        w.pBufferInfo = &bufferInfos[i];
                        break;
                    }
                    default:
                        continue;
                }
                writes.push_back(w);
            }
            if (!writes.empty()) real_update(device, (uint32_t)writes.size(), writes.data(), 0, nullptr);

            std::lock_guard<std::mutex> lock(g_descBufPoolMutex);
            g_descBufSetCache[device][key] = realSet;
        }

        auto real_bind = LazyResolve<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");
        if (real_bind) real_bind(commandBuffer, pipelineBindPoint, layout, firstSet + s, 1, &realSet, 0, nullptr);
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdBindDescriptorBufferEmbeddedSamplersEXT(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
                                              VkPipelineLayout layout, uint32_t set) {
    auto real = LazyResolve<PFN_vkCmdBindDescriptorBufferEmbeddedSamplersEXT>("vkCmdBindDescriptorBufferEmbeddedSamplersEXT");
    if (real) { real(commandBuffer, pipelineBindPoint, layout, set); return; }
    LogStubCall("vkCmdBindDescriptorBufferEmbeddedSamplersEXT (no-op: immutable samplers "
                "already handled via the regular VkDescriptorSetLayout)");
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdDrawMeshTasksEXT(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    auto real = LazyResolve<PFN_vkCmdDrawMeshTasksEXT>("vkCmdDrawMeshTasksEXT");
    if (real) { real(commandBuffer, groupCountX, groupCountY, groupCountZ); return; }
    LogStubCall("vkCmdDrawMeshTasksEXT (VK_EXT_mesh_shader missing from the real driver/GPU -- no software fallback possible)");
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdDrawMeshTasksIndirectEXT(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                               uint32_t drawCount, uint32_t stride) {
    auto real = LazyResolve<PFN_vkCmdDrawMeshTasksIndirectEXT>("vkCmdDrawMeshTasksIndirectEXT");
    if (real) { real(commandBuffer, buffer, offset, drawCount, stride); return; }
    LogStubCall("vkCmdDrawMeshTasksIndirectEXT (VK_EXT_mesh_shader missing from the real driver/GPU)");
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdDrawMeshTasksIndirectCountEXT(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                                    VkBuffer countBuffer, VkDeviceSize countBufferOffset,
                                    uint32_t maxDrawCount, uint32_t stride) {
    auto real = LazyResolve<PFN_vkCmdDrawMeshTasksIndirectCountEXT>("vkCmdDrawMeshTasksIndirectCountEXT");
    if (real) { real(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride); return; }
    LogStubCall("vkCmdDrawMeshTasksIndirectCountEXT (VK_EXT_mesh_shader missing from the real driver/GPU)");
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdSetVertexInputEXT(VkCommandBuffer commandBuffer,
                        uint32_t vertexBindingDescriptionCount,
                        const VkVertexInputBindingDescription2EXT* pVertexBindingDescriptions,
                        uint32_t vertexAttributeDescriptionCount,
                        const VkVertexInputAttributeDescription2EXT* pVertexAttributeDescriptions) {
    auto real = LazyResolve<PFN_vkCmdSetVertexInputEXT>("vkCmdSetVertexInputEXT");
    if (real) {
        real(commandBuffer, vertexBindingDescriptionCount, pVertexBindingDescriptions,
             vertexAttributeDescriptionCount, pVertexAttributeDescriptions);
        return;
    }

    LogStubCall("vkCmdSetVertexInputEXT (extension missing from the real driver -- no software fallback)");
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreatePrivateDataSlot(VkDevice device, const VkPrivateDataSlotCreateInfo* pCreateInfo,
                        const VkAllocationCallbacks* pAllocator, VkPrivateDataSlot* pPrivateDataSlot) {
    auto real = LazyResolve<PFN_vkCreatePrivateDataSlot>("vkCreatePrivateDataSlot");
    if (real) return real(device, pCreateInfo, pAllocator, pPrivateDataSlot);
    
    auto real_ext = LazyResolve<PFN_vkCreatePrivateDataSlotEXT>("vkCreatePrivateDataSlotEXT");
    if (real_ext) return real_ext(device, pCreateInfo, pAllocator, pPrivateDataSlot);

    LogStubCall("vkCreatePrivateDataSlot");
    if (pPrivateDataSlot) {
        *pPrivateDataSlot = (VkPrivateDataSlot)(uintptr_t)0x1;
    }
    return VK_SUCCESS;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkDestroyPrivateDataSlot(VkDevice device, VkPrivateDataSlot privateDataSlot, const VkAllocationCallbacks* pAllocator) {
    auto real = LazyResolve<PFN_vkDestroyPrivateDataSlot>("vkDestroyPrivateDataSlot");
    if (real) { real(device, privateDataSlot, pAllocator); return; }
    
    auto real_ext = LazyResolve<PFN_vkDestroyPrivateDataSlotEXT>("vkDestroyPrivateDataSlotEXT");
    if (real_ext) { real_ext(device, privateDataSlot, pAllocator); return; }

    LogStubCall("vkDestroyPrivateDataSlot");
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkSetPrivateData(VkDevice device, VkObjectType objectType, uint64_t objectHandle, VkPrivateDataSlot privateDataSlot, uint64_t data) {
    auto real = LazyResolve<PFN_vkSetPrivateData>("vkSetPrivateData");
    if (real) return real(device, objectType, objectHandle, privateDataSlot, data);
    
    auto real_ext = LazyResolve<PFN_vkSetPrivateDataEXT>("vkSetPrivateDataEXT");
    if (real_ext) return real_ext(device, objectType, objectHandle, privateDataSlot, data);

    LogStubCall("vkSetPrivateData");
    return VK_SUCCESS;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetPrivateData(VkDevice device, VkObjectType objectType, uint64_t objectHandle, VkPrivateDataSlot privateDataSlot, uint64_t* pData) {
    auto real = LazyResolve<PFN_vkGetPrivateData>("vkGetPrivateData");
    if (real) { real(device, objectType, objectHandle, privateDataSlot, pData); return; }
    
    auto real_ext = LazyResolve<PFN_vkGetPrivateDataEXT>("vkGetPrivateDataEXT");
    if (real_ext) { real_ext(device, objectType, objectHandle, privateDataSlot, pData); return; }

    LogStubCall("vkGetPrivateData");
    if (pData) *pData = 0;
}

// 7. vkGetInstanceProcAddr / vkGetDeviceProcAddr : stock path recommended for everything that isnt core 1.0

// Forwards

extern "C" __declspec(dllexport) VkResult VKAPI_CALL vkEmuNullStubRet0() {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

extern "C" __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName);

extern "C" __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* pName);

static const std::unordered_map<std::string, PFN_vkVoidFunction> g_ourExports = {
    // Proxy to fix missing caps
    { "vkGetInstanceProcAddr",            (PFN_vkVoidFunction)vkGetInstanceProcAddr },
    { "vkGetDeviceProcAddr",              (PFN_vkVoidFunction)vkGetDeviceProcAddr },

    { "vkCreateInstance",           (PFN_vkVoidFunction)vkCreateInstance },
    { "vkDestroyInstance",          (PFN_vkVoidFunction)vkDestroyInstance },

    { "vkCreateWin32SurfaceKHR",                  (PFN_vkVoidFunction)vkCreateWin32SurfaceKHR },
    { "vkDestroyDevice",                          (PFN_vkVoidFunction)vkDestroyDevice },
    { "vkDestroySurfaceKHR",                      (PFN_vkVoidFunction)vkDestroySurfaceKHR },
    { "vkEnumerateDeviceLayerProperties",         (PFN_vkVoidFunction)vkEnumerateDeviceLayerProperties },
    { "vkEnumerateInstanceExtensionProperties",   (PFN_vkVoidFunction)vkEnumerateInstanceExtensionProperties },
    { "vkEnumerateInstanceLayerProperties",       (PFN_vkVoidFunction)vkEnumerateInstanceLayerProperties },
    { "vkEnumeratePhysicalDevices",               (PFN_vkVoidFunction)vkEnumeratePhysicalDevices },
    { "vkGetPhysicalDeviceFeatures",              (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures },
    { "vkGetPhysicalDeviceFormatProperties",      (PFN_vkVoidFunction)vkGetPhysicalDeviceFormatProperties },
    { "vkGetPhysicalDeviceFormatProperties2",     (PFN_vkVoidFunction)vkGetPhysicalDeviceFormatProperties2 },
    { "vkGetPhysicalDeviceFormatProperties2KHR",  (PFN_vkVoidFunction)vkGetPhysicalDeviceFormatProperties2 },
    { "vkGetPhysicalDeviceMemoryProperties",      (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties },
    { "vkGetPhysicalDeviceQueueFamilyProperties",  (PFN_vkVoidFunction)vkGetPhysicalDeviceQueueFamilyProperties },
    { "vkGetPhysicalDeviceQueueFamilyProperties2", (PFN_vkVoidFunction)vkGetPhysicalDeviceQueueFamilyProperties2 },
    { "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceCapabilitiesKHR },
    { "vkGetPhysicalDeviceSurfaceFormatsKHR",      (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfaceFormatsKHR },
    { "vkGetPhysicalDeviceSurfacePresentModesKHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceSurfacePresentModesKHR },
    { "vkEnumerateInstanceVersion", (PFN_vkVoidFunction)vkEnumerateInstanceVersion },
    { "vkEnumerateDeviceExtensionProperties", (PFN_vkVoidFunction)vkEnumerateDeviceExtensionProperties },
    { "vkGetPhysicalDeviceFeatures2",         (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures2 },
    { "vkGetPhysicalDeviceFeatures2KHR",      (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures2 },
    { "vkCreateDevice",                       (PFN_vkVoidFunction)vkCreateDevice },
    { "vkCmdBeginRendering",        (PFN_vkVoidFunction)vkCmdBeginRendering },
    { "vkCmdBeginRenderingKHR",     (PFN_vkVoidFunction)vkCmdBeginRendering },
    { "vkCmdEndRendering",          (PFN_vkVoidFunction)vkCmdEndRendering },
    { "vkCmdEndRenderingKHR",       (PFN_vkVoidFunction)vkCmdEndRendering },
    { "vkCmdSetLineStipple",              (PFN_vkVoidFunction)vkCmdSetLineStipple },
    { "vkCmdBindIndexBuffer2",            (PFN_vkVoidFunction)vkCmdBindIndexBuffer2 },
    { "vkMapMemory2",                     (PFN_vkVoidFunction)vkMapMemory2 },
    { "vkUnmapMemory2",                   (PFN_vkVoidFunction)vkUnmapMemory2 },
    { "vkCmdSetRenderingAttachmentLocations",    (PFN_vkVoidFunction)vkCmdSetRenderingAttachmentLocations },
    { "vkCmdSetRenderingInputAttachmentIndices", (PFN_vkVoidFunction)vkCmdSetRenderingInputAttachmentIndices },
    { "vkGetDeviceImageMemoryRequirements",       (PFN_vkVoidFunction)vkGetDeviceImageMemoryRequirements },
    { "vkGetDeviceImageSparseMemoryRequirements", (PFN_vkVoidFunction)vkGetDeviceImageSparseMemoryRequirements },
    { "vkGetDeviceBufferMemoryRequirements",      (PFN_vkVoidFunction)vkGetDeviceBufferMemoryRequirements },
    { "vkGetRenderingAreaGranularity",            (PFN_vkVoidFunction)vkGetRenderingAreaGranularity },
    { "vkCopyMemoryToImage",   (PFN_vkVoidFunction)vkCopyMemoryToImage },
    { "vkCopyImageToMemory",   (PFN_vkVoidFunction)vkCopyImageToMemory },
    { "vkCopyImageToImage",    (PFN_vkVoidFunction)vkCopyImageToImage },
    { "vkTransitionImageLayout", (PFN_vkVoidFunction)vkTransitionImageLayout },
    { "vkCreateImage",              (PFN_vkVoidFunction)vkCreateImage },
    { "vkDestroyImage",              (PFN_vkVoidFunction)vkDestroyImage },
    { "vkCreateImageView",           (PFN_vkVoidFunction)vkCreateImageView },
    { "vkCreateSwapchainKHR",        (PFN_vkVoidFunction)vkCreateSwapchainKHR },
    { "vkGetSwapchainImagesKHR",     (PFN_vkVoidFunction)vkGetSwapchainImagesKHR },
    { "vkDestroySwapchainKHR",       (PFN_vkVoidFunction)vkDestroySwapchainKHR },
    { "vkDestroyImageView",          (PFN_vkVoidFunction)vkDestroyImageView },
    { "vkAllocateCommandBuffers",    (PFN_vkVoidFunction)vkAllocateCommandBuffers },
    { "vkFreeCommandBuffers",        (PFN_vkVoidFunction)vkFreeCommandBuffers },
    { "vkCmdPipelineBarrier2",            (PFN_vkVoidFunction)vkCmdPipelineBarrier2 },
    { "vkQueueSubmit2",                   (PFN_vkVoidFunction)vkQueueSubmit2 },
    { "vkCmdWriteTimestamp2",             (PFN_vkVoidFunction)vkCmdWriteTimestamp2 },
    { "vkCmdSetEvent2",                   (PFN_vkVoidFunction)vkCmdSetEvent2 },
    { "vkCmdResetEvent2",                 (PFN_vkVoidFunction)vkCmdResetEvent2 },
    { "vkCmdPushDescriptorSetKHR",        (PFN_vkVoidFunction)vkCmdPushDescriptorSetKHR },
    { "vkCmdPushDescriptorSet2KHR",       (PFN_vkVoidFunction)vkCmdPushDescriptorSet2KHR },
    { "vkCreateGraphicsPipelines",         (PFN_vkVoidFunction)vkCreateGraphicsPipelines },
    { "vkGetPhysicalDeviceProperties",     (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties },
    { "vkGetPhysicalDeviceProperties2",    (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties2 },
    { "vkGetPhysicalDeviceProperties2KHR", (PFN_vkVoidFunction)vkGetPhysicalDeviceProperties2 },
    { "vkCmdCopyBuffer2",                  (PFN_vkVoidFunction)vkCmdCopyBuffer2 },
    { "vkCmdCopyImage2",                   (PFN_vkVoidFunction)vkCmdCopyImage2 },
    { "vkCmdCopyBufferToImage2",           (PFN_vkVoidFunction)vkCmdCopyBufferToImage2 },
    { "vkCmdCopyImageToBuffer2",           (PFN_vkVoidFunction)vkCmdCopyImageToBuffer2 },
    { "vkCmdBlitImage2",                   (PFN_vkVoidFunction)vkCmdBlitImage2 },
    { "vkCmdResolveImage2",                (PFN_vkVoidFunction)vkCmdResolveImage2 },
    { "vkCmdSetCullMode",                  (PFN_vkVoidFunction)vkCmdSetCullMode },
    { "vkCmdSetFrontFace",                 (PFN_vkVoidFunction)vkCmdSetFrontFace },
    { "vkCmdSetPrimitiveTopology",         (PFN_vkVoidFunction)vkCmdSetPrimitiveTopology },
    { "vkCmdSetViewportWithCount",         (PFN_vkVoidFunction)vkCmdSetViewportWithCount },
    { "vkCmdSetScissorWithCount",          (PFN_vkVoidFunction)vkCmdSetScissorWithCount },
    { "vkCmdBindVertexBuffers2",           (PFN_vkVoidFunction)vkCmdBindVertexBuffers2 },
    { "vkCmdSetDepthTestEnable",           (PFN_vkVoidFunction)vkCmdSetDepthTestEnable },
    { "vkCmdSetDepthWriteEnable",          (PFN_vkVoidFunction)vkCmdSetDepthWriteEnable },
    { "vkCmdSetDepthCompareOp",            (PFN_vkVoidFunction)vkCmdSetDepthCompareOp },
    { "vkCmdSetDepthBoundsTestEnable",     (PFN_vkVoidFunction)vkCmdSetDepthBoundsTestEnable },
    { "vkCmdSetStencilTestEnable",         (PFN_vkVoidFunction)vkCmdSetStencilTestEnable },
    { "vkCmdSetStencilOp",                 (PFN_vkVoidFunction)vkCmdSetStencilOp },
    { "vkCmdSetDepthBiasEnable",           (PFN_vkVoidFunction)vkCmdSetDepthBiasEnable },
    { "vkCmdSetPrimitiveRestartEnable",    (PFN_vkVoidFunction)vkCmdSetPrimitiveRestartEnable },
    { "vkCmdSetRasterizerDiscardEnable",   (PFN_vkVoidFunction)vkCmdSetRasterizerDiscardEnable },
    { "vkCmdWaitEvents2",                  (PFN_vkVoidFunction)vkCmdWaitEvents2 },

    // VK_KHR_synchronization2
    { "vkCmdPipelineBarrier2KHR",   (PFN_vkVoidFunction)vkCmdPipelineBarrier2 },
    { "vkQueueSubmit2KHR",          (PFN_vkVoidFunction)vkQueueSubmit2 },
    { "vkCmdWriteTimestamp2KHR",    (PFN_vkVoidFunction)vkCmdWriteTimestamp2 },
    { "vkCmdSetEvent2KHR",          (PFN_vkVoidFunction)vkCmdSetEvent2 },
    { "vkCmdResetEvent2KHR",        (PFN_vkVoidFunction)vkCmdResetEvent2 },
    { "vkCmdWaitEvents2KHR",        (PFN_vkVoidFunction)vkCmdWaitEvents2 },

    // VK_KHR_copy_commands2
    { "vkCmdCopyBuffer2KHR",          (PFN_vkVoidFunction)vkCmdCopyBuffer2 },
    { "vkCmdCopyImage2KHR",           (PFN_vkVoidFunction)vkCmdCopyImage2 },
    { "vkCmdCopyBufferToImage2KHR",   (PFN_vkVoidFunction)vkCmdCopyBufferToImage2 },
    { "vkCmdCopyImageToBuffer2KHR",   (PFN_vkVoidFunction)vkCmdCopyImageToBuffer2 },
    { "vkCmdBlitImage2KHR",           (PFN_vkVoidFunction)vkCmdBlitImage2 },
    { "vkCmdResolveImage2KHR",        (PFN_vkVoidFunction)vkCmdResolveImage2 },

    // VK_EXT_extended_dynamic_state
    { "vkCmdSetCullModeEXT",            (PFN_vkVoidFunction)vkCmdSetCullMode },
    { "vkCmdSetFrontFaceEXT",           (PFN_vkVoidFunction)vkCmdSetFrontFace },
    { "vkCmdSetPrimitiveTopologyEXT",   (PFN_vkVoidFunction)vkCmdSetPrimitiveTopology },
    { "vkCmdSetViewportWithCountEXT",   (PFN_vkVoidFunction)vkCmdSetViewportWithCount },
    { "vkCmdSetScissorWithCountEXT",    (PFN_vkVoidFunction)vkCmdSetScissorWithCount },
    { "vkCmdBindVertexBuffers2EXT",     (PFN_vkVoidFunction)vkCmdBindVertexBuffers2 },
    { "vkCmdSetDepthTestEnableEXT",     (PFN_vkVoidFunction)vkCmdSetDepthTestEnable },
    { "vkCmdSetDepthWriteEnableEXT",    (PFN_vkVoidFunction)vkCmdSetDepthWriteEnable },
    { "vkCmdSetDepthCompareOpEXT",      (PFN_vkVoidFunction)vkCmdSetDepthCompareOp },
    { "vkCmdSetDepthBoundsTestEnableEXT", (PFN_vkVoidFunction)vkCmdSetDepthBoundsTestEnable },
    { "vkCmdSetStencilTestEnableEXT",   (PFN_vkVoidFunction)vkCmdSetStencilTestEnable },
    { "vkCmdSetStencilOpEXT",           (PFN_vkVoidFunction)vkCmdSetStencilOp },

    // VK_EXT_extended_dynamic_state2
    { "vkCmdSetDepthBiasEnableEXT",         (PFN_vkVoidFunction)vkCmdSetDepthBiasEnable },
    { "vkCmdSetPrimitiveRestartEnableEXT",  (PFN_vkVoidFunction)vkCmdSetPrimitiveRestartEnable },
    { "vkCmdSetRasterizerDiscardEnableEXT", (PFN_vkVoidFunction)vkCmdSetRasterizerDiscardEnable },
    { "vkCmdSetLogicOpEXT",                 (PFN_vkVoidFunction)vkCmdSetLogicOpEXT },
    { "vkCmdSetPatchControlPointsEXT",      (PFN_vkVoidFunction)vkCmdSetPatchControlPointsEXT },

    // VK_KHR_maintenance4
    { "vkGetDeviceImageMemoryRequirementsKHR",       (PFN_vkVoidFunction)vkGetDeviceImageMemoryRequirements },
    { "vkGetDeviceImageSparseMemoryRequirementsKHR", (PFN_vkVoidFunction)vkGetDeviceImageSparseMemoryRequirements },
    { "vkGetDeviceBufferMemoryRequirementsKHR",      (PFN_vkVoidFunction)vkGetDeviceBufferMemoryRequirements },

    // VK_KHR_map_memory2
    { "vkMapMemory2KHR",   (PFN_vkVoidFunction)vkMapMemory2 },
    { "vkUnmapMemory2KHR", (PFN_vkVoidFunction)vkUnmapMemory2 },

    // VK_KHR_maintenance5 / VK_KHR_dynamic_rendering_local_read (1.4)
    { "vkCmdBindIndexBuffer2KHR",                   (PFN_vkVoidFunction)vkCmdBindIndexBuffer2 },
    { "vkCmdSetRenderingAttachmentLocationsKHR",    (PFN_vkVoidFunction)vkCmdSetRenderingAttachmentLocations },
    { "vkCmdSetRenderingInputAttachmentIndicesKHR", (PFN_vkVoidFunction)vkCmdSetRenderingInputAttachmentIndices },
    { "vkGetRenderingAreaGranularityKHR",           (PFN_vkVoidFunction)vkGetRenderingAreaGranularity },

    // VK_EXT_host_image_copy (1.4)
    { "vkCopyMemoryToImageEXT",    (PFN_vkVoidFunction)vkCopyMemoryToImage },
    { "vkCopyImageToMemoryEXT",    (PFN_vkVoidFunction)vkCopyImageToMemory },
    { "vkCopyImageToImageEXT",     (PFN_vkVoidFunction)vkCopyImageToImage },
    { "vkTransitionImageLayoutEXT", (PFN_vkVoidFunction)vkTransitionImageLayout },
    
    { "vkCmdBindDescriptorSets2",           (PFN_vkVoidFunction)vkCmdBindDescriptorSets2 },
    { "vkCmdPushConstants2",                (PFN_vkVoidFunction)vkCmdPushConstants2 },
    { "vkCmdPushDescriptorSetWithTemplate2", (PFN_vkVoidFunction)vkCmdPushDescriptorSetWithTemplate2 },
    { "vkCmdPushDescriptorSetWithTemplate2KHR", (PFN_vkVoidFunction)vkCmdPushDescriptorSetWithTemplate2KHR },
    { "vkCmdPushDescriptorSetWithTemplateKHR", (PFN_vkVoidFunction)vkCmdPushDescriptorSetWithTemplateKHR },
    { "vkCreateDescriptorUpdateTemplate", (PFN_vkVoidFunction)vkCreateDescriptorUpdateTemplate },
    { "vkCreateDescriptorUpdateTemplateKHR", (PFN_vkVoidFunction)vkCreateDescriptorUpdateTemplateKHR },

    // Stubs for raytracing (VK_EXT_extended_dynamic_state3)
    { "vkCmdSetTessellationDomainOriginEXT", (PFN_vkVoidFunction)vkCmdSetTessellationDomainOriginEXT },
    { "vkCmdSetDepthClampEnableEXT", (PFN_vkVoidFunction)vkCmdSetDepthClampEnableEXT },
    { "vkCmdSetPolygonModeEXT", (PFN_vkVoidFunction)vkCmdSetPolygonModeEXT },
    { "vkCmdSetRasterizationSamplesEXT", (PFN_vkVoidFunction)vkCmdSetRasterizationSamplesEXT },
    { "vkCmdSetSampleMaskEXT", (PFN_vkVoidFunction)vkCmdSetSampleMaskEXT },
    { "vkCmdSetAlphaToCoverageEnableEXT", (PFN_vkVoidFunction)vkCmdSetAlphaToCoverageEnableEXT },
    { "vkCmdSetAlphaToOneEnableEXT", (PFN_vkVoidFunction)vkCmdSetAlphaToOneEnableEXT },
    { "vkCmdSetLogicOpEnableEXT", (PFN_vkVoidFunction)vkCmdSetLogicOpEnableEXT },
    { "vkCmdSetColorBlendEnableEXT", (PFN_vkVoidFunction)vkCmdSetColorBlendEnableEXT },
    { "vkCmdSetColorBlendEquationEXT", (PFN_vkVoidFunction)vkCmdSetColorBlendEquationEXT },
    { "vkCmdSetColorWriteMaskEXT", (PFN_vkVoidFunction)vkCmdSetColorWriteMaskEXT },
    { "vkCmdSetRasterizationStreamEXT", (PFN_vkVoidFunction)vkCmdSetRasterizationStreamEXT },
    { "vkCmdSetConservativeRasterizationModeEXT", (PFN_vkVoidFunction)vkCmdSetConservativeRasterizationModeEXT },
    { "vkCmdSetExtraPrimitiveOverestimationSizeEXT", (PFN_vkVoidFunction)vkCmdSetExtraPrimitiveOverestimationSizeEXT },
    { "vkCmdSetDepthClipEnableEXT", (PFN_vkVoidFunction)vkCmdSetDepthClipEnableEXT },
    { "vkCmdSetSampleLocationsEnableEXT", (PFN_vkVoidFunction)vkCmdSetSampleLocationsEnableEXT },
    { "vkCmdSetColorBlendAdvancedEXT", (PFN_vkVoidFunction)vkCmdSetColorBlendAdvancedEXT },
    { "vkCmdSetProvokingVertexModeEXT", (PFN_vkVoidFunction)vkCmdSetProvokingVertexModeEXT },
    { "vkCmdSetLineRasterizationModeEXT", (PFN_vkVoidFunction)vkCmdSetLineRasterizationModeEXT },
    { "vkCmdSetLineStippleEnableEXT", (PFN_vkVoidFunction)vkCmdSetLineStippleEnableEXT },
    { "vkCmdSetDepthClipNegativeOneToOneEXT", (PFN_vkVoidFunction)vkCmdSetDepthClipNegativeOneToOneEXT },
    { "vkCmdSetViewportWScalingEnableNV", (PFN_vkVoidFunction)vkCmdSetViewportWScalingEnableNV },
    { "vkCmdSetViewportSwizzleNV", (PFN_vkVoidFunction)vkCmdSetViewportSwizzleNV },
    { "vkCmdSetCoverageToColorEnableNV", (PFN_vkVoidFunction)vkCmdSetCoverageToColorEnableNV },
    { "vkCmdSetCoverageToColorLocationNV", (PFN_vkVoidFunction)vkCmdSetCoverageToColorLocationNV },
    { "vkCmdSetCoverageModulationModeNV", (PFN_vkVoidFunction)vkCmdSetCoverageModulationModeNV },
    { "vkCmdSetCoverageModulationTableEnableNV", (PFN_vkVoidFunction)vkCmdSetCoverageModulationTableEnableNV },
    { "vkCmdSetCoverageModulationTableNV", (PFN_vkVoidFunction)vkCmdSetCoverageModulationTableNV },
    { "vkCmdSetShadingRateImageEnableNV", (PFN_vkVoidFunction)vkCmdSetShadingRateImageEnableNV },
    { "vkCmdSetRepresentativeFragmentTestEnableNV", (PFN_vkVoidFunction)vkCmdSetRepresentativeFragmentTestEnableNV },
    { "vkCmdSetCoverageReductionModeNV", (PFN_vkVoidFunction)vkCmdSetCoverageReductionModeNV },

    // VK_EXT_vertex_input_dynamic_state
    { "vkCmdSetVertexInputEXT", (PFN_vkVoidFunction)vkCmdSetVertexInputEXT },

    // VK_EXT_mesh_shader (pure passthrough, see definitions above)
    { "vkCmdDrawMeshTasksEXT",              (PFN_vkVoidFunction)vkCmdDrawMeshTasksEXT },
    { "vkCmdDrawMeshTasksIndirectEXT",      (PFN_vkVoidFunction)vkCmdDrawMeshTasksIndirectEXT },
    { "vkCmdDrawMeshTasksIndirectCountEXT", (PFN_vkVoidFunction)vkCmdDrawMeshTasksIndirectCountEXT },

    // core 1.3
    { "vkCreatePrivateDataSlot", (PFN_vkVoidFunction)vkCreatePrivateDataSlot },
    { "vkDestroyPrivateDataSlot", (PFN_vkVoidFunction)vkDestroyPrivateDataSlot },
    { "vkSetPrivateData", (PFN_vkVoidFunction)vkSetPrivateData },
    { "vkGetPrivateData", (PFN_vkVoidFunction)vkGetPrivateData },

    { "vkCreateShaderModule",        (PFN_vkVoidFunction)vkCreateShaderModule },
    { "vkCreateComputePipelines",    (PFN_vkVoidFunction)vkCreateComputePipelines },
    { "vkCreateDescriptorSetLayout", (PFN_vkVoidFunction)vkCreateDescriptorSetLayout },
    { "vkCreatePipelineLayout",      (PFN_vkVoidFunction)vkCreatePipelineLayout },
    { "vkBindBufferMemory",          (PFN_vkVoidFunction)vkBindBufferMemory },
    { "vkBindBufferMemory2",         (PFN_vkVoidFunction)vkBindBufferMemory2 },
    { "vkMapMemory",                 (PFN_vkVoidFunction)vkMapMemory },
    { "vkUnmapMemory",               (PFN_vkVoidFunction)vkUnmapMemory },

    { "vkGetBufferDeviceAddress",    (PFN_vkVoidFunction)vkGetBufferDeviceAddress },
    { "vkGetBufferDeviceAddressKHR", (PFN_vkVoidFunction)vkGetBufferDeviceAddressKHR },

    // AMD test support
    { "vkCreateDevice",                       (PFN_vkVoidFunction)vkCreateDevice },
    { "vkGetDeviceQueue",                     (PFN_vkVoidFunction)vkGetDeviceQueue }, // ADDED
    { "vkGetDeviceQueue2",                    (PFN_vkVoidFunction)vkGetDeviceQueue2 }, // ADDED
    { "vkCmdWriteBufferMarker2AMD", (PFN_vkVoidFunction)vkCmdWriteBufferMarker2AMD },
};

extern "C" __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    EnsureRealLoaderLoaded();
    if (!pName) return nullptr;

    if (auto it = g_ourExports.find(pName); it != g_ourExports.end())
        return it->second;

    PFN_vkVoidFunction fn = nullptr;
    if (g_real.GetInstanceProcAddr)
        fn = g_real.GetInstanceProcAddr(instance, pName);
    
    if (fn) return fn;

    // Fallback for global functions if the loader does not find them via the instance
    if (g_real.module)
        fn = (PFN_vkVoidFunction)GetProcAddress(g_real.module, pName);

    if (fn) return fn;

    // --- NULL TRAP ---
    std::string msg = "[vk-emu] MISSING FUNCTION (Instance): " + std::string(pName) + "\n";
    VkEmuLog("%s", msg.c_str());
    /* log flushed in VkEmuLog */
    return (PFN_vkVoidFunction)vkEmuNullStubRet0;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdWriteBufferMarker2AMD(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 pipelineStage, VkBuffer dstBuffer, VkDeviceSize dstOffset, uint32_t marker) {
    LogStubCall("vkCmdWriteBufferMarker2AMD");
}

extern "C" __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!pName) return nullptr;

    if (auto it = g_ourExports.find(pName); it != g_ourExports.end())
        return it->second;

    EnsureRealLoaderLoaded();

    PFN_vkGetDeviceProcAddr realGetter = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_deviceMutex);
        auto it = g_realDeviceProcAddr.find(device);
        if (it != g_realDeviceProcAddr.end()) {
            realGetter = it->second;
        } else {
            if (g_lastInstance != VK_NULL_HANDLE) {
                realGetter = (PFN_vkGetDeviceProcAddr)g_real.GetInstanceProcAddr(g_lastInstance, "vkGetDeviceProcAddr");
            }
            g_realDeviceProcAddr[device] = realGetter;
        }
    }

    if (realGetter) {
        PFN_vkVoidFunction fn = realGetter(device, pName);
        if (fn) return fn;
    } else {
        PFN_vkVoidFunction fn = g_real.GetInstanceProcAddr(g_lastInstance, pName);
        if (fn) return fn;
    }

    std::string msg = "[vk-emu] MISSING FUNCTION: " + std::string(pName) + "\n";
    VkEmuLog("%s", msg.c_str());
    /* log flushed in VkEmuLog */
    return (PFN_vkVoidFunction)vkEmuNullStubRet0;
}


#if defined(__x86_64__) || defined(_M_X64)
// ---------------------------------------------------------------------------
// Crash logger: writes the fault (module+offset) and the call stack of the faulting
// thread to vk_emu.log. Essential when the crash is INSIDE the driver (nvoglv64): the stack shows
// which Vulkan call (wrapper / loader / d3d12core) led to it.
// ---------------------------------------------------------------------------
static void VkEmuDescribeAddr(DWORD64 addr, char* out, size_t n) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)addr, &mod) && mod) {
        char path[MAX_PATH] = {0};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        snprintf(out, n, "%s+0x%llx", base, (unsigned long long)(addr - (DWORD64)(uintptr_t)mod));
    } else {
        snprintf(out, n, "0x%llx", (unsigned long long)addr);
    }
}

static LONG CALLBACK VkEmuVectoredHandler(PEXCEPTION_POINTERS ep) {
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_IN_PAGE_ERROR && code != EXCEPTION_PRIV_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;

    static volatile LONG count = 0;
    if (InterlockedIncrement(&count) > 8) return EXCEPTION_CONTINUE_SEARCH;

    char where[360];
    VkEmuDescribeAddr((DWORD64)(uintptr_t)ep->ExceptionRecord->ExceptionAddress, where, sizeof(where));
    const ULONG_PTR kind = ep->ExceptionRecord->NumberParameters > 0 ? ep->ExceptionRecord->ExceptionInformation[0] : 0;
    const ULONG_PTR addr = ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
    VkEmuLog("[vk-emu] *** EXCEPTION 0x%08lX at %s (%s address 0x%llx) thread=%lu ***\n",
             (unsigned long)code, where, kind == 0 ? "read" : (kind == 1 ? "write" : "execute"),
             (unsigned long long)addr, (unsigned long)GetCurrentThreadId());

    CONTEXT ctx = *ep->ContextRecord;
    for (int i = 0; i < 24 && ctx.Rip; i++) {
        char fr[360];
        VkEmuDescribeAddr(ctx.Rip, fr, sizeof(fr));
        VkEmuLog("[vk-emu]   #%02d %s\n", i, fr);

        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (fe) {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fe, &ctx, &handlerData, &establisher, nullptr);
        } else {
            if (IsBadReadPtr((const void*)ctx.Rsp, 8)) break;   // leaf function without unwind info
            ctx.Rip = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += 8;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;   // we mask nothing: we only log
}
#endif

// Entry point: proves that the DLL is loaded, and by which executable.
static PVOID g_vehHandle = nullptr;   // PATCH: remembered so it can be removed at unload
extern "C" BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        // PATCH: without this, after FreeLibrary the VEH handler points into freed memory -> "DEP violation"
        // on the process's next exception, which masks the original error.
#if defined(__x86_64__) || defined(_M_X64)
        if (g_vehHandle) { RemoveVectoredExceptionHandler(g_vehHandle); g_vehHandle = nullptr; }
#endif
        return TRUE;
    }
    if (reason == DLL_PROCESS_ATTACH) {
        g_selfModule = (HMODULE)hinst;
        DisableThreadLibraryCalls(hinst);
#if defined(__x86_64__) || defined(_M_X64)
        g_vehHandle = AddVectoredExceptionHandler(1, VkEmuVectoredHandler);
#endif
        char exe[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        VkEmuLog("[vk-emu] DLL loaded (pid=%lu) by: %s\n", (unsigned long)GetCurrentProcessId(), exe);
    }
    return TRUE;
}
