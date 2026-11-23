CC      := clang
CFLAGS  ?= -std=c99 -Wall -Wextra -O2 -Ivendor $(shell pkg-config --cflags sdl2)
LDLIBS  := $(shell pkg-config --libs sdl2) -lm

SRC := src/main.c src/playback.c src/track.c src/track_wav.c src/track_mp3.c \
       src/track_flac.c src/track_ogg.c src/file_browser.c vendor/stb_vorbis.c
OBJ := $(SRC:.c=.o)
HDR := src/playback.h src/track.h src/track_internal.h src/file_browser.h \
       src/nuklear_config.h vendor/nuklear.h vendor/nuklear_sdl_renderer.h \
       vendor/minimp3.h vendor/dr_flac.h vendor/stb_vorbis.c

PREFIX   ?= $(HOME)/.local
BINDIR   ?= $(PREFIX)/bin
APPDIR   ?= $(PREFIX)/share/applications
ICONDIR  ?= $(PREFIX)/share/icons/hicolor/scalable/apps
DESKTOP  := build/mediaplayer.desktop
APP      := build/mediaplayer.app

mediaplayer: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): $(HDR)

run: mediaplayer
	./mediaplayer

build:
	mkdir -p build

$(DESKTOP): packaging/mediaplayer.desktop.in mediaplayer | build
	sed 's|@BINDIR@|$(BINDIR)|g' $< > $@

install: mediaplayer $(DESKTOP)
	install -Dm755 mediaplayer $(DESTDIR)$(BINDIR)/mediaplayer
	install -Dm644 $(DESKTOP) $(DESTDIR)$(APPDIR)/mediaplayer.desktop
	install -Dm644 packaging/mediaplayer.svg $(DESTDIR)$(ICONDIR)/mediaplayer.svg
	-update-desktop-database $(DESTDIR)$(APPDIR)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/mediaplayer \
	      $(DESTDIR)$(APPDIR)/mediaplayer.desktop \
	      $(DESTDIR)$(ICONDIR)/mediaplayer.svg
	-update-desktop-database $(DESTDIR)$(APPDIR)

app: mediaplayer
	@if [ "$$(uname -s)" != "Darwin" ]; then echo "app: macOS only (needs sips/iconutil)" >&2; exit 1; fi
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources build/mediaplayer.iconset
	cp mediaplayer $(APP)/Contents/MacOS/mediaplayer
	cp packaging/Info.plist $(APP)/Contents/Info.plist
	python3 packaging/make_icon.py build/icon-1024.png
	sips -z 16 16 build/icon-1024.png --out build/mediaplayer.iconset/icon_16x16.png
	sips -z 32 32 build/icon-1024.png --out build/mediaplayer.iconset/icon_16x16@2x.png
	sips -z 32 32 build/icon-1024.png --out build/mediaplayer.iconset/icon_32x32.png
	sips -z 64 64 build/icon-1024.png --out build/mediaplayer.iconset/icon_32x32@2x.png
	sips -z 128 128 build/icon-1024.png --out build/mediaplayer.iconset/icon_128x128.png
	sips -z 256 256 build/icon-1024.png --out build/mediaplayer.iconset/icon_128x128@2x.png
	sips -z 256 256 build/icon-1024.png --out build/mediaplayer.iconset/icon_256x256.png
	sips -z 512 512 build/icon-1024.png --out build/mediaplayer.iconset/icon_256x256@2x.png
	sips -z 512 512 build/icon-1024.png --out build/mediaplayer.iconset/icon_512x512.png
	sips -z 1024 1024 build/icon-1024.png --out build/mediaplayer.iconset/icon_512x512@2x.png
	iconutil -c icns build/mediaplayer.iconset -o $(APP)/Contents/Resources/mediaplayer.icns

clean:
	rm -f $(OBJ) mediaplayer

.PHONY: run clean build install uninstall app $(DESKTOP)
