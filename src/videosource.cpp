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

static bool GetSampleTypeIsFloat(const AVPixFmtDescriptor *desc) {
    return !!(desc->flags & AV_PIX_FMT_FLAG_FLOAT);
}

static bool HasAlpha(const AVPixFmtDescriptor *desc) {
    return !!(desc->flags & AV_PIX_FMT_FLAG_ALPHA);
}

static int GetColorFamily(const AVPixFmtDescriptor *desc) {
    if (desc->nb_components <= 2)
        return 1;
    else if (desc->flags & AV_PIX_FMT_FLAG_RGB)
        return 2;
    else
        return 3;
}

static int GetBitDepth(const AVPixFmtDescriptor *desc) {
    return desc->comp[0].depth;
}

static int IsRealPlanar(const AVPixFmtDescriptor *desc) {
    int maxPlane = 0;
    for (int i = 0; i < desc->nb_components; i++)
        maxPlane = std::max(maxPlane, desc->comp[i].plane);
    return (maxPlane + 1) == desc->nb_components;
}

// attempt to correct framerate to a common fraction if close to one
// FIXME, unused and not exposed at the moment. Pointless?
static void CorrectRationalFramerate(int &Num, int &Den) {
    // Make sure fps is a normalized rational number
    av_reduce(&Den, &Num, Den, Num, INT_MAX);

    const double fps = static_cast<double>(Num) / Den;
    const int fpsList[] = {24, 25, 30, 48, 50, 60, 100, 120};

    for (size_t i = 0; i < sizeof(fpsList) / sizeof(fpsList[0]); i++) {
        const double delta = (fpsList[i] - static_cast<double>(fpsList[i]) / 1.001) / 2.0;
        if (fabs(fps - fpsList[i]) < delta) {
            Num = fpsList[i];
            Den = 1;
            break;
        } else if ((fpsList[i] % 25) && (fabs(fps - static_cast<double>(fpsList[i]) / 1.001) < delta)) {
            Num = fpsList[i] * 1000;
            Den = 1001;
            break;
        }
    }
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
            if (!ReadPacket(Packet))
                return false;
            avcodec_send_packet(CodecContext, Packet);
            av_packet_unref(Packet);
        } else {
            break; // Probably EOF or some unrecoverable error so stop here
        }
    }

    return false;
}

