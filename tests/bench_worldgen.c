/*
 * bench_worldgen.c - Performance benchmark for chunk generation and block operations
 *
 * Measures operations and estimates 68040 @ 40MHz performance.
 * Run on host machine, extrapolates to 68k Mac.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

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

/* Simulates getBlockChange from procedures.c */
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

/* Simulates makeBlockChange from procedures.c (simplified) */
int makeBlockChange(short x, uint8_t y, short z, uint8_t block) {
    int first_gap = block_changes_count;

    /* Search for existing entry or gap */
    for (int i = 0; i < block_changes_count; i++) {
        if (block_changes[i].block == 0xFF) {
            if (first_gap == block_changes_count) first_gap = i;
            continue;
        }
        if (block_changes[i].x == x &&
            block_changes[i].y == y &&
            block_changes[i].z == z) {
            block_changes[i].block = block;
            return 0;
        }
    }

    /* Add new entry */
    if (first_gap >= MAX_BLOCK_CHANGES) return 1;
    block_changes[first_gap].x = x;
    block_changes[first_gap].y = y;
    block_changes[first_gap].z = z;
    block_changes[first_gap].block = block;
    if (first_gap >= block_changes_count) {
        block_changes_count = first_gap + 1;
    }
    return 0;
}

extern uint8_t chunk_section[4096];

/* 68040 cycle estimates for common operations */
#define CYCLES_MEMCPY_4K     2000   /* memcpy 4KB */
#define CYCLES_FUNC_CALL     20     /* Function call overhead */
#define CYCLES_LOOP_ITER     10     /* Simple loop iteration */
#define CYCLES_HASH_COMPUTE  50     /* Hash computation */
#define CYCLES_TERRAIN_BLOCK 150    /* getTerrainAtFromCache per block */
#define CYCLES_COMPARE       5      /* Single comparison */
#define CYCLES_STRUCT_ACCESS 8      /* Struct field access */

/* Network I/O cycle estimates (MacTCP overhead) */
#define CYCLES_RECV_CALL     5000   /* Single recv() system call */
#define CYCLES_SEND_CALL     5000   /* Single send() system call */
#define CYCLES_TCP_OVERHEAD  2000   /* TCP/IP stack processing per call */

/* Clock helper */
static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

/* Estimate 68040 time from cycle count */
static double cycles_to_ms_68k(uint64_t cycles) {
    return (double)cycles / 40000.0;  /* 40 MHz = 40000 cycles/ms */
}

/*
 * Benchmark: Block mining scenario
 * "Move 10 blocks, mine a block, how long until it's in inventory?"
 */
