/*
 * Fused hash + export pass.
 *
 * Reads the decoded frame once and does both jobs on the way through:
 *   - writes the VapourSynth plane layout (linear buffers, one plane per component)
 *   - accumulates the verification hash
 *
 * A compute shader is required regardless of hashing: FFmpeg hands over a tiled VkImage and
 * VapourSynth wants linear buffers with no semi-planar formats, so P010's interleaved UV has to
 * be split. vkCmdCopyImageToBuffer cannot do that. Hashing on the way through therefore costs no
 * additional bandwidth -- every byte is already being read.
 *
 * SHAPE CONFIRMED BY PROBE (vkframeprobe.cpp, AMD/NVIDIA desktop, FFmpeg 9.0):
 *   sw_format is nv12 or p010le -- always 2 plane semi-planar, never 3 plane
 *   ONE multiplane VkImage, so views need VK_IMAGE_ASPECT_PLANE_0/1_BIT, not COLOR_BIT
 *   frames context usage 0x440f already includes VK_IMAGE_USAGE_STORAGE_BIT
 *   plane view formats R8_UNORM/R8G8_UNORM and R16_UNORM/R16G16_UNORM all support STORAGE_IMAGE
 *   images arrive in VK_IMAGE_LAYOUT_VIDEO_DECODE_DST_KHR and must be transitioned to GENERAL
 *   queue_family is VK_QUEUE_FAMILY_IGNORED (allocated CONCURRENT), so no ownership transfer
 *
 * Because the source is always 2 plane semi-planar, this handles exactly that and nothing else.
 * Explicit format qualifiers are used rather than GL_EXT_shader_image_load_formatted, which would
 * require shaderStorageImageReadWithoutFormat -- a feature VapourSynth's device does not enable.
 * The views are created with the _UINT reinterpretation of the plane's _UNORM format so the hash
 * is defined on the stored bits, which sidesteps the LSB/MSB alignment question entirely.
 *
 * Determinism, which is the whole ballgame:
 *   - integer ops only, no float anywhere (non-associative, and driver-variable)
 *   - each tile's hash depends only on its own samples and its own coordinates
 *   - tiles are combined with XOR, which is commutative and associative, so atomicXor across
 *     workgroups gives the same result regardless of scheduling order
 * Never combine with something order-dependent. That is the one mistake that tests clean on one
 * GPU and silently produces a different digest on another.
 *
 * The hash does not need to match the CPU XXH3 in videosource.cpp: HWDevice is written into the
 * index header and compared on load, so an index built with hwdevice=vulkan is never consumed by
 * a software run.
 *
 * Compile two variants via SAMPLE_BITS (8 or 16).
 */

#version 450
#pragma shader_stage(compute)

#extension GL_EXT_scalar_block_layout : require

#ifndef SAMPLE_BITS
#define SAMPLE_BITS 16
#endif

#if SAMPLE_BITS == 16
#extension GL_EXT_shader_16bit_storage : require
#define sample_t uint16_t
#define LUMA_FMT r16ui
#define CHROMA_FMT rg16ui
#else
#extension GL_EXT_shader_8bit_storage : require
#define sample_t uint8_t
#define LUMA_FMT r8ui
#define CHROMA_FMT rg8ui
#endif

/* Samples handled per invocation along x. Each workgroup then covers SAMPLES_X times as much of
   the image, which cuts the number of workgroups -- and therefore the number of atomicXor
   operations contending on the accumulator, and the number of shared memory reduction rounds per
   sample -- by the same factor. The hash value is unaffected: every sample is still mixed with
   its own coordinates and the combine is commutative. */
#ifndef SAMPLES_X
#define SAMPLES_X 8
#endif

