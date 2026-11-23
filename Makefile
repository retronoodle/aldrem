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

TARGET  := aldrem
SRC := src/main.c src/playback.c src/track.c src/track_wav.c src/track_mp3.c \
       src/track_flac.c src/track_ogg.c src/track_aiff.c src/file_browser.c \
       src/playlist.c vendor/stb_vorbis.c
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

# The decoder sources test_load.c links against, minus the UI.
TEST_SRC := src/playback.c src/track.c src/track_wav.c src/track_mp3.c \
            src/track_flac.c src/track_ogg.c src/track_aiff.c
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

# Functional test only; the fixture is generated by the caller (CI) so a plain
# build never needs python3. `make test FIXTURE=path/to.wav` runs it.
# The decoder round-trip opens a device, so pin the dummy driver: CI runners
# and headless boxes have no sound card, and decoding does not need one.
test: $(TARGET) tests/test_load
	@if [ -n "$(FIXTURE)" ]; then \
		SDL_AUDIODRIVER=dummy ./tests/test_load "$(FIXTURE)" || exit 1; \
	else \
		echo "test: FIXTURE not set, skipping decoder round-trip"; \
	fi
	sh tests/smoke.sh ./$(TARGET)

clean:
	rm -rf build/obj tests/test_load $(TARGET)

.PHONY: run clean build build-obj install uninstall app test $(DESKTOP)
