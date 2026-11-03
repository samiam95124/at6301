#
# AT6301 front panel, built on the AMI tool kit (graphical model, static).
#
# AMIDIR is where amitk lives; ". setpath" in amitk sets it, otherwise the
# default below is used.
#
# Linux: the graphical library holds whichever backend amitk was built with
# (lib/graph_backend in amitk records it: wayland, x11 or fb), and the link
# follows that record.
#
AMIDIR ?= $(HOME)/projects/amitk

OSTYPE := $(shell uname)
AMILIB = $(AMIDIR)/lib/libami_graph.a

ifeq ($(OSTYPE),Darwin)
    SSL_PREFIX := $(shell /opt/homebrew/bin/brew --prefix openssl@3)
    LIBS = $(SSL_PREFIX)/lib/libssl.dylib $(SSL_PREFIX)/lib/libcrypto.dylib \
           -framework CoreFoundation -framework CoreGraphics \
           -framework ImageIO -framework CoreMIDI -framework AudioToolbox \
           -framework Cocoa -framework CoreText -framework QuartzCore \
           -framework IOKit
    KEEP = -Wl,-u,_ami_maxx
    AMILINK = -Wl,-force_load,$(AMILIB) -Wl,-e,_pa_entry
else
    # which backend the library was built with
    GRAPHICS_BACKEND := $(shell cat $(AMIDIR)/lib/graph_backend 2>/dev/null)
    ifeq ($(GRAPHICS_BACKEND),wayland)
        GBACKLIBS = -lwayland-client -lwayland-cursor -lxkbcommon \
                    -lfreetype -lfontconfig
    else ifeq ($(GRAPHICS_BACKEND),fb)
        GBACKLIBS = -lfreetype -lfontconfig -lpng -lz
    else
        GBACKLIBS = -lX11 -lXtst -lfreetype -lfontconfig
    endif
    LIBS = -lasound -lfluidsynth -lssl -lcrypto $(GBACKLIBS) -lm -lpthread
    # keeper.o references the core, so the library's event loop stays in
    KEEP = $(AMIDIR)/stub/keeper.o -Wl,-u,ami_maxx
    AMILINK = $(AMILIB)
endif

CC = gcc
CFLAGS = -g3 -Wall -I$(AMIDIR)/include -I$(AMIDIR)/libc

SRCS = at6301.c psu.c

all: at6301

at6301: $(SRCS) psu.h $(AMILIB)
	$(CC) $(CFLAGS) $(SRCS) $(KEEP) $(AMILINK) $(LIBS) -lm -o $@

sim: at6301
	./at6301 --sim

ifeq ($(OSTYPE),Darwin)

#
# Menu bar launcher (macOS): a bolt in the menu bar that opens the panel.
# "make install" puts it in ~/Applications and starts it at login.
#
BAR = AT6301Bar.app

menubar: $(BAR)

$(BAR): menubar.m at6301
	rm -rf $(BAR)
	mkdir -p $(BAR)/Contents/MacOS
	clang -fobjc-arc -O2 -framework Cocoa menubar.m -o $(BAR)/Contents/MacOS/AT6301Bar
	sed 's|@DIR@|$(CURDIR)|' menubar.plist > $(BAR)/Contents/Info.plist
	codesign --force -s - $(BAR)

install: $(BAR)
	pkill -x AT6301Bar || true
	rm -rf $(HOME)/Applications/$(BAR)
	mkdir -p $(HOME)/Applications
	cp -R $(BAR) $(HOME)/Applications/
	osascript -e 'tell application "System Events" to if not (exists login item "AT6301Bar") then make login item at end with properties {path:"$(HOME)/Applications/$(BAR)", hidden:true}'
	open $(HOME)/Applications/$(BAR)

uninstall:
	pkill -x AT6301Bar || true
	osascript -e 'tell application "System Events" to if exists login item "AT6301Bar" then delete login item "AT6301Bar"'
	rm -rf $(HOME)/Applications/$(BAR)

else

#
# Desktop entries (Linux): "AT6301" and "AT6301 (simulated)" in the
# application menu. They run the program from this folder, so it finds
# ami.cfg; run "make install" again if you move the folder.
#
APPDIR = $(HOME)/.local/share/applications

install: at6301
	mkdir -p $(APPDIR)
	sed 's|@DIR@|$(CURDIR)|' at6301.desktop > $(APPDIR)/at6301.desktop
	sed 's|@DIR@|$(CURDIR)|' at6301-sim.desktop > $(APPDIR)/at6301-sim.desktop
	-update-desktop-database $(APPDIR) 2>/dev/null

uninstall:
	rm -f $(APPDIR)/at6301.desktop $(APPDIR)/at6301-sim.desktop
	-update-desktop-database $(APPDIR) 2>/dev/null

menubar:
	@echo "The menu bar launcher is macOS only; use make install for desktop entries."

endif

clean:
	rm -rf at6301 at6301.dSYM $(BAR)

.PHONY: all sim menubar install uninstall clean
