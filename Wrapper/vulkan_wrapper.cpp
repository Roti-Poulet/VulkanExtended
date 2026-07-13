// vulkan_wrapper.cpp (v3 -- fusion)
//
// Proxy vulkan-1.dll : convertit une application ciblant Vulkan 1.3/1.4 en
// appels compatibles avec un pilote limite a Vulkan 1.2.175 (max supporte
// par les pilotes Nvidia sur Windows 7), en emulant VK_KHR_dynamic_rendering
// au-dessus de vkCreateRenderPass2 (core 1.2) + separate_depth_stencil_layouts
// + imageless_framebuffer.
//
// Ce fichier fusionne l'ancien vulkan_proc_dispatch.cpp (trampolines/stubs
// exportes) et l'ancien dynamic_rendering_emu.cpp (emulation du render
// pass/framebuffer), et comble les trous d'infrastructure que le 2e
// referencait sans les definir : g_real n'avait pas les pointeurs de
// fonctions de rendu, et il n'existait aucun registre pour retrouver le
// VkFormat/VkSampleCountFlagBits d'un VkImageView ni le VkDevice d'un
// VkCommandBuffer. Voir la section 3 ci-dessous.
//
// IMPORTANT : par rapport a la version precedente, vkCreateImage,
// vkDestroyImage, vkCreateImageView, vkDestroyImageView,
// vkAllocateCommandBuffers et vkFreeCommandBuffers doivent maintenant etre
// de VRAIS exports a nous (ils alimentent les registres ci-dessous) et
// NE DOIVENT PLUS etre de simples forwarders dans le .def -- voir le
// vulkan-1.def mis a jour.

#include <vulkan/vulkan.h>
#include <windows.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <mutex>
#include <cstdio>
#include <cstring>

// =======================================================================
// 1. Logging (une fois par fonction, au moment de l'APPEL reel, pas du lookup)
// =======================================================================
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

// =======================================================================
// 2. Chargement du vrai loader systeme + resolution paresseuse
// =======================================================================
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
        fprintf(stderr, "[vk-emu] ERREUR FATALE: impossible de charger %s\n", path.c_str());
        return;
    }
    g_real.GetInstanceProcAddr =
        (PFN_vkGetInstanceProcAddr)GetProcAddress(g_real.module, "vkGetInstanceProcAddr");
}

// NOTE CORRECTIF v3 : l'ancienne version mettait le cache "static PFN cached"
// DANS le corps du template -- il est donc partage par tous les appels
// instancies avec le MEME type PFN, pas par nom de fonction. Tant qu'aucune
// paire de fonctions Vulkan differentes ne partage exactement la meme
// signature (donc le meme typedef PFN_...), ca passait par chance, mais
// c'est fragile : une seule collision de signature renverrait le mauvais
// pointeur. Le cache est maintenant une table globale indexee par nom.
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

// =======================================================================
// 3. Registres internes -- combattent les trous laisses par
//    dynamic_rendering_emu.cpp : VkRenderingAttachmentInfo ne donne qu'un
//    VkImageView, jamais son format/samples, et EMU_vkCmdBeginRendering
//    n'a qu'un VkCommandBuffer, jamais le VkDevice associe. Vulkan ne
//    fournit aucune API pour retrouver ces informations depuis les seuls
//    handles : il faut les capturer nous-memes a la creation.
// =======================================================================

// --- VkImage -> nombre d'echantillons (pour retrouver le samples d'une vue) ---
static std::mutex g_imageMutex;
static std::unordered_map<VkImage, VkSampleCountFlagBits> g_imageSamples;

// --- VkImageView -> (format, samples) ---
// --- VkImage -> info complète (samples, dimensions, usage) ---
struct ImageInfo {
    VkSampleCountFlagBits samples;
    uint32_t width;
    uint32_t height;
    uint32_t arrayLayers;
    VkImageUsageFlags usage;
};
static std::unordered_map<VkImage, ImageInfo> g_imageInfo;

// --- VkImageView -> info complète ---
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

// =======================================================================
// 4. Emulation du dynamic rendering (ex dynamic_rendering_emu.cpp)
// =======================================================================

struct AttachmentDesc {
    VkFormat format;
    VkSampleCountFlagBits samples;
    VkAttachmentLoadOp loadOp;
    VkAttachmentStoreOp storeOp;
    VkImageLayout layout;      // layout pendant le render pass (= imageLayout donne a BeginRendering)
    bool hasResolve;
    VkFormat resolveFormat;    // si resolve utilise

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
    VkRenderPass renderPass;   // le render pass emule associe
    uint32_t width, height, layers;
    // avec imageless framebuffer, PAS besoin des VkImageView ici :
    // seulement les formats (deja couverts par renderPass) + dimensions.
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

