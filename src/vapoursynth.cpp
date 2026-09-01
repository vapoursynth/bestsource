//  Copyright (c) 2022-2025 Fredrik Mellbin
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
#include "tracklist.h"
#include "bsshared.h"
#include "version.h"
#include "synthshared.h"
#include "vsgpuexport.h"
#define VS_USE_API_43
#include <VapourSynth4.h>
#include <VSHelper4.h>
#include <vector>
#include <algorithm>
#include <memory>
#include <limits>
#include <string>
#include <chrono>
#include <mutex>
#include <functional>

static std::once_flag BSInitOnce;

static void BSInit() {
    // Slightly ugly to avoid header inclusions
    std::call_once(BSInitOnce, []() {
#ifndef NDEBUG
        SetFFmpegLogLevel(32); // quiet
#else
        SetFFmpegLogLevel(-8); // info
#endif
        });
}

struct BestVideoSourceData {
    VSVideoInfo VI = {};
    std::unique_ptr<BestVideoSource> V;
    /* Non-null only when the frames are published GPU resident. */
    std::unique_ptr<BSVSGpuExport> GpuExport;
    int64_t FPSNum = -1;
    int64_t FPSDen = -1;
    bool RFF = false;
    bool RFFIsUsed = false;
    bool RotationApplied = false;
};

static const VSFrame *VS_CC BestVideoSourceGetFrame(int n, int ActivationReason, void *InstanceData, void **, VSFrameContext *FrameCtx, VSCore *Core, const VSAPI *vsapi) {
    BestVideoSourceData *D = reinterpret_cast<BestVideoSourceData *>(InstanceData);

    if (ActivationReason == arInitial) {
        VSFrame *Dst = nullptr;
        VSFrame *AlphaDst = nullptr;
        std::unique_ptr<BestVideoFrame> Src;
        try {
            if (D->RFF) {
                Src.reset(D->V->GetFrameWithRFF(std::min(n, D->VI.numFrames - 1)));
            } else if (D->FPSNum > 0) {
                double currentTime = D->V->GetVideoProperties().StartTime +
                    (double)(std::min(n, D->VI.numFrames - 1) * D->FPSDen) / D->FPSNum;
                Src.reset(D->V->GetFrameByTime(currentTime));
            } else {
                Src.reset(D->V->GetFrame(std::min(n, D->VI.numFrames - 1)));
            }

            if (!Src)
                throw BestSourceException("No frame returned for frame number " + std::to_string(n) + ". This may be due to an FFmpeg bug. Retry with threads=1 if not already set.");

            VSVideoFormat VideoFormat = {};
            vsapi->queryVideoFormat(&VideoFormat, Src->VF.ColorFamily, Src->VF.Float ? stFloat : stInteger, Src->VF.Bits, Src->VF.SubSamplingW, Src->VF.SubSamplingH, Core);
            VSVideoFormat AlphaFormat = {};
            vsapi->queryVideoFormat(&AlphaFormat, cfGray, VideoFormat.sampleType, VideoFormat.bitsPerSample, 0, 0, Core);

            if (D->GpuExport) {
                /* The decoder writes the planes on the device and publishes a producer pair, so
                   nothing here touches pixels and the frame never crosses the bus. Falls through
                   to the shared property code below; alpha cannot occur here since Create refuses
                   formats carrying it. */
                Dst = D->GpuExport->ExportFrame(Src.get(), &VideoFormat, Src->SSModWidth, Src->SSModHeight, Core, vsapi);
            } else {
                Dst = vsapi->newVideoFrame(&VideoFormat, Src->SSModWidth, Src->SSModHeight, nullptr, Core);
                uint8_t *DstPtrs[3] = {};
                ptrdiff_t DstStride[3] = {};

                for (int Plane = 0; Plane < VideoFormat.numPlanes; Plane++) {
                    DstPtrs[Plane] = vsapi->getWritePtr(Dst, Plane);
                    DstStride[Plane] = vsapi->getStride(Dst, Plane);
                }

                ptrdiff_t AlphaStride = 0;
                if (Src->VF.Alpha) {
                    AlphaDst = vsapi->newVideoFrame(&AlphaFormat, Src->SSModWidth, Src->SSModHeight, nullptr, Core);
                    AlphaStride = vsapi->getStride(AlphaDst, 0);
                    vsapi->mapSetInt(vsapi->getFramePropertiesRW(AlphaDst), "_ColorRange", 0, maAppend);
                }

                if (!Src->ExportAsPlanar(DstPtrs, DstStride, AlphaDst ? vsapi->getWritePtr(AlphaDst, 0) : nullptr, AlphaStride)) {
                    throw BestSourceException("Cannot export to planar format for frame " + std::to_string(n));
                }
            }

        } catch (const std::exception &e) {
            vsapi->freeFrame(Dst);
            vsapi->freeFrame(AlphaDst);
            vsapi->setFilterError(("VideoSource: " + std::string(e.what())).c_str(), FrameCtx);
            return nullptr;
        }

        VSMap *Props = vsapi->getFramePropertiesRW(Dst);
        if (AlphaDst)
            vsapi->mapConsumeFrame(Props, "_Alpha", AlphaDst, maAppend);

        SetSynthFrameProperties(n, Src, *D->V, D->RFFIsUsed, D->V->GetFrameIsTFF(n, D->RFF), D->RotationApplied,
            [Props, vsapi](const char *Name, int64_t V) { vsapi->mapSetInt(Props, Name, V, maAppend); },
            [Props, vsapi](const char *Name, double V) { vsapi->mapSetFloat(Props, Name, V, maAppend); },
            [Props, vsapi](const char *Name, const char *V, int Size, bool Utf8) { vsapi->mapSetData(Props, Name, V, Size, Utf8 ? dtUtf8 : dtBinary, maAppend); });

        return Dst;
    }

    return nullptr;
}

