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
#include <avisynth.h>
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

static std::string DescribeYUVFormat(const BSVideoFormat &VF) {
    static constexpr struct { int W; int H; const char *Name; } Layouts[] = {
        { 0, 0, "4:4:4" }, { 1, 0, "4:2:2" }, { 1, 1, "4:2:0" },
        { 2, 0, "4:1:1" }, { 2, 2, "4:1:0" }, { 0, 1, "4:4:0" },
    };

    std::string Subsampling;
    for (const auto &Iter : Layouts)
        if (Iter.W == VF.SubSamplingW && Iter.H == VF.SubSamplingH)
            Subsampling = Iter.Name;
    if (Subsampling.empty())
        Subsampling = "chroma subsampled " + std::to_string(1 << VF.SubSamplingW) + "x" + std::to_string(1 << VF.SubSamplingH);

    return std::to_string(VF.Bits) + (VF.Float ? " bit float " : " bit ") + Subsampling + (VF.Alpha ? " YUVA" : " YUV");
}

class AvisynthVideoSource : public IClip {
    VideoInfo VI = {};
    std::unique_ptr<BestVideoSource> V;
    int64_t FPSNum;
    int64_t FPSDen;
    bool RFF;
    bool RFFIsUsed;
    bool ApplyRotation;
public:
    AvisynthVideoSource(const char *RawSource, int Track, int ViewID,
        int AFPSNum, int AFPSDen, bool RFF, int Threads, int SeekPreRoll, bool EnableDrefs, bool UseAbsolutePath,
        int CacheMode, const char *RawCachePath, int CacheSize,
        const char *Timecodes, int StartNumber, int VariableFormat, int MaxDecoders, bool ApplyRotation, IScriptEnvironment *Env)
        : FPSNum(AFPSNum), FPSDen(AFPSDen), RFF(RFF), ApplyRotation(ApplyRotation) {

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

            std::filesystem::path Source(CreateProbablyUTF8Path(RawSource));
            std::filesystem::path CachePath(CreateProbablyUTF8Path(RawCachePath));

            /* Avisynth+ has no GPU resident frames, so there is no hardware decoding here: it
               would have to read every frame back, which is what decoding on the CPU already
               does without the round trip. */
            V.reset(new BestVideoSource(Source, false, "", 0, Track, ViewID, Threads, CacheMode, CachePath, &Opts));

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
                // Full list of actually supported avs+ formats can be foud in the source code here:
                // https://github.com/AviSynth/AviSynthPlus/blob/master/avs_core/core/avisynth.cpp#L4377
                const bool Supported =
                    (VP.VF.SubSamplingW == 0 && VP.VF.SubSamplingH == 0) ||
                    (VP.VF.SubSamplingW == 1 && VP.VF.SubSamplingH == 0) ||
                    (VP.VF.SubSamplingW == 1 && VP.VF.SubSamplingH == 1) ||
                    (VP.VF.SubSamplingW == 2 && VP.VF.SubSamplingH == 0 && VP.VF.Bits == 8 && !VP.VF.Float && !VP.VF.Alpha);

                if (!Supported)
                    throw BestSourceException("Avisynth+ has no " + DescribeYUVFormat(VP.VF) + " pixel format, only 4:2:0, 4:2:2 and 4:4:4 at every bitdepth and 8 bit 4:1:1 without alpha can be output");

                VI.pixel_type |= (VP.VF.SubSamplingH == 0) ? VideoInfo::CS_Sub_Height_1 : VideoInfo::CS_Sub_Height_2;

                if (VP.VF.SubSamplingW == 0) {
                    VI.pixel_type |= VideoInfo::CS_Sub_Width_1;
                } else if (VP.VF.SubSamplingW == 1) {
                    VI.pixel_type |= VideoInfo::CS_Sub_Width_2;
                } else {
                    VI.pixel_type |= VideoInfo::CS_Sub_Width_4;
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

            RFFIsUsed = (VP.NumFrames != VP.NumRFFFrames);

            V->SetSeekPreRoll(SeekPreRoll);

            if (CacheSize >= 0)
                V->SetMaxCacheSize(static_cast<size_t>(CacheSize) * 1024 * 1024);

            if (Timecodes)
                V->WriteTimecodes(CreateProbablyUTF8Path(Timecodes));

        } catch (const std::exception &e) {
            Env->ThrowError("BestVideoSource: %s", e.what());
        }
    }

