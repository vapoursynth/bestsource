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
#if BS_GPU_HASH

#include <mutex>
#include <string>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
}

/* Generated from src/shaders/hashexport.comp.glsl at build time; see tools/spv2c.py. */
#include "hashexport_spv.h"

/* Must match what the shader was compiled with; meson passes the same value to both. The default
   only exists so the file still compiles if it is ever built outside that rule. */
#ifndef BS_GPU_HASH_SAMPLES_X
#define BS_GPU_HASH_SAMPLES_X 8
#endif

namespace {

/* Every Vulkan entry point is loaded through AVVulkanDeviceContext::get_proc_addr rather than by
   linking the loader, so BestSource never needs to find libvulkan itself and is guaranteed to be
   using the same loader FFmpeg is. */
#define BS_VK_FUNCS(F)                     \
    F(vkGetPhysicalDeviceMemoryProperties) \
    F(vkGetDeviceQueue)                    \
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
    F(vkWaitSemaphores)

struct VulkanFunctions {
#define BS_DECL(n) PFN_##n n = nullptr;
    BS_VK_FUNCS(BS_DECL)
#undef BS_DECL
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

uint32_t FindMemoryType(const VkPhysicalDeviceMemoryProperties &Props, uint32_t TypeBits, VkMemoryPropertyFlags Want) {
    for (uint32_t i = 0; i < Props.memoryTypeCount; i++)
        if ((TypeBits & (1u << i)) && (Props.memoryTypes[i].propertyFlags & Want) == Want)
            return i;
    return UINT32_MAX;
}

void ThrowVk(const char *What, VkResult Res) {
    throw BestSourceException(std::string("GPU hashing: ") + What + " failed (VkResult " + std::to_string(static_cast<int>(Res)) + ")");
}

struct MappedBuffer {
    VkBuffer Buffer = VK_NULL_HANDLE;
    VkDeviceMemory Memory = VK_NULL_HANDLE;
    void *Mapped = nullptr;
};

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
    VkDescriptorSet DescSet = VK_NULL_HANDLE;
    VkPipelineLayout PipeLayout = VK_NULL_HANDLE;
    VkCommandPool CmdPool = VK_NULL_HANDLE;
    VkCommandBuffer Cmd = VK_NULL_HANDLE;

    /* One pipeline per sample size; created on first use of each. */
    VkShaderModule Module[2] = {};
    VkPipeline Pipeline[2] = {};

    MappedBuffer Acc, Status, Dummy[3];

    /* HashFrame submits to a queue FFmpeg may also be using and mutates shared descriptor and
       command buffer state, so calls are serialized. Nothing is lost by it while the
       implementation is synchronous. */
    std::mutex Mutex;

