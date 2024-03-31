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
#include "bsshared.h"
#include "version.h"
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
    std::string VarPrefix;
public:
    AvisynthVideoSource(const char *SourceFile, int Track,
        int AFPSNum, int AFPSDen, bool RFF, int Threads, int SeekPreRoll, bool EnableDrefs, bool UseAbsolutePath,
        const char *CachePath, int CacheSize, const char *HWDevice, int ExtraHWFrames,
        const char *Timecodes, const char *VarPrefix, IScriptEnvironment *Env)
        : FPSNum(AFPSNum), FPSDen(AFPSDen), RFF(RFF), VarPrefix(VarPrefix) {

        try {
            if (FPSDen < 1)
                throw VideoException("FPS denominator needs to be 1 or greater");

            if (FPSNum > 0 && RFF)
                throw VideoException("Cannot combine CFR and RFF modes");

            std::map<std::string, std::string> Opts;
            if (EnableDrefs)
                Opts["enable_drefs"] = "1";
            if (UseAbsolutePath)
                Opts["use_absolute_path"] = "1";

            V.reset(new BestVideoSource(SourceFile, HWDevice ? HWDevice : "", ExtraHWFrames, Track, false, Threads, CachePath, &Opts));

            const VideoProperties &VP = V->GetVideoProperties();
            if (VP.VF.ColorFamily == cfGray) {
                VI.pixel_type = VideoInfo::CS_GENERIC_Y;
            } else if (VP.VF.ColorFamily == cfYUV && VP.VF.Alpha) {
                VI.pixel_type = VideoInfo::CS_PLANAR | VideoInfo::CS_YUVA | VideoInfo::CS_VPlaneFirst; // Why is there no generic YUVA constant?
            } else if (VP.VF.ColorFamily == cfYUV) {
                VI.pixel_type = VideoInfo::CS_PLANAR | VideoInfo::CS_YUV | VideoInfo::CS_VPlaneFirst; // Why is there no generic YUV constant?
            } else if (VP.VF.ColorFamily == cfRGB && VP.VF.Alpha) {
                VI.pixel_type = VideoInfo::CS_GENERIC_RGBP;
            } else if (VP.VF.ColorFamily == cfRGB) {
                VI.pixel_type = VideoInfo::CS_GENERIC_RGBAP;
            } else {
                throw VideoException("Unsupported output colorspace");
            }

            if (VP.VF.SubSamplingH == 0) {
                VI.pixel_type |= VideoInfo::CS_Sub_Height_1;
            } else if (VP.VF.SubSamplingH == 1) {
                VI.pixel_type |= VideoInfo::CS_Sub_Height_2;
            } else if (VP.VF.SubSamplingH == 2) {
                VI.pixel_type |= VideoInfo::CS_Sub_Height_4;
            } else {
                throw VideoException("Unsupported output subsampling");
            }

            if (VP.VF.SubSamplingW == 0) {
                VI.pixel_type |= VideoInfo::CS_Sub_Width_1;
            } else if (VP.VF.SubSamplingW == 1) {
                VI.pixel_type |= VideoInfo::CS_Sub_Width_2;
            } else if (VP.VF.SubSamplingW == 2) {
                VI.pixel_type |= VideoInfo::CS_Sub_Width_4;
            } else {
                throw VideoException("Unsupported output subsampling");
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
                throw VideoException("Unsupported output bitdepth");
            }

            // FIXME, set TFF flag too?

            VI.width = VP.Width;
            VI.height = VP.Height;

            // Crop to obey subsampling width/height requirements
            VI.width -= VI.width % (1 << VP.VF.SubSamplingW);
            VI.height -= VI.height % (1 << VP.VF.SubSamplingH);
            VI.num_frames = vsh::int64ToIntS(VP.NumFrames);
            VI.SetFPS(VP.FPS.Num, VP.FPS.Den);

            if (FPSNum > 0) {
                vsh::reduceRational(&FPSNum, &FPSDen);
                if (VP.FPS.Den != FPSDen || VP.FPS.Num != FPSNum) {
                    VI.SetFPS(FPSNum, FPSDen);
                    VI.num_frames = std::max(1, static_cast<int>((VP.Duration * VI.fps_numerator) / VI.fps_denominator));
                } else {
                    FPSNum = -1;
                    FPSDen = 1;
                }
            } else if (RFF) {
                VI.num_frames = vsh::int64ToIntS(VP.NumRFFFrames);
            }

            V->SetSeekPreRoll(SeekPreRoll);

            if (CacheSize >= 0)
                V->SetMaxCacheSize(CacheSize * 1024 * 1024);

            if (Timecodes)
                V->WriteTimecodes(Timecodes);

            // FIXME, does anyone use this?
            // Set AR variables
            Env->SetVar(Env->Sprintf("%s%s", this->VarPrefix, "BSSAR_NUM"), VP.SAR.Num);
            Env->SetVar(Env->Sprintf("%s%s", this->VarPrefix, "BSSAR_DEN"), VP.SAR.Den);
            if (VP.SAR.Num > 0 && VP.SAR.Den > 0)
                Env->SetVar(Env->Sprintf("%s%s", this->VarPrefix, "BSSAR"), VP.SAR.Num / static_cast<double>(VP.SAR.Den));

            /*
            FIXME, is this even relevant?
            // Set crop variables
            Env->SetVar(Env->Sprintf("%s%s", this->VarPrefix, "BSCROP_LEFT"), VP->CropLeft);
            Env->SetVar(Env->Sprintf("%s%s", this->VarPrefix, "BSCROP_RIGHT"), VP->CropRight);
            Env->SetVar(Env->Sprintf("%s%s", this->VarPrefix, "BSCROP_TOP"), VP->CropTop);
            Env->SetVar(Env->Sprintf("%s%s", this->VarPrefix, "BSCROP_BOTTOM"), VP->CropBottom);
            */
        } catch (VideoException &e) {
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
                throw VideoException("No frame returned for frame number " + std::to_string(n) + ". This may be due to an FFmpeg bug. Delete index and retry with threads=1.");

            Dst = Env->NewVideoFrame(VI);

            uint8_t *DstPtrs[3] = { Dst->GetWritePtr() };
            ptrdiff_t DstStride[3] = { Dst->GetPitch() };

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
                throw VideoException("Cannot export to planar format for frame " + std::to_string(n));
            }

        } catch (VideoException &e) {
            Env->ThrowError("BestVideoSource: %s", e.what());
        }

        const VideoProperties &VP = V->GetVideoProperties();
        AVSMap *Props = Env->getFramePropsRW(Dst);

        if (VP.SAR.Num > 0 && VP.SAR.Den > 0) {
            Env->propSetInt(Props, "_SARNum", VP.SAR.Num, 0);
            Env->propSetInt(Props, "_SARDen", VP.SAR.Den, 0);
        }

        Env->propSetInt(Props, "_Matrix", Src->Matrix, 0);
        Env->propSetInt(Props, "_Primaries", Src->Primaries, 0);
        Env->propSetInt(Props, "_Transfer", Src->Transfer, 0);
        if (Src->ChromaLocation > 0)
            Env->propSetInt(Props, "_ChromaLocation", Src->ChromaLocation - 1, 0);

        if (Src->ColorRange == 1) // Hardcoded ffmpeg constants, nothing to see here
            Env->propSetInt(Props, "_ColorRange", 1, 0);
        else if (Src->ColorRange == 2)
            Env->propSetInt(Props, "_ColorRange", 0, 0);
        Env->propSetData(Props, "_PictType", &Src->PictType, 1, 0);

        // Set field information
        int FieldBased = 0;
        if (Src->InterlacedFrame)
            FieldBased = (Src->TopFieldFirst ? 2 : 1);
        Env->propSetInt(Props, "_FieldBased", FieldBased, 0);

        if (Src->HasMasteringDisplayPrimaries) {
            for (int i = 0; i < 3; i++) {
                Env->propSetFloat(Props, "MasteringDisplayPrimariesX", Src->MasteringDisplayPrimaries[i][0].ToDouble(), 1);
                Env->propSetFloat(Props, "MasteringDisplayPrimariesY", Src->MasteringDisplayPrimaries[i][1].ToDouble(), 1);
            }
            Env->propSetFloat(Props, "MasteringDisplayWhitePointX", Src->MasteringDisplayWhitePoint[0].ToDouble(), 0);
            Env->propSetFloat(Props, "MasteringDisplayWhitePointY", Src->MasteringDisplayWhitePoint[1].ToDouble(), 0);
        }

        if (Src->HasMasteringDisplayLuminance) {
            Env->propSetFloat(Props, "MasteringDisplayMinLuminance", Src->MasteringDisplayMinLuminance.ToDouble(), 0);
            Env->propSetFloat(Props, "MasteringDisplayMaxLuminance", Src->MasteringDisplayMaxLuminance.ToDouble(), 0);
        }

        if (Src->HasContentLightLevel) {
            Env->propSetFloat(Props, "ContentLightLevelMax", Src->ContentLightLevelMax, 0);
            Env->propSetFloat(Props, "ContentLightLevelAverage", Src->ContentLightLevelAverage, 0);
        }

        if (Src->DolbyVisionRPU && Src->DolbyVisionRPUSize > 0) {
            Env->propSetData(Props, "DolbyVisionRPU", reinterpret_cast<const char *>(Src->DolbyVisionRPU), Src->DolbyVisionRPUSize, 0);
        }

        if (Src->HDR10Plus && Src->HDR10PlusSize > 0) {
            Env->propSetData(Props, "HDR10Plus", reinterpret_cast<const char *>(Src->HDR10Plus), Src->HDR10PlusSize, 0);
        }

        Env->propSetInt(Props, "FlipVertical", VP.FlipVerical, 0);
        Env->propSetInt(Props, "FlipHorizontal", VP.FlipHorizontal, 0);
        Env->propSetInt(Props, "Rotation", VP.Rotation, 0);

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
    const char *CachePath = Args[9].AsString("");
    int CacheSize = Args[10].AsInt(-1);
    const char *HWDevice = Args[11].AsString();
    int ExtraHWFrames = Args[12].AsInt(9);
    const char *Timecodes = Args[13].AsString(nullptr);
    const char *VarPrefix = Args[14].AsString("");

    return new AvisynthVideoSource(Source, Track, FPSNum, FPSDen, RFF, Threads, SeekPreroll, EnableDrefs, UseAbsolutePath, CachePath, CacheSize, HWDevice, ExtraHWFrames, Timecodes, VarPrefix, Env);
}