    // LIMITE CONNUE : RESUMING_BIT / SUSPENDING_BIT (dynamic rendering
    // permettant de "couper" un render pass entre plusieurs command
    // buffers) n'a pas d'equivalent propre en render pass classique.
    // A gerer au cas par cas si une appli cible l'utilise reellement.
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

// =======================================================================
// 5. vkCreateInstance / vkEnumerateInstanceVersion
// =======================================================================
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

    VkResult result = real_vkCreateInstance(&ci, pAllocator, pInstance);
    if (result == VK_SUCCESS)
        g_lastInstance = *pInstance; // memorise pour LazyResolve<>
    return result;
}

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkEnumerateInstanceVersion(uint32_t* pApiVersion) {
    *pApiVersion = VK_API_VERSION_1_3;
    return VK_SUCCESS;
}

// =======================================================================
// 5bis. LE POINT MANQUANT : sans ces 3 interceptions, l'appli ne voit
// jamais VK_KHR_dynamic_rendering, meme si on l'emule parfaitement plus
// bas. Un moteur serieux (VulkanMod inclus) verifie l'extension ET/OU
// la feature AVANT de jamais appeler vkCmdBeginRendering -- l'emulation
// ne sert a rien si ces 3 fonctions ne mentent pas de facon coherente.
// =======================================================================

// Liste des extensions qu'on pretend supporter alors que le driver reel
// ne les connait pas (a completer au fur et a mesure des emulations).
static const char* const g_fakedDeviceExtensions[] = {
    VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, // "VK_KHR_dynamic_rendering"
};
static constexpr uint32_t g_fakedDeviceExtensionCount =
    (uint32_t)(sizeof(g_fakedDeviceExtensions) / sizeof(g_fakedDeviceExtensions[0]));

static bool IsFakedExtension(const char* name) {
    for (uint32_t i = 0; i < g_fakedDeviceExtensionCount; i++)
        if (strcmp(name, g_fakedDeviceExtensions[i]) == 0)
            return true;
    return false;
}

// --- 1. vkEnumerateDeviceExtensionProperties : on ajoute nos fausses
//        extensions a la vraie liste renvoyee par le driver. ---
extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName,
                                      uint32_t* pPropertyCount, VkExtensionProperties* pProperties) {
    auto real = LazyResolve<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
    if (!real) { LogFatal("vkEnumerateDeviceExtensionProperties"); return VK_ERROR_INITIALIZATION_FAILED; }

    // On ne truque jamais la liste par layer (pLayerName != nullptr) --
    // seule la liste "driver" (pLayerName == nullptr) nous interesse ici.
    if (pLayerName != nullptr)
        return real(physicalDevice, pLayerName, pPropertyCount, pProperties);

    uint32_t realCount = 0;
    VkResult res = real(physicalDevice, nullptr, &realCount, nullptr);
    if (res != VK_SUCCESS) return res;

    std::vector<VkExtensionProperties> realProps(realCount);
    res = real(physicalDevice, nullptr, &realCount, realProps.data());
    if (res != VK_SUCCESS && res != VK_INCOMPLETE) return res;

    // N'ajoute que les extensions qui ne sont pas deja reellement presentes
    // (au cas ou un futur pilote/loader plus recent l'exposerait vraiment).
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

// --- 2. vkGetPhysicalDeviceFeatures2 : on force dynamicRendering=VK_TRUE
//        dans le pNext chain si l'appli le demande. ---
extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures) {
    auto real = LazyResolve<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
    if (!real) { LogFatal("vkGetPhysicalDeviceFeatures2"); return; }

    real(physicalDevice, pFeatures);

    // On parcourt le pNext chain fourni par L'APPLI (pas celui du driver --
    // le driver ne connait pas ces structs et les aura donc laissees
    // intactes/non remplies, conformement au spec qui dit d'ignorer les
    // sType inconnus). On cherche les structs qui demandent
    // dynamicRendering et on force TRUE nous-memes.
    VkBaseOutStructure* cur = (VkBaseOutStructure*)pFeatures->pNext;
    while (cur) {
        if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES) {
            ((VkPhysicalDeviceDynamicRenderingFeatures*)cur)->dynamicRendering = VK_TRUE;
        } else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            ((VkPhysicalDeviceVulkan13Features*)cur)->dynamicRendering = VK_TRUE;
        }
        cur = cur->pNext;
    }
}

