<h1 align="center">QEMU Render<br />
<div align="center">
  
[![Build]][build_url]
[![Version]][release_url]
[![Size]][release_url]

</div></h1>

Run QEMU OpenGL on headless Debian hosts without installing an Xorg server or desktop environment.

`qemu-render` provides the host-side  VirGL/Venus and Mesa runtime needed by Debian's official QEMU OpenGL module. It keeps hardware-accelerated EGL/GBM rendering and VirGL/Venus available on servers, containers, and appliance-style systems while leaving out runtime components that are unnecessary for this use case.

## Features ✨

- Designed for headless Debian hosts with no Xorg server or desktop environment
- Hardware-accelerated EGL and GBM rendering through Mesa on Intel and AMD GPUs
- Supports the Mesa `i915`, `crocus`, `iris`, `r600`, and `radeonsi` Gallium drivers
- Custom virglrenderer with VirGL, Venus, and `virgl_render_server` support
- QXL support through a reduced SPICE server runtime for QEMU's VNC display path

## Package design 📦

The package version-provides:

```text
libgbm1
libegl-mesa0
libspice-server1
libvirglrenderer1
```

and conflicts with/replaces Debian's stock packages with those names. It also conflicts with/replaces `virgl-server` because `qemu-render` ships its own `virgl_render_server`. On a headless host, this lets Debian's official QEMU modules satisfy their normal runtime dependencies without installing the broader stock Mesa Gallium/LLVM and SPICE multimedia runtime stacks.

## Mesa runtime 🎨

The Mesa portion contains:

- `i915` for older Intel GPUs
- `crocus` for older Intel generations supported by Gallium Crocus
- `iris` for newer Intel GPUs
- `r600` for older AMD Radeon GPUs based on TeraScale
- `radeonsi` for newer AMD Radeon GPUs based on GCN and RDNA
- EGL and GBM for headless rendering

LLVM is available only while compiling Mesa build-time tools. The final Mesa runtime is built with both `-Dllvm=disabled` and `-Damd-use-llvm=false`, and the finished package is verified to contain no direct or transitive LLVM runtime dependency.

## VirGL and Venus runtime 🎮

The virglrenderer portion provides `libvirglrenderer.so.1` for QEMU's normal VirGL path and also builds Venus support together with `virgl_render_server`.

The renderer is built with:

```text
-Dplatforms=egl
-Dvenus=true
-Drender-server-worker=thread
-Dunstable-apis=true
-Dtests=false
-Dvideo=false
```

The `thread` worker setting applies to the external render-server path used by Venus; it does not change QEMU's normal in-process VirGL renderer. Thread workers are used so all Venus contexts in the render-server process share the device-memory budget accounting.

## Stars 🌟
[![Stargazers](https://raw.githubusercontent.com/star-stats/stars/refs/heads/data/charts/qemus-qemu-render.svg)](https://github.com/qemus/qemu-render/stargazers)

[build_url]: https://github.com/qemus/qemu-render/
[release_url]: https://github.com/qemus/qemu-render/releases/

[Build]: https://github.com/qemus/qemu-render/actions/workflows/build.yml/badge.svg
[Size]: https://img.shields.io/badge/size-18.4_MB-steelblue?style=flat&color=066da5
[Version]: https://img.shields.io/github/v/tag/qemus/qemu-render?label=version&sort=semver&color=066da5
