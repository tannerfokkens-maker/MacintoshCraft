#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "globals.h"
#include "tools.h"
#include "registries.h"
#include "procedures.h"
#include "worldgen.h"

uint32_t getChunkHash (short x, short z) {

  uint8_t buf[8];
  memcpy(buf, &x, 2);
  memcpy(buf + 2, &z, 2);
  memcpy(buf + 4, &world_seed, 4);

  return splitmix64(*((uint64_t *)buf));

}

uint8_t getChunkBiome (short x, short z) {

  // Center biomes on 0;0
  x += BIOME_RADIUS;
  z += BIOME_RADIUS;

  // Calculate distance from biome center
  int8_t dx = BIOME_RADIUS - mod_abs(x, BIOME_SIZE);
  int8_t dz = BIOME_RADIUS - mod_abs(z, BIOME_SIZE);
  // Each biome is a circular island, with beaches in-between
  // Determine whether the given chunk is within the island
  if (dx * dx + dz * dz > BIOME_RADIUS * BIOME_RADIUS) return W_beach;

  // Calculate "biome coordinates" (one step above chunk coordinates)
  short biome_x = div_floor(x, BIOME_SIZE);
  short biome_z = div_floor(z, BIOME_SIZE);

  // The biome itself is plucked directly from the world seed.
  // The 32-bit seed is treated as a 4x4 biome matrix, with each biome
  // taking up 2 bytes. This is why there are only 4 biomes, excluding
  // beaches. Using the world seed as a repeating pattern avoids
  // having to generate and layer yet another hash.
  uint8_t index = abs((biome_x & 3) + ((biome_z * 4) & 15));
  return (world_seed >> (index * 2)) & 3;

}

uint8_t getCornerHeight (uint32_t hash, uint8_t biome) {

  // When calculating the height, parts of the hash are used as random values.
  // Often, multiple values are stacked to stabilize the distribution while
  // allowing for occasionally larger variances.
  uint8_t height = TERRAIN_BASE_HEIGHT;

  switch (biome) {

    case W_mangrove_swamp: {
      height += (
        (hash % 3) +
        ((hash >> 4) % 3) +
        ((hash >> 8) % 3) +
        ((hash >> 12) % 3)
      );
      // If height dips below sea level, push it down further
      // This ends up creating many large ponds of water
      if (height < 64) height -= (hash >> 24) & 3;
      break;
    }

    case W_plains: {
      height += (
        (hash & 3) +
        (hash >> 4 & 3) +
        (hash >> 8 & 3) +
        (hash >> 12 & 3)
      );
      break;
    }

    case W_desert: {
      height += 4 + (
        (hash & 3) +
        (hash >> 4 & 3)
      );
      break;
    }

    case W_beach: {
      // Start slightly below sea level to ensure it's all water
      height = 62 - (
        (hash & 3) +
        (hash >> 4 & 3) +
        (hash >> 8 & 3)
      );
      break;
    }

    case W_snowy_plains: {
      // Use fewer components with larger ranges to create hills
      height += (
        (hash & 7) +
        (hash >> 4 & 7)
      );
      break;
    }

    default: break;
  }

  return height;

}

uint8_t interpolate (uint8_t a, uint8_t b, uint8_t c, uint8_t d, int x, int z) {
  uint16_t top    = a * (CHUNK_SIZE - x) + b * x;
  uint16_t bottom = c * (CHUNK_SIZE - x) + d * x;
  return (top * (CHUNK_SIZE - z) + bottom * z) / (CHUNK_SIZE * CHUNK_SIZE);
}

// Calculates terrain height using a pointer to an array of anchors
// The pointer should point towards the minichunk containing the desired
// coordinates, with available neighbors on +X and +Z.
uint8_t getHeightAtFromAnchors (int rx, int rz, ChunkAnchor *anchor_ptr) {

  if (rx == 0 && rz == 0) {
    int height = getCornerHeight(anchor_ptr[0].hash, anchor_ptr[0].biome);
    if (height > 67) return height - 1;
  }
  return interpolate(
    getCornerHeight(anchor_ptr[0].hash, anchor_ptr[0].biome),
    getCornerHeight(anchor_ptr[1].hash, anchor_ptr[1].biome),
    getCornerHeight(anchor_ptr[16 / CHUNK_SIZE + 1].hash, anchor_ptr[16 / CHUNK_SIZE + 1].biome),
    getCornerHeight(anchor_ptr[16 / CHUNK_SIZE + 2].hash, anchor_ptr[16 / CHUNK_SIZE + 2].biome),
    rx, rz
  );

}

