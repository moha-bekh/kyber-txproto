# Base files
sources = [
    # Lua
    builtin_lua_scripts,
    'lua_common.c',
    'lua_api.c',
    'lua_generic_api.c',

    # Sources
    'iosys_common.c',

    # Muxing
    'mux.c',

    # Demuxing
    'demux.c',

    # Filtering
    'filter.c',
    'filter_proc.c',

    # Encoding
    'encode.c',

    # Decoding
    'decode.c',

    # Packet sink
    'packet_sink.c',

    # Metrics
    'metrics.c',

    # Misc
    'utils.c',
    'log.c',
    'fifo_bufferref.c',
    'fifo_frame.c',
    'fifo_packet.c',
    'os_compat.c',
    'ctrl_template.c',
    'net.c',
    'epoch.c',
    'commit.c',
    'control.c',
    'link.c',
    'io.c',
    'txproto.c',

    # Version
    vcs_tag(command: ['git', 'rev-parse', '--short', 'HEAD'],
                      input: 'version.c.in', output: 'version.c',
                      fallback: 'release')
]

# Public headers
headers = [
    'txproto_main.h',
    'mux.h',
    'demux.h',
    'filter.h',
    'filter_proc.h',
    'encode.h',
    'decode.h',
    'log.h',
    'fifo_frame.h',
    'fifo_packet.h',
    'events.h',
    'bufferlist.h',
    'utils.h',
    'epoch.h',
    'commit.h',
    'control.h',
    'link.h',
    'io.h',
    'metrics.h',
    'txproto.h',
]

# libendit
libedit = dependency('libedit', version: '>= 3.1', required: get_option('libedit'))
if libedit.found()
    dependencies += libedit
    sources += 'cli.c'
    conf.set('HAVE_LIBEDIT', 1)
    features += ', ' + libedit.name() + ' ' + libedit.version()
endif

# libplacebo (required by the watermark filter)
libplacebo = dependency('libplacebo', version: '>= 3.120.0',
    required: get_option('watermark'),
    allow_fallback: host_machine.system() != 'darwin') # darwin: don't force-build GPU stack from wraps
if libplacebo.found()
    dependencies += libplacebo
    conf.set('HAVE_WATERMARK', 1)
    sources += 'watermark.c'
endif

vulkan = dependency('vulkan', version: '>= 1.1', required: false,
    allow_fallback: host_machine.system() != 'darwin') # darwin: no native Vulkan, skip wrap fallback
if vulkan.found()
    dependencies += vulkan
    conf.set('HAVE_VULKAN', 1)
    if libplacebo.found() and host_machine.system() != 'windows'
        sources += 'watermark_vulkan.c'
    endif
endif

have_interface = libplacebo.found() and vulkan.found() and get_option('interface').enabled()
if have_interface
    sources += 'interface.c'
    sources += 'interface_common.c'
    sources += 'pl.c'
    conf.set('HAVE_INTERFACE', 1)
endif

# libavdevice
libavdevice = dependency('libavdevice', version: '>= 58.9.100', required: get_option('libavdevice'))
if libavdevice.found()
    dependencies += libavdevice
    sources += 'iosys_lavd.c'
    conf.set('HAVE_LAVD', 1)
    features += ', ' + libavdevice.name() + ' ' + libavdevice.version()
endif

# libswscale
libswscale = dependency('libswscale', required: true)
if libswscale.found()
    dependencies += libswscale
    features += ', ' + libswscale.name() + ' ' + libswscale.version()
endif

# Pulseaudio
libpulse = dependency('libpulse', version: '>= 0.29.0', required: get_option('pulse'))
if libpulse.found()
    dependencies += libpulse
    sources += 'iosys_pulse.c'
    conf.set('HAVE_PULSEAUDIO', 1)
    features += ', ' + libpulse.name() + ' ' + libpulse.version()
endif

