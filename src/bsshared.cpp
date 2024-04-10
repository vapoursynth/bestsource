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


#include "bsshared.h"
#include "version.h"
#include <string>
#include <atomic>
#include <cassert>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/log.h>
#include <libavutil/rational.h>
}

#ifdef _WIN32
#include <ShlObj.h>
#endif

BSRational::BSRational(const AVRational &r) {
    Num = r.num;
    Den = r.den;
}

double BSRational::ToDouble() const {
    return Num / (double)Den;
}

std::filesystem::path CreateProbablyUTF8Path(const char *Filename) {
    assert(Filename);
    try {
        return std::filesystem::u8path(Filename);
    } catch (...) { // exceptions are "implementation defined" so this is what you get
        return std::filesystem::path(Filename);
    }
}

int SetFFmpegLogLevel(int Level) {
    av_log_set_level(Level);
    return av_log_get_level();
}

static std::atomic_bool PrintDebugInfo(false);

void SetBSDebugOutput(bool DebugOutput) {
    PrintDebugInfo = DebugOutput;
}

void BSDebugPrint(const std::string_view Message, int64_t RequestedN, int64_t CurrentN) {
    if (PrintDebugInfo) {
        if (RequestedN == -1 && CurrentN == -1)
            fprintf(stderr, "%s\n", Message.data());
        else
            fprintf(stderr, "Req/Current: %" PRId64 "/%" PRId64 ", %s\n", RequestedN, CurrentN, Message.data());
    }
}

static std::filesystem::path GetDefaultCachePath() {
#ifdef _WIN32
    std::vector<wchar_t> appDataBuffer(MAX_PATH + 1);
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, appDataBuffer.data()) != S_OK)
        SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_DEFAULT, appDataBuffer.data());
    std::filesystem::path IndexPath = appDataBuffer.data();
#else
    const char *home = getenv("HOME");
    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    std::filesystem::path IndexPath;
    if (xdg_config_home) {
        IndexPath = xdg_config_home;
    } else if (home) {
        IndexPath = home;
    }
#endif

    assert(!IndexPath.empty());

    IndexPath /= "bsindex";
    return IndexPath;
}

static std::filesystem::path MangleCachePath(const std::filesystem::path &CacheBasePath, const std::filesystem::path &Source) {
    std::filesystem::path CachePath = std::filesystem::absolute(CacheBasePath);
    CachePath /= Source.relative_path();
    return CachePath.make_preferred();
}

file_ptr_t OpenNormalFile(const std::filesystem::path &Filename, bool Write) {
#ifdef _WIN32
    file_ptr_t F(_wfopen(Filename.c_str(), Write ? L"wb" : L"rb"));
#else
    file_ptr_t F(fopen(Filename.c_str(), Write ? "wb" : "rb"));
#endif
    return F;
}

file_ptr_t OpenCacheFile(const std::filesystem::path &CacheBasePath, const std::filesystem::path &Source, int Track, bool Write) {
    std::filesystem::path CacheFile = MangleCachePath(CacheBasePath.empty() ? GetDefaultCachePath() : CacheBasePath, Source);
    CacheFile += "." + std::to_string(Track) + ".bsindex";
    std::error_code ec;
    std::filesystem::create_directories(CacheFile.parent_path(), ec);
    return OpenNormalFile(CacheFile, Write);
}

void WriteByte(file_ptr_t &F, uint8_t Value) {
    fwrite(&Value, 1, sizeof(Value), F.get());
}

void WriteInt(file_ptr_t &F, int Value) {
    fwrite(&Value, 1, sizeof(Value), F.get());
}

void WriteInt64(file_ptr_t &F, int64_t Value) {
    fwrite(&Value, 1, sizeof(Value), F.get());
}

void WriteDouble(file_ptr_t &F, double Value) {
    fwrite(&Value, 1, sizeof(Value), F.get());
}

void WriteString(file_ptr_t &F, const std::string &Value) {
    WriteInt(F, static_cast<int>(Value.size()));
    fwrite(Value.c_str(), 1, Value.size(), F.get());
}

void WriteBSHeader(file_ptr_t &F, bool Video) {
    fwrite(Video ? "BS2V" : "BS2A", 1, 4, F.get());
    WriteInt(F, (BEST_SOURCE_VERSION_MAJOR << 16) | BEST_SOURCE_VERSION_MINOR);
    WriteInt(F, avutil_version());
    WriteInt(F, avformat_version());
    WriteInt(F, avcodec_version());
}

uint8_t ReadByte(file_ptr_t &F) {
    uint8_t Value;
    if (fread(&Value, 1, sizeof(Value), F.get()) == sizeof(Value))
        return Value;
    else
        return -1;
}

int ReadInt(file_ptr_t &F) {
    int Value;
    if (fread(&Value, 1, sizeof(Value), F.get()) == sizeof(Value))
        return Value;
    else
        return -1;
}

int64_t ReadInt64(file_ptr_t &F) {
    int64_t Value;
    if (fread(&Value, 1, sizeof(Value), F.get()) == sizeof(Value))
        return Value;
    else
        return -1;
}

double ReadDouble(file_ptr_t &F) {
    double Value;
    if (fread(&Value, 1, sizeof(Value), F.get()) == sizeof(Value))
        return Value;
    else
        return -1;
}

std::string ReadString(file_ptr_t &F) {
    int Size = ReadInt(F);
    std::string S;
    S.resize(Size);
    if (static_cast<int>(fread(&S[0], 1, Size, F.get())) == Size)
        return S;
    else
        return "";
}

bool ReadCompareInt(file_ptr_t &F, int Value) {
    int Value2 = ReadInt(F);
    return (Value == Value2);
}

bool ReadCompareInt64(file_ptr_t &F, int64_t Value) {
    int64_t Value2 = ReadInt64(F);
    return (Value == Value2);
}

bool ReadCompareDouble(file_ptr_t &F, double Value) {
    double Value2 = ReadDouble(F);
    return (Value == Value2);
}

bool ReadCompareString(file_ptr_t &F, const std::string &Value) {
    std::string Value2 = ReadString(F);
    return (Value == Value2);
}

bool ReadBSHeader(file_ptr_t &F, bool Video) {
    char Magic[4] = {};
    if (fread(Magic, 1, sizeof(Magic), F.get()) != sizeof(Magic))
        return false;
    return !memcmp(Video ? "BS2V" : "BS2A", Magic, sizeof(Magic)) &&
        ReadCompareInt(F, (BEST_SOURCE_VERSION_MAJOR << 16) | BEST_SOURCE_VERSION_MINOR) &&
        ReadCompareInt(F, avutil_version()) &&
        ReadCompareInt(F, avformat_version()) &&
        ReadCompareInt(F, avcodec_version());
}