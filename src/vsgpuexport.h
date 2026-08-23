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

#ifndef BSVSGPUEXPORT_H
#define BSVSGPUEXPORT_H

#include <cstdint>
#include <memory>
#include <string>

struct VSCore;
struct VSFrame;
struct VSAPI;
struct VSVideoFormat;
class BestVideoFrame;
class BestVideoSource;

/*
Publishes decoded frames into VapourSynth as GPU resident ones.

VapourSynth's core and FFmpeg's decoder run on separate VkDevices even when both sit on the same
physical GPU -- the core's header is explicit that sharing goes through exportGPUPlane rather than
through handing devices around, and Vulkan handles are per device regardless. So the frame is
allocated by VapourSynth, its backing allocation is exported as an OS handle, imported into the
device the decoder is on, and written there.

That direction is deliberate. VapourSynth can only put a frame in a graph if it allocated it, and
having the core own the memory keeps it inside the core's VRAM budgeting and eviction.

Imports are cached by the memoryId the core hands out, which is stable for an allocation's
lifetime and never reused, unlike the handle, which is fresh on every export call. Without the
cache an import per frame would cost more than the readback this exists to avoid.
*/
/* What QueryDevice learned about the core's device, carried to Create so the device is probed
   once rather than by both. Plain types only, so including this header never needs vulkan. */
struct BSVSGpuDeviceInfo {
    std::string DeviceName;
    uint8_t DeviceUUID[16]; /* VK_UUID_SIZE */
};

class BSVSGpuExport {
public:
    /* Which physical device the core is on, and whether it can share memory at all. Has to be
       asked before the source is constructed, because the answer is what pins the decoder to the
       same device through the hwdevice selector; the same answer is then handed to Create. Returns
       false with Error set when GPU output is unavailable, which is a reason to use CPU frames
       rather than to fail. */
    static bool QueryDevice(VSCore *Core, const VSAPI *vsapi, BSVSGpuDeviceInfo &Device, std::string &Error);

    /* Returns nullptr with Error set when GPU output cannot be used -- no Vulkan API in this core,
       no export support on the device, the decoder did not land on the same physical device, or the
       track holds a format the export pass cannot write. All of those are reasons to fall back to
       CPU frames rather than to fail.

       VariableFormat is the format set the caller will select, or -1 for all of them, and decides
       which formats have to be exportable. Checking that here rather than per frame is what keeps a
       format the shader cannot handle from surfacing after the node exists, at which point falling
       back is no longer possible. */
    static std::unique_ptr<BSVSGpuExport> Create(BestVideoSource *Source, int VariableFormat,
        const BSVSGpuDeviceInfo &Device, VSCore *Core, const VSAPI *vsapi, std::string &Error);

    ~BSVSGpuExport();
    BSVSGpuExport(const BSVSGpuExport &) = delete;
    BSVSGpuExport &operator=(const BSVSGpuExport &) = delete;

    /* The name of the device both sides are on, for the hwdevice selector and for diagnostics. */
    [[nodiscard]] const std::string &GetDeviceName() const;

    /* Allocates a GPU resident VapourSynth frame, has the decoder write the planes into it on the
       device, and publishes the producer pair so consumers wait on the GPU rather than the host.
       Throws BestSourceException on failure. */
    [[nodiscard]] VSFrame *ExportFrame(const BestVideoFrame *Src, const VSVideoFormat *Format,
        int Width, int Height, VSCore *Core, const VSAPI *vsapi);

private:
    BSVSGpuExport();
    struct Impl;
    std::unique_ptr<Impl> P;
};

#endif
