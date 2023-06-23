project('BestSource', 'cpp',
    default_options: ['buildtype=release', 'b_ndebug=if-release', 'cpp_std=c++14'],
    meson_version: '>=0.48.0'
)

libs = []

sources = [
    'src/audiosource.cpp',
    'src/videosource.cpp',
    'src/SrcAttribCache.cpp',
    'src/BSRational.cpp',
    'src/vapoursynth.cpp'
]

p2p_args = []

if host_machine.cpu_family().startswith('x86')
    p2p_args += ['-DP2P_SIMD']
endif

libs += static_library('p2p_main',
    [
        'libp2p/p2p_api.cpp',
        'libp2p/v210.cpp',
        'libp2p/simd/cpuinfo_x86.cpp',
        'libp2p/simd/p2p_simd.cpp'
    ],
    include_directories: ['libp2p', 'libp2p/simd'],
    cpp_args: p2p_args
)

if host_machine.cpu_family().startswith('x86')
    p2p_args += ['-msse4.1']

    libs += static_library('p2p_sse41',
        'libp2p/simd/p2p_sse41.cpp',
        include_directories: ['libp2p', 'libp2p/simd'],
        cpp_args: p2p_args
    )
endif

vapoursynth_dep = dependency('vapoursynth', version: '>=R55').partial_dependency(compile_args: true, includes: true)
jansson_dep = dependency('jansson', version: '>= 2.7', required: true)

deps = [
    vapoursynth_dep,
    jansson_dep,
    dependency('libavcodec', version: '>=59.37.0'),
    dependency('libavformat', version: '>=59.27.0'),
    dependency('libavutil', version: '>=57.28.0'),
    dependency('libswscale', version: '>=6.7.0')
]

shared_module('bestsource', sources,
    dependencies: deps,
    link_with: libs,
    install: true,
    install_dir: join_paths(vapoursynth_dep.get_pkgconfig_variable('libdir'), 'vapoursynth'),
    gnu_symbol_visibility: 'hidden'
)
