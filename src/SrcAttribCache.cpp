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

// FIXME, warn on cache write failure

#include "SrcAttribCache.h"
#include "version.h"

#include <memory>

extern "C" {
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#ifdef _WIN32
#include <windows.h>

static std::wstring Utf16FromUtf8(const std::string &Str) {
    int RequiredSize = MultiByteToWideChar(CP_UTF8, 0, Str.c_str(), -1, nullptr, 0);
    std::wstring Buffer;
    Buffer.resize(RequiredSize - 1);
    MultiByteToWideChar(CP_UTF8, 0, Str.c_str(), static_cast<int>(Str.size()), &Buffer[0], RequiredSize);
    return Buffer;
}
#endif

namespace std {
    template<>
    struct default_delete<FILE> {
        void operator()(FILE *Ptr) {
            fclose(Ptr);
        }
    };
}

typedef std::unique_ptr<FILE> file_ptr_t;

static file_ptr_t OpenCacheFile(const std::string &CachePath, int Track, bool Write) {
    std::string FullPath = CachePath + "." + std::to_string(Track) + ".bsindex";
#ifdef _WIN32
    file_ptr_t F(_wfopen(Utf16FromUtf8(FullPath).c_str(), Write ? L"wb" : L"rb"));
#else
    file_ptr_t F(fopen(FullPath.c_str(), Write ? "wb" : "rb"));
#endif
    return F;
}

static void WriteInt(file_ptr_t &F, int Value) {
    fwrite(&Value, 1, sizeof(Value), F.get());
}

static void WriteInt64(file_ptr_t &F, int64_t Value) {
    fwrite(&Value, 1, sizeof(Value), F.get());
}

static void WriteString(file_ptr_t &F, const std::string &Value) {
    WriteInt(F, Value.size());
    fwrite(Value.c_str(), 1, Value.size(), F.get());
}

static void WriteBSHeader(file_ptr_t &F) {
    fwrite("BS2I", 1, 4, F.get());
    WriteInt(F, (BEST_SOURCE_VERSION_MAJOR << 16) | BEST_SOURCE_VERSION_MINOR);
    WriteInt(F, avutil_version());
    WriteInt(F, avformat_version());
    WriteInt(F, avcodec_version());
}

bool WriteVideoTrackIndex(const std::string &CachePath, int Track, const VideoTrackIndex &Index) {
    file_ptr_t F = OpenCacheFile(CachePath, Track, true);
    if (!F)
        return false;
    WriteBSHeader(F);
    // FIXME, file size, hash or something else here to make sure the index is for the right file?
    WriteInt(F, Track);
    WriteInt(F, Index.Variable);
    WriteString(F, Index.HWDevice);

    WriteInt(F, Index.LAVFOptions.size());
    for (const auto &Iter : Index.LAVFOptions) {
        WriteString(F, Iter.first);
        WriteString(F, Iter.second);
    }

    WriteInt64(F, Index.Frames.size());
    WriteInt64(F, Index.LastFrameDuration);

    for (const auto &Iter : Index.Frames) {
        fwrite(Iter.Hash, 1, sizeof(Iter.Hash), F.get());
        WriteInt64(F, Iter.PTS);
        WriteInt(F, Iter.RepeatPict);
        WriteInt(F, Iter.KeyFrame | (Iter.TFF << 1));
    }

    return true;
}

static int ReadInt(file_ptr_t &F) {
    int Value;
    fread(&Value, 1, sizeof(Value), F.get());
    return Value;
}

static int64_t ReadInt64(file_ptr_t &F) {
    int64_t Value;
    fread(&Value, 1, sizeof(Value), F.get());
    return Value;
}

static std::string ReadString(file_ptr_t &F) {
    int Size = ReadInt(F);
    std::string S;
    S.resize(Size);
    fread(&S[0], 1, Size, F.get());
    return S;
}

static bool ReadCompareInt(file_ptr_t &F, int Value) {
    int Value2 = ReadInt(F);
    return (Value == Value2);
}

static bool ReadBSHeader(file_ptr_t &F) {
    char Magic[4] = {};
    fread(Magic, 1, sizeof(Magic), F.get());
    return !memcmp("BS2I", Magic, sizeof(Magic)) &&
    ReadCompareInt(F, (BEST_SOURCE_VERSION_MAJOR << 16) | BEST_SOURCE_VERSION_MINOR) &&
    ReadCompareInt(F, avutil_version()) &&
    ReadCompareInt(F, avformat_version()) &&
    ReadCompareInt(F, avcodec_version());
}

bool ReadVideoTrackIndex(const std::string &CachePath, int Track, VideoTrackIndex &Index) {
    file_ptr_t F = OpenCacheFile(CachePath, Track, false);
    if (!F)
        return false;
    if (!ReadBSHeader(F))
        return false;
    // FIXME, file size, hash or something else here to make sure the index is for the right file?
    if (!ReadCompareInt(F, Track))
        return false;
    Index.Variable = !!ReadInt(F);
    Index.HWDevice = ReadString(F);
    int LAVFOptCount = ReadInt(F);
    for (int i = 0; i < LAVFOptCount; i++) {
        std::string Key = ReadString(F);
        Index.LAVFOptions[Key] = ReadString(F);
    }
    int64_t NumFrames = ReadInt64(F);
    Index.LastFrameDuration = ReadInt64(F);
    Index.Frames.reserve(NumFrames);

    for (int i = 0; i < NumFrames; i++) {
        VideoTrackIndex::FrameInfo FI = {};
        fread(FI.Hash, 1, sizeof(FI.Hash), F.get());
        FI.PTS = ReadInt64(F);
        FI.RepeatPict = ReadInt(F);
        int Flags = ReadInt(F);
        FI.KeyFrame = !!(Flags & 1);
        FI.TFF = !!(Flags & 2);
        Index.Frames.push_back(FI);
    }

    return true;
}
