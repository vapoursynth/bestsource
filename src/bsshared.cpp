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

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/log.h>
#include <libavutil/rational.h>
}

BSRational::BSRational(const AVRational &r) {
    Num = r.num;
    Den = r.den;
}

double BSRational::ToDouble() const {
    return Num / (double)Den;
}


int SetFFmpegLogLevel(int Level) {
    av_log_set_level(Level);
    return GetFFmpegLogLevel();
}

int GetFFmpegLogLevel() {
    return av_log_get_level();
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

file_ptr_t OpenFile(const std::string &Filename, bool Write) {
#ifdef _WIN32
    file_ptr_t F(_wfopen(Utf16FromUtf8(Filename).c_str(), Write ? L"wb" : L"rb"));
#else
    file_ptr_t F(fopen(Filename.c_str(), Write ? "wb" : "rb"));
#endif
    return F;
}

file_ptr_t OpenCacheFile(const std::string &CachePath, int Track, bool Write) {
    return OpenFile(CachePath + "." + std::to_string(Track) + ".bsindex", Write);
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
    int64_t Value;
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