# Test Makefile for bareiron optimizations

CC = gcc
CFLAGS = -Wall -Wextra -I../include -DTEST_BUILD

# We need to compile a minimal set of sources for testing
SRCS = test_worldgen.c

all: test_worldgen

test_worldgen: test_worldgen.c
	$(CC) $(CFLAGS) -o $@ $<

run: test_worldgen
	./test_worldgen

clean:
	rm -f test_worldgen

.PHONY: all run clean
