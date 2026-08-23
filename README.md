# BestSource

[![build](https://github.com/vapoursynth/bestsource/actions/workflows/build.yml/badge.svg)](https://github.com/vapoursynth/bestsource/actions/workflows/build.yml)
[![codespell](https://github.com/vapoursynth/bestsource/actions/workflows/codespell.yml/badge.svg)](https://github.com/vapoursynth/bestsource/actions/workflows/codespell.yml)

**BestSource** (abbreviated as **BS**) is a cross-platform wrapper library around [FFmpeg](http://ffmpeg.org)
that ensures always sample and frame accurate access to audio and video with good seeking performance for everything except some lossy audio formats.

It can be used as either a C++ library directly or through the combined VapourSynth and Avisynth+ plugin that's included.

## Dependencies

- FFmpeg 9.0.x supported. Later releases may or may not work but FFmpeg API breakages are quite common and don't always generate compilation errors. Only `libavcodec`, `libavformat`, `libavutil` libraries are required.
- xxHash

Optional, and only needed for *gpu*:

- Vulkan headers, 1.4 or newer, plus an FFmpeg built with `--enable-vulkan`. Nothing links the Vulkan loader: every entry point is resolved through FFmpeg's own at runtime, so the headers alone are enough to build. Distributions often still package 1.3.x, which is too old, since the VapourSynth GPU frame API uses entry points that only became core in 1.4.
- `glslangValidator` (or `glslang`, depending on how the distribution names it), which compiles the frame hashing shader to SPIR-V.

Both are picked up automatically. Pass `-Denable_gpu_hash=enabled` to make a missing one fail the build rather than quietly drop GPU decoding.

Hardware decoding is Vulkan only, so the QSV and CUDA/nvcodec headers that older versions wanted are no longer used by anything and can be left out.

The VapourSynth headers come from the installed VapourSynth via `vs.get_include()`. GPU frames need the API 4.3 declarations, which VapourSynth ships from R80a1 onwards.

### Windows Compilation

On Windows the easiest way to compile the the dependencies is to use [vcpkg](https://vcpkg.io) to install `ffmpeg[avcodec,avdevice,avfilter,avformat,swresample,swscale,zlib,bzip2,core,dav1d,gpl,version3,lzma,vulkan,openssl,xml2]:x64-windows-static`, `vulkan-headers:x64-windows-static` and `xxhash:x64-windows-static`. Do however note that this is without Little CMS2 support.
Use the latest version of Visual Studio. It should automatically find all the required libraries if you used vcpkg.
For GPU decoding also install `glslang[tools]:x64-windows-static` and put its `tools/glslang` directory on `PATH` so `glslangValidator` is found.

### Linux and MacOS Compilation

Requires `pkg-config`, `meson` and `ninja-build`.

```
git clone https://github.com/vapoursynth/bestsource.git --depth 1
cd bestsource
meson setup build
meson compile -C build
meson install -C build
```

### Known issues and limitations

- Seeking performance in mpeg/ts/vob files can be quite poor due to the FFmpeg demuxer
- Seeking and decoding performance for lossy audio formats (aac, mp3, dts, ac3, vorbis) can be poor
- VC1 codec is unseekable due to FFmpeg not having bitexact output after seeking
- The unholy combination of VFR H264 in AVI has poor seeking performance
- Needs FFmpeg compiled with Little CMS2 or the color information reported for most image files will be less complete
- Mod files can't be decoded correctly using libmodplug due to the library not having repeatable bitexact output
- Gray+alpha format isn't supported in Avisynth+ and as a result only the Y component is returned
- Files with dimensions that aren't a multiple of the subsampling value will be cropped

## VapourSynth usage

`bs.AudioSource(string source[, int track = -1, int adjustdelay = -1, int threads = 0, bint enable_drefs = False, bint use_absolute_path = False, float drc_scale = 0, int cachemode = 1, string cachepath, int cachesize = 100, bint showprogress = True, maxdecoders = 0, int variableformat = 0])`

`bs.VideoSource(string source[, int track = -1, int variableformat = -1, int fpsnum = -1, int fpsden = 1, bint rff = False, int threads = 0, int seekpreroll = 20, bint enable_drefs = False, bint use_absolute_path = False, int cachemode = 1, string cachepath , int cachesize = 100, int extrahwframes = 9, string timecodes, int start_number, int viewid = 0, bint showprogress = True, maxdecoders = 0, bool gpufallback = True, exporttimestamps = False, bint apply_rotation = True, bint gpu = False])`

`bs.TrackInfo(string source[, bint enable_drefs = False, bint use_absolute_path = False])`

`bs.Metadata(string source[, int track, bint enable_drefs = False, bint use_absolute_path = False])`

`bs.SetDebugOutput(bint enable = False)`

`bs.SetFFmpegLogLevel(int level = <quiet log level>)`

The *TrackInfo* function only returns the most basic information about a track which is the type, codec and disposition. Its main use is to be able to implement custom track selection logic for the source functions. It returns one entry per track in *tracktype*, *tracktypestr*, *codec*, *codecstr*, *disposition* and *dispositionstr*, so the track number to pass to a source function is the index into those arrays.

The *Metadata* function returns all the file or track metadata as key-value pairs depending on whether or not *track* is specified.

## Avisynth+ usage

`BSAudioSource(string source[, int track = -1, int adjustdelay = -1, int threads = 0, bool enable_drefs = False, bool use_absolute_path = False, float drc_scale = 0, int cachemode = 1, string cachepath, int cachesize = 100, int maxdecoders = 0, int variableformat = 0])`

`BSVideoSource(string source[, int track = -1, int fpsnum = -1, int fpsden = 1, bool rff = False, int threads = 0, int seekpreroll = 20, bool enable_drefs = False, bool use_absolute_path = False, int cachemode = 1, string cachepath, int cachesize = 100, string timecodes, int start_number, int variableformat = 0, int viewid = 0, int maxdecoders = 0, bool apply_rotation = True])`

`BSSource(string source[, int atrack = -1, int vtrack = -1, int fpsnum = -1, int fpsden = 1, bool rff = False, int threads = 0, int seekpreroll = 20, bool enable_drefs = False, bool use_absolute_path = False, int cachemode = 1, string cachepath, int acachesize = 100, int vcachesize = 100, string timecodes, int start_number, int vvariableformat = 0, int adjustdelay = -1, float drc_scale = 0, int viewid = 0, int maxdecoders = 0, bool apply_rotation = True, int avariableformat = 0])`

`BSSetDebugOutput(bool enable = False)`

`BSSetFFmpegLogLevel(int level = <quiet log level>)`

Note that the *BSSource* function by default will silently ignore errors when opening audio and in that case only return the video track. However if *atrack* is explicitly set failure to open the audio track will return an error.

## Argument explanation

*source*: The source filename. Note that image sequences also can be opened by using %d or %03d for zero padded numbers. Sequences may start at any number between 0 and 4 unless otherwise specified with *start_number*. It's also possible to pass urls and other ffmpeg protocols like concat.

*track*: Either a positive number starting from 0 specifying the absolute track number or a negative number to select the nth audio or video track. Throws an error on wrong type or no matching track.

*adjustdelay*: Adjust audio start time relative to a video track number. Pass -2 to disable and -1 to be relative to the first video track if one exists. Specifying a non-video track is equivalent to passing -2. Note that the offset is always relative to the first CPU-decodable frame in the stream meaning that it may not be the correct delay when *gpu* and *variableformat* are used.

*variableformat*: Selects which of the formats encountered in the track is used for the output. Pass 0 or greater to choose the nth one, and any frames not matching it are dropped. If the file is constant format (most are) this setting does nothing.

For video, -1 additionally allows the format to change in the output instead of picking one, which is the default in VapourSynth. Avisynth+ has no variable format clips and rejects -1, so its default is 0.

For audio the value must be 0 or greater in both plugins, since neither an Avisynth+ clip nor a VapourSynth audio node can change sample type, sample rate or channel count part way through. The default of 0 keeps the first format encountered and drops everything else.

In *BSSource* the two tracks are set separately as *vvariableformat* and *avariableformat*, following the same convention as *vtrack*/*atrack* and *vcachesize*/*acachesize*. Note that this replaces the old unprefixed *variableformat* argument, which applied to the video track only.

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
    3 = Always try to read index but only write index to disk when it will make a noticeable difference on subsequent runs and store index files with *cachepath* used as the base filename with track number and index extension automatically appended 
    4 = Always try to read and write index to disk and store index files with *cachepath* used as the base filename with track number and index extension automatically appended

*cachepath*: The path where cache files are written. Note that the actual index files are written into subdirectories using based on the source location. Defaults to %LOCALAPPDATA% on Windows and $XDG_CACHE_HOME/bsindex if set otherwise ~/bsindex on other operation systems in mode 1 and 2. For mode 3 and 4 it defaults to *source*.

*cachesize*: Maximum internal cache size in MB.

*gpufallback*: Decode on the CPU when *gpu* was asked for but isn't possible, whether because the core is too old, the driver can't decode the codec or that codec's chroma subsampling, the format can't be exported, the device can't share memory, or the decoder ends up on a different graphics card than VapourSynth. On by default, and it does nothing unless *gpu* is set. VapourSynth only.

**With this enabled the returned clip's frames may be either GPU resident or not, and handling that is up to you.** A script that unconditionally feeds the clip to a filter taking GPU frames will fail on a machine that fell back. Either check the residency and branch, or pass `gpufallback=False` so that anything preventing GPU decoding is an error instead and the residency is whatever you asked for. Note that falling back means the file is indexed for CPU decoding, since the index records which decoder wrote it.

*extrahwframes*: The number of additional frames to allocate when *gpu* is set. The number required is unknowable and found through trial and error. The default may be too high or too low. FFmpeg unfortunately is this badly designed.

*timecodes*: Writes a timecode v2 file with all frame times to the file if specified. Note that this option will produce an error if any frame has an unknown timestamp which would result in an invalid timecode file.

*start_number*: The first number of image sequences.

*viewid*: The view id to output, this is currently only used for some mv-hevc files and is quite rare.

*showprogress*: Print indexing progress as VapourSynth information level log messages.

*maxdecoders*: The maximum number of decoder instances kept around, defaults to 4 but when decoding high resolution content it may be beneficial to reduce it to 1 to reduce peak memory usage. For example 4k h264 material will use approximately 250MB of ram in addition to the specified cache size for decoder instance. Passing a number outside the 1-4 range will set it to the biggest number supported.

*gpu*: Decode on the graphics card with Vulkan and output GPU resident frames, instead of decoding on the CPU. VapourSynth only, and requires a driver with Vulkan video decoding for the codec and a device that can share memory. It always uses the same graphics card VapourSynth itself is on, since sharing frames between two devices only works when they are the same one.

The frames are written directly into VapourSynth's own GPU memory by the decoder and never travel over the bus. They can be fed to any filter taking a GPU clip, and `std.GPUDownload` brings them back to memory.

There is deliberately no mode that decodes on the graphics card and reads the frames back: anything that wants pixels in memory is better off decoding on the CPU, which gets them there without the round trip.

Every argument works with this, *rff* included. RFF interleaves the two fields of two decoded frames, which is done in the export shader rather than by writing into one of them, so it costs an extra hardware frame in flight and nothing else. Raise *extrahwframes* if a file using RFF runs out.

Only the formats FFmpeg's Vulkan decoder produces can be output, which in practice means 4:2:0 and 4:2:2 at 8 to 12 bits. Anything else, monochrome included, is rejected when the source is opened rather than partway through a script, so *gpufallback* can still act on it.

What happens when GPU decoding isn't possible at all depends on *gpufallback*, which is on by default, so the returned frames are only guaranteed to be GPU resident when *gpufallback* is also turned off.

Note that GPU decoding is not necessarily faster than CPU decoding, and on integrated graphics it is usually slower. Its real benefits are freeing up the CPU and keeping frames in video memory for GPU filters.

*apply_rotation*: Apply the vertical flip and the rotation stored in the video track's display matrix so the output has the orientation the video is meant to be shown in. Note that mirroring is always reported as a vertical flip followed by a rotation. The *FlipVertical* and *Rotation* frame properties are set to 0 when enabled since the transform has already been applied. Only rotations that are a multiple of 90 degrees can be applied and anything else is an error. A 90 or 270 degree rotation of a format with non-square chroma subsampling, such as 4:2:2, is handled differently by the two plugins. Avisynth+ resamples the chroma planes and is therefore not lossless, whereas VapourSynth swaps the subsampling axes losslessly and outputs a different format than the source, 4:4:0 for a 4:2:2 source. In VapourSynth a rotation that isn't a multiple of 180 degrees additionally requires a constant format clip, so it can't be combined with a *variableformat* of -1 on a file that actually changes format.

*exporttimestamps*: Returns an additional array of all frame *timestamps* and its timebase in *timebasenum* and *timebaseden* containing all frame times addition to the video clip. Note that unknown timestamps can be set to AV_NOPTS_VALUE. Cannot be combined with *rff* and *fpsnum* modes.

*level*: The log level of the FFmpeg library. By default quiet. See FFmpeg documentation for allowed constants. Mostly useful for debugging purposes.
