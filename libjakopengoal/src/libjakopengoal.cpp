/**
 * libjakopengoal.cpp - Main implementation of the libjakopengoal C API.
 *
 * This boots the GOAL runtime in a background thread set and exposes
 * Jak's state, collision, and rendering data through the C API.
 */

#include "libjakopengoal/include/libjakopengoal.h"
#include "jak_bridge.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/versions/versions.h"

#include "game/common/game_common_types.h"
#include "game/kernel/common/kboot.h"
#include "game/runtime.h"

/* -------------------------------------------------------------------------- */
/*  Internal state                                                            */
/* -------------------------------------------------------------------------- */

static std::atomic<bool> s_initialized{false};
static std::thread s_runtime_thread;
static std::string s_game_data_path;
static uint8_t* s_texture_output = nullptr;

static JakDebugPrintFunction s_debug_print_fn = nullptr;
static JakPlaySoundFunction s_play_sound_fn = nullptr;

// Simple Jak instance tracking (for now, only one instance supported)
static std::atomic<int32_t> s_jak_instance_id{-1};
static float s_jak_spawn_pos[3] = {0, 0, 0};

/* -------------------------------------------------------------------------- */
/*  Runtime thread                                                            */
/* -------------------------------------------------------------------------- */

static void runtime_thread_func() {
  // Set up the game data path so the runtime can find iso_data/ and out/
  if (!s_game_data_path.empty()) {
    // The jak-project uses file_util to locate game data.
    // We set the project path to point to our data directory.
    fs::path proj_path(s_game_data_path);
    file_util::setup_project_path(proj_path);
  }

  // Configure the game launch options for headless jak1
  GameLaunchOptions options;
  options.game_version = GameVersion::Jak1;
  options.disable_display = true;  // No window - we extract mesh data via CPU

  // Build argv for the GOAL runtime
  // -boot -debug: boots game in debug mode (skips title, spawns Jak in village1)
  // -fakeiso: use extracted game data instead of ISO
  // -nosound: disable audio system
  // -lib-jak: our custom flag to enable the bridge
  const char* argv[] = {"libjakopengoal", "-boot", "-fakeiso", "-debug", "-nosound", "-lib-jak"};
  int argc = 6;

  lg::info("[libjakopengoal] Starting GOAL runtime...");
  RuntimeExitStatus status = exec_runtime(options, argc, argv);
  lg::info("[libjakopengoal] GOAL runtime exited with status {}", (int)status);
}

/* -------------------------------------------------------------------------- */
/*  Public API: Lifecycle                                                     */
/* -------------------------------------------------------------------------- */

