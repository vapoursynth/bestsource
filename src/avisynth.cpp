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
#include "bsshared.h"
#include "version.h"
#include "synthshared.h"
#include "../AviSynthPlus/avs_core/include/avisynth.h"
#include <VSHelper4.h>
#include <vector>
#include <algorithm>
#include <memory>
#include <limits>
#include <string>
#include <chrono>
#include <mutex>
#include <cassert>

#ifdef _WIN32
#define AVS_EXPORT __declspec(dllexport)
#else
#define AVS_EXPORT __attribute__((visibility("default")))
#endif

// Endian detection
#ifdef _WIN32
#define BS_LITTLE_ENDIAN
#elif defined(__BYTE_ORDER__)
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define BS_BIG_ENDIAN
#elif __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define BS_LITTLE_ENDIAN
#endif
#endif

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

class AvisynthVideoSource : public IClip {
    VideoInfo VI = {};
    std::unique_ptr<BestVideoSource> V;
    int64_t FPSNum;
    int64_t FPSDen;
    bool RFF;
public:
    AvisynthVideoSource(const char *Source, int Track, int ViewID,
        int AFPSNum, int AFPSDen, bool RFF, int Threads, int SeekPreRoll, bool EnableDrefs, bool UseAbsolutePath,
        int CacheMode, const char *CachePath, int CacheSize, const char *HWDevice, int ExtraHWFrames,
        const char *Timecodes, int StartNumber, int VariableFormat, int MaxDecoders, bool HWFallback, IScriptEnvironment *Env)
        : FPSNum(AFPSNum), FPSDen(AFPSDen), RFF(RFF) {

        try {
            if (VariableFormat < 0)
                throw BestSourceException("Variable format number must be 0 or greater");

            if (FPSDen < 1)
                throw BestSourceException("FPS denominator needs to be 1 or greater");

            if (FPSNum > 0 && RFF)
                throw BestSourceException("Cannot combine CFR and RFF modes");

            std::map<std::string, std::string> Opts;
            if (EnableDrefs)
                Opts["enable_drefs"] = "1";
            if (UseAbsolutePath)
                Opts["use_absolute_path"] = "1";
            if (StartNumber >= 0)
                Opts["start_number"] = std::to_string(StartNumber);

            try {
                V.reset(new BestVideoSource(CreateProbablyUTF8Path(Source), HWDevice ? HWDevice : "", ExtraHWFrames, Track, ViewID, Threads, CacheMode, CachePath, &Opts));
            } catch (BestSourceHWDecoderException &) {
                if (HWFallback)
                    V.reset(new BestVideoSource(CreateProbablyUTF8Path(Source), "", ExtraHWFrames, Track, ViewID, Threads, CacheMode, CachePath, &Opts));
                else
                    throw;
            }

            V->SetMaxDecoderInstances(MaxDecoders);
            V->SelectFormatSet(VariableFormat);

            const BSVideoProperties &VP = V->GetVideoProperties();

            if (VP.SSModWidth == 0 || VP.SSModHeight == 0)
                throw BestSourceException("Rounding dimensions down to nearest subsampling multiple leaves nothing to output");

            if (VP.VF.ColorFamily == cfGray) {
                VI.pixel_type = VideoInfo::CS_GENERIC_Y;
            } else if (VP.VF.ColorFamily == cfYUV && VP.VF.Alpha) {
                VI.pixel_type = VideoInfo::CS_PLANAR | VideoInfo::CS_YUVA | VideoInfo::CS_VPlaneFirst; // Why is there no generic YUVA constant?
            } else if (VP.VF.ColorFamily == cfYUV) {
                VI.pixel_type = VideoInfo::CS_PLANAR | VideoInfo::CS_YUV | VideoInfo::CS_VPlaneFirst; // Why is there no generic YUV constant?
            } else if (VP.VF.ColorFamily == cfRGB && VP.VF.Alpha) {
                VI.pixel_type = VideoInfo::CS_GENERIC_RGBAP;
            } else if (VP.VF.ColorFamily == cfRGB) {
                VI.pixel_type = VideoInfo::CS_GENERIC_RGBP;
            } else if (VP.VF.ColorFamily == 4) {
                throw BestSourceException("Unsupported source colorspace (bayer)");
            } else {
                throw BestSourceException("Unsupported output colorspace");
            }

            // Settings subsampling for non-yuv will error out
            if (VP.VF.ColorFamily == cfYUV) {
                if (VP.VF.SubSamplingH == 0) {
                    VI.pixel_type |= VideoInfo::CS_Sub_Height_1;
                } else if (VP.VF.SubSamplingH == 1) {
                    VI.pixel_type |= VideoInfo::CS_Sub_Height_2;
                } else if (VP.VF.SubSamplingH == 2) {
                    VI.pixel_type |= VideoInfo::CS_Sub_Height_4;
                } else {
                    throw BestSourceException("Unsupported output subsampling");
                }

                if (VP.VF.SubSamplingW == 0) {
                    VI.pixel_type |= VideoInfo::CS_Sub_Width_1;
                } else if (VP.VF.SubSamplingW == 1) {
                    VI.pixel_type |= VideoInfo::CS_Sub_Width_2;
                } else if (VP.VF.SubSamplingW == 2) {
                    VI.pixel_type |= VideoInfo::CS_Sub_Width_4;
                } else {
                    throw BestSourceException("Unsupported output subsampling");
                }
            }

            if (VP.VF.Bits == 32 && VP.VF.Float) {
                VI.pixel_type |= VideoInfo::CS_Sample_Bits_32;
            } else if (VP.VF.Bits == 16 && !VP.VF.Float) {
                VI.pixel_type |= VideoInfo::CS_Sample_Bits_16;
            } else if (VP.VF.Bits == 14 && !VP.VF.Float) {
                VI.pixel_type |= VideoInfo::CS_Sample_Bits_14;
            } else if (VP.VF.Bits == 12 && !VP.VF.Float) {
                VI.pixel_type |= VideoInfo::CS_Sample_Bits_12;
            } else if (VP.VF.Bits == 10 && !VP.VF.Float) {
                VI.pixel_type |= VideoInfo::CS_Sample_Bits_10;
            } else if (VP.VF.Bits == 8 && !VP.VF.Float) {
                VI.pixel_type |= VideoInfo::CS_Sample_Bits_8;
            } else {
                throw BestSourceException("Unsupported output bitdepth");
            }

            VI.image_type = (VP.TFF ? VideoInfo::IT_TFF : VideoInfo::IT_BFF);

            VI.width = VP.SSModWidth;
            VI.height = VP.SSModHeight;

            VI.num_frames = vsh::int64ToIntS(VP.NumFrames);
            VI.SetFPS(VP.FPS.Num, VP.FPS.Den);

            if (FPSNum > 0) {
                vsh::reduceRational(&FPSNum, &FPSDen);
                VI.SetFPS(static_cast<int>(FPSNum), static_cast<int>(FPSDen));
                VI.num_frames = std::max(1, static_cast<int>((VP.Duration * VI.fps_numerator) * VP.TimeBase.ToDouble() / VI.fps_denominator + 0.5));
            } else if (RFF) {
                VI.num_frames = vsh::int64ToIntS(VP.NumRFFFrames);
            }

            V->SetSeekPreRoll(SeekPreRoll);

            if (CacheSize >= 0)
                V->SetMaxCacheSize(CacheSize * 1024 * 1024);

            if (Timecodes)
                V->WriteTimecodes(CreateProbablyUTF8Path(Timecodes));

        } catch (BestSourceException &e) {
            Env->ThrowError("BestVideoSource: %s", e.what());
        }
    }