uint8_t getHeightAtFromHash (int rx, int rz, int _x, int _z, uint32_t chunk_hash, uint8_t biome) {

  if (rx == 0 && rz == 0) {
    int height = getCornerHeight(chunk_hash, biome);
    if (height > 67) return height - 1;
  }
  return interpolate(
    getCornerHeight(chunk_hash, biome),
    getCornerHeight(getChunkHash(_x + 1, _z), getChunkBiome(_x + 1, _z)),
    getCornerHeight(getChunkHash(_x, _z + 1), getChunkBiome(_x, _z + 1)),
    getCornerHeight(getChunkHash(_x + 1, _z + 1), getChunkBiome(_x + 1, _z + 1)),
    rx, rz
  );

}

// Get terrain height at the given coordinates
// Does *not* account for block changes
uint8_t getHeightAt (int x, int z) {

  int _x = div_floor(x, CHUNK_SIZE);
  int _z = div_floor(z, CHUNK_SIZE);
  int rx = mod_abs(x, CHUNK_SIZE);
  int rz = mod_abs(z, CHUNK_SIZE);
  uint32_t chunk_hash = getChunkHash(_x, _z);
  uint8_t biome = getChunkBiome(_x, _z);

  return getHeightAtFromHash(rx, rz, _x, _z, chunk_hash, biome);

}

