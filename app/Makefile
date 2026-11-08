# Build without CMake, for the phone (FuriOS/Debian, aarch64) and for a laptop.
# Requires: g++, make, pkg-config, wayland-scanner and wayland-protocols.
#   make -j1            # on the phone (nproc may be 1)
#   make test           # fft_test + waterfall_test + headless
#   make clean
#
# It does not depend on libdrm or CMake. pkg-config resolves EGL/GLESv2; on the phone
# libEGL.so may be the libhybris wrapper or the Mesa one.

APP              ?= fmiq-app
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
# Header dependencies: without -MMD, editing src/*.hpp leaves the .o files untouched and make
# reports "nothing to be done" with a binary that does not match the sources.
CXXFLAGS += -MMD -MP
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
$(warning SoapySDR not found through pkg-config: the app will be built WITHOUT device)
$(warning support (synthetic and file sources only). Install libsoapysdr-dev, or use)
$(warning tools/crossbuild2.sh to build it with SoapySDR for the phone.)
endif

SRCS := src/main.cpp src/render_egl.cpp src/render_gl.cpp $(SRC_DEVICE)
OBJS := $(SRCS:src/%.cpp=$(OBJ)/%.o) $(OBJ)/xdg-shell-protocol.o
# The test objects are not in OBJS (they are built by the pattern rule), so their .d files are
# taken from the directory as well: editing a header has to rebuild the tests too.
DEPS := $(OBJS:.o=.d) $(wildcard $(OBJ)/*.d)
-include $(DEPS)

.PHONY: all test clean install dirs
# The .d files included above bring targets of their own; without this, the default goal becomes
# the first object of the first .d file and a plain `make` builds only that one.
.DEFAULT_GOAL := all

all: $(APP) fft_test waterfall_test tune_hold_test review_test source_open_test

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

# Startup hang: a source whose start() never returns must not block the app. Checks the open
# deadline, the fallback to the synthetic signal and its notice, and the UI pump.
source_open_test: $(OBJ)/source_open_test.o
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
	./source_open_test
	./$(APP) --headless 48 --out $(BUILD)/headless.ppm
	@# Startup hang, end to end with the real binary: a device source whose start() never returns
	@# (FMIQ_APP_HANG_SOURCE_MS, test hook) must not hang the app. With --source auto the deadline
	@# is respected, the synthetic signal is used and the notice reaches the screen/log; with an
	@# explicit --source soapy the headless mode exits with a clear error.
	@echo "== startup hang: auto falls back to synthetic"
	@timeout 15 env FMIQ_APP_HANG_SOURCE_MS=60000 ./$(APP) --headless 8 --source auto \
	    --open-timeout 300 --out $(BUILD)/hang.ppm >$(BUILD)/hang.out 2>&1 \
	    && grep -q "NO DEVICE: no answer in .* (SYNTHETIC SIGNAL)" $(BUILD)/hang.out \
	    && grep -q "did not answer in .*SDR mode not available" $(BUILD)/hang.out \
	    || { cat $(BUILD)/hang.out; echo "the app did not fall back to the synthetic signal"; exit 1; }
	@grep "SYNTHETIC SIGNAL" $(BUILD)/hang.out
	@echo "== startup hang: explicit device source fails, no hang"
	@timeout 15 env FMIQ_APP_HANG_SOURCE_MS=60000 ./$(APP) --headless 8 --source soapy \
	    --open-timeout 300 --out $(BUILD)/nope.ppm >$(BUILD)/hang2.out 2>&1 \
	    && { cat $(BUILD)/hang2.out; echo "an explicit device source must fail on timeout"; exit 1; } \
	    || true
	@grep -q "did not answer" $(BUILD)/hang2.out \
	    || { cat $(BUILD)/hang2.out; echo "missing timeout reason"; exit 1; }
	@grep "did not answer" $(BUILD)/hang2.out

install: $(APP)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(APP) $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(PREFIX)/share/applications
	install -m 0644 fmiq-app.desktop $(DESTDIR)$(PREFIX)/share/applications/
	install -m 0644 fmiq-app-captura.desktop $(DESTDIR)$(PREFIX)/share/applications/
	install -d $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps
	install -m 0644 assets/icons/furimodem-iq.png $(DESTDIR)$(PREFIX)/share/icons/hicolor/256x256/apps/furimodem-iq.png

PREFIX ?= /usr

clean:
	rm -rf $(BUILD) $(APP) fft_test waterfall_test tune_hold_test review_test source_open_test
