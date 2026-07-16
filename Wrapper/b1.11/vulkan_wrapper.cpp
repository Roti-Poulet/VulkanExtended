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
#include <algorithm>

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
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
	VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
	// VK_EXT_MESH_SHADER_EXTENSION_NAME retire d'ici : aucun repli logiciel possible
    // (etape materielle du pipeline graphique, pas d'equivalent 1.2 emulable). On ne
    // veut PAS mentir en l'annoncant "faked" si le vrai driver/GPU ne le supporte pas --
    // voir vkCmdDrawMeshTasksEXT/etc. plus bas : passthrough pur, aucune emulation.
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
    // Ajoute : ces extensions sont deja wrappees/emulees plus bas dans ce fichier
    // (vkCopyMemoryToImage/ToMemory/ToImage/vkTransitionImageLayout, vkMapMemory2/
    // vkUnmapMemory2) mais n'etaient pas declarees ici -- incoherence corrigee.
    VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME,
    VK_KHR_MAP_MEMORY_2_EXTENSION_NAME,
    VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME
};
static constexpr uint32_t g_fakedDeviceExtensionCount =
    (uint32_t)(sizeof(g_fakedDeviceExtensions) / sizeof(g_fakedDeviceExtensions[0]));

// Extensions genuinement supportees par le vrai driver mais qu'on choisit de
// MASQUER completement (jamais rapportees a vkEnumerateDeviceExtensionProperties,
// jamais activees a vkCreateDevice) -- pas d'emulation, pas de faking, juste
// invisibles pour le jeu, comme si le GPU ne les avait jamais eues.
static const char* const g_blockedDeviceExtensions[] = {
    // VK_AMD_buffer_marker : feature optionnelle de debug GPU (post-mortem
    // crash dump). Notre vkCmdWriteBufferMarker2AMD (variante sync2) n'est
    // qu'un stub qui log et ne fait rien -- retiree de l'enumeration pour
    // que le jeu ne l'active meme pas (log "not supported", comme pour
    // NV Diagnostic Checkpoint / EXT MultiDraw).
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

    // ===================================================================
    // CORRECTIF 0x0/ACCESS_VIOLATION : ces pointeurs sont resolus UNE SEULE
    // FOIS, juste apres vkCreateDevice, via vkGetDeviceProcAddr(*pDevice, ...)
    // -- PAS via vkGetInstanceProcAddr comme le faisait LazyResolve. Certains
    // drivers (AMD notamment) ne repondent pas fiablement a une demande
    // instance-level pour des commandes device-level post-1.0 ; et
    // LazyResolve mettait en cache un echec (nullptr) de facon PERMANENTE au
    // premier essai, meme si l'essai avait eu lieu trop tot (avant qu'un
    // vrai VkInstance ne soit enregistre). Ici : resolution ciblee, par
    // device, au bon moment (juste apres creation reussie), jamais reessayee
    // ni partagee entre devices.
    // ===================================================================
    bool sync2_native = false;
    PFN_vkCmdPipelineBarrier2 fn_CmdPipelineBarrier2 = nullptr;
    PFN_vkQueueSubmit2        fn_QueueSubmit2        = nullptr;
    PFN_vkCmdWriteTimestamp2  fn_CmdWriteTimestamp2  = nullptr;
    PFN_vkCmdSetEvent2        fn_CmdSetEvent2        = nullptr;
    PFN_vkCmdResetEvent2      fn_CmdResetEvent2      = nullptr;
    PFN_vkCmdWaitEvents2      fn_CmdWaitEvents2      = nullptr;

    bool push_descriptor_native = false;
    PFN_vkCmdPushDescriptorSetKHR fn_CmdPushDescriptorSetKHR = nullptr;

    // Repli logiciel (legacy) resolus une fois aussi, meme logique/raison.
    PFN_vkCmdPipelineBarrier fn_CmdPipelineBarrier = nullptr;
    PFN_vkQueueSubmit        fn_QueueSubmit        = nullptr;
    PFN_vkCmdWriteTimestamp  fn_CmdWriteTimestamp  = nullptr;
    PFN_vkCmdSetEvent        fn_CmdSetEvent        = nullptr;
    PFN_vkCmdResetEvent      fn_CmdResetEvent      = nullptr;
    PFN_vkCmdWaitEvents      fn_CmdWaitEvents      = nullptr;
};
static std::mutex g_deviceStateMutex;
static std::unordered_map<VkDevice, DeviceState> g_deviceState;

// Deplacees ici (etaient plus bas, pres de vkGetDeviceProcAddr) : on en a
// besoin des vkCreateDevice pour resoudre correctement les commandes
// device-level via le vrai vkGetDeviceProcAddr, pas via vkGetInstanceProcAddr.
// CORRECTIF HANG AMD : voir resolveDev dans vkCreateDevice plus bas.
static std::mutex g_deviceMutex;
static std::unordered_map<VkDevice, PFN_vkGetDeviceProcAddr> g_realDeviceProcAddr;

// Recupere une COPIE de l'etat du device (evite de garder le mutex pendant
// l'appel Vulkan reel qui suit -- un appel Vulkan peut prendre du temps et
// on ne veut pas serialiser tous les threads dessus).
static bool GetDeviceState(VkDevice device, DeviceState* out) {
    std::lock_guard<std::mutex> lock(g_deviceStateMutex);
    auto it = g_deviceState.find(device);
    if (it == g_deviceState.end()) return false;
    *out = it->second;
    return true;
}

// --- Suivi VkQueue -> VkDevice (necessaire pour vkQueueSubmit2, qui ne
// recoit qu'une VkQueue, pas un VkDevice ni un VkCommandBuffer). ---
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

// =====================================================================
// --- Emulation de VK_EXT_descriptor_buffer ---
// =====================================================================
// Design : comme le format binaire reel des descripteurs GPU (ce que
// vkGetDescriptorEXT est cense ecrire) est prive/opaque au driver, on ne peut
// pas le reproduire a l'identique. A la place, on ecrit NOUS-MEMES une
// representation logique (le "slot") a l'adresse pDescriptor fournie par
// l'appelant (qui est de la memoire mappee du descriptor buffer, donc
// ecrivable/relisible normalement par l'appli). Au moment du bind reel
// (vkCmdSetDescriptorBufferOffsetsEXT), on relit ce slot depuis la memoire
// mappee, on reconstruit un vrai VkDescriptorSet (pioche dans un pool interne
// cache par empreinte de contenu), et on bind ce VkDescriptorSet classique.
// -----------------------------------------------------------------------

// Marqueur pour reconnaitre nos propres slots ecrits en memoire (evite de
// confondre avec du contenu driver reel si jamais le pilote gere aussi
// nativement descriptor_buffer sur une portion du code -- on force toujours
// notre propre emulation complete, jamais un melange partiel).
static constexpr uint32_t kDescSlotMagic = 0x44455343; // "DESC"

// Represente un seul descripteur ecrit via vkGetDescriptorEXT, stocke tel
// quel en memoire mappee a l'offset demande par l'appelant.
struct DescSlot {
    uint32_t magic = kDescSlotMagic;
    VkDescriptorType type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    // Pour les types image / sampler (VkDescriptorImageInfo)
    VkSampler sampler = VK_NULL_HANDLE;
    VkImageView imageView = VK_NULL_HANDLE;
    VkImageLayout imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // Pour les types buffer (VkDescriptorAddressInfoEXT) -- on garde l'adresse
    // GPU brute, resolue en VkBuffer reel au moment du bind via
    // g_deviceAddressToBuffer (cf. plus bas).
    VkDeviceAddress bufferAddress = 0;
    VkDeviceSize bufferRange = 0;
    VkFormat texelBufferFormat = VK_FORMAT_UNDEFINED;
};

// VkDeviceAddress -> (VkBuffer, base address, size) pour pouvoir retrouver le
// VkBuffer reel et l'offset relatif a partir d'une adresse GPU brute (utilisee
// a la fois pour les descriptor buffers eux-memes et pour les buffers
// referme wrapper via VkDescriptorAddressInfoEXT).
struct BufferAddressInfo {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceAddress baseAddress = 0;
    VkDeviceSize size = 0;
};
static std::mutex g_bufferAddressMutex;
static std::unordered_map<VkDeviceAddress, BufferAddressInfo> g_bufferAddressByBase;
// Recherche par plage (necessite de parcourir -- rare, seulement au bind) :
// on garde aussi un vecteur trie par adresse de base pour une recherche par
// intervalle en O(log n) plutot qu'un scan lineaire de la map.
static std::vector<BufferAddressInfo> g_bufferAddressSorted; // maintenu trie
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

