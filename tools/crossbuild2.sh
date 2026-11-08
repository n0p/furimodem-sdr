#!/bin/sh
# Reproducible arm64 cross-build, with artifacts in /out.
#
# IMPORTANT: the base image must match the phone userland (Debian sid / FuriOS).
# With an older base (bookworm) the code does not compile because the Wayland
# structures have fewer fields: the app uses the sid version.
#   docker run --rm -v "$PWD":/src -v "$PWD/out":/out debian:sid sh /src/tools/crossbuild2.sh
#
# Container dependencies (installed here, never assumed):
#   - crossbuild-essential-arm64          aarch64 toolchain
#   - libsoapysdr-dev:arm64               SoapySDR API for the module
#   - libegl-dev / libgles-dev / libwayland-dev :arm64   for the app
#   - libgtk-3-dev:arm64                  GTK3 headers for the window (GTK3 ships with phosh)
#   - wayland-protocols                   xdg-shell XML (arch: all)
#   - libwayland-bin                      wayland-scanner, from the HOST (it runs at build time)
#
# Note: the host pkg-config returns x86 libraries. It has to be pointed at the
# arm64 tree with PKG_CONFIG_LIBDIR, or linking fails (or worse, links the wrong thing).
set -e

dpkg --add-architecture arm64
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    crossbuild-essential-arm64 pkg-config cmake make file \
    libsoapysdr-dev:arm64 libegl-dev:arm64 libgles-dev:arm64 libwayland-dev:arm64 \
    libgtk-3-dev:arm64 \
    wayland-protocols libwayland-bin

# pkg-config looking ONLY at the arm64 tree (+ the architecture-independent .pc files)
export PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig
export PKG_CONFIG_SYSROOT_DIR=/
export PKG_CONFIG_PATH=

# wayland-scanner must be the host one: it is executed during the build
export WAYLAND_SCANNER="${WAYLAND_SCANNER:-/usr/bin/wayland-scanner}"

mkdir -p /out
cd /src

echo "== driver (arm64) =="
cmake -S driver -B /tmp/bd -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++ \
      -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64 >/tmp/c.log 2>&1
cmake --build /tmp/bd -j"$(nproc)" >>/tmp/c.log 2>&1
cp /tmp/bd/libfurimodemIQ.so /out/
# CCB ring tool (--peek / --live / --device) and the ring pull for the phone
cp /tmp/bd/selftest_transport /out/ && echo "  copied selftest_transport"
cp /tmp/bd/iqring_pull /out/ && echo "  copied iqring_pull"

echo "== app (arm64) =="
# Separate build dir AND output path: nothing aarch64 is left in app/ (a later native
# `make -C app` would otherwise think the ARM binary is up to date).
make -C app BUILD=/tmp/ba APP=/tmp/ba/fmiq-app \
     CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc \
     WAYLAND_SCANNER="$WAYLAND_SCANNER" \
     -j"$(nproc)" /tmp/ba/fmiq-app >/tmp/a.log 2>&1 || {
    echo "ERROR: the app did not build. Last lines of /tmp/a.log:" >&2
    tail -25 /tmp/a.log >&2
    exit 1
}
cp /tmp/ba/fmiq-app /out/ && echo "  copied fmiq-app"

echo "== sdr-load (arm64) =="
# SDR mode loader: no dependencies, POSIX + libstdc++.
aarch64-linux-gnu-g++ -std=c++17 -O2 -Wall -Wextra \
    -o /out/sdr-load /src/tools/sdr-load.cpp && echo "  copied sdr-load"

echo "== gui (arm64) =="
# GTK3 window: it invokes sdr-load as a subprocess, it links nothing from the device.
# The binary goes straight to /out so no aarch64 object is left in the native tree.
make -C gui BUILD=/tmp/bg APP=/out/furimodem-sdr \
     CXX=aarch64-linux-gnu-g++ CC=aarch64-linux-gnu-gcc \
     -j"$(nproc)" >/tmp/g.log 2>&1 || {
    echo "ERROR: the window did not build. Last lines of /tmp/g.log:" >&2
    tail -30 /tmp/g.log >&2
    exit 1
}
echo "  copied furimodem-sdr"

echo "== artifacts =="
file /out/* 2>/dev/null
