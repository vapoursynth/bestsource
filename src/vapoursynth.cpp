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
#include "audiosource.h"
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
        VSFrame *Dst = nullptr;
        VSFrame *AlphaDst = nullptr;
        std::unique_ptr<BestVideoFrame> Src;
        try {
            Src.reset(d->V->GetFrame(n));
            if (!Src)
                throw VideoException("No frame returned for frame number " + std::to_string(n));

            VSVideoFormat VideoFormat = {};
            vsapi->queryVideoFormat(&VideoFormat, Src->VF.ColorFamily, Src->VF.Float ? stFloat : stInteger, Src->VF.Bits, Src->VF.SubSamplingW, Src->VF.SubSamplingH, core);
            VSVideoFormat AlphaFormat = {};
            vsapi->queryVideoFormat(&AlphaFormat, cfGray, VideoFormat.sampleType, VideoFormat.bitsPerSample, 0, 0, core);

            Dst = vsapi->newVideoFrame(&VideoFormat, Src->Width, Src->Height, nullptr, core);
            uint8_t *DstPtrs[3] = {};
            ptrdiff_t DstStride[3] = {};

            for (int plane = 0; plane < VideoFormat.numPlanes; plane++) {
                DstPtrs[plane] = vsapi->getWritePtr(Dst, plane);
                DstStride[plane] = vsapi->getStride(Dst, plane);
            }

            ptrdiff_t AlphaStride = 0;
            if (Src->HasAlpha()) {
                AlphaDst = vsapi->newVideoFrame(&AlphaFormat, Src->Width, Src->Height, nullptr, core);
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

        if (Src->ColorRange == 1) // Hardcoded ffmpeg constants, nothing to see here
            vsapi->mapSetInt(Props, "_ColorRange", 1, maAppend);
        else if (Src->ColorRange == 2)
            vsapi->mapSetInt(Props, "_ColorRange", 0, maAppend);
        vsapi->mapSetData(Props, "_PictType", &Src->PictType, 1, dtUtf8, maAppend);

        // Set field information
        int FieldBased = 0;
        if (Src->InterlacedFrame)
            FieldBased = (Src->TopFieldFirst ? 2 : 1);
        vsapi->mapSetInt(Props, "_FieldBased", FieldBased, maAppend);

        // FIXME, missing frame duration/absolute time

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
    if (err)
        ExactFrames = true;

    std::map<std::string, std::string> Opts;
    Opts["enable_drefs"] = vsapi->mapGetInt(in, "enable_drefs", 0, &err) ? "1" : "0";
    Opts["use_absolute_path"] = vsapi->mapGetInt(in, "use_absolute_path", 0, &err) ? "1" : "0";

    BestVideoSourceData *D = new BestVideoSourceData();

    try {
        D->V.reset(new BestVideoSource(Source, Track, VariableFormat, 0, &Opts));
        if (ExactFrames)
            D->V->GetExactDuration();
        const VideoProperties &VP = D->V->GetVideoProperties();
        if (VP.VF.ColorFamily == 0 || !vsapi->queryVideoFormat(&D->VI.format, VP.VF.ColorFamily, VP.VF.Float, VP.VF.Bits, VP.VF.SubSamplingW, VP.VF.SubSamplingH, core))
            throw VideoException("Unsupported video format from decoder (probably less than 8 bit or pallette)");
        D->VI.width = VP.Width;
        D->VI.height = VP.Height;
        if (VariableFormat)
            D->VI = {};
        D->VI.numFrames = vsh::int64ToIntS(VP.NumFrames);
        D->VI.fpsNum = VP.FPS.num;
        D->VI.fpsDen = VP.FPS.den;
        vsh::reduceRational(&D->VI.fpsNum, &D->VI.fpsDen);
        D->V->SetSeekPreRoll(SeekPreRoll);
    } catch (VideoException &e) {
        delete D;
        vsapi->mapSetError(out, (std::string("VideoSource: ") + e.what()).c_str());
        return;
    }

    vsapi->createVideoFilter(out, "VideoSource", &D->VI, BestVideoSourceGetFrame, BestVideoSourceFree, fmUnordered, nullptr, 0, D, core);
}

struct BestAudioSourceData {
    VSAudioInfo AI = {};
    std::unique_ptr<BestAudioSource> A;
};

static const VSFrame *VS_CC BestAudioSourceGetFrame(int n, int activationReason, void *instanceData, void **frameData, VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi) {
    BestAudioSourceData *d = reinterpret_cast<BestAudioSourceData *>(instanceData);

    if (activationReason == arInitial) {
        int64_t samplesOut = std::min<int64_t>(VS_AUDIO_FRAME_SAMPLES, d->AI.numSamples - n * static_cast<int64_t>(VS_AUDIO_FRAME_SAMPLES));
        VSFrame *f = vsapi->newAudioFrame(&d->AI.format, static_cast<int>(samplesOut), nullptr, core);

        std::vector<uint8_t *> tmp;
        tmp.reserve(d->AI.format.numChannels);
        for (int p = 0; p < d->AI.format.numChannels; p++)
            tmp.push_back(vsapi->getWritePtr(f, p));
        try {
            d->A->GetAudio(tmp.data(), n * static_cast<int64_t>(VS_AUDIO_FRAME_SAMPLES), samplesOut);
        } catch (AudioException &e) {
            vsapi->setFilterError(e.what(), frameCtx);
            vsapi->freeFrame(f);
            return nullptr;
        }
        return f;
    }

    return nullptr;
}

static void VS_CC BestAudioSourceFree(void *instanceData, VSCore *core, const VSAPI *vsapi) {
    delete reinterpret_cast<BestAudioSourceData *>(instanceData);
}

static void VS_CC CreateBestAudioSource(const VSMap *in, VSMap *out, void *, VSCore *core, const VSAPI *vsapi) {
    int err;
    const char *Source = vsapi->mapGetData(in, "source", 0, nullptr);
    int Track = vsapi->mapGetIntSaturated(in, "track", 0, &err);
    if (err)
        Track = -1;
    int AdjustDelay = vsapi->mapGetIntSaturated(in, "adjustdelay", 0, &err);
    if (err)
        AdjustDelay = -1;
    bool ExactSamples = !!vsapi->mapGetInt(in, "exactsamples", 0, &err);
    if (err)
        ExactSamples = true;

    std::map<std::string, std::string> Opts;
    Opts["enable_drefs"] = vsapi->mapGetInt(in, "enable_drefs", 0, &err) ? "1" : "0";
    Opts["use_absolute_path"] = vsapi->mapGetInt(in, "use_absolute_path", 0, &err) ? "1" : "0";

    double DrcScale = vsapi->mapGetFloat(in, "drc_scale", 0, &err);

    BestAudioSourceData *D = new BestAudioSourceData();

    try {
        D->A.reset(new BestAudioSource(Source, Track, AdjustDelay, &Opts, DrcScale));
        if (ExactSamples)
            D->A->GetExactDuration();
        const AudioProperties &AP = D->A->GetAudioProperties();
        if (!vsapi->queryAudioFormat(&D->AI.format, AP.IsFloat, AP.BitsPerSample, AP.ChannelLayout, core))
            throw AudioException("Unsupported audio format from decoder (probably 8-bit)");
        D->AI.sampleRate = AP.SampleRate;
        D->AI.numSamples = AP.NumSamples;
        D->AI.numFrames = static_cast<int>((AP.NumSamples + VS_AUDIO_FRAME_SAMPLES - 1) / VS_AUDIO_FRAME_SAMPLES);
        if ((AP.NumSamples + VS_AUDIO_FRAME_SAMPLES - 1) / VS_AUDIO_FRAME_SAMPLES > std::numeric_limits<int>::max())
            throw AudioException("Unsupported audio format from decoder (probably 8-bit)");
    } catch (AudioException &e) {
        delete D;
        vsapi->mapSetError(out, (std::string("AudioSource: ") + e.what()).c_str());
        return;
    }

    vsapi->createAudioFilter(out, "AudioSource", &D->AI, BestAudioSourceGetFrame, BestAudioSourceFree, fmUnordered, nullptr, 0, D, core);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    vspapi->configPlugin("com.vapoursynth.bestsource", "bs", "Best Source", VS_MAKE_VERSION(0, 9), VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("VideoSource", "source:data;track:int:opt;variableformat:int:opt;seekpreroll:int:opt;exactframes:int:opt;enable_drefs:int:opt;use_absolute_path:int:opt;", "clip:vnode;", CreateBestVideoSource, nullptr, plugin);
    vspapi->registerFunction("AudioSource", "source:data;track:int:opt;adjustdelay:int:opt;exactsamples:int:opt;enable_drefs:int:opt;use_absolute_path:int:opt;drc_scale:float:opt;", "clip:anode;", CreateBestAudioSource, nullptr, plugin);
}
