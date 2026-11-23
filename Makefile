CC      ?= clang
EXTRA_CFLAGS ?=
WERROR  ?=
ifeq ($(WERROR),1)
EXTRA_CFLAGS += -Werror
endif

# Version scheme: SemVer tags `vMAJOR.MINOR.PATCH` on main. A tag gives the
# exact version; a commit after the latest tag gives `vX.Y.Z-N-gHASH`, and a
# dirty tree appends `-dirty`. Override with `make VERSION=v0.1.0`.
VERSION     ?= $(shell git describe --tags --always --dirty 2>/dev/null || echo dev)
VERSION_NUM := $(patsubst v%,%,$(VERSION))
BUILD_NUM   ?= $(shell git rev-list --count HEAD 2>/dev/null || echo 1)

CFLAGS  ?= -std=c99 -Wall -Wextra -O2 -Ivendor $(shell pkg-config --cflags sdl2)
CFLAGS  += $(EXTRA_CFLAGS) '-DAPP_VERSION="$(VERSION)"'
LDLIBS  := $(shell pkg-config --libs sdl2) -lm

# Internet radio needs libcurl. Auto-detected; `make NET=0` builds the
# SDL2-only binary, where net.c's stubs report the network as unavailable.
NET ?= $(shell pkg-config --exists libcurl 2>/dev/null && echo 1 || echo 0)
ifeq ($(NET),1)
CFLAGS += -DHAVE_CURL $(shell pkg-config --cflags libcurl)
LDLIBS += $(shell pkg-config --libs libcurl)
endif

# AAC streams need FFmpeg's libavcodec (LGPL, linked dynamically). Auto-detected;
# `make AAC=0` builds without it, and AAC stations are then hidden from search.
AAC ?= $(shell pkg-config --exists libavcodec libavutil 2>/dev/null && echo 1 || echo 0)
ifeq ($(AAC),1)
CFLAGS += -DHAVE_AVCODEC $(shell pkg-config --cflags libavcodec libavutil)
LDLIBS += $(shell pkg-config --libs libavcodec libavutil)
endif

TARGET  := aldrem
SRC := src/main.c src/playback.c src/track.c src/track_wav.c src/track_mp3.c \
       src/track_flac.c src/track_ogg.c src/track_aiff.c src/file_browser.c \
       src/playlist.c src/theme.c src/vu.c src/viz.c src/visualizer.c src/settings.c src/finder.c \
       src/net.c src/radio.c src/radio_search.c src/track_stream.c \
       src/track_stream_aac.c src/tags.c vendor/stb_vorbis.c
OBJ := $(patsubst %.c,build/obj/%.o,$(SRC))

# Runtime window icon: rendered from packaging/aldrem.svg and baked into the
# binary with objcopy so the window shows an icon even outside a .desktop entry
# (e.g. `make run`). Skipped on Darwin, where the .app bundle's .icns is used.
UNAME_S := $(shell uname -s)
ifneq ($(UNAME_S),Darwin)
ICON_BMP := build/icon-256.bmp
ICON_OBJ := build/obj/icon.o
OBJ      += $(ICON_OBJ)
CFLAGS   += -DHAVE_EMBED_ICON
UNAME_M  := $(shell uname -m)
ifeq ($(UNAME_M),aarch64)
OBJCOPY_FORMAT := elf64-littleaarch64
OBJCOPY_ARCH   := aarch64
else
OBJCOPY_FORMAT := elf64-x86-64
OBJCOPY_ARCH   := i386:x86-64
endif
endif

# Launchers (GNOME Shell etc.) hand the app a startup-notification ID that
# SDL2's X11 backend never acknowledges, so the busy cursor and missing dock
# icon last until the launcher times out. Xlib lets main.c send the "startup
# complete" message. Auto-detected; `make X11=0` skips it.
ifneq ($(UNAME_S),Darwin)
X11 ?= $(shell pkg-config --exists x11 2>/dev/null && echo 1 || echo 0)
ifeq ($(X11),1)
CFLAGS += -DHAVE_X11 $(shell pkg-config --cflags x11)
LDLIBS += $(shell pkg-config --libs x11)
endif
endif

