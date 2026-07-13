# SM64-Jak / libjakopengoal — Progress & Handoff

Status snapshot for the long-running "Jak in other game engines" effort.
Goal: build a libsm64-style C library (`libjakopengoal`) that boots Jak's GOAL
runtime headlessly and exposes Jak as a drop-in character for any host engine.
First integration target is **SM64EX** (Mario replaced by Jak).

If you're picking this up cold, read this top-to-bottom, then look at
[libjakopengoal/README.md](libjakopengoal/README.md) for the public C API and
[CLAUDE.md](CLAUDE.md) for build/runtime gotchas.

---

## TL;DR — Where we are right now

- **libjakopengoal**: working DLL. Boots GOAL VM in a background thread, ticks
  Jak at 60 fps, accepts external input, accepts external collision triangles,
  emits a pre-skinned mesh + textures + audio events each frame.
- **SM64-Jak integration**: Mario is hidden, Jak is rendered in his place,
  walks on SM64 collision, inherits SM64 platform motion, throws shells, ground
  pounds, swims in SM64 water, takes damage. Controller works, including
  multi-pad index override.
- **Blender previewer**: separate addon (`libjakopengoal_blender/`) that loads
  the thin DLL via shared-memory IPC and previews Jak in a Blender viewport.
  Used for animation/rendering debugging without a full SM64 build.
- **Texture replacement system**: SM64 textures can be swapped at runtime.
- **Debug menu**: in-game overlay for tweaking parameters.
- **48 commits in** since `084736a33c First test`. Upstream merge base is
  `aa4627dc5f`.

---

## Big-picture architecture

```
┌──────────────────────────────────────────────────────────────────┐
│  Host engine  (SM64EX / Blender / future: anything)              │
│  ─────────────────────────────────────────────────────────────── │
│  sm64-jak/src/pc/jak/jakopengoal.c    ← SM64-side integration    │
│  libjakopengoal/blender/...           ← Blender-side integration │
└────────────────┬─────────────────────────────────────────────────┘
                 │ C API (libjakopengoal.h)
┌────────────────▼─────────────────────────────────────────────────┐
│  libjakopengoal.dll                                              │
│  ─────────────────────────────────────────────────────────────── │
│  libjakopengoal.cpp  ← lifecycle, threads                        │
│  jak_bridge.cpp      ← collision inject / input patch / mesh     │
│                        extract / audio shim / camera matrix      │
│  jak_ipc_shm.cpp     ← shared-mem IPC for Blender thin client    │
└────────────────┬─────────────────────────────────────────────────┘
                 │
┌────────────────▼─────────────────────────────────────────────────┐
│  OpenGOAL runtime (headless GOAL VM)                             │
│  Patched files: kboot.cpp, kmachine.cpp, libpad.cpp, iop.cpp,    │
│                 IOP_Kernel.cpp, sndshim.cpp, kprint, ...         │
│  GOAL bridge:   goal_src/jak1/pc/lib-jak-bridge.gc               │
└──────────────────────────────────────────────────────────────────┘
```

### Repo layout (only the new/touched parts)

```
libjakopengoal/
├─ CMakeLists.txt            ← builds jakopengoal DLL + static + thin + tests
├─ README.md                 ← public API reference (Step 1..8 integration)
├─ include/
│  ├─ libjakopengoal.h       ← THE public C API header
│  └─ jak_ipc_protocol.h     ← shared-mem IPC wire format (Blender)
├─ src/
│  ├─ libjakopengoal.cpp     ← API impl, runtime lifecycle, thread mgmt
│  ├─ libjakopengoal_thin.cpp← thin client: talks to server via shm
│  ├─ jak_bridge.{cpp,h}     ← all the GOAL↔C interop
│  ├─ jak_bridge_state.cpp   ← per-Jak instance state
│  ├─ jak_ipc_shm.{cpp,h}    ← shared-memory ring buffer
│  ├─ jak_server_main.cpp    ← out-of-process server entrypoint
│  └─ runtime_launcher.cpp
├─ blender/
│  └─ libjakopengoal_blender/ ← Blender addon (Python)
└─ test/
   └─ main_visual.cpp        ← SDL3+GL test harness (Mario+Jak side by side)

sm64-jak/                    ← SM64EX fork, MinGW build
└─ src/pc/jak/
   ├─ jakopengoal.c          ← DLL loader, input map, render hook, collide sync
   └─ jakopengoal.h

goal_src/jak1/pc/
└─ lib-jak-bridge.gc         ← GOAL-side bridge: input patch, camera, surface

# Patched GOAL files (search "JAKOPENGOAL" or look at git log to find changes):
goal_src/jak1/engine/collide/collide-cache.gc    ← inject external surfaces
goal_src/jak1/engine/target/logic-target.gc      ← platform attach
goal_src/jak1/engine/target/target2.gc           ← swimming
goal_src/jak1/engine/game/powerups.gc            ← powerup hooks
goal_src/jak1/engine/game/game-info.gc
goal_src/jak1/engine/game/settings.gc
goal_src/jak1/engine/level/level-info.gc

# Patched C++ runtime files:
game/kernel/jak1/kboot.cpp        ← headless boot hook
game/kernel/jak1/kmachine.cpp     ← C symbol registration
game/sce/libpad.cpp               ← scePadRead patch (external input)
game/sce/iop.cpp                  ← IOP shims for audio
game/sound/sndshim.cpp            ← audio event passthrough
game/system/IOP_Kernel.cpp

# Top-level scripts
build_sm64jak.bat                 ← orchestrated full build (recommended)
launch_sm64jak.bat                ← run the built game
release_sm64jak.bat               ← package a release zip
sm64-jak/build/us_pc/run_sm64jak.bat ← launcher w/ controller-index override
```

