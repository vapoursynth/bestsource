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

#ifndef AUDIOSOURCE_H
#define AUDIOSOURCE_H

#include "bsshared.h"
#include <cstdint>
#include <list>
#include <string>
#include <map>
#include <set>
#include <vector>
#include <array>
#include <memory>

struct AVFormatContext;
struct AVCodecContext;
struct AVBufferRef;
struct AVFrame;
struct AVPacket;

struct BSAudioFormat {
    bool Float;
    int Bits;
    int BytesPerSample; // FIXME, kinda odd to have it exposed here but very useful to store internally

    void Set(int Format, int BitsPerRawSample);
};

struct LWAudioProperties {
    BSRational TimeBase;
    int64_t Duration;

    int64_t NumSamples; /* estimated by decoder, may be wrong */
};

struct BSAudioProperties : public LWAudioProperties {
    BSAudioFormat AF;
    int Format;
    int BitsPerSample; // fixme, redundant?
    int SampleRate;
    int Channels;
    uint64_t ChannelLayout;

    int64_t NumFrames;

    double StartTime; /* in seconds */
};

class LWAudioDecoder {
private:
    AVFormatContext *FormatContext = nullptr;
    AVCodecContext *CodecContext = nullptr;
    AVFrame *DecodeFrame = nullptr;
    int64_t CurrentFrame = 0;
    int64_t CurrentSample = 0;
    int TrackNumber = -1;
    bool DecodeSuccess = true;
    AVPacket *Packet = nullptr;
    bool Seeked = false;

    void OpenFile(const std::filesystem::path &SourceFile, int Track, int Threads, const std::map<std::string, std::string> &LAVFOpts, double DrcScale);
    bool ReadPacket();
    bool DecodeNextFrame(bool SkipOutput = false);
    void Free();
public:
    LWAudioDecoder(const std::filesystem::path &SourceFile, int Track, int Threads, const std::map<std::string, std::string> &LAVFOpts, double DrcScale); // Positive track numbers are absolute. Negative track numbers mean nth audio track to simplify things.
    ~LWAudioDecoder();
    [[nodiscard]] int64_t GetSourceSize() const;
    [[nodiscard]] int64_t GetSourcePostion() const;
    [[nodiscard]] int GetTrack() const; // Useful when opening nth video track to get the actual number
    [[nodiscard]] int64_t GetFrameNumber() const; // The frame you will get when calling GetNextFrame()
    [[nodiscard]] int64_t GetSamplePos() const; // The frame you will get when calling GetNextFrame()
    void SetFrameNumber(int64_t N, int64_t SampleNumber); // Use after seeking to update internal frame number
    void GetAudioProperties(LWAudioProperties &VP); // Decodes one frame and advances the position to retrieve the full properties, only call directly after creation
    [[nodiscard]] AVFrame *GetNextFrame(int *BitsPerSample = nullptr);
    bool SkipFrames(int64_t Count);
    [[nodiscard]] bool HasMoreFrames() const;
    [[nodiscard]] bool Seek(int64_t PTS); // Note that the current frame number isn't updated and if seeking fails the decoder is in an undefined state
    [[nodiscard]] bool HasSeeked() const;
};


class BestAudioFrame {
private:
    AVFrame *Frame;
public:
    BestAudioFrame(AVFrame *Frame, int BitsPerSample);
    ~BestAudioFrame();
    [[nodiscard]] const AVFrame *GetAVFrame() const;
    BSAudioFormat AF;
    int NumChannels;

    int64_t Pts;
    int64_t NumSamples;
};

class BestAudioSource {
public:
    struct FormatSet {
        BSAudioFormat AF = {};
        int Format;
        int BitsPerSample; // FIXME, maybe redundant
        int SampleRate;
        int Channels;
        uint64_t ChannelLayout;

        double StartTime = 0;

        int64_t NumFrames; // can be -1 to signal that the number of frames is completely unknown
        int64_t NumSamples;
    };

    struct FrameInfo {
        int64_t PTS;
        int64_t Start;
        int64_t Length;
        int Format;
        int BitsPerSample;
        int SampleRate;
        int Channels;
        uint64_t ChannelLayout;
        std::array<uint8_t, HashSize> Hash;
    };
private:
    struct AudioTrackIndex {
        std::vector<FrameInfo> Frames;
    };

    bool WriteAudioTrackIndex(bool AbsolutePath, const std::filesystem::path &CachePath);
    bool ReadAudioTrackIndex(bool AbsolutePath, const std::filesystem::path &CachePath);

    class Cache {
    private:
        class CacheBlock {
        public:
            int64_t FrameNumber;
            AVFrame *Frame;
            size_t Size = 0;
            CacheBlock(int64_t FrameNumber, AVFrame *Frame);
            ~CacheBlock();
        };

