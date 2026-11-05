# Reference toolchain for cross-compiling from a laptop to the phone (aarch64).
# Requires the Debian/Ubuntu cross-gcc and a sysroot with the phone dependencies:
#   sudo apt install g++-aarch64-linux-gnu
#   # plus a sysroot with libEGL/libGLESv2/libwayland-client (copied from the phone or Debian arm64)
#
# Usage:
#   PKG_CONFIG_PATH=/path/sysroot/usr/lib/aarch64-linux-gnu/pkgconfig \
#   cmake -S app -B app/build-aarch64 \
#         -DCMAKE_TOOLCHAIN_FILE=app/cmake/aarch64-linux-gnu.cmake \
#         -DCMAKE_BUILD_TYPE=Release
#
# If the sysroot is not the cross-gcc one, uncomment and adjust CMAKE_SYSROOT.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
# set(CMAKE_SYSROOT /path/to/sysroot)

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
