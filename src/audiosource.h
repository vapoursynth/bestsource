//  Copyright (c) 2020-2022 Fredrik Mellbin
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

#ifndef AUDIOSOURCE_H
#define AUDIOSOURCE_H

// FIXME, wrap everything its own namespace?
// FIXME, export delay relative to first video track somehow

#include <list>
#include <vector>
#include <string>
#include <cstdint>
#include <stdexcept>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

class AudioException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct AudioProperties {
    bool IsFloat;
    int BytesPerSample;
    int BitsPerSample;
    int SampleRate;
    int Channels;
    uint64_t ChannelLayout;
    int64_t NumSamples; /* estimated by decoder, may be wrong */
    int64_t StartTime; /* in samples, equivalent to the offset used to have a zero start time */
};

/* These correspond to the FFmpeg options of the same name */
struct FFmpegAudioOptions {
    /* mp4/mov/3gpp demuxer options */
    bool enable_drefs = false;
    bool use_absolute_path = false;
    /* ac3 decoder family options */
    double drc_scale = 0;
};

class LWAudioDecoder {
private:
    AudioProperties AP = {};
    AVFormatContext *FormatContext = nullptr;
    AVCodecContext *CodecContext = nullptr;
    AVFrame *DecodeFrame = nullptr;
    int64_t CurrentPosition = 0;
    int64_t CurrentFrame = 0;
    int TrackNumber = -1;
    bool DecodeSuccess = false;
    AVPacket *Packet = nullptr;

    void OpenFile(const char *SourceFile, int Track, const FFmpegAudioOptions &Options);
    bool ReadPacket(AVPacket *Packet);
    bool DecodeNextAVFrame();
    void Free();
public:
    LWAudioDecoder(const char *SourceFile, int Track, const FFmpegAudioOptions &Options); // Positive track numbers are absolute. Negative track numbers mean nth audio track to simplify things.
    ~LWAudioDecoder();
    int GetTrack() const; // Useful when opening nth audio track to get the actual number
    int64_t GetRelativeStartTime(int Track) const; // Returns INT64_MIN on error
    int64_t GetSamplePosition() const;
    int64_t GetSampleLength() const;
    int64_t GetFrameNumber() const;
    const AudioProperties &GetAudioProperties() const;
    AVFrame *GetNextAVFrame();
    bool SkipNextAVFrame();
    bool HasMoreFrames() const;
};

class BestAudioSource {
private:
    class CacheBlock {
    private:
        AVFrame *InternalFrame = nullptr;
        std::vector<uint8_t> Storage;
        size_t LineSize = 0;
    public:
        int64_t FrameNumber;
        int64_t Start;
        int64_t Length;

        CacheBlock(int64_t FrameNumber, int64_t Start, AVFrame *Frame);
        ~CacheBlock();
        uint8_t *GetPlanePtr(int Plane);
    };

    static constexpr size_t MaxAudioSources = 4;
    FFmpegAudioOptions FFOptions = {};
    AudioProperties AP = {};
    std::string Source;
    int AudioTrack;
    bool HasExactNumAudioSamples = false;
    uint64_t DecoderSequenceNum = 0;
    uint64_t DecoderLastUse[MaxAudioSources] = {};
    LWAudioDecoder *Decoders[MaxAudioSources] = {};
    std::list<CacheBlock> Cache;
    size_t MaxSize;
    size_t CacheSize = 0;
    int64_t PreRoll = 200000;

    void ZeroFillStart(uint8_t *Data[], int64_t &Start, int64_t &Count);
    void ZeroFillEnd(uint8_t *Data[], int64_t Start, int64_t &Count);
    bool FillInBlock(CacheBlock &Block, uint8_t *Data[], int64_t &Start, int64_t &Count);
public:
    BestAudioSource(const char *SourceFile, int Track, int AjustDelay = -2, const FFmpegAudioOptions *Options = nullptr);
    ~BestAudioSource();
    int GetTrack() const; // Useful when opening nth audio track to get the actual number
    void SetMaxCacheSize(size_t bytes); /* default max size is 100MB */
    void SetSeekPreRoll(size_t samples); /* the number of samples to cache before the position being fast forwarded to, default is 200k samples */
    bool GetExactDuration();
    const AudioProperties &GetAudioProperties() const;
    void GetAudio(uint8_t * const * const Data, int64_t Start, int64_t Count); // Audio outside the existing range is zeroed
};

#endif