static void VS_CC BestVideoSourceFree(void *InstanceData, VSCore *Core, const VSAPI *vsapi) {
    delete reinterpret_cast<BestVideoSourceData *>(InstanceData);
}

static const char *const OrientationFilters[2][4][2] = {
    /* Not flipped */ { { nullptr, nullptr },        { "Turn90", nullptr },    { "Turn180", nullptr },        { "Turn270", nullptr } },
    /* Flipped     */ { { "FlipVertical", nullptr }, { "Transpose", nullptr }, { "FlipHorizontal", nullptr }, { "FlipVertical", "Turn270" } }
    /*                  0 degrees                     90 degrees                180 degrees                    270 degrees */
};

// Always consumes Node and returns the oriented node, or nullptr with Error set on failure.
static VSNode *ApplyOrientation(VSNode *Node, const BSVideoProperties &VP, VSCore *Core, const VSAPI *vsapi, std::string &Error) {
    // Avisynth+ and VapourSynth alike only have filters for the multiples of 90 that a display
    // matrix realistically contains, so anything else has to be left to the user.
    if (VP.Rotation % 90) {
        Error = "Can't apply the " + std::to_string(VP.Rotation) + " degree rotation of the video, only multiples of 90 are supported, set apply_rotation=False to get the untransformed video";
        vsapi->freeNode(Node);
        return nullptr;
    }

    // Read before the first invoke since a failed one frees the node and with it the properties.
    const char *const *Filters = OrientationFilters[VP.FlipVertical][(VP.Rotation / 90) % 4];
    VSPlugin *StdPlugin = vsapi->getPluginByID(VSH_STD_PLUGIN_ID, Core);

    for (int i = 0; i < 2 && Filters[i]; i++) {
        VSMap *Args = vsapi->createMap();
        vsapi->mapConsumeNode(Args, "clip", Node, maAppend);
        VSMap *Ret = vsapi->invoke(StdPlugin, Filters[i], Args);
        vsapi->freeMap(Args);

        const char *InvokeError = vsapi->mapGetError(Ret);
        if (InvokeError) {
            Error = std::string(InvokeError) + ", set apply_rotation=False to output the video without applying its orientation";
            vsapi->freeMap(Ret);
            return nullptr;
        }

        Node = vsapi->mapGetNode(Ret, "clip", 0, nullptr);
        vsapi->freeMap(Ret);
    }

    return Node;
}

