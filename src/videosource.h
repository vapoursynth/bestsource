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

#ifndef VIDEOSOURCE_H
#define VIDEOSOURCE_H

#include <cstdint>
#include <stdexcept>
#include <list>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVPixFmtDescriptor;

// FIXME, replace with a custom rational class that can be assigned with AVRational
extern "C" {
#include <libavutil/rational.h>
}

class VideoException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct VideoFormat {
    int ColorFamily; /* Other = 0, Gray = 1, RGB = 2, YUV = 3 */
    bool Alpha;
    bool Float;
    int Bits;
    int SubSamplingW;
    int SubSamplingH;

    void Set(const AVPixFmtDescriptor *Desc);
};

struct VideoProperties {
    AVRational TimeBase;
    int64_t StartTime;
    int64_t Duration;
    int64_t NumFrames; // can be -1 to signal that the number of frames is completely unknown, RFF ignored
    int64_t NumFields; // same as NumFrames but for total fields with RFF taken into consideration

    AVRational FPS;
    AVRational SAR;

    int PixFmt;
    VideoFormat VF;
    int Width;
    int Height;

    /* Stereo 3D */
    int Stereo3DType;
    int Stereo3DFlags;

    /* MasteringDisplayPrimaries */
    bool HasMasteringDisplayPrimaries;
    AVRational MasteringDisplayPrimaries[3][2];
    AVRational MasteringDisplayWhitePoint[2];

    /* MasteringDisplayLuminance */
    bool HasMasteringDisplayLuminance;
    AVRational MasteringDisplayMinLuminance;
    AVRational MasteringDisplayMaxLuminance;

    /* ContentLightLevel */
    bool HasContentLightLevel;
    unsigned ContentLightLevelMax;
    unsigned ContentLightLevelAverage;

    /* Orientation */
    bool FlipVerical;
    bool FlipHorizontal;
    int Rotation; /* A positive number in degrees */
};

/* These correspond to the FFmpeg options of the same name */
struct FFmpegOptions {
    /* mp4/mov/3gpp demuxer options */
    bool enable_drefs = false;
    bool use_absolute_path = false;
};

struct LWVideoDecoder {
private:
    VideoProperties VP = {};
    AVFormatContext *FormatContext = nullptr;
    AVCodecContext *CodecContext = nullptr;
    AVFrame *DecodeFrame = nullptr;
    int64_t CurrentFrame = 0;
    int64_t CurrentField = 0;
    int TrackNumber = -1;
    bool DecodeSuccess = false;
    AVPacket *Packet = nullptr;

    void OpenFile(const char *SourceFile, int Track, bool VariableFormat, int Threads, const FFmpegOptions &Options);
    void SetVideoProperties();
    bool ReadPacket(AVPacket *Packet);
    bool DecodeNextAVFrame();
    void Free();
public:
    LWVideoDecoder(const char *SourceFile, int Track, bool VariableFormat, int Threads, const FFmpegOptions &Options); // Positive track numbers are absolute. Negative track numbers mean nth audio track to simplify things.
    ~LWVideoDecoder();
    int64_t GetRelativeStartTime(int Track) const; // Returns INT64_MIN on error
    int64_t GetFrameNumber() const;
    int64_t GetFieldNumber() const;
    const VideoProperties &GetVideoProperties() const;
    AVFrame *GetNextAVFrame();
    bool SkipNextAVFrame();
    bool HasMoreFrames() const;
};


class BestVideoFrame {
private:
    AVFrame *Frame;
public:
    BestVideoFrame(AVFrame *Frame);
    ~BestVideoFrame();
    const AVFrame *GetAVFrame() const;
    bool ExportAsPlanar(uint8_t **Dst, ptrdiff_t *Stride);

    int PixFmt; // Pointless since it can be gotten from the underlying frame?
    VideoFormat VF;
    int Width;
    int Height;

    int Matrix;
    int Primaries;
    int Transfer;
    int ChromaLocation;
    int ColorRange;

    bool InterlacedFrame;
    bool TopFieldFirst;
    char PictType;
    bool KeyFrame;
    int RepeatPict;

    /* MasteringDisplayPrimaries */
    bool HasMasteringDisplayPrimaries = false;
    AVRational MasteringDisplayPrimaries[3][2] = {};
    AVRational MasteringDisplayWhitePoint[2] = {};

    /* MasteringDisplayLuminance */
    bool HasMasteringDisplayLuminance = false;
    AVRational MasteringDisplayMinLuminance = {};
    AVRational MasteringDisplayMaxLuminance = {};

    /* ContentLightLevel */
    bool HasContentLightLevel = false;
    unsigned ContentLightLevelMax = 0;
    unsigned ContentLightLevelAverage = 0;

    /* DolbyVisionRPU */
    uint8_t *DolbyVisionRPU = nullptr;
    int DolbyVisionRPUSize = 0;
};

class BestVideoSource {
private:
    class CacheBlock {
    public:
        int64_t FrameNumber;
        AVFrame *Frame;
        size_t Size;
        CacheBlock(int64_t FrameNumber, AVFrame *Frame);
        ~CacheBlock();
    };

    static constexpr size_t MaxVideoSources = 4;
    FFmpegOptions FFOptions = {};
    VideoProperties VP = {};
    std::string Source;
    int Track;
    bool VariableFormat;
    int Threads;
    bool HasExactNumVideoFrames = false;
    uint64_t DecoderSequenceNum = 0;
    uint64_t DecoderLastUse[MaxVideoSources] = {};
    LWVideoDecoder *Decoders[MaxVideoSources] = {};
    std::list<CacheBlock> Cache;
    size_t MaxSize;
    size_t CacheSize = 0;
    int64_t PreRoll = 10;
public:
    BestVideoSource(const char *SourceFile, int Track, bool VariableFormat, int Threads, const FFmpegOptions *Options = nullptr);
    ~BestVideoSource();
    void SetMaxCacheSize(size_t Bytes); /* default max size is 1GB */
    void SetSeekPreRoll(size_t Frames); /* the number of frames to cache before the position being fast forwarded to, default is 10 frames */
    bool GetExactDuration();
    const VideoProperties &GetVideoProperties() const;
    BestVideoFrame *GetFrame(int64_t N);
};

#endif
