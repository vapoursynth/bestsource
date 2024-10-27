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

#ifndef TRACKLIST_H
#define TRACKLIST_H

#include "bsshared.h"
#include <map>
#include <memory>
#include <vector>

struct AVFormatContext;

struct BestTrackList {
public:
    struct TrackInfo {
        int MediaType;
        std::string MediaTypeString;
        int Codec;
        std::string CodecString;
        int Disposition;
        std::string DispositionString;
    };
private:
    AVFormatContext *FormatContext = nullptr;

    std::vector<TrackInfo> TrackList;

    void OpenFile(const std::filesystem::path &SourceFile, const std::map<std::string, std::string> &LAVFOpts);
    void Free();
public:
    BestTrackList(const std::filesystem::path &SourceFile, const std::map<std::string, std::string> *LAVFOpts);
    ~BestTrackList();
    [[nodiscard]] int GetNumTracks() const;
    [[nodiscard]] const TrackInfo &GetTrackInfo(int Track) const;
    [[nodiscard]] std::map<std::string, std::string> GetFileMetadata() const;
    [[nodiscard]] std::map<std::string, std::string> GetTrackMetadata(int Track) const;
};

#endif