void bench_block_mining_scenario(void) {
    printf("\n");
    printf("================================================================\n");
    printf("  SCENARIO: Move 10 blocks, mine block, item to inventory\n");
    printf("================================================================\n\n");

    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);

    /* Simulate having some block changes (player has been playing) */
    int existing_changes = 500;  /* Reasonable gameplay amount */
    block_changes_count = existing_changes;
    for (int i = 0; i < existing_changes; i++) {
        block_changes[i].x = (short)(i * 3);
        block_changes[i].y = 64;
        block_changes[i].z = (short)(i * 2);
        block_changes[i].block = B_air;
    }

    printf("Setup: %d existing block changes (typical gameplay)\n\n", existing_changes);

    /* ========== STEP 1: Movement (10 blocks, staying in same chunk) ========== */
    printf("STEP 1: Move 10 blocks (within same chunk)\n");

    /* Movement within chunk = no chunk generation needed */
    /* Just position update packets */
    uint64_t move_cycles =
        CYCLES_RECV_CALL * 3 +      /* Receive position packet (3 recv calls with buffering) */
        CYCLES_TCP_OVERHEAD +
        CYCLES_SEND_CALL * 1 +      /* Send position ack */
        CYCLES_TCP_OVERHEAD;

    printf("  Network: recv position + send ack\n");
    printf("  Est. cycles: %llu (%.2f ms)\n\n",
           (unsigned long long)move_cycles, cycles_to_ms_68k(move_cycles));

    /* ========== STEP 2: Receive dig packet ========== */
    printf("STEP 2: Receive dig packet\n");

    /* Dig packet: action(1) + position(8) + face(1) + sequence(varint ~2) = ~12 bytes */
    /* With input buffering: 1-2 recv calls instead of 11-15 */
    uint64_t recv_dig_cycles =
        CYCLES_RECV_CALL * 2 +      /* 2 recv calls with buffering */
        CYCLES_TCP_OVERHEAD +
        CYCLES_LOOP_ITER * 12;      /* Parse 12 bytes */

    printf("  Packet parsing with input buffer\n");
    printf("  Est. cycles: %llu (%.2f ms)\n\n",
           (unsigned long long)recv_dig_cycles, cycles_to_ms_68k(recv_dig_cycles));

    /* ========== STEP 3: getBlockAt (lookup what block was mined) ========== */
    printf("STEP 3: getBlockAt() - lookup mined block\n");

    double start = get_time_ms();
    for (int i = 0; i < 1000; i++) {
        getBlockAt(100, 64, 100);  /* Coords not in block_changes */
    }
    double elapsed = get_time_ms() - start;

    /* Cycles: scan block_changes + terrain generation for one block */
    uint64_t getblock_cycles =
        (CYCLES_LOOP_ITER + CYCLES_STRUCT_ACCESS * 4 + CYCLES_COMPARE * 4) * existing_changes +
        CYCLES_TERRAIN_BLOCK;

    printf("  Scans %d block_changes entries + terrain lookup\n", existing_changes);
    printf("  Host time (1000 calls): %.2f ms\n", elapsed);
    printf("  Est. cycles: %llu (%.2f ms)\n\n",
           (unsigned long long)getblock_cycles, cycles_to_ms_68k(getblock_cycles));

    /* ========== STEP 4: makeBlockChange (add air block) ========== */
    printf("STEP 4: makeBlockChange() - record block as air\n");

    start = get_time_ms();
    for (int i = 0; i < 1000; i++) {
        /* Reset to test fresh insertion */
        block_changes_count = existing_changes;
        makeBlockChange(200 + i, 64, 200, B_air);
    }
    elapsed = get_time_ms() - start;

    /* Cycles: scan for existing entry, then add new */
    uint64_t makechange_cycles =
        (CYCLES_LOOP_ITER + CYCLES_STRUCT_ACCESS * 4 + CYCLES_COMPARE * 4) * existing_changes +
        CYCLES_STRUCT_ACCESS * 4;  /* Write new entry */

    printf("  Scans %d entries to find slot\n", existing_changes);
    printf("  Host time (1000 calls): %.2f ms\n", elapsed);
    printf("  Est. cycles: %llu (%.2f ms)\n\n",
           (unsigned long long)makechange_cycles, cycles_to_ms_68k(makechange_cycles));

    /* ========== STEP 5: givePlayerItem (add to inventory) ========== */
    printf("STEP 5: givePlayerItem() - add to inventory\n");

    /* Scan 41 inventory slots (worst case) */
    uint64_t inventory_cycles =
        CYCLES_LOOP_ITER * 41 +
        CYCLES_COMPARE * 41 * 2;

    printf("  Scans 41 inventory slots\n");
    printf("  Est. cycles: %llu (%.3f ms)\n\n",
           (unsigned long long)inventory_cycles, cycles_to_ms_68k(inventory_cycles));

    /* ========== STEP 6: Send response packets ========== */
    printf("STEP 6: Send response packets\n");

    /* Packets sent: ack block change + set container slot (inventory update) */
    uint64_t send_cycles =
        CYCLES_SEND_CALL * 4 +      /* Multiple small sends per packet */
        CYCLES_TCP_OVERHEAD * 2;    /* 2 packets */

    printf("  Send: block ack + inventory update\n");
    printf("  Est. cycles: %llu (%.2f ms)\n\n",
           (unsigned long long)send_cycles, cycles_to_ms_68k(send_cycles));

    /* ========== TOTAL ========== */
    uint64_t total_cycles = move_cycles + recv_dig_cycles + getblock_cycles +
                           makechange_cycles + inventory_cycles + send_cycles;

    printf("────────────────────────────────────────────────────────────────\n");
    printf("TOTAL ESTIMATED LATENCY: %.1f ms\n", cycles_to_ms_68k(total_cycles));
    printf("────────────────────────────────────────────────────────────────\n\n");

    printf("Breakdown:\n");
    printf("  Movement:           %5.1f ms (%4.1f%%)\n",
           cycles_to_ms_68k(move_cycles), 100.0 * move_cycles / total_cycles);
    printf("  Receive dig:        %5.1f ms (%4.1f%%)\n",
           cycles_to_ms_68k(recv_dig_cycles), 100.0 * recv_dig_cycles / total_cycles);
    printf("  getBlockAt:         %5.1f ms (%4.1f%%)\n",
           cycles_to_ms_68k(getblock_cycles), 100.0 * getblock_cycles / total_cycles);
    printf("  makeBlockChange:    %5.1f ms (%4.1f%%)\n",
           cycles_to_ms_68k(makechange_cycles), 100.0 * makechange_cycles / total_cycles);
    printf("  givePlayerItem:     %5.1f ms (%4.1f%%)\n",
           cycles_to_ms_68k(inventory_cycles), 100.0 * inventory_cycles / total_cycles);
    printf("  Send response:      %5.1f ms (%4.1f%%)\n",
           cycles_to_ms_68k(send_cycles), 100.0 * send_cycles / total_cycles);
}