// Retrouve quel VkBuffer contient l'adresse GPU donnee, et l'offset relatif
// a l'interieur de ce buffer. Retourne VK_NULL_HANDLE si non trouve (l'appli
// a fourni une adresse qu'on n'a jamais vue passer par vkGetBufferDeviceAddress
// via CE wrapper -- ne devrait pas arriver en usage normal).
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
    // Recherche binaire du plus grand baseAddress <= addr
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

// --- Etat par command buffer : quels descriptor buffers sont "bindes"
// (vkCmdBindDescriptorBuffersEXT) via leur index d'appel, en attendant le
// vkCmdSetDescriptorBufferOffsetsEXT qui indique quel set utiliser a quel
// offset dans quel buffer. ---
struct BoundDescBuffer {
    VkDeviceAddress address = 0; // adresse de base du descriptor buffer
};
static std::mutex g_cmdDescBufMutex;
static std::unordered_map<VkCommandBuffer, std::vector<BoundDescBuffer>> g_cmdBoundDescBuffers;

// --- Pool interne de VkDescriptorSet geres par CE wrapper (jamais expose a
// l'appelant), avec cache par empreinte de contenu pour eviter de recreer un
// VkDescriptorSet identique a chaque draw. Cle = (VkDescriptorSetLayout,
// hash du contenu des slots). ---
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
    // Nombre de sets alloues jusqu'ici dans ce pool -- sert a savoir quand il
    // faut passer a un nouveau pool (VK_ERROR_OUT_OF_POOL_MEMORY probable).
    uint32_t allocatedCount = 0;
    static constexpr uint32_t kMaxSetsPerPool = 4096; // reallocation par lots
};
static std::mutex g_descBufPoolMutex;
static std::unordered_map<VkDevice, std::vector<EmuDescBufferPool>> g_descBufPools;
static std::unordered_map<VkDevice, std::unordered_map<EmuDescSetKey, VkDescriptorSet, EmuDescSetKeyHash>> g_descBufSetCache;
// NOTE : GetOrCreateEmuDescPool (utilise LazyResolve) est definie plus bas
// dans ce fichier, juste apres la definition du template LazyResolve.

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

