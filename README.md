# MacintoshCraft

A fork of [bareiron](https://github.com/p2r3/bareiron) - a minimalist Minecraft server ported to run on 68k Macintosh computers.

This project brings Minecraft 1.21.8 server functionality to classic Macintosh hardware using Open Transport networking. It prioritizes **memory usage** and **performance** to run on systems with as little as 8MB of RAM.

- Minecraft version: `1.21.8`
- Protocol version: `772`
- Target platform: 68k Macintosh (System 7+ with MacTCP or Open Transport)

> [!WARNING]
> Only the vanilla Minecraft client is officially supported. Issues have been reported when using Fabric or similar.

## Features

### 68k Mac Specific
- **Dual networking stack** - Supports both MacTCP (System 6+) and Open Transport (System 7.5+)
- **Runtime configuration** - Adjust view distance, chunk cache size, and mob interpolation via menu
- **Chunk caching** - LRU cache reduces repeated terrain generation (configurable size based on available RAM)
- **Mob interpolation** - Smooth mob movement even during chunk loading on slow systems
- **Optimized worldgen** - Two-octave terrain height variation and improved cave generation

### Core Features (from bareiron)
- Procedural terrain generation with biomes (plains, desert, swamp, snowy plains)
- Multiplayer support (up to 16 players configurable)
- Survival gameplay: mining, crafting, building, mobs
- World persistence to disk
- Chest and inventory system
- Day/night cycle

## Building for 68k Macintosh

### Prerequisites
- [Retro68](https://github.com/autc04/Retro68) cross-compiler toolchain
- CMake 3.x+
- Java 21+ (for registry extraction)

### Build Steps

1. **Extract Minecraft registries** (requires vanilla server JAR):
   ```bash
   ./extract_registries.sh
   # Or manually: bun run build_registries.js
   ```

2. **Build for 68k Mac**:
   ```bash
   mkdir -p build-mac68k && cd build-mac68k
   cmake .. -DCMAKE_TOOLCHAIN_FILE=/path/to/Retro68-build/toolchain/m68k-apple-macos/cmake/retro68.toolchain.cmake
   make
   ```

3. **Output files**:
   - `bareiron.dsk` - Disk image for emulators
   - `bareiron.bin` - MacBinary for transfer to real hardware
   - `bareiron.APPL` - Raw application

### Running
- Requires System 7 or later with MacTCP or Open Transport installed
- Minimum 8MB RAM recommended (more RAM = larger chunk cache)
- Connect to your Mac's IP address on port 25565 from Minecraft 1.21.8

## Building for Other Platforms

### Linux/macOS/Windows
```bash
./build.sh
```

### Windows 9x
```bash
./build.sh --9x
```

### ESP32
Use PlatformIO with ESP-IDF framework. See original bareiron documentation.

## Configuration

Configuration options are in `include/globals.h`:

| Option | Description |
|--------|-------------|
| `PORT` | Server port (default: 25565) |
| `MAX_PLAYERS` | Maximum player slots (default: 16) |
| `view_distance` | Render distance in chunks (runtime configurable on Mac) |
| `TERRAIN_BASE_HEIGHT` | Base terrain Y level (default: 60) |
| `ALLOW_CHESTS` | Enable chest functionality |
| `DO_FLUID_FLOW` | Enable water/lava flow simulation |
| `ENABLE_OPTIN_MOB_INTERPOLATION` | Smooth mob movement between ticks |

### Mac-Specific Runtime Options
On 68k Mac, use the application menu to adjust:
- View distance (affects chunk loading range)
- Chunk cache size (based on available memory)
- Mob interpolation toggle

## Architecture

```
src/
├── main.c          # Server loop, packet routing, tick scheduling
├── packets.c       # Minecraft protocol encoding/decoding
├── procedures.c    # Game logic, player/entity management
├── worldgen.c      # Procedural terrain and cave generation
├── crafting.c      # Recipe system
├── tools.c         # Cross-platform utilities
├── serialize.c     # World persistence
├── mac68k_net.c    # Open Transport networking (Mac only)
└── mac68k_console.c # Mac UI and preferences (Mac only)

include/
├── globals.h       # Configuration and data structures
├── mac68k_net.h    # Mac networking header
└── mac68k_console.h # Mac UI header
```

## Credits

- Original [bareiron](https://github.com/p2r3/bareiron) by p2r3
- [Retro68](https://github.com/autc04/Retro68) cross-compiler by autc04
- Minecraft protocol documentation from [wiki.vg](https://wiki.vg)

## License

See original bareiron repository for license information.
