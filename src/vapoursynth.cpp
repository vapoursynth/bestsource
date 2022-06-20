//  Copyright (c) 2020-2022 Fredrik Mellbin
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

#include "videosource.h"
#include <VapourSynth4.h>
#include <VSHelper4.h>
#include <vector>
#include <algorithm>
#include <memory>
#include <limits>
#include <string>

struct BestVideoSourceData {
    VSVideoInfo VI = {};
    std::unique_ptr<BestVideoSource> V;
};

static const VSFrame *VS_CC BestVideoSourceGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    BestVideoSourceData *d = reinterpret_cast<BestVideoSourceData *>(instanceData);

    if (activationReason == arInitial) {
        VSFrame *dst = nullptr;
        try {
            std::unique_ptr<BestVideoFrame> src(d->V->GetFrame(n));
            if (!src)
                throw VideoException("No frame returned for frame number " + std::to_string(n));
            dst = vsapi->newVideoFrame(&d->VI.format, d->VI.width, d->VI.height, nullptr, core);
            uint8_t *dstPtrs[3] = {};
            ptrdiff_t dstStride[3] = {};

            for (int plane = 0; plane < d->VI.format.numPlanes; plane++) {
                dstPtrs[plane] = vsapi->getWritePtr(dst, plane);
                dstStride[plane] = vsapi->getStride(dst, plane);
            }

            if (!src->ExportAsPlanar(dstPtrs, dstStride)) {
                throw VideoException("Cannot export to planar format for frame " + std::to_string(n));
            }

        } catch (VideoException &e) {
            vsapi->freeFrame(dst);
            vsapi->setFilterError(e.what(), frameCtx);
            return nullptr;
        }

        return dst;
    }

    return nullptr;
}

static void VS_CC BestVideoSourceFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    delete reinterpret_cast<BestVideoSourceData *>(instanceData);
}

static void VS_CC CreateBestVideoSource(const VSMap *in, VSMap *out, void *, VSCore *core, const VSAPI *vsapi) {
    int err;
    const char *Source = vsapi->mapGetData(in, "source", 0, nullptr);
    int Track = vsapi->mapGetIntSaturated(in, "track", 0, &err);
    if (err)
        Track = -1;
    bool VariableFormat = vsapi->mapGetIntSaturated(in, "variableformat", 0, &err);
    bool ExactFrames = !!vsapi->mapGetInt(in, "exactframes", 0, &err);

    FFmpegOptions opts;
    opts.enable_drefs = !!vsapi->mapGetInt(in, "enable_drefs", 0, &err);
    opts.use_absolute_path = !!vsapi->mapGetInt(in, "use_absolute_path", 0, &err);

    BestVideoSourceData *D = new BestVideoSourceData();

    try {
        D->V.reset(new BestVideoSource(Source, Track, VariableFormat, 0, &opts));
        if (ExactFrames)
            D->V->GetExactDuration();
        const VideoProperties &VP = D->V->GetVideoProperties();
        
        if (VP.VF.ColorFamily == 0 || !vsapi->queryVideoFormat(&D->VI.format, VP.VF.ColorFamily, VP.VF.Float, VP.VF.Bits, VP.VF.SubSamplingW, VP.VF.SubSamplingH, core))
            throw VideoException("Unsupported video format from decoder (probably less than 8 bit or pallette)");
        D->VI.numFrames = vsh::int64ToIntS(VP.NumFrames);
        D->VI.width = VP.Width;
        D->VI.height = VP.Height;
        D->VI.fpsNum = VP.FPS.num;
        D->VI.fpsDen = VP.FPS.den;
    } catch (VideoException &e) {
        delete D;
        vsapi->mapSetError(out, (std::string("BestVideoSource: ") + e.what()).c_str());
        return;
    }

    vsapi->createVideoFilter(out, "Source", &D->VI, BestVideoSourceGetFrame, BestVideoSourceFree, fmUnordered, nullptr, 0, D, core);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    vspapi->configPlugin("com.vapoursynth.bestvideosource", "bvs", "Best Video Source", VS_MAKE_VERSION(0, 8), VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("Source", "source:data;track:int:opt;variableformat:int:opt;exactframes:int:opt;enable_drefs:int:opt;use_absolute_path:int:opt;", "clip:vnode;", CreateBestVideoSource, nullptr, plugin);
}
