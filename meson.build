project('BestSource', 'cpp',
  default_options : ['buildtype=release', 'b_ndebug=if-release', 'cpp_std=c++14'],
  meson_version : '>=0.48.0'
)

libs = []

sources = [
  'src/audiosource.cpp',
  'src/audiosource.h',
  'src/videosource.cpp',
  'src/videosource.h',
  'src/SrcAttribCache.cpp',
  'src/SrcAttribCache.h',
  'src/BSRational.cpp',
  'src/BSRational.h',
  'src/vapoursynth.cpp'
]

libs += static_library('p2p_main',
    [
        'libp2p/p2p_api.cpp',
        'libp2p/v210.cpp',
        'libp2p/simd/cpuinfo_x86.cpp',
        'libp2p/simd/p2p_simd.cpp'
    ],
    include_directories: ['libp2p', 'libp2p/simd'],
    cpp_args: ['-std=c++14']
)

if host_machine.cpu_family().startswith('x86')
    libs += static_library('p2p_sse41',
        'libp2p/simd/p2p_sse41.cpp',
        include_directories: ['libp2p', 'libp2p/simd'],
        cpp_args: [
          '-std=c++14',
          '-DP2P_SIMD',
          '-msse4.1'
        ]
    )
endif

vapoursynth_dep = dependency('vapoursynth', version: '>=R55').partial_dependency(compile_args: true, includes: true)
jansson_dep = dependency('jansson', version: '>= 2.7', required: true)

deps = [
  vapoursynth_dep,
  jansson_dep,
  dependency('libavcodec', version : '>=58.18.0'),
  dependency('libavformat', version : '>=58.12.0'),
]

shared_module('bestsource', sources,
  dependencies : deps,
  link_with: libs,
  install : true,
  install_dir : join_paths(vapoursynth_dep.get_pkgconfig_variable('libdir'), 'vapoursynth'),
  gnu_symbol_visibility : 'hidden'
)
