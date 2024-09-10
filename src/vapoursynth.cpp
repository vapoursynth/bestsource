//  Copyright (c) 2022-2024 Fredrik Mellbin
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
#include <VapourSynth4.h>
#include <VSHelper4.h>
#include <vector>
#include <algorithm>
#include <memory>
#include <limits>
#include <string>
#include <chrono>
#include <mutex>

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
    int64_t FPSNum = -1;
    int64_t FPSDen = -1;
    bool RFF = false;
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

        } catch (BestSourceException &e) {
            vsapi->freeFrame(Dst);
            vsapi->freeFrame(AlphaDst);
            vsapi->setFilterError(("VideoSource: " + std::string(e.what())).c_str(), FrameCtx);
            return nullptr;
        }

        const BSVideoProperties &VP = D->V->GetVideoProperties();
        VSMap *Props = vsapi->getFramePropertiesRW(Dst);
        if (AlphaDst)
            vsapi->mapConsumeFrame(Props, "_Alpha", AlphaDst, maAppend);

        SetSynthFrameProperties(Src, VP, D->RFF, D->V->GetFrameIsTFF(n, D->RFF),
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

// convert an array of string pairs (denoted by In + InKey) to a std::map
static std::map<std::string, std::string> DataArrToMap(const VSMap *In, const char *InKey, const VSAPI *vsapi) {
    std::map<std::string, std::string> map;
    int count = vsapi->mapNumElements(In, InKey);
    if (count > 0 && (count & 1) == 1)
        throw BestSourceException((std::string(InKey) + " must be an array of string pairs").c_str());

    if (count >= 2) {
        for (int i = 0; i < count - 1; i += 2) {
            std::string key(vsapi->mapGetData(In, InKey, i, nullptr), vsapi->mapGetDataSize(In, InKey, i, nullptr));
            map[key] = std::string(vsapi->mapGetData(In, InKey, i + 1, nullptr), vsapi->mapGetDataSize(In, InKey, i + 1, nullptr));
        }
    }
    return map;
}

static void VS_CC CreateBestVideoSource(const VSMap *In, VSMap *Out, void *, VSCore *Core, const VSAPI *vsapi) {
    BSInit();

    int err;
    std::filesystem::path Source = CreateProbablyUTF8Path(vsapi->mapGetData(In, "source", 0, nullptr));
    const char *CachePath = vsapi->mapGetData(In, "cachepath", 0, &err);
    const char *HWDevice = vsapi->mapGetData(In, "hwdevice", 0, &err);
    const char *Timecodes = vsapi->mapGetData(In, "timecodes", 0, &err);
    int Track = vsapi->mapGetIntSaturated(In, "track", 0, &err);
    if (err)
        Track = -1;
    bool VariableFormat = !!vsapi->mapGetInt(In, "variableformat", 0, &err);
    int Threads = vsapi->mapGetIntSaturated(In, "threads", 0, &err);
    int StartNumber = vsapi->mapGetIntSaturated(In, "start_number", 0, &err);
    if (err)
        StartNumber = -1;
    bool ShowProgress = !!vsapi->mapGetInt(In, "showprogress", 0, &err);
    int ExtraHWFrames = vsapi->mapGetIntSaturated(In, "extrahwframes", 0, &err);
    if (err)
        ExtraHWFrames = 9;
    int CacheMode = vsapi->mapGetIntSaturated(In, "cachemode", 0, &err);
    if (err)
        CacheMode = 1;

    std::map<std::string, std::string> Opts, StreamOpts, CodecOpts;
    try {
        Opts = DataArrToMap(In, "format_opts", vsapi);
        if (vsapi->mapGetInt(In, "enable_drefs", 0, &err))
            Opts["enable_drefs"] = "1";
        if (vsapi->mapGetInt(In, "use_absolute_path", 0, &err))
            Opts["use_absolute_path"] = "1";
        if (StartNumber >= 0)
            Opts["start_number"] = std::to_string(StartNumber);

        StreamOpts = DataArrToMap(In, "stream_opts", vsapi);
        CodecOpts = DataArrToMap(In, "codec_opts", vsapi);
    } catch (BestSourceException &e) {
        vsapi->mapSetError(Out, (std::string("VideoSource: ") + e.what()).c_str());
        return;
    }

    BestVideoSourceData *D = new BestVideoSourceData();

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

        if (ShowProgress) {
            auto NextUpdate = std::chrono::high_resolution_clock::now();
            int LastValue = -1;
            D->V.reset(new BestVideoSource(Source, HWDevice ? HWDevice : "", ExtraHWFrames, Track, VariableFormat, Threads, CacheMode, CachePath ? CachePath : "", &Opts, &StreamOpts, &CodecOpts,
                [vsapi, Core, &NextUpdate, &LastValue](int Track, int64_t Cur, int64_t Total) {
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
                }));

        } else {
            D->V.reset(new BestVideoSource(Source, HWDevice ? HWDevice : "", ExtraHWFrames, Track, VariableFormat, Threads, CacheMode, CachePath ? CachePath : "", &Opts, &StreamOpts, &CodecOpts));
        }

        const BSVideoProperties &VP = D->V->GetVideoProperties();
        if (VP.VF.ColorFamily == 0 || !vsapi->queryVideoFormat(&D->VI.format, VP.VF.ColorFamily, VP.VF.Float, VP.VF.Bits, VP.VF.SubSamplingW, VP.VF.SubSamplingH, Core))
            throw BestSourceException("Unsupported video format from decoder (probably less than 8 bit or palette)");
        D->VI.width = VP.SSModWidth;
        D->VI.height = VP.SSModHeight;
        if (VariableFormat)
            D->VI = {};
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
    } catch (BestSourceException &e) {
        delete D;
        vsapi->mapSetError(Out, (std::string("VideoSource: ") + e.what()).c_str());
        return;
    }

    int64_t CacheSize = vsapi->mapGetInt(In, "cachesize", 0, &err);
    if (!err && CacheSize >= 0)
        D->V->SetMaxCacheSize(CacheSize * 1024 * 1024);

    vsapi->createVideoFilter(Out, "VideoSource", &D->VI, BestVideoSourceGetFrame, BestVideoSourceFree, fmUnordered, nullptr, 0, D, Core);
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
        } catch (BestSourceException &e) {
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
    const char *CachePath = vsapi->mapGetData(In, "cachepath", 0, &err);
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

    std::map<std::string, std::string> Opts;
    if (vsapi->mapGetInt(In, "enable_drefs", 0, &err))
        Opts["enable_drefs"] = "1";
    if (vsapi->mapGetInt(In, "use_absolute_path", 0, &err))
        Opts["use_absolute_path"] = "1";

    double DrcScale = vsapi->mapGetFloat(In, "drc_scale", 0, &err);

    BestAudioSourceData *D = new BestAudioSourceData();

    try {
        if (ShowProgress) {
            auto NextUpdate = std::chrono::high_resolution_clock::now();
            int LastValue = -1;
            D->A.reset(new BestAudioSource(Source, Track, AdjustDelay, false, Threads, CacheMode, CachePath ? CachePath : "", &Opts, DrcScale,
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
            D->A.reset(new BestAudioSource(Source, Track, AdjustDelay, false, Threads, CacheMode, CachePath ? CachePath : "", &Opts, DrcScale));
        }

        const BSAudioProperties &AP = D->A->GetAudioProperties();
        D->Is8Bit = (AP.AF.Bits <= 8);
        if (!vsapi->queryAudioFormat(&D->AI.format, AP.AF.Float, D->Is8Bit ? 16 : AP.AF.Bits, AP.ChannelLayout, Core))
            throw BestSourceException("Unsupported audio format from decoder (probably 8-bit)");
        D->AI.sampleRate = AP.SampleRate;
        D->AI.numSamples = AP.NumSamples;
        D->AI.numFrames = static_cast<int>((AP.NumSamples + VS_AUDIO_FRAME_SAMPLES - 1) / VS_AUDIO_FRAME_SAMPLES);
        if ((AP.NumSamples + VS_AUDIO_FRAME_SAMPLES - 1) / VS_AUDIO_FRAME_SAMPLES > std::numeric_limits<int>::max())
            throw BestSourceException("Too many audio samples, cut file into smaller parts");
    } catch (BestSourceException &e) {
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
    } catch (BestSourceException &e) {
        vsapi->mapSetError(Out, (std::string("TrackInfo: ") + e.what()).c_str());
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
    vspapi->configPlugin("com.vapoursynth.bestsource", "bs", "Best Source 2", VS_MAKE_VERSION(BEST_SOURCE_VERSION_MAJOR, BEST_SOURCE_VERSION_MINOR), VS_MAKE_VERSION(VAPOURSYNTH_API_MAJOR, 0), 0, plugin);
    vspapi->registerFunction("VideoSource", "source:data;track:int:opt;variableformat:int:opt;fpsnum:int:opt;fpsden:int:opt;rff:int:opt;threads:int:opt;seekpreroll:int:opt;enable_drefs:int:opt;use_absolute_path:int:opt;cachemode:int:opt;cachepath:data:opt;cachesize:int:opt;hwdevice:data:opt;extrahwframes:int:opt;timecodes:data:opt;start_number:int:opt;format_opts:data[]:opt;stream_opts:data[]:opt;codec_opts:data[]:opt;showprogress:int:opt;", "clip:vnode;", CreateBestVideoSource, nullptr, plugin);
    vspapi->registerFunction("AudioSource", "source:data;track:int:opt;adjustdelay:int:opt;threads:int:opt;enable_drefs:int:opt;use_absolute_path:int:opt;drc_scale:float:opt;cachemode:int:opt;cachepath:data:opt;cachesize:int:opt;showprogress:int:opt;", "clip:anode;", CreateBestAudioSource, nullptr, plugin);
    vspapi->registerFunction("TrackInfo", "source:data;enable_drefs:int:opt;use_absolute_path:int:opt;", "mediatype:int;mediatypestr:data;codec:int;codecstr:data;disposition:int;dispositionstr:data;", GetTrackInfo, nullptr, plugin);
    vspapi->registerFunction("SetDebugOutput", "enable:int;", "", SetDebugOutput, nullptr, plugin);
    vspapi->registerFunction("SetFFmpegLogLevel", "level:int;", "level:int;", SetLogLevel, nullptr, plugin);
}
