project('BestSource', 'cpp',
    default_options: ['buildtype=release', 'b_lto=true', 'b_ndebug=if-release', 'cpp_std=c++14'],
    license: 'MIT',
    meson_version: '>=0.53.0',
    version: '0.9'
)

link_static = get_option('link_static')

sources = [
    'src/audiosource.cpp',
    'src/BSRational.cpp',
    'src/BSShared.cpp',
    'src/SrcAttribCache.cpp',
    'src/vapoursynth.cpp',
    'src/videosource.cpp'
]

libs = []
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
    cpp_args: p2p_args,
    gnu_symbol_visibility: 'hidden'
)

if host_machine.cpu_family().startswith('x86')
    p2p_args += ['-msse4.1']

    libs += static_library('p2p_sse41', 'libp2p/simd/p2p_sse41.cpp',
        cpp_args: p2p_args,
        gnu_symbol_visibility: 'hidden'
    )
endif

vapoursynth_dep = dependency('vapoursynth', version: '>=55').partial_dependency(compile_args: true, includes: true)

deps = [
    vapoursynth_dep,
    dependency('jansson', version: '>=2.12', static: link_static),
    dependency('libavcodec', version: '>=59.37.0', static: link_static),
    dependency('libavformat', version: '>=59.27.0', static: link_static),
    dependency('libavutil', version: '>=57.28.0', static: link_static),
    dependency('libswscale', version: '>=6.7.0', static: link_static)
]

is_gnu_linker = meson.get_compiler('cpp').get_linker_id() in ['ld.bfd', 'ld.gold', 'ld.mold']
link_args = []

if host_machine.system() == 'windows' and is_gnu_linker and link_static
    link_args += ['-static']
elif is_gnu_linker
    link_args += ['-Wl,-Bsymbolic']
endif

shared_module('bestsource', sources,
    dependencies: deps,
    gnu_symbol_visibility: 'hidden',
    install: true,
    install_dir: vapoursynth_dep.get_variable(pkgconfig: 'libdir') / 'vapoursynth',
    link_args: link_args,
    link_with: libs
)
