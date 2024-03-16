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
#include <algorithm>
#include <thread>
#include <set>
#include <array>
#include <cassert>

#include "../libp2p/p2p_api.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavutil/stereo3d.h>
#include <libavutil/display.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/hdr_dynamic_metadata.h>
#include <libavutil/hash.h>
}

static bool GetSampleTypeIsFloat(const AVPixFmtDescriptor *Desc) {
    return !!(Desc->flags & AV_PIX_FMT_FLAG_FLOAT);
}

static bool HasAlpha(const AVPixFmtDescriptor *Desc) {
    return !!(Desc->flags & AV_PIX_FMT_FLAG_ALPHA);
}

static int GetColorFamily(const AVPixFmtDescriptor *Desc) {
    if (Desc->nb_components <= 2)
        return 1;
    else if (Desc->flags & AV_PIX_FMT_FLAG_RGB)
        return 2;
    else
        return 3;
}

static int GetBitDepth(const AVPixFmtDescriptor *Desc) {
    return Desc->comp[0].depth;
}

static int IsRealPlanar(const AVPixFmtDescriptor *Desc) {
    int MaxPlane = 0;
    for (int i = 0; i < Desc->nb_components; i++)
        MaxPlane = std::max(MaxPlane, Desc->comp[i].plane);
    return (MaxPlane + 1) == Desc->nb_components;
}

bool LWVideoDecoder::ReadPacket(AVPacket *Packet) {
    while (av_read_frame(FormatContext, Packet) >= 0) {
        if (Packet->stream_index == TrackNumber)
            return true;
        av_packet_unref(Packet);
    }
    return false;
}

bool LWVideoDecoder::DecodeNextFrame(bool SkipOutput) {
    if (!DecodeFrame) {
        DecodeFrame = av_frame_alloc();
        if (!DecodeFrame)
            throw VideoException("Couldn't allocate frame");
    }

    while (true) {
        int Ret = avcodec_receive_frame(CodecContext, HWMode ? HWFrame : DecodeFrame);
        if (Ret == 0) {
            if (HWMode) {
                if (!SkipOutput) {
                    av_hwframe_transfer_data(DecodeFrame, HWFrame, 0);
                    av_frame_copy_props(DecodeFrame, HWFrame);
                }
            }
            return true;
        } else if (Ret == AVERROR(EAGAIN)) {
            if (ResendPacket || ReadPacket(Packet)) {
                int SendRet = avcodec_send_packet(CodecContext, Packet);
                ResendPacket = (SendRet == AVERROR(EAGAIN));
                if (!ResendPacket)
                    av_packet_unref(Packet);
            } else {
                avcodec_send_packet(CodecContext, nullptr);
            }
        } else {
            break; // Probably EOF or some unrecoverable error so stop here
        }
    }

    return false;
}