    bool __stdcall GetParity(int n) {
        return V->GetFrameIsTFF(n, RFF);
    }

    int __stdcall SetCacheHints(int cachehints, int frame_range) {
        return 0;
    }

    const VideoInfo &__stdcall GetVideoInfo() {
        return VI;
    }

    void __stdcall GetAudio(void *Buf, int64_t Start, int64_t Count, IScriptEnvironment *Env) {
    }

    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment *Env) {
        PVideoFrame Dst;

        std::unique_ptr<BestVideoFrame> Src;
        try {
            if (RFF) {
                Src.reset(V->GetFrameWithRFF(std::min(n, VI.num_frames - 1)));
            } else if (FPSNum > 0) {
                double currentTime = V->GetVideoProperties().StartTime +
                    (double)(std::min(n, VI.num_frames - 1) * FPSDen) / FPSNum;
                Src.reset(V->GetFrameByTime(currentTime));
            } else {
                Src.reset(V->GetFrame(std::min(n, VI.num_frames - 1)));
            }

            if (!Src)
                throw BestSourceException("No frame returned for frame number " + std::to_string(n) + ". This may be due to an FFmpeg bug. Retry with threads=1 if not already set.");

            Dst = Env->NewVideoFrame(VI);

            uint8_t *DstPtrs[3] = {};
            ptrdiff_t DstStride[3] = {};

            bool DestHasAlpha = (VI.IsYUVA() || VI.IsPlanarRGBA());

            if (VI.IsYUV() || VI.IsYUVA()) {
                DstPtrs[0] = Dst->GetWritePtr(PLANAR_Y);
                DstStride[0] = Dst->GetPitch(PLANAR_Y);
                DstPtrs[1] = Dst->GetWritePtr(PLANAR_U);
                DstStride[1] = Dst->GetPitch(PLANAR_U);
                DstPtrs[2] = Dst->GetWritePtr(PLANAR_V);
                DstStride[2] = Dst->GetPitch(PLANAR_V);
            } else if (VI.IsRGB() || VI.IsPlanarRGBA()) {
                DstPtrs[0] = Dst->GetWritePtr(PLANAR_R);
                DstStride[0] = Dst->GetPitch(PLANAR_R);
                DstPtrs[1] = Dst->GetWritePtr(PLANAR_G);
                DstStride[1] = Dst->GetPitch(PLANAR_G);
                DstPtrs[2] = Dst->GetWritePtr(PLANAR_B);
                DstStride[2] = Dst->GetPitch(PLANAR_B);
            } else if (VI.IsY()) {
                DstPtrs[0] = Dst->GetWritePtr(PLANAR_Y);
                DstStride[0] = Dst->GetPitch(PLANAR_Y);
            } else {
                assert(false);
            }

            if (!Src->ExportAsPlanar(DstPtrs, DstStride, DestHasAlpha ? Dst->GetWritePtr(PLANAR_A) : nullptr, DestHasAlpha ? Dst->GetPitch(PLANAR_A) : 0)) {
                throw BestSourceException("Cannot export to planar format for frame " + std::to_string(n));
            }

        } catch (BestSourceException &e) {
            Env->ThrowError("BestVideoSource: %s", e.what());
        }

        AVSMap *Props = Env->getFramePropsRW(Dst);

        SetSynthFrameProperties(n, Src, *V, RFF, V->GetFrameIsTFF(n, RFF),
            [Props, Env](const char *Name, int64_t V) { Env->propSetInt(Props, Name, V, 1); },
            [Props, Env](const char *Name, double V) { Env->propSetFloat(Props, Name, V, 1); },
            [Props, Env](const char *Name, const char *V, int Size, bool Utf8) { Env->propSetData(Props, Name, V, Size, 1); });

        return Dst;
    }
};

