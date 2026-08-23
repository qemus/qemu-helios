# syntax=docker/dockerfile:1

FROM debian:trixie-slim AS builder

ARG VERSION_ARG="0.0"
ARG MESA_VERSION="25.0.7"
ARG SPICE_VERSION="0.16.0"
ARG VIRGL_VERSION="1.3.0"
ARG VIRGL_REF="986b5fc57b07c06b5e0b3a3694d06898ebc80163"
ARG DEBIAN_SNAPSHOT="20260819T142328Z"
ARG DEBIAN_FRONTEND="noninteractive"

RUN <<EOF_BUILD_DEPS
  set -eu

  apt-get update
  apt-get install --no-install-recommends -y ca-certificates

  cat > /etc/apt/sources.list.d/debian-src.list <<'EOF_SOURCES'
deb-src https://deb.debian.org/debian trixie main
deb-src https://deb.debian.org/debian trixie-updates main
deb-src https://security.debian.org/debian-security trixie-security main
EOF_SOURCES

  # Keep Mesa's complete build environment on Trixie. Mesa 25.0.7 is known
  # to build successfully against this toolchain.
  apt-get update
  apt-get build-dep -y mesa

  # Install the normal SPICE and virglrenderer build dependencies from Trixie as well.
  apt-get install --no-install-recommends -y \
    binutils \
    bzip2 \
    curl \
    dpkg-dev \
    file \
    git \
    gzip \
    libdrm-dev \
    libepoxy-dev \
    libgbm-dev \
    libglib2.0-dev \
    libjpeg-dev \
    libpixman-1-dev \
    libssl-dev \
    libvulkan-dev \
    meson \
    ninja-build \
    pkg-config \
    python3-pyparsing \
    python3-six \
    python3-yaml \
    xz-utils \
    zlib1g-dev

  # SPICE 0.16 requires newer protocol headers than Trixie provides. Download
  # only libspice-protocol-dev from the pinned Sid snapshot without allowing
  # Sid to replace any other part of the build environment.
  echo "deb [check-valid-until=no] https://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT}/ sid main" \
    > /etc/apt/sources.list.d/spice-snapshot.list

  apt-get update

  cd /tmp
  apt-get download -t sid libspice-protocol-dev
  dpkg -i ./libspice-protocol-dev_*.deb
  rm -f ./libspice-protocol-dev_*.deb

  echo "Using libspice-protocol-dev $(dpkg-query -W -f='${Version}' libspice-protocol-dev)"

  rm -f /etc/apt/sources.list.d/spice-snapshot.list
  rm -rf /var/lib/apt/lists/*
EOF_BUILD_DEPS

WORKDIR /src

RUN <<EOF_SOURCE
  set -eu

  curl -fL "https://deb.debian.org/debian/pool/main/m/mesa/mesa_${MESA_VERSION}.orig.tar.xz" -o mesa.tar.xz
  mkdir mesa
  tar -xf mesa.tar.xz -C mesa --strip-components=1
  rm -f mesa.tar.xz

  curl -fL "https://deb.debian.org/debian/pool/main/s/spice/spice_${SPICE_VERSION}.orig.tar.bz2" -o spice.tar.bz2
  mkdir spice
  tar -xf spice.tar.bz2 -C spice --strip-components=1
  rm -f spice.tar.bz2

  git init virglrenderer
  git -C virglrenderer remote add origin https://gitlab.freedesktop.org/virgl/virglrenderer.git
  git -C virglrenderer fetch --depth=1 origin "${VIRGL_REF}"
  git -C virglrenderer checkout --detach FETCH_HEAD
EOF_SOURCE

COPY patches/device-memory-budget.patch /tmp/

RUN <<'EOF_VIRGL_PATCHES'
  set -eu

  git -C /src/virglrenderer apply --check /tmp/device-memory-budget.patch
  git -C /src/virglrenderer apply /tmp/device-memory-budget.patch
  git -C /src/virglrenderer diff --check
EOF_VIRGL_PATCHES

# Build Mesa's shader compiler tools with LLVM available. These tools are used
# only while compiling the final runtime drivers and are never packaged.
RUN <<'EOF_TOOLS'
  set -eu

  meson setup /build-tools /src/mesa \
    --buildtype=release \
    -Dbuild-tests=false \
    -Degl=disabled \
    -Dgbm=disabled \
    -Dgallium-drivers=[] \
    -Dgles1=disabled \
    -Dgles2=disabled \
    -Dglx=disabled \
    -Dinstall-mesa-clc=true \
    -Dllvm=enabled \
    -Dmesa-clc=enabled \
    -Dopengl=false \
    -Dplatforms=[] \
    -Dvulkan-drivers=[]

  meson compile -C /build-tools mesa_clc vtn_bindgen

  install -Dm755 /build-tools/src/compiler/clc/mesa_clc /usr/local/bin/mesa_clc
  install -Dm755 /build-tools/src/compiler/spirv/vtn_bindgen /usr/local/bin/vtn_bindgen
EOF_TOOLS

# Build the Intel and AMD Gallium drivers needed for broad x86 GPU support.
# LLVM is explicitly disabled in this runtime build; the shader compiler tools
# built above are used only to generate the embedded Iris shader data. AMD uses
# the LLVM-free r600 compiler and RadeonSI ACO compiler paths.
RUN <<'EOF_MESA'
  set -eu

  multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"

  meson setup /build-mesa /src/mesa \
    --buildtype=release \
    --prefix=/usr \
    --libdir="lib/${multiarch}" \
    -Damd-use-llvm=false \
    -Dbuild-tests=false \
    -Degl=enabled \
    -Degl-native-platform=drm \
    -Dgbm=enabled \
    -Dgallium-drivers=i915,crocus,iris,r600,radeonsi \
    -Dgallium-nine=false \
    -Dgallium-opencl=disabled \
    -Dgallium-rusticl=false \
    -Dgallium-va=disabled \
    -Dgallium-vdpau=disabled \
    -Dgallium-xa=disabled \
    -Dgles1=disabled \
    -Dgles2=disabled \
    -Dglvnd=enabled \
    -Dglx=disabled \
    -Dintel-elk=true \
    -Dlibunwind=disabled \
    -Dllvm=disabled \
    -Dlmsensors=disabled \
    -Dmesa-clc=system \
    -Dopengl=true \
    -Dplatforms=[] \
    -Dshared-glapi=enabled \
    -Dvalgrind=disabled \
    -Dvideo-codecs=[] \
    -Dvulkan-drivers=[] \
    -Dvulkan-layers=[]

  meson compile -C /build-mesa
  meson install -C /build-mesa --destdir /mesa

  rm -rf \
    /mesa/usr/include \
    /mesa/usr/lib/*/pkgconfig \
    /mesa/usr/share/doc \
    /mesa/usr/share/man \
    /mesa/usr/share/pkgconfig

  find /mesa -type f -exec sh -c '
    for file do
      if file "$file" | grep -q "ELF"; then
        strip --strip-unneeded "$file"
      fi
    done
  ' sh {} +
