CC ?= gcc
PKG_CONFIG ?= pkg-config

CFLAGS ?= -O2 -Wall -Wextra -std=c11
CPPFLAGS += -D_DEFAULT_SOURCE
CPPFLAGS += -Iinclude
LDLIBS_X11 := $(shell $(PKG_CONFIG) --libs x11 xext xrender 2>/dev/null || printf '%s\n' -lX11 -lXext -lXrender)
CFLAGS_X11 := $(shell $(PKG_CONFIG) --cflags x11 xext xrender 2>/dev/null)

PREFIX ?= $(HOME)/.local
BINDIR ?= $(PREFIX)/bin
CONFIGDIR ?= $(HOME)/.config/mc-resizer
SYSTEMD_USER_DIR ?= $(HOME)/.config/systemd/user
SRCDIR := src
INCLUDEDIR := include
CONFIGSRC := config/mc-resizer.conf
SERVICESRC := systemd/mc-resizerd.service

.PHONY: all clean install install-service

all: mc-resizerd mc-resizerctl

mc-resizerd: $(SRCDIR)/mc-resizerd.c $(SRCDIR)/mc-centering.c $(INCLUDEDIR)/mc-centering.h $(SRCDIR)/mc-overlay.c $(INCLUDEDIR)/mc-overlay.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CFLAGS_X11) -o $@ $(SRCDIR)/mc-resizerd.c $(SRCDIR)/mc-centering.c $(SRCDIR)/mc-overlay.c $(LDLIBS_X11)

mc-resizerctl: $(SRCDIR)/mc-resizerctl.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

install: all
	install -d "$(BINDIR)" "$(CONFIGDIR)"
	install -m 0755 mc-resizerd mc-resizerctl "$(BINDIR)"
	@if [ ! -f "$(CONFIGDIR)/config" ]; then install -m 0644 "$(CONFIGSRC)" "$(CONFIGDIR)/config"; fi

install-service: install
	install -d "$(SYSTEMD_USER_DIR)"
	install -m 0644 "$(SERVICESRC)" "$(SYSTEMD_USER_DIR)/mc-resizerd.service"
	systemctl --user daemon-reload

clean:
	rm -f mc-resizerd mc-resizerctl