// --- 3. vkCreateDevice : on retire nos fausses extensions de la liste
//        avant de forwarder, sinon le vrai driver refuse avec
//        VK_ERROR_EXTENSION_NOT_PRESENT (il ne les connait pas). ---
extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
               const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    auto real = LazyResolve<PFN_vkCreateDevice>("vkCreateDevice");
    if (!real) { LogFatal("vkCreateDevice"); return VK_ERROR_INITIALIZATION_FAILED; }

    VkDeviceCreateInfo ci = *pCreateInfo;

    std::vector<const char*> filteredExt;
    filteredExt.reserve(ci.enabledExtensionCount);
    for (uint32_t i = 0; i < ci.enabledExtensionCount; i++) {
        if (!IsFakedExtension(ci.ppEnabledExtensionNames[i]))
            filteredExt.push_back(ci.ppEnabledExtensionNames[i]);
    }
    ci.enabledExtensionCount = (uint32_t)filteredExt.size();
    ci.ppEnabledExtensionNames = filteredExt.empty() ? nullptr : filteredExt.data();

    // 1. On retire les sType inconnus du driver (Vulkan 1.3 features, dynamic rendering)
    // pour éviter que le vieux driver ne crash en lisant la pNext chain.
    VkBaseOutStructure* prev = (VkBaseOutStructure*)&ci;
    VkBaseOutStructure* curr = (VkBaseOutStructure*)ci.pNext;
    while (curr) {
        if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES ||
            curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            prev->pNext = curr->pNext; // bypass
        } else {
            prev = curr;
        }
        curr = curr->pNext;
    }

    // 2. On s'assure que la feature imagelessFramebuffer est activée dans la chaine
    bool imagelessRequested = false;
    curr = (VkBaseOutStructure*)ci.pNext;
    while (curr) {
        if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES) {
            ((VkPhysicalDeviceImagelessFramebufferFeatures*)curr)->imagelessFramebuffer = VK_TRUE;
            imagelessRequested = true;
            break;
        } else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            ((VkPhysicalDeviceVulkan12Features*)curr)->imagelessFramebuffer = VK_TRUE;
            imagelessRequested = true;
            break;
        }
        curr = curr->pNext;
    }

    VkPhysicalDeviceImagelessFramebufferFeatures imagelessFeature{};
    if (!imagelessRequested) {
        imagelessFeature.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGELESS_FRAMEBUFFER_FEATURES;
        imagelessFeature.imagelessFramebuffer = VK_TRUE;
        imagelessFeature.pNext = (void*)ci.pNext;
        ci.pNext = &imagelessFeature;
    }

    return real(physicalDevice, &ci, pAllocator, pDevice);
}

// =======================================================================
// 6. Stubs "incertains" -- vraie signature, vrai export, essaie le driver
// reel en 1er via LazyResolve<>, sinon logue + valeur de repli sure.
//
// A verifier au cas par cas sur vulkan.gpuinfo.org: certaines de ces
// fonctions peuvent en realite deja marcher via l'aliasing du loader
// (ex: si le driver expose l'extension KHR/EXT correspondante), auquel
// cas le log de stub ne se declenchera JAMAIS et tu peux les retirer
// de cette liste (et les remettre en forward pur dans le .def).
// =======================================================================

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
    // Repli correct (pas un no-op dangereux): retombe sur vkMapMemory (1.0).
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
    // No-op assume ici que l'appli utilise le mapping identite par defaut
    // (le cas le plus courant). A verifier si un jeu cible s'en sert
    // vraiment pour re-mapper des indices -- sinon il faudra le prendre
    // en compte dans l'emulation du dynamic rendering elle-meme.
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
    LogStubCall("vkGetDeviceImageMemoryRequirements");
    // Valeurs sures plutot que garbage: taille nulle = detectable au
    // debug (l'appli allouera un buffer de 0 octet, plutot qu'un crash
    // silencieux sur de la memoire non initialisee).
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
    *pSparseMemoryRequirementCount = 0; // "pas de memoire sparse requise"
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetDeviceBufferMemoryRequirements(VkDevice device, const VkDeviceBufferMemoryRequirements* pInfo,
                                     VkMemoryRequirements2* pMemoryRequirements) {
    using PFN = PFN_vkGetDeviceBufferMemoryRequirements;
    if (PFN real = LazyResolve<PFN>("vkGetDeviceBufferMemoryRequirements")) {
        real(device, pInfo, pMemoryRequirements);
        return;
    }
    LogStubCall("vkGetDeviceBufferMemoryRequirements");
    pMemoryRequirements->memoryRequirements.size = 0;
    pMemoryRequirements->memoryRequirements.alignment = 1;
    pMemoryRequirements->memoryRequirements.memoryTypeBits = 0xFFFFFFFF;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetRenderingAreaGranularity(VkDevice device, const VkRenderingAreaInfo* pRenderingAreaInfo, VkExtent2D* pGranularity) {
    using PFN = PFN_vkGetRenderingAreaGranularity;
    if (PFN real = LazyResolve<PFN>("vkGetRenderingAreaGranularity")) {
        real(device, pRenderingAreaInfo, pGranularity);
        return;
    }
    LogStubCall("vkGetRenderingAreaGranularity");
    pGranularity->width = 1; pGranularity->height = 1; // granularite triviale, toujours valide
}

// --- host_image_copy (1.4) : VkResult, plus delicat a "faussement" reussir ---

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCopyMemoryToImage(VkDevice device, const VkCopyMemoryToImageInfo* pInfo) {
    using PFN = PFN_vkCopyMemoryToImage;
    if (PFN real = LazyResolve<PFN>("vkCopyMemoryToImage"))
        return real(device, pInfo);
    LogStubCall("vkCopyMemoryToImage");
    // Pas de repli sur, host_image_copy contourne le staging buffer + upload
    // classique -- si le driver ne l'a pas, l'appli DOIT normalement avoir
    // teste la feature avant (VkPhysicalDeviceHostImageCopyFeatures=FALSE
    // qu'on renverra honnetement). Si elle appelle quand meme: erreur propre.
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
// =======================================================================
// 7. vkGetInstanceProcAddr / vkGetDeviceProcAddr : chemin normal recommande
// par le spec pour tout ce qui n'est pas core 1.0. Renvoie NOS exports
// pour les noms ci-dessous (deja les vraies implementations), sinon
// transmet au vrai loader.
// =======================================================================

// Forward declarations nécessaires car g_ourExports est défini avant
// les implémentations de ces deux fonctions plus bas dans le fichier.
extern "C" __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName);

extern "C" __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* pName);