---

## Milestone timeline (what was done, in order)

Commits below are the canonical "start here when re-reading" anchors.

### Phase 0 — Boot & first signs of life
| Commit | What landed |
|---|---|
| `084736a33c` | **First test.** CMakeLists for DLL, public API header, `jak_bridge.cpp`, `libjakopengoal.cpp`, headless boot via patched `kboot.cpp`, `lib-jak-bridge.gc`. Three test harnesses (`main.c`, `main_combined.c`, `main_visual.cpp`). |
| `3090decae1` | Jak renders as a stick-figure skeleton (joint positions only). 581-line bridge expansion + visual harness. |
| `321a8725c3` | Slowed bridge tick to match Jak's native 60Hz. |
| `d1ebab1267` | Fixed stick→world direction mapping. |

### Phase 1 — Collision in
| Commit | What landed |
|---|---|
| `c909bf1e01` | **Jak stands on external geometry.** Patched `collide-cache.gc` to accept injected triangles. Bridge converts host triangles into GOAL's collide-cache format. |
| `b3860ca555` | Dynamic platform spawning (host spawns/despawns triangles at runtime). |
| `b5c84995a5` | Camera fix — write rotation matrix into `*math-camera*` so stick is world-relative. |

### Phase 2 — Rendering, audio, textures
| Commit | What landed |
|---|---|
| `36ef905207` | **Jak renders as a real mesh** (CPU-skinned vertices extracted from GOAL renderer). |
| `55435ef2ff` | README rewrite — public API now stable enough to document. |
| `a53473a46b` | Audio passthrough — `sndshim.cpp` + `IOP_Kernel.cpp` shims emit sound events to host. |
| `d0bb2ba9c1` | Textures + better tick timing. Texture atlas uploaded as one GL texture; bridge writes vertex UVs. |
| `34a348a6d5` | Eye textures (mostly working — kept as a known-rough-edge). |

### Phase 3 — Blender frontend (out-of-process)
| Commit | What landed |
|---|---|
| `8821616d62` | **Blender addon + IPC.** New `libjakopengoal_thin.cpp` client, `jak_ipc_shm.cpp` ring buffer, `jak_server_main.cpp` server. Lets Blender drive Jak without loading the huge runtime DLL in-process. Adds the `libjakopengoal_blender/` Python addon. |
| `32981a46be` | Rotation API for the visual test. |
| `b1396c2627` | Moving platforms in the visual test. |

### Phase 4 — SM64 integration begins
| Commit | What landed |
|---|---|
| `ca82c50f69` | **Initial Mario↔Jak couple.** First commit on the SM64 side. |
| `8e712ada1c` | "Jak walks where Mario is, Mario glued to Jak." Mario invisible, Jak rendered, position synced. The conceptual breakthrough. |
| `0b3149c2fa` | Memory layout cleanup. |
| `b15aecc602` | jakopengoal.c hookup pass. |

### Phase 5 — SM64 mechanics, Jak-flavored
| Commit | What landed |
|---|---|
| `08d007f024` | First version of throwing (shells). |
| `fb9610852a` | Spinning into things (spin-attack damage applied to SM64 objects). |
| `92b8f68bf3` | First version of slippery slopes (surface-type pass-through). |
| `99142b07fd` | Fix painting (level-warp) crashes. |
| `faaebf0e0d` | Stop Mario voice lines / sound effects (`audio/external.c` gate). |
| `7f317f8c98` | **Ground pound.** Also added the `jak_model_exporter` tool (large addition under `tools/`). |
| `ab37934d67` → `aafb1175ec` → `e7c4d228d9` | Swimming: added, reverted, redone cleanly. Now patches `target2.gc` + `lib-jak-bridge.gc`, host calls `jak_set_swimming`. |
| `154f941df9` | Re-enabled some Mario rendering for unimplemented Jak actions (fallback so the game still works). |
| `733ef2042c` | Fix bottom-height in `level-info.gc`. |
| `ad8ac99438` | **In-game debug menu** (286-line addition to jakopengoal.c). |
| `e924b408fd` | **Texture replacement system** (`gfx_pc.c`, 120 lines). |
| `c32636de34` | Depth-rendering fix in `rendering_graph_node.c`. |
| `3857a20f17` | **Moving platforms working** — Jak rides SM64 moving platforms. New `jak_surface_object_move` API. |

