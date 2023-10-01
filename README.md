[![Windows](https://github.com/vapoursynth/bestsource/actions/workflows/windows.yml/badge.svg)](https://github.com/vapoursynth/bestsource/actions/workflows/windows.yml)
[![Linux](https://github.com/vapoursynth/bestsource/actions/workflows/linux.yml/badge.svg)](https://github.com/vapoursynth/bestsource/actions/workflows/linux.yml)
[![macOS](https://github.com/vapoursynth/bestsource/actions/workflows/macos.yml/badge.svg)](https://github.com/vapoursynth/bestsource/actions/workflows/macos.yml)
[![codespell](https://github.com/vapoursynth/bestsource/actions/workflows/codespell.yml/badge.svg)](https://github.com/vapoursynth/bestsource/actions/workflows/codespell.yml)

**BestSource** (abbreviated as **BS**) is a cross-platform wrapper library around [FFmpeg](http://ffmpeg.org)
that ensures sample/frame accurate access to audio and video by always linearly decoding the input files. With a few tricks this can guarantee accurate "seeking" and can be surprisingly... not slow. 

It can be used as either a C++ library directly or through the VapourSynth plugin that's included.

### Compiling

Requires FFmpeg, Jansson and libp2p to compile.

### VapourSynth plugin

bs.AudioSource(string source[, int track = -1, int adjustdelay = -1, int threads = 0, bint exact = True, bint enable_drefs = False, bint use_absolute_path = False, float drc_scale = 0, string cachepath, int cachesize = 100, bint showprogress = True])

bs.VideoSource(string source[, int track = -1, bint variableformat = False, int threads = 0, int seekpreroll = 20, bint exact = True, bint enable_drefs = False, bint use_absolute_path = False, string cachepath, int cachesize = 1000, string hwdevice, bint showprogress = True])

*track*: Either a positive number starting from 0 specifying the absolute track number or a negative number to select the nth audio or video track. Throws an error on wrong type or no matching track.

*adjustdelay*: Adjust audio start time relative to track number. Pass -2 to disable and -1 to be relative to the first video track if one exists.

*variableformat*: Allow format changes in the output for video. Untested.

*threads*: Number of threads to use for decoding. Pass 0 to autodetect.

*seekpreroll*: Number of frames before the requested frame to cache when seeking.

*exact*: Decode the full track to get an exact frame/sample count. Will throw an error if set to false and the frame or sample count can't be guessed at all. If too many frames/samples is guessed the end is passed with the last available frame/silence to fill up the missing data.

*enable_drefs*: Option passed to the FFmpeg mov demuxer.

*use_absolute_path*: Option passed to the FFmpeg mov demuxer.

*drc_scale*: Apply dynamic range compression to ac3 audio. 0 = None and 1.0 = Normal.

*cachepath*: The full path to the cache file. Defaults to `%LOCALAPPDATA%\bsindex.json` or `~/bsindex.json` depending on the OS.

*cachesize*: Maximum internal cache size in MB.

*hwdevice*: The interface to use for hardware decoding. Depends on OS and hardware. On windows `d3d11va` and `cuda` are probably the ones most likely to work. Defaults to CPU decoding. Will throw errors for formats where hardware decoding isn't possible.

*showprogress*: Print indexing progress as VapourSynth information level log messages when *exact* frame/sample count is determined.