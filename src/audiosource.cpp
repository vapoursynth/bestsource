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

#include "audiosource.h"
#include "videosource.h"

#include <algorithm>
#include <memory>
#include <thread>

extern "C" {
#include <libavutil/dict.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

bool LWAudioDecoder::ReadPacket(AVPacket *Packet) {
    while (av_read_frame(FormatContext, Packet) >= 0) {
        if (Packet->stream_index == TrackNumber)
            return true;
        av_packet_unref(Packet);
    }
    return false;
}

bool LWAudioDecoder::DecodeNextAVFrame() {
    if (!DecodeFrame) {
        DecodeFrame = av_frame_alloc();
        if (!DecodeFrame)
            throw AudioException("Couldn't allocate frame");
    }

    while (true) {
        int Ret = avcodec_receive_frame(CodecContext, DecodeFrame);
        if (Ret == 0) {
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

void LWAudioDecoder::OpenFile(const std::string &SourceFile, int Track, int Threads, const std::map<std::string, std::string> &LAVFOpts, double DrcScale) {
    TrackNumber = Track;

    AVDictionary *Dict = nullptr;
    for (const auto &Iter : LAVFOpts)
        av_dict_set(&Dict, Iter.first.c_str(), Iter.second.c_str(), 0);

    if (avformat_open_input(&FormatContext, SourceFile.c_str(), nullptr, &Dict) != 0)
        throw AudioException("Couldn't open '" + SourceFile + "'");

    av_dict_free(&Dict);

    if (avformat_find_stream_info(FormatContext, nullptr) < 0) {
        avformat_close_input(&FormatContext);
        FormatContext = nullptr;
        throw AudioException("Couldn't find stream information");
    }

    if (TrackNumber < 0) {
        for (int i = 0; i < static_cast<int>(FormatContext->nb_streams); i++) {
            if (FormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
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
        throw AudioException("Invalid track index");

    if (FormatContext->streams[TrackNumber]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
        throw AudioException("Not an audio track");

    for (int i = 0; i < static_cast<int>(FormatContext->nb_streams); i++)
        if (i != TrackNumber)
            FormatContext->streams[i]->discard = AVDISCARD_ALL;

    const AVCodec *Codec = avcodec_find_decoder(FormatContext->streams[TrackNumber]->codecpar->codec_id);
    if (Codec == nullptr)
        throw AudioException("Audio codec not found");

    CodecContext = avcodec_alloc_context3(Codec);
    if (CodecContext == nullptr)
        throw AudioException("Could not allocate audio decoding context");

    if (avcodec_parameters_to_context(CodecContext, FormatContext->streams[TrackNumber]->codecpar) < 0)
        throw AudioException("Could not copy audio codec parameters");

    if (Threads < 1)
        Threads = static_cast<int>(std::thread::hardware_concurrency());
    CodecContext->thread_count = std::min(Threads, 16);

    // FIXME, drop this when encountered
    // Probably guard against mid-stream format changes
    CodecContext->flags |= AV_CODEC_FLAG_DROPCHANGED;

    if (DrcScale < 0)
        throw AudioException("Invalid drc_scale value");

    AVDictionary *CodecDict = nullptr;
    if (Codec->id == AV_CODEC_ID_AC3 || Codec->id == AV_CODEC_ID_EAC3)
        av_dict_set(&CodecDict, "drc_scale", std::to_string(DrcScale).c_str(), 0);

    if (avcodec_open2(CodecContext, Codec, &CodecDict) < 0)
        throw AudioException("Could not open audio codec");

    av_dict_free(&CodecDict);
}

LWAudioDecoder::LWAudioDecoder(const std::string &SourceFile, int Track, int Threads, const std::map<std::string, std::string> &LAVFOpts, double DrcScale) {
    try {
        Packet = av_packet_alloc();
        OpenFile(SourceFile, Track, Threads, LAVFOpts, DrcScale);

        DecodeSuccess = DecodeNextAVFrame();
        
        if (!DecodeSuccess)
            throw AudioException("Couldn't decode initial frame");

        AP.IsFloat = (DecodeFrame->format == AV_SAMPLE_FMT_FLTP || DecodeFrame->format == AV_SAMPLE_FMT_FLT || DecodeFrame->format == AV_SAMPLE_FMT_DBLP || DecodeFrame->format == AV_SAMPLE_FMT_DBL);
        AP.BytesPerSample = av_get_bytes_per_sample(static_cast<AVSampleFormat>(DecodeFrame->format));
        AP.BitsPerSample = CodecContext->bits_per_raw_sample ? (CodecContext->bits_per_raw_sample) : (AP.BytesPerSample * 8); // assume all bits are relevant if not specified
        AP.SampleRate = DecodeFrame->sample_rate;
        AP.Channels = DecodeFrame->ch_layout.nb_channels;

        if (DecodeFrame->ch_layout.order == AV_CHANNEL_ORDER_NATIVE) {
            AP.ChannelLayout = DecodeFrame->ch_layout.u.mask;
        } else if (DecodeFrame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) {
            AVChannelLayout ch = {};
            av_channel_layout_default(&ch, DecodeFrame->ch_layout.nb_channels);
            AP.ChannelLayout = ch.u.mask;
        } else {
            throw AudioException("Ambisonics and custom channel orders not supported");
        }

        AP.NumSamples = (FormatContext->duration * DecodeFrame->sample_rate) / AV_TIME_BASE - FormatContext->streams[TrackNumber]->codecpar->initial_padding;
        if (DecodeFrame->pts != AV_NOPTS_VALUE)
            AP.StartTime = (static_cast<double>(FormatContext->streams[TrackNumber]->time_base.num) * DecodeFrame->pts) / FormatContext->streams[TrackNumber]->time_base.den;

        if (AP.BytesPerSample <= 0)
            throw AudioException("Codec returned zero size audio");

    } catch (...) {
        Free();
        throw;
    }
}

void LWAudioDecoder::Free() {
    av_packet_free(&Packet);
    av_frame_free(&DecodeFrame);
    avcodec_free_context(&CodecContext);
    avformat_close_input(&FormatContext);
}

LWAudioDecoder::~LWAudioDecoder() {
    Free();
}

int64_t LWAudioDecoder::GetSourceSize() const {
    return avio_size(FormatContext->pb);
}

int64_t LWAudioDecoder::GetSourcePostion() const {
    return avio_tell(FormatContext->pb);
}

int LWAudioDecoder::GetTrack() const {
    return TrackNumber;
}

int64_t LWAudioDecoder::GetSamplePosition() const {
    return CurrentPosition;
}

int64_t LWAudioDecoder::GetSampleLength() const {
    if (DecodeFrame)
        return DecodeFrame->nb_samples;
    else
        return 0;
}

int64_t LWAudioDecoder::GetFrameNumber() const {
    return CurrentFrame;
}

const AudioProperties &LWAudioDecoder::GetAudioProperties() const { 
    return AP;
}

AVFrame *LWAudioDecoder::GetNextAVFrame() {
    if (DecodeSuccess) {
        CurrentPosition += DecodeFrame->nb_samples;
        CurrentFrame++;
        AVFrame *Tmp = DecodeFrame;
        DecodeFrame = nullptr;
        DecodeSuccess = DecodeNextAVFrame();
        return Tmp;
    } 
    return nullptr;
}

bool LWAudioDecoder::SkipNextAVFrame() {
    if (DecodeSuccess) {
        CurrentPosition += DecodeFrame->nb_samples;
        CurrentFrame++;
        DecodeSuccess = DecodeNextAVFrame();    
    } 
    return DecodeSuccess;
}

bool LWAudioDecoder::HasMoreFrames() const {
    return DecodeSuccess;
}

template<typename T>
static void UnpackChannels(const uint8_t *Src, uint8_t *Dst, size_t Length, size_t Channels) {
    const T *S = reinterpret_cast<const T *>(Src);
    T *D = reinterpret_cast<T *>(Dst);
    for (size_t i = 0; i < Length; i++) {
        for (size_t c = 0; c < Channels; c++)
            D[Length * c] = S[c];
        S += Channels;
        D += 1;
    }
}

template<typename T>
void PackChannels(const uint8_t *const *const Src, uint8_t *Dst, size_t Length, size_t Channels) {
    const T *const *const S = reinterpret_cast<const T *const *>(Src);
    T *D = reinterpret_cast<T *>(Dst);
    for (size_t i = 0; i < Length; i++) {
        for (size_t c = 0; c < Channels; c++)
            D[c] = S[c][i];
        D += Channels;
    }
}

BestAudioSource::CacheBlock::CacheBlock(int64_t FrameNumber, int64_t Start, AVFrame *Frame) : FrameNumber(FrameNumber), Start(Start), Length(Frame->nb_samples) {
    if (av_sample_fmt_is_planar(static_cast<AVSampleFormat>(Frame->format))) {
        InternalFrame = Frame;
    } else {
        int BytesPerSample = av_get_bytes_per_sample(static_cast<AVSampleFormat>(Frame->format));
        LineSize = Length * BytesPerSample;
        Storage.resize(LineSize * Frame->ch_layout.nb_channels);

        if (BytesPerSample == 1)
            UnpackChannels<uint8_t>(Frame->data[0], Storage.data(), Length, Frame->ch_layout.nb_channels);
        else if (BytesPerSample == 2)
            UnpackChannels<uint16_t>(Frame->data[0], Storage.data(), Length, Frame->ch_layout.nb_channels);
        else if (BytesPerSample == 4)
            UnpackChannels<uint32_t>(Frame->data[0], Storage.data(), Length, Frame->ch_layout.nb_channels);
        else if (BytesPerSample == 8)
            UnpackChannels<uint64_t>(Frame->data[0], Storage.data(), Length, Frame->ch_layout.nb_channels);
        av_frame_free(&Frame);
    }
}

BestAudioSource::CacheBlock::~CacheBlock() {
    av_frame_free(&InternalFrame);
}

uint8_t *BestAudioSource::CacheBlock::GetPlanePtr(int Plane) {
    if (InternalFrame)
        return av_frame_get_plane_buffer(InternalFrame, Plane)->data;
    else
        return Storage.data() + Plane * LineSize;
}

BestAudioSource::BestAudioSource(const std::string &SourceFile, int Track, int AjustDelay, int Threads, const std::string &CachePath, const std::map<std::string, std::string> *LAVFOpts, double DrcScale) : Source(SourceFile), AudioTrack(Track), Threads(Threads), DrcScale(DrcScale) {
    this->CachePath = CachePath;
    if (LAVFOpts)
        LAVFOptions = *LAVFOpts;
    Decoders[0] = new LWAudioDecoder(Source, Track, Threads, LAVFOptions, DrcScale);
    AP = Decoders[0]->GetAudioProperties();
    AudioTrack = Decoders[0]->GetTrack();

    if (AjustDelay >= -1)
        SampleDelay = static_cast<int64_t>(GetRelativeStartTime(AjustDelay) * AP.SampleRate);

    AP.NumSamples += SampleDelay;


    MaxSize = (100 * 1024 * 1024) / (static_cast<size_t>(AP.Channels) * AP.BytesPerSample);
}

BestAudioSource::~BestAudioSource() {
    for (auto Iter : Decoders)
        delete Iter;
}

int BestAudioSource::GetTrack() const {
    return AudioTrack;
}

void BestAudioSource::SetMaxCacheSize(size_t Bytes) {
    MaxSize = Bytes / (static_cast<size_t>(AP.Channels) * AP.BytesPerSample);
    while (CacheSize > MaxSize) {
        CacheSize -= Cache.back().Length;
        Cache.pop_back();
    }
}

void BestAudioSource::SetSeekPreRoll(int64_t Samples) {
    PreRoll = Samples;
}

double BestAudioSource::GetRelativeStartTime(int Track) const {
    if (Track < 0) {
        try {
            std::unique_ptr<LWVideoDecoder> Dec(new LWVideoDecoder(Source, "", 0, Track, true, 0, LAVFOptions));
            VideoProperties VP;
            Dec->SkipFrames(1);
            Dec->GetVideoProperties(VP);
            return AP.StartTime - VP.StartTime;
        } catch (VideoException &) {
        }
        return 0;
    } else {
        try {
            std::unique_ptr<LWVideoDecoder> Dec(new LWVideoDecoder(Source, "", 0, Track, true, 0, LAVFOptions));
            VideoProperties VP;
            Dec->SkipFrames(1);
            Dec->GetVideoProperties(VP);
            return AP.StartTime - VP.StartTime;
        } catch (VideoException &) {
            try {
                std::unique_ptr<LWAudioDecoder> Dec(new LWAudioDecoder(Source, Track, Threads, LAVFOptions, 0));
                return AP.StartTime - Dec->GetAudioProperties().StartTime;
            } catch (AudioException &) {
                throw AudioException("Can't get delay relative to track");
            }
        }
    }
}

bool BestAudioSource::GetExactDuration(const std::function<void(int64_t Current, int64_t Total)> &Progress) {
    if (HasExactNumAudioSamples)
        return true;
    int Index = -1;
    for (int i = 0; i < MaxAudioSources; i++) {
        if (Decoders[i] && (Index < 0 || Decoders[Index]->GetFrameNumber() < Decoders[i]->GetFrameNumber()))
            Index = i;
    }

    if (Index < 0) {
        Decoders[0] = new LWAudioDecoder(Source, AudioTrack, Threads, LAVFOptions, DrcScale);
        Index = 0;
    }

    LWAudioDecoder *Decoder = Decoders[Index];

    int64_t FileSize = Progress ? Decoder->GetSourceSize() : -1;

    while (Decoder->SkipNextAVFrame()) {
        if (Progress)
            Progress(Decoder->GetSourcePostion(), FileSize);
    }

    if (Progress)
        Progress(INT64_MAX, INT64_MAX);

    AP.NumSamples = Decoder->GetSamplePosition();
    HasExactNumAudioSamples = true;
    delete Decoder;
    Decoders[Index] = nullptr;

    return true;
}

const AudioProperties &BestAudioSource::GetAudioProperties() const {
    return AP;
}

void BestAudioSource::ZeroFillStart(uint8_t *Data[], int64_t &Start, int64_t &Count) {
    if (Start < 0) {
        int64_t Length = std::min(Count, -Start);
        size_t ByteLength = Length * AP.BytesPerSample;
        for (int i = 0; i < AP.Channels; i++) {
            memset(Data[i], 0, ByteLength);
            Data[i] += ByteLength;
        }
        Start += Length;
        Count -= Length;
    }
}

void BestAudioSource::ZeroFillEnd(uint8_t *Data[], int64_t Start, int64_t &Count) {
    if (HasExactNumAudioSamples && (Start + Count > AP.NumSamples)) {
        int64_t Length = std::min(Start + Count - AP.NumSamples, Count);
        size_t ByteOffset = std::min<int64_t>(AP.NumSamples - Start, 0) * AP.BytesPerSample;
        for (int i = 0; i < AP.Channels; i++)
            memset(Data[i] + ByteOffset, 0, Length * AP.BytesPerSample);
        Count -= Length;
    }
}

bool BestAudioSource::FillInBlock(CacheBlock &Block, uint8_t *Data[], int64_t &Start, int64_t &Count) {
    if ((Start >= Block.Start) && (Start < Block.Start + Block.Length)) {
        int64_t Length = std::min(Count, Block.Length - Start + Block.Start);
        if (Length == 0)
            return false;
        size_t ByteLength = Length * AP.BytesPerSample;
        size_t ByteOffset = (Start - Block.Start) * AP.BytesPerSample;;
        for (int i = 0; i < AP.Channels; i++) {
            memcpy(Data[i], Block.GetPlanePtr(i) + ByteOffset, ByteLength);
            Data[i] += ByteLength;
        }
        Start += Length;
        Count -= Length;
        return true;
    }
    return false;
}

void BestAudioSource::GetPlanarAudio(uint8_t * const * const Data, int64_t Start, int64_t Count) {
    Start -= SampleDelay;

    if (Count <= 0)
        return;

    std::vector<uint8_t *> DataV;
    DataV.reserve(AP.Channels);
    for (int i = 0; i < AP.Channels; i++)
        DataV.push_back(Data[i]);

    ZeroFillStart(DataV.data(), Start, Count);

    ZeroFillEnd(DataV.data(), Start, Count);

    if (Count == 0)
        return;

    // Check if in cache, use a simple check from start and end and narrow it from both ends at once
    bool FilledIn = false;
    do {
        FilledIn = false;
        for (auto &Iter : Cache)
            FilledIn = FilledIn || FillInBlock(Iter, DataV.data(), Start, Count);
    } while (FilledIn);

    if (Count == 0)
        return;

    int Index = -1;
    for (int i = 0; i < MaxAudioSources; i++) {
        if (Decoders[i] && Decoders[i]->GetSamplePosition() <= Start && (Index < 0 || Decoders[Index]->GetSamplePosition() < Decoders[i]->GetSamplePosition()))
            Index = i;
    }

    // If an empty slot exists simply spawn a new decoder there
    if (Index < 0) {
        for (int i = 0; i < MaxAudioSources; i++) {
            if (!Decoders[i]) {
                Index = i;
                Decoders[i] = new LWAudioDecoder(Source, AudioTrack, Threads, LAVFOptions, DrcScale);
                break;
            }
        }
    }

    // No far enough back decoder exists and all slots are occupied so evict a random one
    if (Index < 0) {
        Index = 0;
        for (int i = 0; i < MaxAudioSources; i++) {
            if (Decoders[i] && DecoderLastUse[i] < DecoderLastUse[Index])
                Index = i;
        }
        delete Decoders[Index];
        Decoders[Index] = new LWAudioDecoder(Source, AudioTrack, Threads, LAVFOptions, DrcScale);
    }

    LWAudioDecoder *Decoder = Decoders[Index];
    DecoderLastUse[Index] = DecoderSequenceNum++;

    while (Count > 0 && Decoder && Decoder->GetSamplePosition() < Start + Count && Decoder->HasMoreFrames()) {
        int64_t Position = Decoder->GetSamplePosition();
        int64_t Length = Decoder->GetSampleLength();
        if (Position + Length >= Start - PreRoll) {
            int64_t FrameNumber = Decoder->GetFrameNumber();
            AVFrame *Frame = Decoder->GetNextAVFrame();
            
            for (auto Iter = Cache.begin(); Iter != Cache.end(); ++Iter) {
                if (Iter->FrameNumber == FrameNumber) {
                    Cache.erase(Iter);
                    break;
                }
            }
                    
            Cache.emplace_front(FrameNumber, Position, Frame);
            CacheSize += Cache.front().Length;

            FillInBlock(Cache.front(), DataV.data(), Start, Count);

            while (CacheSize > MaxSize) {
                CacheSize -= Cache.back().Length;
                Cache.pop_back();
            }

        } else if (Position + Length < Start + Count) {
            Decoder->SkipNextAVFrame();
        }

        if (!Decoder->HasMoreFrames()) {
            AP.NumSamples = Decoder->GetSamplePosition();
            HasExactNumAudioSamples = true;
            delete Decoder;
            Decoders[Index] = nullptr;
            Decoder = nullptr;
        }
    }

    if (HasExactNumAudioSamples)
        ZeroFillEnd(DataV.data(), Start, Count);

    if (Count != 0)
        throw AudioException("Code error, failed to provide all samples");
}

void BestAudioSource::GetPackedAudio(uint8_t *Data, int64_t Start, int64_t Count) {
    if (Count <= 0)
        return;
    size_t ChannelSize = Count * AP.BytesPerSample;
    uint8_t *Tmp = new uint8_t[ChannelSize * AP.Channels];
    std::vector<uint8_t *> DataV;
    DataV.reserve(AP.Channels);
    for (int i = 0; i < AP.Channels; i++)
        DataV.push_back(Tmp + i * ChannelSize);
    GetPlanarAudio(DataV.data(), Start, Count);
    if (AP.BytesPerSample == 1)
        PackChannels<uint8_t>(DataV.data(), Data, Count, AP.Channels);
    else if (AP.BytesPerSample == 2)
        PackChannels<uint16_t>(DataV.data(), Data, Count, AP.Channels);
    else if (AP.BytesPerSample == 4)
        PackChannels<uint32_t>(DataV.data(), Data, Count, AP.Channels);
    else if (AP.BytesPerSample == 8)
        PackChannels<uint64_t>(DataV.data(), Data, Count, AP.Channels);
}