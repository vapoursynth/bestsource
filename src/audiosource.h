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

#include <list>
#include <map>
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
    double StartTime; /* in seconds */
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

    void OpenFile(const std::string &SourceFile, int Track, const std::map<std::string, std::string> &LAVFOpts, double DrcScale);
    bool ReadPacket(AVPacket *Packet);
    bool DecodeNextAVFrame();
    void Free();
public:
    LWAudioDecoder(const std::string &SourceFile, int Track, const std::map<std::string, std::string> &LAVFOpts, double DrcScale); // Positive track numbers are absolute. Negative track numbers mean nth audio track to simplify things.
    ~LWAudioDecoder();
    int GetTrack() const; // Useful when opening nth audio track to get the actual number
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
    std::map<std::string, std::string> LAVFOptions;
    double DrcScale;
    AudioProperties AP = {};
    std::string Source;
    std::string CachePath;
    int AudioTrack;
    bool HasExactNumAudioSamples = false;
    uint64_t DecoderSequenceNum = 0;
    uint64_t DecoderLastUse[MaxAudioSources] = {};
    LWAudioDecoder *Decoders[MaxAudioSources] = {};
    std::list<CacheBlock> Cache;
    size_t MaxSize;
    size_t CacheSize = 0;
    int64_t PreRoll = 2000000;
    int64_t SampleDelay = 0;

    void ZeroFillStart(uint8_t *Data[], int64_t &Start, int64_t &Count);
    void ZeroFillEnd(uint8_t *Data[], int64_t Start, int64_t &Count);
    bool FillInBlock(CacheBlock &Block, uint8_t *Data[], int64_t &Start, int64_t &Count);
public:
    BestAudioSource(const std::string &SourceFile, int Track, int AjustDelay, const char *CachePath, const std::map<std::string, std::string> *LAVFOpts = nullptr, double DrcScale = 0);
    ~BestAudioSource();
    int GetTrack() const; // Useful when opening nth audio track to get the actual number
    void SetMaxCacheSize(size_t Bytes); /* default max size is 100MB */
    void SetSeekPreRoll(int64_t Samples); /* the number of samples to cache before the position being fast forwarded to, default is 200k samples */
    double GetRelativeStartTime(int Track) const; // Offset in seconds
    bool GetExactDuration();
    const AudioProperties &GetAudioProperties() const;
    void GetAudio(uint8_t * const * const Data, int64_t Start, int64_t Count); // Audio outside the existing range is zeroed
};

#endif
