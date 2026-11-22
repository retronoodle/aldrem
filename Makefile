CC      := clang
CFLAGS  ?= -std=c99 -Wall -Wextra -O2 -Ivendor $(shell pkg-config --cflags sdl2)
LDLIBS  := $(shell pkg-config --libs sdl2)

SRC := src/main.c src/playback.c src/track_wav.c
OBJ := $(SRC:.c=.o)
HDR := src/playback.h src/track.h vendor/nuklear.h vendor/nuklear_sdl_renderer.h

mediaplayer: $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

$(OBJ): $(HDR)

run: mediaplayer
	./mediaplayer

clean:
	rm -f $(OBJ) mediaplayer

.PHONY: run clean
