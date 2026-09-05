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

#include "gpuhash.h"
#include "bsshared.h"

/* Always compiled, so BestVideoSource holds the same members either way and the public headers do
   not change shape with the build option. Only the implementation is conditional. */
#if BS_GPU

#include <mutex>
#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
#include <libavutil/version.h>
}

/* Generated from src/shaders/hashexport.comp.glsl at build time; see tools/spv2c.py. */
#include "hashexport_spv.h"

#include "vulkanshared.h"

/* Must match what the shader was compiled with; meson passes the same value to both. The default
   only exists so the file still compiles if it is ever built outside that rule. */
#ifndef BS_GPU_SAMPLES_X
#define BS_GPU_SAMPLES_X 8
#endif

namespace {

/* Every Vulkan entry point is loaded through AVVulkanDeviceContext::get_proc_addr rather than by
   linking the loader, so BestSource never needs to find libvulkan itself and is guaranteed to be
   using the same loader FFmpeg is. */
#define BS_VK_FUNCS(F)                     \
    F(vkGetDeviceQueue2)                   \
    F(vkCreateImageView)                   \
    F(vkDestroyImageView)                  \
    F(vkCreateDescriptorSetLayout)         \
    F(vkDestroyDescriptorSetLayout)        \
    F(vkCreateDescriptorPool)              \
    F(vkDestroyDescriptorPool)             \
    F(vkAllocateDescriptorSets)            \
    F(vkUpdateDescriptorSets)              \
    F(vkCreatePipelineLayout)              \
    F(vkDestroyPipelineLayout)             \
    F(vkCreateShaderModule)                \
    F(vkDestroyShaderModule)               \
    F(vkCreateComputePipelines)            \
    F(vkDestroyPipeline)                   \
    F(vkCreateCommandPool)                 \
    F(vkDestroyCommandPool)                \
    F(vkAllocateCommandBuffers)            \
    F(vkBeginCommandBuffer)                \
    F(vkEndCommandBuffer)                  \
    F(vkResetCommandPool)                  \
    F(vkCmdPipelineBarrier)                \
    F(vkCmdFillBuffer)                     \
    F(vkCmdBindPipeline)                   \
    F(vkCmdBindDescriptorSets)             \
    F(vkCmdPushConstants)                  \
    F(vkCmdDispatch)                       \
    F(vkCreateBuffer)                      \
    F(vkDestroyBuffer)                     \
    F(vkGetBufferMemoryRequirements)       \
    F(vkAllocateMemory)                    \
    F(vkFreeMemory)                        \
    F(vkBindBufferMemory)                  \
    F(vkMapMemory)                         \
    F(vkQueueSubmit)                       \
    F(vkWaitSemaphores)                    \
    F(vkCreateSemaphore)                   \
    F(vkDestroySemaphore)                  \
    F(vkGetSemaphoreCounterValue)

struct VulkanFunctions {
    BS_VK_FUNCS(BS_VK_DECLARE_FUNC)
};

/* The _UINT reinterpretation of a plane's storage format. Reading raw integers rather than through
   a _UNORM view keeps the hash defined on the stored bits, which sidesteps the question of whether
   a 10 bit format is LSB or MSB aligned. FFmpeg creates its images with VK_IMAGE_CREATE_
   MUTABLE_FORMAT_BIT and no VkImageFormatListCreateInfo, so any compatible format is allowed. */
VkFormat UintViewFormat(VkFormat Fmt) {
    switch (Fmt) {
    case VK_FORMAT_R8_UNORM:     return VK_FORMAT_R8_UINT;
    case VK_FORMAT_R8G8_UNORM:   return VK_FORMAT_R8G8_UINT;
    case VK_FORMAT_R16_UNORM:    return VK_FORMAT_R16_UINT;
    case VK_FORMAT_R16G16_UNORM: return VK_FORMAT_R16G16_UINT;
    default:                     return VK_FORMAT_UNDEFINED;
    }
}

[[noreturn]] void ThrowVk(const char *What, VkResult Res) {
    BSThrowVk("GPU hashing", What, Res);
}

struct MappedBuffer {
    VkBuffer Buffer = VK_NULL_HANDLE;
    VkDeviceMemory Memory = VK_NULL_HANDLE;
    void *Mapped = nullptr;
};

/* Mirrors the push constant block in hashexport.comp.glsl. Declared here rather than inside the
   dispatch so the pipeline layout can size itself with sizeof: a hand written byte count silently
   stops covering the block the moment a field is added, and a layout that does not cover the whole
   block makes the shader read garbage rather than fail. Strides and offsets are in samples. */
struct BSHashExportPushConstants {
    int32_t LumaW, LumaH, ChromaW, ChromaH;
    int32_t StrideY, StrideUV;
    int32_t OffsetY, OffsetU, OffsetV;
    int32_t ExportShift;
    int32_t RowOffset, RowStep;
};

/* One frame feeding one dispatch, and which rows of the output it supplies. A whole frame is
   (0, 1); a field merge is two of these, (0, 2) and (1, 2). */
struct DispatchSource {
    const AVFrame *Frame;
    int32_t RowOffset;
    int32_t RowStep;
};

/* RFF merges two frames and nothing merges more, so every array here is sized for that rather than
   being a vector. */
constexpr int MaxDispatchSources = 2;

} // namespace

struct BSGpuHasher::Impl {
    AVBufferRef *DeviceRef = nullptr;
    AVVulkanDeviceContext *HWCtx = nullptr;
    VkDevice Device = VK_NULL_HANDLE;
    VulkanFunctions VK;
    VkPhysicalDeviceMemoryProperties MemProps = {};

    VkQueue Queue = VK_NULL_HANDLE;
    uint32_t QueueFamily = 0;
    int QueueFamilyListIndex = -1;

    VkDescriptorSetLayout SetLayout = VK_NULL_HANDLE;
    VkDescriptorPool DescPool = VK_NULL_HANDLE;
    VkPipelineLayout PipeLayout = VK_NULL_HANDLE;