void LWVideoDecoder::OpenFile(const std::string &SourceFile, const std::string &HWDeviceName, int ExtraHWFrames, int Track, bool VariableFormat, int Threads, const std::map<std::string, std::string> &LAVFOpts) {
    TrackNumber = Track;

    AVHWDeviceType Type = AV_HWDEVICE_TYPE_NONE;
    if (!HWDeviceName.empty()) {
        Type = av_hwdevice_find_type_by_name(HWDeviceName.c_str());
        if (Type == AV_HWDEVICE_TYPE_NONE)
            throw VideoException("Unknown HW device: " + HWDeviceName);
    }

    HWMode = (Type != AV_HWDEVICE_TYPE_NONE);

    AVDictionary *Dict = nullptr;
    for (const auto &Iter : LAVFOpts)
        av_dict_set(&Dict, Iter.first.c_str(), Iter.second.c_str(), 0);

    if (avformat_open_input(&FormatContext, SourceFile.c_str(), nullptr, &Dict) != 0)
        throw VideoException("Couldn't open '" + SourceFile + "'");

    av_dict_free(&Dict);

    if (avformat_find_stream_info(FormatContext, nullptr) < 0) {
        avformat_close_input(&FormatContext);
        FormatContext = nullptr;
        throw VideoException("Couldn't find stream information");
    }

    if (TrackNumber < 0) {
        for (int i = 0; i < static_cast<int>(FormatContext->nb_streams); i++) {
            if (FormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (TrackNumber == -1) {
                    TrackNumber = i;
                    break;
                } else {
                    TrackNumber++;
                }
            }
        }
    }

    if (TrackNumber < 0 || TrackNumber >= static_cast<int>(FormatContext->nb_streams))
        throw VideoException("Invalid track index");

    if (FormatContext->streams[TrackNumber]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
        throw VideoException("Not a video track");

    for (int i = 0; i < static_cast<int>(FormatContext->nb_streams); i++)
        if (i != TrackNumber)
            FormatContext->streams[i]->discard = AVDISCARD_ALL;

    const AVCodec *Codec = nullptr;
    if (HWMode && FormatContext->streams[TrackNumber]->codecpar->codec_id == AV_CODEC_ID_AV1)
        Codec = avcodec_find_decoder_by_name("av1");
    else
        Codec = avcodec_find_decoder(FormatContext->streams[TrackNumber]->codecpar->codec_id);

    if (Codec == nullptr)
        throw VideoException("Video codec not found");

    AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
    if (HWMode) {
        for (int i = 0;; i++) {
            const AVCodecHWConfig *Config = avcodec_get_hw_config(Codec, i);
            if (!Config)
                throw VideoException("Decoder " + std::string(Codec->name) + " does not support device type " + av_hwdevice_get_type_name(Type));
            if (Config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX &&
                Config->device_type == Type) {
                hw_pix_fmt = Config->pix_fmt;
                break;
            }
        }
    }

    CodecContext = avcodec_alloc_context3(Codec);
    if (CodecContext == nullptr)
        throw VideoException("Could not allocate video decoding context");

    if (avcodec_parameters_to_context(CodecContext, FormatContext->streams[TrackNumber]->codecpar) < 0)
        throw VideoException("Could not copy video codec parameters");

    if (Threads < 1) {
        int HardwareConcurrency = std::thread::hardware_concurrency();
        if (Type != AV_HWDEVICE_TYPE_CUDA)
            Threads = std::min(HardwareConcurrency, 16);
        else if (CodecContext->codec_id == AV_CODEC_ID_H264)
            Threads = 1;
        else
            Threads = std::min(HardwareConcurrency, 2);
    }
    CodecContext->thread_count = Threads;

    if (!VariableFormat) {
        // Probably guard against mid-stream format changes
        CodecContext->flags |= AV_CODEC_FLAG_DROPCHANGED;
    }

    // Full explanation by more clever person available here: https://github.com/Nevcairiel/LAVFilters/issues/113
    if (CodecContext->codec_id == AV_CODEC_ID_H264 && CodecContext->has_b_frames) {
        CodecContext->has_b_frames = 15; // the maximum possible value for h264

        if (HWMode)
            CodecContext->extra_hw_frames = ExtraHWFrames;
    }

    if (HWMode) {
        CodecContext->pix_fmt = hw_pix_fmt;
        if (av_hwdevice_ctx_create(&HWDeviceContext, Type, nullptr, nullptr, 0) < 0)
            throw VideoException("Failed to create specified HW device");
        CodecContext->hw_device_ctx = av_buffer_ref(HWDeviceContext);

        HWFrame = av_frame_alloc();
        if (!HWFrame)
                throw VideoException("Couldn't allocate frame");
    }

    if (avcodec_open2(CodecContext, Codec, nullptr) < 0)
        throw VideoException("Could not open video codec");
}

LWVideoDecoder::LWVideoDecoder(const std::string &SourceFile, const std::string &HWDeviceName, int ExtraHWFrames, int Track, bool VariableFormat, int Threads, const std::map<std::string, std::string> &LAVFOpts) {
    try {
        Packet = av_packet_alloc();
        OpenFile(SourceFile, HWDeviceName, ExtraHWFrames, Track, VariableFormat, Threads, LAVFOpts);
    } catch (...) {
        Free();
        throw;
    }
}

void LWVideoDecoder::Free() {
    av_packet_free(&Packet);
    av_frame_free(&DecodeFrame);
    av_frame_free(&HWFrame);
    avcodec_free_context(&CodecContext);
    avformat_close_input(&FormatContext);
    av_buffer_unref(&HWDeviceContext);
}

LWVideoDecoder::~LWVideoDecoder() {
    Free();
}

int64_t LWVideoDecoder::GetSourceSize() const {
    return avio_size(FormatContext->pb);
}

int64_t LWVideoDecoder::GetSourcePostion() const {
    return avio_tell(FormatContext->pb);
}

int LWVideoDecoder::GetTrack() const {
    return TrackNumber;
}

int64_t LWVideoDecoder::GetFrameNumber() const {
    return CurrentFrame;
}

void LWVideoDecoder::SetFrameNumber(int64_t N) {
    CurrentFrame = N;
}

void LWVideoDecoder::GetVideoProperties(VideoProperties &VP) const {
    assert(CurrentFrame != 0);
    VP = {};

    VP.Width = CodecContext->width;
    VP.Height = CodecContext->height;
    VP.PixFmt = CodecContext->pix_fmt;
    VP.VF.Set(av_pix_fmt_desc_get(static_cast<AVPixelFormat>(DecodeFrame->format)));

    VP.FPS = CodecContext->framerate;
    // Set the framerate from the container if the codec framerate is invalid
    if (VP.FPS.Num <= 0 || VP.FPS.Den <= 0)
        VP.FPS = FormatContext->streams[TrackNumber]->r_frame_rate;

    VP.Duration = FormatContext->streams[TrackNumber]->duration;
    VP.TimeBase = FormatContext->streams[TrackNumber]->time_base;

    VP.NumFrames = FormatContext->streams[TrackNumber]->nb_frames;
    if (VP.NumFrames <= 0 && VP.Duration > 0) {
        if (VP.FPS.Num)
            VP.NumFrames = (VP.Duration * VP.FPS.Num) / VP.FPS.Den;
    }

    if (VP.NumFrames <= 0)
        VP.NumFrames = -1;

    // sanity check framerate
    if (VP.FPS.Den <= 0 || VP.FPS.Num <= 0) {
        VP.FPS.Den = 1;
        VP.FPS.Num = 30;
    }

    if (DecodeFrame->pts != AV_NOPTS_VALUE)
        VP.StartTime = (static_cast<double>(FormatContext->streams[TrackNumber]->time_base.num) * DecodeFrame->pts) / FormatContext->streams[TrackNumber]->time_base.den;

    // Set AR variables
    VP.SAR = CodecContext->sample_aspect_ratio;

    // Set the SAR from the container if the codec SAR is invalid
    if (VP.SAR.Num <= 0 || VP.SAR.Den <= 0)
        VP.SAR = FormatContext->streams[TrackNumber]->sample_aspect_ratio;

    // Set stereoscopic 3d type
    VP.Stereo3DType = AV_STEREO3D_2D;

    for (int i = 0; i < FormatContext->streams[TrackNumber]->codecpar->nb_coded_side_data; i++) {
        if (FormatContext->streams[TrackNumber]->codecpar->coded_side_data[i].type == AV_PKT_DATA_STEREO3D) {
            const AVStereo3D *StereoSideData = (const AVStereo3D *)FormatContext->streams[TrackNumber]->codecpar->coded_side_data[i].data;
            VP.Stereo3DType = StereoSideData->type;
            VP.Stereo3DFlags = StereoSideData->flags;
        } else if (FormatContext->streams[TrackNumber]->codecpar->coded_side_data[i].type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA) {
            const AVMasteringDisplayMetadata *MasteringDisplay = (const AVMasteringDisplayMetadata *)FormatContext->streams[TrackNumber]->codecpar->coded_side_data[i].data;
            if (MasteringDisplay->has_primaries) {
                VP.HasMasteringDisplayPrimaries = !!MasteringDisplay->has_primaries;
                for (int i = 0; i < 3; i++) {
                    VP.MasteringDisplayPrimaries[i][0] = MasteringDisplay->display_primaries[i][0];
                    VP.MasteringDisplayPrimaries[i][1] = MasteringDisplay->display_primaries[i][1];
                }
                VP.MasteringDisplayWhitePoint[0] = MasteringDisplay->white_point[0];
                VP.MasteringDisplayWhitePoint[1] = MasteringDisplay->white_point[1];
            }
            if (MasteringDisplay->has_luminance) {
                VP.HasMasteringDisplayLuminance = !!MasteringDisplay->has_luminance;
                VP.MasteringDisplayMinLuminance = MasteringDisplay->min_luminance;
                VP.MasteringDisplayMaxLuminance = MasteringDisplay->max_luminance;
            }

            VP.HasMasteringDisplayPrimaries = !!VP.MasteringDisplayPrimaries[0][0].Num && !!VP.MasteringDisplayPrimaries[0][1].Num &&
                !!VP.MasteringDisplayPrimaries[1][0].Num && !!VP.MasteringDisplayPrimaries[1][1].Num &&
                !!VP.MasteringDisplayPrimaries[2][0].Num && !!VP.MasteringDisplayPrimaries[2][1].Num &&
                !!VP.MasteringDisplayWhitePoint[0].Num && !!VP.MasteringDisplayWhitePoint[1].Num;
            /* MasteringDisplayMinLuminance can be 0 */
            VP.HasMasteringDisplayLuminance = !!VP.MasteringDisplayMaxLuminance.Num;
        } else if (FormatContext->streams[TrackNumber]->codecpar->coded_side_data[i].type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL) {
            const AVContentLightMetadata *ContentLightLevel = (const AVContentLightMetadata *)FormatContext->streams[TrackNumber]->codecpar->coded_side_data[i].data;

            VP.ContentLightLevelMax = ContentLightLevel->MaxCLL;
            VP.ContentLightLevelAverage = ContentLightLevel->MaxFALL;

            /* Only check for either of them */
            VP.HasContentLightLevel = !!VP.ContentLightLevelMax || !!VP.ContentLightLevelAverage;
        }
    }

    /////////////////////////
    // Set rotation
    const int32_t *ConstRotationMatrix = reinterpret_cast<const int32_t *>(av_packet_side_data_get(FormatContext->streams[TrackNumber]->codecpar->coded_side_data, FormatContext->streams[TrackNumber]->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX));
    if (ConstRotationMatrix) {
        int32_t RotationMatrix[9];
        memcpy(RotationMatrix, ConstRotationMatrix, sizeof(RotationMatrix));
        int64_t det = (int64_t)RotationMatrix[0] * RotationMatrix[4] - (int64_t)RotationMatrix[1] * RotationMatrix[3];
        if (det < 0) {
            /* Always assume an horizontal flip for simplicity, it can be changed later if rotation is 180. */
            VP.FlipHorizontal = true;

            /* Flip the matrix to decouple flip and rotation operations. */
            av_display_matrix_flip(RotationMatrix, 1, 0);
        }

        int rot = lround(av_display_rotation_get(RotationMatrix));

        if (rot == 180 && det < 0) {
            /* This is a vertical flip with no rotation. */
            VP.FlipVerical = true;
        } else {
            /* It is possible to have a 90/270 rotation and a horizontal flip:
             * in this case, the rotation angle applies to the video frame
             * (rather than the rendering frame), so add this step to nullify
             * the conversion below. */
            if (VP.FlipHorizontal || VP.FlipVerical)
                rot *= -1;

            /* Return a positive value, noting that this converts angles
             * from the rendering frame to the video frame. */
            VP.Rotation = -rot;
            if (VP.Rotation < 0)
                VP.Rotation += 360;
        }
    }
}

AVFrame *LWVideoDecoder::GetNextFrame() {
    if (DecodeSuccess) {
        DecodeSuccess = DecodeNextFrame();
        if (DecodeSuccess) {
            CurrentFrame++;
            AVFrame *Tmp = DecodeFrame;
            DecodeFrame = nullptr;
            return Tmp;
        }
    } 
    return nullptr;
}

bool LWVideoDecoder::SkipFrames(int64_t Count) {
    while (Count-- > 0) {
        if (DecodeSuccess) {
            DecodeSuccess = DecodeNextFrame(true);
            if (DecodeSuccess)
                CurrentFrame++;
        } else {
            break;
        }
    }
    return DecodeSuccess;
}

bool LWVideoDecoder::HasMoreFrames() const {
    return DecodeSuccess;
}

bool LWVideoDecoder::Seek(int64_t PTS) {
    avcodec_flush_buffers(CodecContext);
    CurrentFrame = INT64_MIN;
    // Mild variable reuse, if seek fails then there's no point to decode more either
    DecodeSuccess = (av_seek_frame(FormatContext, TrackNumber, PTS, AVSEEK_FLAG_BACKWARD) >= 0);
    return DecodeSuccess;
}

void VideoFormat::Set(const AVPixFmtDescriptor *Desc) {
    Alpha = HasAlpha(Desc);
    Float = GetSampleTypeIsFloat(Desc);
    ColorFamily = GetColorFamily(Desc);
    Bits = GetBitDepth(Desc);
    SubSamplingW = Desc->log2_chroma_w;
    SubSamplingH = Desc->log2_chroma_h;
}

BestVideoFrame::BestVideoFrame(AVFrame *f) {
    Frame = av_frame_clone(f);
    auto Desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(Frame->format));
    VF.Set(Desc);
    Pts = Frame->pts;
    Width = Frame->width;
    Height = Frame->height;

    KeyFrame = !!(Frame->flags & AV_FRAME_FLAG_KEY);
    PictType = av_get_picture_type_char(Frame->pict_type);
    RepeatPict = Frame->repeat_pict;
    InterlacedFrame = !!(Frame->flags & AV_FRAME_FLAG_INTERLACED);
    TopFieldFirst = !!(Frame->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST);
    Matrix = Frame->colorspace;
    Primaries = Frame->color_primaries;
    Transfer = Frame->color_trc;
    ChromaLocation = Frame->chroma_location;
    ColorRange = Frame->color_range;

    const AVFrameSideData *MasteringDisplaySideData = av_frame_get_side_data(Frame, AV_FRAME_DATA_MASTERING_DISPLAY_METADATA);
    if (MasteringDisplaySideData) {
        const AVMasteringDisplayMetadata *MasteringDisplay = reinterpret_cast<const AVMasteringDisplayMetadata *>(MasteringDisplaySideData->data);
        if (MasteringDisplay->has_primaries) {
            HasMasteringDisplayPrimaries = !!MasteringDisplay->has_primaries;
            for (int i = 0; i < 3; i++) {
                MasteringDisplayPrimaries[i][0] = MasteringDisplay->display_primaries[i][0];
                MasteringDisplayPrimaries[i][1] = MasteringDisplay->display_primaries[i][1];
            }
            MasteringDisplayWhitePoint[0] = MasteringDisplay->white_point[0];
            MasteringDisplayWhitePoint[1] = MasteringDisplay->white_point[1];
        }

        if (MasteringDisplay->has_luminance) {
            HasMasteringDisplayLuminance = !!MasteringDisplay->has_luminance;
            MasteringDisplayMinLuminance = MasteringDisplay->min_luminance;
            MasteringDisplayMaxLuminance = MasteringDisplay->max_luminance;
        }

        HasMasteringDisplayPrimaries = !!MasteringDisplayPrimaries[0][0].Num && !!MasteringDisplayPrimaries[0][1].Num &&
            !!MasteringDisplayPrimaries[1][0].Num && !!MasteringDisplayPrimaries[1][1].Num &&
            !!MasteringDisplayPrimaries[2][0].Num && !!MasteringDisplayPrimaries[2][1].Num &&
            !!MasteringDisplayWhitePoint[0].Num && !!MasteringDisplayWhitePoint[1].Num;

        /* MasteringDisplayMinLuminance can be 0 */
        HasMasteringDisplayLuminance = !!MasteringDisplayMaxLuminance.Num;
    }

    const AVFrameSideData *ContentLightSideData = av_frame_get_side_data(Frame, AV_FRAME_DATA_CONTENT_LIGHT_LEVEL);
    if (ContentLightSideData) {
        const AVContentLightMetadata *ContentLightLevel = reinterpret_cast<const AVContentLightMetadata *>(ContentLightSideData->data);
        ContentLightLevelMax = ContentLightLevel->MaxCLL;
        ContentLightLevelAverage = ContentLightLevel->MaxFALL;
    }

    HasContentLightLevel = !!ContentLightLevelMax || !!ContentLightLevelAverage;

    const AVFrameSideData *DolbyVisionRPUSideData = av_frame_get_side_data(Frame, AV_FRAME_DATA_DOVI_RPU_BUFFER);
    if (DolbyVisionRPUSideData) {
        DolbyVisionRPU = DolbyVisionRPUSideData->data;
        DolbyVisionRPUSize = DolbyVisionRPUSideData->size;
    }

    AVFrameSideData *HDR10PlusSideData = av_frame_get_side_data(Frame, AV_FRAME_DATA_DYNAMIC_HDR_PLUS);
    if (HDR10PlusSideData) {
        int ret = av_dynamic_hdr_plus_to_t35(reinterpret_cast<const AVDynamicHDRPlus *>(HDR10PlusSideData->data), &HDR10Plus, &HDR10PlusSize);
        if (ret < 0) {
            // report error here "HDR10+ dynamic metadata could not be serialized."
        }
    }
}

