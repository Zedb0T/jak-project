#include "sm64.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/common/kscheme.h"

// Define SM64_LIB_EXPORT so SM64_LIB_FN resolves to dllexport (harmless for static linking)
// rather than dllimport which would cause linker issues
#define SM64_LIB_EXPORT
extern "C" {
#include "libsm64.h"
}

// glad must be included after other GL headers
#include "third-party/glad/include/glad/glad.h"

namespace sm64_bridge {

// ============================================================================
// Internal state
// ============================================================================

static bool s_initialized = false;
static std::vector<uint8_t> s_rom_data;
static GLuint s_texture_id = 0;
static int s_mario_id = -1;

// Geometry buffers for sm64_mario_tick output.
// SM64_GEO_MAX_TRIANGLES = 1024
static float s_positions[SM64_GEO_MAX_TRIANGLES * 9];
static float s_normals[SM64_GEO_MAX_TRIANGLES * 9];
static float s_colors[SM64_GEO_MAX_TRIANGLES * 9];
static float s_uvs[SM64_GEO_MAX_TRIANGLES * 6];

// Double-buffered render data for thread safety.
static MarioRenderData s_render_buffers[2];
static std::atomic<int> s_read_buffer{0};
static std::mutex s_write_mutex;

// ============================================================================
// Coordinate conversion
// ============================================================================

static inline void og_to_sm64(float og_x, float og_y, float og_z,
                               int32_t& sm_x, int32_t& sm_y, int32_t& sm_z) {
  sm_x = (int32_t)(og_x * OG_TO_SM64_SCALE);
  sm_y = (int32_t)(og_y * OG_TO_SM64_SCALE);
  sm_z = (int32_t)(og_z * OG_TO_SM64_SCALE);
}

static inline void og_to_sm64_float(float og_x, float og_y, float og_z,
                                     float& sm_x, float& sm_y, float& sm_z) {
  sm_x = og_x * OG_TO_SM64_SCALE;
  sm_y = og_y * OG_TO_SM64_SCALE;
  sm_z = og_z * OG_TO_SM64_SCALE;
}

// ============================================================================
// Pat-surface to SM64 surface type conversion
// ============================================================================

static int16_t pat_to_sm64_surface(int16_t surface_type) {
  // Map OpenGOAL pat-surface events/materials to SM64 surface types.
  // 0x0000 = SURFACE_DEFAULT
  // 0x0001 = SURFACE_BURNING
  // 0x000A = SURFACE_DEATH_PLANE
  // 0x0013 = SURFACE_ICE
  // 0x0014 = SURFACE_HARD (not slippery)
  // 0x0065 = SURFACE_WALL_MISC
  switch (surface_type) {
    case 1:  // deadly
    case 6:  // melt
      return 0x0001;  // SURFACE_BURNING
    case 2:  // endlessfall
      return 0x000A;  // SURFACE_DEATH_PLANE
    case 3:  // ice material
      return 0x0013;  // SURFACE_ICE
    default:
      return 0x0000;  // SURFACE_DEFAULT
  }
}

// ============================================================================
// Debug print callback
// ============================================================================

static void sm64_debug_print(const char* msg) {
  lg::info("[SM64] {}", msg);
}

// ============================================================================
// Public API
// ============================================================================

void init(const std::string& rom_path) {
  if (s_initialized) {
    lg::warn("[SM64] Already initialized");
    return;
  }

  // Read ROM file
  auto full_path = file_util::get_user_config_dir() / rom_path;
  std::ifstream rom_file(full_path, std::ios::binary | std::ios::ate);
  if (!rom_file.is_open()) {
    // Try as absolute path
    rom_file.open(rom_path, std::ios::binary | std::ios::ate);
    if (!rom_file.is_open()) {
      lg::error("[SM64] Failed to open ROM file: {}", rom_path);
      return;
    }
  }

  auto size = rom_file.tellg();
  rom_file.seekg(0, std::ios::beg);
  s_rom_data.resize(size);
  rom_file.read(reinterpret_cast<char*>(s_rom_data.data()), size);
  rom_file.close();

  lg::info("[SM64] ROM loaded: {} bytes", (long long)size);

  // Register debug print
  sm64_register_debug_print_function(sm64_debug_print);

  // Initialize libsm64 - outputs texture atlas (704x64 RGBA)
  std::vector<uint8_t> texture_data(4 * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT);
  sm64_global_init(s_rom_data.data(), texture_data.data());

  // Upload texture atlas to OpenGL
  glGenTextures(1, &s_texture_id);
  glBindTexture(GL_TEXTURE_2D, s_texture_id);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SM64_TEXTURE_WIDTH, SM64_TEXTURE_HEIGHT, 0, GL_RGBA,
               GL_UNSIGNED_BYTE, texture_data.data());
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glBindTexture(GL_TEXTURE_2D, 0);

