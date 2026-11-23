CC      := clang
CFLAGS  ?= -std=c99 -Wall -Wextra -O2 -Ivendor $(shell pkg-config --cflags sdl2)
LDLIBS  := $(shell pkg-config --libs sdl2) -lm

TARGET  := aldrem
SRC := src/main.c src/playback.c src/track.c src/track_wav.c src/track_mp3.c \
       src/track_flac.c src/track_ogg.c src/track_aiff.c src/file_browser.c \
       src/playlist.c vendor/stb_vorbis.c
OBJ := $(SRC:.c=.o)
HDR := src/playback.h src/track.h src/track_internal.h src/file_browser.h \
       src/playlist.h src/nuklear_config.h vendor/nuklear.h \
       vendor/nuklear_sdl_renderer.h vendor/minimp3.h vendor/dr_flac.h \
       vendor/stb_vorbis.c

PREFIX   ?= $(HOME)/.local
BINDIR   ?= $(PREFIX)/bin
APPDIR   ?= $(PREFIX)/share/applications
ICONDIR  ?= $(PREFIX)/share/icons/hicolor/scalable/apps
DESKTOP  := build/$(TARGET).desktop
APP      := build/$(TARGET).app

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): $(HDR)

run: $(TARGET)
	./$(TARGET)

build:
	mkdir -p build

$(DESKTOP): packaging/$(TARGET).desktop.in $(TARGET) | build
	sed 's|@BINDIR@|$(BINDIR)|g' $< > $@

install: $(TARGET) $(DESKTOP)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -Dm644 $(DESKTOP) $(DESTDIR)$(APPDIR)/$(TARGET).desktop
	install -Dm644 packaging/$(TARGET).svg $(DESTDIR)$(ICONDIR)/$(TARGET).svg
	-update-desktop-database $(DESTDIR)$(APPDIR)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET) \
	      $(DESTDIR)$(APPDIR)/$(TARGET).desktop \
	      $(DESTDIR)$(ICONDIR)/$(TARGET).svg
	-update-desktop-database $(DESTDIR)$(APPDIR)

app: $(TARGET)
	@if [ "$$(uname -s)" != "Darwin" ]; then echo "app: macOS only (needs sips/iconutil)" >&2; exit 1; fi
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources build/$(TARGET).iconset
	cp $(TARGET) $(APP)/Contents/MacOS/$(TARGET)
	cp packaging/Info.plist $(APP)/Contents/Info.plist
	python3 packaging/make_icon.py build/icon-1024.png
	sips -z 16 16 build/icon-1024.png --out build/$(TARGET).iconset/icon_16x16.png
	sips -z 32 32 build/icon-1024.png --out build/$(TARGET).iconset/icon_16x16@2x.png
	sips -z 32 32 build/icon-1024.png --out build/$(TARGET).iconset/icon_32x32.png
	sips -z 64 64 build/icon-1024.png --out build/$(TARGET).iconset/icon_32x32@2x.png
	sips -z 128 128 build/icon-1024.png --out build/$(TARGET).iconset/icon_128x128.png
	sips -z 256 256 build/icon-1024.png --out build/$(TARGET).iconset/icon_128x128@2x.png
	sips -z 256 256 build/icon-1024.png --out build/$(TARGET).iconset/icon_256x256.png
	sips -z 512 512 build/icon-1024.png --out build/$(TARGET).iconset/icon_256x256@2x.png
	sips -z 512 512 build/icon-1024.png --out build/$(TARGET).iconset/icon_512x512.png
	sips -z 1024 1024 build/icon-1024.png --out build/$(TARGET).iconset/icon_512x512@2x.png
	iconutil -c icns build/$(TARGET).iconset -o $(APP)/Contents/Resources/$(TARGET).icns

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: run clean build install uninstall app $(DESKTOP)
