# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Franklin Cordeiro

PKGS    := x11 xext xcomposite xdamage xfixes xrandr xi gl
PREFIX  ?= /usr/local

# CFLAGS/LDFLAGS are left to the user; the flags xcrt needs live separately
# so `make CFLAGS=...` cannot drop them.
CFLAGS  ?= -O2 -g
XCFLAGS := -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags $(PKGS))
XLIBS   := $(shell pkg-config --libs $(PKGS)) -lm

SRC     := $(wildcard src/*.c)
OBJ     := $(SRC:src/%.c=build/%.o) build/shaders.o
SHADERS := $(wildcard shaders/*.vert shaders/*.frag)

all: xcrt

xcrt: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(XLIBS) $(LDLIBS)

build/%.o: src/%.c $(wildcard src/*.h) | build
	$(CC) $(XCFLAGS) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# Embed every shader as a NUL-terminated C string: shaders/crt.frag -> shader_crt_frag
build/shaders.c: $(SHADERS) | build
	@{ for f in $(SHADERS); do \
	     n=$$(basename $$f | tr '.' '_'); \
	     echo "const char shader_$$n[] = {"; xxd -i < $$f; echo ", 0x00 };"; \
	   done; } > $@

build/shaders.o: build/shaders.c
	$(CC) $(XCFLAGS) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

install: xcrt
	install -Dm755 xcrt $(DESTDIR)$(PREFIX)/bin/xcrt
	install -Dm755 gui/xcrt-gui $(DESTDIR)$(PREFIX)/bin/xcrt-gui
	install -Dm644 gui/xcrt-gui.desktop $(DESTDIR)$(PREFIX)/share/applications/xcrt-gui.desktop
	install -Dm644 xcrt.conf.example $(DESTDIR)$(PREFIX)/share/doc/xcrt/xcrt.conf.example
	install -Dm644 LICENSE $(DESTDIR)$(PREFIX)/share/doc/xcrt/LICENSE

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/xcrt $(DESTDIR)$(PREFIX)/bin/xcrt-gui
	rm -f $(DESTDIR)$(PREFIX)/share/applications/xcrt-gui.desktop
	rm -rf $(DESTDIR)$(PREFIX)/share/doc/xcrt

# Cinnamon extension: per-user install, no root needed
EXT_UUID := xcrt@xcrt
EXT_DIR  ?= $(HOME)/.local/share/cinnamon/extensions/$(EXT_UUID)

install-cinnamon:
	install -d $(EXT_DIR)
	install -m644 cinnamon/$(EXT_UUID)/extension.js cinnamon/$(EXT_UUID)/metadata.json \
		cinnamon/$(EXT_UUID)/settings-schema.json cinnamon/$(EXT_UUID)/crt.glsl $(EXT_DIR)/

uninstall-cinnamon:
	rm -rf $(EXT_DIR)

clean:
	rm -rf build xcrt

.PHONY: all install uninstall install-cinnamon uninstall-cinnamon clean