static void VS_CC CreateBestVideoSource(const VSMap *In, VSMap *Out, void *, VSCore *Core, const VSAPI *vsapi) {
    BSInit();

    int err;
    std::filesystem::path Source = CreateProbablyUTF8Path(vsapi->mapGetData(In, "source", 0, nullptr));
    const char *RawCachePath = vsapi->mapGetData(In, "cachepath", 0, &err);
    std::filesystem::path CachePath = CreateProbablyUTF8Path(RawCachePath ? RawCachePath : "");
    const char *Timecodes = vsapi->mapGetData(In, "timecodes", 0, &err);
    int Track = vsapi->mapGetIntSaturated(In, "track", 0, &err);
    if (err)
        Track = -1;
    int ViewID = vsapi->mapGetIntSaturated(In, "viewid", 0, &err);
    int VariableFormat = vsapi->mapGetIntSaturated(In, "variableformat", 0, &err);
    if (err)
        VariableFormat = -1;
    int Threads = vsapi->mapGetIntSaturated(In, "threads", 0, &err);
    int StartNumber = vsapi->mapGetIntSaturated(In, "start_number", 0, &err);
    if (err)
        StartNumber = -1;
    bool ShowProgress = !!vsapi->mapGetInt(In, "showprogress", 0, &err);
    if (err)
        ShowProgress = true;
    int CacheMode = vsapi->mapGetIntSaturated(In, "cachemode", 0, &err);
    if (err)
        CacheMode = 1;
    int MaxDecoders = vsapi->mapGetIntSaturated(In, "maxdecoders", 0, &err);
    bool ExportTimestamps = !!vsapi->mapGetInt(In, "exporttimestamps", 0, &err);
    bool ApplyRotation = !!vsapi->mapGetInt(In, "apply_rotation", 0, &err);
    if (err)
        ApplyRotation = true;
    bool GPU = !!vsapi->mapGetInt(In, "gpu", 0, &err);
    /* Only meaningful with gpu set. Turning it off makes anything that would prevent GPU decoding
       an error instead, which is what a script wanting a guaranteed residency needs. */
    bool GPUFallback = !!vsapi->mapGetInt(In, "gpufallback", 0, &err);
    if (err)
        GPUFallback = true;

    /* Asking the core which GPU it is on has to happen before the source is constructed, because
       the answer is what pins the decoder to the same one: sharing memory between two Vulkan
       devices only works when both come from the same physical device. The selector is the core
       device's UUID, which FFmpeg resolves to that exact physical device or fails, so the two
       cannot end up apart. */
    std::optional<std::array<uint8_t, 16>> GpuDevice;
    if (GPU) {
        std::string GpuError;
        std::array<uint8_t, 16> CoreUUID = {};
        if (BSVSGpuExport::QueryDevice(Core, vsapi, CoreUUID, GpuError)) {
            GpuDevice = CoreUUID;
        } else {
            if (!GPUFallback) {
                vsapi->mapSetError(Out, ("VideoSource: gpu decoding requested but " + GpuError).c_str());
                return;
            }
            vsapi->logMessage(mtInformation, ("VideoSource: decoding on the CPU instead, " + GpuError).c_str(), Core);
            GPU = false;
            GpuDevice.reset();
        }
    }

    std::map<std::string, std::string> Opts;
    if (vsapi->mapGetInt(In, "enable_drefs", 0, &err))
        Opts["enable_drefs"] = "1";
    if (vsapi->mapGetInt(In, "use_absolute_path", 0, &err))
        Opts["use_absolute_path"] = "1";
    if (StartNumber >= 0)
        Opts["start_number"] = std::to_string(StartNumber);

    BestVideoSourceData *D = new BestVideoSourceData();
    D->RotationApplied = ApplyRotation;
    VSNode *Node = nullptr;

    try {
        D->FPSNum = vsapi->mapGetInt(In, "fpsnum", 0, &err);
        if (err)
            D->FPSNum = -1;
        D->FPSDen = vsapi->mapGetInt(In, "fpsden", 0, &err);
        if (err)
            D->FPSDen = 1;
        D->RFF = !!vsapi->mapGetInt(In, "rff", 0, &err);

        if (D->FPSDen < 1)
            throw BestSourceException("FPS denominator needs to be 1 or greater");

        if (D->FPSNum > 0 && D->RFF)
            throw BestSourceException("Cannot combine CFR and RFF modes");

        if (ExportTimestamps && (D->RFF || D->FPSNum > 0))
            throw BestSourceException("Cannot combine RFF or CFR mode with timestamp export");

        /* Constructing the source may have to be done twice, once on the GPU and once on the CPU
           if that fails and gpufallback allows it, so the call is written once here rather than at
           each of the places that can fall back. Everything it touches has to outlive the last of
           those, which is why the progress state lives out here instead of in a branch. An empty
           ProgressFunction is the documented way to ask for no progress reports. */
        auto NextUpdate = std::chrono::high_resolution_clock::now();
        int LastValue = -1;
        ProgressFunction ProgressCB;
        if (ShowProgress) {
            ProgressCB = [vsapi, Core, &NextUpdate, &LastValue](int Track, int64_t Cur, int64_t Total) {
                if (NextUpdate < std::chrono::high_resolution_clock::now()) {
                    if (Total == INT64_MAX && Cur == Total) {
                        vsapi->logMessage(mtInformation, ("VideoSource track #" + std::to_string(Track) + " indexing complete").c_str(), Core);
                    } else {
                        int PValue = (Total > 0) ? static_cast<int>((static_cast<double>(Cur) / static_cast<double>(Total)) * 100) : static_cast<int>(Cur / (1024 * 1024));
                        if (PValue != LastValue) {
                            vsapi->logMessage(mtInformation, ("VideoSource track #" + std::to_string(Track) + " index progress " + std::to_string(PValue) + ((Total > 0) ? "%" : "MB")).c_str(), Core);
                            LastValue = PValue;
                            NextUpdate = std::chrono::high_resolution_clock::now() + std::chrono::seconds(1);
                        }
                    }
                }
                return true;
                };
        }

        auto MakeSource = [&](bool UseGPU) {
            D->V.reset(new BestVideoSource(Source, UseGPU, UseGPU ? GpuDevice : decltype(GpuDevice){}, Track, ViewID, Threads, CacheMode, CachePath, &Opts, ProgressCB));
            };

        try {
            MakeSource(GPU);
        } catch (BestSourceHWDecoderException &) {
            if (!GPU || !GPUFallback)
                throw;
            vsapi->logMessage(mtInformation, ("VideoSource track #" + std::to_string(Track) + " decoding on the CPU instead, the GPU can't decode this track").c_str(), Core);
            GPU = false;
            MakeSource(false);
        }

        if (GPU) {
            std::string GpuError;
            D->GpuExport = BSVSGpuExport::Create(D->V.get(), VariableFormat, Core, vsapi, GpuError);
            if (!D->GpuExport) {
                if (!GPUFallback)
                    throw BestSourceException("gpu decoding unavailable: " + GpuError);
                /* Rebuilding means indexing again, since the index records which decoder wrote it.
                   Only reachable when the device turns out not to be shareable after all, which is
                   rare enough not to be worth avoiding at the cost of deferring the check. */
                vsapi->logMessage(mtInformation, ("VideoSource track #" + std::to_string(Track) + " decoding on the CPU instead, " + GpuError).c_str(), Core);
                GPU = false;
                MakeSource(false);
            }
        }

        D->V->SetMaxDecoderInstances(MaxDecoders);
        D->V->SelectFormatSet(VariableFormat);

        const BSVideoProperties &VP = D->V->GetVideoProperties();
        if (VP.VF.ColorFamily == 4)
            throw BestSourceException("Unsupported source colorspace (bayer)");
        if ((VP.VF.ColorFamily == 0 && VariableFormat != -1) || !vsapi->queryVideoFormat(&D->VI.format, VP.VF.ColorFamily, VP.VF.Float, VP.VF.Bits, VP.VF.SubSamplingW, VP.VF.SubSamplingH, Core))
            throw BestSourceException("Unsupported video format from decoder (probably less than 8 bit or palette)");

        if (VP.SSModWidth == 0 || VP.SSModHeight == 0)
            throw BestSourceException("Rounding dimensions down to nearest subsampling multiple leaves nothing to output");

        D->RFFIsUsed = (VP.NumFrames != VP.NumRFFFrames);
        D->VI.width = VP.SSModWidth;
        D->VI.height = VP.SSModHeight;
        D->VI.numFrames = vsh::int64ToIntS(VP.NumFrames);
        D->VI.fpsNum = VP.FPS.Num;
        D->VI.fpsDen = VP.FPS.Den;
        vsh::reduceRational(&D->VI.fpsNum, &D->VI.fpsDen);

        if (D->FPSNum > 0) {
            vsh::reduceRational(&D->FPSNum, &D->FPSDen);
            D->VI.fpsDen = D->FPSDen;
            D->VI.fpsNum = D->FPSNum;
            D->VI.numFrames = std::max(1, static_cast<int>((VP.Duration * D->VI.fpsNum) * VP.TimeBase.ToDouble() / D->VI.fpsDen + 0.5));
        } else if (D->RFF) {
            D->VI.numFrames = vsh::int64ToIntS(VP.NumRFFFrames);
        }

        int SeekPreRoll = vsapi->mapGetIntSaturated(In, "seekpreroll", 0, &err);
        if (!err)
            D->V->SetSeekPreRoll(SeekPreRoll);

        if (Timecodes)
            D->V->WriteTimecodes(CreateProbablyUTF8Path(Timecodes));

        if (ExportTimestamps) {
            vsapi->mapSetInt(Out, "timebasenum", VP.TimeBase.Num, maAppend);
            vsapi->mapSetInt(Out, "timebaseden", VP.TimeBase.Den, maAppend);
            for (int64_t i = 0; i < D->VI.numFrames; i++)
                vsapi->mapSetInt(Out, "timestamps", D->V->GetFrameInfo(i).PTS, maAppend);
        }

        int64_t CacheSize = vsapi->mapGetInt(In, "cachesize", 0, &err);
        if (!err && CacheSize >= 0)
            D->V->SetMaxCacheSize(CacheSize * 1024 * 1024);

        // Has to be the last thing that can fail while D is still owned here, since a successful
        // call hands D over to the node and the cleanup below would then be a double free.
        /* ffGPUOutput has to match the declared vnode:all residency for this instance, or the core
           kills the process at the function that lied. */
        if (D->GpuExport)
            Node = vsapi->createVideoFilterEx2("VideoSource", &D->VI, BestVideoSourceGetFrame, BestVideoSourceFree, fmUnordered,
                ffGPUOutput, nullptr, 0, D, Core);
        else
            Node = vsapi->createVideoFilter2("VideoSource", &D->VI, BestVideoSourceGetFrame, BestVideoSourceFree, fmUnordered, nullptr, 0, D, Core);
        if (!Node)
            throw BestSourceException("Failed to create filter");
    } catch (const std::exception &e) {
        delete D;
        vsapi->mapSetError(Out, (std::string("VideoSource: ") + e.what()).c_str());
        return;
    }

    if (ApplyRotation) {
        // The node owns D from here on, so the failure path has nothing left to clean up.
        std::string Error;
        Node = ApplyOrientation(Node, D->V->GetVideoProperties(), Core, vsapi, Error);
        if (!Node) {
            vsapi->mapSetError(Out, ("VideoSource: " + Error).c_str());
            return;
        }
    }

    vsapi->mapConsumeNode(Out, "clip", Node, maAppend);
}