static AVSValue __cdecl CreateBSVideoSource(AVSValue Args, void *UserData, IScriptEnvironment *Env) {
    BSInit();

    if (!Args[0].Defined())
        Env->ThrowError("BestVideoSource: No source specified");

    const char *Source = Args[0].AsString();
    int Track = Args[1].AsInt(-1);
    int FPSNum = Args[2].AsInt(-1);
    int FPSDen = Args[3].AsInt(1);
    bool RFF = Args[4].AsBool(false);
    int Threads = Args[5].AsInt(-1);
    int SeekPreroll = Args[6].AsInt(1);
    bool EnableDrefs = Args[7].AsBool(false);
    bool UseAbsolutePath = Args[8].AsBool(false);
    int CacheMode = Args[9].AsInt(1);
    const char *CachePath = Args[10].AsString("");
    int CacheSize = Args[11].AsInt(-1);
    const char *HWDevice = Args[12].AsString();
    int ExtraHWFrames = Args[13].AsInt(9);
    const char *Timecodes = Args[14].AsString(nullptr);
    int StartNumber = Args[15].AsInt(-1);
    int VariableFormat = Args[16].AsInt(0);
    int ViewID = Args[17].AsInt(0);
    int MaxDecoders = Args[18].AsInt(0);
    bool HWFallback = Args[19].AsBool(true);

    return new AvisynthVideoSource(Source, Track, ViewID, FPSNum, FPSDen, RFF, Threads, SeekPreroll, EnableDrefs, UseAbsolutePath, CacheMode, CachePath, CacheSize, HWDevice, ExtraHWFrames, Timecodes, StartNumber, VariableFormat, MaxDecoders, HWFallback, Env);
}

