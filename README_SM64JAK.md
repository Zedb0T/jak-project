# SM64-Jak — Jak & Daxter inside Super Mario 64

**Play as Jak — the real Jak — inside Super Mario 64.** This isn't a model swap:
a full headless [OpenGOAL](https://opengoal.dev) runtime (the Jak & Daxter PC
port) runs *inside* the SM64 process. Jak's original PS2 game logic drives his
movement, animation, physics, and sounds, while SM64 provides the world,
collision, enemies, and objectives. Mario is hidden; Jak walks his levels.

Built on two pieces:

- **`libjakopengoal/`** — a [libsm64](https://github.com/libsm64/libsm64)-style
  embeddable library. Boots the GOAL VM headlessly, accepts external input and
  collision triangles, and emits a pre-skinned, textured Jak mesh every frame.
  Works with any host engine (SM64 today; a Blender previewer also exists).
- **`sm64-jak/`** — an [sm64ex](https://github.com/sm64pc/sm64ex) fork with the
  integration: DLL loading, input mapping, per-frame collision sync, rendering,
  and debug tooling. The whole integration is one file,
  [`sm64-jak/src/pc/jak/jakopengoal.c`](sm64-jak/src/pc/jak/jakopengoal.c).

Deep-dive docs: [SM64_JAK_PROGRESS.md](SM64_JAK_PROGRESS.md) (history & handoff)
and [libjakopengoal/README.md](libjakopengoal/README.md) (public C API).

---

## How it works

```
SM64EX (MinGW)  ──input/collision──▶  libjakopengoal.dll (MSVC)
     ▲                                        │
     │                                        ▼
  Jak mesh + audio events  ◀──────  OpenGOAL runtime (headless GOAL VM,
  rendered in SM64's GL                real Jak 1 engine code)
```

- SM64's ~2000+ surfaces are spatially filtered to the ~461-triangle window
  GOAL's collide-cache supports, re-synced around Jak every frame.
- Jak's skinned vertices are extracted from the GOAL renderer on the CPU and
  drawn in SM64's OpenGL context with his real textures.
- SM64 water volumes drive GOAL's actual swimming system; moving platforms,
  damage, shell riding, and ground pounds all round-trip between engines.
- Optionally, the *real* gk renderer runs into a hidden window, giving a
  picture-in-picture view of the world Jak inhabits on the GOAL side.

## Features

- Jak's native moveset: run, jump, double jump, spin, punch, dive, ground
  pound, swim — driven by original game code
- Mario fallback for not-yet-ported mechanics (cannons, wing cap, poles…),
  toggleable per-mechanic in-game
- gk world picture-in-picture (bottom-left) with the actual GOAL-side world
- **W-key focus mode**: hold `W` to route your controller into the Jak world —
  navigate gk's own debug menus from inside SM64
- Dear ImGui overlay (`Left Alt`) with PiP toggle and a warp-to-any-level
  dropdown
- Runtime texture replacement, in-game debug menu, controller remapping

## Quickstart

Requirements: Windows, MSVC (VS2022), MSYS2/MinGW64, a legally obtained Jak &
Daxter ISO (extracted via OpenGOAL) and an SM64 US ROM (for sm64ex asset
extraction).

```bat
build_sm64jak.bat     :: guided full build: DLL (MSVC) -> copy -> SM64 (MinGW)
launch_sm64jak.bat    :: run it
```

Manual builds, PATH gotchas, and iteration workflows are covered in
[SM64_JAK_PROGRESS.md](SM64_JAK_PROGRESS.md) and [CLAUDE.md](CLAUDE.md).

## Controls

| Input | Action |
|---|---|
| Left stick / A / B | Move / jump / attack (Jak-mapped) |
| Hold `W` (keyboard) | **gk focus**: controller goes to the Jak world (menus); SM64 input paused |
| `Left Alt` | Toggle ImGui overlay (PiP toggle, level warp, FPS) |
| Hold X + D-pad Right | In-game Jak menu (fallback toggles, level warps) |
| While in gk focus: Start / d-pad / X / Triangle | Drive gk's pause & debug menus |

---

## Level status & TODO

Status legend: ✅ working &nbsp; 🟡 partial / rough &nbsp; ❓ untested &nbsp; ❌ broken

### Hub

| Level | Status | TODO / notes |
|---|---|---|
| Castle Grounds | ✅ | Primary test level. Moat swimming tuned (shallow-water dive works). Verify cannon-to-roof route (cannon = Mario fallback). |
| Castle Inside | 🟡 | Painting warps fixed (no more crash). TODO: verify all painting entries, basement water door, mirror room rendering with Jak mesh. |
| Castle Courtyard | ❓ | TODO: boo interactions with spin/punch damage mapping. |

### Main courses

| Level | Status | TODO / notes |
|---|---|---|
| Bob-omb Battlefield | 🟡 | General traversal good. TODO: chain chomp / bob-omb throw interactions, king bob-omb grab (throw currently shell-focused). |
| Whomp's Fortress | ❓ | TODO: verify thwomp/whomp squash damage, tower moving platforms ride cleanly. |
| Jolly Roger Bay | 🟡 | Swimming works but water is the touchiest system. TODO: deep-dive pressure, sunken ship interior water volumes, eel. |
| Cool, Cool Mountain | ❓ | TODO: slide physics (slippery-slope pass-through is first-pass), penguin carry (no carry mechanic yet — Mario fallback?). |
| Big Boo's Haunt | ❓ | TODO: boo damage from spin, merry-go-round area transition, vanish-cap section (fallback). |
| Hazy Maze Cave | ❓ | TODO: toxic gas damage hook, Dorrie ride (moving platform path), metal-cap water section. |
| Lethal Lava Land | ❓ | TODO: lava damage/bounce (SM64 burn vs Jak damage), sinking platforms, volcano interior. |
| Shifting Sand Land | ❓ | TODO: quicksand kill plane vs Jak death handling, tornado, pyramid interior transitions. |
| Dire, Dire Docks | 🟡 | Swimming works. TODO: current/whirlpool forces on Jak, submarine area, water-level changes between areas. |
| Snowman's Land | ❓ | TODO: blizzard wind forces, ice physics (slippery), igloo crawl space (no crawl — fallback?). |
| Wet-Dry World | ❓ | High risk: dynamic water level changes per switch — per-area water reset logic needs verification here. |
| Tall, Tall Mountain | ❓ | TODO: mushroom platform edges, monkey, slide section. |
| Tiny-Huge Island | ❓ | High risk: level rescale on pipe warp — verify collision filter AABB and Jak scale after transitions. |
| Tick Tock Clock | 🟡 | Moving platforms ride correctly in general. TODO: clock-speed variants, pendulums as damage sources. |
| Rainbow Ride | 🟡 | Carpet = moving platform (works). TODO: wind gusts, long fall recovery, flying section (wing cap fallback). |

### Bowser stages

| Level | Status | TODO / notes |
|---|---|---|
| Bowser in the Dark World | ❓ | TODO: full traversal test, elevator platforms. |
| Bowser in the Fire Sea | ❓ | TODO: sinking cage platforms, lava damage, pole sections (fallback). |
| Bowser in the Sky | ❓ | TODO: full traversal test. |
| Bowser 1 Fight | 🟡 | Warp works. Bowser throw needs the grab mechanic — currently Mario-fallback territory. TODO: dedicated Jak grab/throw. |
| Bowser 2 Fight | ❓ | Same throw question as Bowser 1. |
| Bowser 3 Fight | ❓ | Same, plus tilting platform ride. |

### Secret & cap levels

| Level | Status | TODO / notes |
|---|---|---|
| Princess's Secret Slide | ❓ | Slide physics stress test — slippery-slope handling is first-pass. |
| Secret Aquarium | ❓ | Pure swimming level — good swim-system stress test (no floor!). |
| Cavern of the Metal Cap | ❓ | Metal cap = no Jak equivalent yet (fallback). Underground river current. |
| Tower of the Wing Cap | ❓ | Wing cap = Mario fallback by design for now. |
| Vanish Cap under the Moat | ❓ | Vanish cap fallback; slide section. |
| Wing Mario over the Rainbow | ❓ | Wing cap fallback; cloud platforms. |

### Cross-cutting TODOs

- [ ] Carry/grab mechanic for Jak (penguins, king bob-omb, Bowser)
- [ ] Cap power equivalents (wing/metal/vanish) or polished fallback handoffs
- [ ] Slide physics second pass (CCM, PSS, TTM)
- [ ] Dynamic water level changes (WDW) and currents (DDD, COTMC)
- [ ] Eye textures polish; remaining Mario-fallback duality cleanup
- [ ] Per-level playthrough pass to upgrade every ❓ above

---

## Credits & legal

- [OpenGOAL](https://github.com/open-goal/jak-project) (ISC) — the Jak & Daxter
  reverse-engineering project this fork is built on
- [sm64ex](https://github.com/sm64pc/sm64ex) — SM64 PC port
- Jak and Daxter™ is property of Sony Interactive Entertainment / Naughty Dog;
  Super Mario 64 is property of Nintendo. This project contains no game assets —
  you must supply your own legally obtained copies of both games.
