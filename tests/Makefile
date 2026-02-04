# Test Makefile for bareiron optimizations

CC = gcc
CFLAGS = -Wall -Wextra -I../include -DTEST_BUILD -O2

# Source directory
SRC = ../src

all: test_worldgen test_chunk_cache test_binary_search bench_worldgen

# Standalone block_changes test
test_worldgen: test_worldgen.c
	$(CC) $(CFLAGS) -o $@ $<

# Full chunk generation test (links with worldgen.c)
test_chunk_cache: test_chunk_cache.c $(SRC)/worldgen.c $(SRC)/registries.c
	$(CC) $(CFLAGS) -o $@ $^

# Binary search block changes test
test_binary_search: test_binary_search.c $(SRC)/registries.c
	$(CC) $(CFLAGS) -o $@ $^

# Performance benchmark
bench_worldgen: bench_worldgen.c $(SRC)/worldgen.c $(SRC)/registries.c
	$(CC) $(CFLAGS) -o $@ $^

run: test_worldgen test_chunk_cache test_binary_search
	@echo "=== Running block changes tests ==="
	./test_worldgen
	@echo ""
	@echo "=== Running chunk cache tests ==="
	./test_chunk_cache
	@echo ""
	@echo "=== Running binary search tests ==="
	./test_binary_search

bench: bench_worldgen
	@echo "=== Running performance benchmark ==="
	./bench_worldgen

clean:
	rm -f test_worldgen test_chunk_cache test_binary_search bench_worldgen

.PHONY: all run bench clean
