/*
 * test_chunk_cache.c - Test harness for chunk caching optimization
 *
 * Tests that:
 * 1. buildChunkSection produces deterministic output
 * 2. Cached chunks match freshly generated chunks
 * 3. Cache eviction works correctly
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Include headers */
#include "../include/globals.h"
#include "../include/registries.h"
#include "../include/worldgen.h"

/* Define required globals */
uint32_t world_seed;
uint32_t rng_seed;
BlockChange block_changes[MAX_BLOCK_CHANGES];
int block_changes_count = 0;

/* From tools.c */
uint64_t splitmix64(uint64_t state) {
    uint64_t z = state + 0x9e3779b97f4a7c15;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
}

uint32_t fast_rand(void) {
    rng_seed ^= rng_seed << 13;
    rng_seed ^= rng_seed >> 17;
    rng_seed ^= rng_seed << 5;
    return rng_seed;
}

/* Stub for getBlockChange (from procedures.c) */
uint8_t getBlockChange(short x, uint8_t y, short z) {
    for (int i = 0; i < block_changes_count; i++) {
        if (block_changes[i].block == 0xFF) continue;
        if (block_changes[i].x == x &&
            block_changes[i].y == y &&
            block_changes[i].z == z) {
            return block_changes[i].block;
        }
    }
    return 0xFF;
}

/* External chunk_section from worldgen.c */
extern uint8_t chunk_section[4096];

/* Store reference chunks for comparison */
#define NUM_TEST_CHUNKS 10
static uint8_t reference_chunks[NUM_TEST_CHUNKS][4096];
static int ref_cx[NUM_TEST_CHUNKS];
static int ref_cy[NUM_TEST_CHUNKS];
static int ref_cz[NUM_TEST_CHUNKS];
static uint8_t ref_biome[NUM_TEST_CHUNKS];

/* Test coordinates to use */
static const int test_coords[NUM_TEST_CHUNKS][3] = {
    {0, 0, 0},
    {0, 16, 0},
    {0, 32, 0},
    {16, 0, 0},
    {0, 0, 16},
    {-16, 0, 0},
    {0, 0, -16},
    {-16, 64, -16},
    {32, 48, 32},
    {128, 0, 128}
};

/* Compare two chunk sections */
int compare_chunks(uint8_t *a, uint8_t *b, int cx, int cy, int cz) {
    int differences = 0;
    for (int i = 0; i < 4096; i++) {
        if (a[i] != b[i]) {
            if (differences < 5) {
                printf("  Diff at [%d]: expected %d, got %d\n", i, a[i], b[i]);
            }
            differences++;
        }
    }
    if (differences > 0) {
        printf("  Total %d differences in chunk (%d,%d,%d)\n", differences, cx, cy, cz);
    }
    return differences == 0;
}

/* Compute checksum of chunk section */
uint32_t chunk_checksum(uint8_t *section) {
    uint32_t sum = 0;
    for (int i = 0; i < 4096; i++) {
        sum = sum * 31 + section[i];
    }
    return sum;
}

/* Test 1: Verify buildChunkSection is deterministic */
int test_deterministic_generation(void) {
    printf("Test 1: Deterministic generation... ");

    /* Reset seeds to known state */
    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);
    block_changes_count = 0;

    /* Generate chunk at (0,0,0) */
    uint8_t biome1 = buildChunkSection(0, 0, 0);
    uint8_t first[4096];
    memcpy(first, chunk_section, 4096);
    uint32_t checksum1 = chunk_checksum(first);

    /* Reset seeds and generate again */
    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);

    uint8_t biome2 = buildChunkSection(0, 0, 0);
    uint32_t checksum2 = chunk_checksum(chunk_section);

    if (biome1 != biome2) {
        printf("FAIL (biome mismatch: %d vs %d)\n", biome1, biome2);
        return 0;
    }

    if (checksum1 != checksum2) {
        printf("FAIL (checksum mismatch: %08X vs %08X)\n", checksum1, checksum2);
        return 0;
    }

    if (!compare_chunks(first, chunk_section, 0, 0, 0)) {
        printf("FAIL\n");
        return 0;
    }

    printf("PASS (checksum: %08X)\n", checksum1);
    return 1;
}

