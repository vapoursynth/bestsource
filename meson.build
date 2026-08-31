project('BestSource', 'cpp',
    default_options: ['buildtype=release', 'b_ndebug=if-release', 'cpp_std=c++17'],
    license: 'MIT',
    license_files: 'LICENSE',
    meson_version: '>=1.2.3',
    version: '21.1',
)

enable_plugin = get_option('enable_plugin')

libavutil_dep = dependency('libavutil', version: '>=61.1.0')

deps = [
    dependency('libavcodec', version: '>=62.11.0'),
    dependency('libavformat', version: '>=62.3.0'),
    libavutil_dep,
    dependency('libp2p'),
    dependency('libxxhash'),
]

incdir = []

link_args = []

py = import('python').find_installation(pure: false)

sources = files(
    'src/audiosource.cpp',
    'src/bsshared.cpp',
    'src/gpuhash.cpp',
    'src/tracklist.cpp',
    'src/videosource.cpp',
)

cpp_args = []

# GPU hashing needs the vulkan headers to compile against FFmpeg's AVVkFrame, and glslangValidator
# to turn the compute shader into SPIR-V. Nothing links the vulkan loader: every entry point is
# resolved through AVVulkanDeviceContext::get_proc_addr at runtime, so FFmpeg's loader is always
# the one used.
gpu_hash_opt = get_option('enable_gpu_hash')
# Both names are accepted because upstream glslang renamed the binary to 'glslang' and distros are
# split on which one they ship, with the old name only sometimes kept as a compatibility symlink.
glslang = find_program('glslangValidator', 'glslang', required: gpu_hash_opt)
# Checked against libavutil rather than bare: the vulkan headers normally arrive through the same
# include path as FFmpeg rather than being on the compiler's default search path, and hwcontext_
# vulkan.h includes them, so that is the path that actually matters.
cc = meson.get_compiler('cpp')
has_vulkan_headers = cc.has_header('vulkan/vulkan.h', dependencies: libavutil_dep)

if gpu_hash_opt.enabled() and not has_vulkan_headers
    error('enable_gpu_hash was requested but vulkan/vulkan.h was not found')
endif

enable_gpu_hash = glslang.found() and has_vulkan_headers

if enable_gpu_hash
    # Samples each shader invocation handles along x. Defined here rather than in either the shader
    # or the C++ because both need it and they must agree: the dispatch size is derived from it.
    # Higher values mean fewer workgroups, which cuts contention on the atomic accumulator.
    gpu_hash_samples_x = '8'

    # One variant per sample size rather than a runtime branch: the plane image formats differ
    # between 8 and 16 bit, and a GLSL image declaration carries exactly one format qualifier.
    spv = []
    foreach bits : ['8', '16']
        spv += custom_target('hashexport' + bits + '.spv',
            input: 'src/shaders/hashexport.comp.glsl',
            output: 'hashexport' + bits + '.spv',
            command: [glslang, '--target-env', 'vulkan1.2', '-S', 'comp',
                      '-DSAMPLE_BITS=' + bits, '-DSAMPLES_X=' + gpu_hash_samples_x,
                      '@INPUT@', '-o', '@OUTPUT@'],
        )
    endforeach

    sources += custom_target('hashexport_spv.h',
        input: spv,
        output: 'hashexport_spv.h',
        command: [py, files('tools/spv2c.py'), '@OUTPUT@',
                  'BSHashExportSpv8=@INPUT0@', 'BSHashExportSpv16=@INPUT1@'],
    )

    cpp_args += '-DBS_GPU_HASH=1'
    cpp_args += '-DBS_GPU_HASH_SAMPLES_X=' + gpu_hash_samples_x
endif

if enable_plugin
    # VSVulkan4.h is written against Vulkan 1.4: it declares VS_VULKAN_API_VERSION as
    # VK_API_VERSION_1_4 and names vkMapMemory2/vkUnmapMemory2, which are core only from 1.4 having
    # been promoted out of VK_KHR_map_memory2. Older headers fail several levels deep inside the
    # macro that builds the function pointer table, which says nothing about the real problem, so
    # the version is checked up front. Only the plugin includes that header; a library only build
    # with GPU hashing is happy with older ones.
    if enable_gpu_hash and not cc.has_header_symbol('vulkan/vulkan.h', 'VK_API_VERSION_1_4', dependencies: libavutil_dep)
        error('The VapourSynth GPU frame API needs vulkan headers 1.4 or newer, the ones found are older')
    endif

    # Needs the API 4.3 declarations the GPU frame path is written against, which VapourSynth
    # ships from R80a1 onwards.
    incdir += include_directories(
        run_command(py, '-c', 'import vapoursynth as vs; print(vs.get_include())', check: true).stdout().strip(),
    )

    sources += files(
        'src/synthshared.cpp',
        'src/vapoursynth.cpp',
        'src/vsgpuexport.cpp',
    )

    # AviSynth+ is entered through a C++ interface rather than a C one, so a build whose C++ ABI
    # does not match the host AviSynth+ can never be loaded by it. Leave the interface out entirely
    # in that case instead of exporting an AvisynthPluginInit3 that cannot work.
    if get_option('enable_avisynth')
        deps += dependency('avisynthplus')

        sources += files(
            'src/avisynth.cpp',
        )
    endif
endif

if cc.get_linker_id() in ['ld.bfd', 'ld.gold', 'ld.mold']
    link_args += '-Wl,-Bsymbolic'
endif

bestsource = library('bestsource',
    sources,
    cpp_args: cpp_args,
    dependencies: deps,
    include_directories: incdir,
    install: true,
    install_dir: enable_plugin ? py.get_install_dir() / 'vapoursynth/plugins' : [],
    link_args: link_args,
)

if not enable_plugin
    install_headers(
        files(
            'src/audiosource.h',
            'src/bsshared.h',
            'src/tracklist.h',
            'src/version.h',
            'src/videosource.h',
        ),
        subdir: 'bestsource',
    )

    pkg = import('pkgconfig')
    pkg.generate(bestsource,
        description: 'A super great audio/video source and FFmpeg wrapper',
        filebase: 'bestsource',
        subdirs: 'bestsource',
    )
endif

bestsource_dep = declare_dependency(
    include_directories: include_directories('src'),
    link_with: bestsource,
)
meson.override_dependency('bestsource', bestsource_dep)
