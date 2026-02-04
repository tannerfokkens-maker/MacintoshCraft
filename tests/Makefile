# Test Makefile for bareiron optimizations

CC = gcc
CFLAGS = -Wall -Wextra -I../include -DTEST_BUILD -O2

# Source directory
SRC = ../src

all: test_worldgen test_chunk_cache bench_worldgen

# Standalone block_changes test
test_worldgen: test_worldgen.c
	$(CC) $(CFLAGS) -o $@ $<

# Full chunk generation test (links with worldgen.c)
test_chunk_cache: test_chunk_cache.c $(SRC)/worldgen.c $(SRC)/registries.c
	$(CC) $(CFLAGS) -o $@ $^

# Performance benchmark
bench_worldgen: bench_worldgen.c $(SRC)/worldgen.c $(SRC)/registries.c
	$(CC) $(CFLAGS) -o $@ $^

run: test_worldgen test_chunk_cache
	@echo "=== Running block changes tests ==="
	./test_worldgen
	@echo ""
	@echo "=== Running chunk cache tests ==="
	./test_chunk_cache

bench: bench_worldgen
	@echo "=== Running performance benchmark ==="
	./bench_worldgen

clean:
	rm -f test_worldgen test_chunk_cache bench_worldgen

.PHONY: all run bench clean