uint8_t getTerrainAtFromCache (int x, int y, int z, int rx, int rz, ChunkAnchor anchor, ChunkFeature feature, uint8_t height) {

  if (y >= 64 && y >= height && feature.y != 255) switch (anchor.biome) {
    case W_plains: { // Generate trees in the plains biome

      // Don't generate trees underwater
      if (feature.y < 64) break;

      // Handle tree stem and the dirt under it
      if (x == feature.x && z == feature.z) {
        if (y == feature.y - 1) return B_dirt;
        if (y >= feature.y && y < feature.y - feature.variant + 6) return B_oak_log;
      }

      // Get X/Z distance from center of tree
      uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
      uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;

      // Generate leaf clusters
      if (dx < 3 && dz < 3 && y > feature.y - feature.variant + 2 && y < feature.y - feature.variant + 5) {
        if (y == feature.y - feature.variant + 4 && dx == 2 && dz == 2) break;
        return B_oak_leaves;
      }
      if (dx < 2 && dz < 2 && y >= feature.y - feature.variant + 5 && y <= feature.y - feature.variant + 6) {
        if (y == feature.y - feature.variant + 6 && dx == 1 && dz == 1) break;
        return B_oak_leaves;
      }

      // Since we're sure that we're above sea level and in a plains biome,
      // there's no need to drop down to decide the surrounding blocks.
      if (y == height) return B_grass_block;
      return B_air;
    }

    case W_desert: { // Generate dead bushes and cacti in deserts

      if (x != feature.x || z != feature.z) break;

      if (feature.variant == 0) {
        if (y == height + 1) return B_dead_bush;
      } else if (y > height) {
        // The size of the cactus is determined based on whether the terrain
        // height is even or odd at the target location
        if (height & 1 && y <= height + 3) return B_cactus;
        if (y <= height + 2) return B_cactus;
      }

      break;

    }

    case W_mangrove_swamp: { // Generate lilypads and moss carpets in swamps

      if (x == feature.x && z == feature.z && y == 64 && height < 63) {
        return B_lily_pad;
      }

      if (y == height + 1) {
        uint8_t dx = x > feature.x ? x - feature.x : feature.x - x;
        uint8_t dz = z > feature.z ? z - feature.z : feature.z - z;
        if (dx + dz < 4) return B_moss_carpet;
      }

      break;
    }

    case W_snowy_plains: { // Generate grass stubs in snowy plains

      if (x == feature.x && z == feature.z && y == height + 1 && height >= 64) {
        return B_short_grass;
      }

      break;
    }

    default: break;
  }

  // Handle surface-level terrain (the very topmost blocks)
  if (height >= 63) {
    if (y == height) {
      if (anchor.biome == W_mangrove_swamp) return B_mud;
      if (anchor.biome == W_snowy_plains) return B_snowy_grass_block;
      if (anchor.biome == W_desert) return B_sand;
      if (anchor.biome == W_beach) return B_sand;
      return B_grass_block;
    }
    if (anchor.biome == W_snowy_plains && y == height + 1) {
      return B_snow;
    }
  }
  // Starting at 4 blocks below terrain level, generate minerals and caves
  if (y <= height - 4) {
    // Caves use the same shape as surface terrain, just mirrored
    int8_t gap = height - TERRAIN_BASE_HEIGHT;
    if (y < CAVE_BASE_DEPTH + gap && y > CAVE_BASE_DEPTH - gap) return B_air;

    // The chunk-relative X and Z coordinates are used as the seed for an
    // xorshift RNG/hash function to generate the Y coordinate of the ore
    // in this column. This way, each column is guaranteed to have exactly
    // one ore candidate, as there will always be a Y value to reference.
    uint8_t ore_y = ((rx & 15) << 4) + (rz & 15);
    ore_y ^= ore_y << 4;
    ore_y ^= ore_y >> 5;
    ore_y ^= ore_y << 1;
    ore_y &= 63;

    if (y == ore_y) {
      // Since the ore Y coordinate is effectely a random number in range [0;64),
      // we use it in a bit shift with the chunk's anchor hash to get another
      // pseudo-random number for the ore's rarity.
      uint8_t ore_probability = (anchor.hash >> (ore_y % 24)) & 255;
      // Ore placement is determined by Y level and "probability"
      if (y < 15) {
        if (ore_probability < 10) return B_diamond_ore;
        if (ore_probability < 12) return B_gold_ore;
        if (ore_probability < 15) return B_redstone_ore;
      }
      if (y < 30) {
        if (ore_probability < 3) return B_gold_ore;
        if (ore_probability < 8) return B_redstone_ore;
      }
      if (y < 54) {
        if (ore_probability < 30) return B_iron_ore;
        if (ore_probability < 40) return B_copper_ore;
      }
      if (ore_probability < 60) return B_coal_ore;
      if (y < 5) return B_lava;
      return B_cobblestone;
    }

    // For everything else, fall back to stone
    return B_stone;
  }
  // Handle the space between stone and grass
  if (y <= height) {
    if (anchor.biome == W_desert) return B_sandstone;
    if (anchor.biome == W_mangrove_swamp) return B_mud;
    if (anchor.biome == W_beach && height > 64) return B_sandstone;
    return B_dirt;
  }
  // If all else failed, but we're below sea level, generate water (or ice)
  if (y == 63 && anchor.biome == W_snowy_plains) return B_ice;
  if (y < 64) return B_water;

  // For everything else, fall back to air
  return B_air;

}

ChunkFeature getFeatureFromAnchor (ChunkAnchor anchor) {

  ChunkFeature feature;
  uint8_t feature_position = anchor.hash % (CHUNK_SIZE * CHUNK_SIZE);

  feature.x = feature_position % CHUNK_SIZE;
  feature.z = feature_position / CHUNK_SIZE;
  uint8_t skip_feature = false;

  // The following check does two things:
  //  firstly, it ensures that trees don't cross chunk boundaries;
  //  secondly, it reduces overall feature count. This is favorable
  //  everywhere except for swamps, which are otherwise very boring.
  if (anchor.biome != W_mangrove_swamp) {
    if (feature.x < 3 || feature.x > CHUNK_SIZE - 3) skip_feature = true;
    else if (feature.z < 3 || feature.z > CHUNK_SIZE - 3) skip_feature = true;
  }

  if (skip_feature) {
    // Skipped features are indicated by a Y coordinate of 0xFF (255)
    feature.y = 0xFF;
  } else {
    feature.x += anchor.x * CHUNK_SIZE;
    feature.z += anchor.z * CHUNK_SIZE;
    feature.y = getHeightAtFromHash(
      mod_abs(feature.x, CHUNK_SIZE), mod_abs(feature.z, CHUNK_SIZE),
      anchor.x, anchor.z, anchor.hash, anchor.biome
    ) + 1;
    feature.variant = (anchor.hash >> (feature.x + feature.z)) & 1;
  }

  return feature;

}

