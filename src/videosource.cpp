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
#include "SrcAttribCache.h"
#include <algorithm>
#include <thread>
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
}

#define VERSION_CHECK(LIB, cmp, major, minor, micro) ((LIB) cmp (AV_VERSION_INT(major, minor, micro)))

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

bool LWVideoDecoder::DecodeNextAVFrame() {
    if (!DecodeFrame) {
        DecodeFrame = av_frame_alloc();
        if (!DecodeFrame)
            throw VideoException("Couldn't allocate frame");
    }

    while (true) {
        int Ret = avcodec_receive_frame(CodecContext, DecodeFrame);
        if (Ret == 0) {
            return true;
        } else if (Ret == AVERROR(EAGAIN)) {
            if (ReadPacket(Packet)) {
                avcodec_send_packet(CodecContext, Packet);
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

void LWVideoDecoder::OpenFile(const char *SourceFile, int Track, bool VariableFormat, int Threads, const std::map<std::string, std::string> &LAVFOpts) {
    TrackNumber = Track;

    AVDictionary *Dict = nullptr;
    for (const auto &Iter : LAVFOpts)
        av_dict_set(&Dict, Iter.first.c_str(), Iter.second.c_str(), 0);

    if (avformat_open_input(&FormatContext, SourceFile, nullptr, &Dict) != 0)
        throw VideoException(std::string("Couldn't open '") + SourceFile + "'");

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

    const AVCodec *Codec = avcodec_find_decoder(FormatContext->streams[TrackNumber]->codecpar->codec_id);
    if (Codec == nullptr)
        throw VideoException("Video codec not found");

    CodecContext = avcodec_alloc_context3(Codec);
    if (CodecContext == nullptr)
        throw VideoException("Could not allocate video decoding context");

    if (avcodec_parameters_to_context(CodecContext, FormatContext->streams[TrackNumber]->codecpar) < 0)
        throw VideoException("Could not copy video codec parameters");

    if (Threads < 1)
        Threads = std::min(static_cast<int>(std::thread::hardware_concurrency()), 16);
    CodecContext->thread_count = Threads;

    if (!VariableFormat) {
        // Probably guard against mid-stream format changes
        CodecContext->flags |= AV_CODEC_FLAG_DROPCHANGED;
    }

    // Full explanation by more clever person availale here: https://github.com/Nevcairiel/LAVFilters/issues/113
    if (CodecContext->codec_id == AV_CODEC_ID_H264 && CodecContext->has_b_frames)
        CodecContext->has_b_frames = 15; // the maximum possible value for h264

    if (avcodec_open2(CodecContext, Codec, nullptr) < 0)
        throw VideoException("Could not open video codec");
}

LWVideoDecoder::LWVideoDecoder(const char *SourceFile, int Track, bool VariableFormat, int Threads, const std::map<std::string, std::string> &LAVFOpts) {
    try {
        Packet = av_packet_alloc();
        OpenFile(SourceFile, Track, VariableFormat, Threads, LAVFOpts);

        DecodeSuccess = DecodeNextAVFrame();

        if (!DecodeSuccess)
            throw VideoException("Couldn't decode initial frame");

        SetVideoProperties();

    } catch (...) {
        Free();
        throw;
    }
}

void LWVideoDecoder::Free() {
    av_packet_free(&Packet);
    av_frame_free(&DecodeFrame);
    avcodec_free_context(&CodecContext);
    avformat_close_input(&FormatContext);
}

LWVideoDecoder::~LWVideoDecoder() {
    Free();
}

int LWVideoDecoder::GetTrack() const {
    return TrackNumber;
}

int64_t LWVideoDecoder::GetFrameNumber() const {
    return CurrentFrame;
}

int64_t LWVideoDecoder::GetFieldNumber() const {
    return CurrentField;
}

const VideoProperties &LWVideoDecoder::GetVideoProperties() const {
    return VP;
}

AVFrame *LWVideoDecoder::GetNextAVFrame() {
    if (DecodeSuccess) {
        CurrentFrame++;
        CurrentField += 2 + DecodeFrame->repeat_pict;
        AVFrame *Tmp = DecodeFrame;
        DecodeFrame = nullptr;
        DecodeSuccess = DecodeNextAVFrame();
        return Tmp;
    }
    return nullptr;
}

bool LWVideoDecoder::SkipNextAVFrame() {
    if (DecodeSuccess) {
        CurrentFrame++;
        CurrentField += 2 + DecodeFrame->repeat_pict;
        DecodeSuccess = DecodeNextAVFrame();
    }
    return DecodeSuccess;
}

bool LWVideoDecoder::HasMoreFrames() const {
    return DecodeSuccess;
}

void LWVideoDecoder::SetVideoProperties() {
    VP.Width = CodecContext->width;
    VP.Height = CodecContext->height;
    VP.PixFmt = CodecContext->pix_fmt;
    VP.VF.Set(av_pix_fmt_desc_get(CodecContext->pix_fmt));
  
    VP.FPS = CodecContext->framerate;
    VP.Duration = FormatContext->streams[TrackNumber]->duration;
    VP.TimeBase = FormatContext->streams[TrackNumber]->time_base;

    VP.NumFrames = FormatContext->streams[TrackNumber]->nb_frames;
    if (VP.NumFrames <= 0 && VP.Duration > 0) {
        if (VP.FPS.Num)
            VP.NumFrames = (VP.Duration * VP.FPS.Num) / VP.FPS.Den;
    }

    if (VP.NumFrames <= 0)
        VP.NumFrames = -1;

    if (VP.NumFrames > 0)
        VP.NumFields = VP.NumFrames * 2;

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

    for (int i = 0; i < FormatContext->streams[TrackNumber]->nb_side_data; i++) {
        if (FormatContext->streams[TrackNumber]->side_data[i].type == AV_PKT_DATA_STEREO3D) {
            const AVStereo3D *StereoSideData = (const AVStereo3D *)FormatContext->streams[TrackNumber]->side_data[i].data;
            VP.Stereo3DType = StereoSideData->type;
            VP.Stereo3DFlags = StereoSideData->flags;
        } else if (FormatContext->streams[TrackNumber]->side_data[i].type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA) {
            const AVMasteringDisplayMetadata *MasteringDisplay = (const AVMasteringDisplayMetadata *)FormatContext->streams[TrackNumber]->side_data[i].data;
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
        } else if (FormatContext->streams[TrackNumber]->side_data[i].type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL) {
            const AVContentLightMetadata *ContentLightLevel = (const AVContentLightMetadata *)FormatContext->streams[TrackNumber]->side_data[i].data;

            VP.ContentLightLevelMax = ContentLightLevel->MaxCLL;
            VP.ContentLightLevelAverage = ContentLightLevel->MaxFALL;

            /* Only check for either of them */
            VP.HasContentLightLevel = !!VP.ContentLightLevelMax || !!VP.ContentLightLevelAverage;
        }
    }

    /////////////////////////
    // Set rotation
    int32_t *RotationMatrix = reinterpret_cast<int32_t *>(av_stream_get_side_data(FormatContext->streams[TrackNumber], AV_PKT_DATA_DISPLAYMATRIX, nullptr));
    if (RotationMatrix) {
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

    KeyFrame = !!Frame->key_frame;
    PictType = av_get_picture_type_char(Frame->pict_type);
    RepeatPict = Frame->repeat_pict;
    InterlacedFrame = !!Frame->interlaced_frame;
    TopFieldFirst = !!Frame->top_field_first;
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

#if VERSION_CHECK(LIBAVUTIL_VERSION_INT, >=, 57, 9, 100)
    const AVFrameSideData *DolbyVisionRPUSideData = av_frame_get_side_data(Frame, AV_FRAME_DATA_DOVI_RPU_BUFFER);
    if (DolbyVisionRPUSideData) {
        DolbyVisionRPU = DolbyVisionRPUSideData->data;
        DolbyVisionRPUSize = DolbyVisionRPUSideData->size;
    }
#endif
}

BestVideoFrame::~BestVideoFrame() {
    av_frame_free(&Frame);
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
            const uint8_t *Src = Frame->data[Plane];
            uint8_t *Dst = Dsts[Plane];
            for (int h = 0; h < PlaneH; h++) {
                memcpy(Dst, Src, BytesPerSample * PlaneW);
                Src += Frame->linesize[Plane];
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
            case AV_PIX_FMT_ARGB:
                Buf.packing = p2p_argb32;
                break;
            case AV_PIX_FMT_RGBA:
                Buf.packing = p2p_rgba32;
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
    }

    return false;
}

BestVideoSource::CacheBlock::CacheBlock(int64_t FrameNumber, AVFrame *Frame) : FrameNumber(FrameNumber), Frame(Frame) {
    Size = 0;
    for (int i = 0; i < AV_NUM_DATA_POINTERS; i++)
        if (Frame->buf[i])
            Size += Frame->buf[i]->size;
}

BestVideoSource::CacheBlock::~CacheBlock() {
    av_frame_free(&Frame);
}

BestVideoSource::BestVideoSource(const char *SourceFile, int Track, bool VariableFormat, int Threads, const char *CachePath, const std::map<std::string, std::string> *LAVFOpts)
    : Source(SourceFile), VideoTrack(Track), VariableFormat(VariableFormat), Threads(Threads) {
    if (CachePath)
        this->CachePath = CachePath;
    if (LAVFOpts)
        LAVFOptions = *LAVFOpts;
    Decoders[0] = new LWVideoDecoder(Source.c_str(), VideoTrack, VariableFormat, Threads, LAVFOptions);
    VP = Decoders[0]->GetVideoProperties();
    VideoTrack = Decoders[0]->GetTrack();
    
    SourceAttributes Attr = {};
    if (GetSourceAttributes(this->CachePath, Source, Attr, LAVFOptions, VariableFormat)) {
        if (Attr.Tracks.count(VideoTrack) && Attr.Tracks[VideoTrack] > 0) {
            VP.NumFrames = Attr.Tracks[VideoTrack];
            HasExactNumVideoFrames = true;
        }
    }
    
    MaxSize = 1024 * 1024 * 1024;
}

BestVideoSource::~BestVideoSource() {
    for (auto Iter : Decoders)
        delete Iter;
}

int BestVideoSource::GetTrack() const {
    return VideoTrack;
}

void BestVideoSource::SetMaxCacheSize(size_t Bytes) {
    MaxSize = Bytes;
    while (CacheSize > MaxSize) {
        CacheSize -= Cache.back().Size;
        Cache.pop_back();
    }
}

void BestVideoSource::SetSeekPreRoll(size_t Frames) {
    PreRoll = static_cast<int64_t>(Frames);
}

bool BestVideoSource::GetExactDuration() {
    if (HasExactNumVideoFrames)
        return true;
    int Index = -1;
    for (int i = 0; i < MaxVideoSources; i++) {
        if (Decoders[i] && (Index < 0 || Decoders[Index]->GetFrameNumber() < Decoders[i]->GetFrameNumber()))
            Index = i;
    }

    if (Index < 0) {
        Decoders[0] = new LWVideoDecoder(Source.c_str(), VideoTrack, VariableFormat, Threads, LAVFOptions);
        Index = 0;
    }

    LWVideoDecoder *Decoder = Decoders[Index];

    while (Decoder->SkipNextAVFrame());
    VP.NumFrames = Decoder->GetFrameNumber();
    VP.NumFields = Decoder->GetFieldNumber();

    SetSourceAttributes(CachePath, Source, VideoTrack, VP.NumFrames, LAVFOptions, VariableFormat);

    HasExactNumVideoFrames = true;
    delete Decoder;
    Decoders[Index] = nullptr;

    return true;
}

const VideoProperties &BestVideoSource::GetVideoProperties() const {
    return VP;
}

BestVideoFrame *BestVideoSource::GetFrame(int64_t N) {
    if (N < 0 || (HasExactNumVideoFrames && N >= VP.NumFrames))
        return nullptr;

    for (auto &Iter : Cache) {
        if (Iter.FrameNumber == N)
            return new BestVideoFrame(Iter.Frame);
    }

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
                Decoders[i] = new LWVideoDecoder(Source.c_str(), VideoTrack, VariableFormat, Threads, LAVFOptions);
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
        Decoders[Index] = new LWVideoDecoder(Source.c_str(), VideoTrack, VariableFormat, Threads, LAVFOptions);
    }

    LWVideoDecoder *Decoder = Decoders[Index];
    DecoderLastUse[Index] = DecoderSequenceNum++;

    AVFrame *RetFrame = nullptr;

    while (Decoder && Decoder->GetFrameNumber() <= N && Decoder->HasMoreFrames()) {
        int64_t FrameNumber = Decoder->GetFrameNumber();
        if (FrameNumber >= N - PreRoll) {
            AVFrame *Frame = Decoder->GetNextAVFrame();

            for (auto Iter = Cache.begin(); Iter != Cache.end(); ++Iter) {
                if (Iter->FrameNumber == FrameNumber) {
                    Cache.erase(Iter);
                    break;
                }
            }

            Cache.emplace_front(FrameNumber, Frame);
            CacheSize += Cache.front().Size;

            while (CacheSize > MaxSize) {
                CacheSize -= Cache.back().Size;
                Cache.pop_back();
            }

            if (FrameNumber == N)
                RetFrame = Frame;
        } else if (FrameNumber < N) {
            Decoder->SkipNextAVFrame();
        }

        if (!Decoder->HasMoreFrames()) {
            VP.NumFrames = Decoder->GetFrameNumber();
            VP.NumFields = Decoder->GetFieldNumber();
            if (!HasExactNumVideoFrames) {
                SetSourceAttributes(CachePath, Source, VideoTrack, VP.NumFrames, LAVFOptions, VariableFormat);
                HasExactNumVideoFrames = true;
            }
            delete Decoder;
            Decoders[Index] = nullptr;
            Decoder = nullptr;
        }
    }

    if (RetFrame)
        return new BestVideoFrame(RetFrame);
    return nullptr;
}
