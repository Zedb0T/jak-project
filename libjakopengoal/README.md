# libjakopengoal

A C library that embeds the Jak and Daxter GOAL runtime as a standalone character engine. Inspired by [libsm64](https://github.com/libsm64/libsm64), it lets you drop Jak into any project with full physics, animation, and collision.

## Requirements

- **CMake** 3.16+
- **C++17** compiler (MSVC 2019+, GCC 9+, Clang 10+)
- **Jak 1 game data** extracted via [OpenGOAL](https://opengoal.dev) (the `iso_data/jak1` and `out/jak1` directories)
- **OpenGL 3.3+** (for rendering the output mesh)

### Optional

- **libsm64** + SM64 baserom for the combined Mario + Jak test demo
- **SDL3** for the visual test programs

## Building

libjakopengoal is built as part of the jak-project CMake tree.

```bash
# Clone the repository
git clone --recursive https://github.com/open-goal/jak-project.git
cd jak-project

# Configure
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo

# Build the static library
cmake --build build --config RelWithDebInfo --target jakopengoal_static

# Build the visual test (requires SDL3 + libsm64)
cmake --build build --config RelWithDebInfo --target mario_jak_test
```

This produces:

| Artifact | Path |
|---|---|
| Static library | `build/lib/RelWithDebInfo/jakopengoal_static.lib` |
| Header | `libjakopengoal/include/libjakopengoal.h` |
| Test binary | `build/bin/RelWithDebInfo/mario_jak_test.exe` |

## Integration Guide

### Step 1: Include the Header

```c
#include "libjakopengoal.h"
```

Link against `jakopengoal_static` and its transitive dependencies: `runtime`, `common`, `fmt`.

### Step 2: Initialize the Runtime

```c
// Allocate texture buffer (RGBA)
uint8_t* texture = calloc(JAK_TEXTURE_WIDTH * JAK_TEXTURE_HEIGHT * 4, 1);

// Boot the GOAL VM (async -- returns immediately)
// game_data_path is the root of the jak-project directory containing
// iso_data/jak1/ and out/jak1/
int result = jak_global_init("/path/to/jak-project", texture);
if (result < 0) {
    printf("Failed to initialize: %d\n", result);
    return;
}

// Poll until the runtime finishes booting
while (!jak_is_ready()) {
    // pump your event loop, render loading screen, etc.
    sleep_ms(100);
}
```

Boot takes several seconds as the GOAL VM loads and compiles game code.

### Step 3: Load Collision Surfaces

Provide the world geometry that Jak will collide with. Each surface is a triangle with vertices in world-space coordinates. Normals are auto-computed if left as zero.

```c
// Build a flat ground plane from 2 triangles
JakSurface ground[2];
memset(ground, 0, sizeof(ground));

ground[0].type = JAK_SURFACE_STONE;
ground[0].vertices[0][0] = -5000; ground[0].vertices[0][1] = 0; ground[0].vertices[0][2] = -5000;
ground[0].vertices[1][0] =  5000; ground[0].vertices[1][1] = 0; ground[0].vertices[1][2] =  5000;
ground[0].vertices[2][0] = -5000; ground[0].vertices[2][1] = 0; ground[0].vertices[2][2] =  5000;

ground[1].type = JAK_SURFACE_STONE;
ground[1].vertices[0][0] = -5000; ground[1].vertices[0][1] = 0; ground[1].vertices[0][2] = -5000;
ground[1].vertices[1][0] =  5000; ground[1].vertices[1][1] = 0; ground[1].vertices[1][2] = -5000;
ground[1].vertices[2][0] =  5000; ground[1].vertices[2][1] = 0; ground[1].vertices[2][2] =  5000;

jak_static_surfaces_load(ground, 2);
```

Surfaces can be reloaded at any time to add or remove geometry. Calling `jak_static_surfaces_load` replaces all previously loaded static surfaces.

### Step 4: Create a Jak Instance

```c
// Spawn Jak at position (x, y, z) in world units
int32_t jak_id = jak_create(0.0f, 100.0f, 0.0f);
```

### Step 5: Allocate Geometry Buffers

The library writes Jak's pre-skinned mesh into caller-owned buffers each frame.

```c
JakGeometryBuffers geo;
geo.position = calloc(JAK_GEO_MAX_TRIANGLES * 3 * 3, sizeof(float));  // xyz per vertex
geo.normal   = calloc(JAK_GEO_MAX_TRIANGLES * 3 * 3, sizeof(float));  // xyz per vertex
geo.color    = calloc(JAK_GEO_MAX_TRIANGLES * 3 * 4, sizeof(float));  // rgba per vertex
geo.uv       = calloc(JAK_GEO_MAX_TRIANGLES * 3 * 2, sizeof(float));  // uv per vertex
geo.num_triangles_used = 0;
```

### Step 6: Tick Each Frame

```c
JakInputs inputs = {};
JakState  state  = {};

// Camera look direction (world-space vector from camera toward target)
inputs.cam_x = cam_target_x - cam_pos_x;
inputs.cam_z = cam_target_z - cam_pos_z;

// Stick input (normalized -1 to 1, positive Y = forward)
inputs.stick_x = gamepad_left_x;
inputs.stick_y = gamepad_left_y;

// Buttons
if (jump_pressed)   inputs.buttons |= JAK_BUTTON_X;
if (punch_pressed)  inputs.buttons |= JAK_BUTTON_SQUARE;
if (roll_pressed)   inputs.buttons |= JAK_BUTTON_CIRCLE;
if (crouch_pressed) inputs.buttons |= JAK_BUTTON_L1;

// Advance one frame
jak_tick(jak_id, &inputs, &state, &geo);

// state is now populated:
//   state.position[3]  - world position
//   state.velocity[3]  - velocity
//   state.action       - current action (JAK_ACTION_*)
//   state.on_ground    - true if standing on a surface
//   state.hp           - health (0.0 = dead, 1.0 = full)

// geo is now populated:
//   geo.num_triangles_used - number of triangles in the mesh
//   geo.position/normal/color/uv - vertex data
```

### Step 7: Render the Mesh

The mesh is pre-skinned (bone transforms already applied). Upload to your GPU as a plain triangle list.

```c
int num_verts = geo.num_triangles_used * 3;

glBindBuffer(GL_ARRAY_BUFFER, vbo_pos);
glBufferSubData(GL_ARRAY_BUFFER, 0, num_verts * 3 * sizeof(float), geo.position);

glBindBuffer(GL_ARRAY_BUFFER, vbo_nrm);
glBufferSubData(GL_ARRAY_BUFFER, 0, num_verts * 3 * sizeof(float), geo.normal);

glBindBuffer(GL_ARRAY_BUFFER, vbo_col);
glBufferSubData(GL_ARRAY_BUFFER, 0, num_verts * 4 * sizeof(float), geo.color);

glDrawArrays(GL_TRIANGLES, 0, num_verts);
```

### Step 8: Shutdown

```c
jak_delete(jak_id);
jak_global_terminate();
free(texture);
free(geo.position);
free(geo.normal);
free(geo.color);
free(geo.uv);
```

## API Reference

### Lifecycle

| Function | Description |
|---|---|
| `jak_global_init(path, texture)` | Boot the GOAL runtime asynchronously. Returns 0 on success. |
| `jak_is_ready()` | Returns `true` once the VM has finished booting. |
| `jak_global_terminate()` | Shut down the runtime and free internal resources. |

### Callbacks

| Function | Description |
|---|---|
| `jak_register_debug_print(fn)` | Receive debug log output from the runtime. |
| `jak_register_play_sound(fn)` | Receive sound effect requests for external playback. |

### Instance Management

| Function | Description |
|---|---|
| `jak_create(x, y, z)` | Spawn Jak at a world position. Returns instance ID (>= 0) or error (< 0). |
| `jak_delete(jak_id)` | Remove a Jak instance. |
| `jak_tick(id, inputs, state, geo)` | Advance one frame. Reads inputs, writes state and mesh geometry. |

### Static Collision

| Function | Description |
|---|---|
| `jak_static_surfaces_load(surfaces, count)` | Load static collision triangles. Replaces any previously loaded surfaces. |
| `jak_static_surfaces_clear()` | Remove all static collision surfaces. |

### Dynamic Collision Objects

| Function | Description |
|---|---|
| `jak_surface_object_create(obj)` | Create a dynamic (moving) collision object. Returns object ID. |
| `jak_surface_object_move(id, transform)` | Update the position/rotation of a dynamic object. |
| `jak_surface_object_delete(id)` | Remove a dynamic collision object. |

### Collision Queries

| Function | Description |
|---|---|
| `jak_find_floor_height(x, y, z)` | Find the ground Y height at the given (x, z) position. |

### State Manipulation

| Function | Description |
|---|---|
| `jak_set_position(id, x, y, z)` | Teleport Jak to a new position. |
| `jak_set_velocity(id, vx, vy, vz)` | Set Jak's velocity directly. |
| `jak_set_angle(id, yaw)` | Set facing angle (radians). |
| `jak_set_action(id, action)` | Force a specific action state (`JAK_ACTION_*`). |
| `jak_set_health(id, hp)` | Set health (0.0 to 1.0). |

### Damage and Health

| Function | Description |
|---|---|
| `jak_take_damage(id, x, y, z)` | Apply damage from a world position (knockback direction). |
| `jak_heal(id, amount)` | Restore health. |
| `jak_kill(id)` | Immediately kill Jak. |
| `jak_give_eco(id, eco_type)` | Give an eco pickup to Jak. |

### Texture

| Function | Description |
|---|---|
| `jak_get_texture_info()` | Get texture atlas dimensions and metadata. |

## Input Reference

### Camera

Movement is camera-relative. Pass the camera's look direction as a world-space vector via `cam_x` and `cam_z`. The library normalizes this internally and builds the rotation matrix that transforms stick input into world-space movement.

```c
// Example: orbiting camera
float cam_x = target_x + radius * cosf(angle);
float cam_z = target_z + radius * sinf(angle);

inputs.cam_x = target_x - cam_x;  // look direction X
inputs.cam_z = target_z - cam_z;  // look direction Z
```

### Stick

| Field | Range | Description |
|---|---|---|
| `stick_x` | -1.0 to 1.0 | Left analog X (positive = right) |
| `stick_y` | -1.0 to 1.0 | Left analog Y (positive = forward) |
| `r_stick_x` | -1.0 to 1.0 | Right analog X |
| `r_stick_y` | -1.0 to 1.0 | Right analog Y |

### Buttons

| Constant | Bit | Jak Action |
|---|---|---|
| `JAK_BUTTON_X` | `1 << 14` | Jump |
| `JAK_BUTTON_SQUARE` | `1 << 15` | Punch / Spin Kick |
| `JAK_BUTTON_CIRCLE` | `1 << 13` | Roll / Yellow Eco Attack |
| `JAK_BUTTON_TRIANGLE` | `1 << 12` | - |
| `JAK_BUTTON_L1` | `1 << 10` | Crouch / Slide |
| `JAK_BUTTON_R1` | `1 << 11` | - |
| `JAK_BUTTON_L2` | `1 << 8` | - |
| `JAK_BUTTON_R2` | `1 << 9` | - |
| `JAK_BUTTON_DPAD_UP` | `1 << 4` | - |
| `JAK_BUTTON_DPAD_DOWN` | `1 << 6` | - |
| `JAK_BUTTON_DPAD_LEFT` | `1 << 7` | - |
| `JAK_BUTTON_DPAD_RIGHT` | `1 << 5` | - |
| `JAK_BUTTON_START` | `1 << 3` | - |
| `JAK_BUTTON_SELECT` | `1 << 0` | - |

### Surface Types

| Constant | Value | Description |
|---|---|---|
| `JAK_SURFACE_STONE` | 0 | Default solid ground |
| `JAK_SURFACE_ICE` | 1 | Slippery surface |
| `JAK_SURFACE_GRASS` | 2 | Grass |
| `JAK_SURFACE_WOOD` | 3 | Wood |
| `JAK_SURFACE_SAND` | 4 | Sand |
| `JAK_SURFACE_METAL` | 5 | Metal |
| `JAK_SURFACE_DIRT` | 6 | Dirt |
| `JAK_SURFACE_DEFAULT` | 7 | Default |

### Action States

| Constant | Value |
|---|---|
| `JAK_ACTION_IDLE` | 0 |
| `JAK_ACTION_WALK` | 1 |
| `JAK_ACTION_RUN` | 2 |
| `JAK_ACTION_JUMP` | 3 |
| `JAK_ACTION_DOUBLE_JUMP` | 4 |
| `JAK_ACTION_SPIN_ATTACK` | 5 |
| `JAK_ACTION_PUNCH` | 6 |
| `JAK_ACTION_DIVE` | 7 |
| `JAK_ACTION_ROLL` | 8 |
| `JAK_ACTION_CROUCH` | 9 |
| `JAK_ACTION_FALL` | 10 |
| `JAK_ACTION_HIT` | 11 |
| `JAK_ACTION_DEATH` | 12 |
| `JAK_ACTION_SWIM` | 13 |

## Architecture

```
Your Application
    |
    v
libjakopengoal.h  (C API)
    |
    v
libjakopengoal.cpp  (API implementation, thread management)
    |
    v
jak_bridge.cpp  (collision injection, input routing, mesh extraction)
    |
    v
OpenGOAL Runtime  (headless GOAL VM in background thread)
```

The GOAL virtual machine runs headlessly in a dedicated thread. The bridge layer:

- **Collision**: Intercepts `fill-from-background` in the collide-cache to inject external triangle surfaces into GOAL's collision system
- **Input**: Patches `scePadRead` to feed external controller data instead of reading from hardware
- **Camera**: Writes the camera rotation matrix into `*math-camera*` so stick input is correctly transformed to world-space movement
- **Mesh**: Extracts CPU-skinned vertex data from the renderer each frame
- **State**: Reads Jak's position, velocity, health, and animation state from GOAL memory

## Differences from libsm64

| Feature | libsm64 | libjakopengoal |
|---|---|---|
| Runtime | Decompiled C code | Full GOAL VM (interpreted) |
| Boot time | Instant | ~5-10 seconds |
| Tick rate | 30 FPS | 60 FPS (GOAL native) |
| Mesh output | Pre-transformed vertices | CPU-skinned vertices |
| Collision | Direct triangle array | Injected into collide-cache |
| Game data | SM64 ROM file | Extracted jak-project data |
| Threading | Single-threaded | Multi-threaded (EE, IOP threads) |

## Running the Test Demo

The `mario_jak_test` binary renders Mario (via libsm64) and Jak side by side on shared collision geometry.

```bash
mario_jak_test.exe \
  --rom path/to/baserom.us.z64 \
  --jak-data path/to/jak-project
```

### Test Controls

| Input | Action |
|---|---|
| Arrow keys / Left stick | Move |
| X / Gamepad South | Jump |
| S / Gamepad West | Punch / Spin Kick |
| C / Gamepad East | Roll |
| Q / LB | L1 |
| E / RB | R1 |
| Shift L/R / Right stick | Rotate camera |
| Tab | Toggle camera follow (Mario / Jak) |
| Escape | Quit |

The test also demonstrates dynamic surface spawning: jump 4 times to spawn a blue platform, jump 7 times to despawn it.

## License

This library wraps the OpenGOAL project. See the root repository for license information. Game assets from Jak and Daxter are owned by Sony Interactive Entertainment / Naughty Dog.
