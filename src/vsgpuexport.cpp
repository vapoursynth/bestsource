//  Copyright (c) 2026 Fredrik Mellbin
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

#include "vsgpuexport.h"
#include "bsshared.h"

#if BS_GPU_HASH

/* Before any vulkan header, so the platform specific import structures exist. windows.h arrives
   with it and macro-renames things like CreateSemaphore, which is why everything below spells
   Vulkan entry points out with their vk prefix. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "gpuhash.h"
#include "videosource.h"

/* Must match vapoursynth.cpp, which is what configPlugin asks the core for. */
#define VS_USE_API_43
#include <VapourSynth4.h>
#include <VSVulkan4.h>

#include <cstring>
#include <map>
#include <mutex>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}

#include "vulkanshared.h"

namespace {

#define BS_VS_VK_FUNCS(F)                \
    F(vkGetPhysicalDeviceProperties2)    \
    F(vkGetBufferMemoryRequirements)     \
    F(vkCreateBuffer)                    \
    F(vkDestroyBuffer)                   \
    F(vkAllocateMemory)                  \
    F(vkFreeMemory)                      \
    F(vkBindBufferMemory)                \
    F(vkCreateSemaphore)                 \
    F(vkDestroySemaphore)

struct ImportFunctions {
    BS_VS_VK_FUNCS(BS_VK_DECLARE_FUNC)
#ifdef _WIN32
    PFN_vkImportSemaphoreWin32HandleKHR vkImportSemaphoreWin32HandleKHR = nullptr;
    PFN_vkGetMemoryWin32HandlePropertiesKHR vkGetMemoryWin32HandlePropertiesKHR = nullptr;
#else
    PFN_vkImportSemaphoreFdKHR vkImportSemaphoreFdKHR = nullptr;
    PFN_vkGetMemoryFdPropertiesKHR vkGetMemoryFdPropertiesKHR = nullptr;
#endif
};

/* An imported VapourSynth allocation, kept until it falls out of the bounded cache or the filter
   dies. Keyed by the core's memoryId, which is stable for the allocation's lifetime and never
   reused; the handle itself is fresh on every export call and must never be used as a key. */
struct ImportedAllocation {
    VkDeviceMemory Memory = VK_NULL_HANDLE;
    VkBuffer Buffer = VK_NULL_HANDLE;
    /* From the shared use counter, for least recently used eviction. */
    uint64_t LastUse = 0;
};

} // namespace

struct BSVSGpuExport::Impl {
    BestVideoSource *Source = nullptr;
    BSGpuHasher *Hasher = nullptr;
    const VSVULKANAPI *VkAPI = nullptr;
    VSCore *Core = nullptr;
    const VSAPI *vsapi = nullptr;

    AVBufferRef *DeviceRef = nullptr;
    AVVulkanDeviceContext *HWCtx = nullptr;
    VkDevice Device = VK_NULL_HANDLE;
    ImportFunctions VK;
    /* Queried once at creation; the answer cannot change for the device's lifetime. */
    VkPhysicalDeviceMemoryProperties MemProps = {};

    std::string DeviceName;

    /* Ours to signal, the core's to hand to consumers. Created once and released in the
       destructor; planes published on it hold their own references, so frames still in flight
       keep it alive after that. */
    VSGPUTimeline *Timeline = nullptr;
    VkSemaphore ImportedTimeline = VK_NULL_HANDLE;
    uint64_t SignalCounter = 0;

    /* Bounded: an import pins the core's allocation through its OS handle reference, so a cache
       that never let go would keep VRAM alive that the core's own eviction decided to reclaim.
       The bound is generous against the working set -- the core hands out plane regions from a
       few large recycled blocks, so steady state is a handful of ids. */
    static constexpr size_t MaxImports = 16;
    uint64_t ImportUseCounter = 0;
    std::map<uint64_t, ImportedAllocation> Imports;
    std::mutex Mutex;