    const BSVideoProperties &GetSourceVideoProperties() const {
        return V->GetVideoProperties();
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

        } catch (const std::exception &e) {
            Env->ThrowError("BestVideoSource: %s", e.what());
        }

        AVSMap *Props = Env->getFramePropsRW(Dst);

        SetSynthFrameProperties(n, Src, *V, RFFIsUsed, V->GetFrameIsTFF(n, RFF), ApplyRotation,
            [Props, Env](const char *Name, int64_t V) { Env->propSetInt(Props, Name, V, 1); },
            [Props, Env](const char *Name, double V) { Env->propSetFloat(Props, Name, V, 1); },
            [Props, Env](const char *Name, const char *V, int Size, bool Utf8) { Env->propSetData(Props, Name, V, Size, 1); });

        return Dst;
    }
};

static AVSValue InvokeOnClip(const char *Name, AVSValue Clip, IScriptEnvironment *Env) {
    return Env->Invoke(Name, AVSValue(&Clip, 1));
}

static AVSValue ApplyOrientation(AVSValue Clip, const BSVideoProperties &VP, IScriptEnvironment *Env) {
    if (VP.FlipVertical)
        Clip = InvokeOnClip("FlipVertical", Clip, Env);

    if (VP.Rotation == 90)
        Clip = InvokeOnClip("TurnRight", Clip, Env);
    else if (VP.Rotation == 180)
        Clip = InvokeOnClip("Turn180", Clip, Env);
    else if (VP.Rotation == 270)
        Clip = InvokeOnClip("TurnLeft", Clip, Env);
    else if (VP.Rotation != 0)
        Env->ThrowError("BestVideoSource: Can't apply the %d degree rotation of the video, only multiples of 90 are supported, set apply_rotation=false to get the untransformed video", VP.Rotation);

    return Clip;
}

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
    const char *Timecodes = Args[12].AsString(nullptr);
    int StartNumber = Args[13].AsInt(-1);
    int VariableFormat = Args[14].AsInt(0);
    int ViewID = Args[15].AsInt(0);
    int MaxDecoders = Args[16].AsInt(0);
    bool ApplyRotation = Args[17].AsBool(true);

    AvisynthVideoSource *VideoSource = new AvisynthVideoSource(Source, Track, ViewID, FPSNum, FPSDen, RFF, Threads, SeekPreroll, EnableDrefs, UseAbsolutePath, CacheMode, CachePath, CacheSize, Timecodes, StartNumber, VariableFormat, MaxDecoders, ApplyRotation, Env);
    AVSValue Clip = VideoSource;

    if (ApplyRotation)
        Clip = ApplyOrientation(Clip, VideoSource->GetSourceVideoProperties(), Env);

    return Clip;
}