class AvisynthAudioSource : public IClip {
    VideoInfo VI = {};
    std::unique_ptr<BestAudioSource> A;
public:
    AvisynthAudioSource(const char *Source, int Track,
        int AdjustDelay, int Threads, bool EnableDrefs, bool UseAbsolutePath, double DrcScale, int CacheMode, const char *CachePath, int CacheSize, int MaxDecoders, IScriptEnvironment *Env) {

        std::map<std::string, std::string> Opts;
        if (EnableDrefs)
            Opts["enable_drefs"] = "1";
        if (UseAbsolutePath)
            Opts["use_absolute_path"] = "1";

        try {
            A.reset(new BestAudioSource(CreateProbablyUTF8Path(Source), Track, AdjustDelay, Threads, CacheMode, CachePath ? CachePath : "", &Opts, DrcScale));

            A->SetMaxDecoderInstances(MaxDecoders);
            A->SelectFormatSet(0);

            const BSAudioProperties &AP = A->GetAudioProperties();
            if (AP.AF.Float && AP.AF.Bits == 32) {
                VI.sample_type = SAMPLE_FLOAT;
            } else if (!AP.AF.Float && AP.AF.Bits <= 8) {
                VI.sample_type = SAMPLE_INT8;
            } else if (!AP.AF.Float && AP.AF.Bits <= 16) {
                VI.sample_type = SAMPLE_INT16;
            } else if (!AP.AF.Float && AP.AF.Bits <= 24) {
                VI.sample_type = SAMPLE_INT24;
            } else if (!AP.AF.Float && AP.AF.Bits <= 32) {
                VI.sample_type = SAMPLE_INT32;
            } else {
                Env->ThrowError("BestAudioSource: Unsupported audio format");
            }

            VI.audio_samples_per_second = AP.SampleRate;
            VI.num_audio_samples = AP.NumSamples;
            VI.nchannels = AP.Channels;
            if (AP.ChannelLayout <= std::numeric_limits<unsigned>::max())
                VI.SetChannelMask(true, static_cast<unsigned>(AP.ChannelLayout));

        } catch (BestSourceException &e) {
            Env->ThrowError("BestAudioSource: %s", e.what());
        }

        if (CacheSize > 0)
            A->SetMaxCacheSize(CacheSize * 1024 * 1024);
    }

    bool __stdcall GetParity(int n) {
        return false;
    }

    int __stdcall SetCacheHints(int cachehints, int frame_range) {
        return 0;
    }

    const VideoInfo &__stdcall GetVideoInfo() {
        return VI;
    }

    void __stdcall GetAudio(void *Buf, int64_t Start, int64_t Count, IScriptEnvironment *Env) {
        try {
            if (VI.sample_type == SAMPLE_INT24) {
                // Avisynth has no way to signal the number of significant bits and instead requires 24bit packed stuff
                std::unique_ptr<uint8_t[]> Tmp(new uint8_t[Count * VI.nchannels * 4]);
                uint8_t *Dst = reinterpret_cast<uint8_t *>(Buf);
                A->GetPackedAudio(reinterpret_cast<uint8_t *>(Tmp.get()), Start, Count);
                for (int64_t i = 0; i < Count * VI.nchannels; i++) {
#ifdef BS_LITTLE_ENDIAN
                    memcpy(Dst, Tmp.get() + i * 4 + 1, 3);
#else
                    memcpy(Dst, Tmp.get() + i * 4, 3);
#endif
                    Dst += 3;
                }
            } else {
                A->GetPackedAudio(reinterpret_cast<uint8_t *>(Buf), Start, Count);
            }
        } catch (BestSourceException &e) {
            Env->ThrowError("BestAudioSource: %s", e.what());
        }
    }