struct BestAudioSourceData {
    VSAudioInfo AI = {};
    bool Is8Bit = false;
    std::unique_ptr<BestAudioSource> A;
};

static const VSFrame *VS_CC BestAudioSourceGetFrame(int n, int ActivationReason, void *InstanceData, void **, VSFrameContext *FrameCtx, VSCore *Core, const VSAPI *vsapi) {
    BestAudioSourceData *D = reinterpret_cast<BestAudioSourceData *>(InstanceData);

    if (ActivationReason == arInitial) {
        int64_t SamplesOut = std::min<int64_t>(VS_AUDIO_FRAME_SAMPLES, D->AI.numSamples - n * static_cast<int64_t>(VS_AUDIO_FRAME_SAMPLES));
        VSFrame *Dst = vsapi->newAudioFrame(&D->AI.format, static_cast<int>(SamplesOut), nullptr, Core);

        std::vector<uint8_t *> Tmp;
        Tmp.reserve(D->AI.format.numChannels);
        for (int Channel = 0; Channel < D->AI.format.numChannels; Channel++)
            Tmp.push_back(vsapi->getWritePtr(Dst, Channel));
        try {
            D->A->GetPlanarAudio(Tmp.data(), n * static_cast<int64_t>(VS_AUDIO_FRAME_SAMPLES), SamplesOut);
        } catch (const std::exception &e) {
            vsapi->setFilterError(("AudioSource: " + std::string(e.what())).c_str(), FrameCtx);
            vsapi->freeFrame(Dst);
            return nullptr;
        }
        if (D->Is8Bit) {
            // Ugly hack to unpack 8 bit audio inplace
            for (int Channel = 0; Channel < D->AI.format.numChannels; Channel++) {
                uint16_t *WritePtr = reinterpret_cast<uint16_t *>(vsapi->getWritePtr(Dst, Channel));
                const uint8_t *ReadPtr = vsapi->getWritePtr(Dst, Channel);
                for (int64_t i = SamplesOut - 1; i >= 0; i--)
                    WritePtr[i] = (ReadPtr[i] - 0x80U) << 8;
            }
        }

        return Dst;
    }

    return nullptr;
}