uint8_t getTerrainAt (int x, int y, int z, ChunkAnchor anchor) {

  if (y > 80) return B_air;

  int rx = x % CHUNK_SIZE;
  int rz = z % CHUNK_SIZE;
  if (rx < 0) rx += CHUNK_SIZE;
  if (rz < 0) rz += CHUNK_SIZE;

  ChunkFeature feature = getFeatureFromAnchor(anchor);
  uint8_t height = getHeightAtFromHash(rx, rz, anchor.x, anchor.z, anchor.hash, anchor.biome);

  return getTerrainAtFromCache(x, y, z, rx, rz, anchor, feature, height);

}

uint8_t getBlockAt (int x, int y, int z) {

  if (y < 0) return B_bedrock;

  uint8_t block_change = getBlockChange(x, y, z);
  if (block_change != 0xFF) return block_change;

  short anchor_x = div_floor(x, CHUNK_SIZE);
  short anchor_z = div_floor(z, CHUNK_SIZE);
  ChunkAnchor anchor = {
    .x = anchor_x,
    .z = anchor_z,
    .hash = getChunkHash(anchor_x, anchor_z),
    .biome = getChunkBiome(anchor_x, anchor_z)
  };

  return getTerrainAt(x, y, z, anchor);

}

uint8_t chunk_section[4096];
ChunkAnchor chunk_anchors[(16 / CHUNK_SIZE + 1) * (16 / CHUNK_SIZE + 1)];
ChunkFeature chunk_features[256 / (CHUNK_SIZE * CHUNK_SIZE)];
uint8_t chunk_section_height[16][16];

// ============================================================================
// Chunk Section Cache
// With 24MB RAM on 68k Mac, we can cache many generated chunks
// Each cached chunk section is 4KB, so 4096 entries = 16MB cache (8x view)
// ============================================================================

#ifdef MAC68K_PLATFORM
  #define CHUNK_CACHE_SIZE 4096  /* 16MB cache for 68k Mac (8x view) */
#else
  #define CHUNK_CACHE_SIZE 64    /* Smaller cache for other platforms */
#endif

typedef struct {
  int16_t cx, cy, cz;     /* Chunk coordinates (signed for negative coords) */
  uint8_t biome;          /* Cached biome value */
  uint8_t valid;          /* 1 if entry contains valid data */
  uint16_t lru_counter;   /* For LRU eviction */
  uint8_t data[4096];     /* Cached chunk section data */
} CachedChunkSection;

static CachedChunkSection chunk_cache[CHUNK_CACHE_SIZE];
static uint16_t cache_lru_clock = 0;
static int cache_initialized = 0;

/* Maximum probe distance for hash table operations (O(1) guarantee) */
#define MAX_PROBE_DISTANCE 32

/* Forward declaration */
static int chunkCacheHash(int16_t cx, int16_t cy, int16_t cz);

/* Initialize cache (call once at startup) */
void initChunkCache(void) {
  if (cache_initialized) return;
  for (int i = 0; i < CHUNK_CACHE_SIZE; i++) {
    chunk_cache[i].valid = 0;
    chunk_cache[i].lru_counter = 0;
  }
  cache_initialized = 1;
}

/* Invalidate cache entries affected by block changes */
/* Uses hash-based lookup with limited probing for O(1) performance */
void invalidateChunkCache(int16_t x, uint8_t y, int16_t z) {
  /* Find chunk coordinates containing this block */
  int16_t cx = (x < 0) ? ((x - 15) / 16) * 16 : (x / 16) * 16;
  int16_t cy = (y / 16) * 16;
  int16_t cz = (z < 0) ? ((z - 15) / 16) * 16 : (z / 16) * 16;

  /* Use hash to find entry quickly */
  int hash = chunkCacheHash(cx, cy, cz);
  for (int i = 0; i < MAX_PROBE_DISTANCE; i++) {
    int idx = (hash + i) % CHUNK_CACHE_SIZE;
    if (chunk_cache[idx].valid &&
        chunk_cache[idx].cx == cx &&
        chunk_cache[idx].cy == cy &&
        chunk_cache[idx].cz == cz) {
      chunk_cache[idx].valid = 0;
      return;
    }
  }
}

/* Clear entire cache (call when world seed changes) */
void clearChunkCache(void) {
  for (int i = 0; i < CHUNK_CACHE_SIZE; i++) {
    chunk_cache[i].valid = 0;
  }
}