  s_initialized = true;
  lg::info("[SM64] Initialized successfully, texture ID: {}", s_texture_id);
}

void shutdown() {
  if (!s_initialized)
    return;

  if (s_mario_id >= 0) {
    sm64_mario_delete(s_mario_id);
    s_mario_id = -1;
  }

  sm64_global_terminate();

  if (s_texture_id) {
    glDeleteTextures(1, &s_texture_id);
    s_texture_id = 0;
  }

  s_rom_data.clear();
  s_initialized = false;
  lg::info("[SM64] Shutdown complete");
}

bool is_initialized() {
  return s_initialized;
}

int create_mario(float x, float y, float z) {
  if (!s_initialized) {
    lg::error("[SM64] Cannot create Mario: not initialized");
    return -1;
  }

  float sm_x, sm_y, sm_z;
  og_to_sm64_float(x, y, z, sm_x, sm_y, sm_z);

  s_mario_id = sm64_mario_create(sm_x, sm_y, sm_z);
  if (s_mario_id < 0) {
    lg::error("[SM64] Failed to create Mario at ({}, {}, {})", sm_x, sm_y, sm_z);
  } else {
    lg::info("[SM64] Created Mario {} at SM64 coords ({}, {}, {})", s_mario_id, sm_x, sm_y, sm_z);
  }
  return s_mario_id;
}

void delete_mario(int id) {
  if (!s_initialized || id < 0)
    return;
  sm64_mario_delete(id);
  if (id == s_mario_id)
    s_mario_id = -1;
  lg::info("[SM64] Deleted Mario {}", id);
}

void load_surfaces(SM64ControlBlock* block) {
  if (!s_initialized || !block)
    return;

  int num_tris = block->num_collide_tris;
  if (num_tris > 0 && block->collide_tris_ptr) {
    auto* goal_tris = Ptr<SM64CollideTri>(block->collide_tris_ptr).c();
    std::vector<SM64Surface> surfaces(num_tris);

    for (int i = 0; i < num_tris; i++) {
      auto& src = goal_tris[i];
      auto& dst = surfaces[i];

      dst.type = pat_to_sm64_surface(src.surface_type);
      dst.force = 0;
      dst.terrain = src.terrain_type;

      og_to_sm64(src.v0x, src.v0y, src.v0z, dst.vertices[0][0], dst.vertices[0][1],
                 dst.vertices[0][2]);
      og_to_sm64(src.v1x, src.v1y, src.v1z, dst.vertices[1][0], dst.vertices[1][1],
                 dst.vertices[1][2]);
      og_to_sm64(src.v2x, src.v2y, src.v2z, dst.vertices[2][0], dst.vertices[2][1],
                 dst.vertices[2][2]);
    }

    sm64_static_surfaces_load(surfaces.data(), surfaces.size());
    lg::info("[SM64] Loaded {} collision surfaces", num_tris);
  }
}

