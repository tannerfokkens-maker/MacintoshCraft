/*
 * test_worldgen.c - Test harness for worldgen optimizations
 *
 * Verifies that optimized code produces identical output to original.
 * Run on host system (not 68k) to validate before deploying.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Include necessary headers */
#include "../include/globals.h"
#include "../include/registries.h"

/* Define variables */
uint32_t world_seed = 0xA103DE6C;
uint32_t rng_seed = 0xE2B9419;
BlockChange block_changes[MAX_BLOCK_CHANGES];
int block_changes_count = 0;

/* From tools.c */
uint64_t splitmix64(uint64_t state) {
    uint64_t z = state + 0x9e3779b97f4a7c15;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
}

uint32_t fast_rand() {
    rng_seed ^= rng_seed << 13;
    rng_seed ^= rng_seed >> 17;
    rng_seed ^= rng_seed << 5;
    return rng_seed;
}

/* Forward declarations from worldgen.c */
extern uint8_t chunk_section[4096];
extern uint8_t buildChunkSection(int cx, int cy, int cz);

/* Reference implementation of block_changes loop (original) */
void apply_block_changes_original(int cx, int cy, int cz, uint8_t *section) {
    for (int i = 0; i < block_changes_count; i++) {
        if (block_changes[i].block == 0xFF) continue;
        if (block_changes[i].block == B_torch) continue;
        #ifdef ALLOW_CHESTS
        if (block_changes[i].block == B_chest) continue;
        #endif
        if (
            block_changes[i].x >= cx && block_changes[i].x < cx + 16 &&
            block_changes[i].y >= cy && block_changes[i].y < cy + 16 &&
            block_changes[i].z >= cz && block_changes[i].z < cz + 16
        ) {
            int dx = block_changes[i].x - cx;
            int dy = block_changes[i].y - cy;
            int dz = block_changes[i].z - cz;
            unsigned address = (unsigned)(dx + (dz << 4) + (dy << 8));
            unsigned index = (address & ~7u) | (7u - (address & 7u));
            section[index] = block_changes[i].block;
        }
    }
}

/* Optimized implementation with early exit and bounds tracking */
void apply_block_changes_optimized(int cx, int cy, int cz, uint8_t *section) {
    /* Early exit if no block changes */
    if (block_changes_count == 0) return;

    /* Calculate chunk bounds for quick rejection */
    int cx_max = cx + 16;
    int cy_max = cy + 16;
    int cz_max = cz + 16;

    for (int i = 0; i < block_changes_count; i++) {
        uint8_t block = block_changes[i].block;

        /* Skip unallocated and special blocks */
        if (block == 0xFF) continue;
        if (block == B_torch) continue;
        #ifdef ALLOW_CHESTS
        if (block == B_chest) continue;
        #endif

        /* Bounds check with pre-computed limits */
        short bx = block_changes[i].x;
        short bz = block_changes[i].z;
        uint8_t by = block_changes[i].y;

        if (bx < cx || bx >= cx_max) continue;
        if (by < cy || by >= cy_max) continue;
        if (bz < cz || bz >= cz_max) continue;

        /* Apply block change */
        int dx = bx - cx;
        int dy = by - cy;
        int dz = bz - cz;
        unsigned address = (unsigned)(dx + (dz << 4) + (dy << 8));
        unsigned index = (address & ~7u) | (7u - (address & 7u));
        section[index] = block;
    }
}

/* Test helper: compare two chunk sections */
int compare_sections(uint8_t *a, uint8_t *b) {
    for (int i = 0; i < 4096; i++) {
        if (a[i] != b[i]) {
            printf("MISMATCH at index %d: original=%d, optimized=%d\n", i, a[i], b[i]);
            return 0;
        }
    }
    return 1;
}

/* Test helper: fill section with pattern */
void fill_section(uint8_t *section, uint8_t value) {
    for (int i = 0; i < 4096; i++) {
        section[i] = value;
    }
}