/*
 * Benchmark: Block mining with varying block_changes_count
 */
void bench_block_changes_scaling(void) {
    printf("\n");
    printf("================================================================\n");
    printf("  SCALING: Block mining latency vs block_changes_count\n");
    printf("================================================================\n\n");

    int test_counts[] = {0, 100, 500, 1000, 2000, 5000, 10000, 20000};
    int num_tests = sizeof(test_counts) / sizeof(test_counts[0]);

    printf("block_changes | getBlockAt | makeBlockChange | Total (excl network)\n");
    printf("--------------|------------|-----------------|---------------------\n");

    for (int t = 0; t < num_tests; t++) {
        int count = test_counts[t];

        uint64_t get_cycles =
            (CYCLES_LOOP_ITER + CYCLES_STRUCT_ACCESS * 4 + CYCLES_COMPARE * 4) * count +
            CYCLES_TERRAIN_BLOCK;

        uint64_t make_cycles =
            (CYCLES_LOOP_ITER + CYCLES_STRUCT_ACCESS * 4 + CYCLES_COMPARE * 4) * count +
            CYCLES_STRUCT_ACCESS * 4;

        uint64_t total = get_cycles + make_cycles;

        printf("    %5d     |  %5.1f ms  |     %5.1f ms    |     %5.1f ms\n",
               count,
               cycles_to_ms_68k(get_cycles),
               cycles_to_ms_68k(make_cycles),
               cycles_to_ms_68k(total));
    }

    printf("\nNote: Network I/O adds ~0.5-1.0ms additional latency\n");
}

/*
 * Benchmark: Chunk crossing scenario
 */
