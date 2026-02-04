/*
 * test_binary_search.c - Tests for binary search block changes optimization
 *
 * Tests both correctness (same results as linear search) and performance.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include "../include/globals.h"
#include "../include/registries.h"

/* Define required globals */
uint32_t world_seed;
uint32_t rng_seed;
BlockChange block_changes[MAX_BLOCK_CHANGES];
int block_changes_count = 0;

/* From tools.c */
uint32_t fast_rand(void) {
    rng_seed ^= rng_seed << 13;
    rng_seed ^= rng_seed >> 17;
    rng_seed ^= rng_seed << 5;
    return rng_seed;
}

/* ============================================================================
 * LINEAR SEARCH (Original Implementation)
 * ============================================================================ */

uint8_t getBlockChange_linear(short x, uint8_t y, short z) {
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

/* ============================================================================
 * BINARY SEARCH (New Implementation)
 * ============================================================================ */

/*
 * Compare two block changes by coordinates.
 * Sort order: x, then z, then y (groups nearby blocks together)
 * Returns: negative if a < b, zero if equal, positive if a > b
 */
static inline int compareBlockChangeCoords(short x1, uint8_t y1, short z1,
                                           short x2, uint8_t y2, short z2) {
    if (x1 != x2) return (x1 < x2) ? -1 : 1;
    if (z1 != z2) return (z1 < z2) ? -1 : 1;
    if (y1 != y2) return (y1 < y2) ? -1 : 1;
    return 0;
}

/*
 * Binary search for a block change.
 * Returns the index if found, or -1 if not found.
 * If insert_pos is not NULL, stores the insertion position for maintaining sort order.
 */
static int binarySearchBlockChange(short x, uint8_t y, short z, int *insert_pos) {
    int left = 0;
    int right = block_changes_count - 1;

    while (left <= right) {
        int mid = left + (right - left) / 2;

        /* Skip 0xFF entries by searching nearby */
        int actual_mid = mid;
        while (actual_mid <= right && block_changes[actual_mid].block == 0xFF) {
            actual_mid++;
        }
        if (actual_mid > right) {
            /* All entries from mid to right are 0xFF, search left */
            right = mid - 1;
            continue;
        }

        int cmp = compareBlockChangeCoords(
            x, y, z,
            block_changes[actual_mid].x,
            block_changes[actual_mid].y,
            block_changes[actual_mid].z
        );

        if (cmp == 0) {
            return actual_mid;  /* Found */
        } else if (cmp < 0) {
            right = actual_mid - 1;
        } else {
            left = actual_mid + 1;
        }
    }

    if (insert_pos) *insert_pos = left;
    return -1;  /* Not found */
}

/*
 * Get block change using binary search.
 * Array must be kept sorted for this to work correctly.
 */
uint8_t getBlockChange_binary(short x, uint8_t y, short z) {
    int idx = binarySearchBlockChange(x, y, z, NULL);
    if (idx >= 0 && block_changes[idx].block != 0xFF) {
        return block_changes[idx].block;
    }
    return 0xFF;
}

/*
 * Comparison function for qsort
 */
static int compareBlockChangeQsort(const void *a, const void *b) {
    const BlockChange *ba = (const BlockChange *)a;
    const BlockChange *bb = (const BlockChange *)b;

    /* Push 0xFF entries to the end */
    if (ba->block == 0xFF && bb->block != 0xFF) return 1;
    if (ba->block != 0xFF && bb->block == 0xFF) return -1;
    if (ba->block == 0xFF && bb->block == 0xFF) return 0;

    return compareBlockChangeCoords(ba->x, ba->y, ba->z, bb->x, bb->y, bb->z);
}

/*
 * Sort the block_changes array and compact (remove 0xFF entries)
 */
void sortBlockChanges(void) {
    if (block_changes_count == 0) return;

    /* Sort the array */
    qsort(block_changes, block_changes_count, sizeof(BlockChange), compareBlockChangeQsort);

    /* Compact: count valid entries (0xFF entries are now at the end) */
    int valid_count = 0;
    for (int i = 0; i < block_changes_count; i++) {
        if (block_changes[i].block != 0xFF) {
            valid_count++;
        } else {
            break;  /* 0xFF entries are at the end after sort */
        }
    }
    block_changes_count = valid_count;
}

/*
 * Insert a block change maintaining sorted order.
 * Returns 0 on success, 1 on failure (array full).
 */
int insertBlockChangeSorted(short x, uint8_t y, short z, uint8_t block) {
    int insert_pos;
    int existing = binarySearchBlockChange(x, y, z, &insert_pos);

    if (existing >= 0) {
        /* Update existing entry */
        if (block == 0xFF) {
            /* Deleting: shift elements left */
            for (int i = existing; i < block_changes_count - 1; i++) {
                block_changes[i] = block_changes[i + 1];
            }
            block_changes_count--;
        } else {
            block_changes[existing].block = block;
        }
        return 0;
    }

    /* New entry */
    if (block == 0xFF) return 0;  /* Nothing to delete */

    if (block_changes_count >= MAX_BLOCK_CHANGES) return 1;  /* Full */

    /* Shift elements right to make room */
    for (int i = block_changes_count; i > insert_pos; i--) {
        block_changes[i] = block_changes[i - 1];
    }

    /* Insert new entry */
    block_changes[insert_pos].x = x;
    block_changes[insert_pos].y = y;
    block_changes[insert_pos].z = z;
    block_changes[insert_pos].block = block;
    block_changes_count++;

    return 0;
}

/* ============================================================================
 * TESTS
 * ============================================================================ */

static int tests_passed = 0;
static int tests_total = 0;

static void test_result(const char *name, int passed) {
    tests_total++;
    if (passed) {
        tests_passed++;
        printf("Test %d: %s... PASS\n", tests_total, name);
    } else {
        printf("Test %d: %s... FAIL\n", tests_total, name);
    }
}

/* Clock helper */
static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* Test 1: Empty array */
void test_empty_array(void) {
    block_changes_count = 0;

    uint8_t linear = getBlockChange_linear(100, 64, 100);
    uint8_t binary = getBlockChange_binary(100, 64, 100);

    test_result("Empty array returns 0xFF", linear == 0xFF && binary == 0xFF);
}

/* Test 2: Single entry - found */
void test_single_entry_found(void) {
    block_changes_count = 1;
    block_changes[0].x = 50;
    block_changes[0].y = 64;
    block_changes[0].z = 50;
    block_changes[0].block = B_stone;

    uint8_t linear = getBlockChange_linear(50, 64, 50);
    uint8_t binary = getBlockChange_binary(50, 64, 50);

    test_result("Single entry found", linear == B_stone && binary == B_stone);
}

/* Test 3: Single entry - not found */
void test_single_entry_not_found(void) {
    block_changes_count = 1;
    block_changes[0].x = 50;
    block_changes[0].y = 64;
    block_changes[0].z = 50;
    block_changes[0].block = B_stone;

    uint8_t linear = getBlockChange_linear(100, 64, 100);
    uint8_t binary = getBlockChange_binary(100, 64, 100);

    test_result("Single entry not found", linear == 0xFF && binary == 0xFF);
}

/* Test 4: Multiple entries - all found correctly */
void test_multiple_entries(void) {
    block_changes_count = 0;

    /* Insert in sorted order for binary search */
    insertBlockChangeSorted(-100, 32, -100, B_dirt);
    insertBlockChangeSorted(0, 64, 0, B_stone);
    insertBlockChangeSorted(50, 64, 50, B_cobblestone);
    insertBlockChangeSorted(100, 64, 100, B_sand);
    insertBlockChangeSorted(200, 80, 200, B_grass_block);

    int all_match = 1;
    all_match &= (getBlockChange_linear(-100, 32, -100) == getBlockChange_binary(-100, 32, -100));
    all_match &= (getBlockChange_linear(0, 64, 0) == getBlockChange_binary(0, 64, 0));
    all_match &= (getBlockChange_linear(50, 64, 50) == getBlockChange_binary(50, 64, 50));
    all_match &= (getBlockChange_linear(100, 64, 100) == getBlockChange_binary(100, 64, 100));
    all_match &= (getBlockChange_linear(200, 80, 200) == getBlockChange_binary(200, 80, 200));
    all_match &= (getBlockChange_linear(999, 64, 999) == getBlockChange_binary(999, 64, 999));  /* Not found */

    test_result("Multiple entries match", all_match);
}

/* Test 5: Sorted insertion maintains order */
void test_sorted_insertion(void) {
    block_changes_count = 0;

    /* Insert out of order */
    insertBlockChangeSorted(100, 64, 100, B_stone);
    insertBlockChangeSorted(-50, 32, -50, B_dirt);
    insertBlockChangeSorted(50, 64, 50, B_sand);
    insertBlockChangeSorted(0, 64, 0, B_cobblestone);

    /* Verify array is sorted */
    int sorted = 1;
    for (int i = 1; i < block_changes_count; i++) {
        int cmp = compareBlockChangeCoords(
            block_changes[i-1].x, block_changes[i-1].y, block_changes[i-1].z,
            block_changes[i].x, block_changes[i].y, block_changes[i].z
        );
        if (cmp > 0) {
            sorted = 0;
            break;
        }
    }

    test_result("Sorted insertion maintains order", sorted && block_changes_count == 4);
}

/* Test 6: Update existing entry */
void test_update_existing(void) {
    block_changes_count = 0;

    insertBlockChangeSorted(50, 64, 50, B_stone);
    insertBlockChangeSorted(50, 64, 50, B_dirt);  /* Update */

    uint8_t result = getBlockChange_binary(50, 64, 50);

    test_result("Update existing entry", result == B_dirt && block_changes_count == 1);
}

/* Test 7: Delete entry */
void test_delete_entry(void) {
    block_changes_count = 0;

    insertBlockChangeSorted(50, 64, 50, B_stone);
    insertBlockChangeSorted(100, 64, 100, B_dirt);
    insertBlockChangeSorted(50, 64, 50, 0xFF);  /* Delete */

    uint8_t result = getBlockChange_binary(50, 64, 50);

    test_result("Delete entry", result == 0xFF && block_changes_count == 1);
}

/* Test 8: Negative coordinates */
void test_negative_coords(void) {
    block_changes_count = 0;

    insertBlockChangeSorted(-100, 64, -100, B_stone);
    insertBlockChangeSorted(-50, 32, 50, B_dirt);
    insertBlockChangeSorted(50, 64, -50, B_sand);

    int all_match = 1;
    all_match &= (getBlockChange_binary(-100, 64, -100) == B_stone);
    all_match &= (getBlockChange_binary(-50, 32, 50) == B_dirt);
    all_match &= (getBlockChange_binary(50, 64, -50) == B_sand);
    all_match &= (getBlockChange_binary(0, 64, 0) == 0xFF);

    test_result("Negative coordinates", all_match);
}

/* Test 9: Large dataset - correctness */
void test_large_dataset_correctness(void) {
    block_changes_count = 0;
    rng_seed = 12345;

    int count = 1000;

    /* Insert random entries */
    for (int i = 0; i < count; i++) {
        short x = (short)(fast_rand() % 1000) - 500;
        short z = (short)(fast_rand() % 1000) - 500;
        uint8_t y = fast_rand() % 256;
        uint8_t block = (fast_rand() % 64) + 1;  /* Avoid 0 and 0xFF */
        insertBlockChangeSorted(x, y, z, block);
    }

    /* Verify all entries can be found */
    int all_found = 1;
    for (int i = 0; i < block_changes_count; i++) {
        uint8_t result = getBlockChange_binary(
            block_changes[i].x,
            block_changes[i].y,
            block_changes[i].z
        );
        if (result != block_changes[i].block) {
            all_found = 0;
            break;
        }
    }

    test_result("Large dataset correctness (1000 entries)", all_found);
}

/* Test 10: Performance comparison */
void test_performance_comparison(void) {
    printf("\n--- Performance Comparison ---\n");

    int test_sizes[] = {100, 500, 1000, 5000, 10000};
    int num_sizes = sizeof(test_sizes) / sizeof(test_sizes[0]);

    for (int t = 0; t < num_sizes; t++) {
        int count = test_sizes[t];
        block_changes_count = 0;
        rng_seed = 54321;

        /* Insert entries (sorted for binary search) */
        for (int i = 0; i < count; i++) {
            short x = (short)(fast_rand() % 2000) - 1000;
            short z = (short)(fast_rand() % 2000) - 1000;
            uint8_t y = fast_rand() % 256;
            uint8_t block = (fast_rand() % 64) + 1;
            insertBlockChangeSorted(x, y, z, block);
        }

        /* Generate random lookup coordinates */
        int num_lookups = 10000;
        short *lookup_x = malloc(num_lookups * sizeof(short));
        short *lookup_z = malloc(num_lookups * sizeof(short));
        uint8_t *lookup_y = malloc(num_lookups * sizeof(uint8_t));

        rng_seed = 98765;
        for (int i = 0; i < num_lookups; i++) {
            lookup_x[i] = (short)(fast_rand() % 2000) - 1000;
            lookup_z[i] = (short)(fast_rand() % 2000) - 1000;
            lookup_y[i] = fast_rand() % 256;
        }

        /* Benchmark linear search */
        double start = get_time_ms();
        volatile uint8_t result_linear;
        for (int i = 0; i < num_lookups; i++) {
            result_linear = getBlockChange_linear(lookup_x[i], lookup_y[i], lookup_z[i]);
        }
        double linear_time = get_time_ms() - start;

        /* Benchmark binary search */
        start = get_time_ms();
        volatile uint8_t result_binary;
        for (int i = 0; i < num_lookups; i++) {
            result_binary = getBlockChange_binary(lookup_x[i], lookup_y[i], lookup_z[i]);
        }
        double binary_time = get_time_ms() - start;

        double speedup = linear_time / binary_time;
        printf("  %5d entries: linear=%6.2fms, binary=%6.2fms, speedup=%.1fx\n",
               count, linear_time, binary_time, speedup);

        free(lookup_x);
        free(lookup_z);
        free(lookup_y);

        (void)result_linear;
        (void)result_binary;
    }

    tests_total++;
    tests_passed++;
    printf("Test %d: Performance comparison... PASS\n", tests_total);
}

/* Test 11: Verify binary matches linear for all lookups */
void test_binary_matches_linear(void) {
    block_changes_count = 0;
    rng_seed = 11111;

    /* Insert 500 random entries */
    for (int i = 0; i < 500; i++) {
        short x = (short)(fast_rand() % 1000) - 500;
        short z = (short)(fast_rand() % 1000) - 500;
        uint8_t y = fast_rand() % 256;
        uint8_t block = (fast_rand() % 64) + 1;
        insertBlockChangeSorted(x, y, z, block);
    }

    /* Do 10000 random lookups and compare results */
    int mismatches = 0;
    rng_seed = 22222;
    for (int i = 0; i < 10000; i++) {
        short x = (short)(fast_rand() % 1000) - 500;
        short z = (short)(fast_rand() % 1000) - 500;
        uint8_t y = fast_rand() % 256;

        uint8_t linear = getBlockChange_linear(x, y, z);
        uint8_t binary = getBlockChange_binary(x, y, z);

        if (linear != binary) {
            mismatches++;
            if (mismatches <= 3) {
                printf("  Mismatch at (%d,%d,%d): linear=%d, binary=%d\n",
                       x, y, z, linear, binary);
            }
        }
    }

    test_result("Binary matches linear (10000 lookups)", mismatches == 0);
}

/* Test 12: Edge case - Y coordinate boundaries */
void test_y_boundaries(void) {
    block_changes_count = 0;

    insertBlockChangeSorted(0, 0, 0, B_bedrock);
    insertBlockChangeSorted(0, 255, 0, B_air);
    insertBlockChangeSorted(0, 128, 0, B_stone);

    int pass = 1;
    pass &= (getBlockChange_binary(0, 0, 0) == B_bedrock);
    pass &= (getBlockChange_binary(0, 255, 0) == B_air);
    pass &= (getBlockChange_binary(0, 128, 0) == B_stone);
    pass &= (getBlockChange_binary(0, 64, 0) == 0xFF);

    test_result("Y coordinate boundaries (0, 128, 255)", pass);
}

int main(void) {
    printf("=== Binary Search Block Changes Tests ===\n\n");

    test_empty_array();
    test_single_entry_found();
    test_single_entry_not_found();
    test_multiple_entries();
    test_sorted_insertion();
    test_update_existing();
    test_delete_entry();
    test_negative_coords();
    test_large_dataset_correctness();
    test_performance_comparison();
    test_binary_matches_linear();
    test_y_boundaries();

    printf("\n=== Results: %d/%d tests passed ===\n", tests_passed, tests_total);

    return (tests_passed == tests_total) ? 0 : 1;
}