BestVideoFrame::~BestVideoFrame() {
    av_frame_free(&Frame);
    av_freep(&HDR10Plus);
}

const AVFrame *BestVideoFrame::GetAVFrame() const {
    return Frame;
};

bool BestVideoFrame::HasAlpha() const {
    auto Desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(Frame->format));
    return ::HasAlpha(Desc);
};

bool BestVideoFrame::ExportAsPlanar(uint8_t **Dsts, ptrdiff_t *Stride, uint8_t *AlphaDst, ptrdiff_t AlphaStride) const {
    if (VF.ColorFamily == 0)
        return false;
    auto Desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(Frame->format));
    if (IsRealPlanar(Desc)) {

        size_t BytesPerSample = 0;

        if (VF.Bits <= 8)
            BytesPerSample = 1;
        if (VF.Bits > 8 && VF.Bits <= 16)
            BytesPerSample = 2;
        else if (VF.Bits > 16 && VF.Bits <= 32)
            BytesPerSample = 4;
        else if (VF.Bits > 32 && VF.Bits <= 64)
            BytesPerSample = 8;

        if (!BytesPerSample)
            return false;

        int NumBasePlanes = (VF.ColorFamily == 1 ? 1 : 3);
        for (int Plane = 0; Plane < NumBasePlanes; Plane++) {
            int PlaneW = Frame->width;
            int PlaneH = Frame->height;
            if (Plane > 0) {
                PlaneW >>= Desc->log2_chroma_w;
                PlaneH >>= Desc->log2_chroma_h;
            }
            int SrcPlane = Desc->comp[Plane].plane;
            const uint8_t *Src = Frame->data[SrcPlane];
            uint8_t *Dst = Dsts[Plane];
            for (int h = 0; h < PlaneH; h++) {
                memcpy(Dst, Src, BytesPerSample * PlaneW);
                Src += Frame->linesize[SrcPlane];
                Dst += Stride[Plane];
            }
        }

        if (::HasAlpha(Desc) && AlphaDst) {
            const uint8_t *Src = Frame->data[3];
            uint8_t *Dst = AlphaDst;
            for (int h = 0; h < Frame->height; h++) {
                memcpy(Dst, Src, BytesPerSample * Frame->width);
                Src += Frame->linesize[3];
                Dst += AlphaStride;
            }
        }

        return true;
    } else {
        p2p_buffer_param Buf = {};
        Buf.height = Frame->height;
        Buf.width = Frame->width;

        switch (Frame->format) {
            case AV_PIX_FMT_YUYV422:
                Buf.packing = p2p_yuy2;
                break;
            case AV_PIX_FMT_RGB24:
                Buf.packing = p2p_rgb24;
                break;
            case AV_PIX_FMT_UYVY422:
                Buf.packing = p2p_uyvy;
                break;
            case AV_PIX_FMT_NV12:
                Buf.packing = p2p_nv12;
                break;
            case AV_PIX_FMT_P010:
                Buf.packing = p2p_p010;
                break;
            case AV_PIX_FMT_ARGB:
            case AV_PIX_FMT_0RGB:
                Buf.packing = p2p_argb32;
                break;
            case AV_PIX_FMT_RGBA:
            case AV_PIX_FMT_RGB0:
                Buf.packing = p2p_rgba32;
                break;
            case AV_PIX_FMT_0BGR:
                Buf.packing = p2p_rgba32_le;
                break;
            case AV_PIX_FMT_BGR0:
                Buf.packing = p2p_argb32_le;
                break;
            case AV_PIX_FMT_RGB48:
                Buf.packing = p2p_rgb48;
                break;
            case AV_PIX_FMT_RGBA64:
                Buf.packing = p2p_rgba64;
                break;
            default:
                return false;
        }

        for (int Plane = 0; Plane < Desc->nb_components; Plane++) {
            Buf.src[Plane] = Frame->data[Plane];
            Buf.src_stride[Plane] = Frame->linesize[Plane];
        }

        for (int plane = 0; plane < (VF.ColorFamily == 1 ? 1 : 3); plane++) {
            Buf.dst[plane] = Dsts[plane];
            Buf.dst_stride[plane] = Stride[plane];
            if (::HasAlpha(Desc) && AlphaDst) {
                Buf.dst[3] = AlphaDst;
                Buf.dst_stride[3] = AlphaStride;
            }
        }

        p2p_unpack_frame(&Buf, 0);
        return true;
    }

    return false;
}