/* Test 1: Empty block_changes */
int test_empty_block_changes(void) {
    printf("Test 1: Empty block_changes... ");

    uint8_t original[4096], optimized[4096];
    block_changes_count = 0;

    fill_section(original, 0xAA);
    fill_section(optimized, 0xAA);

    apply_block_changes_original(0, 0, 0, original);
    apply_block_changes_optimized(0, 0, 0, optimized);

    if (compare_sections(original, optimized)) {
        printf("PASS\n");
        return 1;
    }
    printf("FAIL\n");
    return 0;
}

/* Test 2: Single block change in range */
int test_single_block_in_range(void) {
    printf("Test 2: Single block in range... ");

    uint8_t original[4096], optimized[4096];

    block_changes_count = 1;
    block_changes[0].x = 8;
    block_changes[0].y = 8;
    block_changes[0].z = 8;
    block_changes[0].block = B_stone;

    fill_section(original, 0xAA);
    fill_section(optimized, 0xAA);

    apply_block_changes_original(0, 0, 0, original);
    apply_block_changes_optimized(0, 0, 0, optimized);

    if (compare_sections(original, optimized)) {
        printf("PASS\n");
        return 1;
    }
    printf("FAIL\n");
    return 0;
}

/* Test 3: Block change out of range */
int test_block_out_of_range(void) {
    printf("Test 3: Block out of range... ");

    uint8_t original[4096], optimized[4096];

    block_changes_count = 1;
    block_changes[0].x = 100;  /* Out of 0-15 range */
    block_changes[0].y = 8;
    block_changes[0].z = 8;
    block_changes[0].block = B_stone;

    fill_section(original, 0xAA);
    fill_section(optimized, 0xAA);

    apply_block_changes_original(0, 0, 0, original);
    apply_block_changes_optimized(0, 0, 0, optimized);

    if (compare_sections(original, optimized)) {
        printf("PASS\n");
        return 1;
    }
    printf("FAIL\n");
    return 0;
}

/* Test 4: Multiple blocks, mixed in/out of range */
int test_multiple_blocks_mixed(void) {
    printf("Test 4: Multiple blocks mixed... ");

    uint8_t original[4096], optimized[4096];

    block_changes_count = 5;

    /* In range */
    block_changes[0].x = 0; block_changes[0].y = 0; block_changes[0].z = 0;
    block_changes[0].block = B_dirt;

    block_changes[1].x = 15; block_changes[1].y = 15; block_changes[1].z = 15;
    block_changes[1].block = B_stone;

    /* Out of range */
    block_changes[2].x = 16; block_changes[2].y = 0; block_changes[2].z = 0;
    block_changes[2].block = B_cobblestone;

    /* Unallocated (0xFF) */
    block_changes[3].x = 5; block_changes[3].y = 5; block_changes[3].z = 5;
    block_changes[3].block = 0xFF;

    /* In range */
    block_changes[4].x = 7; block_changes[4].y = 7; block_changes[4].z = 7;
    block_changes[4].block = B_sand;

    fill_section(original, 0xAA);
    fill_section(optimized, 0xAA);

    apply_block_changes_original(0, 0, 0, original);
    apply_block_changes_optimized(0, 0, 0, optimized);

    if (compare_sections(original, optimized)) {
        printf("PASS\n");
        return 1;
    }
    printf("FAIL\n");
    return 0;
}

/* Test 5: Negative coordinates */
int test_negative_coordinates(void) {
    printf("Test 5: Negative coordinates... ");

    uint8_t original[4096], optimized[4096];

    block_changes_count = 2;

    /* Block at negative chunk */
    block_changes[0].x = -8; block_changes[0].y = 8; block_changes[0].z = -8;
    block_changes[0].block = B_dirt;

    /* Block at origin chunk */
    block_changes[1].x = 8; block_changes[1].y = 8; block_changes[1].z = 8;
    block_changes[1].block = B_stone;

    fill_section(original, 0xAA);
    fill_section(optimized, 0xAA);

    /* Test chunk at negative coords */
    apply_block_changes_original(-16, 0, -16, original);
    apply_block_changes_optimized(-16, 0, -16, optimized);

    if (compare_sections(original, optimized)) {
        printf("PASS\n");
        return 1;
    }
    printf("FAIL\n");
    return 0;
}