void tick(SM64ControlBlock* block) {
  if (!s_initialized || !block || block->mario_id < 0)
    return;

  // 1. Convert collision triangles from GOAL memory to SM64 surfaces
  int num_tris = block->num_collide_tris;
  if (num_tris > 0 && block->collide_tris_ptr) {
    auto* goal_tris = Ptr<SM64CollideTri>(block->collide_tris_ptr).c();
    std::vector<SM64Surface> surfaces(num_tris);

    for (int i = 0; i < num_tris; i++) {
      auto& src = goal_tris[i];
      auto& dst = surfaces[i];

      dst.type = pat_to_sm64_surface(src.surface_type);
      dst.force = 0;
      dst.terrain = src.terrain_type;

      // Convert OpenGOAL world-space vertices to SM64 integer coordinates
      og_to_sm64(src.v0x, src.v0y, src.v0z, dst.vertices[0][0], dst.vertices[0][1],
                 dst.vertices[0][2]);
      og_to_sm64(src.v1x, src.v1y, src.v1z, dst.vertices[1][0], dst.vertices[1][1],
                 dst.vertices[1][2]);
      og_to_sm64(src.v2x, src.v2y, src.v2z, dst.vertices[2][0], dst.vertices[2][1],
                 dst.vertices[2][2]);
    }

    sm64_static_surfaces_load(surfaces.data(), surfaces.size());
  }

  // 2. Convert inputs
  SM64MarioInputs inputs;
  memset(&inputs, 0, sizeof(inputs));
  inputs.camLookX = block->cam_look_x;
  inputs.camLookZ = block->cam_look_z;
  inputs.stickX = block->stick_x;
  inputs.stickY = block->stick_y;
  // GOAL symbols: #f = offset_of_s7(), anything else = #t
  inputs.buttonA = (block->button_a != offset_of_s7()) ? 1 : 0;
  inputs.buttonB = (block->button_b != offset_of_s7()) ? 1 : 0;
  inputs.buttonZ = (block->button_z != offset_of_s7()) ? 1 : 0;

  // 3. Tick Mario
  SM64MarioState state;
  SM64MarioGeometryBuffers geo;
  geo.position = s_positions;
  geo.normal = s_normals;
  geo.color = s_colors;
  geo.uv = s_uvs;

  sm64_mario_tick(block->mario_id, &inputs, &state, &geo);

  // 4. Write state back to GOAL block (SM64 coords -> OpenGOAL coords)
  block->pos_x = state.position[0] * SM64_TO_OG_SCALE;
  block->pos_y = state.position[1] * SM64_TO_OG_SCALE;
  block->pos_z = state.position[2] * SM64_TO_OG_SCALE;
  block->vel_x = state.velocity[0] * SM64_TO_OG_SCALE;
  block->vel_y = state.velocity[1] * SM64_TO_OG_SCALE;
  block->vel_z = state.velocity[2] * SM64_TO_OG_SCALE;
  block->face_angle = state.faceAngle;
  block->health = state.health;
  block->action = state.action;
  block->flags = state.flags;
  block->anim_id = state.animID;
  block->anim_frame = state.animFrame;

  // 5. Copy geometry to render buffer (double-buffered for thread safety)
  int write_idx = 1 - s_read_buffer.load(std::memory_order_acquire);
  {
    std::lock_guard<std::mutex> lock(s_write_mutex);
    auto& buf = s_render_buffers[write_idx];
    int num_verts = geo.numTrianglesUsed * 3;

    buf.num_triangles = geo.numTrianglesUsed;
    buf.positions.resize(num_verts * 3);
    buf.normals.resize(num_verts * 3);
    buf.colors.resize(num_verts * 3);
    buf.uvs.resize(num_verts * 2);

    // Copy and scale positions from SM64 to OpenGOAL coordinate space
    for (int i = 0; i < num_verts * 3; i += 3) {
      buf.positions[i + 0] = s_positions[i + 0] * SM64_TO_OG_SCALE;
      buf.positions[i + 1] = s_positions[i + 1] * SM64_TO_OG_SCALE;
      buf.positions[i + 2] = s_positions[i + 2] * SM64_TO_OG_SCALE;
    }

    memcpy(buf.normals.data(), s_normals, num_verts * 3 * sizeof(float));
    memcpy(buf.colors.data(), s_colors, num_verts * 3 * sizeof(float));
    memcpy(buf.uvs.data(), s_uvs, num_verts * 2 * sizeof(float));
    buf.valid = true;
  }

  // Swap read buffer
  s_read_buffer.store(write_idx, std::memory_order_release);
}

MarioRenderData get_render_data() {
  int idx = s_read_buffer.load(std::memory_order_acquire);
  return s_render_buffers[idx];
}

u32 get_texture_gl_id() {
  return s_texture_id;
}

}  // namespace sm64_bridge