static const std::unordered_map<std::string, PFN_vkVoidFunction> g_ourExports = {
    // --- CRITIQUE : sans ces deux entrées, l'appli obtient le VRAI
    // vkGetDeviceProcAddr du loader système (pas le nôtre), et donc
    // ne passe JAMAIS par notre wrapper pour résoudre les fonctions
    // de device. Toutes les émulations sont alors invisibles. ---
    { "vkGetInstanceProcAddr",            (PFN_vkVoidFunction)vkGetInstanceProcAddr },
    { "vkGetDeviceProcAddr",              (PFN_vkVoidFunction)vkGetDeviceProcAddr },

    { "vkCreateInstance",           (PFN_vkVoidFunction)vkCreateInstance },
    { "vkEnumerateInstanceVersion", (PFN_vkVoidFunction)vkEnumerateInstanceVersion },
    // CORRECTIF: sans ces 3-la, l'appli voit la vraie liste d'extensions
    // du driver (donc PAS VK_KHR_dynamic_rendering) et abandonne avant
    // meme d'essayer -- c'etait exactement le bug du rapport VulkanMod.
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
    // NOUVEAU v3 -- ces 6 fonctions alimentent les registres de la section 3
    // et doivent donc passer par notre code, pas par un forward .def direct.
    { "vkCreateImage",              (PFN_vkVoidFunction)vkCreateImage },
    { "vkDestroyImage",              (PFN_vkVoidFunction)vkDestroyImage },
    { "vkCreateImageView",           (PFN_vkVoidFunction)vkCreateImageView },
    { "vkDestroyImageView",          (PFN_vkVoidFunction)vkDestroyImageView },
    { "vkAllocateCommandBuffers",    (PFN_vkVoidFunction)vkAllocateCommandBuffers },
    { "vkFreeCommandBuffers",        (PFN_vkVoidFunction)vkFreeCommandBuffers },
    { "vkCreateGraphicsPipelines",         (PFN_vkVoidFunction)vkCreateGraphicsPipelines },
};

extern "C" __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    EnsureRealLoaderLoaded();
    if (!pName) return nullptr;

    if (auto it = g_ourExports.find(pName); it != g_ourExports.end())
        return it->second;

    return g_real.GetInstanceProcAddr(instance, pName);
}

static std::mutex g_deviceMutex;
static std::unordered_map<VkDevice, PFN_vkGetDeviceProcAddr> g_realDeviceProcAddr;

extern "C" __declspec(dllexport) PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    if (!pName) return nullptr;

    if (auto it = g_ourExports.find(pName); it != g_ourExports.end())
        return it->second;

    EnsureRealLoaderLoaded();

    // CORRECTIF : On DOIT utiliser g_lastInstance (mémorisé lors de vkCreateInstance) 
    // et NON nullptr. La spec Vulkan dit que vkGetInstanceProcAddr(nullptr, ...) 
    // renvoie NULL pour les fonctions de device comme vkGetDeviceProcAddr.
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
        return realGetter(device, pName);
    }

    // Repli de sécurité au cas où g_lastInstance ne serait pas encore défini
    // ou si le loader ne trouve pas vkGetDeviceProcAddr.
    return g_real.GetInstanceProcAddr(g_lastInstance, pName);
}