static void GetHash(const AVFrame *Frame, uint8_t *Hash) {
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(Frame->format));
    int NumPlanes = 0;
    int SampleSize[4] = {};

    for (int i = 0; i < desc->nb_components; i++) {
        SampleSize[desc->comp[i].plane] = std::max(SampleSize[desc->comp[i].plane], desc->comp[i].step);
        NumPlanes = std::max(NumPlanes, desc->comp[i].plane + 1);
    }

    AVHashContext *hctx;
    av_hash_alloc(&hctx, "md5");

    for (int p = 0; p < NumPlanes; p++) {
        int Width = Frame->width;
        int Height = Frame->height;
        if (p == 1 || p == 2) {
            Width >>= desc->log2_chroma_w;
            Height >>= desc->log2_chroma_h;
        }
        Width *= SampleSize[p];
        assert(Width <= Frame->linesize[p]);
        const uint8_t *Data = Frame->data[p];
        for (int h = 0; h < Height; h++) {
            av_hash_update(hctx, Data, Width);
            Data += Frame->linesize[p];
        }
    }

    av_hash_final(hctx, Hash);
    av_hash_freep(&hctx);
}

BestVideoSource::Cache::CacheBlock::CacheBlock(int64_t FrameNumber, AVFrame *Frame) : FrameNumber(FrameNumber), Frame(Frame) {
    for (int i = 0; i < 4; i++)
        if (Frame->buf[i])
            Size += Frame->buf[i]->size;
}

