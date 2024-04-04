project('BestSource', 'cpp',
    default_options: ['buildtype=release', 'b_ndebug=if-release', 'cpp_std=c++17'],
    license: 'MIT',
    meson_version: '>=0.53.0',
    version: '.'.join([
        run_command('grep', 'BEST_SOURCE_VERSION_MAJOR', 'src/version.h', check: true).stdout().strip().split()[2],
        run_command('grep', 'BEST_SOURCE_VERSION_MINOR', 'src/version.h', check: true).stdout().strip().split()[2],
    ])
)

link_static = get_option('link_static')

api_sources = files(
    'src/audiosource.cpp',
    'src/bsshared.cpp',
    'src/tracklist.cpp',
    'src/videosource.cpp',
)

api_headers = files(
    'src/audiosource.h',
    'src/bsshared.h',
    'src/tracklist.h',
    'src/version.h',
    'src/videosource.h',
)

plugin_sources = files(
    'src/avisynth.cpp',
    'src/synthshared.cpp',
    'src/vapoursynth.cpp',
)

libs = []
p2p_args = []

if host_machine.cpu_family().startswith('x86')
    p2p_args += ['-DP2P_SIMD']
endif

libs += static_library('p2p_main',
    files(
        'libp2p/p2p_api.cpp',
        'libp2p/v210.cpp',
        'libp2p/simd/cpuinfo_x86.cpp',
        'libp2p/simd/p2p_simd.cpp',
    ),
    cpp_args: p2p_args,
    gnu_symbol_visibility: 'hidden',
)

if host_machine.cpu_family().startswith('x86')
    p2p_args += ['-msse4.1']

    libs += static_library('p2p_sse41', files('libp2p/simd/p2p_sse41.cpp'),
        cpp_args: p2p_args,
        gnu_symbol_visibility: 'hidden',
    )
endif

deps = [
    dependency('libavcodec', version: '>=60.31.0', static: link_static),
    dependency('libavformat', version: '>=60.16.0', static: link_static),
    dependency('libavutil', version: '>=58.29.0', static: link_static),
    dependency('libxxhash', static: link_static),
]

is_gnu_linker = meson.get_compiler('cpp').get_linker_id() in ['ld.bfd', 'ld.gold', 'ld.mold']
link_args = []

if host_machine.system() == 'windows' and is_gnu_linker and link_static
    link_args += ['-static']
elif is_gnu_linker
    link_args += ['-Wl,-Bsymbolic']
endif

libbestsource = library('libbestsource', api_sources,
    dependencies: deps,
    gnu_symbol_visibility: 'hidden',
    install: true,
    link_args: link_args,
    link_with: libs,
    name_prefix: '',
)

install_headers(api_headers,
    subdir: 'bestsource',
)

bestsource_dep = declare_dependency(
    include_directories: include_directories('src'),
    link_with: libbestsource,
)

if get_option('enable_plugin')
    vapoursynth_dep = dependency('vapoursynth', version: '>=55').partial_dependency(compile_args: true, includes: true)

    shared_module('bestsource', plugin_sources,
        dependencies: [bestsource_dep, vapoursynth_dep],
        gnu_symbol_visibility: 'hidden',
        install: true,
        install_dir: vapoursynth_dep.get_variable(pkgconfig: 'libdir') / 'vapoursynth',
        link_args: link_args,
        name_prefix: '',
    )
endif
