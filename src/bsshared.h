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

#ifndef BSSHARED_H
#define BSSHARED_H

#include <memory>
#include <cstdio>
#include <string>
#include <stdexcept>
#include <functional>
#include <filesystem>

constexpr size_t HashSize = 8;

class BestSourceException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

class BestSourceHWDecoderException : public BestSourceException {
    using BestSourceException::BestSourceException;
};

namespace std {
    template<>
    struct default_delete<FILE> {
        void operator()(FILE *Ptr) {
            fclose(Ptr);
        }
    };
}

typedef std::unique_ptr<FILE> file_ptr_t;

typedef std::function<bool(int Track, int64_t Current, int64_t Total)> ProgressFunction;

enum BestCacheMode {
    bcmDisable = 0,
    bcmAutoSubTree,
    bcmAlwaysWriteSubTree,
    bcmAutoAbsolutePath,
    bcmAlwaysAbsolutePath,
};

struct AVRational;

struct BSRational {
    int Num;
    int Den;
    BSRational() = default;
    BSRational(const AVRational &r);
    double ToDouble() const;
};

std::filesystem::path CreateProbablyUTF8Path(const char *Filename);

int SetFFmpegLogLevel(int Level);

void SetBSDebugOutput(bool DebugOutput);
void BSDebugPrint(const std::string_view Message, int64_t RequestedN = -1, int64_t CurrentN = -1);

bool ShouldWriteIndex(int CacheMode, size_t Frames);
bool IsAbsolutePathCacheMode(int CacheMode);

file_ptr_t OpenNormalFile(const std::filesystem::path &Filename, bool Write);
file_ptr_t OpenCacheFile(bool AbsolutePath, const std::filesystem::path &CacheBasePath, const std::filesystem::path &Source, int Track, bool Write);
void WriteByte(file_ptr_t &F, uint8_t Value);
void WriteInt(file_ptr_t &F, int Value);
void WriteInt64(file_ptr_t &F, int64_t Value);
void WriteDouble(file_ptr_t &F, double Value);
void WriteString(file_ptr_t &F, const std::string &Value);
void WriteBSHeader(file_ptr_t &F, bool Video);
uint8_t ReadByte(file_ptr_t &F);
int ReadInt(file_ptr_t &F);
int64_t ReadInt64(file_ptr_t &F);
double ReadDouble(file_ptr_t &F);
std::string ReadString(file_ptr_t &F);
bool ReadCompareInt(file_ptr_t &F, int Value);
bool ReadCompareInt64(file_ptr_t &F, int64_t Value);
bool ReadCompareDouble(file_ptr_t &F, double Value);
bool ReadCompareString(file_ptr_t &F, const std::string &Value);
bool ReadBSHeader(file_ptr_t &F, bool Video);

#endif