BestVideoSource::Cache::CacheBlock::~CacheBlock() {
    av_frame_free(&Frame);
}

void BestVideoSource::Cache::ApplyMaxSize() {
    while (Size > MaxSize) {
        Size -= Data.back().Size;
        Data.pop_back();
    }
}

void BestVideoSource::Cache::Clear() {
    Data.clear();
    Size = 0;
}

void BestVideoSource::Cache::SetMaxSize(size_t Bytes) {
    MaxSize = Bytes;
    ApplyMaxSize();
}

void BestVideoSource::Cache::CacheFrame(int64_t FrameNumber, AVFrame *Frame) {
    // Don't cache the same frame twice, get rid of the oldest copy instead
    for (auto Iter = Data.begin(); Iter != Data.end(); ++Iter) {
        if (Iter->FrameNumber == FrameNumber) {
            Size -= Iter->Size;
            Data.erase(Iter);
            break;
        }
    }

    Data.emplace_front(FrameNumber, Frame);
    Size += Data.front().Size;
    ApplyMaxSize();
}

BestVideoFrame *BestVideoSource::Cache::GetFrame(int64_t N) {
    // FIXME, put found frame in front
    for (auto &Iter : Data) {
        if (Iter.FrameNumber == N)
            return new BestVideoFrame(Iter.Frame);
    }
    return nullptr;
}

