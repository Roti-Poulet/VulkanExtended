// Vulkan wrapper, to emulate new vulkan versions with older drivers and unsupported GPUs
// Used with vulkan-1.def

#include <vulkan/vulkan.h>
#include <windows.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <mutex>
#include <cstdio>
#include <cstring>

// device state

// We dont always overwrite the driver extensions
static const char* const g_fakedDeviceExtensions[] = {
    VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
    VK_EXT_NON_SEAMLESS_CUBE_MAP_EXTENSION_NAME,
    VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
    VK_EXT_LINE_RASTERIZATION_EXTENSION_NAME,
    VK_EXT_PIPELINE_CREATION_CACHE_CONTROL_EXTENSION_NAME,
    VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME,
    VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
    VK_EXT_MESH_SHADER_EXTENSION_NAME,
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
    VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME
};
static constexpr uint32_t g_fakedDeviceExtensionCount =
    (uint32_t)(sizeof(g_fakedDeviceExtensions) / sizeof(g_fakedDeviceExtensions[0]));

// Device state to know what we emulate
struct DeviceState {
    bool emu_sync2 = false;
    bool emu_push_desc = false;
};
static std::mutex g_deviceStateMutex;
static std::unordered_map<VkDevice, DeviceState> g_deviceState;

// Cache of the supported extensions
static std::mutex g_physDeviceMutex;
static std::unordered_map<VkPhysicalDevice, std::unordered_set<std::string>> g_realDeviceExtensions;

// 1. Logging (once for each function)
static std::mutex g_logMutex;
static std::unordered_set<std::string> g_alreadyLoggedStub;

static void LogStubCall(const char* name) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_alreadyLoggedStub.insert(name).second) {
        fprintf(stderr, "[vk-emu] STUB APPELE: %s -- verifier si le jeu en a reellement besoin\n", name);
        fflush(stderr);
    }
}

static void LogFatal(const char* name) {
    fprintf(stderr, "[vk-emu] ERREUR FATALE: le pilote reel n'expose pas %s (attendu en core 1.2)\n", name);
    fflush(stderr);
}

// load of the real system loader (vulkan-1.dll)

struct RealLoader {
    HMODULE module = nullptr;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
};
static RealLoader g_real;
static VkInstance g_lastInstance = VK_NULL_HANDLE; // pour les lookups paresseux ci-dessous

static void EnsureRealLoaderLoaded() {
    if (g_real.module) return;
    
    char sysDir[MAX_PATH];
    GetSystemDirectoryA(sysDir, MAX_PATH);
    std::string path = std::string(sysDir) + "\\vulkan-1.dll";
    
    g_real.module = LoadLibraryA(path.c_str());
    if (!g_real.module) {
        fprintf(stderr, "[vk-emu] ERREUR FATALE: impossible de charger le vrai driver %s\n", path.c_str());
        return;
    }
    g_real.GetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)GetProcAddress(g_real.module, "vkGetInstanceProcAddr");
}

static std::mutex g_resolveMutex;
static std::unordered_map<std::string, PFN_vkVoidFunction> g_resolvedCache;

template <typename PFN>
static PFN LazyResolve(const char* name) {
    std::lock_guard<std::mutex> lock(g_resolveMutex);
    auto it = g_resolvedCache.find(name);
    if (it != g_resolvedCache.end())
        return (PFN)it->second;

    EnsureRealLoaderLoaded();
    PFN_vkVoidFunction fn = nullptr;
    if (g_real.GetInstanceProcAddr && g_lastInstance != VK_NULL_HANDLE)
        fn = g_real.GetInstanceProcAddr(g_lastInstance, name);
    g_resolvedCache[name] = fn; // on cache aussi les echecs (nullptr) : un seul essai
    return (PFN)fn;
}

