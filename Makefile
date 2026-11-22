# rdp-launcher: plain POSIX make (GNU make and BSD make both work).
# OS differences are handled in C (src/platform.c), not here.

VERSION = 0.0.7

PREFIX  = $(HOME)/.local
BINDIR  = $(PREFIX)/bin
MANDIR  = $(PREFIX)/share/man/man1
DESTDIR =

CC      = cc
CFLAGS  = -O2 -std=c11 -Wall -Wextra -Wpedantic
LDFLAGS =

OBJS = src/main.o src/config.o src/probe.o src/client.o src/platform.o src/util.o

all: rdp-launcher

rdp-launcher: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

$(OBJS): src/rl.h Makefile

.SUFFIXES: .c .o
.c.o:
	$(CC) $(CFLAGS) $(CPPFLAGS) -DRL_VERSION=\"$(VERSION)\" -c -o $@ $<

check: rdp-launcher
	sh test/smoke.sh ./rdp-launcher

# The binary is copied then renamed: Linux refuses to overwrite a running
# binary in place ("Text file busy"), but replacing the directory entry is fine.
install: rdp-launcher
	mkdir -p "$(DESTDIR)$(BINDIR)" "$(DESTDIR)$(MANDIR)"
	cp rdp-launcher "$(DESTDIR)$(BINDIR)/.rdp-launcher.new"
	chmod 755 "$(DESTDIR)$(BINDIR)/.rdp-launcher.new"
	mv -f "$(DESTDIR)$(BINDIR)/.rdp-launcher.new" "$(DESTDIR)$(BINDIR)/rdp-launcher"
	cp rdp-launcher.1 "$(DESTDIR)$(MANDIR)/rdp-launcher.1"
	chmod 644 "$(DESTDIR)$(MANDIR)/rdp-launcher.1"
	@case ":$$PATH:" in *":$(BINDIR):"*) ;; \
	  *) echo "note: $(BINDIR) is not in your PATH";; esac

uninstall:
	rm -f "$(DESTDIR)$(BINDIR)/rdp-launcher" "$(DESTDIR)$(MANDIR)/rdp-launcher.1"

# Per-user clickable launcher: macOS .app or freedesktop .desktop entry.
install-desktop: install
	@if [ "$$(uname -s)" = Darwin ]; then \
	  app="$(HOME)/Applications/RDP Launcher.app"; \
	  rm -rf "$$app" && mkdir -p "$$app/Contents/MacOS" "$$app/Contents/Resources" && \
	  cp rdp-launcher "$$app/Contents/MacOS/rdp-launcher" && \
	  sed 's|@VERSION@|$(VERSION)|g' desktop/Info.plist > "$$app/Contents/Info.plist" && \
	  cp desktop/icon/AppIcon.icns "$$app/Contents/Resources/AppIcon.icns" && \
	  codesign --force -s - "$$app" 2>/dev/null && \
	  echo "installed $$app (allow Local Network access when macOS asks; again after each rebuild)"; \
	else \
	  dir="$(HOME)/.local/share/applications"; \
	  mkdir -p "$$dir" && \
	  sed -e 's|@BINDIR@|$(BINDIR)|g' \
	      -e 's|@ICON@|$(HOME)/.local/share/icons/hicolor/256x256/apps/rdp-launcher.png|g' \
	      desktop/rdp-launcher.desktop > "$$dir/rdp-launcher.desktop" && \
	  for sz in 48 128 256 512; do \
	    idir="$(HOME)/.local/share/icons/hicolor/$${sz}x$${sz}/apps"; mkdir -p "$$idir" && \
	    cp desktop/icon/rdp-launcher-$$sz.png "$$idir/rdp-launcher.png" || exit 1; \
	  done && \
	  mime="$(HOME)/.local/share/mime"; mkdir -p "$$mime/packages" && \
	  cp desktop/rdp-launcher-mime.xml "$$mime/packages/rdp-launcher.xml" && \
	  { command -v update-mime-database >/dev/null && update-mime-database "$$mime" || true; } && \
	  { command -v update-desktop-database >/dev/null && update-desktop-database "$$dir" || true; } && \
	  { command -v xdg-mime >/dev/null && xdg-mime default rdp-launcher.desktop application/x-rdp || true; } && \
	  echo "installed $$dir/rdp-launcher.desktop (+ opens .rdp files)"; \
	fi

uninstall-desktop:
	rm -rf "$(HOME)/Applications/RDP Launcher.app"
	rm -f "$(HOME)/.local/share/applications/rdp-launcher.desktop"
	rm -f "$(HOME)/.local/share/mime/packages/rdp-launcher.xml"
	rm -f "$(HOME)"/.local/share/icons/hicolor/*/apps/rdp-launcher.png
	-command -v update-mime-database >/dev/null && update-mime-database "$(HOME)/.local/share/mime"

clean:
	rm -f rdp-launcher $(OBJS) desktop/.launcher.tmp

.PHONY: all check install uninstall install-desktop uninstall-desktop clean