    ~Impl();
    void CreateBuffer(VkDeviceSize Size, MappedBuffer &Out);
    void DestroyBuffer(MappedBuffer &B);
    VkPipeline GetPipeline(int BytesPerSample);
    void LockQueue();
    void UnlockQueue();
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
    uint32_t Type = FindMemoryType(MemProps, Req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (Type == UINT32_MAX)
        throw BestSourceException("GPU hashing: no host visible coherent memory type");

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

VkPipeline BSGpuHasher::Impl::GetPipeline(int BytesPerSample) {
    const int Slot = (BytesPerSample == 2) ? 1 : 0;
    if (Pipeline[Slot])
        return Pipeline[Slot];

    const uint32_t *Code = Slot ? BSHashExportSpv16 : BSHashExportSpv8;
    const size_t Size = Slot ? sizeof(BSHashExportSpv16) : sizeof(BSHashExportSpv8);

    VkShaderModuleCreateInfo SMCI = {};
    SMCI.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    SMCI.codeSize = Size;
    SMCI.pCode = Code;
    VkResult Res = VK.vkCreateShaderModule(Device, &SMCI, HWCtx->alloc, &Module[Slot]);
    if (Res != VK_SUCCESS)
        ThrowVk("vkCreateShaderModule", Res);

    /* do_export = 0. The plane writing half of the shader belongs to GPU frame output, which is
       a later step; hashing alone is what removes the readback from indexing. */
    const uint32_t DoExport = 0;
    VkSpecializationMapEntry Entry = { 0, 0, sizeof(uint32_t) };
    VkSpecializationInfo Spec = { 1, &Entry, sizeof(uint32_t), &DoExport };

    VkComputePipelineCreateInfo CPCI = {};
    CPCI.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    CPCI.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    CPCI.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    CPCI.stage.module = Module[Slot];
    CPCI.stage.pName = "main";
    CPCI.stage.pSpecializationInfo = &Spec;
    CPCI.layout = PipeLayout;
    Res = VK.vkCreateComputePipelines(Device, VK_NULL_HANDLE, 1, &CPCI, HWCtx->alloc, &Pipeline[Slot]);
    if (Res != VK_SUCCESS)
        ThrowVk("vkCreateComputePipelines", Res);
    return Pipeline[Slot];
}

/* FFmpeg hands out a queue lock because its own submissions share these queues. It is deprecated
   in favour of VK_KHR_internally_synchronized_queues but still the only portable option. */
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

BSGpuHasher::Impl::~Impl() {
    if (Device) {
        for (int i = 0; i < 2; i++) {
            if (Pipeline[i])
                VK.vkDestroyPipeline(Device, Pipeline[i], HWCtx->alloc);
            if (Module[i])
                VK.vkDestroyShaderModule(Device, Module[i], HWCtx->alloc);
        }
        if (CmdPool)
            VK.vkDestroyCommandPool(Device, CmdPool, HWCtx->alloc);
        if (PipeLayout)
            VK.vkDestroyPipelineLayout(Device, PipeLayout, HWCtx->alloc);
        if (DescPool)
            VK.vkDestroyDescriptorPool(Device, DescPool, HWCtx->alloc);
        if (SetLayout)
            VK.vkDestroyDescriptorSetLayout(Device, SetLayout, HWCtx->alloc);
        DestroyBuffer(Acc);
        DestroyBuffer(Status);
        for (auto &D : Dummy)
            DestroyBuffer(D);
    }
    av_buffer_unref(&DeviceRef);
}

bool BSGpuHasher::IsSupportedFrame(const AVFrame *Frame) {
    if (!Frame || Frame->format != AV_PIX_FMT_VULKAN || !Frame->hw_frames_ctx)
        return false;
    const AVHWFramesContext *Frames = reinterpret_cast<const AVHWFramesContext *>(Frame->hw_frames_ctx->data);
    if (av_pix_fmt_count_planes(Frames->sw_format) != 2)
        return false;
    const VkFormat *PlaneFmts = av_vkfmt_from_pixfmt(Frames->sw_format);
    return PlaneFmts && UintViewFormat(PlaneFmts[0]) != VK_FORMAT_UNDEFINED &&
        UintViewFormat(PlaneFmts[1]) != VK_FORMAT_UNDEFINED;
}

BSGpuHasher::BSGpuHasher(AVBufferRef *HWDeviceContext) : P(new Impl) {
    if (!HWDeviceContext)
        throw BestSourceException("GPU hashing: no hardware device context");

    AVHWDeviceContext *Ctx = reinterpret_cast<AVHWDeviceContext *>(HWDeviceContext->data);
    if (Ctx->type != AV_HWDEVICE_TYPE_VULKAN)
        throw BestSourceException("GPU hashing: device is not vulkan");

    P->DeviceRef = av_buffer_ref(HWDeviceContext);
    if (!P->DeviceRef)
        throw BestSourceException("GPU hashing: couldn't reference device");
    P->HWCtx = reinterpret_cast<AVVulkanDeviceContext *>(Ctx->hwctx);
    P->Device = P->HWCtx->act_dev;

    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        P->HWCtx->get_proc_addr(P->HWCtx->inst, "vkGetDeviceProcAddr"));
    if (!GetDeviceProcAddr)
        throw BestSourceException("GPU hashing: couldn't load vkGetDeviceProcAddr");

    P->VK.vkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        P->HWCtx->get_proc_addr(P->HWCtx->inst, "vkGetPhysicalDeviceMemoryProperties"));

#define BS_LOAD(n) if (!P->VK.n) P->VK.n = reinterpret_cast<PFN_##n>(GetDeviceProcAddr(P->Device, #n));
    BS_VK_FUNCS(BS_LOAD)
#undef BS_LOAD
#define BS_CHECK(n) if (!P->VK.n) throw BestSourceException("GPU hashing: missing entry point " #n);
    BS_VK_FUNCS(BS_CHECK)
#undef BS_CHECK

