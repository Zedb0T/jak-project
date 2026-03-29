# libjakopengoal

A library for embedding Jak (from Jak and Daxter) into other game engines, inspired by [libsm64](https://github.com/libsm64/libsm64).

## Overview

libjakopengoal wraps the OpenGOAL runtime to expose Jak's gameplay logic, collision, animation, and rendering data through a simple C API. Jak continues to run in the GOAL VM internally, but external code can:

- Send static/dynamic collision geometry for Jak to interact with
- Read Jak's state (position, velocity, health, animation)
- Retrieve Jak's mesh data (skinned vertices) for rendering in another engine
- Send inputs, damage, teleport commands, etc.

## Architecture

```
Host Application (your engine)
    |
    v
libjakopengoal C API  (libjakopengoal.h)
    |
    v
C++ Bridge Layer  (jak_bridge.cpp)
    |                          |
    v                          v
GOAL VM Runtime          GOAL Bridge Code
(kernel, IOP,            (lib-jak-bridge.gc)
 collision, anim)
```

- **C API**: Clean C interface matching libsm64's pattern (init, create, tick, destroy)
- **Bridge Layer**: Thread-safe communication between the API and the GOAL runtime
- **GOAL VM**: The full OpenGOAL runtime runs headlessly in background threads
- **GOAL Bridge**: GOAL-side code that exports state, processes commands, and hooks collision

## API Quick Reference

```c
// Initialize (boots GOAL runtime headlessly)
jak_global_init(game_data_path, texture_buffer);

// Wait for boot
while (!jak_is_ready()) { /* sleep */ }

// Load collision
jak_static_surfaces_load(surfaces, count);

// Spawn Jak
int jak_id = jak_create(x, y, z);

// Each frame (1/60s)
jak_tick(jak_id, &inputs, &state, &geometry);

// State manipulation
jak_set_position(jak_id, x, y, z);
jak_take_damage(jak_id, from_x, from_y, from_z);

// Cleanup
jak_delete(jak_id);
jak_global_terminate();
```

## Building

The library is built as part of the jak-project CMake build:

```bash
cmake -DBUILD_LIBJAKOPENGOAL=ON ..
cmake --build . --target jakopengoal
cmake --build . --target jak_test_app
```

### With libsm64 integration

```bash
cmake -DBUILD_LIBJAKOPENGOAL=ON \
      -DWITH_LIBSM64=ON \
      -DLIBSM64_INCLUDE_DIR=/path/to/libsm64/src \
      -DLIBSM64_LIBRARY=/path/to/libsm64/build/libsm64.a \
      ..
cmake --build . --target jak_sm64_test
```

## Requirements

- The jak-project compiled game data (`iso_data/` and `out/` directories)
- All jak-project dependencies (SDL3, etc. - though display is disabled in headless mode)

## Differences from libsm64

| Feature | libsm64 | libjakopengoal |
|---------|---------|----------------|
| Runtime | Decompiled C code | Full GOAL VM (interpreted) |
| Boot time | Instant | ~5-10 seconds (boots GOAL runtime) |
| Tick rate | 30 FPS fixed | 60 FPS (GOAL's native rate) |
| Mesh output | Pre-transformed vertices | CPU-skinned vertices (bone transforms applied) |
| Collision | Direct array of triangles | Injected into GOAL's collide-cache |
| Game data | SM64 ROM file | Extracted/decompiled jak-project data |
| Threading | Single-threaded | Multi-threaded (EE, IOP, worker threads) |

## Test Programs

- `jak_test_app`: Standalone test with simulated inputs and collision
- `jak_sm64_test`: Combined test showing Mario and Jak side by side (requires libsm64)
