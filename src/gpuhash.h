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
#if BS_GPU
#include <vulkan/vulkan.h>
#endif

struct AVBufferRef;
struct AVFrame;

#if BS_GPU
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

The hash deliberately does not match the CPU XXH3 in videosource.cpp. It does not have to: the
index header records whether it was written by a GPU decode (WriteInt/ReadCompareInt on the GPU
flag), so an index built with gpu=True is never consumed by a software run and the two hash
algorithms never meet. What it must be is reproducible across GPUs and drivers,
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

#if BS_GPU
    /* Writes the frame's planes into Targets in VapourSynth's planar layout -- the interleaved
       chroma of an nv12/p010 decode output split into separate U and V planes, and the P010 family
       shifted from MSB to LSB alignment.

       Width and Height are the destination's luma extent and bound every write; the chroma extent
       follows from the format's subsampling. They exist because the two sizes legitimately differ:
       a decoder rounds odd dimensions up to the subsampling grid while planar output crops them
       down to it, so exporting by the decoded size would write one row and column past every plane
       of an odd sized destination. Must be positive and no larger than the decoded frame.

       Targets must have three entries, in Y U V order.

       Deliberately does not hash: every frame is hashed by HashFrame at decode time, before anything
       can ask for its pixels, so a hash computed here would be a value nobody reads.

       SignalTimeline, when not VK_NULL_HANDLE, is signalled with SignalValue once the export
       completes, in addition to the frame's own semaphore. That is what lets a consumer on another
       device wait on the device rather than on the host; pass the semaphore it imported for this.

       Still blocks until the GPU is done, because the image views this creates cannot be destroyed
       before the dispatch that reads them has finished. The signal is what the consumer's later work
       orders against regardless. */
    void ExportAsPlanarGPU(const AVFrame *Frame, int Width, int Height, const BSGpuPlaneTarget *Targets,
        VkSemaphore SignalTimeline, uint64_t SignalValue);

    /* The RFF form: writes the even rows of the output from EvenRows and the odd rows from OddRows,
       which is what MergeField produces on the CPU. The two frames must agree on format and size.

       One submission, two dispatches, writing disjoint rows of the same destination. Neither source
       is modified, which is the point -- on a device resident frame the images belong to FFmpeg's
       pool and merging into one of them would corrupt it for every other holder. */
    void ExportMergedFieldsAsPlanarGPU(const AVFrame *EvenRows, const AVFrame *OddRows,
        int Width, int Height, const BSGpuPlaneTarget *Targets,
        VkSemaphore SignalTimeline, uint64_t SignalValue);
#endif

    /* Whether the frame's format and residency are something HashFrame can handle. */
    [[nodiscard]] static bool IsSupportedFrame(const AVFrame *Frame);

    /* The format half of that test, for a software pixel format on its own. Lets a caller reject a
       source before decoding rather than discovering it one frame at a time, which is the difference
       between falling back to the CPU and failing halfway through a script. Takes an int so the
       header does not drag in libavutil; pass an AVPixelFormat. */
    [[nodiscard]] static bool IsSupportedSwFormat(int PixelFormat);

private:
    struct Impl;
    std::unique_ptr<Impl> P;
};

#endif