extern "C" {

JAK_LIB_FN int32_t jak_global_init(const char* game_data_path, uint8_t* out_texture) {
  if (s_initialized.load()) {
    lg::warn("[libjakopengoal] Already initialized!");
    return -1;
  }

  lg::info("[libjakopengoal] Initializing with data path: {}", game_data_path);

  s_game_data_path = game_data_path ? game_data_path : "";
  s_texture_output = out_texture;

  // Initialize logging
  lg::set_file_level(lg::level::info);
  lg::set_stdout_level(lg::level::info);
  lg::set_flush_level(lg::level::info);
  lg::initialize();

  // Launch the GOAL runtime in a background thread
  s_runtime_thread = std::thread(runtime_thread_func);

  s_initialized.store(true);
  return 0;
}

JAK_LIB_FN void jak_global_terminate(void) {
  if (!s_initialized.load()) {
    return;
  }

  lg::info("[libjakopengoal] Terminating...");

  // Signal the runtime to exit
  MasterExit = RuntimeExitStatus::EXIT;

  // Wait for the runtime thread to finish
  if (s_runtime_thread.joinable()) {
    s_runtime_thread.join();
  }

  jak_bridge::set_runtime_ready(false);
  s_initialized.store(false);
  s_jak_instance_id.store(-1);

  lg::info("[libjakopengoal] Terminated.");
}

JAK_LIB_FN bool jak_is_ready(void) {
  return jak_bridge::is_runtime_ready();
}

/* -------------------------------------------------------------------------- */
/*  Public API: Callbacks                                                     */
/* -------------------------------------------------------------------------- */

JAK_LIB_FN void jak_register_debug_print(JakDebugPrintFunction fn) {
  s_debug_print_fn = fn;
}

JAK_LIB_FN void jak_register_play_sound(JakPlaySoundFunction fn) {
  s_play_sound_fn = fn;
}

/* -------------------------------------------------------------------------- */
/*  Public API: Static collision                                              */
/* -------------------------------------------------------------------------- */

JAK_LIB_FN void jak_static_surfaces_load(const struct JakSurface* surfaces, uint32_t count) {
  auto& coll = jak_bridge::get_collision_state();
  std::lock_guard<std::mutex> lock(coll.mutex);

  coll.static_surfaces.clear();
  coll.static_surfaces.reserve(count);

  for (uint32_t i = 0; i < count; i++) {
    jak_bridge::ExternalSurface ext;
    ext.type = surfaces[i].type;
    ext.flags = surfaces[i].flags;
    memcpy(ext.vertices, surfaces[i].vertices, sizeof(float) * 9);

    // Auto-compute normal if not provided
    if (surfaces[i].normal[0] == 0.0f && surfaces[i].normal[1] == 0.0f &&
        surfaces[i].normal[2] == 0.0f) {
      // Compute face normal from vertices
      float e1[3], e2[3];
      for (int j = 0; j < 3; j++) {
        e1[j] = surfaces[i].vertices[1][j] - surfaces[i].vertices[0][j];
        e2[j] = surfaces[i].vertices[2][j] - surfaces[i].vertices[0][j];
      }
      ext.normal[0] = e1[1] * e2[2] - e1[2] * e2[1];
      ext.normal[1] = e1[2] * e2[0] - e1[0] * e2[2];
      ext.normal[2] = e1[0] * e2[1] - e1[1] * e2[0];
      float len = std::sqrt(ext.normal[0] * ext.normal[0] + ext.normal[1] * ext.normal[1] +
                             ext.normal[2] * ext.normal[2]);
      if (len > 0.0001f) {
        ext.normal[0] /= len;
        ext.normal[1] /= len;
        ext.normal[2] /= len;
      }
    } else {
      memcpy(ext.normal, surfaces[i].normal, sizeof(float) * 3);
    }

    coll.static_surfaces.push_back(ext);
  }

  coll.static_dirty = true;
  lg::info("[libjakopengoal] Loaded {} static collision surfaces", count);
}

JAK_LIB_FN void jak_static_surfaces_clear(void) {
  auto& coll = jak_bridge::get_collision_state();
  std::lock_guard<std::mutex> lock(coll.mutex);
  coll.static_surfaces.clear();
  coll.static_dirty = true;
}

/* -------------------------------------------------------------------------- */
/*  Public API: Dynamic surface objects                                       */
/* -------------------------------------------------------------------------- */

JAK_LIB_FN uint32_t jak_surface_object_create(const struct JakSurfaceObject* obj) {
  auto& coll = jak_bridge::get_collision_state();
  std::lock_guard<std::mutex> lock(coll.mutex);

  jak_bridge::DynamicSurfaceObject dyn;
  dyn.id = coll.next_dynamic_id++;
  dyn.transform = obj->transform;
  dyn.dirty = true;

  dyn.surfaces.reserve(obj->surface_count);
  for (uint32_t i = 0; i < obj->surface_count; i++) {
    jak_bridge::ExternalSurface ext;
    ext.type = obj->surfaces[i].type;
    ext.flags = obj->surfaces[i].flags;
    memcpy(ext.vertices, obj->surfaces[i].vertices, sizeof(float) * 9);
    memcpy(ext.normal, obj->surfaces[i].normal, sizeof(float) * 3);
    dyn.surfaces.push_back(ext);
  }

  coll.dynamic_objects.push_back(std::move(dyn));
  return coll.dynamic_objects.back().id;
}

JAK_LIB_FN void jak_surface_object_move(uint32_t obj_id, const struct JakObjectTransform* t) {
  auto& coll = jak_bridge::get_collision_state();
  std::lock_guard<std::mutex> lock(coll.mutex);

  for (auto& obj : coll.dynamic_objects) {
    if (obj.id == obj_id) {
      obj.transform = *t;
      obj.dirty = true;
      return;
    }
  }
}

JAK_LIB_FN void jak_surface_object_delete(uint32_t obj_id) {
  auto& coll = jak_bridge::get_collision_state();
  std::lock_guard<std::mutex> lock(coll.mutex);

  auto& objs = coll.dynamic_objects;
  objs.erase(std::remove_if(objs.begin(), objs.end(),
                              [obj_id](const jak_bridge::DynamicSurfaceObject& o) {
                                return o.id == obj_id;
                              }),
              objs.end());
}

/* -------------------------------------------------------------------------- */
/*  Public API: Jak instance                                                  */
/* -------------------------------------------------------------------------- */

JAK_LIB_FN int32_t jak_create(float x, float y, float z) {
  if (!jak_bridge::is_runtime_ready()) {
    lg::warn("[libjakopengoal] Runtime not ready, cannot create Jak");
    return -1;
  }

  if (s_jak_instance_id.load() >= 0) {
    lg::warn("[libjakopengoal] Jak instance already exists (id={})", s_jak_instance_id.load());
    return s_jak_instance_id.load();
  }

  s_jak_spawn_pos[0] = x;
  s_jak_spawn_pos[1] = y;
  s_jak_spawn_pos[2] = z;

  // Queue spawn command
  jak_bridge::Command cmd;
  cmd.type = jak_bridge::CommandType::SPAWN;
  cmd.jak_id = 0;
  cmd.f[0] = x;
  cmd.f[1] = y;
  cmd.f[2] = z;

  {
    auto& q = jak_bridge::get_command_queue();
    std::lock_guard<std::mutex> lock(q.mutex);
    q.pending.push_back(cmd);
  }

  s_jak_instance_id.store(0);
  return 0;
}

JAK_LIB_FN void jak_delete(int32_t jak_id) {
  if (jak_id < 0 || jak_id != s_jak_instance_id.load()) {
    return;
  }

  jak_bridge::Command cmd;
  cmd.type = jak_bridge::CommandType::DESTROY;
  cmd.jak_id = jak_id;

  {
    auto& q = jak_bridge::get_command_queue();
    std::lock_guard<std::mutex> lock(q.mutex);
    q.pending.push_back(cmd);
  }

  s_jak_instance_id.store(-1);
}

/* -------------------------------------------------------------------------- */
/*  Public API: Tick                                                          */
/* -------------------------------------------------------------------------- */

JAK_LIB_FN void jak_tick(int32_t jak_id,
                          const struct JakInputs* inputs,
                          struct JakState* state,
                          struct JakGeometryBuffers* geometry) {
  if (jak_id < 0 || !jak_bridge::is_runtime_ready()) {
    return;
  }

  // Inject inputs
  if (inputs) {
    auto& pad = jak_bridge::get_pad_state();
    std::lock_guard<std::mutex> lock(pad.mutex);
    pad.current_inputs = *inputs;
    pad.has_input = true;
  }

  // The GOAL runtime ticks on its own thread. We just read the latest state.
  // The bridge_tick() is called from the EE thread after each GOAL frame.

  // Copy output state
  if (state) {
    auto& jak = jak_bridge::get_jak_state();
    std::lock_guard<std::mutex> lock(jak.mutex);
    if (jak.valid) {
      *state = jak.state;
    } else {
      memset(state, 0, sizeof(JakState));
    }
  }

  // Copy output geometry
  if (geometry) {
    auto& mesh = jak_bridge::get_mesh_state();
    std::lock_guard<std::mutex> lock(mesh.mutex);
    if (mesh.valid && mesh.num_triangles > 0) {
      uint32_t num_tris =
          mesh.num_triangles > JAK_GEO_MAX_TRIANGLES ? JAK_GEO_MAX_TRIANGLES : mesh.num_triangles;
      uint32_t num_verts = num_tris * 3;

      geometry->num_triangles_used = (uint16_t)num_tris;

      if (geometry->position && mesh.positions.size() >= num_verts * 3) {
        memcpy(geometry->position, mesh.positions.data(), num_verts * 3 * sizeof(float));
      }
      if (geometry->normal && mesh.normals.size() >= num_verts * 3) {
        memcpy(geometry->normal, mesh.normals.data(), num_verts * 3 * sizeof(float));
      }
      if (geometry->color && mesh.colors.size() >= num_verts * 3) {
        memcpy(geometry->color, mesh.colors.data(), num_verts * 3 * sizeof(float));
      }
      if (geometry->uv && mesh.uvs.size() >= num_verts * 2) {
        memcpy(geometry->uv, mesh.uvs.data(), num_verts * 2 * sizeof(float));
      }
    } else {
      geometry->num_triangles_used = 0;
    }
  }
}

/* -------------------------------------------------------------------------- */
/*  Public API: State manipulation                                            */
/* -------------------------------------------------------------------------- */

static void queue_command(jak_bridge::CommandType type,
                          int32_t jak_id,
                          float f0 = 0,
                          float f1 = 0,
                          float f2 = 0,
                          float f3 = 0,
                          int32_t i0 = 0,
                          int32_t i1 = 0) {
  jak_bridge::Command cmd;
  cmd.type = type;
  cmd.jak_id = jak_id;
  cmd.f[0] = f0;
  cmd.f[1] = f1;
  cmd.f[2] = f2;
  cmd.f[3] = f3;
  cmd.i[0] = i0;
  cmd.i[1] = i1;

  auto& q = jak_bridge::get_command_queue();
  std::lock_guard<std::mutex> lock(q.mutex);
  q.pending.push_back(cmd);
}

JAK_LIB_FN void jak_set_position(int32_t jak_id, float x, float y, float z) {
  queue_command(jak_bridge::CommandType::SET_POSITION, jak_id, x, y, z);
}

JAK_LIB_FN void jak_set_velocity(int32_t jak_id, float vx, float vy, float vz) {
  queue_command(jak_bridge::CommandType::SET_VELOCITY, jak_id, vx, vy, vz);
}

JAK_LIB_FN void jak_set_angle(int32_t jak_id, float yaw) {
  queue_command(jak_bridge::CommandType::SET_ANGLE, jak_id, yaw);
}

JAK_LIB_FN void jak_set_action(int32_t jak_id, uint32_t action) {
  queue_command(jak_bridge::CommandType::SET_ACTION, jak_id, 0, 0, 0, 0, (int32_t)action);
}

JAK_LIB_FN void jak_set_health(int32_t jak_id, float hp) {
  queue_command(jak_bridge::CommandType::SET_HEALTH, jak_id, hp);
}

JAK_LIB_FN void jak_take_damage(int32_t jak_id, float from_x, float from_y, float from_z) {
  queue_command(jak_bridge::CommandType::TAKE_DAMAGE, jak_id, from_x, from_y, from_z);
}

JAK_LIB_FN void jak_heal(int32_t jak_id, float amount) {
  queue_command(jak_bridge::CommandType::HEAL, jak_id, amount);
}

JAK_LIB_FN void jak_kill(int32_t jak_id) {
  queue_command(jak_bridge::CommandType::KILL, jak_id);
}

JAK_LIB_FN void jak_give_eco(int32_t jak_id, int32_t eco_type) {
  queue_command(jak_bridge::CommandType::GIVE_ECO, jak_id, 0, 0, 0, 0, eco_type);
}

/* -------------------------------------------------------------------------- */
/*  Public API: Collision queries                                             */
/* -------------------------------------------------------------------------- */

JAK_LIB_FN float jak_find_floor_height(float x, float y, float z) {
  // This would query the GOAL collision system.
  // For now, we do a simple check against our loaded static surfaces.
  auto& coll = jak_bridge::get_collision_state();
  std::lock_guard<std::mutex> lock(coll.mutex);

  float best_y = -1000000.0f;

  for (const auto& surf : coll.static_surfaces) {
    // Simple point-in-triangle test (XZ projection) + Y interpolation
    const float* v0 = surf.vertices[0];
    const float* v1 = surf.vertices[1];
    const float* v2 = surf.vertices[2];

    // Barycentric coordinates in XZ plane
    float d00 = (v1[0] - v0[0]) * (v1[0] - v0[0]) + (v1[2] - v0[2]) * (v1[2] - v0[2]);
    float d01 = (v1[0] - v0[0]) * (v2[0] - v0[0]) + (v1[2] - v0[2]) * (v2[2] - v0[2]);
    float d11 = (v2[0] - v0[0]) * (v2[0] - v0[0]) + (v2[2] - v0[2]) * (v2[2] - v0[2]);
    float d20 = (x - v0[0]) * (v1[0] - v0[0]) + (z - v0[2]) * (v1[2] - v0[2]);
    float d21 = (x - v0[0]) * (v2[0] - v0[0]) + (z - v0[2]) * (v2[2] - v0[2]);

    float denom = d00 * d11 - d01 * d01;
    if (std::abs(denom) < 0.0001f)
      continue;

    float u = (d11 * d20 - d01 * d21) / denom;
    float v = (d00 * d21 - d01 * d20) / denom;

    if (u >= 0.0f && v >= 0.0f && (u + v) <= 1.0f) {
      // Point is inside triangle - interpolate Y
      float tri_y = v0[1] * (1.0f - u - v) + v1[1] * u + v2[1] * v;
      if (tri_y <= y && tri_y > best_y) {
        best_y = tri_y;
      }
    }
  }

  return best_y;
}

/* -------------------------------------------------------------------------- */
/*  Public API: Utility                                                       */
/* -------------------------------------------------------------------------- */

JAK_LIB_FN struct JakTextureInfo jak_get_texture_info(void) {
  JakTextureInfo info;
  info.width = JAK_TEXTURE_WIDTH;
  info.height = JAK_TEXTURE_HEIGHT;
  info.num_textures = 0;  // TODO: populate from loaded texture data
  return info;
}

}  // extern "C"
