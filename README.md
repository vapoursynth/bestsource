**BestSource** (abbreviated as **BS**) is a cross-platform wrapper library around [FFmpeg](http://ffmpeg.org)
that ensures sample/frame accurate access to audio and video by always linearly decoding the input files. With a few tricks this can guarantee accurate "seeking" and can be surprisingly... not slow. 

It can be used as either a C++ library directly or through the VapourSynth plugin that's included.

### VapourSynth plugin

bs.AudioSource(string source, int track = -1, int adjustdelay = -1, bint exact = True, bint enable_drefs = False, bint use_absolute_path = False, float drc_scale = 0, string cachepath = \<user data dir\>)

bs.VideoSource(string source, int track = -1, bint variableformat = False, int threads = 0, int seekpreroll = 20, bint exact = True, bint enable_drefs = False, bint use_absolute_path = False, string cachepath = \<user data dir\>)

*track*: Either a positive number starting from 0 specifying the absolute track number or a negative number to select the nth audio or video track. Throws an error on wrong type or no matching track.

*adjustdelay*: Adjust audio start time relative to track number. Pass -2 to disable and -1 to be relative to the first video track if one exists.

*variableformat*: Allow format changes in the output for video. Untested.

*threads*: Number of threads to use for decoding. Pass 0 to autodetect.

*seekpreroll*: Number of frames before the requested frame to cache when seeking.

*exact*: Decode the full track to get an exact frame/sample count. Will throw an error if set to false and the frame or sample count can't be guessed at all.

*enable_drefs*: Option passed to the FFmpeg mov demuxer.

*use_absolute_path*: Option passed to the FFmpeg mov demuxer.

*drc_scale*: Apply dynamic range compression to ac3 audio. 0 = None and 1.0 = Normal.

*cachepath*: The path where the file bscache.json will be stored. Defaults to `%LOCALAPPDATA%\bscache.json` or `~/bscache.json` depending on the OS.