    PVideoFrame __stdcall GetFrame(int n, IScriptEnvironment *Env) {
        return nullptr;
    };
};


static AVSValue __cdecl CreateBSAudioSource(AVSValue Args, void *UserData, IScriptEnvironment *Env) {
    BSInit();

    if (!Args[0].Defined())
        Env->ThrowError("BestAudioSource: No source specified");

    const char *Source = Args[0].AsString();
    int Track = Args[1].AsInt(-1);
    int AdjustDelay = Args[2].AsInt(-1);
    int Threads = Args[3].AsInt(0);
    bool EnableDrefs = Args[4].AsBool(false);
    bool UseAbsolutePath = Args[5].AsBool(false);
    double DrcScale = Args[6].AsFloat(0);
    int CacheMode = Args[7].AsInt(1);
    const char *CachePath = Args[8].AsString("");
    int CacheSize = Args[9].AsInt(-1);
    int MaxDecoders = Args[10].AsInt(0);

    return new AvisynthAudioSource(Source, Track, AdjustDelay, Threads, EnableDrefs, UseAbsolutePath, DrcScale, CacheMode, CachePath, CacheSize, MaxDecoders, Env);
}

// Now some fun magic to parse things from Avisynth arg strings at compile time

static constexpr size_t GetNumAvsArgs(const char *Args) {
    size_t NumArgs = 0;
    while (*Args) {
        if (*Args == '[')
            ++NumArgs;
        ++Args;
    }
    return NumArgs;
}

template<const char Args[]>
static constexpr auto PopulateArgNames() {
    std::array<std::string_view, GetNumAvsArgs(Args)> Result;
    size_t Arg = 0;
    const char *Start = Args + 1;
    const char *Cur = Start;
    while (*Cur) {
        if (*Cur == ']') {
            Result[Arg++] = std::string_view(Start, Cur - Start);
            Start = Cur + 2;
            if (*Start)
                ++Start;
            Cur = Start;
        } else {
            ++Cur;
        }
    }
    return Result;
}

static constexpr char BSVideoSourceAvsArgs[] = "[source]s[track]i[fpsnum]i[fpsden]i[rff]b[threads]i[seekpreroll]i[enable_drefs]b[use_absolute_path]b[cachemode]i[cachepath]s[cachesize]i[hwdevice]s[extrahwframes]i[timecodes]s[start_number]i[variableformat]i[viewid]i[maxdecoders]i[hwfallback]b";
static constexpr char BSAudioSourceAvsArgs[] = "[source]s[track]i[adjustdelay]i[threads]i[enable_drefs]b[use_absolute_path]b[drc_scale]f[cachemode]i[cachepath]s[cachesize]i[maxdecoders]i";
static constexpr char BSSourceAvsArgs[] = "[source]s[atrack]i[vtrack]i[fpsnum]i[fpsden]i[rff]b[threads]i[seekpreroll]i[enable_drefs]b[use_absolute_path]b[cachemode]i[cachepath]s[acachesize]i[vcachesize]i[hwdevice]s[extrahwframes]i[timecodes]s[start_number]i[variableformat]i[adjustdelay]i[drc_scale]f[viewid]i[maxdecoders]i[hwfallback]b";

static constexpr std::array BSVArgNames = PopulateArgNames<BSVideoSourceAvsArgs>();
static constexpr std::array BSAArgNames = PopulateArgNames<BSAudioSourceAvsArgs>();
static constexpr std::array BSArgNames = PopulateArgNames<BSSourceAvsArgs>();

static_assert(BSVArgNames.size() + 4 == BSArgNames.size()); //avtrack, avcachesize, adjustdelay and drc_scale

