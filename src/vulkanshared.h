//  Copyright (c) 2026 Fredrik Mellbin
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

#ifndef BSVULKANSHARED_H
#define BSVULKANSHARED_H

/* The pieces of Vulkan plumbing gpuhash.cpp and vsgpuexport.cpp would otherwise each carry a
   copy of. Deliberately does not include any Vulkan header itself: vsgpuexport.cpp has to
   sequence windows.h and the platform structures around its Vulkan include, so this header is
   included after the Vulkan types already exist rather than deciding how they arrive. */

#include "bsshared.h"

#include <cstdint>
#include <string>

/* Declares one entry of a function pointer table; apply through a per file X-macro list. */
#define BS_VK_DECLARE_FUNC(n) PFN_##n n = nullptr;

/* A hardware decoder exception rather than a plain one, because every caller sits on the GPU
   path: at creation and indexing time that is exactly the type gpufallback catches to rebuild
   the source on the CPU, and at frame time the distinction is harmless since it still is a
   BestSourceException. */
[[noreturn]] inline void BSThrowVk(const char *Prefix, const char *What, VkResult Res) {
    throw BestSourceHWDecoderException(std::string(Prefix) + ": " + What + " failed (VkResult " +
        std::to_string(static_cast<int>(Res)) + ")");
}

/* First memory type in TypeBits carrying all of Want; UINT32_MAX when there is none. Callers
   wanting a preference order ask twice, the preferred flags first and the fallback second. */
inline uint32_t BSFindVkMemoryType(const VkPhysicalDeviceMemoryProperties &Props, uint32_t TypeBits, VkMemoryPropertyFlags Want) {
    for (uint32_t i = 0; i < Props.memoryTypeCount; i++)
        if ((TypeBits & (1u << i)) && (Props.memoryTypes[i].propertyFlags & Want) == Want)
            return i;
    return UINT32_MAX;
}

#endif
