# Build without CMake, for the phone (FuriOS/Debian, aarch64) and for a laptop.
# Requires: g++, make, pkg-config, wayland-scanner and wayland-protocols.
#   make -j1            # on the phone (nproc may be 1)
#   make test           # fft_test + waterfall_test + headless
#   make clean
#
# It does not depend on libdrm or CMake. pkg-config resolves EGL/GLESv2; on the phone
# libEGL.so may be the libhybris wrapper or the Mesa one.

APP              := fmiq-app
BUILD            ?= build-make
CXX              ?= g++
CC               ?= gcc
WAYLAND_SCANNER  ?= wayland-scanner
PKGS             := egl glesv2 wayland-client
ifeq ($(shell pkg-config --exists wayland-egl && echo yes),yes)
PKGS             += wayland-egl
endif
PKG_CFLAGS       := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS         := $(shell pkg-config --libs $(PKGS))
ifeq ($(shell pkg-config --exists wayland-egl && echo yes),)
PKG_LIBS         += -lwayland-egl
endif

WAYLAND_PROTOCOLS_DIR ?= $(shell pkg-config --variable=pkgdatadir wayland-protocols 2>/dev/null)
ifeq ($(strip $(WAYLAND_PROTOCOLS_DIR)),)
WAYLAND_PROTOCOLS_DIR := /usr/share/wayland-protocols
endif
XDG_XML := $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml

GEN := $(BUILD)/gen
OBJ := $(BUILD)/obj
XDG_H := $(GEN)/xdg-shell-client-protocol.h
XDG_C := $(GEN)/xdg-shell-protocol.c

CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-unused-parameter -Isrc -I../common -I$(GEN) $(PKG_CFLAGS)
CFLAGS   ?= -O2 -g
LDFLAGS  ?=
LDLIBS   += $(PKG_LIBS) -lm -lpthread -ldl

# The real source needs SoapySDR. Without it a stub is compiled that reports a clear message to
# the user, so the app always builds on any machine.
HAVE_SOAPY := $(shell pkg-config --exists SoapySDR && echo yes)
ifeq ($(HAVE_SOAPY),yes)
PKG_CFLAGS += $(shell pkg-config --cflags SoapySDR)
LDLIBS     += $(shell pkg-config --libs SoapySDR)
SRC_DEVICE := src/source_device.cpp
else
SRC_DEVICE := src/source_device_stub.cpp
endif

SRCS := src/main.cpp src/render_egl.cpp src/render_gl.cpp $(SRC_DEVICE)
OBJS := $(SRCS:src/%.cpp=$(OBJ)/%.o) $(OBJ)/xdg-shell-protocol.o

.PHONY: all test clean install dirs

all: $(APP) fft_test waterfall_test tune_hold_test review_test

$(APP): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

fft_test: $(OBJ)/fft_test.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< -lm -lpthread -ldl

waterfall_test: $(OBJ)/waterfall_test.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< -lm

tune_hold_test: $(OBJ)/tune_hold_test.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< -lm -lpthread -ldl

# Review tests: waterfall minification (max per group), gain as a dB shift, capture age and the
# three ring notices. Reads the block files of tools/testdata.
review_test: $(OBJ)/review_test.o
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $< -lm -lpthread -ldl

$(OBJ)/%.o: src/%.cpp $(XDG_H) | dirs
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(OBJ)/xdg-shell-protocol.o: $(XDG_C) | dirs
	$(CC) $(CFLAGS) -I$(GEN) -c -o $@ $<

$(XDG_H): $(XDG_XML) | dirs
	$(WAYLAND_SCANNER) client-header $(XDG_XML) $@

$(XDG_C): $(XDG_XML) | dirs
	$(WAYLAND_SCANNER) private-code $(XDG_XML) $@

dirs:
	@mkdir -p $(GEN) $(OBJ)

test: all
	./fft_test
	./waterfall_test
	./tune_hold_test
	./review_test
	./$(APP) --headless 48 --out $(BUILD)/headless.ppm

install: $(APP)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(APP) $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(PREFIX)/share/applications
	install -m 0644 fmiq-app.desktop $(DESTDIR)$(PREFIX)/share/applications/
	install -d $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps
	install -m 0644 assets/icons/furimodem-iq.jpg $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/furimodem-iq.jpg

PREFIX ?= /usr/local

clean:
	rm -rf $(BUILD) $(APP) fft_test waterfall_test tune_hold_test review_test