// Cree (ou reutilise le dernier) VkDescriptorPool interne pour ce device,
// dimensionne large pour couvrir tous les types de descripteurs usuels --
// evite d'avoir a connaitre a l'avance la repartition exacte par type.
// (Definie ici, apres LazyResolve/RealDeviceHasExtension, dont elle depend.)
static VkDescriptorPool GetOrCreateEmuDescPool(VkDevice device) {
    std::lock_guard<std::mutex> lock(g_descBufPoolMutex);
    auto& pools = g_descBufPools[device];
    if (!pools.empty() && pools.back().allocatedCount < EmuDescBufferPool::kMaxSetsPerPool)
        return pools.back().pool;

    auto real_createPool = LazyResolve<PFN_vkCreateDescriptorPool>("vkCreateDescriptorPool");
    if (!real_createPool) return VK_NULL_HANDLE;

    // Dimensionnement large : ce pool sert uniquement a l'emulation interne
    // (jamais expose au jeu), donc le cout memoire d'un pool genereux est
    // largement compense par le fait qu'on evite les reallocations frequentes.
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

// --- Suivi VkQueue -> VkDevice, necessaire pour que vkQueueSubmit2 (qui ne
// recoit qu'une VkQueue) puisse retrouver le DeviceState correspondant. ---
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

    // ===================================================================
    // CORRECTIF HANG AMD : sans VkSubpassDependency explicite ici, ce
    // render pass classique n'offre AUCUNE synchronisation implicite a
    // ses frontieres (dependencyCount=0 -> pas de barriere implicite du
    // tout, ni en entree ni en sortie -- ce n'est PAS pareil que "safe by
    // default"). Le dynamic rendering natif n'a pas besoin de ca (l'appli
    // gere ses propres barrieres explicites autour de vkCmdBeginRendering/
    // EndRendering), mais un vrai vkCmdBeginRenderPass2 EST concu autour
    // de l'idee que ces dependances existent. Nvidia tolere/compense ca
    // via heuristiques internes ; RADV (Mesa/AMD), plus strict au spec,
    // peut se retrouver sans aucune garantie d'ordonnancement aux
    // frontieres du render pass -> hang GPU reel (correspond exactement
    // au symptome observe : hang uniquement sur AMD, jamais sur Nvidia).
    // On ajoute donc une paire de dependances larges et conservatrices
    // (ALL_COMMANDS / lecture+ecriture memoire complete) en entree et en
    // sortie, pour que le render pass fournisse LUI-MEME une garantie de
    // synchro correcte, en plus des barrieres explicites que l'appli a pu
    // deja poser (redondant mais jamais incorrect -- juste potentiellement
    // un peu moins performant qu'une dependance ciblee).
    // ===================================================================
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

    // Retire les extensions qu'on a choisi de masquer completement (voir
    // g_blockedDeviceExtensions) avant meme de fusionner avec les "faked" --
    // le jeu ne doit jamais les voir dans la liste, qu'elles soient reelles ou pas.
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
    if (!real) { LogFatal("vkGetPhysicalDeviceFeatures2"); return; }

    real(physicalDevice, pFeatures); // Le driver remplit les VRAIES features

    VkBaseOutStructure* cur = (VkBaseOutStructure*)pFeatures->pNext;
    while (cur) {
        if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* f = (VkPhysicalDeviceVulkan12Features*)cur;
            // On force uniquement ce qu'on EMULE reellement
            f->imagelessFramebuffer = VK_TRUE; // Necessaire pour notre emulation dynamicRendering
            // Les autres features 1.2 (timelineSemaphore, bufferDeviceAddress, etc.)
            // sont laissees a la valeur renvoyee par le VRAI driver pour eviter les crashs.
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
            f->synchronization2 = VK_TRUE;                    // Emulé
            f->textureCompressionASTC_HDR = VK_FALSE;         
            f->shaderZeroInitializeWorkgroupMemory = VK_TRUE; 
            f->dynamicRendering = VK_TRUE;                    // Emulé
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
            f->pushDescriptor = VK_TRUE;                      // Emulé
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
    // DIAGNOSTIC TEMPORAIRE : voir si l'app demande bien l'extension et ce
    // que RealDeviceHasExtension repond reellement pour ce physicalDevice.
    fprintf(stderr, "[vk-emu] DIAGNOSTIC push_descriptor: real_has_pd=%d, extensions demandees par l'app (%u):\n",
            (int)real_has_pd, pCreateInfo->enabledExtensionCount);
    for (uint32_t di = 0; di < pCreateInfo->enabledExtensionCount; di++) {
        fprintf(stderr, "  [app-requested] -> %s\n", pCreateInfo->ppEnabledExtensionNames[di]);
    }
    fflush(stderr);
    bool real_has_eds3 = RealDeviceHasExtension(physicalDevice, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    bool real_has_hic = RealDeviceHasExtension(physicalDevice, VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME);
    bool real_has_m5 = RealDeviceHasExtension(physicalDevice, VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    bool real_has_vids = RealDeviceHasExtension(physicalDevice, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    bool real_has_mesh = RealDeviceHasExtension(physicalDevice, VK_EXT_MESH_SHADER_EXTENSION_NAME);

    // =====================================================================
    // CORRECTIF AMD: Interroger les VRAIES features supportees par le driver
    // pour eviter de forcer VK_TRUE sur des features non supportees (ce qui
    // cause VK_ERROR_FEATURE_NOT_PRESENT sur AMD).
    // =====================================================================
    VkPhysicalDeviceFeatures2 realFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceVulkan12Features real12Features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceDescriptorIndexingFeatures realDescIndexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    realFeatures.pNext = &real12Features;
    real12Features.pNext = &realDescIndexing;
    
    auto realGetFeatures2 = LazyResolve<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
    if (realGetFeatures2) {
        realGetFeatures2(physicalDevice, &realFeatures);
    } else {
        auto realGetFeatures = LazyResolve<PFN_vkGetPhysicalDeviceFeatures>("vkGetPhysicalDeviceFeatures");
        if (realGetFeatures) realGetFeatures(physicalDevice, &realFeatures.features);
    }

    // --- Masquer les features de base 1.0 si pEnabledFeatures est utilise ---
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

        // Extension explicitement masquee (voir g_blockedDeviceExtensions) --
        // ne devrait normalement jamais arriver ici puisqu'on ne la rapporte
        // plus dans vkEnumerateDeviceExtensionProperties, mais filet de
        // securite si le jeu la demande quand meme en dur.
        if (IsBlockedDeviceExtension(ext)) {
            fprintf(stderr, "[vk-emu] Extension masquee explicitement, filtrage: %s\n", ext);
            continue;
        }

        // CORRECTIF HANG AMD (push descriptor) : VK_KHR_push_descriptor etait
        // systematiquement retire/emule ici meme quand le vrai driver le
        // supporte nativement (real_has_pd). Consequence sur AMD : la
        // fonction de repli vkCmdPushDescriptorSetWithTemplateKHR resout a
        // nullptr (extension jamais activee => spec-legal de renvoyer null),
        // et l'appel devient un no-op SILENCIEUX (LogStubCall "no legacy
        // fallback") -- les descripteurs ne sont jamais pousses, le draw
        // suivant lit des descripteurs perimes/non lies, et RADV (strict)
        // peut faire fauter/geler le GPU en interne -> timeout de 5s sur le
        // semaphore. Nvidia tolere ca car il renvoie quand meme un pointeur
        // fonctionnel meme sans extension activee. On garde l'extension
        // reelle activee quand le driver la supporte, au lieu de forcer
        // l'emulation dans ce cas.
        if (strcmp(ext, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) == 0 && real_has_pd) {
            filteredExt.push_back(ext);
            continue;
        }

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
        
         if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            auto* f = (VkPhysicalDeviceFeatures2*)curr;
            VkBool32* appF = (VkBool32*)&f->features;
            VkBool32* realF = (VkBool32*)&realFeatures.features;
            size_t featSize = sizeof(VkPhysicalDeviceFeatures);
            for (size_t i = 0; i < featSize / sizeof(VkBool32); i++) {
                if (appF[i] && !realF[i]) appF[i] = VK_FALSE; // Masquer si non supporté
            }
            keep = true;
        }
        else if (curr->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* f = (VkPhysicalDeviceVulkan12Features*)curr;
            // On remplace les features demandees par celles REELLEMENT supportees
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
        // =================================================================
        // Resolution ciblee, device-level, une seule fois -- remplace le
        // mecanisme LazyResolve (instance-level + cache-echec-permanent) qui
        // causait des pointeurs 0x0 et des ACCESS_VIOLATION sur AMD pour
        // synchronization2 et push_descriptor.
        //
        // CORRECTIF HANG/TIMEOUT SEMAPHORE (AMD) : la version precedente
        // appelait encore g_real.GetInstanceProcAddr(g_lastInstance, name)
        // directement pour des commandes device-level (vkQueueSubmit2,
        // vkCmdPipelineBarrier2, etc). Ca ne passe PAS par le vrai
        // vkGetDeviceProcAddr -- or seul vkGetDeviceProcAddr est garanti par
        // la spec de router correctement vers le dispatch du bon device pour
        // ces commandes-la. Comme ce wrapper charge l'ICD brut (pas le loader
        // Khronos officiel, qui fait normalement ce trampoline lui-meme),
        // AMD renvoie parfois un pointeur non-nul via GetInstanceProcAddr qui
        // ne route pas correctement -- sync2_native passait alors a `true` a
        // tort, et vkQueueSubmit2 soumettait le travail de facon incorrecte :
        // le semaphore attendu par la passe suivante n'etait jamais signale
        // -> timeout de 5s ("GUI before blur"). On resout maintenant TOUJOURS
        // via le vrai vkGetDeviceProcAddr(*pDevice, name), et on enregistre ce
        // resolveur dans g_realDeviceProcAddr pour que notre propre
        // vkGetDeviceProcAddr (plus bas) reutilise la meme resolution.
        // =================================================================
        PFN_vkGetDeviceProcAddr realDevProcAddr =
            (PFN_vkGetDeviceProcAddr)g_real.GetInstanceProcAddr(g_lastInstance, "vkGetDeviceProcAddr");
        {
            std::lock_guard<std::mutex> lock(g_deviceMutex);
            g_realDeviceProcAddr[*pDevice] = realDevProcAddr;
        }

        auto resolveDev = [&](const char* name) -> PFN_vkVoidFunction {
            if (!realDevProcAddr) return nullptr;
            return realDevProcAddr(*pDevice, name);
        };

        // --- synchronization2 : "tout ou rien" pour le groupe. ---
        // DIAGNOSTIC : on log systematiquement l'etat, "supporte" ou pas,
        // pour savoir sans ambiguite quel chemin (natif vs emule legacy)
        // est reellement emprunte sur CE driver -- necessaire pour trancher
        // entre "le bug est dans la resolution" et "le bug est dans la
        // traduction legacy elle-meme", ce qu'on ne peut pas deviner a
        // l'aveugle depuis un simple log de timeout cote jeu.
        fprintf(stderr, "[vk-emu] DIAGNOSTIC sync2: real_has_sync2=%d\n", (int)real_has_sync2);
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

            fprintf(stderr, "[vk-emu] DIAGNOSTIC sync2: CmdPipelineBarrier2=%p QueueSubmit2=%p "
                            "CmdWriteTimestamp2=%p CmdSetEvent2=%p CmdResetEvent2=%p CmdWaitEvents2=%p "
                            "=> sync2_native=%d\n",
                            (void*)state.fn_CmdPipelineBarrier2, (void*)state.fn_QueueSubmit2,
                            (void*)state.fn_CmdWriteTimestamp2, (void*)state.fn_CmdSetEvent2,
                            (void*)state.fn_CmdResetEvent2, (void*)state.fn_CmdWaitEvents2,
                            (int)state.sync2_native);

            if (!state.sync2_native) {
                fprintf(stderr, "[vk-emu] ATTENTION: VK_KHR_synchronization2 annonce par le "
                                "driver mais au moins une fonction introuvable -- bascule integrale sur l'emulation.\n");
            }
        }
        fflush(stderr);
        
        // Repli legacy resolu ici aussi
        state.fn_CmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)resolveDev("vkCmdPipelineBarrier");
        state.fn_QueueSubmit        = (PFN_vkQueueSubmit)resolveDev("vkQueueSubmit");
        state.fn_CmdWriteTimestamp  = (PFN_vkCmdWriteTimestamp)resolveDev("vkCmdWriteTimestamp");
        state.fn_CmdSetEvent        = (PFN_vkCmdSetEvent)resolveDev("vkCmdSetEvent");
        state.fn_CmdResetEvent      = (PFN_vkCmdResetEvent)resolveDev("vkCmdResetEvent");
        state.fn_CmdWaitEvents      = (PFN_vkCmdWaitEvents)resolveDev("vkCmdWaitEvents");

        // --- push_descriptor ---
        if (real_has_pd) {
            state.fn_CmdPushDescriptorSetKHR =
                (PFN_vkCmdPushDescriptorSetKHR)resolveDev("vkCmdPushDescriptorSetKHR");
            state.push_descriptor_native = (state.fn_CmdPushDescriptorSetKHR != nullptr);

            if (!state.push_descriptor_native) {
                fprintf(stderr, "[vk-emu] ATTENTION: VK_KHR_push_descriptor annonce par le "
                                "driver mais fonction introuvable -- bascule sur l'emulation.\n");
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
    // Repli : VK_EXT_line_rasterization (core-promu en 1.4, dispo comme EXT en 1.2)
    // expose vkCmdSetLineStippleEXT avec la meme signature -- appel binairement
    // identique, donc repli exact (pas de degradation).
    using PFN_Ext = PFN_vkCmdSetLineStippleEXT;
    if (PFN_Ext ext = LazyResolve<PFN_Ext>("vkCmdSetLineStippleEXT")) {
        ext(commandBuffer, lineStippleFactor, lineStipplePattern);
        return;
    }
    LogStubCall("vkCmdSetLineStipple");
    // Dernier repli : pas de pointille, ligne pleine (degradation visuelle mineure,
    // pas de crash -- comportement par defaut si l'etat n'est jamais set).
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdBindIndexBuffer2(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                       VkDeviceSize size, VkIndexType indexType) {
    // 1) Vraie fonction 1.4 core, si le driver l'expose deja (peu probable en 1.2.175).
    using PFN = PFN_vkCmdBindIndexBuffer2;
    if (PFN real = LazyResolve<PFN>("vkCmdBindIndexBuffer2")) {
        real(commandBuffer, buffer, offset, size, indexType);
        return;
    }
    // 2) VK_KHR_maintenance5 promeut cette fonction depuis 1.2.175 sous forme KHR :
    // vkCmdBindIndexBuffer2KHR a exactement la meme signature -- repli exact, "size"
    // est bien respecte (borne la region d'indices utilisable par le prochain draw).
    using PFN_Khr = PFN_vkCmdBindIndexBuffer2KHR;
    if (PFN_Khr khr = LazyResolve<PFN_Khr>("vkCmdBindIndexBuffer2KHR")) {
        khr(commandBuffer, buffer, offset, size, indexType);
        return;
    }

    // 3) Dernier repli : core 1.0 vkCmdBindIndexBuffer, qui n'a pas de parametre
    // "size". ATTENTION : ceci ne borne PAS la lecture d'indices a [offset, offset+size) ;
    // si l'appelant utilise size pour partager un seul gros index buffer entre plusieurs
    // sous-maillages, le comportement differera (le draw pourra techniquement lire au-dela
    // de "size", bien que vkCmdDrawIndexed reste, in fine, borne par indexCount+firstIndex).
    // On logue explicitement cette degradation au lieu de la masquer.
    LogStubCall("vkCmdBindIndexBuffer2 (repli 1.0 sans 'size' -- VK_KHR_maintenance5 absent du pilote)");
    using PFN_Legacy = PFN_vkCmdBindIndexBuffer;
    if (PFN_Legacy legacy = LazyResolve<PFN_Legacy>("vkCmdBindIndexBuffer"))
        legacy(commandBuffer, buffer, offset, indexType);
}

// =======================================================================
// Infrastructure manquante pour VK_EXT_descriptor_buffer -- sans ca, les
// registres consultes plus bas (g_descSetLayoutInfo, g_pipelineLayoutSets,
// g_activeMappings) ne sont jamais remplis et l'emulation ne fait rien.
// =======================================================================

// --- 1. g_descSetLayoutInfo : quels indices de binding un
//        VkDescriptorSetLayout contient, dans l'ordre de creation. ---
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
    
    // Si le driver natif ne supporte pas le push descriptor, on retire le flag !
    // Cela permet d'allouer un vrai VkDescriptorSet pour l'émulation plus tard.
    if (GetDeviceState(device, &devState) && !devState.push_descriptor_native) {
        if (modifiedCi.flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) {
            modifiedCi.flags &= ~VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        }
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

// --- 2. g_pipelineLayoutSets : les VkDescriptorSetLayout utilises pour
//        creer un VkPipelineLayout donne, indexes par numero de set. ---
static std::mutex g_pipelineLayoutSetsMutex;
static std::unordered_map<VkPipelineLayout, std::vector<VkDescriptorSetLayout>> g_pipelineLayoutSets;

extern "C" __declspec(dllexport) VkResult VKAPI_CALL
vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* pCreateInfo,
                        const VkAllocationCallbacks* pAllocator, VkPipelineLayout* pPipelineLayout) {
    auto real = LazyResolve<PFN_vkCreatePipelineLayout>("vkCreatePipelineLayout");
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult res = real(device, pCreateInfo, pAllocator, pPipelineLayout);
    if (res == VK_SUCCESS) {
        std::vector<VkDescriptorSetLayout> sets(pCreateInfo->pSetLayouts,
            pCreateInfo->pSetLayouts + pCreateInfo->setLayoutCount);

        std::lock_guard<std::mutex> lock(g_pipelineLayoutSetsMutex);
        g_pipelineLayoutSets[*pPipelineLayout] = std::move(sets);
    }
    return res;
}

// --- 3. g_activeMappings : pointeur CPU valide pour un VkBuffer donne, SI
//        sa memoire sous-jacente est actuellement mappee. Derive de deux
//        sources : quelle memoire est mappee a quel pointeur/offset
//        (vkMapMemory/vkUnmapMemory), et quel buffer est lie a quelle
//        memoire/offset (vkBindBufferMemory[2]). Recalcule a chaque
//        changement de l'un ou l'autre cote. ---
struct MemoryMapState { void* ptr; VkDeviceSize mappedOffset; };
static std::mutex g_memoryMapMutex;
static std::unordered_map<VkDeviceMemory, MemoryMapState> g_memoryMapState;

struct BufferBinding { VkDeviceMemory memory; VkDeviceSize offset; };
static std::mutex g_bufferBindingMutex;
static std::unordered_map<VkBuffer, BufferBinding> g_bufferBinding;
static std::unordered_multimap<VkDeviceMemory, VkBuffer> g_memoryToBuffers;

static std::mutex g_activeMappingsMutex;
static std::unordered_map<VkBuffer, void*> g_activeMappings;

// Recalcule g_activeMappings pour tous les buffers lies a "memory", a
// appeler apres tout vkMapMemory/vkUnmapMemory/vkBindBufferMemory
// touchant cette memoire.
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
            // NOTE: suppose que mappedOffset <= bindIt->second.offset, ce qui
            // est garanti si le jeu mappe toute la memoire (offset 0, VK_WHOLE_SIZE)
            // comme l'exige VK_EXT_descriptor_buffer -- cas le plus courant.
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

// vkMapMemory/vkUnmapMemory (legacy) : jusque-la pur forward .def, donc
// g_activeMappings n'etait alimente par rien du tout. On les intercepte
// desormais nous-memes. IMPORTANT: vkMapMemory2/vkUnmapMemory2 (plus haut
// dans ce fichier) appelaient soit le vrai driver, soit ce legacy en
// fallback, mais SANS jamais passer par ce tracking -- corrige juste apres.
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
        // Appel direct de NOTRE vkMapMemory (pas via proc-addr) pour que le
        // tracking g_activeMappings soit alimente dans tous les cas.
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
    // Appel direct de NOTRE vkUnmapMemory pour la meme raison que ci-dessus.
    vkUnmapMemory(device, pMemoryUnmapInfo->memory);
    return VK_SUCCESS;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdSetRenderingAttachmentLocations(VkCommandBuffer commandBuffer, const VkRenderingAttachmentLocationInfo* pInfo) {
    // VK_KHR_dynamic_rendering_local_read (core 1.4) : remappe dynamiquement quelle
    // sortie du fragment shader (location N) ecrit dans quel attachment de couleur du
    // VkRenderingInfo courant. Il n'existe PAS d'equivalent core 1.2 -- le render pass
    // "reel" que ce wrapper genere via GetOrCreateRenderPass() a un mapping fixe
    // location == index d'attachment, defini a la creation du pipeline/render pass.
    using PFN = PFN_vkCmdSetRenderingAttachmentLocations;
    if (PFN real = LazyResolve<PFN>("vkCmdSetRenderingAttachmentLocations")) {
        real(commandBuffer, pInfo);
        return;
    }
    // Repli honnete : si l'appelant demande un mapping non-identite (pLocations[i] != i
    // pour un i quelconque), on ne peut PAS l'honorer avec ce wrapper -- le silence total
    // masquerait un rendu incorrect. On logue une seule fois un avertissement explicite.
    // Si le mapping demande est deja l'identite, ne rien faire est correct et sans danger.
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
        LogStubCall("vkCmdSetRenderingAttachmentLocations (mapping non-identite demande, "
                     "NON honore -- pilote sans VK_KHR_dynamic_rendering_local_read/1.4 ; "
                     "rendu potentiellement incorrect si le jeu depend de ce remapping)");
    }
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdSetRenderingInputAttachmentIndices(VkCommandBuffer commandBuffer, const VkRenderingInputAttachmentIndexInfo* pInfo) {
    // Meme famille que ci-dessus (dynamic_rendering_local_read) : remappe les indices
    // d'input attachment pour la lecture "local read" depuis le fragment shader.
    // Pas d'equivalent 1.2 -- meme strategie : identite = silencieux, sinon avertir.
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
        LogStubCall("vkCmdSetRenderingInputAttachmentIndices (mapping non-identite demande, "
                     "NON honore -- pilote sans VK_KHR_dynamic_rendering_local_read/1.4)");
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

    // Repli reel (comme pour vkGetDeviceImageMemoryRequirements/vkGetDeviceBufferMemoryRequirements
    // juste au-dessus) : cree une image temporaire jetable pour interroger le driver via
    // vkGetImageSparseMemoryRequirements2 (core 1.1), puis la detruit immediatement.
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

    LogStubCall("vkGetDeviceImageSparseMemoryRequirements (repli via creation temporaire d'image impossible)");
    if (pSparseMemoryRequirementCount) *pSparseMemoryRequirementCount = 0;
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetRenderingAreaGranularity(VkDevice device, const VkRenderingAreaInfo* pRenderingAreaInfo, VkExtent2D* pGranularity) {
    using PFN = PFN_vkGetRenderingAreaGranularity;
    if (PFN real = LazyResolve<PFN>("vkGetRenderingAreaGranularity")) {
        real(device, pRenderingAreaInfo, pGranularity);
        return;
    }

    // Pas de repli fiable : VkRenderingAreaInfo ne fournit que des formats
    // d'attachment (pas de VkImageView reelles), donc impossible de retrouver ou
    // reconstruire de facon fiable le VkRenderPass equivalent deja cree par
    // GetOrCreateRenderPass() pour interroger vkGetRenderAreaGranularity (1.0) dessus.
    LogStubCall("vkGetRenderingAreaGranularity (repli conservateur : granularite 1x1)");
    // Repli conservateur sur : granularite 1x1 = toujours un multiple valide de la
    // granularite reelle du driver (jamais incorrect, seulement potentiellement
    // sous-optimal pour du tiled rendering -- aucun risque de corruption).
    if (pGranularity) { pGranularity->width = 1; pGranularity->height = 1; }
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

// =========================================================================
// CORRECTIF HANG AMD (traduction sync2 -> legacy) :
//
// 1) Un simple cast (VkPipelineStageFlags)/(VkAccessFlags) sur un masque
//    VkPipelineStageFlags2/VkAccessFlags2 (64 bits) tronque SILENCIEUSEMENT
//    les bits > 31. Or les variantes granulaires propres a sync2 (COPY/
//    BLIT/RESOLVE/CLEAR pour les stages, SHADER_SAMPLED_READ/STORAGE_READ/
//    STORAGE_WRITE pour les acces) vivent toutes au-dela du bit 31 et n'ont
//    PAS d'equivalent bit-a-bit dans l'ancien enum -- elles remplacent les
//    anciens bits grossiers (TRANSFER_BIT, SHADER_READ/WRITE_BIT). Une
//    barriere qui ne specifie QUE ces bits-la (typique d'une passe de blur
//    qui blit/copy/resolve une image) se retrouvait tronquee a 0.
//
// 2) Le fallback historique quand le masque tombe a 0 utilisait TOP_OF_PIPE
//    comme source et BOTTOM_OF_PIPE comme destination -- exactement
//    l'inverse de ce que dit la spec Vulkan (notes de depreciation de ces
//    bits) : TOP_OF_PIPE en source et BOTTOM_OF_PIPE en destination sont
//    tous deux equivalents a "NONE" (aucune synchronisation reelle), alors
//    que BOTTOM_OF_PIPE en source et TOP_OF_PIPE en destination equivalent
//    a ALL_COMMANDS (le choix sur, qui bloque vraiment). Le "filet de
//    securite" etait donc en realite un no-op total de synchronisation --
//    exactement le genre de race qui peut faire planter/geler une queue
//    AMD en interne, d'ou le semaphore jamais signale et le timeout de 5s.
// =========================================================================
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
        // Meme principe que vkQueueSubmit2 : une barriere silencieusement
        // ignorée peut laisser une image/un buffer dans un layout ou un
        // etat de synchronisation incoherent pour la suite du pipeline
        // (typiquement une passe de blur qui lit une image jamais
        // correctement transitionnee) -- ce qui peut se manifester plus
        // loin comme un hang au lieu d'un crash immediat. On tente un
        // forward direct au vrai driver en dernier recours plutot que de
        // simplement renoncer.
        fprintf(stderr, "[vk-emu] ATTENTION vkCmdPipelineBarrier2: command buffer non trace "
                        "(vkAllocateCommandBuffers manque ce cmd ?) -- tentative de forward "
                        "direct au driver reel pour eviter un hang.\n");
        auto rawReal = LazyResolve<PFN_vkCmdPipelineBarrier2>("vkCmdPipelineBarrier2");
        if (rawReal) { rawReal(commandBuffer, pDependencyInfo); return; }
        LogStubCall("vkCmdPipelineBarrier2 (device inconnu, aucun repli possible)");
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

    // CORRECTIF (voir commentaire au-dessus de LegacyStageMaskFromSync2) :
    // TOP_OF_PIPE en source et BOTTOM_OF_PIPE en destination equivalent tous
    // deux a "aucune synchronisation" -- c'etait l'inverse du filet de
    // securite voulu. BOTTOM_OF_PIPE en source et TOP_OF_PIPE en destination
    // sont les equivalents surs de ALL_COMMANDS.
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

    // ===================================================================
    // CORRECTIF HANG/TIMEOUT : l'ancienne version faisait juste
    // "return VK_ERROR_UNKNOWN" ici si le tracking queue->device echouait
    // -- ce qui signifie que les command buffers soumis n'etaient JAMAIS
    // executes par le GPU. Tout semaphore/fence attendu par ailleurs
    // (typiquement la passe suivante, ex: "GUI before blur") attendait
    // alors indefiniment un signal qui n'arriverait jamais -> exactement
    // le timeout de 5s remonte par Minecraft/VulkanMod. On abandonne
    // desormais SEULEMENT en tout dernier recours, et on essaie d'abord
    // deux filets de securite avant de renoncer.
    // ===================================================================
    if (device == VK_NULL_HANDLE && submitCount > 0) {
        // Filet 1 : retrouver le device via le premier VkCommandBuffer soumis
        // (trace de facon quasi universelle des vkAllocateCommandBuffers,
        // bien plus fiable que le tracking de VkQueue qui depend de si le
        // jeu est passe par NOTRE vkGetDeviceQueue/2).
        for (uint32_t i = 0; i < submitCount && device == VK_NULL_HANDLE; i++) {
            for (uint32_t j = 0; j < pSubmits[i].commandBufferInfoCount; j++) {
                VkDevice d = GetDeviceForCommandBuffer(pSubmits[i].pCommandBufferInfos[j].commandBuffer);
                if (d != VK_NULL_HANDLE) { device = d; break; }
            }
        }
        if (device != VK_NULL_HANDLE) {
            fprintf(stderr, "[vk-emu] vkQueueSubmit2: VkQueue non trace, device retrouve via "
                            "un command buffer soumis -- tracking de la queue rattrape.\n");
            TrackQueueDevice(queue, device);
        }
    }

    DeviceState state;
    bool haveState = (device != VK_NULL_HANDLE) && GetDeviceState(device, &state);

    if (!haveState) {
        // Filet 2 (dernier recours) : on ne connait ni la queue ni le device,
        // mais on NE DOIT PAS abandonner la soumission -- ca fige le jeu.
        // On tente un appel direct au vrai vkQueueSubmit2 du driver reel s'il
        // existe (fonctionne sur un GPU qui a nativement synchronization2,
        // independamment de notre tracking), en dernier recours seulement.
        fprintf(stderr, "[vk-emu] ATTENTION vkQueueSubmit2: aucun device retrouve (ni via la "
                        "queue, ni via les command buffers soumis) -- tentative de forward direct "
                        "au driver reel pour eviter un blocage. A investiguer : pourquoi cette "
                        "VkQueue/ces VkCommandBuffer echappent au tracking.\n");
        auto rawReal = LazyResolve<PFN_vkQueueSubmit2>("vkQueueSubmit2");
        if (rawReal) return rawReal(queue, submitCount, pSubmits, fence);

        auto rawLegacy = LazyResolve<PFN_vkQueueSubmit>("vkQueueSubmit");
        if (!rawLegacy) {
            fprintf(stderr, "[vk-emu] ERREUR FATALE vkQueueSubmit2: aucun repli possible, "
                            "la soumission est perdue -- attendez-vous a un freeze/timeout.\n");
            return VK_ERROR_UNKNOWN;
        }
        // Traduction minimale sans acces a DeviceState (pas de VkTimelineSemaphoreSubmitInfo
        // ici par simplicite -- ce chemin ne devrait de toute facon presque jamais s'emprunter).
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
            // CORRECTIF (meme bug que LegacyStageMaskFromSync2 pour les barrieres) :
            // un mask sync2 compose uniquement de bits granulaires (COPY/BLIT/
            // RESOLVE/CLEAR, > bit 31) n'est pas egal a 0 mais se tronque a 0
            // via un cast direct -- ce qui donne un VkSubmitInfo avec
            // pWaitDstStageMask=0 pour ce semaphore (UB spec) et provoque
            // exactement le hang/timeout observe sur AMD.
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

// Groupe restant de synchronization2
extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdWriteTimestamp2(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage, VkQueryPool queryPool, uint32_t query) {
    VkDevice device = GetDeviceForCommandBuffer(commandBuffer);
    DeviceState state;
    if (!GetDeviceState(device, &state)) { LogStubCall("vkCmdWriteTimestamp2 (device inconnu)"); return; }

    if (state.sync2_native) { state.fn_CmdWriteTimestamp2(commandBuffer, stage, queryPool, query); return; }
    if (state.fn_CmdWriteTimestamp) state.fn_CmdWriteTimestamp(commandBuffer, (VkPipelineStageFlagBits)stage, queryPool, query);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdSetEvent2(VkCommandBuffer commandBuffer, VkEvent event, const VkDependencyInfo* pDependencyInfo) {
    VkDevice device = GetDeviceForCommandBuffer(commandBuffer);
    DeviceState state;
    if (!GetDeviceState(device, &state)) { LogStubCall("vkCmdSetEvent2 (device inconnu)"); return; }

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
    if (!GetDeviceState(device, &state)) { LogStubCall("vkCmdResetEvent2 (device inconnu)"); return; }

    if (state.sync2_native) { state.fn_CmdResetEvent2(commandBuffer, event, stageMask); return; }
    if (state.fn_CmdResetEvent) state.fn_CmdResetEvent(commandBuffer, event, (VkPipelineStageFlagBits)stageMask);
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdWaitEvents2(VkCommandBuffer commandBuffer, uint32_t eventCount, const VkEvent* pEvents, const VkDependencyInfo* pDependencyInfos) {
    VkDevice device = GetDeviceForCommandBuffer(commandBuffer);
    DeviceState state;
    if (!GetDeviceState(device, &state)) { LogStubCall("vkCmdWaitEvents2 (device inconnu)"); return; }

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
    // Meme correctif que vkCmdPipelineBarrier2 (voir commentaire plus haut) :
    // BOTTOM_OF_PIPE en source / TOP_OF_PIPE en destination = les vrais
    // equivalents surs de ALL_COMMANDS, pas l'inverse.
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

// =========================================================================
// CORRECTIF HANG AMD (push descriptor AVEC template) :
// Contrairement a vkCmdPushDescriptorSetKHR (repli logiciel deja present
// plus bas, via un vrai VkDescriptorSet alloue/mis a jour/lie), la variante
// "WithTemplate" n'avait AUCUNE emulation logicielle : si le driver reel ne
// supporte ni le nouveau core (2/2KHR) ni VK_KHR_push_descriptor (ce qui est
// le cas ici -- ces extensions sont TOUJOURS retirees de la creation reelle
// du device, voir vkCreateDevice), l'appel finissait en no-op SILENCIEUX
// (LogStubCall "no legacy fallback"). Consequence : les descripteurs pousses
// par template (bindings de textures/materiaux, typiquement TRES frequent)
// n'etaient jamais lies -> le draw suivant lit des descripteurs non-lies ->
// RADV (strict) peut faire fauter/geler la queue en interne -> timeout de 5s
// sur le semaphore attendu par CommandEncoder.submit. Nvidia tolere ce genre
// d'acces invalide, d'ou l'absence de crash cote Nvidia.
//
// On implemente donc un vrai repli logiciel, sur le meme modele que
// vkCmdPushDescriptorSetKHR plus bas : on alloue un vrai VkDescriptorSet
// depuis le pool interne d'emulation, on le met a jour via
// vkUpdateDescriptorSetWithTemplate (core 1.1, toujours disponible, pas
// besoin de VK_KHR_push_descriptor pour cette fonction-la), puis on le lie
// normalement. Le pipelineBindPoint necessaire pour le bind est capture a
// la creation du template (vkCreateDescriptorUpdateTemplate ci-dessous).
// =========================================================================
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
    if (device == VK_NULL_HANDLE) { LogStubCall("PushDescriptorSetWithTemplate (device inconnu)"); return; }

    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(g_pipelineLayoutSetsMutex);
        auto it = g_pipelineLayoutSets.find(layout);
        if (it != g_pipelineLayoutSets.end() && set < it->second.size())
            setLayout = it->second[set];
    }
    if (setLayout == VK_NULL_HANDLE) {
        LogStubCall("PushDescriptorSetWithTemplate (layout de set introuvable)");
        return;
    }

    VkDescriptorPool pool = GetOrCreateEmuDescPool(device);
    if (pool == VK_NULL_HANDLE) return;

    auto real_alloc      = LazyResolve<PFN_vkAllocateDescriptorSets>("vkAllocateDescriptorSets");
    auto real_updateTpl  = LazyResolve<PFN_vkUpdateDescriptorSetWithTemplate>("vkUpdateDescriptorSetWithTemplate");
    auto real_bind       = LazyResolve<PFN_vkCmdBindDescriptorSets>("vkCmdBindDescriptorSets");
    if (!real_alloc || !real_updateTpl || !real_bind) {
        LogStubCall("PushDescriptorSetWithTemplate (fonctions core introuvables)");
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
        LogStubCall("PushDescriptorSetWithTemplate (echec allocation descriptor set)");
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

// CORRECTIF : c'est CETTE fonction (nom KHR direct, ancien style d'API) que
// l'app va chercher via vkGetDeviceProcAddr -- elle etait absente de la
// table de dispatch, donc l'app recevait nullptr silencieusement
// ("[vk-emu] FONCTION MANQUANTE"), sans jamais passer par l'emulation
// ci-dessus ni par les variantes "2"/"2KHR". D'ou le hang qui persistait
// malgre l'ajout de l'emulation sur les mauvaises fonctions.
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

    // Repli natif si VK_KHR_push_descriptor + VK_KHR_descriptor_update_template
    // sont vraiment actives sur le device reel (cas rare avec ce wrapper).
    auto legacy = LazyResolve<PFN_vkCmdPushDescriptorSetWithTemplateKHR>("vkCmdPushDescriptorSetWithTemplateKHR");
    if (legacy && pInfo) {
        legacy(commandBuffer, pInfo->descriptorUpdateTemplate, pInfo->layout, pInfo->set, pInfo->pData);
        return;
    }

    // Repli logiciel complet (cas AMD ici : ni core 1.4, ni KHR_push_descriptor).
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
        LogStubCall("vkCmdPushDescriptorSetKHR (device inconnu)");
        return;
    }

    // 1. Chemin natif (si le vrai driver supporte push_descriptor)
    if (state.push_descriptor_native && state.fn_CmdPushDescriptorSetKHR) {
        state.fn_CmdPushDescriptorSetKHR(commandBuffer, pipelineBindPoint, layout, set, descriptorWriteCount, pDescriptorWrites);
        return;
    }

    // 2. Repli logiciel : on alloue/met à jour un vrai VkDescriptorSet
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(g_pipelineLayoutSetsMutex);
        auto it = g_pipelineLayoutSets.find(layout);
        if (it != g_pipelineLayoutSets.end() && set < it->second.size()) {
            setLayout = it->second[set];
        }
    }
    if (setLayout == VK_NULL_HANDLE) {
        LogStubCall("vkCmdPushDescriptorSetKHR (layout de set introuvable)");
        return;
    }

    // Hash les writes pour vérifier le cache g_descBufSetCache
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
        
        // On appelle NOTRE vkCmdPushDescriptorSetKHR, qui gère le repli logiciel
        vkCmdPushDescriptorSetKHR(commandBuffer, bindPoint, pPushDescriptorSetInfo->layout, pPushDescriptorSetInfo->set,
                                  pPushDescriptorSetInfo->descriptorWriteCount, pPushDescriptorSetInfo->pDescriptorWrites);
    } else {
        LogStubCall("vkCmdPushDescriptorSet2KHR (pPushDescriptorSetInfo null)");
    }
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

// =====================================================================
// --- VK_EXT_descriptor_buffer : implementation complete ---
// =====================================================================
// Voir le design detaille en tete de fichier (structures DescSlot,
// BufferAddressInfo, EmuDescSetKey, etc.) Resume : on intercepte
// vkGetBufferDeviceAddress pour connaitre l'adresse GPU <-> VkBuffer de
// chaque buffer, on ecrit notre propre representation logique dans
// vkGetDescriptorEXT (au lieu du format binaire prive du driver), et on
// reconstruit un vrai VkDescriptorSet au moment du bind reel.

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
        // On a besoin de la taille du buffer pour la recherche par intervalle
        // dans ResolveBufferFromAddress -- on la retrouve via un round-trip
        // vkGetBufferMemoryRequirements2 (donne .size arrondie, suffisant : on
        // ne s'en sert que pour delimiter une plage de recherche, jamais pour
        // une allocation reelle).
        VkDeviceSize size = 0;
        auto real_req = LazyResolve<PFN_vkGetBufferMemoryRequirements2>("vkGetBufferMemoryRequirements2");
        if (real_req) {
            VkBufferMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2};
            info.buffer = pInfo->buffer;
            VkMemoryRequirements2 req{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
            real_req(device, &info, &req);
            size = req.memoryRequirements.size;
        }
        if (size == 0) size = 1; // repli conservateur : evite une plage vide
        RegisterBufferAddress(pInfo->buffer, addr, size);
    }
    return addr;
}

extern "C" __declspec(dllexport) VkDeviceAddress VKAPI_CALL
vkGetBufferDeviceAddressKHR(VkDevice device, const VkBufferDeviceAddressInfo* pInfo) {
    // Meme logique -- redirige simplement vers l'implementation core ci-dessus
    // pour garder l'enregistrement d'adresse a un seul endroit.
    return vkGetBufferDeviceAddress(device, pInfo);
}

// vkDestroyBuffer est deja wrappe plus haut dans ce fichier pour d'autres
// besoins ? Non : ce wrapper ne le fait pas actuellement pour les buffers
// (seulement les images). On ajoute donc ici le nettoyage de la table
// d'adresses -- mais SANS toucher au comportement existant : on ne wrappe
// vkDestroyBuffer qu'une seule fois dans le fichier (voir plus haut, pres de
// vkDestroyImage) pour eviter toute redefinition. Cf. la fonction
// vkDestroyBuffer ajoutee juste apres vkDestroyImage plus haut dans ce fichier.

// --- vkGetDescriptorEXT : ecrit notre representation logique (DescSlot) a
// l'adresse pDescriptor fournie, qui est de la memoire mappee appartenant a
// l'appelant (portion d'un descriptor buffer prealablement vkMapMemory'e). ---
extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetDescriptorEXT(VkDevice device, const VkDescriptorGetInfoEXT* pDescriptorInfo,
                    size_t dataSize, void* pDescriptor) {
    if (!pDescriptorInfo || !pDescriptor) return;

    // Si jamais le vrai driver expose nativement descriptor_buffer (peu probable
    // sur un 1.2.175, mais possible sur d'autres IHV/versions), on delegue
    // directement -- notre emulation ne doit jamais se melanger avec un format
    // binaire reel ecrit par le driver.
    auto real = LazyResolve<PFN_vkGetDescriptorEXT>("vkGetDescriptorEXT");
    if (real) { real(device, pDescriptorInfo, dataSize, pDescriptor); return; }

    if (dataSize < sizeof(DescSlot)) {
        // Le jeu a interroge vkGetDescriptorSetLayoutSizeEXT/BindingOffsetEXT
        // (ci-dessous) pour dimensionner son buffer, donc dataSize devrait
        // toujours etre >= sizeof(DescSlot) avec notre emulation. Si ce n'est
        // pas le cas, on ne peut pas ecrire notre slot sans deborder -- log et
        // on abandonne proprement plutot que de corrompre la memoire voisine.
        LogStubCall("vkGetDescriptorEXT (dataSize insuffisant pour l'emulation -- verifier "
                     "vkGetDescriptorSetLayoutSizeEXT/BindingOffsetEXT cote appelant)");
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
            // Types non geres (ex: acceleration structures / raytracing) --
            // hors-scope de cette emulation (voir aussi VK_EXT_mesh_shader :
            // meme logique, pas de repli logiciel pour du raytracing materiel).
            LogStubCall("vkGetDescriptorEXT (type de descripteur non emule)");
            break;
    }
    std::memcpy(pDescriptor, &slot, sizeof(DescSlot));
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkGetDescriptorSetLayoutSizeEXT(VkDevice device, VkDescriptorSetLayout layout, VkDeviceSize* pLayoutSizeInBytes) {
    auto real = LazyResolve<PFN_vkGetDescriptorSetLayoutSizeEXT>("vkGetDescriptorSetLayoutSizeEXT");
    if (real) { real(device, layout, pLayoutSizeInBytes); return; }

    // Emulation : notre "taille de layout" n'a pas besoin de correspondre au
    // format reel du driver -- elle doit juste etre coherente avec ce que
    // vkGetDescriptorSetLayoutBindingOffsetEXT (ci-dessous) rapporte, et assez
    // grande pour contenir sizeof(DescSlot) par binding. On ne connait pas le
    // nombre de bindings ici (l'API ne le donne pas), donc on renvoie une
    // borne large et fixe par binding (voir BindingOffsetEXT pour le detail du
    // calcul reel utilise cote offsets).
    if (pLayoutSizeInBytes) *pLayoutSizeInBytes = 0; // rempli dynamiquement, voir note ci-dessous
    LogStubCall("vkGetDescriptorSetLayoutSizeEXT (emulation -- taille calculee de facon "
                "conservatrice, voir vkGetDescriptorSetLayoutBindingOffsetEXT)");
    // NOTE IMPORTANTE : sans acces direct aux VkDescriptorSetLayoutBinding du
    // layout (l'API Vulkan ne les expose pas apres coup), l'emulation exacte
    // de la taille necessiterait de garder nous-memes une copie du
    // VkDescriptorSetLayoutCreateInfo a la creation. C'est fait ci-dessous
    // (voir vkCreateDescriptorSetLayout wrappe plus loin) : on relit cette
    // copie ici pour calculer la vraie taille.
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

    // Emulation : offset = index du binding (dans l'ordre de creation) *
    // sizeof(DescSlot). Necessite la copie du layout enregistree a la
    // creation (voir vkCreateDescriptorSetLayout plus loin dans ce fichier).
    std::lock_guard<std::mutex> lock(g_descSetLayoutInfoMutex);
    auto it = g_descSetLayoutInfo.find(layout);
    if (it == g_descSetLayoutInfo.end() || !pOffset) {
        LogStubCall("vkGetDescriptorSetLayoutBindingOffsetEXT (layout inconnu -- "
                     "cree avant l'activation de l'emulation descriptor_buffer ?)");
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

// --- vkCmdBindDescriptorBuffersEXT : enregistre quels descriptor buffers
// (identifies par adresse GPU) sont disponibles pour ce command buffer, sous
// les indices 0..bufferCount-1 utilises ensuite par SetDescriptorBufferOffsets. ---
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

// --- vkCmdSetDescriptorBufferOffsetsEXT : le coeur de l'emulation. Pour
// chaque set demande, on relit les DescSlot depuis la memoire mappee du
// descriptor buffer correspondant, on reconstruit/reutilise un vrai
// VkDescriptorSet via le cache, et on le bind avec l'API classique. ---
extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdSetDescriptorBufferOffsetsEXT(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint,
                                    VkPipelineLayout layout, uint32_t firstSet, uint32_t setCount,
                                    const uint32_t* pBufferIndices, const VkDeviceSize* pOffsets) {
    auto real = LazyResolve<PFN_vkCmdSetDescriptorBufferOffsetsEXT>("vkCmdSetDescriptorBufferOffsetsEXT");
    if (real) { real(commandBuffer, pipelineBindPoint, layout, firstSet, setCount, pBufferIndices, pOffsets); return; }

    VkDevice device = GetDeviceForCommandBuffer(commandBuffer);
    if (device == VK_NULL_HANDLE) {
        LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (device inconnu pour ce command buffer)");
        return;
    }

    std::vector<BoundDescBuffer> boundCopy;
    {
        std::lock_guard<std::mutex> lock(g_cmdDescBufMutex);
        auto it = g_cmdBoundDescBuffers.find(commandBuffer);
        if (it == g_cmdBoundDescBuffers.end()) {
            LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (aucun descriptor buffer bind -- "
                         "vkCmdBindDescriptorBuffersEXT manquant avant cet appel)");
            return;
        }
        boundCopy = it->second; // copie pour eviter de garder le lock pendant les allocations
    }

    auto real_getLayoutSupport = LazyResolve<PFN_vkCreateDescriptorSetLayout>("vkCreateDescriptorSetLayout");
    (void)real_getLayoutSupport; // reserve pour extension future si besoin de validation

    for (uint32_t s = 0; s < setCount; s++) {
        uint32_t bufIdx = pBufferIndices[s];
        if (bufIdx >= boundCopy.size()) {
            LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (index de descriptor buffer hors bornes)");
            continue;
        }
        VkDeviceAddress bufBase = boundCopy[bufIdx].address;
        VkDeviceAddress slotsAddr = bufBase + pOffsets[s];

        VkDeviceSize relOffset = 0;
        VkBuffer realBuffer = ResolveBufferFromAddress(slotsAddr, &relOffset);
        if (realBuffer == VK_NULL_HANDLE) {
            LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (impossible de resoudre l'adresse "
                        "du descriptor buffer -- vkGetBufferDeviceAddress n'est peut-etre pas "
                        "passe par ce wrapper pour ce buffer)");
            continue;
        }

        // Retrouve le layout attendu pour ce set (necessaire pour savoir combien
        // de bindings lire et pour creer/retrouver le VkDescriptorSet). On le
        // recupere via la table enregistree a vkCreateDescriptorSetLayout, en
        // le retrouvant par correspondance de VkPipelineLayout + index de set.
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        {
            std::lock_guard<std::mutex> lock(g_pipelineLayoutSetsMutex);
            auto it = g_pipelineLayoutSets.find(layout);
            if (it != g_pipelineLayoutSets.end() && (firstSet + s) < it->second.size())
                setLayout = it->second[firstSet + s];
        }
        if (setLayout == VK_NULL_HANDLE) {
            LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (layout de set introuvable -- "
                        "vkCreatePipelineLayout non trace pour ce VkPipelineLayout)");
            continue;
        }

                std::vector<DescSetLayoutBindingInfo> bindingInfos;
        {
            std::lock_guard<std::mutex> lock(g_descSetLayoutInfoMutex);
            auto it = g_descSetLayoutInfo.find(setLayout);
            if (it == g_descSetLayoutInfo.end()) {
                LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (VkDescriptorSetLayout non trace)");
                continue;
            }
            bindingInfos = it->second.bindings;
        }

        // Lit les DescSlot directement depuis la memoire mappee du descriptor
        // buffer (memoire hote de l'appelant -- ResolveBufferFromAddress nous
        // a donne le VkBuffer reel, mais on a besoin d'un pointeur CPU valide :
        // on le retrouve via la table des mappings actifs, cf. vkMapMemory
        // wrappe plus haut dans ce fichier qui alimente g_activeMappings).
        void* mappedBase = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_activeMappingsMutex);
            auto it = g_activeMappings.find(realBuffer);
            if (it != g_activeMappings.end()) mappedBase = it->second;
        }
        if (!mappedBase) {
            LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (descriptor buffer non mappe "
                        "cote CPU -- le jeu doit garder ce buffer mappe en permanence, "
                        "comme l'exige VK_EXT_descriptor_buffer)");
            continue;
        }

        // Calcule un hash de contenu pour le cache -- evite de recreer un
        // VkDescriptorSet identique a chaque frame si le contenu n'a pas change.
        const uint8_t* slotsBase = (const uint8_t*)mappedBase + relOffset;
        size_t contentHash = 0;
        std::vector<DescSlot> slots(bindingInfos.size());
        for (size_t i = 0; i < bindingInfos.size(); i++) {
            std::memcpy(&slots[i], slotsBase + i * sizeof(DescSlot), sizeof(DescSlot));
            // Hash simple par combinaison (FNV-like) -- suffisant pour un cache,
            // pas besoin de cryptographique ici.
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
            // Pas en cache : alloue un nouveau VkDescriptorSet et ecrit son contenu.
            VkDescriptorPool pool = GetOrCreateEmuDescPool(device);
            if (pool == VK_NULL_HANDLE) {
                LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (echec creation pool interne)");
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
                // Pool plein (peu probable vu le dimensionnement, mais possible en
                // usage tres intensif) : on force la creation d'un nouveau pool au
                // prochain appel en marquant celui-ci comme sature.
                std::lock_guard<std::mutex> lock(g_descBufPoolMutex);
                auto& pools = g_descBufPools[device];
                if (!pools.empty()) pools.back().allocatedCount = EmuDescBufferPool::kMaxSetsPerPool;
                LogStubCall("vkCmdSetDescriptorBufferOffsetsEXT (pool interne sature, "
                            "nouveau pool alloue au prochain appel)");
                continue;
            }
            {
                std::lock_guard<std::mutex> lock(g_descBufPoolMutex);
                auto& pools = g_descBufPools[device];
                if (!pools.empty()) pools.back().allocatedCount++;
            }

            // Ecrit chaque binding via vkUpdateDescriptorSets, a partir des DescSlot lus.
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
                        // Texel buffers necessiteraient un VkBufferView, non
                        // reconstructible ici sans etat supplementaire -- non
                        // couvert par cette premiere passe d'emulation.
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
    // Samplers immuables embarques directement dans le layout -- deja geres
    // par le VkDescriptorSetLayout classique cote emulation (les samplers
    // immuables sont fixes a la creation du layout, ce call est donc un no-op
    // correct dans notre modele : rien a "rebinder" separement).
    LogStubCall("vkCmdBindDescriptorBufferEmbeddedSamplersEXT (no-op : samplers immuables "
                "deja geres via le VkDescriptorSetLayout classique)");
}


// Les mesh/task shaders remplacent l'etage vertex/geometry/tessellation du pipeline
// graphique et s'executent directement sur le materiel du GPU. Il n'existe aucun
// chemin logiciel pour "emuler" ca en 1.2 -- soit le driver/GPU le supporte
// nativement (VK_EXT_mesh_shader), soit c'est un VK_ERROR_FEATURE_NOT_PRESENT franc.
// Voir aussi : VK_EXT_MESH_SHADER_EXTENSION_NAME n'est PAS dans g_fakedDeviceExtensions
// -- on ne l'annonce au jeu que si le vrai driver l'expose.
extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdDrawMeshTasksEXT(VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    auto real = LazyResolve<PFN_vkCmdDrawMeshTasksEXT>("vkCmdDrawMeshTasksEXT");
    if (real) { real(commandBuffer, groupCountX, groupCountY, groupCountZ); return; }
    LogStubCall("vkCmdDrawMeshTasksEXT (VK_EXT_mesh_shader absent du pilote/GPU reel -- aucun repli logiciel possible)");
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdDrawMeshTasksIndirectEXT(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                               uint32_t drawCount, uint32_t stride) {
    auto real = LazyResolve<PFN_vkCmdDrawMeshTasksIndirectEXT>("vkCmdDrawMeshTasksIndirectEXT");
    if (real) { real(commandBuffer, buffer, offset, drawCount, stride); return; }
    LogStubCall("vkCmdDrawMeshTasksIndirectEXT (VK_EXT_mesh_shader absent du pilote/GPU reel)");
}

extern "C" __declspec(dllexport) void VKAPI_CALL
vkCmdDrawMeshTasksIndirectCountEXT(VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset,
                                    VkBuffer countBuffer, VkDeviceSize countBufferOffset,
                                    uint32_t maxDrawCount, uint32_t stride) {
    auto real = LazyResolve<PFN_vkCmdDrawMeshTasksIndirectCountEXT>("vkCmdDrawMeshTasksIndirectCountEXT");
    if (real) { real(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride); return; }
    LogStubCall("vkCmdDrawMeshTasksIndirectCountEXT (VK_EXT_mesh_shader absent du pilote/GPU reel)");
}



// Extension nativement disponible en 1.2 (specVersion 2, promue 1.3-optionnelle) :
// simple passthrough, pas d'emulation logicielle possible sans re-creer le pipeline
// (ce que l'extension existe justement pour eviter). Si le pilote reel ne l'expose
// pas, aucun repli sur -- il faudrait alors reconstruire les VkPipeline a la volee.
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
    // Pas de repli logiciel possible : ceci necessiterait de suivre l'etat courant
    // du vertex input par command buffer et de re-selectionner/re-creer un VkPipeline
    // correspondant a chaque appel de draw -- hors-scope d'un simple wrapper d'API.
    LogStubCall("vkCmdSetVertexInputEXT (extension absente du pilote reel -- aucun repli logiciel)");
}

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

    // VK_EXT_mesh_shader (passthrough pur, voir definitions plus haut)
    { "vkCmdDrawMeshTasksEXT",              (PFN_vkVoidFunction)vkCmdDrawMeshTasksEXT },
    { "vkCmdDrawMeshTasksIndirectEXT",      (PFN_vkVoidFunction)vkCmdDrawMeshTasksIndirectEXT },
    { "vkCmdDrawMeshTasksIndirectCountEXT", (PFN_vkVoidFunction)vkCmdDrawMeshTasksIndirectCountEXT },

    // core 1.3
    { "vkCreatePrivateDataSlot", (PFN_vkVoidFunction)vkCreatePrivateDataSlot },
    { "vkDestroyPrivateDataSlot", (PFN_vkVoidFunction)vkDestroyPrivateDataSlot },
    { "vkSetPrivateData", (PFN_vkVoidFunction)vkSetPrivateData },
    { "vkGetPrivateData", (PFN_vkVoidFunction)vkGetPrivateData },

    // CORRECTIF : necessaires pour que l'infrastructure descriptor_buffer
    // (g_descSetLayoutInfo, g_pipelineLayoutSets, g_activeMappings) soit
    // reellement alimentee -- sans ces 6 entrees, vkGetInstanceProcAddr
    // forwarde ces noms tel quel vers le vrai driver et nos wrappers ne
    // sont jamais appeles (compile, mais descriptor_buffer reste un no-op).
    { "vkCreateDescriptorSetLayout", (PFN_vkVoidFunction)vkCreateDescriptorSetLayout },
    { "vkCreatePipelineLayout",      (PFN_vkVoidFunction)vkCreatePipelineLayout },
    { "vkBindBufferMemory",          (PFN_vkVoidFunction)vkBindBufferMemory },
    { "vkBindBufferMemory2",         (PFN_vkVoidFunction)vkBindBufferMemory2 },
    { "vkMapMemory",                 (PFN_vkVoidFunction)vkMapMemory },
    { "vkUnmapMemory",               (PFN_vkVoidFunction)vkUnmapMemory },

    // AMD support
    { "vkCreateDevice",                       (PFN_vkVoidFunction)vkCreateDevice },
    { "vkGetDeviceQueue",                     (PFN_vkVoidFunction)vkGetDeviceQueue }, // AJOUT
    { "vkGetDeviceQueue2",                    (PFN_vkVoidFunction)vkGetDeviceQueue2 }, // AJOUT
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

    // --- PIÈGE À NULL ---
    // La fonction n'existe ni dans notre code ni dans le driver.
    // On renvoie un stub qui loguera l'appel pour éviter le segfault (pc=0x0)
    std::string msg = "[vk-emu] FONCTION MANQUANTE: " + std::string(pName) + "\n";
    fputs(msg.c_str(), stderr);
    fflush(stderr);
    return (PFN_vkVoidFunction)vkEmuNullStubRet0;
}