BestVideoSource::BestVideoSource(const std::string &SourceFile, const std::string &HWDeviceName, int ExtraHWFrames, int Track, bool VariableFormat, int Threads, const std::string &CachePath, const std::map<std::string, std::string> *LAVFOpts, const std::function<void(int Track, int64_t Current, int64_t Total)> &Progress)
    : Source(SourceFile), HWDevice(HWDeviceName), ExtraHWFrames(ExtraHWFrames), VideoTrack(Track), VariableFormat(VariableFormat), Threads(Threads) {
    if (LAVFOpts)
        LAVFOptions = *LAVFOpts;
    Decoders[0] = new LWVideoDecoder(Source, HWDevice, ExtraHWFrames, VideoTrack, VariableFormat, Threads, LAVFOptions);
    Decoders[0]->SkipFrames(1);
    Decoders[0]->GetVideoProperties(VP);
    VideoTrack = Decoders[0]->GetTrack();
    
    if (!ReadVideoTrackIndex(CachePath.empty() ? SourceFile : CachePath, VideoTrack, TrackIndex)) {
        if (!IndexTrack(Progress)) {
            delete Decoders[0];
            throw VideoException("Indexing of '" + SourceFile + "' track #" + std::to_string(VideoTrack) + " failed");
        }

        WriteVideoTrackIndex(CachePath.empty() ? SourceFile : CachePath, VideoTrack, TrackIndex);
    }

    VP.NumFrames = TrackIndex.Frames.size();
}

BestVideoSource::~BestVideoSource() {
    for (auto &Iter : Decoders)
        delete Iter;
}

int BestVideoSource::GetTrack() const {
    return VideoTrack;
}

void BestVideoSource::SetMaxCacheSize(size_t Bytes) {
    FrameCache.SetMaxSize(Bytes);
}

void BestVideoSource::SetSeekPreRoll(int64_t Frames) {
    PreRoll = std::max<int64_t>(Frames, 0);
}

bool BestVideoSource::IndexTrack(const std::function<void(int Track, int64_t Current, int64_t Total)> &Progress) {
    // FIXME, break out into its own class or something?

    std::unique_ptr<LWVideoDecoder> Decoder(new LWVideoDecoder(Source, HWDevice, ExtraHWFrames, VideoTrack, VariableFormat, Threads, LAVFOptions));

    int64_t FileSize = Progress ? Decoder->GetSourceSize() : -1;

    while (true) {
        AVFrame *F = Decoder->GetNextFrame();
        if (!F)
            break;
        VideoTrackIndex::FrameInfo FI = { F->pts, F->repeat_pict, !!(F->flags & AV_FRAME_FLAG_KEY), !!(F->flags & AV_FRAME_FLAG_TOP_FIELD_FIRST) };
        GetHash(F, FI.Hash);
        TrackIndex.Frames.push_back(FI);
        if (Progress)
            Progress(VideoTrack, Decoder->GetSourcePostion(), FileSize);
    };

    if (Progress)
        Progress(VideoTrack, INT64_MAX, INT64_MAX);

    VP.NumFrames = Decoder->GetFrameNumber();

    return !TrackIndex.Frames.empty();
}

const VideoProperties &BestVideoSource::GetVideoProperties() const {
    return VP;
}

BestVideoFrame *BestVideoSource::GetFrame(int64_t N, bool Linear) {
    if (N < 0 || N >= VP.NumFrames)
        return nullptr;

    BestVideoFrame *F = FrameCache.GetFrame(N);
    if (!F)
        F = (Linear ? GetFrameLinearInternal(N) : GetFrameInternal(N));
    std::array<uint8_t, 16> FrameHash;
    GetHash(F->GetAVFrame(), FrameHash.data());

    if (memcmp(TrackIndex.Frames[N].Hash, FrameHash.data(), 16)) {
        // If the frame isn't correct fall back to linear mode
        if (!LinearMode) {
            SetLinearMode();
            return GetFrame(N);
        } else {
            // Complete failure that should never happen
            return nullptr;
        }
    }

    return F;
}

void BestVideoSource::SetLinearMode() {
    assert(!LinearMode);
    if (!LinearMode) {
        LinearMode = true;
        FrameCache.Clear();
        for (size_t i = 0; i < MaxVideoSources; i++) {
            delete Decoders[i];
            Decoders[i] = nullptr;
        }
    }
}