# xcb
have_xcb = false
libxcb = dependency('xcb', version: '>= 1.14', required: get_option('xcb'))
if libxcb.found()
    libxcb_shm = dependency('xcb-shm', version: '>= 1.14', required: get_option('xcb'))
    libxcb_randr = dependency('xcb-randr', version: '>= 1.14', required: get_option('xcb'))
    if libxcb_shm.found() and libxcb_randr.found()
        dependencies += libxcb
        dependencies += libxcb_shm
        dependencies += libxcb_randr
        conf.set('HAVE_XCB', 1)
        features += ', ' + 'xcb' + ' ' + libxcb.version()
        sources += 'iosys_xcb.c'
        have_xcb = true
    endif
endif

# Wayland
wayland_scanner = find_program('wayland-scanner', required: get_option('wayland'))
wayland_deps = [ # Reorder with care, wayland_protocols expects them like this
    dependency('wayland-client', required: get_option('wayland')),
    dependency('wayland-cursor', required: get_option('wayland')),
    dependency('wayland-protocols', version: '>=1.14', required: get_option('wayland')),
    dependency('xkbcommon', version: '>= 0.3.0', required: get_option('wayland')),
    dependency('libdrm', version: '>=2.4.95', required: get_option('wayland')),
]

have_wayland = wayland_deps.length() == 5 and wayland_scanner.found() and has_memfd
if have_wayland
    subdir('wayland_protocols')
    dependencies += wayland_client_protos
    dependencies += wayland_deps.get(3)
    dependencies += wayland_deps.get(4)
    sources += 'iosys_wayland.c'
    sources += 'wayland_common.c'

    if have_interface
        sources += 'interface_wayland.c'
    endif

    conf.set('HAVE_WAYLAND', 1)
    features += ', ' + 'wayland' + ' ' + wayland_deps.get(0).version()

    gbm_dep = dependency('gbm', version: '>=17.1.0', required: false)
    if gbm_dep.found()
        dependencies += gbm_dep
        conf.set('HAVE_GBM', 1)
    endif
endif

# DXGI
lib_d3d11 = cc.find_library('d3d11', required: false)
lib_dxgi = cc.find_library('dxgi', required: false)
have_dxgi = lib_d3d11.found() and lib_dxgi.found()
if have_dxgi
    dependencies += lib_d3d11
    dependencies += lib_dxgi
    sources += 'dxgi_cursor.c'
    sources += 'iosys_dxgi.c'
    conf.set('HAVE_DXGI', 1)
    conf.set('HAVE_D3D11', 1)
endif

# WASAPI
lib_ole32 = cc.find_library('ole32', required: false)
lib_ksuser = cc.find_library('ksuser', required: false)
have_wasapi = lib_ole32.found() and lib_ksuser.found()
if have_wasapi
    dependencies += lib_ole32
    dependencies += lib_ksuser
    sources += 'iosys_wasapi.c'
    conf.set('HAVE_WASAPI', 1)
endif

# NVFBC
libnvcodec = dependency('ffnvcodec', required: false,
    allow_fallback: host_machine.system() != 'darwin') # darwin: skip wrap fallback (nvfbc unused)
have_nvfbc = libnvcodec.found() and have_xcb
if have_nvfbc
    sources += 'iosys_nvfbc.c'
    conf.set('HAVE_NVFBC', 1)
    dependencies += libnvcodec
endif

# DRM
libdrm = dependency('libdrm', required: false)
libcap = dependency('libcap', required: false)
libgbm = dependency('gbm', required: false)
libegl = dependency('egl', required: false)
libopengl = dependency('GL', required: false)

libva = dependency('libva', required: false)
libva_drm = dependency('libva-drm', required: false)

have_drm = libdrm.found() and libcap.found() and libgbm.found() and libegl.found() and libopengl.found()
have_vaapi_drm = libva.found() and libva_drm.found()
if have_drm
    dependencies += libdrm
    dependencies += libcap
    dependencies += libgbm
    dependencies += libegl
    dependencies += libopengl
    if have_vaapi_drm
        dependencies += libva
        dependencies += libva_drm
        conf.set('HAVE_VAAPI_DRM', 1)
    endif
    sources += [
        'iosys_drm.c',
        'iosys_drm_wrapper.c',
        'iosys_drm_gl.c',
    ]
    conf.set('HAVE_DRM', 1)
endif

link_args = []

