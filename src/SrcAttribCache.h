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

#ifndef SRC_ATTRIB_CACHE_H
#define SRC_ATTRIB_CACHE_H

#include <string>
#include <map>

struct SourceAttributes {
    struct TrackAttributes {
        int64_t Samples;
        bool Variable;
        std::string HWDevice;
    };
    std::map<int, TrackAttributes> Tracks;
};

bool GetSourceAttributes(const std::string &CachePath, const std::string &Filename, SourceAttributes &Attrs, std::map<std::string, std::string> &LAVFOpts);
bool SetSourceAttributes(const std::string &CachePath, const std::string &Filename, const SourceAttributes &Attrs, std::map<std::string, std::string> &LAVFOpts);

#endif