// 1. If a current decoder is close to the requested frame simply start from there
//    Determine if a decoder is "close" by either a 50(?) frame threshold or longer if already in the optimal zone based on the keyframe
// 2. If a decoder isn't nearby and one of the first 100 frames are requested simply start with a fresh decoder to avoid the seek to start issue
// 3. Seek with an existing or new decoder. Seek to frame N-preroll-20 using PTS. If the index doesn't have a valid PTS there or earlier linearly decode from the start
//    After seeking match the hash of the decoded frame. For duplicate hashes match a string of frame hashes.
// 4. If the frame is determined to not exist (corrupt decoding) enable linear mode and start over.
// 5. If the frame is beyond the specified destination try again but 100 frames earlier (or decode from start if needed). Do this a few times before enabling linear mode.
// 6. Handle other weirdness like too many duplicates in a section to identify by the frame hash pattern.

int64_t BestVideoSource::GetSeekFrame(int64_t N) {
    for (int64_t i = N - PreRoll; i >= 100; i--) {
        if (TrackIndex.Frames[i].PTS != AV_NOPTS_VALUE)
            return i;
    }

    return -1;
}

namespace {
    class FrameHolder {
    private:
        std::vector<std::pair<AVFrame *, std::array<uint8_t, 16>>> Data;
    public:
        void clear() {
            for (auto &iter : Data)
                av_frame_free(&iter.first);
            Data.clear();
        }

        void push_back(AVFrame *F) {
            std::array<uint8_t, 16> FrameHash;
            ::GetHash(F, FrameHash.data());
            Data.push_back(std::make_pair(F, FrameHash));
        }

        size_t size() {
            return Data.size();
        }

        size_t empty() {
            return Data.empty();
        }

        [[nodiscard]] AVFrame *GetFrame(size_t Index, bool Extract = false) {
            AVFrame *Tmp = Data[Index].first;
            if (Extract)
                Data[Index].first = nullptr;
            return Tmp;
        }

        [[nodiscard]] bool CompareHash(size_t Index, const uint8_t *Other) {
            return !memcmp(Data[Index].second.data(), Other, 16);
        }

        ~FrameHolder() {
            clear();
        }
    };
}

BestVideoFrame *BestVideoSource::SeekAndDecode(int64_t N, int64_t SeekFrame, int Index, size_t Depth) {
    LWVideoDecoder *Decoder = Decoders[Index];
    if (!Decoder->Seek(TrackIndex.Frames[SeekFrame].PTS)) {
        SetLinearMode();
        return GetFrameLinearInternal(N);
    }

    FrameHolder Frames;

    // "automatically" free all large allocations before heading to another function
    auto GetFrameLinearWrapper = [this, &Frames](int64_t N) {
        Frames.clear();
        return GetFrameLinearInternal(N);
        };

    while (true) {
        AVFrame *F = Decoder->GetNextFrame();
        if (!F && Frames.empty()) {
            SetLinearMode();
            return GetFrameLinearWrapper(N);
        }

        std::set<int64_t> Matches;

        if (F) {
            Frames.push_back(F);

            for (int64_t i = 0; i <= TrackIndex.Frames.size() - Frames.size(); i++) {
                bool HashMatch = true;
                for (int64_t j = 0; j < Frames.size(); j++)
                    HashMatch = HashMatch && Frames.CompareHash(j, TrackIndex.Frames[i + j].Hash);
                if (HashMatch)
                    Matches.insert(i);
            }
        } else if (!F) {
            bool HashMatch = true;
            for (int64_t j = 0; j < Frames.size(); j++)
                HashMatch = HashMatch && Frames.CompareHash(j, TrackIndex.Frames[TrackIndex.Frames.size() - Frames.size() + j].Hash);
            if (HashMatch)
                Matches.insert(TrackIndex.Frames.size() - Frames.size());
        }

        // #3 Seek failure, fall back to linear
        if (Matches.empty()) {
            SetLinearMode();
            return GetFrameLinearWrapper(N);
        } else if (Matches.size() >= 1) {
            // Check if any match is in target zone, if not seek further back a couple of times
            bool SuitableCandidate = false;
            for (const auto &iter : Matches)
                if (iter <= N) // Do we care about preroll or is it just a nice thing to have? With seeking it's a lot less important anyway...
                    SuitableCandidate = true;

            bool UndeterminableLocation = (Matches.size() > 1 && (!F || Frames.size() >= 10));

            if (!SuitableCandidate || UndeterminableLocation) {
                if (Depth < 2) {
                    int64_t SeekFrameNext = GetSeekFrame(SeekFrame - 100);
                    if (SeekFrameNext < 100) { // #2 again
                        return GetFrameLinearWrapper(N);
                    } else {
                        // Free 
                        Frames.clear();
                        return SeekAndDecode(N, SeekFrameNext, Index, Depth + 1);
                    }
                } else {
                    // Fall back to linear decoding permanently since we failed to seek to any even remotably suitable frame in 3 attempts
                    SetLinearMode();
                    return GetFrameLinearWrapper(N);
                }
            }

            if (Matches.size() == 1) {
                int64_t MatchedN = *Matches.begin();
                if (MatchedN < 100) { // Kinda another #2 case again
                    delete Decoder;
                    Decoders[Index] = nullptr;
                    return GetFrameLinearWrapper(N);
                }

                Decoder->SetFrameNumber(MatchedN + Frames.size());

                // Insert frames into cache if appropriate
                BestVideoFrame *RetFrame = nullptr;
                for (int64_t FramesIdx = 0; FramesIdx < Frames.size(); FramesIdx++) {
                    int64_t FrameNumber = MatchedN + FramesIdx;

                    if (FrameNumber >= N - PreRoll) {
                        if (FrameNumber == N)
                            RetFrame = new BestVideoFrame(Frames.GetFrame(FramesIdx));

                        FrameCache.CacheFrame(FrameNumber, Frames.GetFrame(FramesIdx, true));
                    }
                }

                if (RetFrame)
                    return RetFrame;

                // Now that we have done everything we can and aren't holding on to the frame to output let the linear function do the rest
                return GetFrameLinearWrapper(N);
            }

            assert(Matches.size() > 1);

            // Multiple candidates match, go another lap to figure out which one it is
        }
    };

    // All paths should exit elsewhere
    assert(false);
    return nullptr;
}