/* Test 6: Torch blocks (should be skipped) */
int test_torch_skip(void) {
    printf("Test 6: Torch blocks skipped... ");

    uint8_t original[4096], optimized[4096];

    block_changes_count = 2;

    block_changes[0].x = 5; block_changes[0].y = 5; block_changes[0].z = 5;
    block_changes[0].block = B_torch;  /* Should be skipped */

    block_changes[1].x = 10; block_changes[1].y = 10; block_changes[1].z = 10;
    block_changes[1].block = B_stone;  /* Should apply */

    fill_section(original, 0xAA);
    fill_section(optimized, 0xAA);

    apply_block_changes_original(0, 0, 0, original);
    apply_block_changes_optimized(0, 0, 0, optimized);

    if (compare_sections(original, optimized)) {
        printf("PASS\n");
        return 1;
    }
    printf("FAIL\n");
    return 0;
}

/* Test 7: Large block_changes_count with sparse data */
int test_large_sparse(void) {
    printf("Test 7: Large sparse block_changes... ");

    uint8_t original[4096], optimized[4096];

    /* Fill with unallocated entries */
    for (int i = 0; i < MAX_BLOCK_CHANGES; i++) {
        block_changes[i].block = 0xFF;
    }

    /* Add a few real entries scattered throughout */
    block_changes[0].x = 1; block_changes[0].y = 1; block_changes[0].z = 1;
    block_changes[0].block = B_dirt;

    block_changes[5000].x = 5; block_changes[5000].y = 5; block_changes[5000].z = 5;
    block_changes[5000].block = B_stone;

    block_changes[19999].x = 10; block_changes[19999].y = 10; block_changes[19999].z = 10;
    block_changes[19999].block = B_sand;

    block_changes_count = MAX_BLOCK_CHANGES;

    fill_section(original, 0xAA);
    fill_section(optimized, 0xAA);

    apply_block_changes_original(0, 0, 0, original);
    apply_block_changes_optimized(0, 0, 0, optimized);

    if (compare_sections(original, optimized)) {
        printf("PASS\n");
        return 1;
    }
    printf("FAIL\n");
    return 0;
}

/* Test 8: Edge cases at chunk boundaries */
int test_chunk_boundaries(void) {
    printf("Test 8: Chunk boundary edges... ");

    uint8_t original[4096], optimized[4096];

    block_changes_count = 4;

    /* Exactly at boundaries */
    block_changes[0].x = 0; block_changes[0].y = 0; block_changes[0].z = 0;
    block_changes[0].block = B_dirt;

    block_changes[1].x = 15; block_changes[1].y = 0; block_changes[1].z = 0;
    block_changes[1].block = B_stone;

    block_changes[2].x = 0; block_changes[2].y = 15; block_changes[2].z = 15;
    block_changes[2].block = B_sand;

    block_changes[3].x = 15; block_changes[3].y = 15; block_changes[3].z = 15;
    block_changes[3].block = B_cobblestone;

    fill_section(original, 0xAA);
    fill_section(optimized, 0xAA);

    apply_block_changes_original(0, 0, 0, original);
    apply_block_changes_optimized(0, 0, 0, optimized);

    if (compare_sections(original, optimized)) {
        printf("PASS\n");
        return 1;
    }
    printf("FAIL\n");
    return 0;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("=== Worldgen Optimization Tests ===\n\n");

    /* Hash seeds like the real code does */
    world_seed = splitmix64(world_seed);
    rng_seed = splitmix64(rng_seed);

    int passed = 0;
    int total = 8;

    passed += test_empty_block_changes();
    passed += test_single_block_in_range();
    passed += test_block_out_of_range();
    passed += test_multiple_blocks_mixed();
    passed += test_negative_coordinates();
    passed += test_torch_skip();
    passed += test_large_sparse();
    passed += test_chunk_boundaries();

    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);

    return (passed == total) ? 0 : 1;
}