### Phase 6 — Polish & tuning
| Commit | What landed |
|---|---|
| `dc318f9a44` | Rotations fixed (small two-line correction). |
| `5765a31753` | 10-second momentum behaviour after jump release. |
| `ceaae4391f` | Fix infinite jumps. |
| `674943e5d5` | Shell drop-off at end position (cutscene tweak). |
| `4b46e7d763` | "No more wall clip." |
| `fa4513efe5` | "This works" — controller fixes in `controller_sdl2.c` (+26) and `jakopengoal.c` (+43). |
| `87681431b6` | More controller polish. |
| `75b306b649` | `gamecontrollerdb.txt` for broad SDL controller support. |
| `38ece52003`, `cfa57050c9` | More controller tweaks. |
| `41835a7593` | **`run_sm64jak.bat` launcher** with controller index override. |
| `f67d9544e1` | Final controller-mapping pass + `controllers.txt`. |

---

## How to pick up where we left off

### Build everything from scratch
```bat
build_sm64jak.bat
```
Walks through: prereq check → DLL build (MSVC) → DLL copy → SM64 build (MinGW)
→ generates `launch_sm64jak.bat`. Each step has a Y/N skip prompt.

### Manual steps (if `build_sm64jak.bat` breaks)
See [CLAUDE.md](CLAUDE.md) → "Build System". Critical bits:
- DLL: **Release config only** (`cmake --build build --target jakopengoal --config Release`)
- Copy ALL DLLs from `build/bin/Release/*.dll` to `sm64-jak/build/us_pc/`
- SM64: must pass `WINDOWS_BUILD=1 JAKOPENGOAL=1` AND wrap `TMPDIR=/tmp` AND
  PATH must include `/c/msys64/mingw64/bin`.

### Iterate on GOAL code
Edit `.gc` file → in the running `goalc` REPL: `(mi)` rebuilds, then either
`(ml "goal_src/jak1/pc/lib-jak-bridge.gc")` to hot-patch a single file, or
restart `sm64.us.f3dex2e.exe` if you changed something the bridge boots with.

### Iterate on C++ bridge
Rebuild DLL → copy to `sm64-jak/build/us_pc/` → relaunch SM64.

### Iterate on SM64 side
Rebuild SM64 (MinGW) → relaunch. No DLL rebuild needed.

---

## Things that are intentionally rough / known TODO

These are the "yeah we know" items — not bugs to file, but worth flagging so
you don't go chasing them as regressions.

- **Eye textures**: working but the comment in `34a348a6d5` is "kind of works."
  Expect occasional wrongness.
- **Mario fallback rendering**: `154f941df9` re-enabled Mario for actions Jak
  doesn't implement. The visibility flag toggle in `mario.c` is guarded by
  `#ifndef JAKOPENGOAL` — still some duality.
- **Swimming**: shipped after one revert. Works but is gated on SM64 water
  flags being injected per the `project_water_system.md` notes — touchy.
- **Collision cap**: GOAL's collide-cache is `MAX_TRIS=461`. SM64 levels are
  2000+. We spatially filter to the AABB around Jak each frame. If you see Jak
  fall through far geometry, the filter is the suspect, not the level.
- **Audio**: SM64's Mario voice lines are silenced (`faaebf0e0d`). Jak's own
  sounds route through `sndshim.cpp` → host callback. SM64 BGM still plays.
- **Controller**: works but needed a lot of iteration. If a new controller
  doesn't bind, check `controllers.txt` and the `run_sm64jak.bat` override.
- **Two "fart" commits** (`38ece52003`, `cfa57050c9`, `87681431b6`): these are
  real controller tweaks, just badly named. Don't ignore them.

---

## Where the public API lives

Single source of truth: **[libjakopengoal/include/libjakopengoal.h](libjakopengoal/include/libjakopengoal.h)**.

Full integration guide with step-by-step host-engine setup, button bitmasks,
surface types, action constants, and per-function reference is in
[libjakopengoal/README.md](libjakopengoal/README.md).

The SM64 side that consumes this API is one file:
[sm64-jak/src/pc/jak/jakopengoal.c](sm64-jak/src/pc/jak/jakopengoal.c)
— ~75 KB, the reference example of a host integration.

---

## When you come back, do this first

1. `git log --oneline 084736a33c^..HEAD` — see if anything new since this doc.
2. Read [CLAUDE.md](CLAUDE.md) for build env / unit conversions / GOAL traps.
3. If something's broken, run `build_sm64jak.bat` from a clean tree — it will
   tell you which stage fails.
4. To add a new host engine: copy `sm64-jak/src/pc/jak/jakopengoal.c` as a
   template and re-read the integration guide in `libjakopengoal/README.md`.