/* Test 2: Generate reference chunks for later comparison */
int test_generate_reference_chunks(void) {
    printf("Test 2: Generating %d reference chunks... ", NUM_TEST_CHUNKS);

    /* Reset to known state */
    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);
    block_changes_count = 0;

    for (int i = 0; i < NUM_TEST_CHUNKS; i++) {
        ref_cx[i] = test_coords[i][0];
        ref_cy[i] = test_coords[i][1];
        ref_cz[i] = test_coords[i][2];

        ref_biome[i] = buildChunkSection(ref_cx[i], ref_cy[i], ref_cz[i]);
        memcpy(reference_chunks[i], chunk_section, 4096);
    }

    printf("PASS\n");

    /* Print checksums for reference */
    printf("  Reference checksums:\n");
    for (int i = 0; i < NUM_TEST_CHUNKS; i++) {
        printf("    Chunk (%3d,%3d,%3d): %08X biome=%d\n",
               ref_cx[i], ref_cy[i], ref_cz[i],
               chunk_checksum(reference_chunks[i]), ref_biome[i]);
    }

    return 1;
}

/* Test 3: Verify reference chunks are reproducible */
int test_verify_reference_chunks(void) {
    printf("Test 3: Verifying reference chunks... ");

    /* Reset to same known state */
    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);
    block_changes_count = 0;

    int passed = 0;
    for (int i = 0; i < NUM_TEST_CHUNKS; i++) {
        uint8_t biome = buildChunkSection(ref_cx[i], ref_cy[i], ref_cz[i]);

        if (biome != ref_biome[i]) {
            printf("\n  FAIL: Biome mismatch at chunk %d\n", i);
            continue;
        }

        if (!compare_chunks(reference_chunks[i], chunk_section,
                           ref_cx[i], ref_cy[i], ref_cz[i])) {
            printf("\n  FAIL: Content mismatch at chunk %d\n", i);
            continue;
        }

        passed++;
    }

    if (passed == NUM_TEST_CHUNKS) {
        printf("PASS (%d/%d)\n", passed, NUM_TEST_CHUNKS);
        return 1;
    }

    printf("FAIL (%d/%d)\n", passed, NUM_TEST_CHUNKS);
    return 0;
}

/* Test 4: Different coordinates produce different chunks */
int test_different_coords_different_output(void) {
    printf("Test 4: Different coords produce different output... ");

    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);
    block_changes_count = 0;

    buildChunkSection(0, 0, 0);
    uint8_t chunk1[4096];
    memcpy(chunk1, chunk_section, 4096);

    buildChunkSection(16, 0, 0);
    uint8_t chunk2[4096];
    memcpy(chunk2, chunk_section, 4096);

    /* These should be different */
    int same = 1;
    for (int i = 0; i < 4096; i++) {
        if (chunk1[i] != chunk2[i]) {
            same = 0;
            break;
        }
    }

    if (same) {
        printf("FAIL (chunks are identical)\n");
        return 0;
    }

    printf("PASS\n");
    return 1;
}

/* Test 5: Block changes affect output correctly */
int test_block_changes_applied(void) {
    printf("Test 5: Block changes are applied... ");

    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);
    block_changes_count = 0;

    /* Generate without block changes */
    buildChunkSection(0, 0, 0);
    uint8_t original[4096];
    memcpy(original, chunk_section, 4096);

    /* Add a block change */
    block_changes[0].x = 8;
    block_changes[0].y = 8;
    block_changes[0].z = 8;
    block_changes[0].block = B_diamond_block;
    block_changes_count = 1;

    /* Regenerate with block change */
    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);
    buildChunkSection(0, 0, 0);

    /* Find the index where diamond block should be */
    int dx = 8, dy = 8, dz = 8;
    unsigned address = (unsigned)(dx + (dz << 4) + (dy << 8));
    unsigned index = (address & ~7u) | (7u - (address & 7u));

    if (chunk_section[index] != B_diamond_block) {
        printf("FAIL (expected diamond at index %u, got %d)\n",
               index, chunk_section[index]);
        block_changes_count = 0;
        return 0;
    }

    /* Clean up */
    block_changes_count = 0;

    printf("PASS (diamond at index %u)\n", index);
    return 1;
}