    /* Everything one submission needs to keep until the GPU is done with it. An export returns
       as soon as it is submitted, so its context stays claimed until the Done timeline says the
       work has completed; a hash waits for its result and retires its context on the spot. One
       descriptor set per possible source frame, because a merge binds a different image pair
       per dispatch and a set cannot be rewritten between two dispatches recorded into the same
       command buffer. A command pool each, so recycling one context never resets another's
       buffer. */
    struct ExecContext {
        VkCommandPool CmdPool = VK_NULL_HANDLE;
        VkCommandBuffer Cmd = VK_NULL_HANDLE;
        VkDescriptorSet DescSet[MaxDispatchSources] = {};
        VkImageView Views[MaxDispatchSources][2] = {};
        /* References that keep the source frames, and with them the images the views name,
           alive until the context is retired. */
        AVFrame *Held[MaxDispatchSources] = {};
        /* The Done value whose completion retires it; 0 while free. */
        uint64_t Value = 0;
    };
    /* Two is what the steady state needs: the next frame's hash has to record while the previous
       export is still running, and any hash drains the ring, since on one queue its completion
       implies everything submitted before it. More would only smooth runs of exports with no
       hash in between -- frames served from the cache -- at the price of one held pool frame
       per context between requests. With every context claimed, the next dispatch waits for
       the oldest to complete. */
    static constexpr int NumContexts = 2;
    ExecContext Contexts[NumContexts];

    /* Signalled by every submission with its sequence number, so completion is a counter read
       and teardown knows when the last one is done. */
    VkSemaphore Done = VK_NULL_HANDLE;
    uint64_t Submitted = 0;

    /* One pipeline per sample size and pass, created on first use. Which pass it is comes in as a
       specialization constant, so the hash pipeline carries no plane writing code and the export
       pipeline no hashing code. */
    VkShaderModule Module[2] = {};
    VkPipeline Pipeline[2][2] = {};

    MappedBuffer Acc, Dummy[3];

    /* HashFrame submits to a queue FFmpeg may also be using and mutates shared descriptor and
       command buffer state, so calls are serialized. Nothing is lost by it while the
       implementation is synchronous. */
    std::mutex Mutex;

    ~Impl();
    void CreateBuffer(VkDeviceSize Size, MappedBuffer &Out);
    void DestroyBuffer(MappedBuffer &B);
    VkPipeline GetPipeline(int BytesPerSample, bool DoExport);
    void LockQueue();
    void UnlockQueue();
    /* Retires what has completed and hands out a free context, waiting for the oldest in
       flight when every one is claimed. */
    ExecContext &AcquireContext();
    /* Releases everything a context held and marks it free. Only for a context whose work
       has completed or was never submitted. */
    void RetireContext(ExecContext &C);
    void RetireCompleted();
    /* Waits for every submission so far, then retires all contexts. A failed wait can only be
       a lost device, where nothing further is reachable anyway, so it does not throw. */
    void FinishAll();
    /* Targets is what selects the pass. Null hashes, and ExportWidth/ExportHeight are then ignored
       -- a hash must cover the full decoded frame, since that is what the index hashes were computed
       over. Set, it exports, and they bound the writes to the destination's extent, which for odd
       dimensions is smaller than the decoded frame. SignalTimeline null means only the frames' own
       semaphores are signalled. Returns the frame's hash when hashing and zero when exporting. */
    uint64_t RunDispatch(const DispatchSource *Sources, int NumSources,
                         int ExportWidth, int ExportHeight,
                         const BSGpuPlaneTarget *Targets,
                         VkSemaphore SignalTimeline, uint64_t SignalValue);
};

