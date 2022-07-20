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

// FIXME, indexing progress update
// FIXME, expose cache size setting

#include "SrcAttribCache.h"
#include "version.h"

#include <memory>
#include <sys/types.h>
#include <sys/stat.h>
#include <jansson.h>

extern "C" {
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#ifdef _WIN32
#include <shlobj_core.h>

#define STAT_STRUCT_TYPE _stat64

static std::wstring Utf16FromUtf8(const std::string &Str) {
    int RequiredSize = MultiByteToWideChar(CP_UTF8, 0, Str.c_str(), -1, nullptr, 0);
    std::wstring Buffer;
    Buffer.resize(RequiredSize - 1);
    MultiByteToWideChar(CP_UTF8, 0, Str.c_str(), static_cast<int>(Str.size()), &Buffer[0], RequiredSize);
    return Buffer;
}
#else

#define STAT_STRUCT_TYPE stat

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
    bsversion: {
        bestsource: int,
        avutil: int,
        avformat: int,
        avcodec: int
    },
    files: {
        "<file path>": { 
            size: int,
            lavfopts: {
                "enable_drefs": "1",
                "use_absolute_paths": "0"
            },
            tracks: {
                "0": {
                        hwdevice: string,
                        variable: bool,
                        samples: int,
                },
                "1": {
                        hwdevice: string,
                        variable: bool,
                        samples: int,
                },
                ...
            }
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
        CachePath += L"\\bsindex.json";
    } else {
        CachePath = Utf16FromUtf8(Path);
    }
    return file_ptr_t(_wfopen(CachePath.c_str(), Write ? L"wb" : L"rb"));
#else
    std::string CachePath = Path.empty() ? "~/bsindex.json" : Path;
    return file_ptr_t(fopen(CachePath.c_str(), Write ? "wb" : "rb"));
#endif
}

static bool StatWrapper(const std::string &Filename, struct STAT_STRUCT_TYPE &Info) {
#ifdef _WIN32
    return !_wstat64(Utf16FromUtf8(Filename).c_str(), &Info);
#else
    return !stat(Filename.c_str(), &Info);
#endif
}

static bool CheckBSVersion(const json_ptr_t &Root) {
    json_t *VersionData = json_object_get(Root.get(), "bsversion");
    if (!VersionData)
        return false;
    if (json_integer_value(json_object_get(VersionData, "bestsource")) != (((BEST_SOURCE_VERSION_MAJOR) << 16) | BEST_SOURCE_VERSION_MINOR))
        return false;
    if (json_integer_value(json_object_get(VersionData, "avutil")) != avutil_version())
        return false;
    if (json_integer_value(json_object_get(VersionData, "avformat")) != avformat_version())
        return false;
    if (json_integer_value(json_object_get(VersionData, "avcodec")) != avcodec_version())
        return false;
    return true;
}

bool GetSourceAttributes(const std::string &CachePath, const std::string &Filename, SourceAttributes &Attrs, std::map<std::string, std::string> &LAVFOpts) {
    file_ptr_t File = OpenCacheFile(CachePath, false);
    if (!File)
        return false;

    json_ptr_t Data(json_loadf(File.get(), 0, nullptr));
    if (!Data)
        return false;

    if (!CheckBSVersion(Data))
        return false;

    json_t *FilesData = json_object_get(Data.get(), "files");
    if (!FilesData)
        return false;

    json_t *FileData = json_object_get(FilesData, Filename.c_str());
    if (!FileData)
        return false;

    json_int_t FileSize = json_integer_value(json_object_get(FileData, "size"));

    struct STAT_STRUCT_TYPE Info = {};
    if (!StatWrapper(Filename, Info))
        return false;

    if (Info.st_size != FileSize)
        return false;

    std::map<std::string, std::string> Opts;
    json_t *LAVFData = json_object_get(FileData, "lavfopts");
    const char *Key;
    json_t *Value;
    json_object_foreach(LAVFData, Key, Value) {
        Opts[Key] = json_string_value(Value);
    }

    if (Opts != LAVFOpts)
        return false;

    json_t *TracksData = json_object_get(FileData, "tracks");

    json_object_foreach(TracksData, Key, Value) {
        bool VariableFormat = json_boolean_value(json_object_get(Value, "variable"));
        int64_t Samples = json_integer_value(json_object_get(Value, "samples"));
        const char *HWDevice = json_string_value(json_object_get(Value, "hwdevice"));
        if (Samples > 0)
            Attrs.Tracks[atoi(Key)] = { Samples, VariableFormat, HWDevice ? HWDevice : "" };
    }

    return true;
}

bool SetSourceAttributes(const std::string &CachePath, const std::string &Filename, const SourceAttributes &Attrs, std::map<std::string, std::string> &LAVFOpts) {
    struct STAT_STRUCT_TYPE Info = {};
    if (!StatWrapper(Filename, Info))
        return false;

    file_ptr_t File = OpenCacheFile(CachePath, false);
    json_ptr_t Data(File ? json_loadf(File.get(), 0, nullptr) : json_object());

    if (!Data || !CheckBSVersion(Data))
        Data.reset(json_object());

    json_t *VersionData = json_object_get(Data.get(), "bsversion");
    if (!VersionData) {
        VersionData = json_object();
        json_object_set_new(Data.get(), "bsversion", VersionData);
    }

    json_object_set_new(VersionData, "bestsource", json_integer(((BEST_SOURCE_VERSION_MAJOR) << 16) | BEST_SOURCE_VERSION_MINOR));
    json_object_set_new(VersionData, "avutil", json_integer(avutil_version()));
    json_object_set_new(VersionData, "avformat", json_integer(avformat_version()));
    json_object_set_new(VersionData, "avcodec", json_integer(avcodec_version()));

    json_t *FilesData = json_object_get(Data.get(), "files");
    if (!FilesData) {
        FilesData = json_object();
        json_object_set_new(Data.get(), "files", FilesData);
    }

    json_t *FileData = json_object_get(FilesData, Filename.c_str());
    if (!FileData) {
        FileData = json_object();
        json_object_set_new(FilesData, Filename.c_str(), FileData);
    }

    json_object_set_new(FileData, "size", json_integer(Info.st_size));

    std::map<std::string, std::string> Opts;
    json_t *LAVFData = json_object_get(FileData, "lavfopts");
    const char *Key;
    json_t *Value;
    json_object_foreach(LAVFData, Key, Value) {
        Opts[Key] = json_string_value(Value);
    }

    bool ReplaceTracks = (Opts != LAVFOpts);

    LAVFData = json_object();
    json_object_set_new(FileData, "lavfopts", LAVFData);
    for (const auto &Iter : LAVFOpts)
        json_object_set_new(LAVFData, Iter.first.c_str(), json_string(Iter.second.c_str()));

    json_t *TracksData = (ReplaceTracks ? nullptr : json_object_get(FileData, "tracks"));
    if (!TracksData) {
        TracksData = json_object();
        json_object_set_new(FileData, "tracks", TracksData);
    }

    for (auto &Iter : Attrs.Tracks) {
        json_t *TrackData = json_object();
        json_object_set_new(TrackData, "samples", json_integer(Iter.second.Samples));
        json_object_set_new(TrackData, "variable", json_boolean(Iter.second.Variable));
        json_object_set_new(TrackData, "hwdevice", json_string(Iter.second.HWDevice.c_str()));
        json_object_set_new(TracksData, std::to_string(Iter.first).c_str(), TrackData);
    }

    File = OpenCacheFile(CachePath, true);
    if (!File)
        return false;

    if (json_dumpf(Data.get(), File.get(), JSON_INDENT(2) | JSON_SORT_KEYS))
        return false;

    return true;
}
