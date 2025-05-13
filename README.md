# BestSource

[![Windows](https://github.com/vapoursynth/bestsource/actions/workflows/windows.yml/badge.svg)](https://github.com/vapoursynth/bestsource/actions/workflows/windows.yml)
[![Linux](https://github.com/vapoursynth/bestsource/actions/workflows/linux.yml/badge.svg)](https://github.com/vapoursynth/bestsource/actions/workflows/linux.yml)
[![macOS](https://github.com/vapoursynth/bestsource/actions/workflows/macos.yml/badge.svg)](https://github.com/vapoursynth/bestsource/actions/workflows/macos.yml)
[![codespell](https://github.com/vapoursynth/bestsource/actions/workflows/codespell.yml/badge.svg)](https://github.com/vapoursynth/bestsource/actions/workflows/codespell.yml)

**BestSource** (abbreviated as **BS**) is a cross-platform wrapper library around [FFmpeg](http://ffmpeg.org)
that ensures always sample and frame accurate access to audio and video with good seeking performance for everything except some lossy audio formats.

It can be used as either a C++ library directly or through the combined VapourSynth and Avisynth+ plugin that's included.

## Dependencies

- FFmpeg 7.1.x. Later releases may or may not work but FFmpeg API breakages are quite common and don't always generate compilation errors. Only `libavcodec`, `libavformat`, `libavutil` libraries are required.
- xxHash
- libp2p (already included as submodule)

### Windows Compilation

On Windows the easiest way to compile the the dependencies is to use [vcpkg](https://vcpkg.io) to install `ffmpeg[avcodec,avdevice,avfilter,avformat,swresample,swscale,zlib,bzip2,core,dav1d,gpl,version3,lzma,nvcodec,qsv,openssl,xml2]:x64-windows-static` and `xxhash:x64-windows-static`. Do however note that this is without Little CMS2 support.
Use the latest version of Visual Studio. It should automatically find all the required libraries if you used vcpkg.

### Linux and MacOS Compilation

Requires `pkg-config`, `meson` and `ninja-build`.

```
git clone https://github.com/vapoursynth/bestsource.git --depth 1 --recurse-submodules --shallow-submodules --remote-submodules
cd bestsource
meson setup build
ninja -C build
ninja -C build install
```

### Known issues and limitations

- Seeking performance in mpeg/ts/vob files can be quite poor due to the FFmpeg demuxer
- VC1 codec is unseekable due to FFmpeg not having bitexact output after seeking
- The unholy combination of VFR H264 in AVI has poor seeking performance
- Needs FFmpeg compiled with Little CMS2 or the color information reported for most image files will be less complete
- Mod files can't be decoded correctly using libmodplug due to the library not having repeatable bitexact output
- Gray+alpha format isn't supported in Avisynth+ and as a result only the Y component is returned
- Files with dimensions that aren't a multiple of the subsampling value will be cropped

## VapourSynth usage

`bs.AudioSource(string source[, int track = -1, int adjustdelay = -1, int threads = 0, bint enable_drefs = False, bint use_absolute_path = False, float drc_scale = 0, int cachemode = 1, string cachepath, int cachesize = 100, bint showprogress = True, maxdecoders = 0])`

`bs.VideoSource(string source[, int track = -1, int variableformat = -1, int fpsnum = -1, int fpsden = 1, bint rff = False, int threads = 0, int seekpreroll = 20, bint enable_drefs = False, bint use_absolute_path = False, int cachemode = 1, string cachepath , int cachesize = 100, string hwdevice, int extrahwframes = 9, string timecodes, int start_number, int viewid = 0, bint showprogress = True, maxdecoders = 0, bool hwfallback = True])`

`bs.TrackInfo(string source[, bint enable_drefs = False, bint use_absolute_path = False])`

`bs.Metadata(string source[, int track, bint enable_drefs = False, bint use_absolute_path = False])`

`bs.SetDebugOutput(bint enable = False)`

`bs.SetFFmpegLogLevel(int level = <quiet log level>)`

The *TrackInfo* function only returns the most basic information about a track which is the type, codec and disposition. Its main use is to be able to implement custom track selection logic for the source functions.

The *Metadata* function returns all the file or track metadata as key-value pairs depending on whether or not *track* is specified.

## Avisynth+ usage

`BSAudioSource(string source[, int track = -1, int adjustdelay = -1, int threads = 0, bool enable_drefs = False, bool use_absolute_path = False, float drc_scale = 0, int cachemode = 1, string cachepath, int cachesize = 100, int maxdecoders = 0])`

`BSVideoSource(string source[, int track = -1, int fpsnum = -1, int fpsden = 1, bool rff = False, int threads = 0, int seekpreroll = 20, bool enable_drefs = False, bool use_absolute_path = False, int cachemode = 1, string cachepath, int cachesize = 100, string hwdevice, int extrahwframes = 9, string timecodes, int start_number, int variableformat = 0, int viewid = 0, int maxdecoders = 0, bool hwfallback = True])`

`BSSource(string source[, int atrack = -1, int vtrack = -1, int fpsnum = -1, int fpsden = 1, bool rff = False, int threads = 0, int seekpreroll = 20, bool enable_drefs = False, bool use_absolute_path = False, int cachemode = 1, string cachepath, int acachesize = 100, int vcachesize = 100, string hwdevice, int extrahwframes = 9, string timecodes, int start_number, int variableformat = 0, int adjustdelay = -1, float drc_scale = 0, int viewid = 0, int maxdecoders = 0, bool hwfallback = True])`

`BSSetDebugOutput(bool enable = False)`

`BSSetFFmpegLogLevel(int level = <quiet log level>)`

Note that the *BSSource* function by default will silently ignore errors when opening audio and in that case only return the video track. However if *atrack* is explicitly set failure to open the audio track will return an error.

## Argument explanation

*source*: The source filename. Note that image sequences also can be opened by using %d or %03d for zero padded numbers. Sequences may start at any number between 0 and 4 unless otherwise specified with *start_number*. It's also possible to pass urls and other ffmpeg protocols like concat.

*track*: Either a positive number starting from 0 specifying the absolute track number or a negative number to select the nth audio or video track. Throws an error on wrong type or no matching track.

*adjustdelay*: Adjust audio start time relative to track number. Pass -2 to disable and -1 to be relative to the first video track if one exists.

*variableformat*: Allow format changes in the output for video. To only allow fixed format output pass 0 or greater to choose the nth encountered format as the output format. Any frames not matching the chosen format are dropped. If the file is constant format (most are) this setting does nothing.

*fpsnum*: Convert the source material to constant framerate. Cannot be combined with *rff*.

*fpsden*: Convert the source material to constant framerate. Used in conjunction with *fpsnum*.

*rff*: Apply RFF flags to the video. If the video doesn't have or use RFF flags the output is unchanged compare to when the option is disabled. Cannot be combined with *fpsnum*.

*threads*: Number of threads to use for decoding. Pass 0 to autodetect.

*seekpreroll*: Number of frames before the requested frame to cache when seeking.

*enable_drefs*: Option passed to the FFmpeg mov demuxer.

*use_absolute_path*: Option passed to the FFmpeg mov demuxer.

*drc_scale*: Apply dynamic range compression to ac3 audio. 0 = None and 1.0 = Normal.

*cachemode*:

    0 = Never read or write index to disk
    1 = Always try to read index but only write index to disk when it will make a noticeable difference on subsequent runs and store index files in a subtree of *cachepath*
    2 = Always try to read and write index to disk and store index files in a subtree of *cachepath*
    3 = Always try to read index but only write index to disk when it will make a noticeable difference on subsequent runs and store index files in the absolute path in *cachepath* with track number and index extension appended
    4 = Always try to read and write index to disk and store index files in the absolute path in *cachepath* with track number and index extension appended

*cachepath*: The path where cache files are written. Note that the actual index files are written into subdirectories using based on the source location. Defaults to %LOCALAPPDATA% on Windows and $XDG_CACHE_HOME/bsindex if set otherwise ~/bsindex on other operation systems in mode 1 and 2. For mode 3 and 4 it defaults to *source*.

*cachesize*: Maximum internal cache size in MB.

*hwdevice*: The interface to use for hardware decoding. Depends on OS and hardware. On windows `d3d11va`, `cuda` and `vulkan` (H264, HEVC and AV1) are probably the ones most likely to work. Defaults to CPU decoding. Will throw errors for formats where hardware decoding isn't possible.

*extrahwframes*: The number of additional frames to allocate when *hwdevice* is set. The number required is unknowable and found through trial and error. The default may be too high or too low. FFmpeg unfortunately is this badly designed.

*timecodes*: Writes a timecode v2 file with all frame times to the file if specified. Note that this option will produce an error if any frame has an unknown timestamp which would result in an invalid timecode file.

*start_number*: The first number of image sequences.

*viewid*: The view id to output, this is currently only used for some mv-hevc files and is quite rare.

*showprogress*: Print indexing progress as VapourSynth information level log messages.

*maxdecoders*: The maximum number of decoder instances kept around, defaults to 4 but when decoding high resolution content it may be beneficial to reduce it to 1 to reduce peak memory usage. For example 4k h264 material will use approximately 250MB of ram in addition to the specified cache size for decoder instance. Passing a number outside the 1-4 range will set it to the biggest number supported.

*hwfallback*: Automatically fall back to CPU decoding if hardware decoding can't be used for the current video track when *hwdevice* is set. Note that the fallback only happens when a hardware decoder is unavailable and not on any other category of error such as *hwdevice* having an invalid value.

*level*: The log level of the FFmpeg library. By default quiet. See FFmpeg documentation for allowed constants. Mostly useful for debugging purposes.
