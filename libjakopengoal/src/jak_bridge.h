/**
 * jak_bridge.h - Internal bridge between the C API and the GOAL runtime.
 *
 * This layer manages:
 *   - External collision surfaces (injected into GOAL's collide-cache)
 *   - Pad input injection (bypass SDL, write directly to CPadInfo)
 *   - State extraction (read target process fields)
 *   - Mesh extraction (CPU-side bone skinning of Merc vertex data)
 *   - Command queue (spawn, teleport, damage, etc.)
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "libjakopengoal/include/libjakopengoal.h"

namespace jak_bridge {

/* -------------------------------------------------------------------------- */
/*  External collision surface storage                                        */
/* -------------------------------------------------------------------------- */

struct ExternalSurface {
  float vertices[3][3];  // 3 vertices, xyz each
  float normal[3];       // face normal
  int16_t type;
  int16_t flags;
};

struct DynamicSurfaceObject {
  uint32_t id;
  JakObjectTransform transform;
  JakObjectTransform prev_transform;  // previous frame's transform (for velocity)
  float velocity[3] = {0, 0, 0};     // computed delta position per move call
  bool velocity_applied = false;      // reset each move, set after displacement
  float half_width = 0;              // XZ bounding half-size (computed from surfaces)
  float half_depth = 0;
  float half_height = 0;             // Y half-height
  std::vector<ExternalSurface> surfaces;
  bool dirty;  // transform changed, needs update
};

/**
 * Global storage for collision surfaces sent from the host engine.
 * GOAL reads from these during its collision fill phase.
 */
struct CollisionState {
  std::mutex mutex;
  std::vector<ExternalSurface> static_surfaces;
  std::vector<DynamicSurfaceObject> dynamic_objects;
  uint32_t next_dynamic_id = 1;
  bool static_dirty = false;
};

CollisionState& get_collision_state();

/* -------------------------------------------------------------------------- */
/*  Water level                                                               */
/* -------------------------------------------------------------------------- */

struct WaterState {
  std::atomic<float> height{-11000.0f};  // SM64 units; -11000 = no water
};

WaterState& get_water_state();
void set_water_level(float height);

struct PlatformRiderState {
  std::atomic<bool> on_platform{false};
  std::atomic<float> vel_x{0.0f};
  std::atomic<float> vel_y{0.0f};
  std::atomic<float> vel_z{0.0f};
};

PlatformRiderState& get_platform_rider_state();

/* -------------------------------------------------------------------------- */
/*  Pad input injection                                                       */
/* -------------------------------------------------------------------------- */

struct PadState {
  std::mutex mutex;
  JakInputs current_inputs;
  bool has_input = false;
};

PadState& get_pad_state();

/**
 * Called from the GOAL runtime's pad read path.
 * Injects external inputs instead of reading from SDL.
 */
void inject_pad_inputs();

/* -------------------------------------------------------------------------- */
/*  Jak state extraction                                                      */
/* -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- */
/*  Bone debug data (for skeleton visualization)                              */
/* -------------------------------------------------------------------------- */

struct BoneDebugData {
  std::mutex mutex;
  float positions[256][3];     // world-space bone positions (render units)
  int parent_indices[256];     // parent bone index (-1 = no parent)
  int num_bones = 0;
  bool valid = false;
};

BoneDebugData& get_bone_debug_data();

struct JakInternalState {
  std::mutex mutex;
  JakState state;
  bool valid = false;
};

JakInternalState& get_jak_state();

/**
 * Read Jak's (target) state from GOAL memory.
 * Called after each GOAL tick.
 */
void extract_jak_state();

/* -------------------------------------------------------------------------- */
/*  Mesh / geometry extraction                                                */
/* -------------------------------------------------------------------------- */

struct TextureAtlasInfo {
  uint32_t atlas_width = 0;
  uint32_t atlas_height = 0;
  uint32_t num_textures = 0;
  bool valid = false;
};

TextureAtlasInfo& get_texture_atlas_info();

/**
 * Set the texture output buffer pointer (called from libjakopengoal.cpp during init).
 */
void set_texture_output(uint8_t* ptr);

/**
 * Get the texture output buffer pointer.
 */
uint8_t* get_texture_output();

struct MeshState {
  std::mutex mutex;
  // Pre-skinned vertex data (CPU-side bone transform applied)
  std::vector<float> positions;  // x,y,z per vertex
  std::vector<float> normals;    // nx,ny,nz per vertex
  std::vector<float> colors;     // r,g,b,a per vertex
  std::vector<float> uvs;        // u,v per vertex (atlas-remapped)
  uint32_t num_triangles = 0;
  bool valid = false;
};

MeshState& get_mesh_state();

/**
 * Extract Jak's mesh data by performing CPU-side bone skinning.
 * Called after each GOAL tick when the Merc data is available.
 */
void extract_jak_mesh();

/* -------------------------------------------------------------------------- */
/*  Command queue (host -> GOAL)                                              */
/* -------------------------------------------------------------------------- */

enum class CommandType {
  SPAWN,
  DESTROY,
  SET_POSITION,
  SET_VELOCITY,
  SET_ANGLE,
  SET_ACTION,
  SET_HEALTH,
  TAKE_DAMAGE,
  HEAL,
  KILL,
  GIVE_ECO,
};

struct Command {
  CommandType type;
  int32_t jak_id;
  float f[4];    // generic float args
  int32_t i[2];  // generic int args
};

struct CommandQueue {
  std::mutex mutex;
  std::vector<Command> pending;
};

CommandQueue& get_command_queue();

/**
 * Process all pending commands. Called at the start of each GOAL tick.
 */
void process_commands();

/* -------------------------------------------------------------------------- */
/*  Runtime lifecycle                                                         */
/* -------------------------------------------------------------------------- */

/**
 * Initialize the bridge. Called after the GOAL runtime boots.
 * Registers C++ functions as GOAL callables and sets up shared state.
 */
void initialize_bridge();

/**
 * Launch the GOAL runtime headlessly. Resolves exec_runtime() via
 * a function pointer to avoid pulling in runtime.obj at link time
 * (which brings in graphics static constructors that fail in Blender).
 * Returns the exit status, or -1 if exec_runtime couldn't be resolved.
 */
int launch_runtime_headless(const std::string& game_data_path);

/**
 * Called once per GOAL frame (from the EE thread) to:
 *   1. Process pending commands
 *   2. Inject pad inputs
 *   3. (Collision is injected during GOAL's fill phase via registered function)
 *   4. Extract Jak's state
 *   5. Extract Jak's mesh
 */
void bridge_tick();

/**
 * Returns true after the GOAL runtime has fully booted
 * and the target (Jak) process is initialized.
 */
bool is_runtime_ready();

/**
 * Set the runtime ready flag.
 */
void set_runtime_ready(bool ready);

/* -------------------------------------------------------------------------- */
/*  C++ functions registered as GOAL callables                                */
/* -------------------------------------------------------------------------- */

/**
 * Called from GOAL during collide-cache fill to get external collision data.
 * GOAL signature: (function collide-cache int)
 * Returns number of triangles added.
 */
uint64_t goal_fill_external_collide(uint64_t cache_addr);

/**
 * Called from GOAL to signal that the target process is ready.
 * GOAL signature: (function none)
 */
uint64_t goal_notify_target_ready();

/**
 * Called from GOAL to provide Jak's bone matrices for CPU skinning.
 * GOAL signature: (function pointer int none)
 * Args: bone_data_ptr, num_bones
 */
uint64_t goal_provide_bone_data(uint64_t bone_data, uint64_t num_bones);

}  // namespace jak_bridge