void LWVideoDecoder::OpenFile(const char *SourceFile, int Track, bool VariableFormat, int Threads, const FFmpegOptions &Options) {
    TrackNumber = Track;

    AVDictionary *Dict = nullptr;
    av_dict_set_int(&Dict, "enable_drefs", Options.enable_drefs, 0);
    av_dict_set_int(&Dict, "use_absolute_path", Options.use_absolute_path, 0);

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
        Threads = std::min(std::thread::hardware_concurrency(), 16u);
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

LWVideoDecoder::LWVideoDecoder(const char *SourceFile, int Track, bool VariableFormat, int Threads, const FFmpegOptions &Options) {
    try {
        Packet = av_packet_alloc();
        OpenFile(SourceFile, Track, VariableFormat, Threads, Options);

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

int64_t LWVideoDecoder::GetRelativeStartTime(int Track) const {
    if (Track < 0 || Track >= static_cast<int>(FormatContext->nb_streams))
        return INT64_MIN;
    return 0;
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
    if (!VP.NumFrames) {
        if (VP.FPS.num)
            VP.NumFrames = (VP.Duration * VP.FPS.num) / VP.FPS.den;
    }

    if (VP.NumFrames <= 0)
        VP.NumFrames = -1;

    if (VP.NumFrames > 0)
        VP.NumFields = VP.NumFrames * 2;

    // sanity check framerate
    if (VP.FPS.den <= 0 || VP.FPS.num <= 0) {
        VP.FPS.den = 1;
        VP.FPS.num = 30;
    }

    VP.StartTime = FormatContext->streams[TrackNumber]->start_time;
    assert(VP.StartTime != AV_NOPTS_VALUE);
    if (VP.StartTime == AV_NOPTS_VALUE)
        VP.StartTime = 0;

    // Set AR variables
    VP.SAR = CodecContext->sample_aspect_ratio;

    // Set the SAR from the container if the codec SAR is invalid
    if (VP.SAR.num <= 0 || VP.SAR.den <= 0)
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

            VP.HasMasteringDisplayPrimaries = !!VP.MasteringDisplayPrimaries[0][0].num && !!VP.MasteringDisplayPrimaries[0][1].num &&
                !!VP.MasteringDisplayPrimaries[1][0].num && !!VP.MasteringDisplayPrimaries[1][1].num &&
                !!VP.MasteringDisplayPrimaries[2][0].num && !!VP.MasteringDisplayPrimaries[2][1].num &&
                !!VP.MasteringDisplayWhitePoint[0].num && !!VP.MasteringDisplayWhitePoint[1].num;
            /* MasteringDisplayMinLuminance can be 0 */
            VP.HasMasteringDisplayLuminance = !!VP.MasteringDisplayMaxLuminance.num;
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
    PixFmt = Frame->format;
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

        HasMasteringDisplayPrimaries = !!MasteringDisplayPrimaries[0][0].num && !!MasteringDisplayPrimaries[0][1].num &&
            !!MasteringDisplayPrimaries[1][0].num && !!MasteringDisplayPrimaries[1][1].num &&
            !!MasteringDisplayPrimaries[2][0].num && !!MasteringDisplayPrimaries[2][1].num &&
            !!MasteringDisplayWhitePoint[0].num && !!MasteringDisplayWhitePoint[1].num;

        /* MasteringDisplayMinLuminance can be 0 */
        HasMasteringDisplayLuminance = !!MasteringDisplayMaxLuminance.num;
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

bool BestVideoFrame::ExportAsPlanar(uint8_t **Dst, ptrdiff_t *Stride, uint8_t *AlphaDst, ptrdiff_t AlphaStride) const {
    if (VF.ColorFamily == 0)
        return false;
    auto Desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(Frame->format));
    if (IsRealPlanar(Desc)) {

        size_t bytesPerSample = 0;

        if (VF.Bits <= 8)
            bytesPerSample = 1;
        if (VF.Bits > 8 && VF.Bits <= 16)
            bytesPerSample = 2;
        else if (VF.Bits > 16 && VF.Bits <= 32)
            bytesPerSample = 4;
        else if (VF.Bits > 32 && VF.Bits <= 64)
            bytesPerSample = 8;

        if (!bytesPerSample)
            return false;

        int numBasePlanes = (VF.ColorFamily == 1 ? 1 : 3);
        for (int plane = 0; plane < numBasePlanes; plane++) {
            int planew = Frame->width;
            int planeh = Frame->height;
            if (plane > 0) {
                planew >>= Desc->log2_chroma_w;
                planeh >>= Desc->log2_chroma_h;
            }
            const uint8_t *src = Frame->data[plane];
            uint8_t *dst = Dst[plane];
            for (int h = 0; h < planeh; h++) {
                memcpy(dst, src, bytesPerSample * planew);
                src += Frame->linesize[plane];
                dst += Stride[plane];
            }
        }

        if (::HasAlpha(Desc) && AlphaDst) {
            const uint8_t *src = Frame->data[3];
            uint8_t *dst = AlphaDst;
            for (int h = 0; h < Frame->height; h++) {
                memcpy(dst, src, bytesPerSample * Frame->width);
                src += Frame->linesize[3];
                dst += AlphaStride;
            }
        }

        return true;
    } else {
        p2p_buffer_param buf = {};

        switch (PixFmt) {
            case AV_PIX_FMT_YUYV422:
                buf.packing = p2p_yuy2;
                break;
            case AV_PIX_FMT_RGB24:
                buf.packing = p2p_rgb24;
                break;
            case AV_PIX_FMT_UYVY422:
                buf.packing = p2p_uyvy;
                break;
            case AV_PIX_FMT_NV12:
                buf.packing = p2p_nv12;
                break;
            case AV_PIX_FMT_ARGB:
                buf.packing = p2p_argb32;
                break;
            case AV_PIX_FMT_RGBA:
                buf.packing = p2p_rgba32;
                break;
            case AV_PIX_FMT_RGB48:
                buf.packing = p2p_rgb48;
                break;
            case AV_PIX_FMT_RGBA64:
                buf.packing = p2p_rgba64;
                break;
            default:
                return false;
        }

        for (int plane = 0; plane < Desc->nb_components; plane++) {
            buf.src[plane] = Frame->data[plane];
            buf.src_stride[plane] = Frame->linesize[plane];
        }

        for (int plane = 0; plane < (VF.ColorFamily == 1 ? 1 : 3); plane++) {
            buf.dst[plane] = Dst[plane];
            buf.dst_stride[plane] = Stride[plane];
            if (::HasAlpha(Desc) && AlphaDst) {
                buf.dst[3] = AlphaDst;
                buf.dst_stride[3] = AlphaStride;
            }
        }

        p2p_unpack_frame(&buf, 0);
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

BestVideoSource::BestVideoSource(const char *SourceFile, int Track, bool VariableFormat, int Threads, const FFmpegOptions *Options)
    : Source(SourceFile), VideoTrack(Track), VariableFormat(VariableFormat), Threads(Threads) {
    if (Options)
        FFOptions = *Options;
    Decoders[0] = new LWVideoDecoder(Source.c_str(), VideoTrack, VariableFormat, Threads, FFOptions);
    VP = Decoders[0]->GetVideoProperties();
    VideoTrack = Decoders[0]->GetTrack();
    
    SourceAttributes attr = {};
    if (GetSourceAttributes(Source, attr)) {
        if (attr.tracks.count(VideoTrack) && attr.tracks[VideoTrack] > 0) {
            VP.NumFrames = attr.tracks[VideoTrack];
            HasExactNumVideoFrames = true;
        }
    }
    
    MaxSize = 1024 * 1024 * 1024;
}

BestVideoSource::~BestVideoSource() {
    for (auto iter : Decoders)
        delete iter;
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
        Decoders[0] = new LWVideoDecoder(Source.c_str(), VideoTrack, VariableFormat, Threads, FFOptions);
        Index = 0;
    }

    LWVideoDecoder *Decoder = Decoders[Index];

    while (Decoder->SkipNextAVFrame());
    VP.NumFrames = Decoder->GetFrameNumber();
    VP.NumFields = Decoder->GetFieldNumber();

    SourceAttributes attr;
    attr.tracks[VideoTrack] = VP.NumFrames;
    SetSourceAttributes(Source, attr);

    HasExactNumVideoFrames = true;
    delete Decoder;
    Decoders[Index] = nullptr;

    return true;
}

const VideoProperties &BestVideoSource::GetVideoProperties() const {
    return VP;
}

BestVideoFrame *BestVideoSource::GetFrame(int64_t N) {
    if (N < 0)
        return nullptr;

    for (auto &iter : Cache) {
        if (iter.FrameNumber == N)
            return new BestVideoFrame(iter.Frame);
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
                Decoders[i] = new LWVideoDecoder(Source.c_str(), VideoTrack, VariableFormat, Threads, FFOptions);
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
        Decoders[Index] = new LWVideoDecoder(Source.c_str(), VideoTrack, VariableFormat, Threads, FFOptions);
    }

    LWVideoDecoder *Decoder = Decoders[Index];
    DecoderLastUse[Index] = DecoderSequenceNum++;

    AVFrame *RetFrame = nullptr;

    while (Decoder && Decoder->GetFrameNumber() <= N && Decoder->HasMoreFrames()) {
        int64_t FrameNumber = Decoder->GetFrameNumber();
        if (FrameNumber >= N - PreRoll) {
            AVFrame *Frame = Decoder->GetNextAVFrame();

            for (auto iter = Cache.begin(); iter != Cache.end(); ++iter) {
                if (iter->FrameNumber == FrameNumber) {
                    Cache.erase(iter);
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
                SourceAttributes attr;
                attr.tracks[VideoTrack] = VP.NumFrames;
                SetSourceAttributes(Source, attr);
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
