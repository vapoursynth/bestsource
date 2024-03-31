# BestSource

[![Windows](https://github.com/vapoursynth/bestsource/actions/workflows/windows.yml/badge.svg)](https://github.com/vapoursynth/bestsource/actions/workflows/windows.yml)
[![Linux](https://github.com/vapoursynth/bestsource/actions/workflows/linux.yml/badge.svg)](https://github.com/vapoursynth/bestsource/actions/workflows/linux.yml)
[![macOS](https://github.com/vapoursynth/bestsource/actions/workflows/macos.yml/badge.svg)](https://github.com/vapoursynth/bestsource/actions/workflows/macos.yml)
[![codespell](https://github.com/vapoursynth/bestsource/actions/workflows/codespell.yml/badge.svg)](https://github.com/vapoursynth/bestsource/actions/workflows/codespell.yml)

**BestSource** (abbreviated as **BS**) is a cross-platform wrapper library around [FFmpeg](http://ffmpeg.org)
that ensures always sample and frame accurate access to audio and video with good seeking performance for everything except some lossy audio formats.

It can be used as either a C++ library directly or through the combined VapourSynth and Avisynth+ plugin that's included.

## Dependencies

- FFmpeg 6.1.x. Later releases may or may not work but FFmpeg API breakages are quite common and don't always generate compilation errors. Only `libavcodec`, `libavformat`, `libavutil` and `libswscale` libraries are required.
- libp2p (already included as submodule)

### Windows Compilation

On Windows the easiest way to compile the FFmpeg dependency is to use [vcpkg](https://vcpkg.io) and install `ffmpeg[avcodec,avdevice,avfilter,avformat,bzip2,core,dav1d,gpl,lzma,nvcodec,swresample,swscale,zlib]:x64-windows-static`.

Use the latest version of Visual Studio. It should automatically find all the required libraries if you used vcpkg.

### Linux and MacOS Compilation

Requires `pkg-config`, `meson` and `ninja-build`.

```
git clone https://github.com/vapoursynth/bestsource.git --depth 1 --recurse-submodules --shallow-submodules
cd bestsource
meson setup build
ninja -C build
ninja -C build install
```

## VapourSynth usage

`bs.AudioSource(string source[, int track = -1, int adjustdelay = -1, int threads = 0, bint enable_drefs = False, bint use_absolute_path = False, float drc_scale = 0, string cachepath, int cachesize = 100, bint showprogress = True])`

`bs.VideoSource(string source[, int track = -1, bint variableformat = False, int fpsnum = -1, int fpsden = 1, bint rff = False, int threads = 0, int seekpreroll = 20, bint enable_drefs = False, bint use_absolute_path = False, string cachepath = source, int cachesize = 1000, string hwdevice, int extrahwframes = 9, string timecodes, bint showprogress = True])`

`bs.SetDebugOutput(bint enable = False)`

`bs.SetFFmpegLogLevel(int level = <quiet log level>)`

## Avisynth+ usage

`BSAudioSource(string source[, int track = -1, int adjustdelay = -1, int threads = 0, bool enable_drefs = False, bool use_absolute_path = False, float drc_scale = 0, string cachepath, int cachesize = 100])`

`BSVideoSource(string source[, int track = -1, bint variableformat = False, int fpsnum = -1, int fpsden = 1, bool rff = False, int threads = 0, int seekpreroll = 20, bool enable_drefs = False, bool use_absolute_path = False, string cachepath = source, int cachesize = 1000, string hwdevice, int extrahwframes = 9, string timecodes])`

`BSSetDebugOutput(bool enable = False)`

`BSSetFFmpegLogLevel(int level = <quiet log level>)`

## Argument explanation

*track*: Either a positive number starting from 0 specifying the absolute track number or a negative number to select the nth audio or video track. Throws an error on wrong type or no matching track.

*adjustdelay*: Adjust audio start time relative to track number. Pass -2 to disable and -1 to be relative to the first video track if one exists.

*variableformat*: Allow format changes in the output for video. Untested.

*fpsnum*: Convert the source material to constant framerate. Cannot be combined with *rff*.

*fpsden*: Convert the source material to constant framerate. Used in conjunction with *fpsnum*.

*rff*: Apply RFF flags to the video. If the video doesn't have or use RFF flags the output is unchanged compare to when the option is disabled. Cannot be combined with *fpsnum*.

*threads*: Number of threads to use for decoding. Pass 0 to autodetect.

*seekpreroll*: Number of frames before the requested frame to cache when seeking.

*enable_drefs*: Option passed to the FFmpeg mov demuxer.

*use_absolute_path*: Option passed to the FFmpeg mov demuxer.

*drc_scale*: Apply dynamic range compression to ac3 audio. 0 = None and 1.0 = Normal.

*cachepath*: The full path to of the cache file. Defaults to `source.<track>.bsindex`. Current ignored for AudioSource.

*cachesize*: Maximum internal cache size in MB.

*hwdevice*: The interface to use for hardware decoding. Depends on OS and hardware. On windows `d3d11va`, `cuda` and `vulkan` (H264, HEVC and AV1) are probably the ones most likely to work. Defaults to CPU decoding. Will throw errors for formats where hardware decoding isn't possible.

*extrahwframes*: The number of additional frames to allocate when *hwdevice* is set. The number required is unknowable and found through trial and error. The default may be too high or too low. FFmpeg unfortunately is this badly designed.

*timecodes*: Writes a timecode v2 file with all frame times to the file if specified. Note that this option can produce EXTREMELY INVALID TIMECODE FILES due to performing no additional processing or check on the timestamps reported by FFmpeg. It is common for transport streams and other containers to have unknown values (shows up as large negative values) and discontinuous timestamps.

*showprogress*: Print indexing progress as VapourSynth information level log messages.

*level*: The log level of the FFmpeg library. By default quiet. See FFmpeg documentation for allowed constants. Mostly useful for debugging purposes.