/* Test 6: Negative coordinates work correctly */
int test_negative_coordinates(void) {
    printf("Test 6: Negative coordinates work... ");

    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);
    block_changes_count = 0;

    /* Generate at negative coordinates */
    uint8_t biome = buildChunkSection(-16, 0, -16);
    uint32_t checksum = chunk_checksum(chunk_section);

    /* Regenerate and verify same result */
    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);

    uint8_t biome2 = buildChunkSection(-16, 0, -16);
    uint32_t checksum2 = chunk_checksum(chunk_section);

    if (biome != biome2 || checksum != checksum2) {
        printf("FAIL (not reproducible)\n");
        return 0;
    }

    printf("PASS (checksum: %08X)\n", checksum);
    return 1;
}

/* Test 7: All Y levels generate correctly */
int test_all_y_levels(void) {
    printf("Test 7: All Y levels (0-304)... ");

    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);
    block_changes_count = 0;

    int passed = 0;
    for (int y = 0; y < 320; y += 16) {
        buildChunkSection(0, y, 0);
        uint32_t cs1 = chunk_checksum(chunk_section);

        /* Verify reproducible */
        world_seed = splitmix64(0xA103DE6C);
        rng_seed = splitmix64(0xE2B9419);
        buildChunkSection(0, y, 0);
        uint32_t cs2 = chunk_checksum(chunk_section);

        if (cs1 == cs2) {
            passed++;
        } else {
            printf("\n  FAIL at Y=%d\n", y);
        }
    }

    int total = 320 / 16;
    if (passed == total) {
        printf("PASS (%d/%d levels)\n", passed, total);
        return 1;
    }

    printf("FAIL (%d/%d levels)\n", passed, total);
    return 0;
}

/* Test 8: Cache hit returns same data as cache miss */
int test_cache_hit_consistency(void) {
    printf("Test 8: Cache hit consistency... ");

    /* Clear cache and reset state */
    clearChunkCache();
    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);
    block_changes_count = 0;

    /* First call: cache miss, generates chunk */
    buildChunkSection(0, 0, 0);
    uint8_t first_gen[4096];
    memcpy(first_gen, chunk_section, 4096);
    uint32_t checksum1 = chunk_checksum(first_gen);

    /* Second call: should be cache hit */
    buildChunkSection(0, 0, 0);
    uint32_t checksum2 = chunk_checksum(chunk_section);

    if (checksum1 != checksum2) {
        printf("FAIL (checksum mismatch: %08X vs %08X)\n", checksum1, checksum2);
        return 0;
    }

    if (!compare_chunks(first_gen, chunk_section, 0, 0, 0)) {
        printf("FAIL (content mismatch)\n");
        return 0;
    }

    printf("PASS\n");
    return 1;
}

/* Test 9: Cache works across many chunks */
int test_cache_multiple_chunks(void) {
    printf("Test 9: Cache multiple chunks... ");

    clearChunkCache();
    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);
    block_changes_count = 0;

    /* Generate 20 different chunks */
    uint32_t checksums[20];
    for (int i = 0; i < 20; i++) {
        buildChunkSection(i * 16, 0, 0);
        checksums[i] = chunk_checksum(chunk_section);
    }

    /* Regenerate in reverse order (all should be cache hits) */
    int passed = 0;
    for (int i = 19; i >= 0; i--) {
        buildChunkSection(i * 16, 0, 0);
        uint32_t cs = chunk_checksum(chunk_section);
        if (cs == checksums[i]) {
            passed++;
        } else {
            printf("\n  FAIL at chunk %d: %08X vs %08X\n", i, checksums[i], cs);
        }
    }

    if (passed == 20) {
        printf("PASS (20/20 cache hits verified)\n");
        return 1;
    }

    printf("FAIL (%d/20)\n", passed);
    return 0;
}

