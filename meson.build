project('BestSource', 'cpp',
    default_options: ['buildtype=release', 'b_ndebug=if-release', 'cpp_std=c++17'],
    license: 'MIT',
    meson_version: '>=0.58.0',
    version: '16.0',
)

sources = files(
    'src/audiosource.cpp',
    'src/bsshared.cpp',
    'src/tracklist.cpp',
    'src/videosource.cpp',
)

headers = files(
    'src/audiosource.h',
    'src/bsshared.h',
    'src/tracklist.h',
    'src/version.h',
    'src/videosource.h',
)

deps = []
install_dir = get_option('libdir')
libs = []
link_args = []
p2p_args = []

if get_option('enable_plugin')
    sources += files(
        'src/avisynth.cpp',
        'src/synthshared.cpp',
        'src/vapoursynth.cpp',
    )

    vapoursynth_dep = dependency('vapoursynth', version: '>=55').partial_dependency(compile_args: true, includes: true)
    deps += vapoursynth_dep

    install_dir = vapoursynth_dep.get_variable('libdir') / 'vapoursynth'
endif

deps += [
    dependency('libavcodec', version: '>=61.19.0'),
    dependency('libavformat', version: '>=61.7.0'),
    dependency('libavutil', version: '>=59.39.0'),
    dependency('libxxhash'),
]

if host_machine.cpu_family().startswith('x86')
    p2p_args += '-DP2P_SIMD'
endif

libs += static_library('p2p_main',
    files(
        'libp2p/simd/cpuinfo_x86.cpp',
        'libp2p/simd/p2p_simd.cpp',
        'libp2p/p2p_api.cpp',
        'libp2p/v210.cpp',
    ),
    cpp_args: p2p_args,
    gnu_symbol_visibility: 'hidden',
)

if host_machine.cpu_family().startswith('x86')
    p2p_args += '-msse4.1'

    libs += static_library('p2p_sse41',
        files('libp2p/simd/p2p_sse41.cpp'),
        cpp_args: p2p_args,
        gnu_symbol_visibility: 'hidden',
    )
endif

if meson.get_compiler('cpp').get_linker_id() in ['ld.bfd', 'ld.gold', 'ld.mold']
    link_args += ['-Wl,-Bsymbolic']
endif

bestsource = library('bestsource',
    sources,
    dependencies: deps,
    install: true,
    install_dir: install_dir,
    link_args: link_args,
    link_with: libs,
)

install_headers(headers,
    subdir: 'bestsource',
)

pkg = import('pkgconfig')
pkg.generate(bestsource,
    description: 'A super great audio/video source and FFmpeg wrapper',
    filebase: 'bestsource',
    install_dir: get_option('libdir') / 'pkgconfig',
    subdirs: 'bestsource',
)

bestsource_dep = declare_dependency(
    include_directories: include_directories('src'),
    link_with: bestsource,
)