EOF_MESA

# Build virglrenderer with both the normal VirGL renderer and Venus render-server
# support. The Helios patches only affect the Venus/proxy paths; normal VirGL
# remains the default unless QEMU explicitly enables Venus.
RUN <<'EOF_VIRGL'
  set -eu

  multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"

  meson setup /build-virgl /src/virglrenderer \
    --buildtype=release \
    --prefix=/usr \
    --libdir="lib/${multiarch}" \
    --libexecdir=libexec \
    -Dplatforms=egl \
    -Dvenus=true \
    -Drender-server-worker=thread \
    -Dunstable-apis=true \
    -Dtests=false \
    -Dvideo=false

  meson compile -C /build-virgl
  meson install -C /build-virgl --destdir /virgl

  rm -rf \
    /virgl/usr/bin \
    /virgl/usr/include \
    /virgl/usr/lib/*/pkgconfig \
    /virgl/usr/share

  # Keep only the runtime SONAME links/objects and the Venus render server.
  rm -f /virgl/usr/lib/*/libvirglrenderer.so

  library=$(find /virgl/usr/lib -type f -name 'libvirglrenderer.so.1.*' -print -quit)
  if [ -z "$library" ]; then
    echo "FAIL: libvirglrenderer runtime was not produced."
    exit 1
  fi

  if ! readelf -d "$library" | grep -q 'SONAME.*libvirglrenderer.so.1'; then
    echo "FAIL: unexpected virglrenderer SONAME."
    readelf -d "$library"
    exit 1
  fi

  if [ ! -x /virgl/usr/libexec/virgl_render_server ]; then
    echo "FAIL: virgl_render_server was not produced."
    exit 1
  fi

  # The memory-budget patch is compiled only into the Venus renderer path.
  if ! strings "$library" | grep -q 'VKR_DEVICE_MEMORY_LIMIT_BYTES'; then
    echo "FAIL: Helios device-memory-budget patch is missing from virglrenderer."
    exit 1
  fi

  find /virgl -type f -exec sh -c '
    for file do
      if file "$file" | grep -q "ELF"; then
        strip --strip-unneeded "$file"
      fi
    done
  ' sh {} +