static void VS_CC BestAudioSourceFree(void *InstanceData, VSCore *Core, const VSAPI *vsapi) {
    delete reinterpret_cast<BestAudioSourceData *>(InstanceData);
}

static void VS_CC CreateBestAudioSource(const VSMap *In, VSMap *Out, void *, VSCore *Core, const VSAPI *vsapi) {
    BSInit();

    int err;
    std::filesystem::path Source = CreateProbablyUTF8Path(vsapi->mapGetData(In, "source", 0, nullptr));
    const char *RawCachePath = vsapi->mapGetData(In, "cachepath", 0, &err);
    std::filesystem::path CachePath = CreateProbablyUTF8Path(RawCachePath ? RawCachePath : "");
    int Track = vsapi->mapGetIntSaturated(In, "track", 0, &err);
    if (err)
        Track = -1;
    int AdjustDelay = vsapi->mapGetIntSaturated(In, "adjustdelay", 0, &err);
    if (err)
        AdjustDelay = -1;
    int Threads = vsapi->mapGetIntSaturated(In, "threads", 0, &err);
    bool ShowProgress = !!vsapi->mapGetInt(In, "showprogress", 0, &err);
    int CacheMode = vsapi->mapGetIntSaturated(In, "cachemode", 0, &err);
    if (err)
        CacheMode = 1;
    int MaxDecoders = vsapi->mapGetIntSaturated(In, "maxdecoders", 0, &err);
    int VariableFormat = vsapi->mapGetIntSaturated(In, "variableformat", 0, &err);
    if (err)
        VariableFormat = 0;

    std::map<std::string, std::string> Opts;
    if (vsapi->mapGetInt(In, "enable_drefs", 0, &err))
        Opts["enable_drefs"] = "1";
    if (vsapi->mapGetInt(In, "use_absolute_path", 0, &err))
        Opts["use_absolute_path"] = "1";

    double DrcScale = vsapi->mapGetFloat(In, "drc_scale", 0, &err);

    BestAudioSourceData *D = new BestAudioSourceData();

    try {
        if (VariableFormat < 0)
            throw BestSourceException("Variable format number must be 0 or greater");

        if (ShowProgress) {
            auto NextUpdate = std::chrono::high_resolution_clock::now();
            int LastValue = -1;
            D->A.reset(new BestAudioSource(Source, Track, AdjustDelay, Threads, CacheMode, CachePath, &Opts, DrcScale,
                [vsapi, Core, &NextUpdate, &LastValue](int Track, int64_t Cur, int64_t Total) {
                    if (NextUpdate < std::chrono::high_resolution_clock::now()) {
                        if (Total == INT64_MAX && Cur == Total) {
                            vsapi->logMessage(mtInformation, ("AudioSource track #" + std::to_string(Track) + " indexing complete").c_str(), Core);
                        } else {
                            int PValue = (Total > 0) ? static_cast<int>((static_cast<double>(Cur) / static_cast<double>(Total)) * 100) : static_cast<int>(Cur / (1024 * 1024));
                            if (PValue != LastValue) {
                                vsapi->logMessage(mtInformation, ("AudioSource track #" + std::to_string(Track) + " index progress " + std::to_string(PValue) + ((Total > 0) ? "%" : "MB")).c_str(), Core);
                                LastValue = PValue;
                                NextUpdate = std::chrono::high_resolution_clock::now() + std::chrono::seconds(1);
                            }
                        }
                    }
                    return true;
                }));

        } else {
            D->A.reset(new BestAudioSource(Source, Track, AdjustDelay, Threads, CacheMode, CachePath, &Opts, DrcScale));
        }

        D->A->SetMaxDecoderInstances(MaxDecoders);
        D->A->SelectFormatSet(VariableFormat);

        const BSAudioProperties &AP = D->A->GetAudioProperties();
        D->Is8Bit = (AP.AF.Bits <= 8);
        if (!vsapi->queryAudioFormat(&D->AI.format, AP.AF.Float, D->Is8Bit ? 16 : AP.AF.Bits, AP.ChannelLayout, Core))
            throw BestSourceException("Unsupported audio format from decoder (probably 8-bit)");
        D->AI.sampleRate = AP.SampleRate;
        D->AI.numSamples = AP.NumSamples;
        D->AI.numFrames = static_cast<int>((AP.NumSamples + VS_AUDIO_FRAME_SAMPLES - 1) / VS_AUDIO_FRAME_SAMPLES);
        if ((AP.NumSamples + VS_AUDIO_FRAME_SAMPLES - 1) / VS_AUDIO_FRAME_SAMPLES > std::numeric_limits<int>::max())
            throw BestSourceException("Too many audio samples, cut file into smaller parts");
    } catch (const std::exception &e) {
        delete D;
        vsapi->mapSetError(Out, (std::string("AudioSource: ") + e.what()).c_str());
        return;
    }

    int64_t CacheSize = vsapi->mapGetInt(In, "cachesize", 0, &err);
    if (!err && CacheSize > 0)
        D->A->SetMaxCacheSize(CacheSize * 1024 * 1024);

    vsapi->createAudioFilter(Out, "AudioSource", &D->AI, BestAudioSourceGetFrame, BestAudioSourceFree, fmUnordered, nullptr, 0, D, Core);
}

