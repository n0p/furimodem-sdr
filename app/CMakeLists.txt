cmake_minimum_required(VERSION 3.16)
project(furimodem_iq_app CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FMIQ_BUILD_TESTS "Build fft_test and waterfall_test" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(EGL REQUIRED IMPORTED_TARGET egl)
pkg_check_modules(GLES REQUIRED IMPORTED_TARGET glesv2)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(WAYLAND_EGL REQUIRED IMPORTED_TARGET wayland-egl)
# Optional, exactly like the Makefile: without SoapySDR a stub reports the reason on screen.
pkg_check_modules(SOAPY QUIET IMPORTED_TARGET SoapySDR)

find_program(WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)
pkg_check_modules(WAYLAND_PROTOCOLS QUIET wayland-protocols)
if(WAYLAND_PROTOCOLS_FOUND)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)
endif()
if(NOT WAYLAND_PROTOCOLS_DIR)
    set(WAYLAND_PROTOCOLS_DIR "/usr/share/wayland-protocols")
endif()
set(XDG_SHELL_XML "${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml")
if(NOT EXISTS "${XDG_SHELL_XML}")
    message(FATAL_ERROR "Missing ${XDG_SHELL_XML}; install wayland-protocols")
endif()

set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${GEN_DIR}")
add_custom_command(
    OUTPUT "${GEN_DIR}/xdg-shell-client-protocol.h"
    COMMAND "${WAYLAND_SCANNER}" client-header "${XDG_SHELL_XML}"
            "${GEN_DIR}/xdg-shell-client-protocol.h"
    DEPENDS "${XDG_SHELL_XML}"
    VERBATIM)
add_custom_command(
    OUTPUT "${GEN_DIR}/xdg-shell-protocol.c"
    COMMAND "${WAYLAND_SCANNER}" private-code "${XDG_SHELL_XML}" "${GEN_DIR}/xdg-shell-protocol.c"
    DEPENDS "${XDG_SHELL_XML}"
    VERBATIM)

add_executable(fmiq-app
    src/main.cpp
    src/render_egl.cpp
    src/render_gl.cpp
    "${GEN_DIR}/xdg-shell-protocol.c"
    "${GEN_DIR}/xdg-shell-client-protocol.h")
target_include_directories(fmiq-app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common" "${GEN_DIR}")
target_link_libraries(fmiq-app PRIVATE
    PkgConfig::EGL
    PkgConfig::GLES
    PkgConfig::WAYLAND
    PkgConfig::WAYLAND_EGL
    m
    pthread
    dl)
target_compile_options(fmiq-app PRIVATE -Wall -Wextra -Wno-unused-parameter)

if(SOAPY_FOUND)
    target_sources(fmiq-app PRIVATE src/source_device.cpp)
    target_link_libraries(fmiq-app PRIVATE PkgConfig::SOAPY)
else()
    target_sources(fmiq-app PRIVATE src/source_device_stub.cpp)
endif()

if(FMIQ_BUILD_TESTS)
    enable_testing()
    add_executable(fft_test src/fft_test.cpp)
    target_include_directories(fft_test PRIVATE src)
    target_link_libraries(fft_test PRIVATE m pthread dl)
    add_test(NAME fft COMMAND fft_test)

    add_executable(waterfall_test src/waterfall_test.cpp)
    target_include_directories(waterfall_test PRIVATE src)
    target_link_libraries(waterfall_test PRIVATE m)
    add_test(NAME waterfall COMMAND waterfall_test)

    add_executable(tune_hold_test src/tune_hold_test.cpp)
    target_include_directories(tune_hold_test PRIVATE src "${CMAKE_CURRENT_SOURCE_DIR}/../common")
    target_link_libraries(tune_hold_test PRIVATE m pthread dl)
    add_test(NAME tune_hold COMMAND tune_hold_test)

    # Waterfall minification, gain as a dB shift, capture age and the ring notices. It reads the
    # block files of tools/testdata relative to app/, hence the working directory.
    add_executable(review_test src/review_test.cpp)
    target_include_directories(review_test PRIVATE src "${CMAKE_CURRENT_SOURCE_DIR}/../common")
    target_link_libraries(review_test PRIVATE m pthread dl)
    add_test(NAME review COMMAND review_test)
    set_tests_properties(review PROPERTIES WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")

    # Startup hang: the source is opened with a deadline and, if the module never answers, the
    # synthetic signal is used with a notice instead of leaving the window on the boot screen.
    add_executable(source_open_test src/source_open_test.cpp)
    target_include_directories(source_open_test PRIVATE src "${CMAKE_CURRENT_SOURCE_DIR}/../common")
    target_link_libraries(source_open_test PRIVATE m pthread dl)
    add_test(NAME source_open COMMAND source_open_test)
endif()

install(TARGETS fmiq-app RUNTIME DESTINATION bin)
