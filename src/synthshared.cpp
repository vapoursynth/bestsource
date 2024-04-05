//  Copyright (c) 2022-2024 Fredrik Mellbin
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

#include "synthshared.h"
#include "VSHelper4.h"

void SetSynthFrameProperties(const std::unique_ptr<BestVideoFrame> &Src, const VideoProperties &VP, bool RFF, const std::function<void(const char *, int64_t)> &mapSetInt, const std::function<void(const char *, double)> &mapSetFloat, const std::function<void(const char *, const char *, int, bool)> &mapSetData) {
    // Set AR variables
    if (VP.SAR.Num > 0 && VP.SAR.Den > 0) {
        mapSetInt("_SARNum", VP.SAR.Num);
        mapSetInt("_SARDen", VP.SAR.Den);
    }

    mapSetInt("_Matrix", Src->Matrix);
    mapSetInt("_Primaries", Src->Primaries);
    mapSetInt("_Transfer", Src->Transfer);
    if (Src->ChromaLocation > 0)
        mapSetInt("_ChromaLocation", Src->ChromaLocation - 1);

    if (Src->ColorRange == 1) // Hardcoded ffmpeg constants, nothing to see here
        mapSetInt("_ColorRange", 1);
    else if (Src->ColorRange == 2)
        mapSetInt("_ColorRange", 0);

    if (!RFF) {
        mapSetData("_PictType", &Src->PictType, 1, true);

        // Set field information
        int FieldBased = 0;
        if (Src->InterlacedFrame)
            FieldBased = (Src->TopFieldFirst ? 2 : 1);
        mapSetInt("_FieldBased", FieldBased);
        mapSetInt("RepeatField", Src->RepeatPict);

        // FIXME, use PTS difference between frames instead?
        if (Src->Duration > 0) {
            int64_t DurNum = VP.TimeBase.Num;
            int64_t DurDen = VP.TimeBase.Den;
            vsh::muldivRational(&DurNum, &DurDen, Src->Duration, 1);
            mapSetInt("_DurationNum", DurNum);
            mapSetInt("_DurationDen", DurDen);
        }
    }

    if (Src->HasMasteringDisplayPrimaries) {
        for (int i = 0; i < 3; i++) {
            mapSetFloat("MasteringDisplayPrimariesX", Src->MasteringDisplayPrimaries[i][0].ToDouble());
            mapSetFloat("MasteringDisplayPrimariesY", Src->MasteringDisplayPrimaries[i][1].ToDouble());
        }
        mapSetFloat("MasteringDisplayWhitePointX", Src->MasteringDisplayWhitePoint[0].ToDouble());
        mapSetFloat("MasteringDisplayWhitePointY", Src->MasteringDisplayWhitePoint[1].ToDouble());
    }

    if (Src->HasMasteringDisplayLuminance) {
        mapSetFloat("MasteringDisplayMinLuminance", Src->MasteringDisplayMinLuminance.ToDouble());
        mapSetFloat("MasteringDisplayMaxLuminance", Src->MasteringDisplayMaxLuminance.ToDouble());
    }

    if (Src->HasContentLightLevel) {
        mapSetInt("ContentLightLevelMax", Src->ContentLightLevelMax);
        mapSetInt("ContentLightLevelAverage", Src->ContentLightLevelAverage);
    }

    if (Src->DolbyVisionRPU && Src->DolbyVisionRPUSize) {
        mapSetData("DolbyVisionRPU", reinterpret_cast<const char *>(Src->DolbyVisionRPU), static_cast<int>(Src->DolbyVisionRPUSize), false);
    }

    if (Src->HDR10Plus && Src->HDR10PlusSize > 0) {
        mapSetData("HDR10Plus", reinterpret_cast<const char *>(Src->HDR10Plus), static_cast<int>(Src->HDR10PlusSize), false);
    }

    if (Src->ICCProfile && Src->ICCProfileSize > 0) {
        mapSetData("ICCProfile", reinterpret_cast<const char *>(Src->ICCProfile), static_cast<int>(Src->ICCProfileSize), false);
    }

    mapSetInt("FlipVertical", VP.FlipVerical);
    mapSetInt("FlipHorizontal", VP.FlipHorizontal);
    mapSetInt("Rotation", VP.Rotation);
}