static constexpr auto GetVideoArgMapping() {
    auto GetArgPos = [](size_t Position) {
        for (size_t i = 0; i < BSArgNames.size(); i++)
            if (BSVArgNames[Position] == BSArgNames[i] || (BSArgNames[i].substr(0, 1) == "v" && BSVArgNames[Position] == BSArgNames[i].substr(1)))
                return static_cast<int>(i);
        return -1;
        };
    std::array<int, BSVArgNames.size()> Result{};
    for (size_t i = 0; i < BSVArgNames.size(); i++)
        Result[i] = GetArgPos(i);
    return Result;
}

static constexpr auto GetAudioArgMapping() {
    auto GetArgPos = [](size_t Position) {
        for (size_t i = 0; i < BSArgNames.size(); i++)
            if (BSAArgNames[Position] == BSArgNames[i] || (BSArgNames[i].substr(0, 1) == "a" && BSAArgNames[Position] == BSArgNames[i].substr(1)))
                return static_cast<int>(i);
        return -1;
        };
    std::array<int, BSAArgNames.size()> Result{};
    for (size_t i = 0; i < BSAArgNames.size(); i++)
        Result[i] = GetArgPos(i);
    return Result;
}

static AVSValue __cdecl CreateBSSource(AVSValue Args, void *UserData, IScriptEnvironment *Env) {
    static constexpr std::array VideoArgMapping = GetVideoArgMapping();
    static constexpr std::array AudioArgMapping = GetAudioArgMapping();

    std::array<AVSValue, VideoArgMapping.size()> BSVArgs;
    for (size_t i = 0; i < VideoArgMapping.size(); i++)
        BSVArgs[i] = Args[VideoArgMapping[i]];

    AVSValue Video = Env->Invoke("BSVideoSource", AVSValue(BSVArgs.data(), static_cast<int>(BSVArgs.size())));

    try {
        // FIXME, adjustdelay should probably be set to vtrack by default to make more sense here but I doubt anyone will ever notice
        std::array<AVSValue, AudioArgMapping.size()> BSAArgs;
        for (size_t i = 0; i < AudioArgMapping.size(); i++)
            BSAArgs[i] = Args[AudioArgMapping[i]];

        AVSValue Audio = Env->Invoke("BSAudioSource", AVSValue(BSAArgs.data(), static_cast<int>(BSAArgs.size())));

        AVSValue AudioDubArgs[] = { Video, Audio };
        return Env->Invoke("AudioDubEx", AVSValue(AudioDubArgs, 2));
    } catch(...) {
        // Only fail on audio errors when atrack is explicitly set
        if (Args[AudioArgMapping[1]].Defined())
            throw;
        return Video;
    }
}

static AVSValue __cdecl BSSetDebugOutput(AVSValue Args, void *UserData, IScriptEnvironment *Env) {
    BSInit();
    SetBSDebugOutput(Args[0].AsBool(false));
    return AVSValue();
}

static AVSValue __cdecl BSSetFFmpegLogLevel(AVSValue Args, void *UserData, IScriptEnvironment *Env) {
    BSInit();
    return SetFFmpegLogLevel(Args[0].AsInt(32));
}

const AVS_Linkage *AVS_linkage = nullptr;

extern "C" AVS_EXPORT const char *__stdcall AvisynthPluginInit3(IScriptEnvironment * Env, const AVS_Linkage *const vectors) {
    AVS_linkage = vectors;

    Env->AddFunction("BSVideoSource", BSVideoSourceAvsArgs, CreateBSVideoSource, nullptr);
    Env->AddFunction("BSAudioSource", BSAudioSourceAvsArgs, CreateBSAudioSource, nullptr);
    Env->AddFunction("BSSource", BSSourceAvsArgs, CreateBSSource, nullptr);
    Env->AddFunction("BSSetDebugOutput", "[enable]b", BSSetDebugOutput, nullptr);
    Env->AddFunction("BSSetFFmpegLogLevel", "[level]i", BSSetFFmpegLogLevel, nullptr);

    return "Best Source 2";
}
