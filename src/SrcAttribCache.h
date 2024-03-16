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

#ifndef SRC_ATTRIB_CACHE_H
#define SRC_ATTRIB_CACHE_H

#include <string>
#include <map>
#include <vector>
#include <cstdint>

struct VideoTrackIndex {
    struct FrameInfo {
        int64_t PTS;
        int RepeatPict;
        bool KeyFrame;
        bool TFF;
        uint8_t Hash[16];
    };

    bool Variable;
    std::string HWDevice;
    std::map<std::string, std::string> LAVFOptions;
    std::vector<FrameInfo> Frames;
};

bool WriteVideoTrackIndex(const std::string &CachePath, int Track, const VideoTrackIndex &Index);
bool ReadVideoTrackIndex(const std::string &CachePath, int Track, VideoTrackIndex &Index);

#endif