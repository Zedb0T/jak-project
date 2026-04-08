#include "mod_params.h"

#include "game/mods/mod_manager.h"

#include "game/graphics/gfx.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/common/kscheme.h"
#include "game/runtime.h"

#include "common/log/log.h"
#include "common/symbols.h"

namespace mods {

namespace {

// Byte offsets of fields within the GOAL `surface` structure, relative to the GOAL pointer.
//
// GOAL `basic` pointers point PAST the 4-byte type tag (BASIC_OFFSET = 4).
// The deftype field offsets are absolute from the object start (including type tag),
// so we subtract BASIC_OFFSET to get offsets relative to the GOAL pointer.
//
// deftype layout:    offset 0 = type tag, 4 = name, 8 = turnv, 12 = turnvv, ...
// GOAL pointer at:   object_start + 4  (skips type tag)
// C++ access:        g_ee_main_mem + goal_ptr + (deftype_offset - BASIC_OFFSET)
//
constexpr int BASIC_OFF = 4;
constexpr int SURFACE_OFFSET_TURNV = 8 - BASIC_OFF;             // deftype offset 8
constexpr int SURFACE_OFFSET_TURNVV = 12 - BASIC_OFF;           // deftype offset 12
constexpr int SURFACE_OFFSET_TRANSV_MAX = 24 - BASIC_OFF;       // deftype offset 24
constexpr int SURFACE_OFFSET_TARGET_SPEED = 28 - BASIC_OFF;     // deftype offset 28
constexpr int SURFACE_OFFSET_SEEK0 = 32 - BASIC_OFF;            // deftype offset 32
constexpr int SURFACE_OFFSET_SEEK90 = 36 - BASIC_OFF;           // deftype offset 36
constexpr int SURFACE_OFFSET_SEEK180 = 40 - BASIC_OFF;          // deftype offset 40
constexpr int SURFACE_OFFSET_FRIC = 44 - BASIC_OFF;             // deftype offset 44
constexpr int SURFACE_OFFSET_NONLIN_FRIC_DIST = 48 - BASIC_OFF; // deftype offset 48
constexpr int SURFACE_OFFSET_SLIP_FACTOR = 52 - BASIC_OFF;      // deftype offset 52
constexpr int SURFACE_OFFSET_SLOPE_SLIP_ANGLE = 68 - BASIC_OFF; // deftype offset 68

/// Original values for a single surface (for reverting).
struct SurfaceOriginals {
  float fric = 0;
  float nonlin_fric_dist = 0;
  float slip_factor = 0;
  float seek0 = 0;
  float seek90 = 0;
  float seek180 = 0;
  float turnv = 0;
  float slope_slip_angle = 0;
  float transv_max = 0;
  float target_speed = 0;
  bool saved = false;
};

/// Info about one resolved surface symbol.
struct SurfaceEntry {
  const char* symbol_name;
  u32 offset = 0;  // EE memory offset of the surface struct
  SurfaceOriginals originals;
};

/// All tracked surface symbols.
struct SurfaceSymbolCache {
  static constexpr int NUM_SURFACES = 5;
  SurfaceEntry surfaces[NUM_SURFACES] = {
      {"*stone-surface*"},
      {"*grass-surface*"},
      {"*edge-surface*"},
      {"*wade-surface*"},
      {"*tread-surface*"},
  };
  bool resolved = false;
};

SurfaceSymbolCache g_surface_cache;

/// Read the value of a GOAL symbol from EE memory given the symbol's offset.
/// For Jak1, Symbol is a struct with u32 value at offset 0.
/// For Jak2/3/X, Symbol4<u32> stores the value 1 byte before the symbol offset.
u32 read_symbol_value(u32 sym_offset) {
  if (sym_offset == 0 || !g_ee_main_mem) {
    return 0;
  }
  if (g_game_version == GameVersion::Jak1) {
    // Jak1 Symbol: u32 value at offset 0
    return *(u32*)(g_ee_main_mem + sym_offset);
  } else {
    // Jak2/3/X Symbol4<u32>: value is at (sym_offset - 1)
    return *(u32*)(g_ee_main_mem + sym_offset - 1);
  }
}

/// Write a value into a GOAL symbol's value field.
void write_symbol_value(u32 sym_offset, u32 value) {
  if (sym_offset == 0 || !g_ee_main_mem) {
    return;
  }
  if (g_game_version == GameVersion::Jak1) {
    *(u32*)(g_ee_main_mem + sym_offset) = value;
  } else {
    *(u32*)(g_ee_main_mem + sym_offset - 1) = value;
  }
}

/// Write a GOAL boolean (#t or #f) into a symbol.
void write_goal_bool(u32 sym_offset, bool val) {
  if (val) {
    write_symbol_value(sym_offset, s7.offset + true_symbol_offset(g_game_version));
  } else {
    write_symbol_value(sym_offset, s7.offset);
  }
}

/// Get the symbol table offset (location of the symbol itself, NOT its value).
/// Returns 0 if not available.
u32 resolve_symbol_offset(const char* name) {
  if (!g_ee_main_mem || !g_pc_port_funcs.intern_from_c) {
    return 0;
  }
  try {
    auto info = g_pc_port_funcs.intern_from_c(name);
    return info.offset;
  } catch (...) {
    return 0;
  }
}

/// Attempt to resolve a GOAL symbol and return its value (a pointer into EE memory).
/// Returns 0 if not yet available or if the symbol is uninitialized (#f).
u32 resolve_goal_symbol(const char* name) {
  if (!g_ee_main_mem || !g_pc_port_funcs.intern_from_c) {
    return 0;
  }
  try {
    auto info = g_pc_port_funcs.intern_from_c(name);
    if (info.offset == 0) {
      return 0;
    }
    // The intern_from_c wrapper only sets info.offset (the symbol's location),
    // NOT info.value. We must read the symbol's value from EE memory ourselves.
    u32 val = read_symbol_value(info.offset);
    // In GOAL, s7.offset is the value of #f (false/uninitialized).
    // If the symbol hasn't been assigned yet, its value will be s7.offset.
    if (val == 0 || val == s7.offset) {
      return 0;
    }
    return val;
  } catch (...) {
    return 0;
  }
}

float read_surface_float(u32 surface_offset, int field_offset) {
  if (surface_offset == 0 || !g_ee_main_mem) {
    return 0.0f;
  }
  return *(float*)(g_ee_main_mem + surface_offset + field_offset);
}

void write_surface_float(u32 surface_offset, int field_offset, float value) {
  if (surface_offset == 0 || !g_ee_main_mem) {
    return;
  }
  *(float*)(g_ee_main_mem + surface_offset + field_offset) = value;
}

bool resolve_surface_symbols() {
  if (g_surface_cache.resolved) {
    return true;
  }

  // The first surface (*stone-surface*) is required. The rest are optional.
  g_surface_cache.surfaces[0].offset = resolve_goal_symbol(g_surface_cache.surfaces[0].symbol_name);
  if (g_surface_cache.surfaces[0].offset == 0) {
    // GOAL runtime isn't up yet or symbols not loaded — retry next frame.
    return false;
  }

  for (int i = 1; i < SurfaceSymbolCache::NUM_SURFACES; i++) {
    g_surface_cache.surfaces[i].offset =
        resolve_goal_symbol(g_surface_cache.surfaces[i].symbol_name);
  }

  g_surface_cache.resolved = true;

  // Log resolved addresses and sanity-check by reading a known field.
  // *stone-surface* has fric = 153600.0 when unmodified.
  u32 stone = g_surface_cache.surfaces[0].offset;
  float stone_fric = read_surface_float(stone, SURFACE_OFFSET_FRIC);
  lg::info("[ModParams] Resolved GOAL surface symbols (stone=0x{:x}, fric={:.1f})",
           stone, stone_fric);
  return true;
}

/// Save original values of a single surface for later reverting.
void save_surface_originals(SurfaceEntry& entry) {
  if (entry.originals.saved || entry.offset == 0) {
    return;
  }
  u32 s = entry.offset;
  entry.originals.fric = read_surface_float(s, SURFACE_OFFSET_FRIC);
  entry.originals.nonlin_fric_dist = read_surface_float(s, SURFACE_OFFSET_NONLIN_FRIC_DIST);
  entry.originals.slip_factor = read_surface_float(s, SURFACE_OFFSET_SLIP_FACTOR);
  entry.originals.seek0 = read_surface_float(s, SURFACE_OFFSET_SEEK0);
  entry.originals.seek90 = read_surface_float(s, SURFACE_OFFSET_SEEK90);
  entry.originals.seek180 = read_surface_float(s, SURFACE_OFFSET_SEEK180);
  entry.originals.turnv = read_surface_float(s, SURFACE_OFFSET_TURNV);
  entry.originals.slope_slip_angle = read_surface_float(s, SURFACE_OFFSET_SLOPE_SLIP_ANGLE);
  entry.originals.transv_max = read_surface_float(s, SURFACE_OFFSET_TRANSV_MAX);
  entry.originals.target_speed = read_surface_float(s, SURFACE_OFFSET_TARGET_SPEED);
  entry.originals.saved = true;
}

/// Apply ice-like friction values to a single surface structure.
void apply_ice_to_surface(u32 surface_offset) {
  if (surface_offset == 0) {
    return;
  }
  // Ice values from *ice-surface* in surface-h.gc:
  // fric: 23756.8 (vs stone's 153600.0 — about 6.5x lower)
  // nonlin-fric-dist: 4091904.0 (vs stone's 5120.0 — huge, makes friction very nonlinear)
  // slip-factor: 0.7 (reduced grip)
  // seek values: 24576.0 (vs stone's 153600.0 — much slower acceleration)
  // turnv: 0.5 (half turning speed)
  // slope-slip-angle: 16384.0 (slides on slopes more easily)
  // transv-max: 1.5 (can slide faster than normal)
  // target-speed: 1.5
  write_surface_float(surface_offset, SURFACE_OFFSET_FRIC, 23756.8f);
  write_surface_float(surface_offset, SURFACE_OFFSET_NONLIN_FRIC_DIST, 4091904.0f);
  write_surface_float(surface_offset, SURFACE_OFFSET_SLIP_FACTOR, 0.7f);
  write_surface_float(surface_offset, SURFACE_OFFSET_SEEK0, 24576.0f);
  write_surface_float(surface_offset, SURFACE_OFFSET_SEEK90, 24576.0f);
  write_surface_float(surface_offset, SURFACE_OFFSET_SEEK180, 24576.0f);
  write_surface_float(surface_offset, SURFACE_OFFSET_TURNV, 0.5f);
  write_surface_float(surface_offset, SURFACE_OFFSET_SLOPE_SLIP_ANGLE, 16384.0f);
  write_surface_float(surface_offset, SURFACE_OFFSET_TRANSV_MAX, 1.5f);
  write_surface_float(surface_offset, SURFACE_OFFSET_TARGET_SPEED, 1.5f);
}

/// Revert a surface back to its saved original values.
void revert_surface(SurfaceEntry& entry) {
  if (entry.offset == 0 || !entry.originals.saved) {
    return;
  }
  u32 s = entry.offset;
  write_surface_float(s, SURFACE_OFFSET_FRIC, entry.originals.fric);
  write_surface_float(s, SURFACE_OFFSET_NONLIN_FRIC_DIST, entry.originals.nonlin_fric_dist);
  write_surface_float(s, SURFACE_OFFSET_SLIP_FACTOR, entry.originals.slip_factor);
  write_surface_float(s, SURFACE_OFFSET_SEEK0, entry.originals.seek0);
  write_surface_float(s, SURFACE_OFFSET_SEEK90, entry.originals.seek90);
  write_surface_float(s, SURFACE_OFFSET_SEEK180, entry.originals.seek180);
  write_surface_float(s, SURFACE_OFFSET_TURNV, entry.originals.turnv);
  write_surface_float(s, SURFACE_OFFSET_SLOPE_SLIP_ANGLE, entry.originals.slope_slip_angle);
  write_surface_float(s, SURFACE_OFFSET_TRANSV_MAX, entry.originals.transv_max);
  write_surface_float(s, SURFACE_OFFSET_TARGET_SPEED, entry.originals.target_speed);
}

}  // namespace

void register_default_params(GameVersion /*version*/) {
  auto& mgr = ModManager::get();

  // --- Rendering Parameters ---

  mgr.register_param("render.target_fps", 60.0f, [](const ModValue& val) {
    if (auto* f = std::get_if<float>(&val)) {
      Gfx::g_global_settings.target_fps = *f;
    } else if (auto* i = std::get_if<int>(&val)) {
      Gfx::g_global_settings.target_fps = static_cast<float>(*i);
    }
  });

  mgr.register_param("render.framelimiter", true, [](const ModValue& val) {
    if (auto* b = std::get_if<bool>(&val)) {
      Gfx::g_global_settings.framelimiter = *b;
    }
  });

  mgr.register_param("render.vsync", true, [](const ModValue& val) {
    if (auto* b = std::get_if<bool>(&val)) {
      Gfx::g_global_settings.vsync = *b;
    }
  });

  mgr.register_param("render.msaa_samples", 1, [](const ModValue& val) {
    if (auto* i = std::get_if<int>(&val)) {
      Gfx::g_global_settings.msaa_samples = *i;
    }
  });

  mgr.register_param("render.no_textures", false, [](const ModValue& val) {
    if (auto* b = std::get_if<bool>(&val)) {
      Gfx::g_global_settings.hack_no_tex = *b;
    }
  });

  mgr.register_param("render.lod_tfrag", 0, [](const ModValue& val) {
    if (auto* i = std::get_if<int>(&val)) {
      Gfx::g_global_settings.lod_tfrag = *i;
    }
  });

  mgr.register_param("render.lod_tie", 0, [](const ModValue& val) {
    if (auto* i = std::get_if<int>(&val)) {
      Gfx::g_global_settings.lod_tie = *i;
    }
  });

  // --- Collision Debug Parameters ---
  // GOAL code in pckernel-common.gc overwrites the C++ collision flags every frame
  // from `*collision-renderer*` and `*collision-wireframe*` GOAL symbols.
  // We must write both the C++ flag AND the GOAL symbol to make mods work.

  mgr.register_param("debug.collision_enable", false, [](const ModValue& val) {
    if (auto* b = std::get_if<bool>(&val)) {
      Gfx::g_global_settings.collision_enable = *b;
      // also set the GOAL-side symbol so pckernel doesn't overwrite us
      u32 sym = resolve_symbol_offset("*collision-renderer*");
      if (sym != 0) {
        write_goal_bool(sym, *b);
      }
    }
  });

  mgr.register_param("debug.collision_wireframe", true, [](const ModValue& val) {
    if (auto* b = std::get_if<bool>(&val)) {
      Gfx::g_global_settings.collision_wireframe = *b;
      u32 sym = resolve_symbol_offset("*collision-wireframe*");
      if (sym != 0) {
        write_goal_bool(sym, *b);
      }
    }
  });

  // --- Display Parameters ---

  mgr.register_param("render.brightness", 0, [](const ModValue& val) {
    if (auto* i = std::get_if<int>(&val)) {
      Gfx::g_global_settings.brightness_contrast_color = *i;
    }
  });

  mgr.register_param("render.contrast", 128, [](const ModValue& val) {
    if (auto* i = std::get_if<int>(&val)) {
      Gfx::g_global_settings.brightness_contrast_alpha = *i;
    }
  });

  // --- Physics Parameters (write to GOAL memory) ---

  mgr.register_param("physics.ice_surfaces", false, [](const ModValue& val) {
    auto* b = std::get_if<bool>(&val);
    if (!b) {
      return;
    }

    // defer symbol resolution until GOAL runtime is up
    if (!resolve_surface_symbols()) {
      return;
    }

    if (*b) {
      // save each surface's original values, then apply ice physics
      for (int i = 0; i < SurfaceSymbolCache::NUM_SURFACES; i++) {
        auto& entry = g_surface_cache.surfaces[i];
        save_surface_originals(entry);
        apply_ice_to_surface(entry.offset);
      }
    } else {
      // revert each surface to its own saved original values
      for (int i = 0; i < SurfaceSymbolCache::NUM_SURFACES; i++) {
        revert_surface(g_surface_cache.surfaces[i]);
      }
    }
  });

  lg::info("[ModManager] Registered {} default parameters", mgr.get_param_names().size());
}

}  // namespace mods