EOF_VIRGL

# Build a SPICE server runtime for QEMU QXL without the optional multimedia,
# authentication, smartcard and extra compression stacks. QEMU's own SPICE
# modules remain supplied by Debian so their module stamps stay synchronized
# with every QEMU point release.
RUN <<'EOF_SPICE'
  set -eu

  multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"

  meson setup /build-spice /src/spice \
    --buildtype=release \
    --prefix=/usr \
    --libdir="lib/${multiarch}" \
    -Dgstreamer=no \
    -Dlz4=false \
    -Dsasl=false \
    -Dopus=disabled \
    -Dsmartcard=disabled \
    -Dmanual=false \
    -Dstatistics=false \
    -Dinstrumentation=no \
    -Dtests=false

  meson compile -C /build-spice
  meson install -C /build-spice --destdir /spice

  rm -rf \
    /spice/usr/include \
    /spice/usr/lib/*/pkgconfig \
    /spice/usr/share

  # The unversioned linker name belongs to the development package. Keep only
  # the SONAME link and versioned runtime object that libspice-server1 ships.
  rm -f /spice/usr/lib/*/libspice-server.so

  library=$(find /spice/usr/lib -type f -name 'libspice-server.so.1.*' -print -quit)
  if [ -z "$library" ]; then
    echo "FAIL: libspice-server runtime was not produced."
    exit 1
  fi

  if ! readelf -d "$library" | grep -q 'SONAME.*libspice-server.so.1'; then
    echo "FAIL: unexpected SPICE server SONAME."
    readelf -d "$library"
    exit 1
  fi

  find /spice -type f -exec sh -c '
    for file do
      if file "$file" | grep -q "ELF"; then
        strip --strip-unneeded "$file"
      fi
    done
  ' sh {} +
EOF_SPICE

