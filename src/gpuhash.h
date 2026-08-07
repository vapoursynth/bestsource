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

#ifndef BSGPUHASH_H
#define BSGPUHASH_H

#include <cstdint>
#include <cstddef>
#include <memory>

/* The GPU frame export API is typed in terms of Vulkan objects, so it only exists in a build that
   has the headers. The class itself is declared either way so BestVideoSource holds the same
   members regardless, and adding a member function does not change its layout. */
#if BS_GPU_HASH
#include <vulkan/vulkan.h>
#endif

struct AVBufferRef;
struct AVFrame;

#if BS_GPU_HASH
/* Where one exported plane goes.
 *
 * Buffer must live on the device BSGpuHasher was constructed with. It is NOT a handle from another
 * device: Vulkan handles are per VkDevice, and a VkBuffer from another device is meaningless here
 * even when both devices come from the same VkPhysicalDevice. To send frames to another device the
 * caller exports that device's allocation as an OS handle, imports it into this one with
 * vkAllocateMemory, and creates a VkBuffer here bound to the imported memory.
 *
 * Planes usually share a single allocation, since an imported allocation is imported whole and
 * addressed by offset, so the same Buffer with three different Offsets is the expected case. */
struct BSGpuPlaneTarget {
    VkBuffer Buffer;
    uint64_t Offset;  /* bytes from the start of Buffer */
    ptrdiff_t Stride; /* bytes */
};
#endif

/*
Hashes Vulkan resident frames on the GPU, so hardware decoding does not have to read every frame
back over the bus purely to identify it.

Why this exists: the frame hash is not an optional check. It is computed for every output frame,
and after a seek it is the positioning mechanism -- SeekAndDecode matches a run of decoded hashes
against the index to work out which frame it is holding. With hardware decoding that currently
forces av_hwframe_transfer_data on every frame, including during indexing where the pixels are
discarded immediately afterwards.

The hash deliberately does not match the CPU XXH3 in videosource.cpp. It does not have to:
ReadCompareString(F, HWDevice) means an index built with hwdevice=vulkan is never consumed by a
software run, so the two never meet. What it must be is reproducible across GPUs and drivers,
since index files are portable -- hence integer only arithmetic, a fixed tile decomposition, and
an XOR combine, which is commutative and associative so scheduling order cannot affect the result.

Only nv12 and p010 style two plane semi-planar formats are supported, which is what FFmpeg's
Vulkan decoder produces.
*/
class BSGpuHasher {
public:
    /* HWDeviceContext must be an AV_HWDEVICE_TYPE_VULKAN device. Throws BestSourceException if
       the device cannot be used, which callers should treat the same as hardware decoding being
       unavailable. */
    explicit BSGpuHasher(AVBufferRef *HWDeviceContext);
    ~BSGpuHasher();

    BSGpuHasher(const BSGpuHasher &) = delete;
    BSGpuHasher &operator=(const BSGpuHasher &) = delete;

    /* Frame must be a Vulkan resident frame from the device this was constructed with. Blocks
       until the GPU has finished, then returns the hash. Also advances the frame's timeline
       semaphore and updates its layout and access bookkeeping, which is the caller's
       responsibility once an image has been transitioned. */
    [[nodiscard]] uint64_t HashFrame(const AVFrame *Frame);

#if BS_GPU_HASH
    /* Writes the frame's planes into Targets in VapourSynth's planar layout -- the interleaved
       chroma of an nv12/p010 decode output split into separate U and V planes, and the P010 family
       shifted from MSB to LSB alignment -- and returns the same hash HashFrame would.
       Both happen in one pass over the image, so the hash costs no additional bandwidth.

       Targets must have three entries, in Y U V order.

       SignalTimeline, when not VK_NULL_HANDLE, is signalled with SignalValue once the export
       completes, in addition to the frame's own semaphore. That is what lets a consumer on another
       device wait on the device rather than on the host; pass the semaphore it imported for this.

       Still blocks until the GPU is done, because the hash has to be read back to be returned. The
       signal is useful anyway: it is what the consumer's later work orders against. */
    [[nodiscard]] uint64_t ExportAsPlanarGPU(const AVFrame *Frame, const BSGpuPlaneTarget *Targets,
        VkSemaphore SignalTimeline, uint64_t SignalValue);
#endif

    /* Whether the frame's format and residency are something HashFrame can handle. */
    [[nodiscard]] static bool IsSupportedFrame(const AVFrame *Frame);

private:
    struct Impl;
    std::unique_ptr<Impl> P;
};

#endif
