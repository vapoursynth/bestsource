**BestSource** (usually known as **BS**) is a cross-platform wrapper library around [FFmpeg](http://ffmpeg.org)
that ensures sample/frame accurate access to audio and video by always linearly decoding the input files. With a few tricks to speed
things up and better handle seeking.

It can be used as either a C++ library directly or through the VapourSynth plugin that's included.

The track length cache is stored in `%LOCALAPPDATA%\bscache.json` or `~/bscache.json` depending on the OS.

### VapourSynth plugin

bs.AudioSource(string source, int track = -1, int adjustdelay = ???, bint exactsamples = True, bint enable_drefs = False, bint use_absolute_path = False, float drc_scale = 0)
bs.VideoSource(string source, int track = -1, bint variableformat = False, int seekpreroll = 20, bint exactframes = True, bint enable_drefs = False, bint use_absolute_path = False)

*track*: Either a positive number starting from 0 specifying the absolute track number or a negative number to select the nth audio or video track. Throws an error on wrong type or no matching track.
*adjustdelay*: Not implemented yet. Will pad/cut audio relative to video track start time.
*variableformat*: Allow format changes in the output for video. Untested.
*seekpreroll*: Number of frames before the requested frame to cache when seeking.
*exactsamples/exactframes*: Decode the full track to get an exact frame/sample count. Only set to false for quick preview or possibly AVI files.
*enable_drefs*: Option passed to the FFmpeg mov demuxer.
*use_absolute_path*: Option passed to the FFmpeg mov demuxer.
*drc_scale*: Apply dynamic range compression to ac3 audio. 0 = None and 1.0 = Normal.