    P->VK.vkGetPhysicalDeviceMemoryProperties(P->HWCtx->phys_dev, &P->MemProps);

    /* Deliberately not gated on device type. On unified memory devices av_hwframe_transfer_data is
       a memory copy rather than a bus transfer, so the readback this avoids is free there and GPU
       hashing is a small net loss -- measured at 1080p on an AMD APU, 0.061 ms/frame against a
       1.64 ms decode. Refusing those devices would make the hash algorithm depend on which GPU was
       picked, so on a machine with both an integrated and a discrete adapter an index built on one
       would be rejected and rebuilt on the other. Uniform behaviour is worth more than the few
       percent, and hardware decoding on slow graphics is a recommendation rather than a library
       policy. */

    for (int i = 0; i < P->HWCtx->nb_qf; i++) {
        if (P->HWCtx->qf[i].flags & VK_QUEUE_COMPUTE_BIT) {
            P->QueueFamilyListIndex = i;
            P->QueueFamily = static_cast<uint32_t>(P->HWCtx->qf[i].idx);
            break;
        }
    }
    if (P->QueueFamilyListIndex < 0)
        throw BestSourceException("GPU hashing: device has no compute queue family");
    P->VK.vkGetDeviceQueue(P->Device, P->QueueFamily, 0, &P->Queue);

    P->CreateBuffer(2 * sizeof(uint32_t), P->Acc);
    P->CreateBuffer(sizeof(uint32_t), P->Status);
    for (auto &D : P->Dummy)
        P->CreateBuffer(64, D);

    /* Bindings 0-1 are the plane images, 2-4 the export destinations, 5 the hash accumulator and
       6 the status word. The export destinations are unused with do_export = 0 but still have to
       be bound, since the shader declares them. */
    VkDescriptorSetLayoutBinding Bindings[7] = {};
    for (int i = 0; i < 7; i++) {
        Bindings[i].binding = i;
        Bindings[i].descriptorCount = 1;
        Bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        Bindings[i].descriptorType = (i < 2) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    }
    VkDescriptorSetLayoutCreateInfo DSLCI = {};
    DSLCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    DSLCI.bindingCount = 7;
    DSLCI.pBindings = Bindings;
    VkResult Res = P->VK.vkCreateDescriptorSetLayout(P->Device, &DSLCI, P->HWCtx->alloc, &P->SetLayout);
    if (Res != VK_SUCCESS)
        ThrowVk("vkCreateDescriptorSetLayout", Res);

    VkDescriptorPoolSize PoolSizes[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5 },
    };
    VkDescriptorPoolCreateInfo DPCI = {};
    DPCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    DPCI.maxSets = 1;
    DPCI.poolSizeCount = 2;
    DPCI.pPoolSizes = PoolSizes;
    Res = P->VK.vkCreateDescriptorPool(P->Device, &DPCI, P->HWCtx->alloc, &P->DescPool);
    if (Res != VK_SUCCESS)
        ThrowVk("vkCreateDescriptorPool", Res);

    VkDescriptorSetAllocateInfo DSAI = {};
    DSAI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    DSAI.descriptorPool = P->DescPool;
    DSAI.descriptorSetCount = 1;
    DSAI.pSetLayouts = &P->SetLayout;
    Res = P->VK.vkAllocateDescriptorSets(P->Device, &DSAI, &P->DescSet);
    if (Res != VK_SUCCESS)
        ThrowVk("vkAllocateDescriptorSets", Res);