BestVideoFrame *BestVideoSource::GetFrameInternal(int64_t N) {
    if (LinearMode)
        return GetFrameLinearInternal(N);

    // Algorithm starts here

    // #2 Checked first due to simplicity and this will grab a suitable decoder/start over as necessary
    if (N < 100) 
        return GetFrameLinearInternal(N);

    // #1 Do the more complicated check to see if linear decoding is the obvious answer
    int64_t NearestKF = 0;
    for (int64_t i = N; i >= 0; i--) {
        if (TrackIndex.Frames[i].KeyFrame) {
            NearestKF = i;
            break;
        }
    }
    int64_t SeekLimit = std::max(std::min(NearestKF, N - SeekThreshold), 0LL);
    // If the seek limit is less than 100 frames away from the start see #2 and do linear decoding
    if (SeekLimit < 100)
        return GetFrameLinearInternal(N);

    // # 1 Continued, a suitable linear decoder exists and seeking is out of the question
    for (int i = 0; i < MaxVideoSources; i++) {
        if (Decoders[i] && Decoders[i]->GetFrameNumber() <= N && Decoders[i]->GetFrameNumber() >= SeekLimit)
            return GetFrameLinearInternal(N);
    }

    // #3 Preparations here
    int64_t SeekFrame = GetSeekFrame(N);
    // Another #2 case which may happen
    if (SeekFrame == -1)
        return GetFrameLinearInternal(N);

    // Grab/create a decoder to use
    int Index = -1;
    for (int i = 0; i < MaxVideoSources; i++) {
        if (!Decoders[i]) {
            Index = i;
            Decoders[i] = new LWVideoDecoder(Source, HWDevice, ExtraHWFrames, VideoTrack, VariableFormat, Threads, LAVFOptions);
            break;
        }
    }

    // No empty slot exists, select the least recently used one
    if (Index < 0) {
        Index = 0;
        for (int i = 0; i < MaxVideoSources; i++) {
            if (Decoders[i] && DecoderLastUse[i] < DecoderLastUse[Index])
                Index = i;
        }
    }

    DecoderLastUse[Index] = DecoderSequenceNum++;

    // #3 Actual seeking dance of death starts here
    return SeekAndDecode(N, SeekFrame, Index);
}

BestVideoFrame *BestVideoSource::GetFrameLinearInternal(int64_t N) {
    // FIXME, can this selection code be written in a more compact way?
    // Check for a suitable existing decoder
    int Index = -1;
    for (int i = 0; i < MaxVideoSources; i++) {
        if (Decoders[i] && Decoders[i]->GetFrameNumber() <= N && (Index < 0 || Decoders[Index]->GetFrameNumber() < Decoders[i]->GetFrameNumber()))
            Index = i;
    }

    // If an empty slot exists simply spawn a new decoder there
    if (Index < 0) {
        for (int i = 0; i < MaxVideoSources; i++) {
            if (!Decoders[i]) {
                Index = i;
                Decoders[i] = new LWVideoDecoder(Source, HWDevice, ExtraHWFrames, VideoTrack, VariableFormat, Threads, LAVFOptions);
                break;
            }
        }
    }

    // No far enough back decoder exists and all slots are occupied so evict a random one
    if (Index < 0) {
        Index = 0;
        for (int i = 0; i < MaxVideoSources; i++) {
            if (Decoders[i] && DecoderLastUse[i] < DecoderLastUse[Index])
                Index = i;
        }
        delete Decoders[Index];
        Decoders[Index] = new LWVideoDecoder(Source, HWDevice, ExtraHWFrames, VideoTrack, VariableFormat, Threads, LAVFOptions);
    }

    LWVideoDecoder *Decoder = Decoders[Index];
    DecoderLastUse[Index] = DecoderSequenceNum++;

    BestVideoFrame *RetFrame = nullptr;

    while (Decoder && Decoder->GetFrameNumber() <= N && Decoder->HasMoreFrames()) {
        int64_t FrameNumber = Decoder->GetFrameNumber();
        if (FrameNumber >= N - PreRoll) {
            AVFrame *Frame = Decoder->GetNextFrame();

            if (FrameNumber == N)
                RetFrame = new BestVideoFrame(Frame);

            FrameCache.CacheFrame(FrameNumber, Frame);
        } else if (FrameNumber < N) {
            Decoder->SkipFrames(N - PreRoll - FrameNumber);
        }

        if (!Decoder->HasMoreFrames()) {
            delete Decoder;
            Decoders[Index] = nullptr;
            Decoder = nullptr;
        }
    }

    return RetFrame;
}