void bench_chunk_crossing(void) {
    printf("\n");
    printf("================================================================\n");
    printf("  SCENARIO: Cross chunk boundary (triggers chunk generation)\n");
    printf("================================================================\n\n");

    world_seed = splitmix64(0xA103DE6C);
    rng_seed = splitmix64(0xE2B9419);
    block_changes_count = 0;

    /* Pre-warm cache with current view */
    clearChunkCache();
    for (int cx = -2; cx <= 2; cx++) {
        for (int cz = -2; cz <= 2; cz++) {
            for (int cy = 0; cy < 320; cy += 16) {
                buildChunkSection(cx * 16, cy, cz * 16);
            }
        }
    }

    printf("Cache warmed with 5x5 chunk view (500 sections)\n\n");

    /* Cross into new chunk column - need to generate 20 new sections */
    /* and send 5 columns × 20 sections = 100 chunk packets */

    int new_sections = 5 * 20;  /* 5 columns at edge, 20 Y levels each */
    int cached_sections = 20 * 20;  /* 20 columns from cache */

    /* Time to generate new chunks */
    double start = get_time_ms();
    for (int i = 0; i < new_sections; i++) {
        clearChunkCache();  /* Force regeneration */
        buildChunkSection(1000 + (i % 5) * 16, (i / 5) * 16, 0);
    }
    (void)(get_time_ms() - start);  /* Host timing for reference */

    /* Estimate 68k time */
    uint64_t gen_cycles = (uint64_t)new_sections *
        (CYCLES_TERRAIN_BLOCK * 4096 + CYCLES_MEMCPY_4K);
    uint64_t cache_cycles = (uint64_t)cached_sections *
        (CYCLES_HASH_COMPUTE + CYCLES_LOOP_ITER * 2 + CYCLES_MEMCPY_4K);

    /* Network: send chunk data packets */
    /* Each chunk packet is ~82KB, assume multiple send calls */
    uint64_t network_cycles = (uint64_t)(new_sections + cached_sections) *
        (CYCLES_SEND_CALL * 20 + CYCLES_TCP_OVERHEAD);

    uint64_t total = gen_cycles + cache_cycles + network_cycles;

    printf("New chunk sections to generate: %d\n", new_sections);
    printf("Cached sections to send:        %d\n", cached_sections);
    printf("\n");
    printf("Estimated 68k times:\n");
    printf("  Chunk generation:  %6.1f ms\n", cycles_to_ms_68k(gen_cycles));
    printf("  Cache retrieval:   %6.1f ms\n", cycles_to_ms_68k(cache_cycles));
    printf("  Network send:      %6.1f ms\n", cycles_to_ms_68k(network_cycles));
    printf("  ─────────────────────────────\n");
    printf("  TOTAL:             %6.1f ms (%.1f sec)\n",
           cycles_to_ms_68k(total), cycles_to_ms_68k(total) / 1000);
}

/*
 * Summary comparison
 */
void print_scenario_summary(void) {
    printf("\n");
    printf("================================================================\n");
    printf("                    SCENARIO SUMMARY\n");
    printf("================================================================\n\n");

    printf("Scenario                              | Est. 68k Time\n");
    printf("--------------------------------------|---------------\n");
    printf("Mine block (500 existing changes)     |    ~2-3 ms\n");
    printf("Mine block (5000 existing changes)    |   ~15-20 ms\n");
    printf("Mine block (20000 existing changes)   |   ~60-70 ms\n");
    printf("Move within chunk                     |    ~0.5 ms\n");
    printf("Cross chunk boundary                  |    ~3-5 sec\n");
    printf("Initial world load                    |    ~8-10 sec\n");
    printf("\n");
    printf("Key insight: The 2.5s delay you observed is likely from:\n");
    printf("  1. Chunk boundary crossing (not in-chunk movement)\n");
    printf("  2. High block_changes_count (O(n) loops)\n");
    printf("  3. Network I/O overhead (multiple send/recv calls)\n");
}

/*
 * Before/After comparison for chunk crossing
 */