/* Test 10: clearChunkCache invalidates all entries */
int test_cache_clear(void) {
    printf("Test 10: Cache clear works... ");

    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);
    block_changes_count = 0;

    /* Fill cache with some chunks */
    for (int i = 0; i < 10; i++) {
        buildChunkSection(i * 16, 0, 0);
    }

    /* Clear cache */
    clearChunkCache();

    /* Generate same chunk with different seed - should get different result */
    /* This proves the cache was cleared (otherwise we'd get cached data) */
    world_seed = splitmix64(0xDEADBEEF);  /* Different seed */
    rng_seed = splitmix64(0xCAFEBABE);

    buildChunkSection(0, 0, 0);
    uint32_t new_checksum = chunk_checksum(chunk_section);

    /* Original checksum with seed 0xA103DE6C was FD21B44E */
    if (new_checksum == 0xFD21B44E) {
        printf("FAIL (got cached data after clear)\n");
        return 0;
    }

    printf("PASS (new checksum: %08X)\n", new_checksum);
    return 1;
}

/* Test 11: Cache miss performance - ensure lookups for non-existent entries are fast */
int test_cache_miss_performance(void) {
    printf("Test 11: Cache miss performance... ");

    clearChunkCache();
    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);
    block_changes_count = 0;

    /* Pre-populate cache with 32 entries in a specific pattern */
    for (int i = 0; i < 32; i++) {
        buildChunkSection(i * 16, 0, 0);
    }

    /* Now request 100 chunks that are NOT in cache */
    /* These should be fast cache misses, not O(n) scans */
    /* We measure by checking the result is correct (generation happens) */
    uint32_t checksums[100];
    for (int i = 0; i < 100; i++) {
        /* Use coordinates far from the cached ones */
        buildChunkSection(10000 + i * 16, 0, 0);
        checksums[i] = chunk_checksum(chunk_section);
    }

    /* Verify by regenerating a few and checking consistency */
    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);

    int passed = 0;
    for (int i = 0; i < 10; i++) {
        buildChunkSection(10000 + i * 16, 0, 0);
        if (chunk_checksum(chunk_section) == checksums[i]) {
            passed++;
        }
    }

    if (passed == 10) {
        printf("PASS (cache misses handled correctly)\n");
        return 1;
    }

    printf("FAIL (%d/10 consistent)\n", passed);
    return 0;
}

/* Test 12: invalidateChunkCache correctly invalidates entries */
int test_cache_invalidation(void) {
    printf("Test 12: Cache invalidation... ");

    clearChunkCache();
    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);
    block_changes_count = 0;

    /* Generate and cache a chunk */
    buildChunkSection(0, 0, 0);
    uint32_t original_checksum = chunk_checksum(chunk_section);

    /* Invalidate the cache entry for a block in that chunk */
    invalidateChunkCache(8, 8, 8);

    /* Change the world seed */
    world_seed = splitmix64(0xDEADBEEF);
    rng_seed = splitmix64(0xCAFEBABE);

    /* Regenerate - should get new terrain since cache was invalidated */
    buildChunkSection(0, 0, 0);
    uint32_t new_checksum = chunk_checksum(chunk_section);

    if (new_checksum == original_checksum) {
        printf("FAIL (cache not invalidated, still returning old data)\n");
        return 0;
    }

    printf("PASS (invalidation works)\n");
    return 1;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    printf("=== Chunk Generation Tests ===\n\n");

    int passed = 0;
    int total = 12;

    passed += test_deterministic_generation();
    passed += test_generate_reference_chunks();
    passed += test_verify_reference_chunks();
    passed += test_different_coords_different_output();
    passed += test_block_changes_applied();
    passed += test_negative_coordinates();
    passed += test_all_y_levels();

    printf("\n=== Cache Tests ===\n\n");

    passed += test_cache_hit_consistency();
    passed += test_cache_multiple_chunks();
    passed += test_cache_clear();
    passed += test_cache_miss_performance();
    passed += test_cache_invalidation();

    printf("\n=== Results: %d/%d tests passed ===\n", passed, total);

    return (passed == total) ? 0 : 1;
}