void BSGpuHasher::Impl::CreateBuffer(VkDeviceSize Size, MappedBuffer &Out) {
    VkBufferCreateInfo BCI = {};
    BCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    BCI.size = Size;
    BCI.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    BCI.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult Res = VK.vkCreateBuffer(Device, &BCI, HWCtx->alloc, &Out.Buffer);
    if (Res != VK_SUCCESS)
        ThrowVk("vkCreateBuffer", Res);

    VkMemoryRequirements Req = {};
    VK.vkGetBufferMemoryRequirements(Device, Out.Buffer, &Req);
    uint32_t Type = BSFindVkMemoryType(MemProps, Req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (Type == UINT32_MAX)
        throw BestSourceHWDecoderException("GPU hashing: no host visible coherent memory type");

    VkMemoryAllocateInfo AI = {};
    AI.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    AI.allocationSize = Req.size;
    AI.memoryTypeIndex = Type;
    Res = VK.vkAllocateMemory(Device, &AI, HWCtx->alloc, &Out.Memory);
    if (Res != VK_SUCCESS)
        ThrowVk("vkAllocateMemory", Res);
    Res = VK.vkBindBufferMemory(Device, Out.Buffer, Out.Memory, 0);
    if (Res != VK_SUCCESS)
        ThrowVk("vkBindBufferMemory", Res);
    Res = VK.vkMapMemory(Device, Out.Memory, 0, VK_WHOLE_SIZE, 0, &Out.Mapped);
    if (Res != VK_SUCCESS)
        ThrowVk("vkMapMemory", Res);
    memset(Out.Mapped, 0, static_cast<size_t>(Size));
}

void BSGpuHasher::Impl::DestroyBuffer(MappedBuffer &B) {
    if (B.Buffer)
        VK.vkDestroyBuffer(Device, B.Buffer, HWCtx->alloc);
    if (B.Memory)
        VK.vkFreeMemory(Device, B.Memory, HWCtx->alloc);
    B = {};
}

VkPipeline BSGpuHasher::Impl::GetPipeline(int BytesPerSample, bool DoExport) {
    const int Slot = (BytesPerSample == 2) ? 1 : 0;
    const int Mode = DoExport ? 1 : 0;
    if (Pipeline[Slot][Mode])
        return Pipeline[Slot][Mode];

    if (!Module[Slot]) {
        const uint32_t *Code = Slot ? BSHashExportSpv16 : BSHashExportSpv8;
        const size_t Size = Slot ? sizeof(BSHashExportSpv16) : sizeof(BSHashExportSpv8);

        VkShaderModuleCreateInfo SMCI = {};
        SMCI.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        SMCI.codeSize = Size;
        SMCI.pCode = Code;
        VkResult MRes = VK.vkCreateShaderModule(Device, &SMCI, HWCtx->alloc, &Module[Slot]);
        if (MRes != VK_SUCCESS)
            ThrowVk("vkCreateShaderModule", MRes);
    }

    const uint32_t Const = DoExport ? 1u : 0u;
    VkSpecializationMapEntry Entry = { 0, 0, sizeof(uint32_t) };
    VkSpecializationInfo Spec = { 1, &Entry, sizeof(Const), &Const };

    VkComputePipelineCreateInfo CPCI = {};
    CPCI.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    CPCI.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    CPCI.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    CPCI.stage.module = Module[Slot];
    CPCI.stage.pName = "main";
    CPCI.stage.pSpecializationInfo = &Spec;
    CPCI.layout = PipeLayout;
    VkResult Res = VK.vkCreateComputePipelines(Device, VK_NULL_HANDLE, 1, &CPCI, HWCtx->alloc, &Pipeline[Slot][Mode]);
    if (Res != VK_SUCCESS)
        ThrowVk("vkCreateComputePipelines", Res);
    return Pipeline[Slot][Mode];
}

/* FFmpeg hands out a queue lock because its own submissions share these queues. It is deprecated
   in favour of VK_KHR_internally_synchronized_queues but still the only portable option. The
   macro exists on every libavutil the build accepts (61 defines it true, 62 will define it false
   once the field leaves the ABI), so this guard can only ever compile the lock out together with
   the field itself -- never on a libavutil that still needs the locking, which is why the
   dependency floor is 61. */
void BSGpuHasher::Impl::LockQueue() {
#if defined(FF_API_VULKAN_SYNC_QUEUES) && FF_API_VULKAN_SYNC_QUEUES
    AVHWDeviceContext *Ctx = reinterpret_cast<AVHWDeviceContext *>(DeviceRef->data);
    if (HWCtx->lock_queue)
        HWCtx->lock_queue(Ctx, QueueFamily, 0);
#endif
}

void BSGpuHasher::Impl::UnlockQueue() {
#if defined(FF_API_VULKAN_SYNC_QUEUES) && FF_API_VULKAN_SYNC_QUEUES
    AVHWDeviceContext *Ctx = reinterpret_cast<AVHWDeviceContext *>(DeviceRef->data);
    if (HWCtx->unlock_queue)
        HWCtx->unlock_queue(Ctx, QueueFamily, 0);
#endif
}

void BSGpuHasher::Impl::RetireContext(ExecContext &C) {
    for (int s = 0; s < MaxDispatchSources; s++) {
        for (int p = 0; p < 2; p++) {
            if (C.Views[s][p])
                VK.vkDestroyImageView(Device, C.Views[s][p], HWCtx->alloc);
            C.Views[s][p] = VK_NULL_HANDLE;
        }
        av_frame_free(&C.Held[s]);
    }
    C.Value = 0;
}

void BSGpuHasher::Impl::RetireCompleted() {
    uint64_t Completed = 0;
    if (!Done || VK.vkGetSemaphoreCounterValue(Device, Done, &Completed) != VK_SUCCESS)
        return;
    for (auto &C : Contexts)
        if (C.Value && C.Value <= Completed)
            RetireContext(C);
}

BSGpuHasher::Impl::ExecContext &BSGpuHasher::Impl::AcquireContext() {
    RetireCompleted();
    ExecContext *Oldest = nullptr;
    for (auto &C : Contexts) {
        if (!C.Value)
            return C;
        if (!Oldest || C.Value < Oldest->Value)
            Oldest = &C;
    }

    VkSemaphoreWaitInfo SWI = {};
    SWI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
    SWI.semaphoreCount = 1;
    SWI.pSemaphores = &Done;
    SWI.pValues = &Oldest->Value;
    VkResult Res = VK.vkWaitSemaphores(Device, &SWI, UINT64_MAX);
    if (Res != VK_SUCCESS)
        ThrowVk("vkWaitSemaphores", Res);
    RetireContext(*Oldest);
    return *Oldest;
}

void BSGpuHasher::Impl::FinishAll() {
    if (Done && Submitted) {
        VkSemaphoreWaitInfo SWI = {};
        SWI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        SWI.semaphoreCount = 1;
        SWI.pSemaphores = &Done;
        SWI.pValues = &Submitted;
        (void)VK.vkWaitSemaphores(Device, &SWI, UINT64_MAX);
    }
    for (auto &C : Contexts)
        RetireContext(C);
}

BSGpuHasher::Impl::~Impl() {
    if (Device) {
        FinishAll();
        for (auto &C : Contexts)
            if (C.CmdPool)
                VK.vkDestroyCommandPool(Device, C.CmdPool, HWCtx->alloc);
        if (Done)
            VK.vkDestroySemaphore(Device, Done, HWCtx->alloc);
        for (int i = 0; i < 2; i++) {
            for (int m = 0; m < 2; m++)
                if (Pipeline[i][m])
                    VK.vkDestroyPipeline(Device, Pipeline[i][m], HWCtx->alloc);
            if (Module[i])
                VK.vkDestroyShaderModule(Device, Module[i], HWCtx->alloc);
        }
        if (PipeLayout)
            VK.vkDestroyPipelineLayout(Device, PipeLayout, HWCtx->alloc);
        if (DescPool)
            VK.vkDestroyDescriptorPool(Device, DescPool, HWCtx->alloc);
        if (SetLayout)
            VK.vkDestroyDescriptorSetLayout(Device, SetLayout, HWCtx->alloc);
        DestroyBuffer(Acc);
        for (auto &D : Dummy)
            DestroyBuffer(D);
    }
    av_buffer_unref(&DeviceRef);
}

bool BSGpuHasher::IsSupportedSwFormat(int PixelFormat) {
    const AVPixelFormat Fmt = static_cast<AVPixelFormat>(PixelFormat);
    if (av_pix_fmt_count_planes(Fmt) != 2)
        return false;
    const VkFormat *PlaneFmts = av_vkfmt_from_pixfmt(Fmt);
    return PlaneFmts && UintViewFormat(PlaneFmts[0]) != VK_FORMAT_UNDEFINED &&
        UintViewFormat(PlaneFmts[1]) != VK_FORMAT_UNDEFINED;
}

bool BSGpuHasher::IsSupportedFrame(const AVFrame *Frame) {
    if (!Frame || Frame->format != AV_PIX_FMT_VULKAN || !Frame->hw_frames_ctx)
        return false;
    const AVHWFramesContext *Frames = reinterpret_cast<const AVHWFramesContext *>(Frame->hw_frames_ctx->data);
    if (!IsSupportedSwFormat(Frames->sw_format))
        return false;
    /* Everything downstream treats the frame as one multiplane image: img[0] with plane aspects,
       one semaphore, one layout. FFmpeg documents img[] as "may be one for multiplane formats, or
       multiple", and falls back to an image per plane where a driver lacks the multiplane feature
       bits, in which case a plane view of img[0] would be invalid usage. There is no count field;
       a second image is what says the layout is per plane. */
    const AVVkFrame *Vkf = reinterpret_cast<const AVVkFrame *>(Frame->data[0]);
    return Vkf && Vkf->img[1] == VK_NULL_HANDLE;
}

BSGpuHasher::BSGpuHasher(AVBufferRef *HWDeviceContext) : P(new Impl) {
    if (!HWDeviceContext)
        throw BestSourceHWDecoderException("GPU hashing: no hardware device context");

    AVHWDeviceContext *Ctx = reinterpret_cast<AVHWDeviceContext *>(HWDeviceContext->data);
    if (Ctx->type != AV_HWDEVICE_TYPE_VULKAN)
        throw BestSourceHWDecoderException("GPU hashing: device is not vulkan");

    P->DeviceRef = av_buffer_ref(HWDeviceContext);
    if (!P->DeviceRef)
        throw BestSourceHWDecoderException("GPU hashing: couldn't reference device");
    P->HWCtx = reinterpret_cast<AVVulkanDeviceContext *>(Ctx->hwctx);
    P->Device = P->HWCtx->act_dev;

    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        P->HWCtx->get_proc_addr(P->HWCtx->inst, "vkGetDeviceProcAddr"));
    if (!GetDeviceProcAddr)
        throw BestSourceHWDecoderException("GPU hashing: couldn't load vkGetDeviceProcAddr");

#define BS_LOAD(n) P->VK.n = reinterpret_cast<PFN_##n>(GetDeviceProcAddr(P->Device, #n));
    BS_VK_FUNCS(BS_LOAD)
#undef BS_LOAD
#define BS_CHECK(n) if (!P->VK.n) throw BestSourceHWDecoderException("GPU hashing: missing entry point " #n);
    BS_VK_FUNCS(BS_CHECK)
#undef BS_CHECK

    /* Instance level, so it has to come from get_proc_addr on the instance: asking
       vkGetDeviceProcAddr for it is specified to return null, even though some drivers hand the
       pointer out anyway. */
    auto GetMemProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        P->HWCtx->get_proc_addr(P->HWCtx->inst, "vkGetPhysicalDeviceMemoryProperties"));
    if (!GetMemProps)
        throw BestSourceHWDecoderException("GPU hashing: couldn't load vkGetPhysicalDeviceMemoryProperties");
    GetMemProps(P->HWCtx->phys_dev, &P->MemProps);

    for (int i = 0; i < P->HWCtx->nb_qf; i++) {
        if (P->HWCtx->qf[i].flags & VK_QUEUE_COMPUTE_BIT) {
            P->QueueFamilyListIndex = i;
            P->QueueFamily = static_cast<uint32_t>(P->HWCtx->qf[i].idx);
            break;
        }
    }
    if (P->QueueFamilyListIndex < 0)
        throw BestSourceHWDecoderException("GPU hashing: device has no compute queue family");

    /* FFmpeg 9 (libavutil 61) creates its queues with queue_flags, which includes
       VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR wherever the extension exists --
       mesa exposes it, the NVIDIA driver currently does not. A queue created with nonzero flags
       must be retrieved with vkGetDeviceQueue2 carrying the same flags, and vkGetDeviceQueue is
       invalid for it. With flags zero the two are equivalent, so 2 is used unconditionally. */
    VkDeviceQueueInfo2 QueueInfo = {};
    QueueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2;
    QueueInfo.flags = P->HWCtx->queue_flags;
    QueueInfo.queueFamilyIndex = P->QueueFamily;
    QueueInfo.queueIndex = 0;
    P->VK.vkGetDeviceQueue2(P->Device, &QueueInfo, &P->Queue);
    if (P->Queue == VK_NULL_HANDLE)
        throw BestSourceHWDecoderException("GPU hashing: couldn't retrieve the compute queue");

    P->CreateBuffer(2 * sizeof(uint32_t), P->Acc);
    for (auto &D : P->Dummy)
        P->CreateBuffer(64, D);

    /* Bindings 0-1 are the plane images, 2-4 the export destinations and 5 the hash accumulator.
       The export destinations are unused with do_export = 0 but still have to be bound, since the
       shader declares them. */
    VkDescriptorSetLayoutBinding Bindings[6] = {};
    for (int i = 0; i < 6; i++) {
        Bindings[i].binding = i;
        Bindings[i].descriptorCount = 1;
        Bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        Bindings[i].descriptorType = (i < 2) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    VkDescriptorSetLayoutCreateInfo DSLCI = {};
    DSLCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    DSLCI.bindingCount = 6;
    DSLCI.pBindings = Bindings;
    VkResult Res = P->VK.vkCreateDescriptorSetLayout(P->Device, &DSLCI, P->HWCtx->alloc, &P->SetLayout);
    if (Res != VK_SUCCESS)
        ThrowVk("vkCreateDescriptorSetLayout", Res);

    constexpr uint32_t NumSets = Impl::NumContexts * MaxDispatchSources;
    VkDescriptorPoolSize PoolSizes[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * NumSets },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 * NumSets },
    };
    VkDescriptorPoolCreateInfo DPCI = {};
    DPCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    DPCI.maxSets = NumSets;
    DPCI.poolSizeCount = 2;
    DPCI.pPoolSizes = PoolSizes;
    Res = P->VK.vkCreateDescriptorPool(P->Device, &DPCI, P->HWCtx->alloc, &P->DescPool);
    if (Res != VK_SUCCESS)
        ThrowVk("vkCreateDescriptorPool", Res);

    VkDescriptorSetLayout SetLayouts[MaxDispatchSources];
    for (auto &L : SetLayouts)
        L = P->SetLayout;
    /* The buffer bindings are written once so every set is complete from the start; the
       destinations are rewritten per dispatch and the accumulator never changes. */
    VkBuffer BufOrder[4] = { P->Dummy[0].Buffer, P->Dummy[1].Buffer, P->Dummy[2].Buffer, P->Acc.Buffer };
    VkDescriptorBufferInfo BufInfo[4] = {};
    for (int i = 0; i < 4; i++) {
        BufInfo[i].buffer = BufOrder[i];
        BufInfo[i].range = VK_WHOLE_SIZE;
    }
    for (auto &C : P->Contexts) {
        VkDescriptorSetAllocateInfo DSAI = {};
        DSAI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        DSAI.descriptorPool = P->DescPool;
        DSAI.descriptorSetCount = MaxDispatchSources;
        DSAI.pSetLayouts = SetLayouts;
        Res = P->VK.vkAllocateDescriptorSets(P->Device, &DSAI, C.DescSet);
        if (Res != VK_SUCCESS)
            ThrowVk("vkAllocateDescriptorSets", Res);

        VkWriteDescriptorSet Writes[4 * MaxDispatchSources] = {};
        for (int i = 0; i < 4; i++) {
            for (int s = 0; s < MaxDispatchSources; s++) {
                VkWriteDescriptorSet &W = Writes[s * 4 + i];
                W.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                W.dstSet = C.DescSet[s];
                W.dstBinding = i + 2;
                W.descriptorCount = 1;
                W.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                W.pBufferInfo = &BufInfo[i];
            }
        }
        P->VK.vkUpdateDescriptorSets(P->Device, 4 * MaxDispatchSources, Writes, 0, nullptr);
    }

    VkPushConstantRange PCR = {};
    PCR.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    PCR.size = sizeof(BSHashExportPushConstants);
    VkPipelineLayoutCreateInfo PLCI = {};
    PLCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    PLCI.setLayoutCount = 1;
    PLCI.pSetLayouts = &P->SetLayout;
    PLCI.pushConstantRangeCount = 1;
    PLCI.pPushConstantRanges = &PCR;
    Res = P->VK.vkCreatePipelineLayout(P->Device, &PLCI, P->HWCtx->alloc, &P->PipeLayout);
    if (Res != VK_SUCCESS)
        ThrowVk("vkCreatePipelineLayout", Res);

    for (auto &C : P->Contexts) {
        VkCommandPoolCreateInfo CPCI = {};
        CPCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        CPCI.queueFamilyIndex = P->QueueFamily;
        Res = P->VK.vkCreateCommandPool(P->Device, &CPCI, P->HWCtx->alloc, &C.CmdPool);
        if (Res != VK_SUCCESS)
            ThrowVk("vkCreateCommandPool", Res);

        VkCommandBufferAllocateInfo CBAI = {};
        CBAI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        CBAI.commandPool = C.CmdPool;
        CBAI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        CBAI.commandBufferCount = 1;
        Res = P->VK.vkAllocateCommandBuffers(P->Device, &CBAI, &C.Cmd);
        if (Res != VK_SUCCESS)
            ThrowVk("vkAllocateCommandBuffers", Res);
    }

    VkSemaphoreTypeCreateInfo DoneType = {};
    DoneType.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    DoneType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo DoneCI = {};
    DoneCI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    DoneCI.pNext = &DoneType;
    Res = P->VK.vkCreateSemaphore(P->Device, &DoneCI, P->HWCtx->alloc, &P->Done);
    if (Res != VK_SUCCESS)
        ThrowVk("vkCreateSemaphore", Res);
}

