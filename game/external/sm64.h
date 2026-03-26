#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "common/common_types.h"

// Shared struct for GOAL <-> C++ communication.
// Must match the GOAL deftype sm64-control-block field-for-field.
struct SM64ControlBlock {
  u32 enabled;   // GOAL symbol
  s32 mario_id;  // -1 if not created

  // Inputs (GOAL writes, C++ reads)
  float cam_look_x;
  float cam_look_z;
  float stick_x;
  float stick_y;
  u32 button_a;  // GOAL symbol (#t/#f)
  u32 button_b;
  u32 button_z;

  // State output (C++ writes, GOAL reads)
  float pos_x, pos_y, pos_z;
  float vel_x, vel_y, vel_z;
  float face_angle;
  s16 health;
  s16 health_pad;  // padding for alignment
  u32 action;
  u32 flags;
  s32 anim_id;
  s16 anim_frame;
  s16 anim_frame_pad;  // padding

  // Collision feed (GOAL writes triangle data)
  s32 num_collide_tris;
  u32 collide_tris_ptr;  // Ptr to inline-array of SM64CollideTri in GOAL memory

  // Position for collision query center
  float query_x, query_y, query_z;
  float query_radius;
};

// Flat triangle for collision transfer from GOAL to C++.
// Must match GOAL deftype sm64-collide-tri.
struct SM64CollideTri {
  float v0x, v0y, v0z;
  float v1x, v1y, v1z;
  float v2x, v2y, v2z;
  s16 surface_type;
  s16 terrain_type;
};

namespace sm64_bridge {

// Render data for the Mario mesh, consumed by SM64MarioRenderer.
struct MarioRenderData {
  std::vector<float> positions;  // 3 floats per vertex, 3 verts per tri
  std::vector<float> normals;    // 3 floats per vertex
  std::vector<float> colors;     // 3 floats per vertex
  std::vector<float> uvs;        // 2 floats per vertex
  int num_triangles = 0;
  bool valid = false;
};

// Lifecycle
void init(const std::string& rom_path);
void shutdown();
bool is_initialized();

// Mario management
int create_mario(float x, float y, float z);
void delete_mario(int id);

// Load collision surfaces from block (can be called before Mario exists)
void load_surfaces(SM64ControlBlock* block);

// Per-frame tick: reads collision + inputs from block, writes state + geometry
void tick(SM64ControlBlock* block);

// Rendering data access (thread-safe)
MarioRenderData get_render_data();
u32 get_texture_gl_id();

// Coordinate conversion scale factor.
// SM64 uses ~100 units per real meter, OpenGOAL uses 4096 units per real meter.
// So: SM64_coord = OG_coord * (100 / 4096)
constexpr float OG_TO_SM64_SCALE = 50.0f / 4096.0f;
constexpr float SM64_TO_OG_SCALE = 4096.0f / 50.0f;

}  // namespace sm64_bridge