layout (local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

/* Indexing only needs the hash; skip the plane writes entirely. */
layout (constant_id = 0) const int do_export = 1;

/* Two separate bindings rather than an array: the planes have different formats, and a GLSL
 * image declaration carries exactly one format qualifier. */
layout (set = 0, binding = 0, LUMA_FMT)   uniform readonly uimage2D src_luma;
layout (set = 0, binding = 1, CHROMA_FMT) uniform readonly uimage2D src_chroma;

layout (set = 0, binding = 2) writeonly buffer DstY { sample_t data[]; } dst_y;
layout (set = 0, binding = 3) writeonly buffer DstU { sample_t data[]; } dst_u;
layout (set = 0, binding = 4) writeonly buffer DstV { sample_t data[]; } dst_v;

/* Two 32-bit lanes rather than one 64-bit value: avoids requiring shaderBufferInt64Atomics, and
 * XOR needs no carry so the halves accumulate independently and correctly. */
layout (set = 0, binding = 5) buffer HashAcc { uint lane[2]; } acc;

/* Written by verify.comp.glsl for an earlier frame. Checked only to avoid writing garbage into a
 * VapourSynth buffer already known to be wrong; it does not inform the host, which Vulkan has no
 * mechanism for. */
layout (set = 0, binding = 6) readonly buffer Status { uint failed; } status;

layout (push_constant, scalar) uniform Push {
    ivec2 luma_size;      /* in samples */
    ivec2 chroma_size;    /* in samples, per component */
    int   dst_stride_y;   /* in samples, not bytes */
    int   dst_stride_uv;
} pc;

/* NOTE: nothing position-dependent may enter the hash -- no frame number, no PTS. SeekAndDecode
 * matches a run of decoded hashes against index[i..i+k] at an unknown i, so the hash has to be a
 * pure function of pixel content. Genuinely duplicate frames therefore hash identically, which is
 * not a defect: it is exactly the case UndeterminableLocation handles by decoding another frame
 * to extend the run. */

/* xxHash32 constants. Well-studied avalanche, and every op here is full rate on GPU hardware
 * (32-bit add/xor/rotate/multiply). Deliberately not a cryptographic hash: the threat model is
 * "did the decoder hand me the wrong frame", not an adversary constructing collisions. */
const uint PRIME32_1 = 0x9E3779B1u;
const uint PRIME32_2 = 0x85EBCA77u;
const uint PRIME32_3 = 0xC2B2AE3Du;
const uint PRIME32_4 = 0x27D4EB2Fu;
const uint PRIME32_5 = 0x165667B1u;

uint mix32(uint h, uint v)
{
    h ^= v * PRIME32_2;
    h  = (h << 13) | (h >> 19);
    return h * PRIME32_1 + PRIME32_3;
}

uint avalanche32(uint h)
{
    h ^= h >> 15; h *= PRIME32_2;
    h ^= h >> 13; h *= PRIME32_3;
    h ^= h >> 16;
    return h;
}

/* One entry per invocation; reduced in shared memory so each workgroup issues a single pair of
 * atomics rather than 256 of them. */
shared uvec2 red[gl_WorkGroupSize.x * gl_WorkGroupSize.y];

void main()
{
    const int plane = int(gl_GlobalInvocationID.z);
    const ivec2 size = (plane == 0) ? pc.luma_size : pc.chroma_size;
    const int base_x = int(gl_GlobalInvocationID.x) * SAMPLES_X;
    const int y = int(gl_GlobalInvocationID.y);

    uvec2 h = uvec2(0u);

    /* Dispatch covers the luma plane, so chroma invocations past its smaller extent idle here. */
    for (int s = 0; s < SAMPLES_X; s++) {
        const ivec2 pos = ivec2(base_x + s, y);
        if (any(greaterThanEqual(pos, size)))
            break;

        const uvec4 texel = (plane == 0) ? imageLoad(src_luma, pos)
                                         : imageLoad(src_chroma, pos);

        /* Coordinates and plane index are folded in so a transposed or shifted frame cannot
         * collide, and so sample order cannot be confused by the commutative combine below. */
        uint seed = mix32(uint(pos.x), uint(pos.y));
        seed = mix32(seed, uint(plane));

        uint h0 = mix32(seed ^ PRIME32_4, texel.x);
        uint h1 = mix32(seed ^ PRIME32_5, texel.x);

        if (plane == 1) {
            h0 = mix32(h0, texel.y);
            h1 = mix32(h1, texel.y);
        }

        /* Avalanched and folded in here, so the per invocation accumulator is the XOR of its own
         * samples and the reduction below stays a plain XOR tree. */
        h.x ^= avalanche32(h0);
        h.y ^= avalanche32(h1);

        if (do_export != 0 && status.failed == 0u) {
            if (plane == 0) {
                dst_y.data[pos.y * pc.dst_stride_y + pos.x] = sample_t(texel.x);
            } else {
                /* The split that makes this a shader and not a copy command. */
                dst_u.data[pos.y * pc.dst_stride_uv + pos.x] = sample_t(texel.x);
                dst_v.data[pos.y * pc.dst_stride_uv + pos.x] = sample_t(texel.y);
            }
        }
    }

    /* Already avalanched per sample above, so an invocation with no samples in bounds contributes
     * zero, which is the XOR identity. */
    const uint lid = gl_LocalInvocationIndex;
    red[lid] = h;
    barrier();

    /* Fixed-shape XOR tree. Shape depends only on workgroup size, so it is identical on every
     * device; XOR makes the ordering irrelevant anyway. */
    for (uint s = (gl_WorkGroupSize.x * gl_WorkGroupSize.y) / 2u; s > 0u; s >>= 1u) {
        if (lid < s)
            red[lid] ^= red[lid + s];
        barrier();
    }

    if (lid == 0u) {
        atomicXor(acc.lane[0], red[0].x);
        atomicXor(acc.lane[1], red[0].y);
    }
}
