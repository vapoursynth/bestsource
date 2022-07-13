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

static std::wstring Utf16FromUtf8(const std::string &str) {
    int required_size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    std::wstring wbuffer;
    wbuffer.resize(required_size - 1);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &wbuffer[0], required_size);
    return wbuffer;
}
#endif

namespace std {
    template<>
    struct default_delete<json_t> {
        void operator()(json_t *ptr) {
            json_decref(ptr);
        }
    };

    template<>
    struct default_delete<FILE> {
        void operator()(FILE *ptr) {
            fclose(ptr);
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

static file_ptr_t OpenCacheFile(bool write) {
#ifdef _WIN32
    PWSTR app = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &app) != S_OK)
        return nullptr;
    std::wstring cachePath = app;
    CoTaskMemFree(app);
    cachePath += L"\\bscache.json";
    return file_ptr_t(_wfopen(cachePath.c_str(), write ? L"wb" : L"rb"));
#else
    return file_ptr_t(fopen("~/bscache.json", write ? "wb" : "rb"));
#endif
}

static bool StatWrapper(const std::string &filename, struct _stat64 &info) {
#ifdef _WIN32
    return !_wstat64(Utf16FromUtf8(filename).c_str(), &info);
#else
    return !_stat64(filename.c_str(), &info);
#endif
}

bool GetSourceAttributes(const std::string &filename, SourceAttributes &attrs, std::map<std::string, std::string> &LAVFOpts, bool variable) {
    file_ptr_t f = OpenCacheFile(false);
    if (!f)
        return false;

    json_ptr_t data(json_loadf(f.get(), 0, nullptr));
    if (!data)
        return false;

    json_t *fdata = json_object_get(data.get(), filename.c_str());
    if (!fdata)
        return false;

    json_int_t filesize = json_integer_value(json_object_get(fdata, "size"));

    struct _stat64 info = {};
    if (!StatWrapper(filename, info))
        return false;

    if (info.st_size != filesize)
        return false;

    bool VariableFormat = json_boolean_value(json_object_get(fdata, "variable"));
    std::map<std::string, std::string> opts;

    json_t *lobj = json_object_get(fdata, "lavfopts");
    const char *key;
    json_t *value;
    json_object_foreach(lobj, key, value) {
        opts[key] = json_string_value(value);
    }

    if (VariableFormat != variable || opts != LAVFOpts)
        return false;

    json_t *tdata = json_object_get(fdata, "tracks");

    json_object_foreach(tdata, key, value) {
        attrs.Tracks[atoi(key)] = json_integer_value(value);
    }

    return true;
}

bool SetSourceAttributes(const std::string &filename, int track, int64_t samples, std::map<std::string, std::string> &LAVFOpts, bool variable) {
    struct _stat64 info = {};
    if (!StatWrapper(filename, info))
        return false;

    file_ptr_t f = OpenCacheFile(false);
    json_ptr_t data(f ? json_loadf(f.get(), 0, nullptr) : json_object());

    if (!data)
        return false;

    json_t *fobj = json_object_get(data.get(), filename.c_str());
    if (!fobj) {
        fobj = json_object();
        json_object_set_new(data.get(), filename.c_str(), fobj);
    }

    json_object_set_new(fobj, "size", json_integer(info.st_size));

    bool VariableFormat = json_boolean_value(json_object_get(fobj, "variable"));
    std::map<std::string, std::string> opts;

    json_t *lobj = json_object_get(fobj, "lavfopts");
    const char *key;
    json_t *value;
    json_object_foreach(lobj, key, value) {
        opts[key] = json_string_value(value);
    }

    bool ReplaceTracks = (VariableFormat != variable || opts != LAVFOpts);

    json_object_set_new(fobj, "variable", json_boolean(variable));
    lobj = json_object();
    json_object_set_new(fobj, "lavfopts", lobj);

    for (const auto &iter : LAVFOpts)
        json_object_set_new(lobj, iter.first.c_str(), json_string(iter.second.c_str()));

    json_t *tobj = json_object_get(data.get(), "tracks");
    if (!tobj) {
        tobj = json_object();
        json_object_set_new(fobj, "tracks", tobj);
    }

    json_object_set_new(tobj, std::to_string(track).c_str(), json_integer(samples));

    f = OpenCacheFile(true);
    if (!f)
        return false;

    if (json_dumpf(data.get(), f.get(), JSON_INDENT(2) | JSON_SORT_KEYS))
        return false;

    return true;
}