    ~Impl();
    /* Returns the buffer covering an exported allocation, importing it the first time it is seen.
       Takes ownership of the handle in Exported either way. */
    VkBuffer ImportAllocation(const VSVulkanExportedMemory &Exported);
};

namespace {

[[noreturn]] void ThrowVk(const char *What, VkResult Res) {
    BSThrowVk("GPU export", What, Res);
}

/* NT handles are not consumed by a successful import and have to be closed; POSIX file descriptors
   are consumed on success and must only be closed when the import failed. Getting this backwards
   leaks a handle per frame in one direction and double closes in the other. */
void CloseExportedHandle(intptr_t Handle, bool ImportSucceeded) {
#ifdef _WIN32
    (void)ImportSucceeded;
    if (Handle)
        CloseHandle(reinterpret_cast<HANDLE>(Handle));
#else
    if (!ImportSucceeded && Handle >= 0)
        close(static_cast<int>(Handle));
#endif
}

} // namespace

BSVSGpuExport::Impl::~Impl() {
    if (Device) {
        for (auto &Iter : Imports) {
            if (Iter.second.Buffer)
                VK.vkDestroyBuffer(Device, Iter.second.Buffer, HWCtx->alloc);
            if (Iter.second.Memory)
                VK.vkFreeMemory(Device, Iter.second.Memory, HWCtx->alloc);
        }
        if (ImportedTimeline)
            VK.vkDestroySemaphore(Device, ImportedTimeline, HWCtx->alloc);
    }
    if (Timeline && VkAPI)
        VkAPI->freeGPUTimeline(Timeline);
    av_buffer_unref(&DeviceRef);
}