HDR := src/playback.h src/track.h src/track_internal.h src/track_stream_internal.h \
       src/file_browser.h src/playlist.h src/theme.h src/vu.h src/viz.h src/visualizer.h src/settings.h \
       src/finder.h src/net.h src/tags.h src/radio.h src/radio_search.h vendor/jsmn.h \
       src/nuklear_config.h vendor/nuklear.h \
       vendor/nuklear_sdl_renderer.h vendor/minimp3.h vendor/dr_flac.h \
       vendor/stb_vorbis.c

PREFIX   ?= $(HOME)/.local
BINDIR   ?= $(PREFIX)/bin
APPDIR   ?= $(PREFIX)/share/applications
ICONDIR  ?= $(PREFIX)/share/icons/hicolor/scalable/apps
DESKTOP  := build/$(TARGET).desktop
APP      := build/$(TARGET).app

# The decoder sources test_load.c links against, minus the UI. Stream tracks
# pull in net.c, so it is part of the set.
TEST_SRC := src/playback.c src/track.c src/track_wav.c src/track_mp3.c \
            src/track_flac.c src/track_ogg.c src/track_aiff.c \
            src/track_stream.c src/track_stream_aac.c src/net.c src/tags.c
TEST_OBJ := $(patsubst src/%.c,build/obj/src/%.o,$(TEST_SRC)) \
            build/obj/vendor/stb_vorbis.o

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): $(HDR)

ifdef ICON_BMP
$(ICON_BMP): packaging/make_icon.py packaging/aldrem.svg | build
	python3 packaging/make_icon.py --bmp 256 $@

$(ICON_OBJ): $(ICON_BMP) | build-obj
	objcopy -I binary -O $(OBJCOPY_FORMAT) -B $(OBJCOPY_ARCH) \
	  --rename-section .data=.rodata,alloc,load,readonly,data,contents \
	  --redefine-sym _binary_build_icon_256_bmp_start=aldrem_icon_start \
	  --redefine-sym _binary_build_icon_256_bmp_end=aldrem_icon_end \
	  --redefine-sym _binary_build_icon_256_bmp_size=aldrem_icon_size \
	  $< $@
endif

build/obj/%.o: %.c | build-obj
	$(CC) $(CFLAGS) -c -o $@ $<

build-obj:
	@mkdir -p build/obj build/obj/src build/obj/vendor

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

# The packaged Info.plist is the template with both version keys replaced, so
# no version literal is committed for the shipped bundle.
app: $(TARGET)
	@if [ "$$(uname -s)" != "Darwin" ]; then echo "app: macOS only (needs sips/iconutil)" >&2; exit 1; fi
	mkdir -p $(APP)/Contents/MacOS $(APP)/Contents/Resources build/$(TARGET).iconset
	cp $(TARGET) $(APP)/Contents/MacOS/$(TARGET)
	cp packaging/Info.plist $(APP)/Contents/Info.plist
	if command -v plutil >/dev/null 2>&1; then \
		plutil -replace CFBundleShortVersionString -string "$(VERSION_NUM)" $(APP)/Contents/Info.plist && \
		plutil -replace CFBundleVersion -string "$(BUILD_NUM)" $(APP)/Contents/Info.plist && \
		plutil -lint $(APP)/Contents/Info.plist; \
	else \
		sed -i '' -e '/<key>CFBundleShortVersionString<\/key>/{n;s|.*|<string>$(VERSION_NUM)</string>|;}' \
		             -e '/<key>CFBundleVersion<\/key>/{n;s|.*|<string>$(BUILD_NUM)</string>|;}' \
		             $(APP)/Contents/Info.plist; \
	fi
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

