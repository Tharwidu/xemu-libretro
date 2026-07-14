nv2a_vsh_cpu_opts = cmake.subproject_options()
nv2a_vsh_cpu_opts.add_cmake_defines({'CMAKE_POSITION_INDEPENDENT_CODE': get_option('libretro')})
nv2a_vsh_cpu_opts.add_cmake_defines({
  'CMAKE_OSX_ARCHITECTURES': cmake_macos_arch,
  'nv2a_vsh_cpu_UNIT_TEST': 'OFF',
  })
nv2a_vsh_cpu_subproj = cmake.subproject('nv2a_vsh_cpu', options: nv2a_vsh_cpu_opts)
nv2a_vsh_cpu = declare_dependency(include_directories: nv2a_vsh_cpu_subproj.include_directories('nv2a_vsh_emulator'),
                                  link_with: [
                                    nv2a_vsh_cpu_subproj.target('nv2a_vsh_emulator'),
                                    nv2a_vsh_cpu_subproj.target('nv2a_vsh_cpu'),
                                    nv2a_vsh_cpu_subproj.target('nv2a_vsh_disassembler'),
                                    ])

if get_option('libretro')
  # Both platform backends guard themselves (_WIN32 / !_WIN32): WGL on
  # Windows, EGL+GLX (via epoxy) elsewhere. X11 provides the GLX path;
  # without it the POSIX backend is EGL-only.
  glo_libretro_deps = []
  if host_os != 'windows'
    glo_libretro_deps += epoxy
    glo_x11 = dependency('x11', method: 'pkg-config', required: false)
    if glo_x11.found()
      glo_libretro_deps += glo_x11
    endif
  endif
  libgloffscreen = static_library('libgloffscreen',
                            sources: files('gloffscreen/common.c',
                                           'gloffscreen/gloffscreen_libretro.c',
                                           'gloffscreen/gloffscreen_libretro_posix.c'),
                            c_args: ['-DLIBRETRO=1'],
                            dependencies: glo_libretro_deps)
else
  libgloffscreen = static_library('libgloffscreen',
                            sources: files('gloffscreen/common.c', 'gloffscreen/sdl.c'),
                            dependencies: sdl)
endif
gloffscreen = declare_dependency(link_with: libgloffscreen,
                           include_directories: ['gloffscreen'])