VkBuffer BSVSGpuExport::Impl::ImportAllocation(const VSVulkanExportedMemory &Exported) {
    auto Existing = Imports.find(Exported.memoryId);
    if (Existing != Imports.end()) {
        Existing->second.LastUse = ++ImportUseCounter;
        CloseExportedHandle(Exported.handle, false);
        return Existing->second.Buffer;
    }

    ImportedAllocation New;
    bool Succeeded = false;

    /* The buffer must declare at creation that it will be bound to memory imported with this
       handle type; binding external memory to a buffer created without it is invalid, and it is
       also what lets the implementation report the external case's own memory requirements. */
    VkExternalMemoryBufferCreateInfo ExtBCI = {};
    ExtBCI.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
    ExtBCI.handleTypes = static_cast<VkExternalMemoryHandleTypeFlags>(Exported.handleType);

    VkBufferCreateInfo BCI = {};
    BCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    BCI.pNext = &ExtBCI;
    BCI.size = Exported.memorySize;
    BCI.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    BCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult Res = VK.vkCreateBuffer(Device, &BCI, HWCtx->alloc, &New.Buffer);
    if (Res != VK_SUCCESS) {
        CloseExportedHandle(Exported.handle, false);
        ThrowVk("vkCreateBuffer", Res);
    }

    VkMemoryRequirements Req = {};
    VK.vkGetBufferMemoryRequirements(Device, New.Buffer, &Req);

    /* The imported allocation is exactly memorySize; a buffer whose requirements pad beyond that
       cannot be bound to it, and finding out here beats an invalid bind. */
    if (Req.size > Exported.memorySize) {
        VK.vkDestroyBuffer(Device, New.Buffer, HWCtx->alloc);
        CloseExportedHandle(Exported.handle, false);
        throw BestSourceException("GPU export: the imported allocation is smaller than the buffer's memory requirements");
    }

    /* Which memory types the handle may legally be imported as, intersected with what the buffer
       accepts. Both devices are the same physical device so the indices line up, but asking is the
       only way to know that rather than assume it. */
    uint32_t AllowedTypes = Req.memoryTypeBits;
#ifdef _WIN32
    VkMemoryWin32HandlePropertiesKHR HandleProps = {};
    HandleProps.sType = VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR;
    if (VK.vkGetMemoryWin32HandlePropertiesKHR &&
        VK.vkGetMemoryWin32HandlePropertiesKHR(Device,
            static_cast<VkExternalMemoryHandleTypeFlagBits>(Exported.handleType),
            reinterpret_cast<HANDLE>(Exported.handle), &HandleProps) == VK_SUCCESS)
        AllowedTypes &= HandleProps.memoryTypeBits;
#else
    VkMemoryFdPropertiesKHR HandleProps = {};
    HandleProps.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
    if (VK.vkGetMemoryFdPropertiesKHR &&
        VK.vkGetMemoryFdPropertiesKHR(Device,
            static_cast<VkExternalMemoryHandleTypeFlagBits>(Exported.handleType),
            static_cast<int>(Exported.handle), &HandleProps) == VK_SUCCESS)
        AllowedTypes &= HandleProps.memoryTypeBits;
#endif

    /* Device local preferred, anything importable as the fallback. */
    uint32_t TypeIndex = BSFindVkMemoryType(MemProps, AllowedTypes, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (TypeIndex == UINT32_MAX)
        TypeIndex = BSFindVkMemoryType(MemProps, AllowedTypes, 0);
    if (TypeIndex == UINT32_MAX) {
        VK.vkDestroyBuffer(Device, New.Buffer, HWCtx->alloc);
        CloseExportedHandle(Exported.handle, false);
        throw BestSourceException("GPU export: no memory type accepts the imported allocation");
    }

#ifdef _WIN32
    VkImportMemoryWin32HandleInfoKHR ImportInfo = {};
    ImportInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
    ImportInfo.handleType = static_cast<VkExternalMemoryHandleTypeFlagBits>(Exported.handleType);
    ImportInfo.handle = reinterpret_cast<HANDLE>(Exported.handle);
#else
    VkImportMemoryFdInfoKHR ImportInfo = {};
    ImportInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
    ImportInfo.handleType = static_cast<VkExternalMemoryHandleTypeFlagBits>(Exported.handleType);
    ImportInfo.fd = static_cast<int>(Exported.handle);
#endif

    VkMemoryAllocateInfo AI = {};
    AI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    AI.pNext = &ImportInfo;
    AI.allocationSize = Exported.memorySize;
    AI.memoryTypeIndex = TypeIndex;
    Res = VK.vkAllocateMemory(Device, &AI, HWCtx->alloc, &New.Memory);
    Succeeded = (Res == VK_SUCCESS);
    CloseExportedHandle(Exported.handle, Succeeded);
    if (!Succeeded) {
        VK.vkDestroyBuffer(Device, New.Buffer, HWCtx->alloc);
        ThrowVk("vkAllocateMemory (import)", Res);
    }

    Res = VK.vkBindBufferMemory(Device, New.Buffer, New.Memory, 0);
    if (Res != VK_SUCCESS) {
        VK.vkFreeMemory(Device, New.Memory, HWCtx->alloc);
        VK.vkDestroyBuffer(Device, New.Buffer, HWCtx->alloc);
        ThrowVk("vkBindBufferMemory", Res);
    }

    /* Least recently used first, freeing the core's allocation to actually die when its own
       eviction let go of it. Safe at any moment: every export dispatch blocks the host until the
       GPU is done, so no imported buffer is referenced by in-flight work between calls. The one
       hazard is the current frame's other planes, imported moments ago and about to be handed to
       the same dispatch -- the recency guard keeps those out of reach, with room to spare against
       the three planes a frame can have. */
    while (Imports.size() >= MaxImports) {
        auto Victim = Imports.end();
        for (auto It = Imports.begin(); It != Imports.end(); ++It) {
            if (It->second.LastUse + 8 > ImportUseCounter)
                continue;
            if (Victim == Imports.end() || It->second.LastUse < Victim->second.LastUse)
                Victim = It;
        }
        if (Victim == Imports.end())
            break;
        VK.vkDestroyBuffer(Device, Victim->second.Buffer, HWCtx->alloc);
        VK.vkFreeMemory(Device, Victim->second.Memory, HWCtx->alloc);
        BSDebugPrint("GPU export: evicted imported allocation " + std::to_string(Victim->first));
        Imports.erase(Victim);
    }

    New.LastUse = ++ImportUseCounter;
    Imports[Exported.memoryId] = New;
    BSDebugPrint("GPU export: imported allocation " + std::to_string(Exported.memoryId) +
        ", " + std::to_string(Imports.size()) + " cached");
    return New.Buffer;
}

BSVSGpuExport::BSVSGpuExport() : P(new Impl) {}
BSVSGpuExport::~BSVSGpuExport() = default;

const std::string &BSVSGpuExport::GetDeviceName() const {
    return P->DeviceName;
}

namespace {

/* Whether the vulkan device FFmpeg creates for Selector sits on the physical device with the
   given UUID. FFmpeg selects by name and adapter names are not unique: with two identical GPUs
   the selector only ever reaches the first, so when VapourSynth sits on the second, every open
   would index on the GPU, fail the identity check in Create, rebuild on the CPU, and reindex
   again -- thrashing the cached index between the two flavors forever. One throwaway device
   creation here turns that into a clean CPU fallback before any source or index exists. */
bool FFmpegDeviceMatchesUUID(const std::string &Selector, const uint8_t *UUID, std::string &Error) {
    AVBufferRef *DevRef = nullptr;
    if (av_hwdevice_ctx_create(&DevRef, AV_HWDEVICE_TYPE_VULKAN, Selector.c_str(), nullptr, 0) < 0) {
        Error = "couldn't create a vulkan decoder device matching '" + Selector + "'";
        return false;
    }
    const AVHWDeviceContext *Ctx = reinterpret_cast<const AVHWDeviceContext *>(DevRef->data);
    const AVVulkanDeviceContext *HWCtx = reinterpret_cast<const AVVulkanDeviceContext *>(Ctx->hwctx);
    auto GetProps2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        HWCtx->get_proc_addr(HWCtx->inst, "vkGetPhysicalDeviceProperties2"));
    /* Absent only on a pre 1.1 instance, which FFmpeg does not create; treated as a pass so the
       equally guarded check in Create stays the final word. */
    bool Match = true;
    if (GetProps2) {
        VkPhysicalDeviceIDProperties IDProps = {};
        IDProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 Props2 = {};
        Props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        Props2.pNext = &IDProps;
        GetProps2(HWCtx->phys_dev, &Props2);
        Match = (memcmp(IDProps.deviceUUID, UUID, VK_UUID_SIZE) == 0);
        if (!Match)
            Error = std::string("the decoder lands on '") + Props2.properties.deviceName +
                "', which is not the device VapourSynth is on; sharing frames needs the same GPU";
    }
    av_buffer_unref(&DevRef);
    return Match;
}

} // namespace