# Package the minimal Mesa, SPICE and virglrenderer runtimes together. The
# package replaces their Debian runtime providers; version-matched QEMU modules
# are deliberately not included.
RUN <<EOF_PACKAGE
  set -eu

  multiarch="$(dpkg-architecture -qDEB_HOST_MULTIARCH)"
  libdir="/package/usr/lib/${multiarch}"

  mkdir -p /package/DEBIAN
  cp -a /mesa/. /package/
  cp -a /spice/. /package/
  cp -a /virgl/. /package/

  mkdir -p /package/usr/share/doc/qemu-render
  cp /src/mesa/docs/license.rst /package/usr/share/doc/qemu-render/copyright.Mesa
  cp /src/spice/COPYING /package/usr/share/doc/qemu-render/copyright.SPICE
  cp /src/virglrenderer/COPYING /package/usr/share/doc/qemu-render/copyright.virglrenderer

  # Add the Debian packages required by the custom runtimes. Libraries shipped
  # inside this package are intentionally excluded.
  : > /tmp/depends

  find /package/usr -type f -exec sh -c '
    libdir=$1
    shift

    for file do
      file "$file" | grep -q "ELF" || continue

      LD_LIBRARY_PATH="$libdir" ldd "$file" 2>/dev/null \
        | awk "
            /=> \// { print \$3 }
            /^[[:space:]]*\/[^ ]/ { print \$1 }
          "
    done
  ' sh "$libdir" {} + \
    | sort -u \
    | while IFS= read -r library; do
        [ -n "$library" ] || continue

        case "$library" in
          /package/*) continue ;;
        esac

        library="$(realpath "$library")"
        owner="$(dpkg-query -S "$library" 2>/dev/null | head -n 1 || true)"
        [ -n "$owner" ] || continue

        owner="${owner%%:*}"
        echo "$owner" >> /tmp/depends
      done

  printf '%s\n' libegl1 libopengl0 >> /tmp/depends

  sort -u /tmp/depends -o /tmp/depends
  depends="$(paste -sd, /tmp/depends | sed 's/,/, /g')"
  installed_size="$(du -sk /package/usr | cut -f1)"

  cat > /package/DEBIAN/control <<EOF_CONTROL
Package: qemu-render
Version: ${VERSION_ARG}
Section: libs
Priority: optional
Architecture: amd64
Maintainer: qemus <qemus@users.noreply.github.com>
Depends: ${depends}
Provides: libgbm1 (= ${MESA_VERSION}), libegl-mesa0 (= ${MESA_VERSION}), libspice-server1 (= ${SPICE_VERSION}), libvirglrenderer1 (= ${VIRGL_VERSION})
Conflicts: libgbm1, libegl-mesa0, libspice-server1, libvirglrenderer1, virgl-server
Replaces: libgbm1, libegl-mesa0, libspice-server1, libvirglrenderer1, virgl-server
Installed-Size: ${installed_size}
Homepage: https://github.com/qemus/qemu-render
Description: Minimal graphics runtime for QEMU
 Provides an Intel and AMD Mesa runtime supporting i915, Crocus, Iris, r600 and
 RadeonSI together with EGL and GBM, a minimal SPICE server runtime for QXL, and
 a VirGL/Venus renderer with the Venus render server and device-memory budgeting,
 without LLVM, GStreamer, Opus, SASL, smartcard or optional compression runtimes.
EOF_CONTROL

  echo
  echo "================================================================"
  echo "Test 1: packaged ELF dependency isolation"
  echo "================================================================"

  failed=0

  for file in $(find /package/usr -type f); do
    file "$file" | grep -q "ELF" || continue

    echo
    echo "--- $file"
    needed="$(readelf -d "$file" 2>/dev/null | grep 'NEEDED' || true)"
    printf '%s\n' "$needed"

    if printf '%s\n' "$needed" | grep -qi 'libLLVM'; then
      echo "FAIL: $file depends directly on LLVM."
      failed=1
    fi
  done

  spice_library=$(find "$libdir" -type f -name 'libspice-server.so.1.*' -print -quit)
  spice_needed="$(readelf -d "$spice_library" 2>/dev/null | grep 'NEEDED' || true)"

  for unwanted in libgstreamer libgst libopus libsasl liblz4 libcacard liborc; do
    if printf '%s\n' "$spice_needed" | grep -qi "$unwanted"; then
      echo "FAIL: minimal SPICE runtime still depends on $unwanted."
      failed=1
    fi
  done

  if [ "$failed" -ne 0 ]; then
    exit 1
  fi

  echo
  echo "PASS: optional Mesa/SPICE dependency stacks are absent."

  echo
  echo "================================================================"
  echo "Package size diagnostics"
  echo "================================================================"
  du -sh /mesa /spice /virgl /package

  echo
  echo "Largest packaged files:"
  du -ah /package/usr | sort -h | tail -n 30

  mkdir -p /dist
  dpkg-deb \
    --root-owner-group \
    --build \
    /package \
    "/dist/qemu-render_${VERSION_ARG}_amd64.deb"

  package="/dist/qemu-render_${VERSION_ARG}_amd64.deb"
  size="$(stat -c %s "$package")"

  echo
  echo "================================================================"
  echo "Package metadata"
  echo "================================================================"
  dpkg-deb -I "$package"
  echo
  dpkg-deb -c "$package"
  echo
  du -h "$package"

  if [ "$size" -gt 104857600 ]; then
    echo "FAIL: package is unexpectedly larger than 100 MB."
    exit 1
  fi
EOF_PACKAGE

FROM debian:trixie-slim AS verify

ARG VERSION_ARG="0.0"
ARG VERSION_QEMU="1:11.1.0+ds-2"
ARG DEBIAN_SNAPSHOT="20260819T142328Z"
ARG DEBIAN_FRONTEND="noninteractive"

COPY --from=builder /dist/ /dist/

# Install the finished package with Debian's official version-matched QEMU
# OpenGL and SPICE modules. This proves the custom runtime satisfies both
# dependency chains without embedding QEMU modules in qemu-render itself.
RUN <<EOF_VERIFY
  set -eu

  apt-get update
  apt-get install --no-install-recommends -y \
    binutils \
    ca-certificates \
    file

  echo "deb [check-valid-until=no] https://snapshot.debian.org/archive/debian/${DEBIAN_SNAPSHOT}/ sid main" \
    > /etc/apt/sources.list.d/qemu-snapshot.list

  apt-get update
  apt-get --no-install-recommends -y -t sid install \
    "/dist/qemu-render_${VERSION_ARG}_amd64.deb" \
    "qemu-system-x86=${VERSION_QEMU}" \
    "qemu-system-modules-opengl=${VERSION_QEMU}" \
    "qemu-system-modules-spice=${VERSION_QEMU}"

  echo
  echo "================================================================"
  echo "Package isolation"
  echo "================================================================"

  for package in libgbm1 libegl-mesa0 libspice-server1 libvirglrenderer1 virgl-server mesa-libgallium; do
    if dpkg-query -W -f='${Status}\n' "$package" 2>/dev/null | grep -q '^install ok installed$'; then
      echo "FAIL: unwanted stock package was installed: $package"
      exit 1
    fi
  done

  if dpkg-query -W -f='${Status} ${binary:Package}\n' 'libllvm*' 2>/dev/null | grep -q '^install ok installed '; then
    dpkg-query -W -f='${Status} ${binary:Package}\t${Version}\n' 'libllvm*' 2>/dev/null | grep '^install ok installed ' || true
    echo "FAIL: an LLVM runtime package was installed."
    exit 1
  fi

  echo "PASS: stock Mesa/SPICE/virglrenderer providers and LLVM are absent."

  echo
  echo "================================================================"
  echo "Runtime loader availability"
  echo "================================================================"

  for library in libEGL.so.1 libOpenGL.so.0 libspice-server.so.1 libvirglrenderer.so.1; do
    if ! ldconfig -p | grep -q "$library"; then
      echo "FAIL: required runtime library is missing: $library"
      exit 1
    fi
    echo "PASS: $library is available."
  done

  if [ ! -x /usr/libexec/virgl_render_server ]; then
    echo "FAIL: virgl_render_server is missing from qemu-render."
    exit 1
  fi
  echo "PASS: virgl_render_server is available."

  virgl_library="$(dpkg-query -L qemu-render | grep '/libvirglrenderer.so.1$' | head -n 1)"
  if [ -z "$virgl_library" ] || [ ! -e "$virgl_library" ]; then
    echo "FAIL: qemu-render does not own the libvirglrenderer SONAME link."
    exit 1
  fi
  echo "PASS: custom virglrenderer is owned by qemu-render: $virgl_library"

  opengl_module="$(dpkg-query -L qemu-system-modules-opengl | grep '/hw-display-virtio-gpu-gl.so$' | head -n 1)"
  if [ -z "$opengl_module" ]; then
    echo "FAIL: QEMU virtio-gpu OpenGL module was not found."
    exit 1
  fi

  resolved_virgl="$(ldd "$opengl_module" | awk '/libvirglrenderer\.so\.1 =>/ { print $3; exit }')"
  if [ -z "$resolved_virgl" ] || \
     [ "$(readlink -f "$resolved_virgl")" != "$(readlink -f "$virgl_library")" ]; then
    ldd "$opengl_module"
    echo "FAIL: QEMU OpenGL module is not resolving to qemu-render virglrenderer."
    exit 1
  fi
  echo "PASS: QEMU OpenGL module resolves to the custom virglrenderer."

  echo
  echo "================================================================"
  echo "Test 2: installed runtime dependency scan"
  echo "================================================================"

  missing=0
  llvm_transitive=0
  spice_optional=0

  for package in qemu-render qemu-system-modules-opengl qemu-system-modules-spice; do
    for file in $(dpkg-query -L "$package"); do
      [ -f "$file" ] || continue
      file "$file" | grep -q "ELF" || continue

      echo
      echo "--- $file"
      deps="$(ldd "$file" 2>&1 || true)"
      printf '%s\n' "$deps"

      if printf '%s\n' "$deps" | grep -q 'not found'; then
        missing=1
      fi

      if printf '%s\n' "$deps" | grep -qi 'libLLVM'; then
        llvm_transitive=1
      fi

      case "$file" in
        *libspice-server.so.1.* )
          if printf '%s\n' "$deps" | grep -Eqi 'lib(gst|gstreamer|opus|sasl|lz4|cacard|orc)'; then
            spice_optional=1
          fi ;;
      esac
    done
  done

  if [ "$missing" -ne 0 ]; then
    echo
    echo "FAIL: one or more runtime dependencies could not be resolved."
    exit 1
  fi

  if [ "$llvm_transitive" -ne 0 ]; then
    echo
    echo "FAIL: LLVM appears in the runtime dependency tree."
    exit 1
  fi

  if [ "$spice_optional" -ne 0 ]; then
    echo
    echo "FAIL: an optional SPICE dependency reappeared at runtime."
    exit 1
  fi

  echo
  echo "PASS: all runtime dependencies resolve without the removed stacks."

  echo
  echo "================================================================"
  echo "Test 3: virtio-gpu OpenGL module loading"
  echo "================================================================"

  if ! qemu-system-x86_64 -device virtio-vga-gl,help >/tmp/virtio-vga-gl-help 2>&1; then
    cat /tmp/virtio-vga-gl-help
    echo "FAIL: QEMU could not load the virtio-vga-gl device module."
    exit 1
  fi

  cat /tmp/virtio-vga-gl-help
  echo "PASS: virtio-vga-gl device module loaded with the custom virglrenderer."

  echo
  echo "================================================================"
  echo "Test 4: QXL module loading"
  echo "================================================================"

  if ! qemu-system-x86_64 -device qxl-vga,help >/tmp/qxl-help 2>&1; then
    cat /tmp/qxl-help
    echo "FAIL: QEMU could not load the QXL device module."
    exit 1
  fi

  cat /tmp/qxl-help
  echo "PASS: QXL device module loaded successfully."

  echo
  echo "================================================================"
  echo "Test 5: QXL with the VNC display path"
  echo "================================================================"

  set +e
  timeout 3s qemu-system-x86_64 \
    -nodefaults \
    -machine pc,accel=tcg \
    -m 64M \
    -monitor none \
    -serial none \
    -vnc unix:/tmp/qxl-vnc.sock \
    -device qxl-vga \
    -S \
    >/tmp/qxl-vnc.log 2>&1
  rc=$?
  set -e

  if [ "$rc" -ne 124 ]; then
    cat /tmp/qxl-vnc.log
    echo "FAIL: QEMU did not remain running with QXL and VNC."
    exit 1
  fi

  echo "PASS: QEMU remained running with QXL and VNC without a SPICE listener."

  echo
  echo "================================================================"
  echo "Installed package status"
  echo "================================================================"
  dpkg-query -W \
    -f='${binary:Package}\t${Version}\n' \
    qemu-render \
    qemu-system-x86 \
    qemu-system-common \
    qemu-system-modules-opengl \
    qemu-system-modules-spice \
    libegl1 \
    libopengl0 \
    libglvnd0
EOF_VERIFY

FROM scratch AS artifact
COPY --from=verify /dist/ /