static void VS_CC GetTrackInfo(const VSMap *In, VSMap *Out, void *, VSCore *core, const VSAPI *vsapi) {
    BSInit();

    int err;
    std::filesystem::path Source = CreateProbablyUTF8Path(vsapi->mapGetData(In, "source", 0, nullptr));

    std::map<std::string, std::string> Opts;
    if (vsapi->mapGetInt(In, "enable_drefs", 0, &err))
        Opts["enable_drefs"] = "1";
    if (vsapi->mapGetInt(In, "use_absolute_path", 0, &err))
        Opts["use_absolute_path"] = "1";

    try {
        std::unique_ptr<BestTrackList> TrackList(new BestTrackList(Source, &Opts));
        for (int i = 0; i < TrackList->GetNumTracks(); i++) {
            auto TI = TrackList->GetTrackInfo(i);
            vsapi->mapSetInt(Out, "tracktype", TI.MediaType, maAppend);
            vsapi->mapSetData(Out, "tracktypestr", TI.MediaTypeString.c_str(), -1, dtUtf8, maAppend);
            vsapi->mapSetInt(Out, "codec", TI.Codec, maAppend);
            vsapi->mapSetData(Out, "codecstr", TI.CodecString.c_str(), -1, dtUtf8, maAppend);
            vsapi->mapSetInt(Out, "disposition", TI.Disposition, maAppend);
            vsapi->mapSetData(Out, "dispositionstr", TI.DispositionString.c_str(), -1, dtUtf8, maAppend);
        }
    } catch (const std::exception &e) {
        vsapi->mapSetError(Out, (std::string("TrackInfo: ") + e.what()).c_str());
    }
}

