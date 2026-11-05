# Build of the SDR mode window (GTK3), for the phone and for the laptop.
# Requires: g++, make, pkg-config and the GTK3 headers (libgtk-3-dev).
#   make -j1          # on the phone
#   make test         # logic tests, without GTK or the phone
#   make install      # binary + desktop launcher + polkit action
#   make clean
#
# The window links nothing from the device: it invokes sdr-load as a subprocess.
# The stylesheet (style.css) is wrapped into a C string at build time and compiled
# into the binary, so the window needs no data file at run time.

APP     := furimodem-sdr
BUILD   ?= build-make
CXX     ?= g++
PKGS    := gtk+-3.0

PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS) 2>/dev/null)
PKG_LIBS   := $(shell pkg-config --libs $(PKGS) 2>/dev/null)

CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra -Wno-unused-parameter -Isrc -I$(BUILD) $(PKG_CFLAGS)
LDFLAGS  ?=
LDLIBS   += $(PKG_LIBS)

OBJ  := $(BUILD)/obj
SRCS := src/main.cpp src/runner.cpp src/status.cpp src/actions.cpp src/privileges.cpp src/theme.cpp
OBJS := $(SRCS:src/%.cpp=$(OBJ)/%.o)
TEST_OBJS := $(OBJ)/parse_test.o $(OBJ)/status.o $(OBJ)/actions.o
PRIV_OBJS := $(OBJ)/privileges_test.o $(OBJ)/privileges.o $(OBJ)/actions.o

# gui/style.css -> $(BUILD)/style_css.hpp: one quoted line of CSS per source line.
STYLE     := style.css
STYLE_HPP := $(BUILD)/style_css.hpp

POLICY      := org.furimodem.sdr.policy
# polkit only reads the system directory, not the prefix: a policy in /usr/local is
# ignored. Overridable for a DESTDIR install or a packaging tree.
POLKIT_DIR  ?= /usr/share/polkit-1/actions

.PHONY: all test clean install check-pkg-config

all: check-pkg-config $(APP)

# Clear warning if GTK3 is missing in the tree being used (e.g. cross-build).
check-pkg-config:
	@if [ -z "$(PKG_LIBS)" ]; then \
	    echo "ERROR: gtk+-3.0 not found through pkg-config." >&2; \
	    echo "       Install libgtk-3-dev (or point PKG_CONFIG_LIBDIR at the arm64 tree)." >&2; \
	    exit 1; \
	fi

$(APP): $(OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(OBJ)/%.o: src/%.cpp | $(OBJ)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(STYLE_HPP): $(STYLE) | $(OBJ)
	@mkdir -p $(BUILD)
	@{ [ -s $(STYLE) ] && sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $(STYLE); } > $@.tmp
	@[ -s $@.tmp ] || printf '""\n' > $@.tmp
	@mv $@.tmp $@

# The stylesheet is part of the object that embeds it: editing the CSS relinks.
$(OBJ)/theme.o: $(STYLE_HPP)

parse_test: $(TEST_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(TEST_OBJS)

$(OBJ)/parse_test.o: tests/parse_test.cpp | $(OBJ)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

privileges_test: $(PRIV_OBJS)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(PRIV_OBJS)

$(OBJ)/privileges_test.o: tests/privileges_test.cpp | $(OBJ)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

test: parse_test privileges_test
	./parse_test
	./privileges_test

$(OBJ):
	@mkdir -p $(OBJ)

PREFIX ?= /usr/local
install: $(APP)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(APP) $(DESTDIR)$(PREFIX)/bin/
	install -d $(DESTDIR)$(PREFIX)/share/applications
	install -m 0644 furimodem-sdr.desktop $(DESTDIR)$(PREFIX)/share/applications/
	install -d $(DESTDIR)$(POLKIT_DIR)
	install -m 0644 $(POLICY) $(DESTDIR)$(POLKIT_DIR)/

clean:
	rm -rf $(BUILD) $(APP) parse_test privileges_test
