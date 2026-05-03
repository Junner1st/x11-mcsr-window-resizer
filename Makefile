CC ?= gcc
PKG_CONFIG ?= pkg-config

CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS += -D_DEFAULT_SOURCE
LDLIBS_X11 := $(shell $(PKG_CONFIG) --libs x11 2>/dev/null || printf '%s\n' -lX11)
CFLAGS_X11 := $(shell $(PKG_CONFIG) --cflags x11 2>/dev/null)

PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
CONFIGDIR ?= $(HOME)/.config/mc-resizer
SYSTEMD_USER_DIR ?= $(HOME)/.config/systemd/user

.PHONY: all clean install install-service

all: mc-resizerd mc-resizerctl

mc-resizerd: mc-resizerd.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CFLAGS_X11) -o $@ $< $(LDLIBS_X11)

mc-resizerctl: mc-resizerctl.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

install: all
	install -d "$(BINDIR)" "$(CONFIGDIR)"
	install -m 0755 mc-resizerd mc-resizerctl "$(BINDIR)"
	@if [ ! -f "$(CONFIGDIR)/config" ]; then install -m 0644 mc-resizer.conf "$(CONFIGDIR)/config"; fi

install-service: install
	install -d "$(SYSTEMD_USER_DIR)"
	install -m 0644 mc-resizerd.service "$(SYSTEMD_USER_DIR)/mc-resizerd.service"
	systemctl --user daemon-reload

clean:
	rm -f mc-resizerd mc-resizerctl