bool BSVSGpuExport::QueryDevice(VSCore *Core, const VSAPI *vsapi, BSVSGpuDeviceInfo &Device, std::string &Error) {
    const VSVULKANAPI *VkAPI = vsapi->getVulkanAPI();

    char Err[512] = {};
    VSVulkanCoreInfo Info = {};
    if (VkAPI->getVulkanCoreInfo(Core, &Info, Err, sizeof(Err))) {
        Error = std::string("couldn't bring up the Vulkan device: ") + Err;
        return false;
    }

    if (!Info.exportHandleType || !Info.semaphoreExportHandleType) {
        Error = "the Vulkan device can't export memory or semaphores, which frame sharing needs";
        return false;
    }

    Device.DeviceName = Info.deviceName;
    static_assert(sizeof(Device.DeviceUUID) == VK_UUID_SIZE);
    memcpy(Device.DeviceUUID, Info.deviceUUID, VK_UUID_SIZE);

    if (!FFmpegDeviceMatchesUUID(Device.DeviceName, Device.DeviceUUID, Error))
        return false;

    return true;
}

std::unique_ptr<BSVSGpuExport> BSVSGpuExport::Create(BestVideoSource *Source, int VariableFormat,
    const BSVSGpuDeviceInfo &Device, VSCore *Core, const VSAPI *vsapi, std::string &Error) {
    std::unique_ptr<BSVSGpuExport> Self(new BSVSGpuExport());
    Impl *P = Self->P.get();

    P->Source = Source;
    P->Core = Core;
    P->vsapi = vsapi;
    P->Hasher = Source->GetGpuHasher();
    P->VkAPI = vsapi->getVulkanAPI();

    if (!P->Hasher || !Source->GetHWDeviceContext()) {
        Error = "the source isn't using vulkan hardware decoding with GPU hashing";
        return nullptr;
    }

    /* Every format the track contains is checked here rather than per frame, because a frame time
       refusal comes after the node exists, where falling back to CPU decoding is no longer an
       option and a script simply fails partway through. The index already lists every format set
       the track holds, so a variable format file is covered as well as a constant one. */
    const auto &FormatSets = Source->GetFormatSets();
    for (size_t i = 0; i < FormatSets.size(); i++) {
        /* A specific selection drops every other format, so only that one has to be exportable. */
        if (VariableFormat >= 0 && static_cast<size_t>(VariableFormat) != i)
            continue;
        const auto &Set = FormatSets[i];
        if (!BSGpuHasher::IsSupportedSwFormat(Set.Format)) {
            Error = std::string("the decoder produces ") +
                (av_get_pix_fmt_name(static_cast<AVPixelFormat>(Set.Format)) ?
                    av_get_pix_fmt_name(static_cast<AVPixelFormat>(Set.Format)) : "an unnamed format") +
                ", and only two plane semi-planar formats can be exported";
            return nullptr;
        }
        /* The export shader writes three planes and nothing else, so anything that is not YUV is
           out. Alpha has no representation in a two plane decode output, so a format claiming it
           would mean the plane count assumption is wrong somewhere upstream. */
        if (Set.VF.ColorFamily != 3 || Set.VF.Alpha) {
            Error = "only three plane YUV output can be exported to the GPU";
            return nullptr;
        }
    }
    /* The device was already probed by QueryDevice, whose result arrives in Device; asking the
       core again would bring up nothing new. */
    char Err[512] = {};
    P->DeviceName = Device.DeviceName;

    P->DeviceRef = av_buffer_ref(Source->GetHWDeviceContext());
    if (!P->DeviceRef) {
        Error = "couldn't reference the decoder's device";
        return nullptr;
    }
    AVHWDeviceContext *Ctx = reinterpret_cast<AVHWDeviceContext *>(P->DeviceRef->data);
    P->HWCtx = reinterpret_cast<AVVulkanDeviceContext *>(Ctx->hwctx);
    P->Device = P->HWCtx->act_dev;

    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        P->HWCtx->get_proc_addr(P->HWCtx->inst, "vkGetDeviceProcAddr"));
    if (!GetDeviceProcAddr) {
        Error = "couldn't load vkGetDeviceProcAddr";
        return nullptr;
    }