static bool RealDeviceHasExtension(VkPhysicalDevice physDevice, const char* extName) {
    std::lock_guard<std::mutex> lock(g_physDeviceMutex);
    auto it = g_realDeviceExtensions.find(physDevice);
    if (it == g_realDeviceExtensions.end()) {
        // Interroge le vrai driver
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
    LogStubCall("GetDeviceForCommandBuffer:cmd_inconnu");
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

    bool operator==(const RenderPassKey& o) const {
        return viewMask == o.viewMask &&
               hasDepth == o.hasDepth && hasStencil == o.hasStencil &&
               colorAttachments.size() == o.colorAttachments.size() &&
               std::equal(colorAttachments.begin(), colorAttachments.end(), o.colorAttachments.begin()) &&
               (!hasDepth   || depth   == o.depth) &&
               (!hasStencil || stencil == o.stencil);
    }
};

struct RenderPassKeyHash {
    size_t operator()(const RenderPassKey& k) const {
        size_t h = std::hash<uint32_t>()(k.viewMask);
        for (auto& a : k.colorAttachments)
            h ^= std::hash<uint64_t>()((uint64_t)a.format << 32 | a.loadOp) + 0x9e3779b9 + (h << 6) + (h >> 2);
        if (k.hasDepth)
            h ^= std::hash<uint32_t>()(k.depth.format) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct FramebufferKey {
    VkRenderPass renderPass;
    uint32_t width, height, layers;
    // test with imageless framebuffers
    bool operator==(const FramebufferKey& o) const {
        return renderPass == o.renderPass && width == o.width &&
               height == o.height && layers == o.layers;
    }
};
struct FramebufferKeyHash {
    size_t operator()(const FramebufferKey& k) const {
        return std::hash<void*>()(k.renderPass) ^ (k.width << 1) ^ (k.height << 2) ^ (k.layers << 3);
    }
};

static std::mutex g_cacheMutex;
static std::unordered_map<RenderPassKey, VkRenderPass, RenderPassKeyHash> g_renderPassCache;
static std::unordered_map<FramebufferKey, VkFramebuffer, FramebufferKeyHash> g_framebufferCache;

static VkFramebuffer GetOrCreateFramebuffer(VkDevice device, VkRenderPass rp,
                                             const std::vector<VkImageView>& views) {
    uint32_t w = 1, h = 1, layers = 1;
    if (!views.empty()) {
        auto info = LookupImageViewInfo(views[0]);
        w = info.width;
        h = info.height;
        layers = info.layerCount;
    }

    FramebufferKey key{rp, w, h, layers};

    std::lock_guard<std::mutex> lock(g_cacheMutex);
    auto it = g_framebufferCache.find(key);
    if (it != g_framebufferCache.end())
        return it->second;

    std::vector<VkFormat> formatsStorage(views.size());
    std::vector<VkFramebufferAttachmentImageInfo> imgInfos;
    imgInfos.reserve(views.size());
    
    for (size_t i = 0; i < views.size(); i++) {
        auto info = LookupImageViewInfo(views[i]);
        formatsStorage[i] = info.format;
        
        VkFramebufferAttachmentImageInfo imgInfo{VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO};
        imgInfo.usage = info.usage;
        if (imgInfo.usage == 0) imgInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; // repli sûr
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

    auto real_vkCreateFramebuffer = LazyResolve<PFN_vkCreateFramebuffer>("vkCreateFramebuffer");
    if (!real_vkCreateFramebuffer) {
        LogFatal("vkCreateFramebuffer");
        return VK_NULL_HANDLE;
    }
    VkFramebuffer fb = VK_NULL_HANDLE;
    real_vkCreateFramebuffer(device, &fbci, nullptr, &fb);
    g_framebufferCache[key] = fb;
    return fb;
}

static VkRenderPass CreateRenderPassFromKey(VkDevice device, const RenderPassKey& key) {
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

    VkRenderPassCreateInfo2 rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    rpci.attachmentCount = (uint32_t)attachments.size();
    rpci.pAttachments = attachments.data();
    rpci.subpassCount = 1;
    rpci.pSubpasses = &subpass;

    auto real_vkCreateRenderPass2 = LazyResolve<PFN_vkCreateRenderPass2>("vkCreateRenderPass2");
    if (!real_vkCreateRenderPass2) {
        LogFatal("vkCreateRenderPass2");
        return VK_NULL_HANDLE;
    }
    VkRenderPass rp = VK_NULL_HANDLE;
    real_vkCreateRenderPass2(device, &rpci, nullptr, &rp);
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

    VkFramebuffer fb = GetOrCreateFramebuffer(device, rp, views);

    std::vector<VkClearValue> clears;
    for (uint32_t i = 0; i < info->colorAttachmentCount; i++)
        clears.push_back(info->pColorAttachments[i].clearValue);
    if (info->pDepthAttachment) clears.push_back(info->pDepthAttachment->clearValue);

    VkRenderPassAttachmentBeginInfo rpabi{VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO};
    rpabi.attachmentCount = (uint32_t)views.size();
    rpabi.pAttachments = views.data();

    VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rpbi.pNext = &rpabi;
    rpbi.renderPass = rp;
    rpbi.framebuffer = fb;
    rpbi.renderArea = info->renderArea;
    rpbi.clearValueCount = (uint32_t)clears.size();
    rpbi.pClearValues = clears.data();

    VkSubpassBeginInfo sbi{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO};
    sbi.contents = (info->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT)
                       ? VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS
                       : VK_SUBPASS_CONTENTS_INLINE;

    auto real_vkCmdBeginRenderPass2 = LazyResolve<PFN_vkCmdBeginRenderPass2>("vkCmdBeginRenderPass2");
    if (!real_vkCmdBeginRenderPass2) { LogFatal("vkCmdBeginRenderPass2"); return; }
    real_vkCmdBeginRenderPass2(cmd, &rpbi, &sbi);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdEndRendering(VkCommandBuffer cmd) {
    VkSubpassEndInfo sei{VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
    auto real_vkCmdEndRenderPass2 = LazyResolve<PFN_vkCmdEndRenderPass2>("vkCmdEndRenderPass2");
    if (!real_vkCmdEndRenderPass2) { LogFatal("vkCmdEndRenderPass2"); return; }
    real_vkCmdEndRenderPass2(cmd, &sei);
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

    if (modified) {
        return real(device, pipelineCache, createInfoCount, modifiedInfos.data(), pAllocator, pPipelines);
    } else {
        return real(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
    }
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

    VkInstanceCreateInfo ci = *pCreateInfo;
    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    if (ci.pApplicationInfo) appInfo = *ci.pApplicationInfo;

    uint32_t major = VK_API_VERSION_MAJOR(appInfo.apiVersion);
    uint32_t minor = VK_API_VERSION_MINOR(appInfo.apiVersion);
    if (major > 1 || (major == 1 && minor > 2))
        appInfo.apiVersion = VK_API_VERSION_1_2;
    ci.pApplicationInfo = &appInfo;

    // Forcer VK_KHR_get_physical_device_properties2 pour résoudre les fonctions KHR manquantes
    std::vector<const char*> enabledExts;
    if (ci.enabledExtensionCount > 0) {
        enabledExts.assign(ci.ppEnabledExtensionNames, ci.ppEnabledExtensionNames + ci.enabledExtensionCount);
    }
    bool has_gpdp2 = false;
    for (auto& ext : enabledExts) {
        if (strcmp(ext, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0) { has_gpdp2 = true; break; }
    }
    if (!has_gpdp2) {
        enabledExts.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    }
    ci.enabledExtensionCount = (uint32_t)enabledExts.size();
    ci.ppEnabledExtensionNames = enabledExts.data();

    VkResult result = real_vkCreateInstance(&ci, pAllocator, pInstance);
    if (result == VK_SUCCESS)
        g_lastInstance = *pInstance; 
    return result;
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkEnumerateInstanceVersion(uint32_t* pApiVersion) {
    *pApiVersion = VK_API_VERSION_1_3;
    return VK_SUCCESS;
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
    if (!real) { LogFatal("vkGetPhysicalDeviceFeatures2"); return; }

    real(physicalDevice, pFeatures);

    VkBaseOutStructure* cur = (VkBaseOutStructure*)pFeatures->pNext;
    while (cur) {
        if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* f = (VkPhysicalDeviceVulkan12Features*)cur;
            f->imagelessFramebuffer = VK_TRUE;
            f->timelineSemaphore = VK_TRUE;
            f->descriptorBindingPartiallyBound = VK_TRUE;
            f->descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
            f->descriptorBindingVariableDescriptorCount = VK_TRUE;
            f->runtimeDescriptorArray = VK_TRUE;
            f->scalarBlockLayout = VK_TRUE;
            f->bufferDeviceAddress = VK_TRUE;
        } 
        // --- AUTOMATISATION : ON MET TOUT A TRUE ---
        else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            auto* f = (VkPhysicalDeviceVulkan13Features*)cur;
            size_t offset = offsetof(VkPhysicalDeviceVulkan13Features, robustImageAccess);
            memset((char*)f + offset, 0xFF, sizeof(VkPhysicalDeviceVulkan13Features) - offset);
        } else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES) {
            auto* f = (VkPhysicalDeviceVulkan14Features*)cur;
            size_t offset = offsetof(VkPhysicalDeviceVulkan14Features, globalPriorityQuery);
            memset((char*)f + offset, 0xFF, sizeof(VkPhysicalDeviceVulkan14Features) - offset);
        } else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT) {
            auto* f = (VkPhysicalDeviceExtendedDynamicState3FeaturesEXT*)cur;
            size_t offset = offsetof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT, extendedDynamicState3TessellationDomainOrigin);
            memset((char*)f + offset, 0xFF, sizeof(VkPhysicalDeviceExtendedDynamicState3FeaturesEXT) - offset);
        }
        cur = cur->pNext;
    }
    
    // Features de base 1.0 souvent exigées
    pFeatures->features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    pFeatures->features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
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
    bool real_has_eds3 = RealDeviceHasExtension(physicalDevice, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);

    std::vector<const char*> filteredExt;
    for (uint32_t i = 0; i < ci.enabledExtensionCount; i++) {
        const char* ext = ci.ppEnabledExtensionNames[i];
        
        // Si c'est une extensionqu'on émule (dynamic rendering, sync2, etc.), on la retire
        bool is_faked = false;
        for (uint32_t j = 0; j < g_fakedDeviceExtensionCount; j++) {
            if (strcmp(ext, g_fakedDeviceExtensions[j]) == 0) {
                is_faked = true;
                break;
            }
        }
        if (is_faked) {
            if (strcmp(ext, VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0) state.emu_sync2 = true;
            if (strcmp(ext, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0) state.emu_push_desc = true;
            continue;
        }

        // Si le vrai driver ne supporte pas l'extension, on la retire pour éviter le crash
        if (!RealDeviceHasExtension(physicalDevice, ext)) {
            fprintf(stderr, "[vk-emu] Extension non supportée par le driver, filtrage: %s\n", ext);
            continue;
        }

        filteredExt.push_back(ext);
    }
    ci.enabledExtensionCount = (uint32_t)filteredExt.size();
    ci.ppEnabledExtensionNames = filteredExt.empty() ? nullptr : filteredExt.data();

    // Nettoyer la pNext chain des features inconnues du driver réel
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

    // --- RECONSTRUCTEUR DE CHAÎNE PNEXT ---
    // On ne garde QUE les structures que le vieux driver Nvidia 1.2.175 connaît.
    // Tout le reste (Vulkan 1.3, 1.4, Raytracing, Mesh Shaders) est jeté pour éviter le crash (-8).
    VkBaseOutStructure* newChainHead = nullptr;
    VkBaseOutStructure* newChainTail = nullptr;
    
    VkBaseOutStructure* curr = (VkBaseOutStructure*)ci.pNext;
    while (curr) {
        VkBaseOutStructure* next = curr->pNext;
        bool keep = false;
        
        if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            // On vide la structure pour ne garder que ce qu'on veut
            auto* f = (VkPhysicalDeviceVulkan12Features*)curr;
            size_t offset = offsetof(VkPhysicalDeviceVulkan12Features, pNext) + sizeof(void*);
            memset((char*)f + offset, 0, sizeof(VkPhysicalDeviceVulkan12Features) - offset);
            f->imagelessFramebuffer = VK_TRUE;
            f->timelineSemaphore = VK_TRUE;
            f->descriptorBindingPartiallyBound = VK_TRUE;
            f->descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
            f->descriptorBindingVariableDescriptorCount = VK_TRUE;
            f->runtimeDescriptorArray = VK_TRUE;
            f->scalarBlockLayout = VK_TRUE;
            f->bufferDeviceAddress = VK_TRUE;
            keep = true;
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES) {
            ((VkPhysicalDeviceImagelessFramebufferFeatures*)curr)->imagelessFramebuffer = VK_TRUE;
            keep = true;
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES) {
            ((VkPhysicalDeviceTimelineSemaphoreFeatures*)curr)->timelineSemaphore = VK_TRUE;
            keep = true;
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES) {
            keep = true; // Supporté en 1.2
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES) {
            keep = true; // Supporté en 1.2
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
            keep = true; // Supporté en 1.2
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES) {
            keep = true; // Supporté en 1.2
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES) {
            keep = true; // Supporté en 1.2
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
    ci.pNext = newChainHead;

    // Log des extensions finales envoyées au driver
    fprintf(stderr, "[vk-emu] Extensions envoyées au driver:\n");
    for (uint32_t i = 0; i < ci.enabledExtensionCount; i++) {
        fprintf(stderr, "  -> %s\n", ci.ppEnabledExtensionNames[i]);
    }
    fflush(stderr);

    VkResult res = real(physicalDevice, &ci, pAllocator, pDevice);
    if (res != VK_SUCCESS) {
        fprintf(stderr, "[vk-emu] vkCreateDevice a échoué avec l'erreur : %d\n", res);
        fflush(stderr);
    } else {
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
    LogStubCall("vkCmdSetLineStipple");
    // Repli : pas de pointille, ligne pleine (degradation visuelle mineure,
    // pas de crash -- comportement par defaut si l'etat n'est jamais set).
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdBindIndexBuffer2(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                       VkDeviceSize size, VkIndexType indexType) {
    using PFN = PFN_vkCmdBindIndexBuffer2;
    if (PFN real = LazyResolve<PFN>("vkCmdBindIndexBuffer2")) {
        real(commandBuffer, buffer, offset, size, indexType);
        return;
    }
    LogStubCall("vkCmdBindIndexBuffer2");
    // Repli correct: retombe sur vkCmdBindIndexBuffer (core 1.0), qui n'a
    // pas de parametre "size" -- a implementer proprement plus tard, car
    // ignorer l'appel casserait le rendu (pas juste degrade). Pour l'instant
    // on force un binding via l'API classique en ignorant "size":
    using PFN_Legacy = PFN_vkCmdBindIndexBuffer;
    if (PFN_Legacy legacy = LazyResolve<PFN_Legacy>("vkCmdBindIndexBuffer"))
        legacy(commandBuffer, buffer, offset, indexType);
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkMapMemory2(VkDevice device, const VkMemoryMapInfo* pMemoryMapInfo, void** ppData) {
    using PFN = PFN_vkMapMemory2;
    if (PFN real = LazyResolve<PFN>("vkMapMemory2"))
        return real(device, pMemoryMapInfo, ppData);

    LogStubCall("vkMapMemory2");
    using PFN_Legacy = PFN_vkMapMemory;
    if (PFN_Legacy legacy = LazyResolve<PFN_Legacy>("vkMapMemory"))
        return legacy(device, pMemoryMapInfo->memory, pMemoryMapInfo->offset,
                       pMemoryMapInfo->size, pMemoryMapInfo->flags, ppData);
    return VK_ERROR_MEMORY_MAP_FAILED;
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkUnmapMemory2(VkDevice device, const VkMemoryUnmapInfo* pMemoryUnmapInfo) {
    using PFN = PFN_vkUnmapMemory2;
    if (PFN real = LazyResolve<PFN>("vkUnmapMemory2"))
        return real(device, pMemoryUnmapInfo);

    LogStubCall("vkUnmapMemory2");
    using PFN_Legacy = PFN_vkUnmapMemory;
    if (PFN_Legacy legacy = LazyResolve<PFN_Legacy>("vkUnmapMemory")) {
        legacy(device, pMemoryUnmapInfo->memory);
        return VK_SUCCESS;
    }
    return VK_ERROR_UNKNOWN;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdSetRenderingAttachmentLocations(VkCommandBuffer commandBuffer, const VkRenderingAttachmentLocationInfo* pInfo) {
    using PFN = PFN_vkCmdSetRenderingAttachmentLocations;
    if (PFN real = LazyResolve<PFN>("vkCmdSetRenderingAttachmentLocations")) {
        real(commandBuffer, pInfo);
        return;
    }
    LogStubCall("vkCmdSetRenderingAttachmentLocations");
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdSetRenderingInputAttachmentIndices(VkCommandBuffer commandBuffer, const VkRenderingInputAttachmentIndexInfo* pInfo) {
    using PFN = PFN_vkCmdSetRenderingInputAttachmentIndices;
    if (PFN real = LazyResolve<PFN>("vkCmdSetRenderingInputAttachmentIndices")) {
        real(commandBuffer, pInfo);
        return;
    }
    LogStubCall("vkCmdSetRenderingInputAttachmentIndices");
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
    if (real_create && real_get && real_destroy && pInfo && pMemoryRequirements) {
        VkImage img;
        if (real_create(device, pInfo->pCreateInfo, nullptr, &img) == VK_SUCCESS) {
            VkImageMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
            info.image = img;
            real_get(device, &info, pMemoryRequirements);
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
    if (real_create && real_get && real_destroy && pInfo && pMemoryRequirements) {
        VkBuffer buf;
        if (real_create(device, pInfo->pCreateInfo, nullptr, &buf) == VK_SUCCESS) {
            VkBufferMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
            info.buffer = buf;
            real_get(device, &info, pMemoryRequirements);
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
    LogStubCall("vkGetDeviceImageSparseMemoryRequirements");
    *pSparseMemoryRequirementCount = 0;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetRenderingAreaGranularity(VkDevice device, const VkRenderingAreaInfo* pRenderingAreaInfo, VkExtent2D* pGranularity) {
    using PFN = PFN_vkGetRenderingAreaGranularity;
    if (PFN real = LazyResolve<PFN>("vkGetRenderingAreaGranularity")) {
        real(device, pRenderingAreaInfo, pGranularity);
        return;
    }
    LogStubCall("vkGetRenderingAreaGranularity");
    pGranularity->width = 1; pGranularity->height = 1; // always ok
}

// --- host_image_copy (1.4) : VkResult, harder to stub...

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

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdPipelineBarrier2(VkCommandBuffer commandBuffer, const VkDependencyInfo* pDependencyInfo) {
    auto real = LazyResolve<PFN_vkCmdPipelineBarrier2>("vkCmdPipelineBarrier2");
    if (real) { real(commandBuffer, pDependencyInfo); return; }

    auto legacy = LazyResolve<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier");
    if (!legacy || !pDependencyInfo) return;

    VkPipelineStageFlags srcStageMask = 0, dstStageMask = 0;
    std::vector<VkMemoryBarrier> memBarriers;
    std::vector<VkBufferMemoryBarrier> bufBarriers;
    std::vector<VkImageMemoryBarrier> imgBarriers;

    for (uint32_t i = 0; i < pDependencyInfo->memoryBarrierCount; i++) {
        srcStageMask |= (VkPipelineStageFlags)pDependencyInfo->pMemoryBarriers[i].srcStageMask;
        dstStageMask |= (VkPipelineStageFlags)pDependencyInfo->pMemoryBarriers[i].dstStageMask;
        VkMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = (VkAccessFlags)pDependencyInfo->pMemoryBarriers[i].srcAccessMask;
        b.dstAccessMask = (VkAccessFlags)pDependencyInfo->pMemoryBarriers[i].dstAccessMask;
        memBarriers.push_back(b);
    }
    for (uint32_t i = 0; i < pDependencyInfo->bufferMemoryBarrierCount; i++) {
        const auto& src = pDependencyInfo->pBufferMemoryBarriers[i];
        srcStageMask |= (VkPipelineStageFlags)src.srcStageMask;
        dstStageMask |= (VkPipelineStageFlags)src.dstStageMask;
        VkBufferMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.srcAccessMask = (VkAccessFlags)src.srcAccessMask; b.dstAccessMask = (VkAccessFlags)src.dstAccessMask;
        b.srcQueueFamilyIndex = src.srcQueueFamilyIndex; b.dstQueueFamilyIndex = src.dstQueueFamilyIndex;
        b.buffer = src.buffer; b.offset = src.offset; b.size = src.size;
        bufBarriers.push_back(b);
    }
    for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount; i++) {
        const auto& src = pDependencyInfo->pImageMemoryBarriers[i];
        srcStageMask |= (VkPipelineStageFlags)src.srcStageMask;
        dstStageMask |= (VkPipelineStageFlags)src.dstStageMask;
        VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = (VkAccessFlags)src.srcAccessMask; b.dstAccessMask = (VkAccessFlags)src.dstAccessMask;
        b.oldLayout = src.oldLayout; b.newLayout = src.newLayout;
        b.srcQueueFamilyIndex = src.srcQueueFamilyIndex; b.dstQueueFamilyIndex = src.dstQueueFamilyIndex;
        b.image = src.image; b.subresourceRange = src.subresourceRange;
        imgBarriers.push_back(b);
    }

    legacy(commandBuffer, srcStageMask, dstStageMask, pDependencyInfo->dependencyFlags,
           (uint32_t)memBarriers.size(), memBarriers.data(),
           (uint32_t)bufBarriers.size(), bufBarriers.data(),
           (uint32_t)imgBarriers.size(), imgBarriers.data());
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkQueueSubmit2(VkQueue queue, uint32_t submitCount, const VkSubmitInfo2* pSubmits, VkFence fence) {
    auto real = LazyResolve<PFN_vkQueueSubmit2>("vkQueueSubmit2");
    if (real) return real(queue, submitCount, pSubmits, fence);

    auto legacy = LazyResolve<PFN_vkQueueSubmit>("vkQueueSubmit");
    if (!legacy) return VK_ERROR_INITIALIZATION_FAILED;

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
        legacySubmits[i].pNext = s2.pNext;

        waitSemaphores[i].resize(s2.waitSemaphoreInfoCount);
        waitDstStageMasks[i].resize(s2.waitSemaphoreInfoCount);
        waitValues[i].resize(s2.waitSemaphoreInfoCount);
        for (uint32_t j = 0; j < s2.waitSemaphoreInfoCount; j++) {
            waitSemaphores[i][j] = s2.pWaitSemaphoreInfos[j].semaphore;
            waitDstStageMasks[i][j] = (VkPipelineStageFlags)s2.pWaitSemaphoreInfos[j].stageMask;
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
    return legacy(queue, submitCount, legacySubmits.data(), fence);
}

// Stubs for the other commands Sync2
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdWriteTimestamp2(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage, VkQueryPool queryPool, uint32_t query) {
    auto real = LazyResolve<PFN_vkCmdWriteTimestamp2>("vkCmdWriteTimestamp2");
    if (real) { real(commandBuffer, stage, queryPool, query); return; }
    auto legacy = LazyResolve<PFN_vkCmdWriteTimestamp>("vkCmdWriteTimestamp");
    if (legacy) legacy(commandBuffer, (VkPipelineStageFlagBits)stage, queryPool, query);
}

extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetEvent2(VkCommandBuffer commandBuffer, VkEvent event, const VkDependencyInfo* pDependencyInfo) {
    auto real = LazyResolve<PFN_vkCmdSetEvent2>("vkCmdSetEvent2");
    if (real) { real(commandBuffer, event, pDependencyInfo); return; }
    auto legacy = LazyResolve<PFN_vkCmdSetEvent>("vkCmdSetEvent");
    if (legacy && pDependencyInfo) {
        VkPipelineStageFlags mask = 0;
        for (uint32_t i = 0; i < pDependencyInfo->memoryBarrierCount; i++) mask |= (VkPipelineStageFlags)pDependencyInfo->pMemoryBarriers[i].srcStageMask;
        for (uint32_t i = 0; i < pDependencyInfo->bufferMemoryBarrierCount; i++) mask |= (VkPipelineStageFlags)pDependencyInfo->pBufferMemoryBarriers[i].srcStageMask;
        for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount; i++) mask |= (VkPipelineStageFlags)pDependencyInfo->pImageMemoryBarriers[i].srcStageMask;
        legacy(commandBuffer, event, mask);
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdResetEvent2(VkCommandBuffer commandBuffer, VkEvent event, VkPipelineStageFlags2 stageMask) {
    auto real = LazyResolve<PFN_vkCmdResetEvent2>("vkCmdResetEvent2");
    if (real) { real(commandBuffer, event, stageMask); return; }
    auto legacy = LazyResolve<PFN_vkCmdResetEvent>("vkCmdResetEvent");
    if (legacy) legacy(commandBuffer, event, (VkPipelineStageFlagBits)stageMask);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdWaitEvents2(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent* pEvents, const VkDependencyInfo* pDependencyInfos) {
    auto real = LazyResolve<PFN_vkCmdWaitEvents2>("vkCmdWaitEvents2");
    if (real) { real(commandBuffer, eventCount, pEvents, pDependencyInfos); return; }

    auto legacy = LazyResolve<PFN_vkCmdWaitEvents>("vkCmdWaitEvents");
    if (!legacy || !pDependencyInfos) return;

    VkPipelineStageFlags srcStageMask = 0, dstStageMask = 0;
    std::vector<VkMemoryBarrier> memBarriers;
    std::vector<VkBufferMemoryBarrier> bufBarriers;
    std::vector<VkImageMemoryBarrier> imgBarriers;

    for (uint32_t i = 0; i < eventCount; i++) {
        const auto& dep = pDependencyInfos[i];
        for (uint32_t j = 0; j < dep.memoryBarrierCount; j++) {
            srcStageMask |= (VkPipelineStageFlags)dep.pMemoryBarriers[j].srcStageMask;
            dstStageMask |= (VkPipelineStageFlags)dep.pMemoryBarriers[j].dstStageMask;
            VkMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            b.srcAccessMask = (VkAccessFlags)dep.pMemoryBarriers[j].srcAccessMask;
            b.dstAccessMask = (VkAccessFlags)dep.pMemoryBarriers[j].dstAccessMask;
            memBarriers.push_back(b);
        }
        for (uint32_t j = 0; j < dep.bufferMemoryBarrierCount; j++) {
            const auto& src = dep.pBufferMemoryBarriers[j];
            srcStageMask |= (VkPipelineStageFlags)src.srcStageMask;
            dstStageMask |= (VkPipelineStageFlags)src.dstStageMask;
            VkBufferMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            b.srcAccessMask = (VkAccessFlags)src.srcAccessMask; b.dstAccessMask = (VkAccessFlags)src.dstAccessMask;
            b.srcQueueFamilyIndex = src.srcQueueFamilyIndex; b.dstQueueFamilyIndex = src.dstQueueFamilyIndex;
            b.buffer = src.buffer; b.offset = src.offset; b.size = src.size;
            bufBarriers.push_back(b);
        }
        for (uint32_t j = 0; j < dep.imageMemoryBarrierCount; j++) {
            const auto& src = dep.pImageMemoryBarriers[j];
            srcStageMask |= (VkPipelineStageFlags)src.srcStageMask;
            dstStageMask |= (VkPipelineStageFlags)src.dstStageMask;
            VkImageMemoryBarrier b{}; b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = (VkAccessFlags)src.srcAccessMask; b.dstAccessMask = (VkAccessFlags)src.dstAccessMask;
            b.oldLayout = src.oldLayout; b.newLayout = src.newLayout;
            b.srcQueueFamilyIndex = src.srcQueueFamilyIndex; b.dstQueueFamilyIndex = src.dstQueueFamilyIndex;
            b.image = src.image; b.subresourceRange = src.subresourceRange;
            imgBarriers.push_back(b);
        }
    }

    legacy(commandBuffer, eventCount, pEvents, srcStageMask, dstStageMask, 
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
            // Repli sûr: on assume le point de binding graphique
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

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdPushDescriptorSetWithTemplate2(VkCommandBuffer commandBuffer, const VkPushDescriptorSetWithTemplateInfo* pInfo) {
    auto real = LazyResolve<PFN_vkCmdPushDescriptorSetWithTemplate2>("vkCmdPushDescriptorSetWithTemplate2");
    if (real) { real(commandBuffer, pInfo); return; }

    // Repli vers la version legacy (VK_KHR_push_descriptor + VK_KHR_descriptor_update_template)
    auto legacy = LazyResolve<PFN_vkCmdPushDescriptorSetWithTemplateKHR>("vkCmdPushDescriptorSetWithTemplateKHR");
    if (legacy && pInfo) {
        legacy(commandBuffer, pInfo->descriptorUpdateTemplate, pInfo->layout, pInfo->set, pInfo->pData);
        return;
    }
    LogStubCall("vkCmdPushDescriptorSetWithTemplate2 (no legacy fallback)");
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdPushDescriptorSetWithTemplate2KHR(VkCommandBuffer commandBuffer, const VkPushDescriptorSetWithTemplateInfo* pInfo) {
    auto real = LazyResolve<PFN_vkCmdPushDescriptorSetWithTemplate2KHR>("vkCmdPushDescriptorSetWithTemplate2KHR");
    if (real) { real(commandBuffer, pInfo); return; }

    // Repli vers la version legacy (VK_KHR_push_descriptor + VK_KHR_descriptor_update_template)
    auto legacy = LazyResolve<PFN_vkCmdPushDescriptorSetWithTemplateKHR>("vkCmdPushDescriptorSetWithTemplateKHR");
    if (legacy && pInfo) {
        legacy(commandBuffer, pInfo->descriptorUpdateTemplate, pInfo->layout, pInfo->set, pInfo->pData);
        return;
    }
    LogStubCall("vkCmdPushDescriptorSetWithTemplate2KHR (no legacy fallback)");
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdPushDescriptorSetKHR(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
                          VkPipelineLayout layout, uint32_t set, uint32_t descriptorWriteCount,
                          const VkWriteDescriptorSet* pDescriptorWrites) {
    auto real = LazyResolve<PFN_vkCmdPushDescriptorSetKHR>("vkCmdPushDescriptorSetKHR");
    if (real) { real(commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites); return; }
    LogStubCall("vkCmdPushDescriptorSetKHR (No fallback possible)");
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdPushDescriptorSet2KHR(VkCommandBuffer commandBuffer, const VkPushDescriptorSetInfo* pPushDescriptorSetInfo) {
    auto real = LazyResolve<PFN_vkCmdPushDescriptorSet2KHR>("vkCmdPushDescriptorSet2KHR");
    if (real) { real(commandBuffer, pPushDescriptorSetInfo); return; }
    LogStubCall("vkCmdPushDescriptorSet2KHR (No fallback possible)");
}

// Vulkan 1.3 feature emulation

// --- Copy Commands 2 (VK_KHR_copy_commands2) ---
extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdCopyBuffer2(VkCommandBuffer commandBuffer, const VkCopyBufferInfo2* pCopyBufferInfo) {
    auto real = LazyResolve<PFN_vkCmdCopyBuffer2>("vkCmdCopyBuffer2KHR");
    if (real) { real(commandBuffer, pCopyBufferInfo); return; }
    auto legacy = LazyResolve<PFN_vkCmdCopyBuffer>("vkCmdCopyBuffer");
    if (legacy && pCopyBufferInfo) {
        // VkBufferCopy2 est binairement compatible avec VkBufferCopy
        legacy(commandBuffer, pCopyBufferInfo->srcBuffer, pCopyBufferInfo->dstBuffer, pCopyBufferInfo->regionCount, (const VkBufferCopy*)pCopyBufferInfo->pRegions);
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdCopyImage2(VkCommandBuffer commandBuffer, const VkCopyImageInfo2* pCopyImageInfo) {
    auto real = LazyResolve<PFN_vkCmdCopyImage2>("vkCmdCopyImage2KHR");
    if (real) { real(commandBuffer, pCopyImageInfo); return; }
    auto legacy = LazyResolve<PFN_vkCmdCopyImage>("vkCmdCopyImage");
    if (legacy && pCopyImageInfo) {
        legacy(commandBuffer, pCopyImageInfo->srcImage, pCopyImageInfo->srcImageLayout, pCopyImageInfo->dstImage, pCopyImageInfo->dstImageLayout, pCopyImageInfo->regionCount, (const VkImageCopy*)pCopyImageInfo->pRegions);
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdCopyBufferToImage2(VkCommandBuffer commandBuffer, const VkCopyBufferToImageInfo2* pCopyBufferToImageInfo) {
    auto real = LazyResolve<PFN_vkCmdCopyBufferToImage2>("vkCmdCopyBufferToImage2KHR");
    if (real) { real(commandBuffer, pCopyBufferToImageInfo); return; }
    auto legacy = LazyResolve<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage");
    if (legacy && pCopyBufferToImageInfo) {
        legacy(commandBuffer, pCopyBufferToImageInfo->srcBuffer, pCopyBufferToImageInfo->dstImage, pCopyBufferToImageInfo->dstImageLayout, pCopyBufferToImageInfo->regionCount, (const VkBufferImageCopy*)pCopyBufferToImageInfo->pRegions);
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdCopyImageToBuffer2(VkCommandBuffer commandBuffer, const VkCopyImageToBufferInfo2* pCopyImageToBufferInfo) {
    auto real = LazyResolve<PFN_vkCmdCopyImageToBuffer2>("vkCmdCopyImageToBuffer2KHR");
    if (real) { real(commandBuffer, pCopyImageToBufferInfo); return; }
    auto legacy = LazyResolve<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer");
    if (legacy && pCopyImageToBufferInfo) {
        legacy(commandBuffer, pCopyImageToBufferInfo->srcImage, pCopyImageToBufferInfo->srcImageLayout, pCopyImageToBufferInfo->dstBuffer, pCopyImageToBufferInfo->regionCount, (const VkBufferImageCopy*)pCopyImageToBufferInfo->pRegions);
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdBlitImage2(VkCommandBuffer commandBuffer, const VkBlitImageInfo2* pBlitImageInfo) {
    auto real = LazyResolve<PFN_vkCmdBlitImage2>("vkCmdBlitImage2KHR");
    if (real) { real(commandBuffer, pBlitImageInfo); return; }
    auto legacy = LazyResolve<PFN_vkCmdBlitImage>("vkCmdBlitImage");
    if (legacy && pBlitImageInfo) {
        legacy(commandBuffer, pBlitImageInfo->srcImage, pBlitImageInfo->srcImageLayout, pBlitImageInfo->dstImage, pBlitImageInfo->dstImageLayout, pBlitImageInfo->regionCount, (const VkImageBlit*)pBlitImageInfo->pRegions, pBlitImageInfo->filter);
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdResolveImage2(VkCommandBuffer commandBuffer, const VkResolveImageInfo2* pResolveImageInfo) {
    auto real = LazyResolve<PFN_vkCmdResolveImage2>("vkCmdResolveImage2KHR");
    if (real) { real(commandBuffer, pResolveImageInfo); return; }
    auto legacy = LazyResolve<PFN_vkCmdResolveImage>("vkCmdResolveImage");
    if (legacy && pResolveImageInfo) {
        legacy(commandBuffer, pResolveImageInfo->srcImage, pResolveImageInfo->srcImageLayout, pResolveImageInfo->dstImage, pResolveImageInfo->dstImageLayout, pResolveImageInfo->regionCount, (const VkImageResolve*)pResolveImageInfo->pRegions);
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
    if (real) real(cb, t);
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetViewportWithCount(VkCommandBuffer cb, uint32_t c, const VkViewport* v) {
    auto real = LazyResolve<PFN_vkCmdSetViewportWithCount>("vkCmdSetViewportWithCountEXT");
    if (real) real(cb, c, v);
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdSetScissorWithCount(VkCommandBuffer cb, uint32_t c, const VkRect2D* s) {
    auto real = LazyResolve<PFN_vkCmdSetScissorWithCount>("vkCmdSetScissorWithCountEXT");
    if (real) real(cb, c, s);
}
extern "C" __declspec(dllexport) void VKAPI_CALL vkCmdBindVertexBuffers2(VkCommandBuffer cb, uint32_t firstBinding, uint32_t bindingCount, const VkBuffer* pBuffers, const VkDeviceSize* pOffsets, const VkDeviceSize* pSizes, const VkDeviceSize* pStrides) {
    auto real = LazyResolve<PFN_vkCmdBindVertexBuffers2>("vkCmdBindVertexBuffers2EXT");
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

// Bump API Version
extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetPhysicalDeviceProperties(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties* pProperties) {
    auto real = LazyResolve<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties");
    if (!real) { LogFatal("vkGetPhysicalDeviceProperties"); return; }
    real(physicalDevice, pProperties);
    if (pProperties) pProperties->apiVersion = VK_API_VERSION_1_3;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties) {
    auto real = LazyResolve<PFN_vkGetPhysicalDeviceProperties2>("vkGetPhysicalDeviceProperties2");
    if (!real) { LogFatal("vkGetPhysicalDeviceProperties2"); return; }
    real(physicalDevice, pProperties);
    if (pProperties) pProperties->properties.apiVersion = VK_API_VERSION_1_3;
}

// =======================================================================
// Stubs pour VK_EXT_extended_dynamic_state3
// On déclare la fonction, mais on ne fait rien (no-op). Si le driver
// réel supporte l'extension, on lui délègue. Sinon, on ignore l'appel.
// =======================================================================
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

// --- Émulation de VK_EXT_private_data / Vulkan 1.3 ---
extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreatePrivateDataSlot(VkDevice device, const VkPrivateDataSlotCreateInfo* pCreateInfo,
                        const VkAllocationCallbacks* pAllocator, VkPrivateDataSlot* pPrivateDataSlot) {
    auto real = LazyResolve<PFN_vkCreatePrivateDataSlot>("vkCreatePrivateDataSlot");
    if (real) return real(device, pCreateInfo, pAllocator, pPrivateDataSlot);
    
    auto real_ext = LazyResolve<PFN_vkCreatePrivateDataSlotEXT>("vkCreatePrivateDataSlotEXT");
    if (real_ext) return real_ext(device, pCreateInfo, pAllocator, pPrivateDataSlot);

    LogStubCall("vkCreatePrivateDataSlot");
    if (pPrivateDataSlot) {
        // On renvoie un handle factice mais non nul pour que le jeu ne crash pas
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
    { "vkEnumerateInstanceVersion", (PFN_vkVoidFunction)vkEnumerateInstanceVersion },
    { "vkEnumerateDeviceExtensionProperties", (PFN_vkVoidFunction)vkEnumerateDeviceExtensionProperties },
    { "vkGetPhysicalDeviceFeatures2",         (PFN_vkVoidFunction)vkGetPhysicalDeviceFeatures2 },
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

    // ==================================================================
    // PASSAGE SYSTEMATIQUE ALIAS KHR/EXT -- meme categorie de bug que le
    // crash AMD sur synchronization2 : chaque fonction core-1.3/1.4
    // ci-dessus a un nom d'extension historique que les vieux bindings
    // (LWJGL y compris) peuvent encore utiliser directement. Marche par
    // chance sur un driver qui a l'extension native (le forward .def /
    // vrai loader repond avant d'arriver ici) ; casse partout ailleurs.
    // ==================================================================

    // VK_KHR_synchronization2 (reste du groupe)
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
    
	// Radiance test fix
	{ "vkCmdWaitEvents2",                   (PFN_vkVoidFunction)vkCmdWaitEvents2 },
    { "vkCmdBindDescriptorSets2",           (PFN_vkVoidFunction)vkCmdBindDescriptorSets2 },
    { "vkCmdPushConstants2",                (PFN_vkVoidFunction)vkCmdPushConstants2 },
    { "vkCmdPushDescriptorSetWithTemplate2", (PFN_vkVoidFunction)vkCmdPushDescriptorSetWithTemplate2 },
    { "vkCmdPushDescriptorSetWithTemplate2KHR", (PFN_vkVoidFunction)vkCmdPushDescriptorSetWithTemplate2KHR },

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

    // core 1.3
    { "vkCreatePrivateDataSlot", (PFN_vkVoidFunction)vkCreatePrivateDataSlot },
    { "vkDestroyPrivateDataSlot", (PFN_vkVoidFunction)vkDestroyPrivateDataSlot },
    { "vkSetPrivateData", (PFN_vkVoidFunction)vkSetPrivateData },
    { "vkGetPrivateData", (PFN_vkVoidFunction)vkGetPrivateData },
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

    // Repli pour les fonctions globales si le loader ne les trouve pas via instance
    if (g_real.module)
        fn = (PFN_vkVoidFunction)GetProcAddress(g_real.module, pName);

    if (fn) return fn;

    // --- PIÈGE À NULL ---
    std::string msg = "[vk-emu] FONCTION MANQUANTE (Instance): " + std::string(pName) + "\n";
    fputs(msg.c_str(), stderr);
    fflush(stderr);
    return (PFN_vkVoidFunction)vkEmuNullStubRet0;
}

static std::mutex g_deviceMutex;
static std::unordered_map<VkDevice, PFN_vkGetDeviceProcAddr> g_realDeviceProcAddr;

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

    // --- PIÈGE À NULL ---
    // La fonction n'existe ni dans notre code ni dans le driver.
    // On renvoie un stub qui loguera l'appel pour éviter le segfault (pc=0x0)
    std::string msg = "[vk-emu] FONCTION MANQUANTE: " + std::string(pName) + "\n";
    fputs(msg.c_str(), stderr);
    fflush(stderr);
    return (PFN_vkVoidFunction)vkEmuNullStubRet0;
}