    /* The buffer half of the descriptor set never changes; only the two images do, per frame. */
    VkBuffer BufOrder[5] = { P->Dummy[0].Buffer, P->Dummy[1].Buffer, P->Dummy[2].Buffer, P->Acc.Buffer, P->Status.Buffer };
    VkDescriptorBufferInfo BufInfo[5] = {};
    VkWriteDescriptorSet Writes[5] = {};
    for (int i = 0; i < 5; i++) {
        BufInfo[i].buffer = BufOrder[i];
        BufInfo[i].range = VK_WHOLE_SIZE;
        Writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        Writes[i].dstSet = P->DescSet;
        Writes[i].dstBinding = i + 2;
        Writes[i].descriptorCount = 1;
        Writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        Writes[i].pBufferInfo = &BufInfo[i];
    }
    P->VK.vkUpdateDescriptorSets(P->Device, 5, Writes, 0, nullptr);

    VkPushConstantRange PCR = {};
    PCR.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    PCR.size = 6 * sizeof(int32_t);
    VkPipelineLayoutCreateInfo PLCI = {};
    PLCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    PLCI.setLayoutCount = 1;
    PLCI.pSetLayouts = &P->SetLayout;
    PLCI.pushConstantRangeCount = 1;
    PLCI.pPushConstantRanges = &PCR;
    Res = P->VK.vkCreatePipelineLayout(P->Device, &PLCI, P->HWCtx->alloc, &P->PipeLayout);
    if (Res != VK_SUCCESS)
        ThrowVk("vkCreatePipelineLayout", Res);

    VkCommandPoolCreateInfo CPCI = {};
    CPCI.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    CPCI.queueFamilyIndex = P->QueueFamily;
    Res = P->VK.vkCreateCommandPool(P->Device, &CPCI, P->HWCtx->alloc, &P->CmdPool);
    if (Res != VK_SUCCESS)
        ThrowVk("vkCreateCommandPool", Res);

    VkCommandBufferAllocateInfo CBAI = {};
    CBAI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    CBAI.commandPool = P->CmdPool;
    CBAI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    CBAI.commandBufferCount = 1;
    Res = P->VK.vkAllocateCommandBuffers(P->Device, &CBAI, &P->Cmd);
    if (Res != VK_SUCCESS)
        ThrowVk("vkAllocateCommandBuffers", Res);
}

BSGpuHasher::~BSGpuHasher() = default;