if host_machine.system() == 'windows'
    link_args += ['-static-libgcc', '-Wl,-Bstatic', '-lpthread', '-Wl,-Bdynamic']

    # When any static archive linked into a DLL contains __declspec(dllexport)
    # symbols (e.g. libplacebo built static with -DPL_EXPORT), MinGW switches
    # from auto-export mode to explicit-export mode, hiding libtxproto's own
    # symbols.  --export-all-symbols restores the auto-export behavior.
    # --exclude-libs then suppresses re-export of third-party archives so that
    # downstream consumers linking those archives statically themselves don't
    # hit `multiple definition` errors.
    link_args += [
        '-Wl,--export-all-symbols',
        '-Wl,--exclude-libs=libavcodec.a',
        '-Wl,--exclude-libs=libavformat.a',
        '-Wl,--exclude-libs=libavutil.a',
        '-Wl,--exclude-libs=libavfilter.a',
        '-Wl,--exclude-libs=libavdevice.a',
        '-Wl,--exclude-libs=libswresample.a',
        '-Wl,--exclude-libs=libswscale.a',
        '-Wl,--exclude-libs=libplacebo.a',
    ]

    lib_ws2_32 = cc.find_library('ws2_32', required: true)
    dependencies += lib_ws2_32
endif

conf.set('HAVE_SOCK_CLOEXEC', host_machine.system() != 'windows' and
                              cc.has_header_symbol('sys/socket.h', 'SOCK_CLOEXEC'))

inc = include_directories('./include')
lib = library('txproto', sources,
    install: true,
    dependencies: dependencies,
    link_args: link_args,
    soversion: ver_major,
    include_directories: inc,
)

libtxproto = declare_dependency(
    link_with: lib,
    include_directories: inc,
)

# Allows projects to build libavstream by cloning into ./subprojects/libtxproto
meson.override_dependency('libtxproto', libtxproto)

# Install headers
foreach h : headers
    parts = h.split('/')
    path = 'lib'+meson.project_name()
    foreach p : parts
        if p != parts[-1]
            path = path / p
        endif
    endforeach

    install_headers('include' / 'lib'+meson.project_name() / h, subdir: path)
endforeach

pkg = import('pkgconfig')
pkg.generate(
    name: meson.project_name(),
    description: 'Scriptable multimedia framework',
    libraries: lib,
    version: meson.project_version(),
)

# executable
if get_option('cli').enabled()
    cli_deps = dependencies
    cli_deps += libtxproto
    cli = executable('txproto',
        install: true,
        sources: cli_lua_scripts + 'txproto_main.c',
        dependencies: cli_deps,
    )
endif

summary({
    'CLI': libedit.found(),
    'Wayland': have_wayland,
    'Vulkan GUI': have_interface,
}, section: 'Interface', bool_yn: true)

summary({
    'pulse': libpulse.found(),
    'xcb': have_xcb,
    'wayland': have_wayland,
    'dxgi': have_dxgi,
    'wasapi': have_wasapi,
    'drm': have_drm,
    'nvfbc': have_nvfbc,
    'libavdevice': libavdevice.found(),
}, section: 'I/O systems', bool_yn: true)

summary({
    'drm': have_drm,
    'libdrm': libdrm.found(),
    'libcap': libcap.found(),
    'libgbm': libgbm.found(),
    'libegl': libegl.found(),
    'libopengl': libopengl.found(),
    'libva': libva.found(),
    'libva-drm': libva_drm.found(),
}, section: 'DRM capture', bool_yn: true)

summary({
    'nvfbc': have_nvfbc,
    'nvcodec': libnvcodec.found(),
    'xcb': have_xcb,
}, section: 'NVFBC capture', bool_yn: true)

test('test1', cli, args : ['-V', 'trace', '-s', '../test/transcode_audio.lua', '-r', 'io,package', '/tmp/testa.flac', '/tmp/resulta.flac'], env : ['LUA_PATH=../test/common.lua'])
#test('test1', cli, args : ['-V', 'trace', '-s', '../test/transcode_video.lua', '-r', 'io,package', '/tmp/testv.mkv', '/tmp/resultv.mkv'], env : ['LUA_PATH=../test/common.lua'])