#define BS_LOAD(n) P->VK.n = reinterpret_cast<PFN_##n>(GetDeviceProcAddr(P->Device, #n));
    BS_VS_VK_FUNCS(BS_LOAD)
#undef BS_LOAD
    P->VK.vkGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
        P->HWCtx->get_proc_addr(P->HWCtx->inst, "vkGetPhysicalDeviceProperties2"));

    PFN_vkGetPhysicalDeviceMemoryProperties GetMemProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        P->HWCtx->get_proc_addr(P->HWCtx->inst, "vkGetPhysicalDeviceMemoryProperties"));
    if (!GetMemProps) {
        Error = "couldn't load vkGetPhysicalDeviceMemoryProperties";
        return nullptr;
    }
    GetMemProps(P->HWCtx->phys_dev, &P->MemProps);
#ifdef _WIN32
    P->VK.vkImportSemaphoreWin32HandleKHR = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
        GetDeviceProcAddr(P->Device, "vkImportSemaphoreWin32HandleKHR"));
    P->VK.vkGetMemoryWin32HandlePropertiesKHR = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
        GetDeviceProcAddr(P->Device, "vkGetMemoryWin32HandlePropertiesKHR"));
    const bool HaveImport = P->VK.vkImportSemaphoreWin32HandleKHR != nullptr;
