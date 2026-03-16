project('BestSource', 'cpp',
    default_options: ['buildtype=release', 'b_ndebug=if-release', 'cpp_std=c++17'],
    license: 'MIT',
    meson_version: '>=1.2.3',
    version: '16.0',
)

enable_plugin = get_option('enable_plugin')

sources = files(
    'src/audiosource.cpp',
    'src/bsshared.cpp',
    'src/tracklist.cpp',
    'src/videosource.cpp',
)

if enable_plugin
    sources += files(
        'src/avisynth.cpp',
        'src/synthshared.cpp',
        'src/vapoursynth.cpp',
    )
endif

deps = [
    dependency('libavcodec', version: '>=61.19.0'),
    dependency('libavformat', version: '>=61.7.0'),
    dependency('libavutil', version: '>=59.39.0'),
    dependency('libxxhash'),
]

libs = []

link_args = []

p2p_args = []

py = import('python').find_installation(pure: false)

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
    p2p_args += '-march=x86-64-v2'

    libs += static_library('p2p_sse41',
        files('libp2p/simd/p2p_sse41.cpp'),
        cpp_args: p2p_args,
        gnu_symbol_visibility: 'hidden',
    )
endif

if meson.get_compiler('cpp').get_linker_id() in ['ld.bfd', 'ld.gold', 'ld.mold']
    link_args += '-Wl,-Bsymbolic'
endif

bestsource = library('bestsource',
    sources,
    dependencies: deps,
    include_directories: include_directories('vapoursynth/include'),
    install: true,
    install_dir: enable_plugin ? py.get_install_dir() / 'vapoursynth/plugins' : get_option('libdir'),
    link_args: link_args,
    link_with: libs,
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