void bench_before_after_comparison(void) {
    printf("\n");
    printf("================================================================\n");
    printf("  BEFORE/AFTER: Chunk Boundary Crossing Optimization Impact\n");
    printf("================================================================\n\n");

    /*
     * BEFORE optimizations (no cache, O(n) block_changes for each chunk):
     * - Every chunk crossing regenerated ALL visible chunks
     * - Each chunk scanned entire block_changes array
     * - No loop unrolling
     */

    int total_chunks = 500;  /* Full view */
    int new_chunks = 100;    /* Edge chunks that need generation */
    int cached_chunks = 400; /* Chunks that would be cache hits */

    /* BEFORE: No cache, all chunks regenerated */
    uint64_t before_gen_cycles = (uint64_t)total_chunks *
        (CYCLES_TERRAIN_BLOCK * 4096);

    /* BEFORE: block_changes loop scanned all entries for EACH chunk */
    /* Assume moderate gameplay with 2000 block changes */
    int block_changes_before = 2000;
    uint64_t before_blockchange_cycles = (uint64_t)total_chunks *
        (CYCLES_LOOP_ITER * block_changes_before);

    /* BEFORE: Network same as after */
    uint64_t network_cycles = (uint64_t)total_chunks *
        (CYCLES_SEND_CALL * 20 + CYCLES_TCP_OVERHEAD);

    uint64_t before_total = before_gen_cycles + before_blockchange_cycles + network_cycles;

    /* AFTER: With cache, only new chunks regenerated */
    uint64_t after_gen_cycles = (uint64_t)new_chunks *
        (CYCLES_TERRAIN_BLOCK * 4096 + CYCLES_MEMCPY_4K);

    /* AFTER: Cache hits are just hash + memcpy */
    uint64_t after_cache_cycles = (uint64_t)cached_chunks *
        (CYCLES_HASH_COMPUTE + CYCLES_LOOP_ITER * 2 + CYCLES_MEMCPY_4K);

    /* AFTER: block_changes early exit (count=0 for cached, applied on hit) */
    /* Much less overhead since we skip the loop when count is 0 */
    uint64_t after_blockchange_cycles = (uint64_t)new_chunks *
        (CYCLES_LOOP_ITER * block_changes_before);  /* Only new chunks scan */

    uint64_t after_total = after_gen_cycles + after_cache_cycles +
                          after_blockchange_cycles + network_cycles;

    printf("Assumptions:\n");
    printf("  - VIEW_DISTANCE=2 (5x5 = 25 columns, 500 sections)\n");
    printf("  - Crossing 1 chunk boundary (100 new sections, 400 cached)\n");
    printf("  - %d existing block changes\n\n", block_changes_before);

    printf("┌─────────────────────────┬─────────────┬─────────────┐\n");
    printf("│ Component               │   BEFORE    │    AFTER    │\n");
    printf("├─────────────────────────┼─────────────┼─────────────┤\n");
    printf("│ Chunk generation        │ %7.1f sec │ %7.2f sec │\n",
           cycles_to_ms_68k(before_gen_cycles) / 1000,
           cycles_to_ms_68k(after_gen_cycles) / 1000);
    printf("│ Cache retrieval         │     N/A     │ %7.2f sec │\n",
           cycles_to_ms_68k(after_cache_cycles) / 1000);
    printf("│ block_changes scanning  │ %7.2f sec │ %7.2f sec │\n",
           cycles_to_ms_68k(before_blockchange_cycles) / 1000,
           cycles_to_ms_68k(after_blockchange_cycles) / 1000);
    printf("│ Network I/O             │ %7.2f sec │ %7.2f sec │\n",
           cycles_to_ms_68k(network_cycles) / 1000,
           cycles_to_ms_68k(network_cycles) / 1000);
    printf("├─────────────────────────┼─────────────┼─────────────┤\n");
    printf("│ TOTAL                   │ %7.1f sec │ %7.2f sec │\n",
           cycles_to_ms_68k(before_total) / 1000,
           cycles_to_ms_68k(after_total) / 1000);
    printf("└─────────────────────────┴─────────────┴─────────────┘\n\n");

    double speedup = (double)before_total / after_total;
    double time_saved = cycles_to_ms_68k(before_total - after_total) / 1000;

    printf("SPEEDUP: %.1fx faster\n", speedup);
    printf("TIME SAVED: %.1f seconds per chunk crossing\n", time_saved);

    printf("\n");
    printf("Breakdown of savings:\n");
    printf("  - Caching 400 chunks:      %.1f sec saved\n",
           cycles_to_ms_68k(before_gen_cycles - after_gen_cycles - after_cache_cycles) / 1000);
    printf("  - block_changes early exit: %.2f sec saved\n",
           cycles_to_ms_68k(before_blockchange_cycles - after_blockchange_cycles) / 1000);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    printf("Bareiron 68k Performance Benchmark\n");
    printf("==================================\n");
    printf("Target: Motorola 68040 @ 40MHz\n");

    bench_block_mining_scenario();
    bench_block_changes_scaling();
    bench_chunk_crossing();
    bench_before_after_comparison();
    print_scenario_summary();

    return 0;
}

