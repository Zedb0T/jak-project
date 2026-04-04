# CLAUDE.md — jak-project

## Self-Updating Instructions
When you learn something new during a conversation that would be valuable in future sessions — architecture decisions, build gotchas, rendering fixes, user preferences, or project context — update the memory system at `C:\Users\ZedBo\.claude\projects\C--Users-ZedBo-OneDrive-Documents-GitHub-jak-project\memory\`. Create or update memory files and keep MEMORY.md in sync. Do this proactively without being asked.

## Project Overview
This is **jak-project** (OpenGOAL) — the Jak and Daxter PC port. The active work is embedding Jak as a character inside **SM64EX** (Super Mario 64 PC port) via a shared library (`libjakopengoal`).

### Architecture
- **libjakopengoal DLL** (`libjakopengoal/`): MSVC-built DLL that boots the GOAL VM headlessly, exposes Jak's state/collision/rendering through a C API. Key files:
  - `libjakopengoal/include/libjakopengoal.h` — public C API header
  - `libjakopengoal/src/libjakopengoal.cpp` — API implementation (lifecycle, tick, collision)
  - `libjakopengoal/src/jak_bridge.cpp` — bridge between GOAL VM and C API (skinning, mesh extraction, collision sync)
  - `libjakopengoal/src/jak_bridge.h` — internal bridge types
- **SM64EX fork** (`sm64-jak/`): MinGW-built SM64 with Jak integration. Key files:
  - `sm64-jak/src/pc/jak/jakopengoal.c` — main integration (DLL loading, input mapping, rendering, collision sync)
  - `sm64-jak/src/pc/jak/jakopengoal.h` — SM64-side header
  - `sm64-jak/src/game/mario.c` — modified to hide Mario's rendering with `#ifndef JAKOPENGOAL`
  - `sm64-jak/src/pc/gfx/gfx_pc.c` — calls `jak_sm64_render()` before buffer swap

### Build System
**DLL (MSVC via CMake):**
```bash
cd jak-project && cmake --build build --target jakopengoal --config Release
```
- Always build Release config (Debug requires MSVC debug runtime DLLs)
- After build, copy ALL DLLs: `cp build/bin/Release/*.dll sm64-jak/build/us_pc/`
- DLL has ~15 companion DLLs (mman.dll, imgui.dll, SDL3.dll, common.dll, fmt.dll, etc.) that must be co-located

**SM64EX (MinGW via make):**
```bash
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
cd sm64-jak
/c/msys64/usr/bin/env.exe TMPDIR=/tmp TMP=/tmp TEMP=/tmp OS=Windows_NT /c/msys64/usr/bin/make.exe -j$(nproc) JAKOPENGOAL=1 WINDOWS_BUILD=1
```
- Must pass `WINDOWS_BUILD=1` explicitly or `-lGL -ldl` linker errors occur
- Must wrap with `TMPDIR=/tmp` env vars or GCC fails with temp file permission errors
- `make` is at `/c/msys64/usr/bin/make.exe`, `sdl2-config` needs `/c/msys64/mingw64/bin` in PATH

**Launch:**
```bash
cd sm64-jak/build/us_pc
export JAK_DATA_PATH="C:\\Users\\ZedBo\\OneDrive\\Documents\\GitHub\\jak-project"
./sm64.us.f3dex2e.exe --skip-intro
```

### Key Technical Details

**Unit conversions:**
- `UNITS_TO_METERS = 4096.0f / 50.0f` — SM64/render units to GOAL meters (collision)
- `METERS_TO_UNITS = 50.0f / 4096.0f` — GOAL meters to render units (mesh positions)

**Collision system:**
- GOAL's collide-cache has MAX_TRIS=461 — only 461 triangles fit
- SM64 levels have 2000+ surfaces — spatial filtering using AABB overlap with GOAL's collide-box bounding box is essential
- Collide-box bbox is at offset 28 in GOAL's collide-cache structure (min vector), offset 44 (max vector)
- Collision is cleared and reloaded from SM64's `sSurfacePool` every frame
- All triangles treated as `SURFACE_DEFAULT (0x0000)` for now

**Rendering:**
- Jak's mesh: skinned vertices from GOAL VM, extracted in jak_bridge.cpp
- Color buffer: **4 floats per vertex (RGBA)** — must be consistent across DLL and SM64 side
- Texture: atlas built from FR3 model textures, uploaded as single GL texture
- GL state for Jak rendering: depth test ON, depth mask ON, blend OFF, alpha test OFF
- Uses `GL_COMBINE` tex env: RGB = texture * vertex color, Alpha = vertex color only (ignores texture alpha to prevent hair/edge transparency)
- Vertex alpha forced to 1.0 (always opaque)

**Mario visibility:**
- `GRAPH_RENDER_INVISIBLE = (1 << 4)` set on `gMarioObject` every frame
- SM64's `mario.c:1758` normally clears this flag — guarded with `#ifndef JAKOPENGOAL`

**Input mapping:**
- Stick X is negated: `inputs->stick_x = -(ctrl->stickX / 64.0f)` to fix left/right swap

**Logging:**
- SM64 is built with `-mwindows` (GUI app) — no stdout/stderr
- Debug logging goes to file: `C:\Users\ZedBo\jak_debug.log` via `JAK_LOG()` / `JAK_ERR()` macros

**GOAL memory access from C++:**
- `offset-assert` values in all-types.gc are relative to allocation start
- GOAL object pointer is 4 bytes AFTER allocation start (type tag at -4)
- Field at `offset-assert N` is accessed at `ptr + (N - 4)` from C++

### Common Pitfalls
- DLL LoadLibrary error 126 = missing dependency DLL, not the DLL itself. Use `objdump -p` to check deps
- Color buffer stride mismatch between DLL (was RGB/3) and renderer (RGBA/4) caused garbled vertex colors
- `GL_MODULATE` tex env passes texture alpha through — causes transparency on hair/edges
- `GL_BLEND` with `GL_SRC_ALPHA` makes semi-transparent texture pixels visible — use `GL_COMBINE` to ignore texture alpha instead
