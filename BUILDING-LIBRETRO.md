# Building the xemu libretro core

The core is built from the normal xemu tree with `-Dlibretro=true`.
The output is a single self-contained core: `xemu_libretro.dll`
(Windows), `xemu_libretro.so` (Linux).

## Linux (native)

Install the usual xemu build dependencies for your distro (see the
upstream xemu build docs), including `sdl3` development headers. On
Fedora/Nobara:

```
sudo dnf install gcc gcc-c++ meson ninja-build glib2-devel pixman-devel \
    libepoxy-devel vulkan-headers vulkan-loader-devel sdl3-devel \
    libX11-devel cmake curl
```

Then:

```
mkdir build-libretro && cd build-libretro
../configure --extra-cflags="-DXBOX=1" --target-list=i386-softmmu \
    -Dlibretro=true -Db_staticpic=true
ninja xemu_libretro.so
```

Notes:

- `-Db_staticpic=true` is required: the core is a shared library, so
  the static helper libraries must be built as position-independent
  code. The build stops with an explanatory error if it is missing.
- If `curl` is unavailable, pre-download the `dsp56300` prebuilt
  archive for your platform from
  `https://github.com/mborgerson/dsp56300/releases` into
  `subprojects/dsp56300/` and the build will pick it up.
- Without X11 development headers the GL offscreen backend is
  EGL-only, which is sufficient for Wayland and EGL-based frontends.

Copy `xemu_libretro.so` into RetroArch's `cores` directory, and place
the usual xemu system files (BIOS, MCPX ROM, `xbox_hdd.qcow2`) in
`<retroarch>/system/xemu/`.

## Windows (cross-compile, same containers as xemu CI)

```
podman run --rm -v $PWD:/xemu -w /xemu \
    ghcr.io/xemu-project/xemu-win64-toolchain-gcc:latest \
    bash -c "./build.sh -p win64-cross -Dlibretro=true; ninja -C build xemu_libretro.dll"
```

`build.sh` currently exits with an error after configuring (it tries
to build the standalone executable target); the subsequent direct
`ninja` invocation builds the core.

## Windows (MSYS2)

From a MINGW64 shell with the xemu dependencies installed, the same
`configure`/`ninja` flow as Linux applies (the static glslang
libraries available in MSYS2 are picked up automatically; elsewhere
the build falls back to the bundled subproject).