class AvisynthAudioSource : public IClip {
    VideoInfo VI = {};
    std::unique_ptr<BestAudioSource> A;
public:
    AvisynthAudioSource(const char *Source, int Track,
        int AdjustDelay, int Threads, bool EnableDrefs, bool UseAbsolutePath, double DrcScale, const char *CachePath, int CacheSize, IScriptEnvironment *Env) {

        std::map<std::string, std::string> Opts;
        if (EnableDrefs)
            Opts["enable_drefs"] = "1";
        if (UseAbsolutePath)
            Opts["use_absolute_path"] = "1";

        try {
            A.reset(new BestAudioSource(Source, Track, AdjustDelay, false, Threads, CachePath ? CachePath : "", &Opts, DrcScale));

            const AudioProperties &AP = A->GetAudioProperties();
            if (AP.AF.Float && AP.AF.Bits == 32) {
                VI.sample_type = SAMPLE_FLOAT;
            } else if (!AP.AF.Float && AP.AF.Bits <= 8) {
                VI.sample_type = SAMPLE_INT8;
            } else if (!AP.AF.Float && AP.AF.Bits <= 16) {
                VI.sample_type = SAMPLE_INT16;
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
            
        } catch (AudioException &e) {
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
                A->GetPackedAudio(reinterpret_cast<uint8_t *>(Buf), Start, Count);
            } catch (AudioException &e) {
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
    const char *CachePath = Args[7].AsString("");
    int CacheSize = Args[8].AsInt(-1);

    return new AvisynthAudioSource(Source, Track, AdjustDelay, Threads, EnableDrefs, UseAbsolutePath, DrcScale, CachePath, CacheSize, Env);
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

    Env->AddFunction("BSVideoSource", "[source]s[track]i[fpsnum]i[fpsden]i[rff]b[threads]i[seekpreroll]i[enable_drefs]b[use_absolute_path]b[cachepath]s[cachesize]i[hwdevice]s[extrahwframes]i[timecodes]s[varprefix]s", CreateBSVideoSource, nullptr);
    Env->AddFunction("BSAudioSource", "[source]s[track]i[adjustdelay]i[threads]i[enable_drefs]b[use_absolute_path]b[drc_scale]f[cachepath]s[cachesize]i", CreateBSAudioSource, nullptr);
    Env->AddFunction("BSSetDebugOutput", "b[enable]", BSSetDebugOutput, nullptr);
    Env->AddFunction("BSSetFFmpegLogLevel", "i[level]", BSSetFFmpegLogLevel, nullptr);

    return "Best Source 2";
}