#else
    P->VK.vkImportSemaphoreFdKHR = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        GetDeviceProcAddr(P->Device, "vkImportSemaphoreFdKHR"));
    P->VK.vkGetMemoryFdPropertiesKHR = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(
        GetDeviceProcAddr(P->Device, "vkGetMemoryFdPropertiesKHR"));
    const bool HaveImport = P->VK.vkImportSemaphoreFdKHR != nullptr;
#endif

    if (!HaveImport || !P->VK.vkAllocateMemory || !P->VK.vkCreateBuffer || !P->VK.vkCreateSemaphore) {
        Error = "the decoder's Vulkan device doesn't have the external memory extensions enabled";
        return nullptr;
    }

    /* The final guard. Opaque handles only import into a device made from the same physical
       device, and FFmpeg selects by name rather than by UUID. QueryDevice already verified the
       name reaches the right device with a throwaway creation, so failing here means something
       shifted between then and now -- still better caught than silently sharing memory between
       two GPUs. */
    if (P->VK.vkGetPhysicalDeviceProperties2) {
        VkPhysicalDeviceIDProperties IDProps = {};
        IDProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 Props2 = {};
        Props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        Props2.pNext = &IDProps;
        P->VK.vkGetPhysicalDeviceProperties2(P->HWCtx->phys_dev, &Props2);
        if (memcmp(IDProps.deviceUUID, Device.DeviceUUID, VK_UUID_SIZE) != 0) {
            Error = std::string("the decoder landed on '") + Props2.properties.deviceName +
                "' but VapourSynth is on '" + Device.DeviceName + "'; they must be the same GPU";
            return nullptr;
        }
    }

    P->Timeline = P->VkAPI->createGPUTimeline(Core, Err, sizeof(Err));
    if (!P->Timeline) {
        Error = std::string("couldn't create a timeline: ") + Err;
        return nullptr;
    }

    VSVulkanExportedSemaphore ExportedSem = {};
    if (P->VkAPI->exportGPUSemaphore(Core, P->VkAPI->getGPUTimelineSemaphore(P->Timeline),
            &ExportedSem, Err, sizeof(Err))) {
        Error = std::string("couldn't export the timeline: ") + Err;
        return nullptr;
    }

    /* Imported rather than created with a value: the two handles then name the same timeline, so
       signalling on the decoder's device is what the core's consumers wait on. */
    VkSemaphoreTypeCreateInfo TypeInfo = {};
    TypeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    TypeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo SemInfo = {};
    SemInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    SemInfo.pNext = &TypeInfo;
    if (P->VK.vkCreateSemaphore(P->Device, &SemInfo, P->HWCtx->alloc, &P->ImportedTimeline) != VK_SUCCESS) {
        CloseExportedHandle(ExportedSem.handle, false);
        Error = "couldn't create a semaphore to import the timeline into";
        return nullptr;
    }

#ifdef _WIN32
    VkImportSemaphoreWin32HandleInfoKHR SemImport = {};
    SemImport.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR;
    SemImport.semaphore = P->ImportedTimeline;
    SemImport.handleType = static_cast<VkExternalSemaphoreHandleTypeFlagBits>(ExportedSem.handleType);
    SemImport.handle = reinterpret_cast<HANDLE>(ExportedSem.handle);
    const VkResult SemRes = P->VK.vkImportSemaphoreWin32HandleKHR(P->Device, &SemImport);
