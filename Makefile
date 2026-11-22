CC      := clang
CFLAGS  ?= -std=c99 -Wall -Wextra -O2 -Ivendor $(shell pkg-config --cflags sdl2)
LDLIBS  := $(shell pkg-config --libs sdl2)

mediaplayer: src/main.c vendor/nuklear.h vendor/nuklear_sdl_renderer.h
	$(CC) $(CFLAGS) -o $@ src/main.c $(LDLIBS)

run: mediaplayer
	./mediaplayer

clean:
	rm -f mediaplayer

.PHONY: run clean
