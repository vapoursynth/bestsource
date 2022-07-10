//  Copyright (c) 2022 Fredrik Mellbin
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
    VSVideoFormat AlphaFormat = {};
    std::unique_ptr<BestVideoSource> V;
};

static const VSFrame *VS_CC BestVideoSourceGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    BestVideoSourceData *d = reinterpret_cast<BestVideoSourceData *>(instanceData);

    if (activationReason == arInitial) {
        VSFrame *Dst = nullptr;
        VSFrame *AlphaDst = nullptr;
        std::unique_ptr<BestVideoFrame> Src;
        try {
            Src.reset(d->V->GetFrame(n));
            if (!Src)
                throw VideoException("No frame returned for frame number " + std::to_string(n));
            Dst = vsapi->newVideoFrame(&d->VI.format, d->VI.width, d->VI.height, nullptr, core);
            uint8_t *DstPtrs[3] = {};
            ptrdiff_t DstStride[3] = {};

            for (int plane = 0; plane < d->VI.format.numPlanes; plane++) {
                DstPtrs[plane] = vsapi->getWritePtr(Dst, plane);
                DstStride[plane] = vsapi->getStride(Dst, plane);
            }

            ptrdiff_t AlphaStride = 0;
            if (Src->HasAlpha()) {
                AlphaDst = vsapi->newVideoFrame(&d->AlphaFormat, d->VI.width, d->VI.height, nullptr, core);
                AlphaStride = vsapi->getStride(AlphaDst, 0);
                vsapi->mapSetInt(vsapi->getFramePropertiesRW(AlphaDst), "_ColorRange", 0, maAppend);
            }

            if (!Src->ExportAsPlanar(DstPtrs, DstStride, AlphaDst ? vsapi->getWritePtr(AlphaDst, 0) : nullptr, AlphaStride)) {
                throw VideoException("Cannot export to planar format for frame " + std::to_string(n));
            }

        } catch (VideoException &e) {
            vsapi->freeFrame(Dst);
            vsapi->freeFrame(AlphaDst);
            vsapi->setFilterError(e.what(), frameCtx);
            return nullptr;
        }

        const VideoProperties &VP = d->V->GetVideoProperties();
        VSMap *Props = vsapi->getFramePropertiesRW(Dst);
        if (AlphaDst)
            vsapi->mapConsumeFrame(Props, "_Alpha", AlphaDst, maAppend);

        // Set AR variables
        if (VP.SAR.num > 0 && VP.SAR.den > 0) {
            vsapi->mapSetInt(Props, "_SARNum", VP.SAR.num, maAppend);
            vsapi->mapSetInt(Props, "_SARDen", VP.SAR.den, maAppend);
        }

        vsapi->mapSetInt(Props, "_Matrix", Src->Matrix, maAppend);
        vsapi->mapSetInt(Props, "_Primaries", Src->Primaries, maAppend);
        vsapi->mapSetInt(Props, "_Transfer", Src->Transfer, maAppend);
        if (Src->ChromaLocation > 0)
            vsapi->mapSetInt(Props, "_ChromaLocation", Src->ChromaLocation - 1, maAppend);

        if (Src->ColorRange == 1) // FIXME, ugly hardcoded ffmpeg constants
            vsapi->mapSetInt(Props, "_ColorRange", 1, maAppend);
        else if (Src->ColorRange == 2)
            vsapi->mapSetInt(Props, "_ColorRange", 0, maAppend);
        vsapi->mapSetData(Props, "_PictType", &Src->PictType, 1, dtUtf8, maAppend);

        // Set field information
        int FieldBased = 0;
        if (Src->InterlacedFrame)
            FieldBased = (Src->TopFieldFirst ? 2 : 1);
        vsapi->mapSetInt(Props, "_FieldBased", FieldBased, maAppend);

        if (Src->HasMasteringDisplayPrimaries) {
            for (int i = 0; i < 3; i++) {
                vsapi->mapSetFloat(Props, "MasteringDisplayPrimariesX", Src->MasteringDisplayPrimaries[i][0].ToDouble(), maAppend);
                vsapi->mapSetFloat(Props, "MasteringDisplayPrimariesY", Src->MasteringDisplayPrimaries[i][1].ToDouble(), maAppend);
            }
            vsapi->mapSetFloat(Props, "MasteringDisplayWhitePointX", Src->MasteringDisplayWhitePoint[0].ToDouble(), maAppend);
            vsapi->mapSetFloat(Props, "MasteringDisplayWhitePointY", Src->MasteringDisplayWhitePoint[1].ToDouble(), maAppend);
        }

        if (Src->HasMasteringDisplayLuminance) {
            vsapi->mapSetFloat(Props, "MasteringDisplayMinLuminance", Src->MasteringDisplayMinLuminance.ToDouble(), maAppend);
            vsapi->mapSetFloat(Props, "MasteringDisplayMaxLuminance", Src->MasteringDisplayMaxLuminance.ToDouble(), maAppend);
        }

        if (Src->HasContentLightLevel) {
            vsapi->mapSetInt(Props, "ContentLightLevelMax", Src->ContentLightLevelMax, maAppend);
            vsapi->mapSetInt(Props, "ContentLightLevelAverage", Src->ContentLightLevelAverage, maAppend);
        }

        if (Src->DolbyVisionRPU && Src->DolbyVisionRPUSize) {
            vsapi->mapSetData(Props, "DolbyVisionRPU", (const char *)Src->DolbyVisionRPU, Src->DolbyVisionRPUSize, dtBinary, maAppend);
        }

        vsapi->mapSetInt(Props, "FlipVertical", VP.FlipVerical, maAppend);
        vsapi->mapSetInt(Props, "FlipHorizontal", VP.FlipHorizontal, maAppend);
        vsapi->mapSetInt(Props, "Rotation", VP.Rotation, maAppend);

        return Dst;
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
    int SeekPreRoll = vsapi->mapGetIntSaturated(in, "seekpreroll", 0, &err);
    if (err)
        SeekPreRoll = 20;
    bool VariableFormat = !!vsapi->mapGetInt(in, "variableformat", 0, &err);
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
        D->V->SetSeekPreRoll(SeekPreRoll);
    } catch (VideoException &e) {
        delete D;
        vsapi->mapSetError(out, (std::string("BestVideoSource: ") + e.what()).c_str());
        return;
    }

    vsapi->queryVideoFormat(&D->AlphaFormat, cfGray, D->VI.format.sampleType, D->VI.format.bitsPerSample, 0, 0, core);

    vsapi->createVideoFilter(out, "Source", &D->VI, BestVideoSourceGetFrame, BestVideoSourceFree, fmUnordered, nullptr, 0, D, core);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    vspapi->configPlugin("com.vapoursynth.bestvideosource", "bvs", "Best Video Source", VS_MAKE_VERSION(0, 8), VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("Source", "source:data;track:int:opt;variableformat:int:opt;seekpreroll:int:opt;exactframes:int:opt;enable_drefs:int:opt;use_absolute_path:int:opt;", "clip:vnode;", CreateBestVideoSource, nullptr, plugin);
}