uint64_t BSGpuHasher::HashFrame(AVFrame *Frame) {
    if (!IsSupportedFrame(Frame))
        throw BestSourceException("GPU hashing: unsupported frame");

    std::lock_guard<std::mutex> Lock(P->Mutex);

    AVHWFramesContext *Frames = reinterpret_cast<AVHWFramesContext *>(Frame->hw_frames_ctx->data);
    AVVkFrame *Vkf = reinterpret_cast<AVVkFrame *>(Frame->data[0]);
    const AVPixFmtDescriptor *Desc = av_pix_fmt_desc_get(Frames->sw_format);
    const int BytesPerSample = (Desc->comp[0].depth > 8) ? 2 : 1;
    const VkFormat *PlaneFmts = av_vkfmt_from_pixfmt(Frames->sw_format);

    struct { int32_t LumaW, LumaH, ChromaW, ChromaH, StrideY, StrideUV; } PC = {
        Frame->width, Frame->height,
        AV_CEIL_RSHIFT(Frame->width, Desc->log2_chroma_w),
        AV_CEIL_RSHIFT(Frame->height, Desc->log2_chroma_h),
        0, 0
    };

    /* Views are created per call. FFmpeg's frame pool recycles a bounded set of images so caching
       them by VkImage would pay off, but a cached view outliving its image after a decoder reset
       is a worse bug than the allocation is a cost. */
    VkImageView Views[2] = {};
    const VkImageAspectFlagBits Aspects[2] = { VK_IMAGE_ASPECT_PLANE_0_BIT, VK_IMAGE_ASPECT_PLANE_1_BIT };
    for (int p = 0; p < 2; p++) {
        VkImageViewUsageCreateInfo Usage = {};
        Usage.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO;
        Usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        VkImageViewCreateInfo CI = {};
        CI.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        CI.pNext = &Usage;
        CI.image = Vkf->img[0];
        CI.viewType = VK_IMAGE_VIEW_TYPE_2D;
        CI.format = UintViewFormat(PlaneFmts[p]);
        CI.subresourceRange.aspectMask = Aspects[p];
        CI.subresourceRange.levelCount = 1;
        CI.subresourceRange.layerCount = 1;
        VkResult Res = P->VK.vkCreateImageView(P->Device, &CI, P->HWCtx->alloc, &Views[p]);
        if (Res != VK_SUCCESS) {
            for (int q = 0; q < p; q++)
                P->VK.vkDestroyImageView(P->Device, Views[q], P->HWCtx->alloc);
            ThrowVk("vkCreateImageView", Res);
        }
    }

    uint64_t Result = 0;
    try {
        VkDescriptorImageInfo ImgInfo[2] = {};
        VkWriteDescriptorSet Writes[2] = {};
        for (int p = 0; p < 2; p++) {
            ImgInfo[p].imageView = Views[p];
            ImgInfo[p].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            Writes[p].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            Writes[p].dstSet = P->DescSet;
            Writes[p].dstBinding = p;
            Writes[p].descriptorCount = 1;
            Writes[p].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            Writes[p].pImageInfo = &ImgInfo[p];
        }
        P->VK.vkUpdateDescriptorSets(P->Device, 2, Writes, 0, nullptr);

        VkPipeline Pipe = P->GetPipeline(BytesPerSample);

        VkResult Res = P->VK.vkResetCommandPool(P->Device, P->CmdPool, 0);
        if (Res != VK_SUCCESS)
            ThrowVk("vkResetCommandPool", Res);

        VkCommandBufferBeginInfo BI = {};
        BI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        BI.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Res = P->VK.vkBeginCommandBuffer(P->Cmd, &BI);
        if (Res != VK_SUCCESS)
            ThrowVk("vkBeginCommandBuffer", Res);

        P->VK.vkCmdFillBuffer(P->Cmd, P->Acc.Buffer, 0, VK_WHOLE_SIZE, 0);

        /* Frames arrive in VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR; a compute read needs GENERAL.
           One barrier covers both planes. queue_family is VK_QUEUE_FAMILY_IGNORED because FFmpeg
           allocates CONCURRENT, so there is no ownership transfer to perform. */
        VkImageMemoryBarrier ImgBar = {};
        ImgBar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ImgBar.srcAccessMask = static_cast<VkAccessFlags>(Vkf->access[0]);
        ImgBar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        ImgBar.oldLayout = Vkf->layout[0];
        ImgBar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ImgBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ImgBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ImgBar.image = Vkf->img[0];
        ImgBar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT;
        ImgBar.subresourceRange.levelCount = 1;
        ImgBar.subresourceRange.layerCount = 1;

        VkBufferMemoryBarrier FillBar = {};
        FillBar.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        FillBar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        FillBar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        FillBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        FillBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        FillBar.buffer = P->Acc.Buffer;
        FillBar.size = VK_WHOLE_SIZE;

        P->VK.vkCmdPipelineBarrier(P->Cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &FillBar, 1, &ImgBar);

        P->VK.vkCmdBindPipeline(P->Cmd, VK_PIPELINE_BIND_POINT_COMPUTE, Pipe);
        P->VK.vkCmdBindDescriptorSets(P->Cmd, VK_PIPELINE_BIND_POINT_COMPUTE, P->PipeLayout, 0, 1, &P->DescSet, 0, nullptr);
        P->VK.vkCmdPushConstants(P->Cmd, P->PipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PC), &PC);
        /* Each workgroup covers 16 invocations of BS_GPU_HASH_SAMPLES_X samples along x. */
        const int XPerGroup = 16 * BS_GPU_HASH_SAMPLES_X;
        P->VK.vkCmdDispatch(P->Cmd, (Frame->width + XPerGroup - 1) / XPerGroup,
            (Frame->height + 15) / 16, 2);

        VkBufferMemoryBarrier HostBar = FillBar;
        HostBar.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        HostBar.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        P->VK.vkCmdPipelineBarrier(P->Cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &HostBar, 0, nullptr);

        Res = P->VK.vkEndCommandBuffer(P->Cmd);
        if (Res != VK_SUCCESS)
            ThrowVk("vkEndCommandBuffer", Res);

        /* AVVkFrame's timeline contract: wait on sem_value, signal an incremented value. */
        const uint64_t WaitValue = Vkf->sem_value[0];
        const uint64_t SignalValue = WaitValue + 1;
        VkPipelineStageFlags WaitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

        VkTimelineSemaphoreSubmitInfo TL = {};
        TL.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        TL.waitSemaphoreValueCount = 1;
        TL.pWaitSemaphoreValues = &WaitValue;
        TL.signalSemaphoreValueCount = 1;
        TL.pSignalSemaphoreValues = &SignalValue;

        VkSubmitInfo SI = {};
        SI.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        SI.pNext = &TL;
        SI.waitSemaphoreCount = 1;
        SI.pWaitSemaphores = &Vkf->sem[0];
        SI.pWaitDstStageMask = &WaitStage;
        SI.commandBufferCount = 1;
        SI.pCommandBuffers = &P->Cmd;
        SI.signalSemaphoreCount = 1;
        SI.pSignalSemaphores = &Vkf->sem[0];

        P->LockQueue();
        Res = P->VK.vkQueueSubmit(P->Queue, 1, &SI, VK_NULL_HANDLE);
        P->UnlockQueue();
        if (Res != VK_SUCCESS)
            ThrowVk("vkQueueSubmit", Res);

        VkSemaphoreWaitInfo SWI = {};
        SWI.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        SWI.semaphoreCount = 1;
        SWI.pSemaphores = &Vkf->sem[0];
        SWI.pValues = &SignalValue;
        Res = P->VK.vkWaitSemaphores(P->Device, &SWI, UINT64_MAX);
        if (Res != VK_SUCCESS)
            ThrowVk("vkWaitSemaphores", Res);

        /* Bookkeeping the caller owns once an image has been transitioned. access must be 0 and
           not SHADER_READ: whatever touches the frame next uses it as srcAccessMask, and FFmpeg's
           transfer path runs on a transfer only queue where SHADER_READ is not a legal access.
           Zero is also correct in its own right, since availability operations exist for writes
           and this pass only reads. */
        Vkf->sem_value[0] = SignalValue;
        Vkf->layout[0] = VK_IMAGE_LAYOUT_GENERAL;
        Vkf->access[0] = {};

        const uint32_t *Lanes = static_cast<const uint32_t *>(P->Acc.Mapped);
        Result = (static_cast<uint64_t>(Lanes[1]) << 32) | Lanes[0];
    } catch (...) {
        for (auto &V : Views)
            P->VK.vkDestroyImageView(P->Device, V, P->HWCtx->alloc);
        throw;
    }

    for (auto &V : Views)
        P->VK.vkDestroyImageView(P->Device, V, P->HWCtx->alloc);

    return Result;
}

#else /* !BS_GPU_HASH */

/* Built without vulkan headers or without glslangValidator. Constructing one is an error rather
   than a silent no-op: callers decide whether to use GPU hashing by catching this, the same way
   they decide about hardware decoding itself. */

struct BSGpuHasher::Impl {};

BSGpuHasher::BSGpuHasher(AVBufferRef *) {
    throw BestSourceException("GPU hashing was not compiled into this build");
}

BSGpuHasher::~BSGpuHasher() = default;

uint64_t BSGpuHasher::HashFrame(AVFrame *) {
    throw BestSourceException("GPU hashing was not compiled into this build");
}

bool BSGpuHasher::IsSupportedFrame(const AVFrame *) {
    return false;
}

#endif
