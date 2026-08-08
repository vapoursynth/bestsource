# Vendored VapourSynth headers

Temporary. Delete this directory and go back to `vs.get_include()` once a VapourSynth release
ships the GPU frame API.

## Why

The GPU frame path (`gpu=True`) needs API 4.3: `getVulkanAPI`, `newGPUVideoFrame`,
`exportGPUPlane`, `createGPUTimeline` and friends. The `VapourSynth` wheel on PyPI still ships
API 4.2 headers, so building against `vs.get_include()` produced either a plugin that failed to
compile or -- worse -- one that compiled and then crashed, because `VSVULKANAPI` declares its
members in a different order than the core it ends up loaded into while carrying the same
`VS_VULKAN_API_VERSION`. A stale header there is not a missing-symbol error, it is a wrong-offset
call through a function pointer table.

Vendoring pins the declarations to the ones the code was written against, so the build no longer
depends on which VapourSynth happens to be installed on the build machine.

## Contents

Only what the plugin includes, taken verbatim from the VapourSynth tree:

| File | Included by |
| --- | --- |
| `VapourSynth4.h` | `src/vapoursynth.cpp`, `src/vsgpuexport.cpp` |
| `VSHelper4.h` | `src/vapoursynth.cpp`, `src/synthshared.cpp`, `src/avisynth.cpp` |
| `VSVulkan4.h` | `src/vsgpuexport.cpp` |

`VSVulkan4.h` also needs `vulkan/vulkan_core.h`, which comes from the vulkan headers package, not
from here.

## Licence

These files are LGPL 2.1 or later, unlike the rest of this repository, which is MIT. They are the
only LGPL files here and they are headers describing an API -- nothing in them is compiled into the
output beyond the declarations. The distinction matters if the directory is ever repackaged, so it
is stated rather than left to be discovered.

## Updating

Copy the three files from a VapourSynth checkout whose core actually implements what the plugin
calls. Do not mix: taking one file from one revision and another from a different one reintroduces
exactly the mismatch this directory exists to prevent.

The runtime check stays regardless of what is vendored here. `src/vapoursynth.cpp` compiles against
the 4.3 declarations but calls `configPlugin` with 4.0 and consults `getAPIVersion()` before
touching anything newer, so the plugin still loads on an older core -- it just does not offer
`gpu=True` there.