        const AudioTrackIndex &TrackIndex;
        size_t Size = 0;
        size_t MaxSize = 100 * 1024 * 1024;
        std::list<CacheBlock> Data;
        void ApplyMaxSize();
    public:
        Cache(const AudioTrackIndex &TrackIndex);
        void Clear();
        void SetMaxSize(size_t Bytes);
        void CacheFrame(int64_t FrameNumber, AVFrame *Frame); // Takes ownership of Frame
        [[nodiscard]] BestAudioFrame *GetFrame(int64_t N);
    };

    AudioTrackIndex TrackIndex;
    Cache FrameCache;

    std::vector<FormatSet> FormatSets;
    FormatSet DefaultFormatSet;

    static constexpr int MaxAudioDecoders = 4;
    int MaxUsedAudioDecoders = MaxAudioDecoders;
    std::map<std::string, std::string> LAVFOptions;
    double DrcScale;
    BSAudioProperties AP = {};
    std::filesystem::path Source;
    int AudioTrack;
    int VariableFormat = -1;
    int Threads;
    bool LinearMode = false;
    uint64_t DecoderSequenceNum = 0;
    uint64_t DecoderLastUse[MaxAudioDecoders] = {};
    std::unique_ptr<LWAudioDecoder> Decoders[MaxAudioDecoders];
    int64_t PreRoll = 40;
    int64_t SampleDelay = 0;
    int64_t FileSize = -1;
    static constexpr size_t RetrySeekAttempts = 10;
    std::set<int64_t> BadSeekLocations;
    void SetLinearMode();
    [[nodiscard]] int64_t GetSeekFrame(int64_t N);
    [[nodiscard]] BestAudioFrame *SeekAndDecode(int64_t N, int64_t SeekFrame, std::unique_ptr<LWAudioDecoder> &Decoder, size_t Depth = 0);
    [[nodiscard]] BestAudioFrame *GetFrameInternal(int64_t N);
    [[nodiscard]] BestAudioFrame *GetFrameLinearInternal(int64_t N, int64_t SeekFrame = -1, size_t Depth = 0, bool ForceUnseeked = false);
    [[nodiscard]] bool IndexTrack(const ProgressFunction &Progress = nullptr);
    void InitializeFormatSets();
    void ZeroFillStartPacked(uint8_t *&Data, int64_t &Start, int64_t &Count);
    void ZeroFillEndPacked(uint8_t *Data, int64_t Start, int64_t &Count);
    bool FillInFramePacked(const BestAudioFrame *Frame, int64_t FrameStartSample, uint8_t *&Data, int64_t &Start, int64_t &Count);
    void ZeroFillStartPlanar(uint8_t *Data[], int64_t &Start, int64_t &Count);
    void ZeroFillEndPlanar(uint8_t *Data[], int64_t Start, int64_t &Count);
    bool FillInFramePlanar(const BestAudioFrame *Frame, int64_t FrameStartSample, uint8_t *Data[], int64_t &Start, int64_t &Count);
public:
    struct FrameRange {
        int64_t First;
        int64_t Last;
        int64_t FirstSamplePos;
    };

    BestAudioSource(const std::filesystem::path &SourceFile, int Track, int AjustDelay, int Threads, int CacheMode, const std::filesystem::path &CachePath, const std::map<std::string, std::string> *LAVFOpts, double DrcScale, const ProgressFunction &Progress = nullptr);
    [[nodiscard]] int GetTrack() const; // Useful when opening nth video track to get the actual number
    void SetMaxCacheSize(size_t Bytes); /* default max size is 1GB */
    void SetSeekPreRoll(int64_t Frames); /* the number of frames to cache before the position being fast forwarded to */
    double GetRelativeStartTime(int Track) const;
    [[nodiscard]] const BSAudioProperties &GetAudioProperties() const;
    [[nodiscard]] const std::vector<FormatSet> &GetFormatSets() const; /* Get a listing of all the number of formats  */
    void SelectFormatSet(int Index); /* Sets the output format to the specified format set, passing -1 means the default variable format will be used */
    [[nodiscard]] BestAudioFrame *GetFrame(int64_t N, bool Linear = false);
    [[nodiscard]] FrameRange GetFrameRangeBySamples(int64_t Start, int64_t Count) const;
    void GetPackedAudio(uint8_t *Data, int64_t Start, int64_t Count);
    void GetPlanarAudio(uint8_t *const *const Data, int64_t Start, int64_t Count);
    [[nodiscard]] const FrameInfo &GetFrameInfo(int64_t N) const;
    [[nodiscard]] bool GetLinearDecodingState() const;
    int SetMaxDecoderInstances(int NumInstances); /* Default value is MaxAudioDecoders */
};

#endif