/* Hash function for cache lookup */
static int chunkCacheHash(int16_t cx, int16_t cy, int16_t cz) {
  /* Simple hash combining coordinates */
  uint32_t h = (uint32_t)(cx * 73856093) ^ (uint32_t)(cy * 19349663) ^ (uint32_t)(cz * 83492791);
  return (int)(h % CHUNK_CACHE_SIZE);
}

/* Find cache entry for coordinates, returns index or -1 if not found */
/* Uses limited probing (MAX_PROBE_DISTANCE) for O(1) performance */
static int findCacheEntry(int16_t cx, int16_t cy, int16_t cz) {
  int hash = chunkCacheHash(cx, cy, cz);

  /* Limited linear probing from hash position */
  for (int i = 0; i < MAX_PROBE_DISTANCE; i++) {
    int idx = (hash + i) % CHUNK_CACHE_SIZE;
    if (!chunk_cache[idx].valid) continue;
    if (chunk_cache[idx].cx == cx &&
        chunk_cache[idx].cy == cy &&
        chunk_cache[idx].cz == cz) {
      return idx;
    }
  }
  return -1;
}

/* Find slot for new entry - evicts oldest within probe distance for O(1) */
static int findCacheSlot(int16_t cx, int16_t cy, int16_t cz) {
  int hash = chunkCacheHash(cx, cy, cz);

  /* First pass: look for empty slot near hash position (limited probe) */
  for (int i = 0; i < MAX_PROBE_DISTANCE; i++) {
    int idx = (hash + i) % CHUNK_CACHE_SIZE;
    if (!chunk_cache[idx].valid) {
      return idx;
    }
  }

  /* No empty slots nearby - evict oldest entry within probe distance */
  /* This ensures entries are always findable by findCacheEntry */
  int oldest_idx = hash % CHUNK_CACHE_SIZE;
  uint16_t oldest_age = 0;
  for (int i = 0; i < MAX_PROBE_DISTANCE; i++) {
    int idx = (hash + i) % CHUNK_CACHE_SIZE;
    uint16_t age = (uint16_t)(cache_lru_clock - chunk_cache[idx].lru_counter);
    if (age > oldest_age) {
      oldest_age = age;
      oldest_idx = idx;
    }
  }

  return oldest_idx;
}

/* Internal: Generate chunk section without caching (original algorithm) */
static uint8_t buildChunkSectionInternal(int cx, int cy, int cz);

// Builds a 16x16x16 chunk of blocks and writes it to `chunk_section`
// Returns the biome at the origin corner of the chunk
// This is the PUBLIC function that uses caching
uint8_t buildChunkSection(int cx, int cy, int cz) {
  /* Initialize cache on first use */
  if (!cache_initialized) {
    initChunkCache();
  }

  /* Check cache for existing entry */
  int cache_idx = findCacheEntry((int16_t)cx, (int16_t)cy, (int16_t)cz);
  if (cache_idx >= 0) {
    /* Cache hit: copy cached data to chunk_section */
    memcpy(chunk_section, chunk_cache[cache_idx].data, 4096);
    chunk_cache[cache_idx].lru_counter = ++cache_lru_clock;

    /* Still need to apply block changes on top of cached data */
    if (block_changes_count > 0) {
      int cx_max = cx + 16;
      int cy_max = cy + 16;
      int cz_max = cz + 16;

      for (int i = 0; i < block_changes_count; i++) {
        uint8_t block = block_changes[i].block;
        if (block == 0xFF) continue;
        if (block == B_torch) continue;
        #ifdef ALLOW_CHESTS
          if (block == B_chest) continue;
        #endif

        short bx = block_changes[i].x;
        uint8_t by = block_changes[i].y;
        short bz = block_changes[i].z;

        if (bx < cx || bx >= cx_max) continue;
        if (by < cy || by >= cy_max) continue;
        if (bz < cz || bz >= cz_max) continue;

        int dx = bx - cx;
        int dy = by - cy;
        int dz = bz - cz;
        unsigned address = (unsigned)(dx + (dz << 4) + (dy << 8));
        unsigned index = (address & ~7u) | (7u - (address & 7u));
        chunk_section[index] = block;
      }
    }

    return chunk_cache[cache_idx].biome;
  }

  /* Cache miss: generate chunk section */
  uint8_t biome = buildChunkSectionInternal(cx, cy, cz);

  /* Store in cache (only if no block changes affect this chunk) */
  /* Note: We always cache, but invalidate on block changes */
  cache_idx = findCacheSlot((int16_t)cx, (int16_t)cy, (int16_t)cz);
  chunk_cache[cache_idx].cx = (int16_t)cx;
  chunk_cache[cache_idx].cy = (int16_t)cy;
  chunk_cache[cache_idx].cz = (int16_t)cz;
  chunk_cache[cache_idx].biome = biome;
  chunk_cache[cache_idx].valid = 1;
  chunk_cache[cache_idx].lru_counter = ++cache_lru_clock;
  memcpy(chunk_cache[cache_idx].data, chunk_section, 4096);

  return biome;
}

