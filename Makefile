PKG_CONFIG ?= pkg-config

BASE_CFLAGS = -Wall -Wextra -Wpedantic -Werror -Wno-missing-field-initializers -O2 $(shell $(PKG_CONFIG) --cflags gdk-pixbuf-2.0 glib-2.0 libnotify libswscale mpv)
BASE_LDFLAGS = $(shell $(PKG_CONFIG) --libs gdk-pixbuf-2.0 glib-2.0 libnotify libswscale)

SCRIPTS_DIR := $(HOME)/.config/mpv/scripts

PREFIX := /usr/local
PLUGINDIR := $(PREFIX)/lib/mpv-notification-osd
SYS_SCRIPTS_DIR := /etc/mpv/scripts

UID ?= $(shell id -u)

.PHONY: install install-user install-system \
	uninstall uninstall-user uninstall-system \
	clean

notification-osd.so: notification-osd.c
	$(CC) -o notification-osd.so notification-osd.c $(BASE_CFLAGS) $(CFLAGS) $(BASE_LDFLAGS) $(LDFLAGS) -shared -fPIC

ifneq ($(UID),0)
install: install-user
uninstall: uninstall-user
else
install: install-system
uninstall: uninstall-system
endif

install-user: notification-osd.so
	install -Dm755 -t $(SCRIPTS_DIR) notification-osd.so

uninstall-user: notification-osd.so
	$(RM) $(SCRIPTS_DIR)/notification-osd.so

install-system: notification-osd.so
	install -Dm755 -t $(DESTDIR)$(PLUGINDIR) notification-osd.so
	mkdir -p $(DESTDIR)$(SYS_SCRIPTS_DIR)
	ln -s $(PLUGINDIR)/notification-osd.so $(DESTDIR)$(SYS_SCRIPTS_DIR)

uninstall-system: notification-osd.so
	$(RM) $(DESTDIR)$(SYS_SCRIPTS_DIR)/notification-osd.so
	$(RM) $(DESTDIR)$(PLUGINDIR)/notification-osd.so
	-rmdir $(DESTDIR)$(PLUGINDIR) 2>/dev/null

clean:
	$(RM) notification-osd.so