BSGpuHasher::~BSGpuHasher() = default;

uint64_t BSGpuHasher::Impl::RunDispatch(const DispatchSource *Sources, int NumSources,
    int ExportWidth, int ExportHeight,
    const BSGpuPlaneTarget *Targets,
    VkSemaphore SignalTimeline, uint64_t SignalValue) {
    Impl *P = this;
    const bool DoExport = (Targets != nullptr);
    const AVFrame *Frame = Sources[0].Frame;

    if (DoExport && (ExportWidth <= 0 || ExportHeight <= 0 || ExportWidth > Frame->width || ExportHeight > Frame->height))
        throw BestSourceHWDecoderException("GPU export: destination size must be positive and no larger than the decoded frame");

    /* What the shader bounds itself by, per plane. The decoded size for hashing, the destination
       size for export: a decoder pads odd dimensions up to the subsampling grid, the destination
       is cropped down to it, and writing by the decoded size runs one row and column past every
       plane of such a destination. */
    const int Width = DoExport ? ExportWidth : Frame->width;
    const int Height = DoExport ? ExportHeight : Frame->height;

    AVHWFramesContext *Frames = reinterpret_cast<AVHWFramesContext *>(Frame->hw_frames_ctx->data);
    const AVPixFmtDescriptor *Desc = av_pix_fmt_desc_get(Frames->sw_format);
    const int Depth = Desc->comp[0].depth;
    const int BytesPerSample = (Depth > 8) ? 2 : 1;
    const VkFormat *PlaneFmts = av_vkfmt_from_pixfmt(Frames->sw_format);

    /* Every source writes into one destination, so they have to agree on geometry. Distinct images
       matter too: the submission below waits on and signals each source's timeline once, and doing
       that twice for the same semaphore in one submit is not a legal thing to ask for. */
    AVVkFrame *Vkf[MaxDispatchSources] = {};
    AVHWFramesContext *FC[MaxDispatchSources] = {};
    for (int s = 0; s < NumSources; s++) {
        const AVFrame *F = Sources[s].Frame;
        FC[s] = reinterpret_cast<AVHWFramesContext *>(F->hw_frames_ctx->data);
        if (F->width != Frame->width || F->height != Frame->height || FC[s]->sw_format != Frames->sw_format)
            throw BestSourceHWDecoderException("GPU export: merged frames must have the same format and size");
        Vkf[s] = reinterpret_cast<AVVkFrame *>(F->data[0]);
        for (int t = 0; t < s; t++)
            if (Vkf[t] == Vkf[s])
                throw BestSourceHWDecoderException("GPU export: merged frames must be distinct images");
    }

    BSHashExportPushConstants PC = {
        Width, Height,
        AV_CEIL_RSHIFT(Width, Desc->log2_chroma_w),
        AV_CEIL_RSHIFT(Height, Desc->log2_chroma_h),
        0, 0, 0, 0, 0, 0, 0, 1
    };

    if (DoExport) {
        for (int i = 0; i < 3; i++) {
            if (Targets[i].Stride <= 0)
                throw BestSourceHWDecoderException("GPU export: plane stride must be positive");
            if (Targets[i].Stride % BytesPerSample || Targets[i].Offset % BytesPerSample)
                throw BestSourceHWDecoderException("GPU export: plane stride and offset must be a whole number of samples");
            /* The shader addresses samples with int32 arithmetic, so a plane sitting deep inside a
               large shared allocation must be rejected rather than have its offset wrap negative
               and the writes land somewhere unrelated in the buffer. The bound covers the last
               sample the plane can touch, not just its first. */
            const int PlaneRows = (i == 0) ? Height : AV_CEIL_RSHIFT(Height, Desc->log2_chroma_h);
            const int64_t LastSample = static_cast<int64_t>(Targets[i].Offset / BytesPerSample) +
                static_cast<int64_t>(PlaneRows) * (Targets[i].Stride / BytesPerSample);
            if (LastSample > INT32_MAX)
                throw BestSourceHWDecoderException("GPU export: plane offset and extent exceed the shader's addressable range");
        }
        PC.StrideY = static_cast<int32_t>(Targets[0].Stride / BytesPerSample);
        PC.StrideUV = static_cast<int32_t>(Targets[1].Stride / BytesPerSample);
        PC.OffsetY = static_cast<int32_t>(Targets[0].Offset / BytesPerSample);
        PC.OffsetU = static_cast<int32_t>(Targets[1].Offset / BytesPerSample);
        PC.OffsetV = static_cast<int32_t>(Targets[2].Offset / BytesPerSample);
        if (Targets[1].Stride != Targets[2].Stride)
            throw BestSourceHWDecoderException("GPU export: the two chroma planes must share a stride");
        /* The P010 family stores samples MSB aligned in a 16 bit container while planar output
           wants them LSB aligned. libp2p applies the same shift under the name nv_shift, which is
           what keeps this agreeing with ExportAsPlanar. */
        PC.ExportShift = (BytesPerSample == 2) ? (16 - Depth) : 0;
    }

    /* What the GPU may still be using after this returns is kept in a context: image views
       (created per call, since a cached view outliving its image after a decoder reset is a worse
       bug than the allocation is a cost), references to the source frames, and the command buffer
       and descriptor sets. A hash retires it before returning; an export leaves it claimed until
       the Done timeline says the work has completed. */
    ExecContext &C = P->AcquireContext();
    const VkImageAspectFlagBits Aspects[2] = { VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_ASPECT_PLANE_1_BIT };
    for (int s = 0; s < NumSources; s++) {
        C.Held[s] = av_frame_clone(Sources[s].Frame);
        if (!C.Held[s]) {
            P->RetireContext(C);
            throw BestSourceHWDecoderException("GPU hashing: couldn't reference a source frame");
        }
        for (int p = 0; p < 2; p++) {
            VkImageViewUsageCreateInfo Usage = {};
            Usage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
            Usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
            VkImageViewCreateInfo CI = {};
            CI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            CI.pNext = &Usage;
            CI.image = Vkf[s]->img[0];
            CI.viewType = VK_IMAGE_VIEW_TYPE_2D;
            CI.format = UintViewFormat(PlaneFmts[p]);
            CI.subresourceRange.aspectMask = Aspects[p];
            CI.subresourceRange.levelCount = 1;
            CI.subresourceRange.layerCount = 1;
            VkResult Res = P->VK.vkCreateImageView(P->Device, &CI, P->HWCtx->alloc, &C.Views[s][p]);
            if (Res != VK_SUCCESS) {
                C.Views[s][p] = VK_NULL_HANDLE;
                P->RetireContext(C);
                ThrowVk("vkCreateImageView", Res);
            }
        }
    }

    /* FFmpeg's frame lock, held from the first read of a frame's synchronization state until the
       new state has been published after submission, and never across the host wait: that is
       the contract in hwcontext_vulkan.h, and the decoder's worker threads take the same lock
       when they add these frames as decode references, so without it the two race over
       sem_value and layout. Address order, so two merges holding each other's frames cannot
       deadlock. */
    int LockOrder[MaxDispatchSources] = { 0, 1 };
    if (NumSources > 1 && reinterpret_cast<uintptr_t>(Vkf[1]) < reinterpret_cast<uintptr_t>(Vkf[0])) {
        LockOrder[0] = 1;
        LockOrder[1] = 0;
    }
    bool FramesLocked = false;
    auto LockFrames = [&]() {
        for (int i = 0; i < NumSources; i++) {
            const int s = LockOrder[i];
            reinterpret_cast<AVVulkanFramesContext *>(FC[s]->hwctx)->lock_frame(FC[s], Vkf[s]);
        }
        FramesLocked = true;
    };
    auto UnlockFrames = [&]() {
        if (!FramesLocked)
            return;
        FramesLocked = false;
        for (int i = NumSources - 1; i >= 0; i--) {
            const int s = LockOrder[i];
            reinterpret_cast<AVVulkanFramesContext *>(FC[s]->hwctx)->unlock_frame(FC[s], Vkf[s]);
        }
    };

    uint64_t Result = 0;
    try {
        VkDescriptorImageInfo ImgInfo[MaxDispatchSources][2] = {};
        VkWriteDescriptorSet Writes[MaxDispatchSources * 2] = {};
        for (int s = 0; s < NumSources; s++) {
            for (int p = 0; p < 2; p++) {
                ImgInfo[s][p].imageView = C.Views[s][p];
                ImgInfo[s][p].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                VkWriteDescriptorSet &W = Writes[s * 2 + p];
                W.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                W.dstSet = C.DescSet[s];
                W.dstBinding = p;
                W.descriptorCount = 1;
                W.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                W.pImageInfo = &ImgInfo[s][p];
            }
        }
        P->VK.vkUpdateDescriptorSets(P->Device, NumSources * 2, Writes, 0, nullptr);

        /* Destination buffers are bound whole, at offset 0, with the plane offsets carried in the
           push constants instead. Binding at the plane offset would have to satisfy
           minStorageBufferOffsetAlignment, which an allocation imported from another device has no
           reason to meet. Every source writes into the same destination, so every set gets them.

           The hash pass rebinds them to the dummy buffers rather than leaving the set naming the
           last export's destination, which belongs to the consumer and can be freed at any time.
           A set naming a freed buffer is invalid at dispatch even in the pass that never writes
           through it, since the specialization constant that makes those writes dead is not folded
           until the pipeline is created and the module still counts them as statically used. */
        VkDescriptorBufferInfo DstInfo[3] = {};
        VkWriteDescriptorSet DstWrites[MaxDispatchSources * 3] = {};
        for (int i = 0; i < 3; i++) {
            DstInfo[i].buffer = DoExport ? Targets[i].Buffer : P->Dummy[i].Buffer;
            DstInfo[i].range = VK_WHOLE_SIZE;
            for (int s = 0; s < NumSources; s++) {
                VkWriteDescriptorSet &W = DstWrites[s * 3 + i];
                W.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                W.dstSet = C.DescSet[s];
                W.dstBinding = 2 + i;
                W.descriptorCount = 1;
                W.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                W.pBufferInfo = &DstInfo[i];
            }
        }
        P->VK.vkUpdateDescriptorSets(P->Device, NumSources * 3, DstWrites, 0, nullptr);

        VkPipeline Pipe = P->GetPipeline(BytesPerSample, DoExport);

        VkResult Res = P->VK.vkResetCommandPool(P->Device, C.CmdPool, 0);
        if (Res != VK_SUCCESS)
            ThrowVk("vkResetCommandPool", Res);

        VkCommandBufferBeginInfo BI = {};
        BI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        BI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Res = P->VK.vkBeginCommandBuffer(C.Cmd, &BI);
        if (Res != VK_SUCCESS)
            ThrowVk("vkBeginCommandBuffer", Res);

        /* The accumulator only exists for the hash pass; the export pass never touches it, so it
           pays for neither the clear nor the barriers around it. */
        if (!DoExport)
            P->VK.vkCmdFillBuffer(C.Cmd, P->Acc.Buffer, 0, VK_WHOLE_SIZE, 0);

        /* Frames arrive in VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR; a compute read needs GENERAL.
           One barrier per source covers both of its planes. queue_family is
           VK_QUEUE_FAMILY_IGNORED because FFmpeg allocates CONCURRENT, so there is no ownership
           transfer to perform. */
        LockFrames();
        VkImageMemoryBarrier ImgBar[MaxDispatchSources] = {};
        for (int s = 0; s < NumSources; s++) {
            ImgBar[s].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            ImgBar[s].srcAccessMask = static_cast<VkAccessFlags>(Vkf[s]->access[0]);
            ImgBar[s].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            ImgBar[s].oldLayout = Vkf[s]->layout[0];
            ImgBar[s].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            ImgBar[s].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ImgBar[s].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            ImgBar[s].image = Vkf[s]->img[0];
            /* COLOR and not the plane aspects: FFmpeg never creates its frames disjoint, and on a
               non-disjoint multiplanar image the spec requires a barrier to name COLOR, which
               covers every plane (VUID-VkImageMemoryBarrier-image-01671). FFmpeg's own frame
               barriers do the same. The per plane aspects remain correct for the image views
               above, where they select a plane rather than describe the transition. */
            ImgBar[s].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ImgBar[s].subresourceRange.levelCount = 1;
            ImgBar[s].subresourceRange.layerCount = 1;
        }

        VkBufferMemoryBarrier FillBar = {};
        FillBar.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        FillBar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        FillBar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        FillBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        FillBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        FillBar.buffer = P->Acc.Buffer;
        FillBar.size = VK_WHOLE_SIZE;

        P->VK.vkCmdPipelineBarrier(C.Cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, DoExport ? 0 : 1, &FillBar,
            static_cast<uint32_t>(NumSources), ImgBar);

        P->VK.vkCmdBindPipeline(C.Cmd, VK_PIPELINE_BIND_POINT_COMPUTE, Pipe);
        /* Each workgroup covers 16 invocations of BS_GPU_SAMPLES_X samples along x. */
        const int XPerGroup = 16 * BS_GPU_SAMPLES_X;
        for (int s = 0; s < NumSources; s++) {
            PC.RowOffset = Sources[s].RowOffset;
            PC.RowStep = Sources[s].RowStep;
            /* Rows this source actually supplies, which is every RowStep'th one from RowOffset. */
            const int Rows = (Height - PC.RowOffset + PC.RowStep - 1) / PC.RowStep;
            if (Rows <= 0)
                continue;
            P->VK.vkCmdBindDescriptorSets(C.Cmd, VK_PIPELINE_BIND_POINT_COMPUTE, P->PipeLayout, 0, 1, &C.DescSet[s], 0, nullptr);
            P->VK.vkCmdPushConstants(C.Cmd, P->PipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PC), &PC);
            /* No barrier between the dispatches: they read different images and write disjoint rows
               of the same buffers, so there is nothing for them to race over. */
            P->VK.vkCmdDispatch(C.Cmd, (Width + XPerGroup - 1) / XPerGroup,
                (Rows + 15) / 16, 2);
        }

        /* The accumulator has to become host readable; the exported planes have to become visible
           to whatever reads them next, which may be another device picking them up after the
           semaphore signal, hence MEMORY_READ rather than anything narrower. */
        VkBufferMemoryBarrier OutBars[3] = {};
        uint32_t NumOutBars = 0;
        if (!DoExport) {
            OutBars[NumOutBars] = FillBar;
            OutBars[NumOutBars].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            OutBars[NumOutBars].dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            NumOutBars++;
        } else {
            for (int i = 0; i < 3; i++) {
                /* Planes commonly share one buffer, so skip the duplicates. */
                bool Seen = false;
                for (int j = 0; j < i; j++)
                    Seen = Seen || (Targets[j].Buffer == Targets[i].Buffer);
                if (Seen)
                    continue;
                OutBars[NumOutBars] = FillBar;
                OutBars[NumOutBars].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                OutBars[NumOutBars].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                OutBars[NumOutBars].buffer = Targets[i].Buffer;
                NumOutBars++;
            }
        }
        P->VK.vkCmdPipelineBarrier(C.Cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            DoExport ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : (VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT),
            0, 0, nullptr, NumOutBars, OutBars, 0, nullptr);

        Res = P->VK.vkEndCommandBuffer(C.Cmd);
        if (Res != VK_SUCCESS)
            ThrowVk("vkEndCommandBuffer", Res);

        /* AVVkFrame's timeline contract: wait on sem_value, signal an incremented value. Each source
           gets that treatment separately, since they are separate frames with separate timelines.
           The caller's timeline, when given, is signalled alongside them so a consumer on another
           device can order its work against this without a host round trip, and Done last, with
           the sequence number that retires the context. */
        VkSemaphore WaitSems[MaxDispatchSources] = {};
        uint64_t WaitValues[MaxDispatchSources] = {};
        VkPipelineStageFlags WaitStages[MaxDispatchSources] = {};
        VkSemaphore SignalSems[MaxDispatchSources + 2] = {};
        uint64_t SignalValues[MaxDispatchSources + 2] = {};
        uint64_t FrameSignalValues[MaxDispatchSources] = {};
        for (int s = 0; s < NumSources; s++) {
            WaitSems[s] = Vkf[s]->sem[0];
            WaitValues[s] = Vkf[s]->sem_value[0];
            WaitStages[s] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            FrameSignalValues[s] = WaitValues[s] + 1;
            SignalSems[s] = Vkf[s]->sem[0];
            SignalValues[s] = FrameSignalValues[s];
        }
        uint32_t NumSignals = static_cast<uint32_t>(NumSources);
        if (SignalTimeline) {
            SignalSems[NumSignals] = SignalTimeline;
            SignalValues[NumSignals] = SignalValue;
            NumSignals++;
        }
        const uint64_t DoneValue = P->Submitted + 1;
        SignalSems[NumSignals] = P->Done;
        SignalValues[NumSignals] = DoneValue;
        NumSignals++;

        VkTimelineSemaphoreSubmitInfo TL = {};
        TL.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        TL.waitSemaphoreValueCount = static_cast<uint32_t>(NumSources);
        TL.pWaitSemaphoreValues = WaitValues;
        TL.signalSemaphoreValueCount = NumSignals;
        TL.pSignalSemaphoreValues = SignalValues;

        VkSubmitInfo SI = {};
        SI.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        SI.pNext = &TL;
        SI.waitSemaphoreCount = static_cast<uint32_t>(NumSources);
        SI.pWaitSemaphores = WaitSems;
        SI.pWaitDstStageMask = WaitStages;
        SI.commandBufferCount = 1;
        SI.pCommandBuffers = &C.Cmd;
        SI.signalSemaphoreCount = NumSignals;
        SI.pSignalSemaphores = SignalSems;

        P->LockQueue();
        Res = P->VK.vkQueueSubmit(P->Queue, 1, &SI, VK_NULL_HANDLE);
        P->UnlockQueue();
        if (Res != VK_SUCCESS)
            ThrowVk("vkQueueSubmit", Res);
        P->Submitted = DoneValue;
        C.Value = DoneValue;

        /* The frames' new state, published the moment the submission that produces it is in
           flight and before anything waits on it, so the next user of a frame -- possibly a
           decoder thread already blocked on the lock -- orders against this work rather than
           against stale values. access must be 0 and not SHADER_READ: whatever touches the frame
           next uses it as srcAccessMask, and FFmpeg's transfer path runs on a transfer only queue
           where SHADER_READ is not a legal access. Zero is also correct in its own right, since
           availability operations exist for writes and this pass only reads. */
        for (int s = 0; s < NumSources; s++) {
            Vkf[s]->sem_value[0] = FrameSignalValues[s];
            Vkf[s]->layout[0] = VK_IMAGE_LAYOUT_GENERAL;
            Vkf[s]->access[0] = {};
        }
        UnlockFrames();

        /* Only the hash has to come back to the host. An export is complete for its consumer when
           the semaphores say so, and its context waits for that in the background. */
        if (!DoExport) {
            VkSemaphoreWaitInfo SWI = {};
            SWI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
            SWI.semaphoreCount = 1;
            SWI.pSemaphores = &P->Done;
            SWI.pValues = &C.Value;
            Res = P->VK.vkWaitSemaphores(P->Device, &SWI, UINT64_MAX);
            if (Res != VK_SUCCESS)
                ThrowVk("vkWaitSemaphores", Res);

            const uint32_t *Lanes = static_cast<const uint32_t *>(P->Acc.Mapped);
            Result = (static_cast<uint64_t>(Lanes[1]) << 32) | Lanes[0];
            P->RetireContext(C);
        }
    } catch (...) {
        UnlockFrames();
        /* Nothing submitted means nothing the GPU could still be using; a submitted context stays
           claimed and retires once the timeline says the work is done. */
        if (!C.Value)
            P->RetireContext(C);
        throw;
    }

    return Result;
}

