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
#include <memory>

struct AVBufferRef;
struct AVFrame;

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
    [[nodiscard]] uint64_t HashFrame(AVFrame *Frame);

    /* Whether the frame's format and residency are something HashFrame can handle. */
    [[nodiscard]] static bool IsSupportedFrame(const AVFrame *Frame);

private:
    struct Impl;
    std::unique_ptr<Impl> P;
};

#endif