class AvisynthAudioSource : public IClip {
    VideoInfo VI = {};
    std::unique_ptr<BestAudioSource> A;
public:
    AvisynthAudioSource(const char *RawSource, int Track,
        int AdjustDelay, int Threads, bool EnableDrefs, bool UseAbsolutePath, double DrcScale, int CacheMode, const char *RawCachePath, int CacheSize, int MaxDecoders, int VariableFormat, IScriptEnvironment *Env) {

        if (VariableFormat < 0)
            Env->ThrowError("BestAudioSource: Variable format number must be 0 or greater");

        std::map<std::string, std::string> Opts;
        if (EnableDrefs)
            Opts["enable_drefs"] = "1";
        if (UseAbsolutePath)
            Opts["use_absolute_path"] = "1";

        std::filesystem::path Source(CreateProbablyUTF8Path(RawSource));
        std::filesystem::path CachePath(CreateProbablyUTF8Path(RawCachePath));

        try {
            A.reset(new BestAudioSource(Source, Track, AdjustDelay, Threads, CacheMode, CachePath, &Opts, DrcScale));

            A->SetMaxDecoderInstances(MaxDecoders);
            A->SelectFormatSet(VariableFormat);

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

        } catch (const std::exception &e) {
            Env->ThrowError("BestAudioSource: %s", e.what());
        }

        if (CacheSize > 0)
            A->SetMaxCacheSize(static_cast<size_t>(CacheSize) * 1024 * 1024);
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
        } catch (const std::exception &e) {
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
    int VariableFormat = Args[11].AsInt(0);

    return new AvisynthAudioSource(Source, Track, AdjustDelay, Threads, EnableDrefs, UseAbsolutePath, DrcScale, CacheMode, CachePath, CacheSize, MaxDecoders, VariableFormat, Env);
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

static constexpr char BSVideoSourceAvsArgs[] = "[source]s[track]i[fpsnum]i[fpsden]i[rff]b[threads]i[seekpreroll]i[enable_drefs]b[use_absolute_path]b[cachemode]i[cachepath]s[cachesize]i[timecodes]s[start_number]i[variableformat]i[viewid]i[maxdecoders]i[apply_rotation]b";
static constexpr char BSAudioSourceAvsArgs[] = "[source]s[track]i[adjustdelay]i[threads]i[enable_drefs]b[use_absolute_path]b[drc_scale]f[cachemode]i[cachepath]s[cachesize]i[maxdecoders]i[variableformat]i";
static constexpr char BSSourceAvsArgs[] = "[source]s[atrack]i[vtrack]i[fpsnum]i[fpsden]i[rff]b[threads]i[seekpreroll]i[enable_drefs]b[use_absolute_path]b[cachemode]i[cachepath]s[acachesize]i[vcachesize]i[timecodes]s[start_number]i[vvariableformat]i[adjustdelay]i[drc_scale]f[viewid]i[maxdecoders]i[apply_rotation]b[avariableformat]i";

static constexpr std::array BSVArgNames = PopulateArgNames<BSVideoSourceAvsArgs>();
static constexpr std::array BSAArgNames = PopulateArgNames<BSAudioSourceAvsArgs>();
static constexpr std::array BSArgNames = PopulateArgNames<BSSourceAvsArgs>();

static_assert(BSVArgNames.size() + 5 == BSArgNames.size()); //avtrack, avcachesize, avariableformat, adjustdelay and drc_scale

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

// Nothing may be left unmapped, an unmapped argument would index Args with -1 at runtime.
static_assert([]() { for (int i : GetVideoArgMapping()) if (i < 0) return false; return true; }());
static_assert([]() { for (int i : GetAudioArgMapping()) if (i < 0) return false; return true; }());

template<typename T>
static constexpr size_t IndexOfArg(const T &Names, std::string_view Name) {
    for (size_t i = 0; i < Names.size(); i++)
        if (Names[i] == Name)
            return i;
    return static_cast<size_t>(-1);
}

static_assert(BSArgNames[GetVideoArgMapping()[IndexOfArg(BSVArgNames, "variableformat")]] == "vvariableformat");
static_assert(BSArgNames[GetAudioArgMapping()[IndexOfArg(BSAArgNames, "variableformat")]] == "avariableformat");
static_assert(BSArgNames[GetVideoArgMapping()[IndexOfArg(BSVArgNames, "track")]] == "vtrack");
static_assert(BSArgNames[GetAudioArgMapping()[IndexOfArg(BSAArgNames, "track")]] == "atrack");
static_assert(BSArgNames[GetVideoArgMapping()[IndexOfArg(BSVArgNames, "cachesize")]] == "vcachesize");
static_assert(BSArgNames[GetAudioArgMapping()[IndexOfArg(BSAArgNames, "cachesize")]] == "acachesize");

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
