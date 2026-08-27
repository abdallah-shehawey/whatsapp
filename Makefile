# whatsapp -- lightweight WhatsApp Web client
#
# The generated inject.js.h keeps the page-side JavaScript in its own file
# instead of a C string literal, so it stays readable and lintable.

CC      ?= gcc
PKGS     = gtk4 webkitgtk-6.0
PREFIX  ?= $(HOME)/.local
DESTDIR ?=

CFLAGS  += -O2 -Wall -Wextra -Wno-unused-parameter $(shell pkg-config --cflags $(PKGS))
LDFLAGS += $(shell pkg-config --libs $(PKGS))

BIN = whatsapp
SRC = src/main.c src/tray.c
GEN = src/inject.js.h

all: $(BIN)

$(GEN): src/inject.js
	@python3 -c "d=open('src/inject.js','rb').read(); \
open('$(GEN)','w').write('/* generated from src/inject.js -- do not edit */\\n' \
'static const char WA_INJECT_JS[] = {' + ','.join(str(b) for b in d) + ',0};\\n')"
	@echo "  GEN  $(GEN)"

$(BIN): $(SRC) $(GEN)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)
	@echo "  CC   $(BIN)  ($$(stat -c%s $(BIN)) bytes)"

APP_ID  = io.github.shehawey.whatsapp

# Runtime paths, kept separate from DESTDIR so a staged package install still
# writes the real prefix into the desktop entry.
bindir       = $(PREFIX)/bin
appdir       = $(PREFIX)/share/applications
autostartdir = $(HOME)/.config/autostart

ICON_SIZES = 16 22 24 32 48 64 128 256
icontheme  = $(PREFIX)/share/icons/hicolor

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(bindir)/$(BIN)
	@# Tray icons land in both contexts: SNI hosts disagree on which they search.
	@for s in $(ICON_SIZES); do \
	  for f in apps/$(APP_ID).png apps/$(APP_ID)-tray.png \
	           status/$(APP_ID)-tray.png status/$(APP_ID)-tray-attention.png; do \
	    install -Dm644 data/icons/$$s/$$f $(DESTDIR)$(icontheme)/$${s}x$${s}/$$f; \
	  done; \
	done
	@install -d $(DESTDIR)$(appdir)
	@sed 's|@BINDIR@|$(bindir)|g' data/$(APP_ID).desktop > $(DESTDIR)$(appdir)/$(APP_ID).desktop
	@chmod 644 $(DESTDIR)$(appdir)/$(APP_ID).desktop
	@# Skip the caches when staging for a package; packaging scripts run them.
	@test -n "$(DESTDIR)" || update-desktop-database $(appdir) 2>/dev/null || true
	@test -n "$(DESTDIR)" || gtk-update-icon-cache -qtf $(icontheme) 2>/dev/null || true
	@echo "  INSTALL  $(DESTDIR)$(bindir)/$(BIN)  + $(words $(ICON_SIZES)) icon sizes"

# Start hidden at login: connected and in the tray, no window on screen.
autostart: install
	@install -d $(autostartdir)
	@sed 's|@BINDIR@|$(bindir)|g' data/$(APP_ID)-autostart.desktop > $(autostartdir)/$(APP_ID).desktop
	@chmod 644 $(autostartdir)/$(APP_ID).desktop
	@echo "  AUTOSTART  $(autostartdir)/$(APP_ID).desktop"

no-autostart:
	rm -f $(autostartdir)/$(APP_ID).desktop

uninstall: no-autostart
	rm -f $(DESTDIR)$(bindir)/$(BIN) $(DESTDIR)$(appdir)/$(APP_ID).desktop
	@for s in $(ICON_SIZES); do \
	  rm -f $(DESTDIR)$(icontheme)/$${s}x$${s}/apps/$(APP_ID).png \
	        $(DESTDIR)$(icontheme)/$${s}x$${s}/apps/$(APP_ID)-tray.png \
	        $(DESTDIR)$(icontheme)/$${s}x$${s}/status/$(APP_ID)-tray.png \
	        $(DESTDIR)$(icontheme)/$${s}x$${s}/status/$(APP_ID)-tray-attention.png; \
	done

icons:
	python3 tools/make-icons.py

clean:
	rm -f $(BIN) $(GEN)

.PHONY: all install autostart no-autostart uninstall icons clean