static void VS_CC GetMetadata(const VSMap *In, VSMap *Out, void *, VSCore *core, const VSAPI *vsapi) {
    BSInit();

    int err;
    std::filesystem::path Source = CreateProbablyUTF8Path(vsapi->mapGetData(In, "source", 0, nullptr));

    int Track = vsapi->mapGetIntSaturated(In, "track", 0, &err);
    if (err)
        Track = -1;

    std::map<std::string, std::string> Opts;
    if (vsapi->mapGetInt(In, "enable_drefs", 0, &err))
        Opts["enable_drefs"] = "1";
    if (vsapi->mapGetInt(In, "use_absolute_path", 0, &err))
        Opts["use_absolute_path"] = "1";

    try {
        std::unique_ptr<BestTrackList> TrackList(new BestTrackList(Source, &Opts));
        
        auto Metadata = (Track < 0) ? TrackList->GetFileMetadata() : TrackList->GetTrackMetadata(Track);

        for (const auto &Iter : Metadata) {
            vsapi->mapSetData(Out, Iter.first.c_str(), Iter.second.c_str(), -1, dtUtf8, maAppend);
        }
    } catch (const std::exception &e) {
        vsapi->mapSetError(Out, (std::string("Metadata: ") + e.what()).c_str());
    }
}

