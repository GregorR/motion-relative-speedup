CC=gcc
#CFLAGS=-O3 -g -Wall -Werror -ansi -pedantic -Wno-long-long -Wno-overlength-strings
CFLAGS=-O3 -g
LIBS=-lm

all: mrspeedup

mrspeedup: mrspeedup.c
	$(CC) $(CFLAGS) $< $(LIBS) -o $@

clean:
	rm mrspeedup
