#
# Makefile for Excise Boring Bits (ebb)
#
# To build, run:
#
#     $ make
#

CC=gcc
CFLAGS=-c -std=gnu99 -Wall -O2 -g \
	`pkg-config --cflags libavformat` \
	`pkg-config --cflags libavcodec` \
	`pkg-config --cflags libavutil` \
	`pkg-config --cflags libswscale`
LDLIBS= -lm -lpng -lz \
	`pkg-config --libs libavformat` \
	`pkg-config --libs libavcodec` \
	`pkg-config --libs libavutil` \
	`pkg-config --libs libswscale`

all: ebb

ebb: src/ebb.o
	$(CC) $(LDLIBS) src/ebb.o -o ebb

ebb.o: src/ebb.c
	$(CC) $(CFLAGS) src/ebb.c

clean:
	rm -rf *.o src/*.o ebb *~ src/*~

