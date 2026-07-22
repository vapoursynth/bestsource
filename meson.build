project('BestSource', 'cpp',
    default_options: ['buildtype=release', 'b_ndebug=if-release', 'cpp_std=c++17'],
    license: 'MIT',
    license_files: 'LICENSE',
    meson_version: '>=1.2.3',
    version: '20.0rc2',
)

enable_plugin = get_option('enable_plugin')

deps = [
    dependency('libavcodec', version: '>=62.11.0'),
    dependency('libavformat', version: '>=62.3.0'),
    dependency('libavutil', version: '>=60.8.0'),
    dependency('libp2p'),
    dependency('libxxhash'),
]

incdir = []

link_args = []

py = import('python').find_installation(pure: false)

sources = files(
    'src/audiosource.cpp',
    'src/bsshared.cpp',
    'src/tracklist.cpp',
    'src/videosource.cpp',
)

if enable_plugin
    incdir += include_directories(
        run_command(py, '-c', 'import vapoursynth as vs; print(vs.get_include())', check: true).stdout().strip(),
    )

    sources += files(
        'src/synthshared.cpp',
        'src/vapoursynth.cpp',
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

if meson.get_compiler('cpp').get_linker_id() in ['ld.bfd', 'ld.gold', 'ld.mold']
    link_args += '-Wl,-Bsymbolic'
endif

bestsource = library('bestsource',
    sources,
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
