# sdr-load and iqmode: no dependencies beyond POSIX and libstdc++.
#
#   make                 native build (development bench)
#   make check           sdr-load self-test, no device needed
#   make cross           aarch64 binaries in dist/ (debian:sid container, the phone's userland)
#   make install         sdr-load, iqmode and the flash script (PREFIX=/usr by default,
#                        the path the polkit action names)
#
# The app, the Soapy module and the GTK window have their own build files.

CXX ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++17 -Wall -Wextra
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share/furimodem-sdr

PROGS := sdr-load iqmode

.PHONY: all check cross install clean

all: $(PROGS)

sdr-load: sdr-load.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

iqmode: iqmode.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

check: sdr-load
	./sdr-load --selftest

# Same base as the phone (Debian sid) and the same pattern as tools/crossbuild2.sh, but only
# for these two programs. ARTIFACTS: dist/sdr-load and dist/iqmode.
cross:
	mkdir -p dist
	docker run --rm -v "$(CURDIR)":/src -v "$(CURDIR)/dist":/dist debian:sid sh -c '\
	  dpkg --add-architecture arm64 >/dev/null && \
	  apt-get update -qq && \
	  apt-get install -y -qq --no-install-recommends crossbuild-essential-arm64 >/dev/null && \
	  aarch64-linux-gnu-g++ -std=c++17 -O2 -Wall -Wextra -o /dist/sdr-load /src/sdr-load.cpp && \
	  aarch64-linux-gnu-g++ -std=c++17 -O2 -Wall -Wextra -o /dist/iqmode /src/iqmode.cpp'

install: sdr-load iqmode
	install -d $(DESTDIR)$(BINDIR)
	install -m755 sdr-load $(DESTDIR)$(BINDIR)/sdr-load
	install -m755 iqmode $(DESTDIR)$(BINDIR)/iqmode
	install -d $(DESTDIR)$(DATADIR)
	install -m755 flash_modem.sh $(DESTDIR)$(DATADIR)/flash_modem.sh

clean:
	rm -f $(PROGS)
	rm -rf dist