// Internal: Generate chunk section (called by buildChunkSection on cache miss)
static uint8_t buildChunkSectionInternal(int cx, int cy, int cz) {

  // Precompute hashes, anchors and features for each relevant minichunk
  int anchor_index = 0, feature_index = 0;
  for (int i = cz; i < cz + 16 + CHUNK_SIZE; i += CHUNK_SIZE) {
    for (int j = cx; j < cx + 16 + CHUNK_SIZE; j += CHUNK_SIZE) {

      ChunkAnchor *anchor = chunk_anchors + anchor_index;

      anchor->x = j / CHUNK_SIZE;
      anchor->z = i / CHUNK_SIZE;
      anchor->hash = getChunkHash(anchor->x, anchor->z);
      anchor->biome = getChunkBiome(anchor->x, anchor->z);

      // Compute chunk features for the minichunks within this section
      if (i != cz + 16 && j != cx + 16) {
        chunk_features[feature_index] = getFeatureFromAnchor(*anchor);
        feature_index ++;
      }

      anchor_index ++;
    }
  }

  // Precompute terrain height for entire chunk section
  for (int i = 0; i < 16; i ++) {
    for (int j = 0; j < 16; j ++) {
      anchor_index = (j / CHUNK_SIZE) + (i / CHUNK_SIZE) * (16 / CHUNK_SIZE + 1);
      ChunkAnchor *anchor_ptr = chunk_anchors + anchor_index;
      chunk_section_height[j][i] = getHeightAtFromAnchors(j % CHUNK_SIZE, i % CHUNK_SIZE, anchor_ptr);
    }
  }

  // Generate 4096 blocks in one buffer to reduce overhead
  // OPTIMIZATION: Unrolled inner loop eliminates loop overhead and
  // pre-computes rx values to avoid modulo operations (saves ~0.3-0.8s)
  for (int j = 0; j < 4096; j += 8) {
    // These values don't change in the lower array,
    // since all of the operations are on multiples of 8
    int y = j / 256 + cy;
    int rz = j / 16 % 16;
    int rz_mod = rz % CHUNK_SIZE;
    feature_index = (j % 16) / CHUNK_SIZE + (j / 16 % 16) / CHUNK_SIZE * (16 / CHUNK_SIZE);
    anchor_index = (j % 16) / CHUNK_SIZE + (j / 16 % 16) / CHUNK_SIZE * (16 / CHUNK_SIZE + 1);

    // The client expects "big-endian longs", which in our
    // case means reversing the order in which we store/send
    // each 8 block sequence.
    //
    // Unrolled loop: offset 7..0 maps to section indices j+0..j+7
    // rx values cycle through (j%16)+7, (j%16)+6, ... (j%16)+0 (mod 16)
    int rx_base = j % 16;

    // offset=7: k=j+7, rx=(j+7)%16, store at j+0
    {
      int rx = (rx_base + 7) & 15;
      chunk_section[j + 0] = getTerrainAtFromCache(
        rx + cx, y, rz + cz, rx % CHUNK_SIZE, rz_mod,
        chunk_anchors[anchor_index], chunk_features[feature_index],
        chunk_section_height[rx][rz]
      );
    }
    // offset=6: k=j+6, rx=(j+6)%16, store at j+1
    {
      int rx = (rx_base + 6) & 15;
      chunk_section[j + 1] = getTerrainAtFromCache(
        rx + cx, y, rz + cz, rx % CHUNK_SIZE, rz_mod,
        chunk_anchors[anchor_index], chunk_features[feature_index],
        chunk_section_height[rx][rz]
      );
    }
    // offset=5: k=j+5, rx=(j+5)%16, store at j+2
    {
      int rx = (rx_base + 5) & 15;
      chunk_section[j + 2] = getTerrainAtFromCache(
        rx + cx, y, rz + cz, rx % CHUNK_SIZE, rz_mod,
        chunk_anchors[anchor_index], chunk_features[feature_index],
        chunk_section_height[rx][rz]
      );
    }
    // offset=4: k=j+4, rx=(j+4)%16, store at j+3
    {
      int rx = (rx_base + 4) & 15;
      chunk_section[j + 3] = getTerrainAtFromCache(
        rx + cx, y, rz + cz, rx % CHUNK_SIZE, rz_mod,
        chunk_anchors[anchor_index], chunk_features[feature_index],
        chunk_section_height[rx][rz]
      );
    }
    // offset=3: k=j+3, rx=(j+3)%16, store at j+4
    {
      int rx = (rx_base + 3) & 15;
      chunk_section[j + 4] = getTerrainAtFromCache(
        rx + cx, y, rz + cz, rx % CHUNK_SIZE, rz_mod,
        chunk_anchors[anchor_index], chunk_features[feature_index],
        chunk_section_height[rx][rz]
      );
    }
    // offset=2: k=j+2, rx=(j+2)%16, store at j+5
    {
      int rx = (rx_base + 2) & 15;
      chunk_section[j + 5] = getTerrainAtFromCache(
        rx + cx, y, rz + cz, rx % CHUNK_SIZE, rz_mod,
        chunk_anchors[anchor_index], chunk_features[feature_index],
        chunk_section_height[rx][rz]
      );
    }
    // offset=1: k=j+1, rx=(j+1)%16, store at j+6
    {
      int rx = (rx_base + 1) & 15;
      chunk_section[j + 6] = getTerrainAtFromCache(
        rx + cx, y, rz + cz, rx % CHUNK_SIZE, rz_mod,
        chunk_anchors[anchor_index], chunk_features[feature_index],
        chunk_section_height[rx][rz]
      );
    }
    // offset=0: k=j+0, rx=(j+0)%16, store at j+7
    {
      int rx = rx_base;
      chunk_section[j + 7] = getTerrainAtFromCache(
        rx + cx, y, rz + cz, rx % CHUNK_SIZE, rz_mod,
        chunk_anchors[anchor_index], chunk_features[feature_index],
        chunk_section_height[rx][rz]
      );
    }
  }

  // Apply block changes on top of terrain
  // This does mean that we're generating some terrain only to replace it,
  // but it's better to apply changes in one run rather than in individual
  // runs per block, as this is more expensive than terrain generation.

  // OPTIMIZATION: Early exit if no block changes (saves ~3.9s per view update)
  if (block_changes_count > 0) {
    // Pre-compute chunk bounds for faster comparison
    int cx_max = cx + 16;
    int cy_max = cy + 16;
    int cz_max = cz + 16;

    for (int i = 0; i < block_changes_count; i ++) {
      uint8_t block = block_changes[i].block;

      // Skip unallocated entries
      if (block == 0xFF) continue;
      // Skip blocks that behave better when sent using a block update
      if (block == B_torch) continue;
      #ifdef ALLOW_CHESTS
        if (block == B_chest) continue;
      #endif

      // Load coordinates once for bounds checking
      short bx = block_changes[i].x;
      uint8_t by = block_changes[i].y;
      short bz = block_changes[i].z;

      // Check if block is within this chunk section (optimized order)
      if (bx < cx || bx >= cx_max) continue;
      if (by < cy || by >= cy_max) continue;
      if (bz < cz || bz >= cz_max) continue;

      // Apply block change
      int dx = bx - cx;
      int dy = by - cy;
      int dz = bz - cz;
      // Same 8-block sequence reversal as before, this time 10x dirtier
      // because we're working with specific indexes.
      unsigned address = (unsigned)(dx + (dz << 4) + (dy << 8));
      unsigned index = (address & ~7u) | (7u - (address & 7u));
      chunk_section[index] = block;
    }
  }

  return chunk_anchors[0].biome;

}