tests/test_load: tests/test_load.c $(TEST_OBJ)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_load.c $(TEST_OBJ) $(LDLIBS)

# Pure meter maths and settings round-trip; no audio device needed.
tests/test_vu: tests/test_vu.c src/vu.c src/vu.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_vu.c src/vu.c $(LDLIBS)

# Visualizer maths: FFT, bars, smoothing, colour; no device.
tests/test_viz: tests/test_viz.c src/viz.c src/viz.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_viz.c src/viz.c $(LDLIBS)

tests/test_settings: tests/test_settings.c src/settings.c src/settings.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_settings.c src/settings.c $(LDLIBS)

tests/test_finder: tests/test_finder.c src/finder.c src/finder.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_finder.c src/finder.c $(LDLIBS)

# Radio Browser parsing and URL building; no network needed.
tests/test_radio: tests/test_radio.c src/radio.c src/radio.h vendor/jsmn.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_radio.c src/radio.c $(LDLIBS)

# Playlist station entries: names kept in step with paths, #EXTINF and URL
# lines. Runs the real playlist.c against an isolated XDG config dir.
# Embedded title/artist readers, against synthetic tagged fixtures.
tests/test_tags: tests/test_tags.c src/tags.c src/tags.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_tags.c src/tags.c $(LDLIBS)

tests/test_playlist: tests/test_playlist.c build/obj/src/playlist.o $(TEST_OBJ)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_playlist.c build/obj/src/playlist.o $(TEST_OBJ) $(LDLIBS)

# Stream decoders (MP3, Ogg, AAC) against small committed fixtures in tests/data,
# fed through an in-memory StreamIO: no network, no sound device.
tests/test_stream: tests/test_stream.c $(TEST_OBJ)
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_stream.c $(TEST_OBJ) $(LDLIBS)

# Loopback HTTP tests for net.c. They need libcurl, so they exist only when
# NET=1; otherwise the test target skips them.
ifeq ($(NET),1)
TEST_NET_BIN := tests/test_net
tests/test_net: tests/test_net.c src/net.c src/net.h
	$(CC) $(CFLAGS) -Isrc -o $@ tests/test_net.c src/net.c $(LDLIBS)
endif

# Functional test only; the fixture is generated by the caller (CI) so a plain
# build never needs python3. `make test FIXTURE=path/to.wav` runs it.
# The decoder round-trip opens a device, so pin the dummy driver: CI runners
# and headless boxes have no sound card, and decoding does not need one.
test: $(TARGET) tests/test_load tests/test_vu tests/test_viz tests/test_settings tests/test_finder tests/test_radio tests/test_stream tests/test_playlist tests/test_tags $(TEST_NET_BIN)
	./tests/test_vu
	./tests/test_viz
	./tests/test_radio
	./tests/test_stream tests/data
	./tests/test_tags
	rm -rf build/test-playlist && ./tests/test_playlist build/test-playlist tests/data/tone.mp3
	$(if $(TEST_NET_BIN),./tests/test_net)
	rm -rf build/test-finder && mkdir -p build/test-finder && ./tests/test_finder build/test-finder
	mkdir -p build/test-xdg && XDG_CONFIG_HOME=build/test-xdg ./tests/test_settings
	@if [ -n "$(FIXTURE)" ]; then \
		SDL_AUDIODRIVER=dummy ./tests/test_load "$(FIXTURE)" || exit 1; \
	else \
		echo "test: FIXTURE not set, skipping decoder round-trip"; \
	fi
	sh tests/smoke.sh ./$(TARGET)

clean:
	rm -rf build/obj tests/test_load tests/test_vu tests/test_viz tests/test_settings tests/test_finder tests/test_radio tests/test_stream tests/test_playlist tests/test_tags tests/test_net $(TARGET)

.PHONY: run clean build build-obj install uninstall app test $(DESKTOP)
