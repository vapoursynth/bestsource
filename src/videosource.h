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

#include "BSRational.h"
#include <cstdint>
#include <stdexcept>
#include <list>
#include <map>

struct AVFormatContext;
struct AVCodecContext;
struct AVBufferRef;
struct AVFrame;
struct AVPacket;
struct AVPixFmtDescriptor;

class VideoException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct VideoFormat {
    int ColorFamily; /* Unknown = 0, Gray = 1, RGB = 2, YUV = 3 */
    bool Alpha;
    bool Float;
    int Bits;
    int SubSamplingW;
    int SubSamplingH;

    void Set(const AVPixFmtDescriptor *Desc);
};

struct VideoProperties {
    BSRational TimeBase;
    double StartTime; // in seconds
    int64_t Duration;
    int64_t NumFrames; // can be -1 to signal that the number of frames is completely unknown, RFF ignored
    int64_t NumFields; // same as NumFrames but for total fields with RFF taken into consideration, currently broken

    BSRational FPS;
    BSRational SAR;

    int PixFmt;
    VideoFormat VF;
    int Width;
    int Height;

    /* Stereo 3D */
    int Stereo3DType;
    int Stereo3DFlags;

    /* MasteringDisplayPrimaries */
    bool HasMasteringDisplayPrimaries;
    BSRational MasteringDisplayPrimaries[3][2];
    BSRational MasteringDisplayWhitePoint[2];

    /* MasteringDisplayLuminance */
    bool HasMasteringDisplayLuminance;
    BSRational MasteringDisplayMinLuminance;
    BSRational MasteringDisplayMaxLuminance;

    /* ContentLightLevel */
    bool HasContentLightLevel;
    unsigned ContentLightLevelMax;
    unsigned ContentLightLevelAverage;

    /* Orientation */
    bool FlipVerical;
    bool FlipHorizontal;
    int Rotation; /* A positive number in degrees */
};


struct LWVideoDecoder {
private:
    VideoProperties VP = {};
    AVFormatContext *FormatContext = nullptr;
    AVCodecContext *CodecContext = nullptr;
    AVBufferRef *HWDeviceContext = nullptr;
    AVFrame *DecodeFrame = nullptr;
    AVFrame *HWFrame = nullptr;
    int64_t CurrentFrame = 0;
    int64_t CurrentField = 0;
    int TrackNumber = -1;
    bool HWMode = false;
    bool DecodeSuccess = false;
    AVPacket *Packet = nullptr;

    void OpenFile(const std::string &SourceFile, const std::string &HWDeviceName, int Track, bool VariableFormat, int Threads, const std::map<std::string, std::string> &LAVFOpts);
    void SetVideoProperties();
    bool ReadPacket(AVPacket *Packet);
    bool DecodeNextAVFrame(bool SkipOutput = false);
    void Free();
public:
    LWVideoDecoder(const std::string &SourceFile, const std::string &HWDeviceName, int Track, bool VariableFormat, int Threads, const std::map<std::string, std::string> &LAVFOpts); // Positive track numbers are absolute. Negative track numbers mean nth audio track to simplify things.
    ~LWVideoDecoder();
    int GetTrack() const; // Useful when opening nth video track to get the actual number
    int64_t GetFrameNumber() const;
    int64_t GetFieldNumber() const;
    const VideoProperties &GetVideoProperties() const;
    AVFrame *GetNextAVFrame();
    bool SkipAVFrames(int64_t Count);
    bool HasMoreFrames() const;
};


class BestVideoFrame {
private:
    AVFrame *Frame;
public:
    BestVideoFrame(AVFrame *Frame);
    ~BestVideoFrame();
    const AVFrame *GetAVFrame() const;
    bool HasAlpha() const;
    bool ExportAsPlanar(uint8_t **Dsts, ptrdiff_t *Stride, uint8_t *AlphaDst = nullptr, ptrdiff_t AlphaStride = 0) const;

    VideoFormat VF;
    int Width;
    int Height;

    int64_t Pts;
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
    BSRational MasteringDisplayPrimaries[3][2] = {};
    BSRational MasteringDisplayWhitePoint[2] = {};

    /* MasteringDisplayLuminance */
    bool HasMasteringDisplayLuminance = false;
    BSRational MasteringDisplayMinLuminance = {};
    BSRational MasteringDisplayMaxLuminance = {};

    /* ContentLightLevel */
    bool HasContentLightLevel = false;
    unsigned ContentLightLevelMax = 0;
    unsigned ContentLightLevelAverage = 0;

    /* DolbyVisionRPU */
    uint8_t *DolbyVisionRPU = nullptr;
    size_t DolbyVisionRPUSize = 0;
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
    std::map<std::string, std::string> LAVFOptions;
    VideoProperties VP = {};
    std::string Source;
    std::string CachePath;
    std::string HWDevice;
    int VideoTrack;
    bool VariableFormat;
    int Threads;
    bool HasExactNumVideoFrames = false;
    uint64_t DecoderSequenceNum = 0;
    uint64_t DecoderLastUse[MaxVideoSources] = {};
    LWVideoDecoder *Decoders[MaxVideoSources] = {};
    std::list<CacheBlock> Cache;
    size_t MaxSize;
    size_t CacheSize = 0;
    int64_t PreRoll = 20;
public:
    BestVideoSource(const std::string &SourceFile, const std::string &HWDeviceName, int Track, bool VariableFormat, int Threads, const std::string &CachePath, const std::map<std::string, std::string> *LAVFOpts);
    ~BestVideoSource();
    int GetTrack() const; // Useful when opening nth video track to get the actual number
    void SetMaxCacheSize(size_t Bytes); /* default max size is 1GB */
    void SetSeekPreRoll(size_t Frames); /* the number of frames to cache before the position being fast forwarded to, default is 10 frames */
    bool GetExactDuration();
    const VideoProperties &GetVideoProperties() const;
    BestVideoFrame *GetFrame(int64_t N);
    BestVideoFrame *GetFrameExtendLast(int64_t N);
};

#endif
