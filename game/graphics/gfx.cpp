/*!
 * @file gfx.cpp
 * Graphics component for the runtime. Abstraction layer for the main graphics routines.
 */

#include "gfx.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

#include "display.h"

#include "common/global_profiler/GlobalProfiler.h"
#include "common/log/log.h"
#include "common/symbols.h"
#include "common/util/FileUtil.h"
#include "common/util/json_util.h"

#include "game/common/file_paths.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/common/kscheme.h"
#include "game/runtime.h"
#include "pipelines/opengl.h"

namespace Gfx {

std::function<void()> vsync_callback;
GfxGlobalSettings g_global_settings;
game_settings::DebugSettings g_debug_settings;

// libjakopengoal world-view capture (see gfx.h)
bool g_lib_hidden_display = false;

namespace {
std::mutex g_world_frame_mutex;
std::vector<u8> g_world_frame_pixels;
int g_world_frame_w = 0;
int g_world_frame_h = 0;
s32 g_world_frame_counter = 0;
}  // namespace

bool lib_display_live() {
  return GetCurrentRenderer() != nullptr && Display::GetMainDisplay() != nullptr;
}

void lib_store_world_frame(const u8* rgba, int w, int h) {
  std::lock_guard<std::mutex> lock(g_world_frame_mutex);
  g_world_frame_pixels.assign(rgba, rgba + (size_t)w * h * 4);
  g_world_frame_w = w;
  g_world_frame_h = h;
  g_world_frame_counter++;
}

s32 lib_get_world_frame(u8* dst, s32 max_bytes, s32* out_w, s32* out_h) {
  std::lock_guard<std::mutex> lock(g_world_frame_mutex);
  if (out_w) {
    *out_w = g_world_frame_w;
  }
  if (out_h) {
    *out_h = g_world_frame_h;
  }
  if (g_world_frame_counter == 0) {
    return 0;
  }
  if ((s32)g_world_frame_pixels.size() > max_bytes) {
    return -1;
  }
  memcpy(dst, g_world_frame_pixels.data(), g_world_frame_pixels.size());
  return g_world_frame_counter;
}

const GfxRendererModule* GetRenderer(GfxPipeline pipeline) {
  switch (pipeline) {
    case GfxPipeline::Invalid:
      lg::error("Requested invalid renderer", fmt::underlying(pipeline));
      return NULL;
    case GfxPipeline::OpenGL:
      return &gRendererOpenGL;
    default:
      lg::error("Requested unknown renderer {}", fmt::underlying(pipeline));
      return NULL;
  }
}

void SetRenderer(GfxPipeline pipeline) {
  g_global_settings.renderer = GetRenderer(pipeline);
}

const GfxRendererModule* GetCurrentRenderer() {
  return g_global_settings.renderer;
}

u32 Init(GameVersion version) {
  lg::info("GFX Init");
  prof().instant_event("ROOT");

  g_debug_settings = game_settings::DebugSettings();
  g_debug_settings.load_settings();
  {
    auto p = scoped_prof("startup::gfx::get_renderer");
    g_global_settings.renderer = GetRenderer(GfxPipeline::OpenGL);
  }

  {
    auto p = scoped_prof("startup::gfx::init_current_renderer");
    if (GetCurrentRenderer()->init(g_global_settings)) {
      lg::error("Gfx::Init error");
      return 1;
    }
  }

  if (g_main_thread_id != std::this_thread::get_id()) {
    lg::error("Ran Gfx::Init outside main thread. Init display elsewhere?");
  } else {
    {
      auto p = scoped_prof("startup::gfx::init_main_display");
      std::string title = "OpenGOAL";
      if (g_game_version == GameVersion::Jak2 || g_game_version == GameVersion::Jak3 ||
          g_game_version == GameVersion::JakX) {
        title += " - Work in Progress";
      }
      title += fmt::format(" - {} - {}", version_to_game_name_external(g_game_version),
                           build_revision());
      Display::InitMainDisplay(640, 480, title.c_str(), g_global_settings, version);
    }
  }

  return 0;
}

void Loop(std::function<bool()> f) {
  lg::info("GFX Loop");
  while (f()) {
    auto p = scoped_prof("gfx loop");
    // check if we have a display
    if (Display::GetMainDisplay()) {
      Display::GetMainDisplay()->render();
    }
  }
}

u32 Exit() {
  lg::info("GFX Exit");
  Display::KillMainDisplay();
  GetCurrentRenderer()->exit();
  g_debug_settings.save_settings();
  return 0;
}

void register_vsync_callback(std::function<void()> f) {
  vsync_callback = std::move(f);
}

void clear_vsync_callback() {
  vsync_callback = nullptr;
}

u32 vsync() {
  if (GetCurrentRenderer()) {
    // Inform the IOP kernel that we're vsyncing so it can run the vblank handler
    if (vsync_callback != nullptr)
      vsync_callback();
    return GetCurrentRenderer()->vsync();
  }
  return 0;
}

u32 sync_path() {
  if (GetCurrentRenderer()) {
    return GetCurrentRenderer()->sync_path();
  }
  return 0;
}

bool CollisionRendererGetMask(GfxGlobalSettings::CollisionRendererMode mode, s64 mask_id) {
  int arr_idx = mask_id / 32;
  int arr_ofs = mask_id % 32;

  switch (mode) {
    case GfxGlobalSettings::CollisionRendererMode::Mode:
      return (g_global_settings.collision_mode_mask[arr_idx] >> arr_ofs) & 1;
    case GfxGlobalSettings::CollisionRendererMode::Event:
      return (g_global_settings.collision_event_mask[arr_idx] >> arr_ofs) & 1;
    case GfxGlobalSettings::CollisionRendererMode::Material:
      return (g_global_settings.collision_material_mask[arr_idx] >> arr_ofs) & 1;
    case GfxGlobalSettings::CollisionRendererMode::Skip:
      if (mask_id == -1) {
        return g_global_settings.collision_skip_nomask_allowed;
      } else {
        return g_global_settings.collision_skip_mask & mask_id;
      }
    case GfxGlobalSettings::CollisionRendererMode::SkipHide:
      return g_global_settings.collision_skip_hide_mask & mask_id;
    default:
      lg::error("{} invalid params {} {}", __PRETTY_FUNCTION__, fmt::underlying(mode), mask_id);
      return false;
  }
}

void CollisionRendererSetMask(GfxGlobalSettings::CollisionRendererMode mode, s64 mask_id) {
  int arr_idx = mask_id / 32;
  int arr_ofs = mask_id % 32;

  switch (mode) {
    case GfxGlobalSettings::CollisionRendererMode::Mode:
      g_global_settings.collision_mode_mask[arr_idx] |= 1 << arr_ofs;
      break;
    case GfxGlobalSettings::CollisionRendererMode::Event:
      g_global_settings.collision_event_mask[arr_idx] |= 1 << arr_ofs;
      break;
    case GfxGlobalSettings::CollisionRendererMode::Material:
      g_global_settings.collision_material_mask[arr_idx] |= 1 << arr_ofs;
      break;
    case GfxGlobalSettings::CollisionRendererMode::Skip:
      if (mask_id == -1) {
        g_global_settings.collision_skip_nomask_allowed = true;
      } else {
        g_global_settings.collision_skip_mask |= mask_id;
      }
      break;
    case GfxGlobalSettings::CollisionRendererMode::SkipHide:
      g_global_settings.collision_skip_hide_mask |= mask_id;
      break;
    default:
      lg::error("{} invalid params {} {}", __PRETTY_FUNCTION__, fmt::underlying(mode), mask_id);
      break;
  }
}

void CollisionRendererClearMask(GfxGlobalSettings::CollisionRendererMode mode, s64 mask_id) {
  int arr_idx = mask_id / 32;
  int arr_ofs = mask_id % 32;

  switch (mode) {
    case GfxGlobalSettings::CollisionRendererMode::Mode:
      g_global_settings.collision_mode_mask[arr_idx] &= ~(1 << arr_ofs);
      break;
    case GfxGlobalSettings::CollisionRendererMode::Event:
      g_global_settings.collision_event_mask[arr_idx] &= ~(1 << arr_ofs);
      break;
    case GfxGlobalSettings::CollisionRendererMode::Material:
      g_global_settings.collision_material_mask[arr_idx] &= ~(1 << arr_ofs);
      break;
    case GfxGlobalSettings::CollisionRendererMode::Skip:
      if (mask_id == -1) {
        g_global_settings.collision_skip_nomask_allowed = false;
      } else {
        g_global_settings.collision_skip_mask &= ~mask_id;
      }
      break;
    case GfxGlobalSettings::CollisionRendererMode::SkipHide:
      g_global_settings.collision_skip_hide_mask &= ~mask_id;
      break;
    default:
      lg::error("{} invalid params {} {}", __PRETTY_FUNCTION__, fmt::underlying(mode), mask_id);
      break;
  }
}

void CollisionRendererSetMode(GfxGlobalSettings::CollisionRendererMode mode) {
  g_global_settings.collision_mode = mode;
}

}  // namespace Gfx