#else
    VkImportSemaphoreFdInfoKHR SemImport = {};
    SemImport.sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR;
    SemImport.semaphore = P->ImportedTimeline;
    SemImport.handleType = static_cast<VkExternalSemaphoreHandleTypeFlagBits>(ExportedSem.handleType);
    SemImport.fd = static_cast<int>(ExportedSem.handle);
    const VkResult SemRes = P->VK.vkImportSemaphoreFdKHR(P->Device, &SemImport);
#endif
    CloseExportedHandle(ExportedSem.handle, SemRes == VK_SUCCESS);
    if (SemRes != VK_SUCCESS) {
        Error = "couldn't import the timeline into the decoder's device";
        return nullptr;
    }

    return Self;
}

VSFrame *BSVSGpuExport::ExportFrame(const BestVideoFrame *Src, const VSVideoFormat *Format,
    int Width, int Height, VSCore *Core, const VSAPI *vsapi) {
    std::lock_guard<std::mutex> Lock(P->Mutex);

    if (Format->numPlanes != 3)
        throw BestSourceException("GPU export: only three plane output is supported");

    VSFrame *Dst = P->VkAPI->newGPUVideoFrame(Format, Width, Height, nullptr, Core);
    if (!Dst)
        throw BestSourceException("GPU export: couldn't allocate a GPU frame");

    try {
        BSGpuPlaneTarget Targets[3] = {};
        char Err[512] = {};
        for (int Plane = 0; Plane < 3; Plane++) {
            VSVulkanExportedMemory Exported = {};
            if (P->VkAPI->exportGPUPlane(Dst, Plane, &Exported, Err, sizeof(Err)))
                throw BestSourceException(std::string("GPU export: couldn't export plane ") +
                    std::to_string(Plane) + ": " + Err);

            Targets[Plane].Buffer = P->ImportAllocation(Exported);
            Targets[Plane].Offset = Exported.offset;
            Targets[Plane].Stride = vsapi->getStride(Dst, Plane);
        }

        const uint64_t SignalValue = ++P->SignalCounter;
        if (Src->HasPendingFieldMerge()) {
            /* RFF. MergeField could not write into either decoded image, so the interleave happens
               here instead: one dispatch per source frame, each writing its own parity of rows. */
            P->Hasher->ExportMergedFieldsAsPlanarGPU(Src->GetEvenRowsAVFrame(), Src->GetOddRowsAVFrame(),
                Width, Height, Targets, P->ImportedTimeline, SignalValue);
        } else {
            P->Hasher->ExportAsPlanarGPU(Src->GetAVFrame(), Width, Height, Targets, P->ImportedTimeline, SignalValue);
        }

        /* Every plane is produced by the same submission, so they share the pair. Each takes its
           own reference to the timeline, which is what lets this object release its reference in
           the destructor without waiting for frames still in flight. */
        for (int Plane = 0; Plane < 3; Plane++)
            P->VkAPI->setGPUPlaneProducer(Dst, Plane, P->Timeline, SignalValue);
    } catch (...) {
        vsapi->freeFrame(Dst);
        throw;
    }

    return Dst;
}

#else /* !BS_GPU_HASH */

bool BSVSGpuExport::QueryDevice(VSCore *, const VSAPI *, BSVSGpuDeviceInfo &, std::string &Error) {
    Error = "GPU frame output was not compiled into this build";
    return false;
}

std::unique_ptr<BSVSGpuExport> BSVSGpuExport::Create(BestVideoSource *, int, const BSVSGpuDeviceInfo &, VSCore *, const VSAPI *, std::string &Error) {
    Error = "GPU frame output was not compiled into this build";
    return nullptr;
}

struct BSVSGpuExport::Impl {};
BSVSGpuExport::BSVSGpuExport() = default;
BSVSGpuExport::~BSVSGpuExport() = default;

const std::string &BSVSGpuExport::GetDeviceName() const {
    static const std::string Empty;
    return Empty;
}

VSFrame *BSVSGpuExport::ExportFrame(const BestVideoFrame *, const VSVideoFormat *, int, int, VSCore *, const VSAPI *) {
    throw BestSourceException("GPU frame output was not compiled into this build");
}

#endif
