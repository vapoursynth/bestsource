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

#include "tracklist.h"
#include <cassert>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

void BestTrackList::OpenFile(const std::filesystem::path &SourceFile, const std::map<std::string, std::string> &LAVFOpts) {
    AVDictionary *Dict = nullptr;
    for (const auto &Iter : LAVFOpts)
        av_dict_set(&Dict, Iter.first.c_str(), Iter.second.c_str(), 0);

    if (avformat_open_input(&FormatContext, SourceFile.u8string().c_str(), nullptr, &Dict) != 0) {
        av_dict_free(&Dict);
        throw BestSourceException("Couldn't open '" + SourceFile.u8string() + "'");
    }

    av_dict_free(&Dict);

    if (avformat_find_stream_info(FormatContext, nullptr) < 0) {
        avformat_close_input(&FormatContext);
        FormatContext = nullptr;
        throw BestSourceException("Couldn't find stream information");
    }

    for (unsigned int i = 0; i < FormatContext->nb_streams; i++) {
        TrackInfo TI = {};
        TI.MediaType = FormatContext->streams[i]->codecpar->codec_type;
        TI.MediaTypeString = av_get_media_type_string(FormatContext->streams[i]->codecpar->codec_type);
        TI.Codec = FormatContext->streams[i]->codecpar->codec_id;
        TI.CodecString = avcodec_get_name(FormatContext->streams[i]->codecpar->codec_id);

        int Disposition = FormatContext->streams[i]->disposition;
        TI.Disposition = Disposition;

        while (const char *DispPart = av_disposition_to_string(Disposition)) {
            if (!TI.DispositionString.empty())
                TI.DispositionString += ", ";
            TI.DispositionString += DispPart;
            Disposition = Disposition & (Disposition - 1);
        }

        TrackList.push_back(TI);
    }
}

BestTrackList::BestTrackList(const std::filesystem::path &SourceFile, const std::map<std::string, std::string> *LAVFOpts) {
    std::map<std::string, std::string> LAVFOptions;
    if (LAVFOpts)
        LAVFOptions = *LAVFOpts;
    try {
        OpenFile(SourceFile, LAVFOptions);
    } catch (...) {
        Free();
        throw;
    }
}

void BestTrackList::Free() {
    avformat_close_input(&FormatContext);
}

BestTrackList::~BestTrackList() {
    Free();
}

int BestTrackList::GetNumTracks() const {
    return static_cast<int>(TrackList.size());
}

const BestTrackList::TrackInfo &BestTrackList::GetTrackInfo(int Track) const {
    return TrackList[Track];
}