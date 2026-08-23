<h1 align="center">Helios<br />
<div align="center">
  
[![Build]][build_url]
[![Version]][release_url]
[![Size]][release_url]

</div></h1>

Custom QEMU build with patches for accelerated Windows graphics.

## Design 📦

The binary is based on upstream QEMU 11.1.0 plus the patch and source files stored directly in this repository.

QEMU loadable modules are disabled for this build. OpenGL/VirGL support is compiled into the executable so the custom binary does not depend on Debian's version-matched QEMU module files. Runtime graphics libraries remain dynamically linked and are supplied by the normal QEMU environment.

## Patch stack 🛠️

The build carries these changes on top of QEMU 11.1.0:

- Native Venus optimal scanout support
- SDL compositor EGL context validation
- Modifier-backed Venus scanout reconstruction
- Helios scanout tracing
- Optional HOST3D blob budgeting
- Non-power-of-two virtio-gpu host memory sizes
- Vulkan scanout publication pacing
- Adaptive VNC lossy-damage coalescing

## Acknowledgements 🙏

Special thanks to the [WinBoat](https://github.com/winboat-org/winboat) team, this project would not exist without their invaluable work.

[build_url]: https://github.com/qemus/qemu-helios/
[release_url]: https://github.com/qemus/qemu-helios/releases/

[Build]: https://github.com/qemus/qemu-helios/actions/workflows/build.yml/badge.svg
[Size]: https://img.shields.io/badge/size-18.4_MB-steelblue?style=flat&color=066da5
[Version]: https://img.shields.io/github/v/tag/qemus/qemu-helios?label=version&sort=semver&color=066da5
