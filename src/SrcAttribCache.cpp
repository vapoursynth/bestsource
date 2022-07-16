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

#include "SrcAttribCache.h"

#include <memory>
#include <sys/stat.h>
#include <jansson.h>

#ifdef _WIN32
#include <shlobj_core.h>

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
    struct default_delete<json_t> {
        void operator()(json_t *Ptr) {
            json_decref(Ptr);
        }
    };

    template<>
    struct default_delete<FILE> {
        void operator()(FILE *Ptr) {
            fclose(Ptr);
        }
    };
}

typedef std::unique_ptr<json_t> json_ptr_t;
typedef std::unique_ptr<FILE> file_ptr_t;

/*
{
        "<file path>": { 
            size: int,
            lavfopts: { "enable_drefs": "1", "use_absolute_paths": "0" },
            variableformat: bool,
            tracks: {
                "0": frames/samples, "1": frames/samples, ...
            }
       }
}
*/

static file_ptr_t OpenCacheFile(const std::string &Path, bool Write) {
#ifdef _WIN32
    std::wstring CachePath;
    if (Path.empty()) {
        PWSTR App = nullptr;
        if (SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &App) != S_OK)
            return nullptr;
        CachePath = App;
        CoTaskMemFree(App);
    } else {
        CachePath = Utf16FromUtf8(Path);
    }
    CachePath += L"\\bscache.json";
    return file_ptr_t(_wfopen(CachePath.c_str(), Write ? L"wb" : L"rb"));
#else
    std::string CachePath = Path.empty() ? "~/bscache.json" : (Path + "/bscache.json");
    return file_ptr_t(fopen(CachePath.c_str(), Write ? "wb" : "rb"));
#endif
}

static bool StatWrapper(const std::string &Filename, struct _stat64 &Info) {
#ifdef _WIN32
    return !_wstat64(Utf16FromUtf8(Filename).c_str(), &Info);
#else
    return !_stat64(Filename.c_str(), &Info);
#endif
}

bool GetSourceAttributes(const std::string &CachePath, const std::string &Filename, SourceAttributes &Attrs, std::map<std::string, std::string> &LAVFOpts, bool Variable) {
    file_ptr_t File = OpenCacheFile(CachePath, false);
    if (!File)
        return false;

    json_ptr_t Data(json_loadf(File.get(), 0, nullptr));
    if (!Data)
        return false;

    json_t *FileData = json_object_get(Data.get(), Filename.c_str());
    if (!FileData)
        return false;

    json_int_t FileSize = json_integer_value(json_object_get(FileData, "size"));

    struct _stat64 Info = {};
    if (!StatWrapper(Filename, Info))
        return false;

    if (Info.st_size != FileSize)
        return false;

    bool VariableFormat = json_boolean_value(json_object_get(FileData, "variable"));
    std::map<std::string, std::string> Opts;

    json_t *LAVFData = json_object_get(FileData, "lavfopts");
    const char *Key;
    json_t *Value;
    json_object_foreach(LAVFData, Key, Value) {
        Opts[Key] = json_string_value(Value);
    }

    if (VariableFormat != Variable || Opts != LAVFOpts)
        return false;

    json_t *TrackData = json_object_get(FileData, "tracks");

    json_object_foreach(TrackData, Key, Value) {
        Attrs.Tracks[atoi(Key)] = json_integer_value(Value);
    }

    return true;
}

bool SetSourceAttributes(const std::string &CachePath, const std::string &Filename, int Track, int64_t Samples, std::map<std::string, std::string> &LAVFOpts, bool Variable) {
    struct _stat64 Info = {};
    if (!StatWrapper(Filename, Info))
        return false;

    file_ptr_t File = OpenCacheFile(CachePath, false);
    json_ptr_t Data(File ? json_loadf(File.get(), 0, nullptr) : json_object());

    if (!Data)
        return false;

    json_t *FileData = json_object_get(Data.get(), Filename.c_str());
    if (!FileData) {
        FileData = json_object();
        json_object_set_new(Data.get(), Filename.c_str(), FileData);
    }

    json_object_set_new(FileData, "size", json_integer(Info.st_size));

    bool VariableFormat = json_boolean_value(json_object_get(FileData, "variable"));
    std::map<std::string, std::string> Opts;

    json_t *LAVFData = json_object_get(FileData, "lavfopts");
    const char *Key;
    json_t *Value;
    json_object_foreach(LAVFData, Key, Value) {
        Opts[Key] = json_string_value(Value);
    }

    bool ReplaceTracks = (VariableFormat != Variable || Opts != LAVFOpts);

    json_object_set_new(FileData, "variable", json_boolean(Variable));
    LAVFData = json_object();
    json_object_set_new(FileData, "lavfopts", LAVFData);

    for (const auto &Iter : LAVFOpts)
        json_object_set_new(LAVFData, Iter.first.c_str(), json_string(Iter.second.c_str()));

    json_t *TrackData = json_object_get(Data.get(), "tracks");
    if (!TrackData) {
        TrackData = json_object();
        json_object_set_new(FileData, "tracks", TrackData);
    }

    json_object_set_new(TrackData, std::to_string(Track).c_str(), json_integer(Samples));

    File = OpenCacheFile(CachePath, true);
    if (!File)
        return false;

    if (json_dumpf(Data.get(), File.get(), JSON_INDENT(2) | JSON_SORT_KEYS))
        return false;

    return true;
}
