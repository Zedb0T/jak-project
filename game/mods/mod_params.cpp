#include "mod_params.h"

#include "game/mods/mod_manager.h"

#include "game/graphics/gfx.h"

#include "common/log/log.h"

namespace mods {

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

  mgr.register_param("debug.collision_enable", false, [](const ModValue& val) {
    if (auto* b = std::get_if<bool>(&val)) {
      Gfx::g_global_settings.collision_enable = *b;
    }
  });

  mgr.register_param("debug.collision_wireframe", true, [](const ModValue& val) {
    if (auto* b = std::get_if<bool>(&val)) {
      Gfx::g_global_settings.collision_wireframe = *b;
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

  lg::info("[ModManager] Registered {} default parameters", mgr.get_param_names().size());
}

}  // namespace mods