static void VS_CC SetDebugOutput(const VSMap *In, VSMap *, void *, VSCore *, const VSAPI *vsapi) {
    BSInit();
    SetBSDebugOutput(!!vsapi->mapGetInt(In, "enable", 0, nullptr));
}

static void VS_CC SetLogLevel(const VSMap *In, VSMap *Out, void *, VSCore *, const VSAPI *vsapi) {
    BSInit();
    int err;
    int level = vsapi->mapGetIntSaturated(In, "level", 0, &err);
    if (err)
        level = 32;
    vsapi->mapSetInt(Out, "level", SetFFmpegLogLevel(level), maReplace);
}

VS_EXTERNAL_API(void) VapourSynthPluginInit2(VSPlugin *plugin, const VSPLUGINAPI *vspapi) {
    vspapi->configPlugin("com.vapoursynth.bestsource", "bs", "Best Source 2", VS_MAKE_VERSION(BEST_SOURCE_VERSION_MAJOR, BEST_SOURCE_VERSION_MINOR), VAPOURSYNTH_API_VERSION, 0, plugin);
    vspapi->registerFunction("VideoSource", "source:data;track:int:opt;variableformat:int:opt;fpsnum:int:opt;fpsden:int:opt;rff:int:opt;threads:int:opt;seekpreroll:int:opt;enable_drefs:int:opt;use_absolute_path:int:opt;cachemode:int:opt;cachepath:data:opt;cachesize:int:opt;timecodes:data:opt;start_number:int:opt;viewid:int:opt;showprogress:int:opt;maxdecoders:int:opt;gpufallback:int:opt;exporttimestamps:int:opt;apply_rotation:int:opt;gpu:int:opt;", "clip:vnode:all;", CreateBestVideoSource, nullptr, plugin);
    vspapi->registerFunction("AudioSource", "source:data;track:int:opt;adjustdelay:int:opt;threads:int:opt;enable_drefs:int:opt;use_absolute_path:int:opt;drc_scale:float:opt;cachemode:int:opt;cachepath:data:opt;cachesize:int:opt;showprogress:int:opt;maxdecoders:int:opt;variableformat:int:opt;", "clip:anode;", CreateBestAudioSource, nullptr, plugin);
    vspapi->registerFunction("TrackInfo", "source:data;enable_drefs:int:opt;use_absolute_path:int:opt;", "tracktype:int[];tracktypestr:data[];codec:int[];codecstr:data[];disposition:int[];dispositionstr:data[];", GetTrackInfo, nullptr, plugin);
    vspapi->registerFunction("Metadata", "source:data;track:int:opt;enable_drefs:int:opt;use_absolute_path:int:opt;", "any", GetMetadata, nullptr, plugin);
    vspapi->registerFunction("SetDebugOutput", "enable:int;", "", SetDebugOutput, nullptr, plugin);
    vspapi->registerFunction("SetFFmpegLogLevel", "level:int;", "level:int;", SetLogLevel, nullptr, plugin);
}