uint64_t BSGpuHasher::HashFrame(const AVFrame *Frame) {
    if (!IsSupportedFrame(Frame))
        throw BestSourceHWDecoderException("GPU hashing: unsupported frame");
    const DispatchSource Source = { Frame, 0, 1 };
    std::lock_guard<std::mutex> Lock(P->Mutex);
    return P->RunDispatch(&Source, 1, 0, 0, nullptr, VK_NULL_HANDLE, 0);
}

void BSGpuHasher::ExportAsPlanarGPU(const AVFrame *Frame, int Width, int Height,
    const BSGpuPlaneTarget *Targets, VkSemaphore SignalTimeline, uint64_t SignalValue) {
    if (!IsSupportedFrame(Frame))
        throw BestSourceHWDecoderException("GPU export: unsupported frame");
    if (!Targets)
        throw BestSourceHWDecoderException("GPU export: no plane targets");
    const DispatchSource Source = { Frame, 0, 1 };
    std::lock_guard<std::mutex> Lock(P->Mutex);
    (void)P->RunDispatch(&Source, 1, Width, Height, Targets, SignalTimeline, SignalValue);
}

void BSGpuHasher::ExportMergedFieldsAsPlanarGPU(const AVFrame *EvenRows, const AVFrame *OddRows,
    int Width, int Height, const BSGpuPlaneTarget *Targets,
    VkSemaphore SignalTimeline, uint64_t SignalValue) {
    if (!IsSupportedFrame(EvenRows) || !IsSupportedFrame(OddRows))
        throw BestSourceHWDecoderException("GPU export: unsupported frame");
    if (!Targets)
        throw BestSourceHWDecoderException("GPU export: no plane targets");
    const DispatchSource Sources[2] = { { EvenRows, 0, 2 }, { OddRows, 1, 2 } };
    std::lock_guard<std::mutex> Lock(P->Mutex);
    (void)P->RunDispatch(Sources, 2, Width, Height, Targets, SignalTimeline, SignalValue);
}

void BSGpuHasher::FinishExports() {
    std::lock_guard<std::mutex> Lock(P->Mutex);
    P->FinishAll();
}

#else /* !BS_GPU */

/* Built without vulkan headers or without glslangValidator. Constructing one is an error rather
   than a silent no-op, and it is what turns the whole build option off: hardware decoding needs a
   hasher, so throwing here is how a source built without GPU support falls back to the CPU. */

struct BSGpuHasher::Impl {};

BSGpuHasher::BSGpuHasher(AVBufferRef *) {
    throw BestSourceHWDecoderException("GPU support was not compiled into this build");
}

BSGpuHasher::~BSGpuHasher() = default;

uint64_t BSGpuHasher::HashFrame(const AVFrame *) {
    throw BestSourceHWDecoderException("GPU support was not compiled into this build");
}

bool BSGpuHasher::IsSupportedFrame(const AVFrame *) {
    return false;
}

bool BSGpuHasher::IsSupportedSwFormat(int) {
    return false;
}

#endif
