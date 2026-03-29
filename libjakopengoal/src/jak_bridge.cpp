/**
 * jak_bridge.cpp - Bridge between the C API and the GOAL runtime.
 */

#include "jak_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/log/log.h"
#include "common/symbols.h"
#include "game/kernel/common/Ptr.h"
#include "game/kernel/common/kernel_types.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/runtime.h"

namespace jak_bridge {

/* -------------------------------------------------------------------------- */
/*  Singleton state accessors                                                 */
/* -------------------------------------------------------------------------- */

static CollisionState s_collision_state;
static PadState s_pad_state;
static JakInternalState s_jak_state;
static MeshState s_mesh_state;
static CommandQueue s_command_queue;
static std::atomic<bool> s_runtime_ready{false};

// Bone matrix storage for CPU skinning
struct BoneMatrix {
  float tmat[4][4];  // 4x4 transform matrix
  float nmat[3][3];  // 3x3 normal matrix
};
static std::mutex s_bone_mutex;
static std::vector<BoneMatrix> s_bone_matrices;
static bool s_bones_valid = false;

CollisionState& get_collision_state() {
  return s_collision_state;
}
PadState& get_pad_state() {
  return s_pad_state;
}
JakInternalState& get_jak_state() {
  return s_jak_state;
}
MeshState& get_mesh_state() {
  return s_mesh_state;
}
CommandQueue& get_command_queue() {
  return s_command_queue;
}

bool is_runtime_ready() {
  return s_runtime_ready.load();
}

void set_runtime_ready(bool ready) {
  s_runtime_ready.store(ready);
}

/* -------------------------------------------------------------------------- */
/*  Pad input injection                                                       */
/* -------------------------------------------------------------------------- */

void inject_pad_inputs() {
  // This writes directly to the CPadInfo structure that GOAL reads.
  // We access it via the pad_dma_buf global in kmachine.
  // The actual injection happens in our patched scePadRead function.
  // See libjakopengoal.cpp for the scePadRead override.
}

/* -------------------------------------------------------------------------- */
/*  State extraction                                                          */
/* -------------------------------------------------------------------------- */

void extract_jak_state() {
  if (!g_ee_main_mem) {
    return;
  }

  // Find the *target* symbol - this is Jak's process
  auto target_sym = jak1::intern_from_c("*target*");
  if (!target_sym.offset || target_sym->value == 0) {
    return;
  }

  // target is a process-drawable with control-info overlay at root
  // We read key fields from GOAL memory using known offsets.
  //
  // The target structure layout (from target-h.gc):
  //   process-drawable base:
  //     root (trsqv / control-info) at offset 108 in process-drawable
  //       trans (vector) at offset 16 in trsqv
  //     ...
  //   game (game-info) at known offset
  //
  // We use the symbol approach: read fields from GOAL symbols that the
  // engine maintains for us.

  u32 target_ptr = target_sym->value;
  if (target_ptr == 0 || target_ptr == s7.offset) {
    return;
  }

  // Try the GOAL bridge structure first
  auto state_sym = jak1::intern_from_c("*lib-jak-state-data*");
  if (state_sym.offset && state_sym->value != 0 &&
      state_sym->value != s7.offset) {
    // Read from the lib-jak-state-data structure that our GOAL bridge code allocates.
    float* fdata = (float*)(g_ee_main_mem + state_sym->value);
    int32_t* idata = (int32_t*)(g_ee_main_mem + state_sym->value);

    std::lock_guard<std::mutex> lock(s_jak_state.mutex);
    s_jak_state.state.position[0] = fdata[0];
    s_jak_state.state.position[1] = fdata[1];
    s_jak_state.state.position[2] = fdata[2];
    s_jak_state.state.velocity[0] = fdata[3];
    s_jak_state.state.velocity[1] = fdata[4];
    s_jak_state.state.velocity[2] = fdata[5];
    s_jak_state.state.face_angle = fdata[6];
    s_jak_state.state.forward_velocity = fdata[7];
    s_jak_state.state.hp = fdata[8];
    s_jak_state.state.eco_type = idata[9];
    s_jak_state.state.action = (uint32_t)idata[10];
    s_jak_state.state.anim_id = idata[11];
    s_jak_state.state.anim_frame = (int16_t)idata[12];
    s_jak_state.state.flags = (uint32_t)idata[13];
    s_jak_state.state.orb_count = idata[14];
    s_jak_state.state.buzz_count = idata[15];
    s_jak_state.state.cell_count = idata[16];

    uint8_t* bdata = (uint8_t*)(g_ee_main_mem + state_sym->value + 17 * 4);
    s_jak_state.state.on_ground = bdata[0] != 0;
    s_jak_state.state.in_water = bdata[1] != 0;
    s_jak_state.valid = true;
    return;
  }

  // Fallback: read position directly from *target* process memory.
  // process-drawable has 'root' pointer at offset 112 (0x70).
  // root (trsqv) has 'trans' vector at offset 16 (0x10).
  // trans is a vector4: (x y z w) as 4 floats in GOAL units (4096 = 1 meter).
  constexpr float METERS_TO_UNITS = 50.0f / 4096.0f;

  // 'root' is a GOAL pointer at offset 108 in process-drawable.
  // (process at offset ~96, then drawable fields, root at 108)
  u32 root_ptr = *(u32*)(g_ee_main_mem + target_ptr + 108);

  if (root_ptr == 0 || root_ptr == s7.offset) {
    return;
  }

  // trans vector is at offset 12 within root (verified against known continue-point data).
  // Each component is a float in GOAL's internal units (4096 per meter).
  float* trans = (float*)(g_ee_main_mem + root_ptr + 12);

  std::lock_guard<std::mutex> lock(s_jak_state.mutex);
  s_jak_state.state.position[0] = trans[0] * METERS_TO_UNITS;
  s_jak_state.state.position[1] = trans[1] * METERS_TO_UNITS;
  s_jak_state.state.position[2] = trans[2] * METERS_TO_UNITS;
  s_jak_state.valid = true;
}

/* -------------------------------------------------------------------------- */
/*  Mesh extraction (CPU-side bone skinning)                                  */
/* -------------------------------------------------------------------------- */

static void skin_vertex(const float* pos_in,
                         const float* nrm_in,
                         const uint8_t* mats,
                         const float* weights,
                         const std::vector<BoneMatrix>& bones,
                         float* pos_out,
                         float* nrm_out) {
  pos_out[0] = pos_out[1] = pos_out[2] = 0.0f;
  nrm_out[0] = nrm_out[1] = nrm_out[2] = 0.0f;

  for (int b = 0; b < 3; b++) {
    float w = weights[b];
    if (w <= 0.0f)
      continue;

    uint8_t bone_idx = mats[b];
    if (bone_idx >= bones.size())
      continue;

    const auto& bone = bones[bone_idx];

    // Transform position: pos_out += w * (bone.tmat * pos_in)
    for (int r = 0; r < 3; r++) {
      float v = bone.tmat[r][3];  // translation component
      for (int c = 0; c < 3; c++) {
        v += bone.tmat[r][c] * pos_in[c];
      }
      pos_out[r] += w * v;
    }

    // Transform normal: nrm_out += w * (bone.nmat * nrm_in)
    for (int r = 0; r < 3; r++) {
      float v = 0.0f;
      for (int c = 0; c < 3; c++) {
        v += bone.nmat[r][c] * nrm_in[c];
      }
      nrm_out[r] += w * v;
    }
  }

  // Normalize the normal
  float len = std::sqrt(nrm_out[0] * nrm_out[0] + nrm_out[1] * nrm_out[1] +
                         nrm_out[2] * nrm_out[2]);
  if (len > 0.001f) {
    nrm_out[0] /= len;
    nrm_out[1] /= len;
    nrm_out[2] /= len;
  }
}

void extract_jak_mesh() {
  if (!g_ee_main_mem) {
    return;
  }

  // Check if we have bone data and mesh vertex data
  // The mesh data pointer is provided by our GOAL bridge code
  auto mesh_sym = jak1::intern_from_c("*lib-jak-mesh-data*");
  auto mesh_count_sym = jak1::intern_from_c("*lib-jak-mesh-count*");

  if (!mesh_sym.offset || mesh_sym->value == 0 ||
      mesh_sym->value == s7.offset) {
    return;
  }
  if (!mesh_count_sym.offset || mesh_count_sym->value == 0) {
    return;
  }

  uint32_t vert_count = mesh_count_sym->value;
  if (vert_count == 0 || vert_count > JAK_GEO_MAX_TRIANGLES * 3) {
    return;
  }

  // The mesh data is a flat array of our vertex format in GOAL memory:
  // struct { float pos[3]; float normal[3]; float uv[2]; u8 rgba[4]; u8 mats[3]; u8 weights_u8[3]; }
  // = 44 bytes per vertex

  struct GoalMeshVertex {
    float pos[3];
    float normal[3];
    float uv[2];
    uint8_t rgba[4];
    uint8_t mats[3];
    uint8_t pad;
    float weights[3];
    float pad2;
  };
  static_assert(sizeof(GoalMeshVertex) == 56);

  // Alternate: use the tfrag3::MercVertex format (64 bytes) that's already in the vertex buffer
  // We'll read whatever format our GOAL bridge provides

  std::lock_guard<std::mutex> bone_lock(s_bone_mutex);
  if (!s_bones_valid || s_bone_matrices.empty()) {
    return;
  }

  uint8_t* vert_base = g_ee_main_mem + mesh_sym->value;

  std::lock_guard<std::mutex> lock(s_mesh_state.mutex);
  uint32_t num_verts = vert_count;
  uint32_t num_tris = num_verts / 3;

  s_mesh_state.positions.resize(num_verts * 3);
  s_mesh_state.normals.resize(num_verts * 3);
  s_mesh_state.colors.resize(num_verts * 4);
  s_mesh_state.uvs.resize(num_verts * 2);

  for (uint32_t i = 0; i < num_verts; i++) {
    auto* v = (GoalMeshVertex*)(vert_base + i * sizeof(GoalMeshVertex));

    float skinned_pos[3], skinned_nrm[3];
    skin_vertex(v->pos, v->normal, v->mats, v->weights, s_bone_matrices, skinned_pos,
                skinned_nrm);

    s_mesh_state.positions[i * 3 + 0] = skinned_pos[0];
    s_mesh_state.positions[i * 3 + 1] = skinned_pos[1];
    s_mesh_state.positions[i * 3 + 2] = skinned_pos[2];

    s_mesh_state.normals[i * 3 + 0] = skinned_nrm[0];
    s_mesh_state.normals[i * 3 + 1] = skinned_nrm[1];
    s_mesh_state.normals[i * 3 + 2] = skinned_nrm[2];

    s_mesh_state.colors[i * 4 + 0] = v->rgba[0] / 255.0f;
    s_mesh_state.colors[i * 4 + 1] = v->rgba[1] / 255.0f;
    s_mesh_state.colors[i * 4 + 2] = v->rgba[2] / 255.0f;
    s_mesh_state.colors[i * 4 + 3] = v->rgba[3] / 255.0f;

    s_mesh_state.uvs[i * 2 + 0] = v->uv[0];
    s_mesh_state.uvs[i * 2 + 1] = v->uv[1];
  }

  s_mesh_state.num_triangles = num_tris;
  s_mesh_state.valid = true;
}

/* -------------------------------------------------------------------------- */
/*  Command processing                                                        */
/* -------------------------------------------------------------------------- */

void process_commands() {
  std::vector<Command> cmds;
  {
    std::lock_guard<std::mutex> lock(s_command_queue.mutex);
    cmds.swap(s_command_queue.pending);
  }

  if (cmds.empty() || !g_ee_main_mem) {
    return;
  }

  // Find the command buffer symbol that our GOAL bridge reads
  auto cmd_sym = jak1::intern_from_c("*lib-jak-command*");
  auto cmd_type_sym = jak1::intern_from_c("*lib-jak-command-type*");
  auto cmd_args_sym = jak1::intern_from_c("*lib-jak-command-args-data*");

  if (!cmd_sym.offset || !cmd_type_sym.offset || !cmd_args_sym.offset) {
    return;
  }

  // Process one command per tick (GOAL will read and clear it)
  // If multiple commands queued, we process the first and re-queue the rest
  for (auto& cmd : cmds) {
    // Write command type
    cmd_type_sym->value = static_cast<u32>(cmd.type);

    // Write float args to the command args buffer
    float* args = (float*)(g_ee_main_mem + cmd_args_sym->value);
    if (cmd_args_sym->value != 0 &&
        cmd_args_sym->value != s7.offset) {
      args[0] = cmd.f[0];
      args[1] = cmd.f[1];
      args[2] = cmd.f[2];
      args[3] = cmd.f[3];
      // Int args stored as floats for simplicity
      memcpy(&args[4], &cmd.i[0], sizeof(int32_t));
      memcpy(&args[5], &cmd.i[1], sizeof(int32_t));
    }

    // Signal that a command is pending
    cmd_sym->value = 1;
  }
}

/* -------------------------------------------------------------------------- */
/*  GOAL-callable functions                                                   */
/* -------------------------------------------------------------------------- */

uint64_t goal_fill_external_collide(uint64_t cache_addr) {
  if (cache_addr == 0) {
    return 0;
  }

  std::lock_guard<std::mutex> lock(s_collision_state.mutex);

  // The cache_addr points to a collide-cache structure in GOAL memory.
  // Layout (from collide-cache-h.gc):
  //   offset 0:   type tag (basic)
  //   offset 4:   num-tris (int32)
  //   offset 8:   num-prims (int32)
  //   offset 12:  ignore-mask (pat-surface)
  //   offset 16:  proc (process-drawable)
  //   offset 20:  collide-box (bounding-box, 32 bytes)
  //   offset 52:  collide-box4w (bounding-box4w, 32 bytes)
  //   offset 84:  collide-with (collide-kind, 8 bytes)
  //   offset 92:  pad to 96
  //   offset 96:  prims array (100 * 48 bytes = 4800 bytes)
  //   offset 4896: tris array (461 * 64 bytes = 29504 bytes)

  // We write collision triangles directly into the cache's tri array
  // and set up a single background prim that references them.

  uint8_t* cache = g_ee_main_mem + (uint32_t)cache_addr;

  int32_t* num_tris_ptr = (int32_t*)(cache + 4);
  int32_t existing_tris = *num_tris_ptr;

  // collide-cache-tri is 64 bytes:
  //   offset 0:  vertex[0] (vector, 16 bytes: x,y,z,w)
  //   offset 16: vertex[1] (vector, 16 bytes)
  //   offset 32: vertex[2] (vector, 16 bytes)
  //   offset 48: pat-surface (uint32)
  //   offset 52: prim-index (uint16)
  //   offset 54: user16 (uint16)
  //   offset 56: user32[0] (uint32)
  //   offset 60: user32[1] (uint32)

  const int TRI_OFFSET = 4896;  // offset of tris array from cache base
  const int MAX_TRIS = 461;

  int tris_added = 0;
  auto& surfaces = s_collision_state.static_surfaces;

  for (size_t s = 0; s < surfaces.size() && (existing_tris + tris_added) < MAX_TRIS; s++) {
    auto& surf = surfaces[s];
    int tri_idx = existing_tris + tris_added;
    uint8_t* tri_base = cache + TRI_OFFSET + tri_idx * 64;

    // Write vertex 0
    float* v0 = (float*)(tri_base + 0);
    v0[0] = surf.vertices[0][0];
    v0[1] = surf.vertices[0][1];
    v0[2] = surf.vertices[0][2];
    v0[3] = 1.0f;

    // Write vertex 1
    float* v1 = (float*)(tri_base + 16);
    v1[0] = surf.vertices[1][0];
    v1[1] = surf.vertices[1][1];
    v1[2] = surf.vertices[1][2];
    v1[3] = 1.0f;

    // Write vertex 2
    float* v2 = (float*)(tri_base + 32);
    v2[0] = surf.vertices[2][0];
    v2[1] = surf.vertices[2][1];
    v2[2] = surf.vertices[2][2];
    v2[3] = 1.0f;

    // Write pat-surface (material type encoded as GOAL pat-surface bitfield)
    // pat-surface is a uint32 with fields:
    //   bits 0-2: material (stone, ice, etc)
    //   bits 3-5: event (none, deadly, endlessfall, etc)
    //   bits 6-13: noentity, nocamera, etc flags
    // For external surfaces, we just set the material bits and mark as solid
    uint32_t* pat = (uint32_t*)(tri_base + 48);
    *pat = (uint32_t)(surf.type & 0x7);  // material in bottom 3 bits

    // prim-index - point to prim 0 (background prim)
    uint16_t* prim_idx = (uint16_t*)(tri_base + 52);
    *prim_idx = 0;

    tris_added++;
  }

  // Also add dynamic object surfaces
  for (auto& obj : s_collision_state.dynamic_objects) {
    for (auto& surf : obj.surfaces) {
      if ((existing_tris + tris_added) >= MAX_TRIS)
        break;

      int tri_idx = existing_tris + tris_added;
      uint8_t* tri_base = cache + TRI_OFFSET + tri_idx * 64;

      // Apply object transform to vertices
      float cos_y = std::cos(obj.transform.rotation[1]);
      float sin_y = std::sin(obj.transform.rotation[1]);

      for (int vi = 0; vi < 3; vi++) {
        float lx = surf.vertices[vi][0];
        float ly = surf.vertices[vi][1];
        float lz = surf.vertices[vi][2];

        // Simple Y-axis rotation + translation
        float wx = cos_y * lx + sin_y * lz + obj.transform.position[0];
        float wy = ly + obj.transform.position[1];
        float wz = -sin_y * lx + cos_y * lz + obj.transform.position[2];

        float* v = (float*)(tri_base + vi * 16);
        v[0] = wx;
        v[1] = wy;
        v[2] = wz;
        v[3] = 1.0f;
      }

      uint32_t* pat = (uint32_t*)(tri_base + 48);
      *pat = (uint32_t)(surf.type & 0x7);
      uint16_t* prim_idx = (uint16_t*)(tri_base + 52);
      *prim_idx = 0;

      tris_added++;
    }
  }

  // Update the tri count
  *num_tris_ptr = existing_tris + tris_added;

  // Ensure there's at least one prim (background prim) if we added triangles
  if (tris_added > 0) {
    int32_t* num_prims_ptr = (int32_t*)(cache + 8);
    if (*num_prims_ptr == 0) {
      // Set up prim 0 as background prim
      // collide-cache-prim is 48 bytes at offset 96 from cache base
      const int PRIM_OFFSET = 96;
      uint8_t* prim_base = cache + PRIM_OFFSET;

      // prim-core: world-sphere (16 bytes), then collide-as, action, offense, prim-type
      // We set a large bounding sphere and mark as solid background
      float* sphere = (float*)prim_base;
      sphere[0] = 0.0f;
      sphere[1] = 0.0f;
      sphere[2] = 0.0f;
      sphere[3] = 1000000.0f;  // huge radius

      // collide-as at offset 16 (8 bytes) - set background bit
      uint64_t* collide_as = (uint64_t*)(prim_base + 16);
      *collide_as = 1;  // collide-kind background

      // action at offset 24 (4 bytes) - set solid
      uint32_t* action = (uint32_t*)(prim_base + 24);
      *action = 1;  // collide-action solid

      // prim-type at offset 31 (1 byte) - 0+ means mesh
      int8_t* prim_type = (int8_t*)(prim_base + 31);
      *prim_type = 0;

      // first-tri at offset 40 (uint16)
      uint16_t* first_tri = (uint16_t*)(prim_base + 40);
      *first_tri = (uint16_t)existing_tris;

      // num-tris at offset 42 (uint16)
      uint16_t* num_tris_field = (uint16_t*)(prim_base + 42);
      *num_tris_field = (uint16_t)tris_added;

      *num_prims_ptr = 1;
    } else {
      // Update existing prim's tri count
      const int PRIM_OFFSET = 96;
      uint8_t* prim_base = cache + PRIM_OFFSET;
      uint16_t* num_tris_field = (uint16_t*)(prim_base + 42);
      *num_tris_field = (uint16_t)(existing_tris + tris_added);
    }
  }

  return tris_added;
}

uint64_t goal_notify_target_ready() {
  lg::info("[libjakopengoal] Target (Jak) process is ready!");
  set_runtime_ready(true);
  return 0;
}

uint64_t goal_provide_bone_data(uint64_t bone_data_offset, uint64_t num_bones) {
  if (bone_data_offset == 0 || num_bones == 0 || !g_ee_main_mem) {
    return 0;
  }

  // Bone data is an array of ShaderMercMat-equivalent structures:
  // Each bone = 4 vec4 (tmat) + 3 vec4 (nmat) = 7 * 16 = 112 bytes
  struct GoalBoneData {
    float tmat[4][4];  // 64 bytes
    float nmat[3][4];  // 48 bytes (padded vec3s)
  };
  static_assert(sizeof(GoalBoneData) == 112);

  uint32_t n = (uint32_t)num_bones;
  if (n > 128)
    n = 128;

  GoalBoneData* src = (GoalBoneData*)(g_ee_main_mem + (uint32_t)bone_data_offset);

  std::lock_guard<std::mutex> lock(s_bone_mutex);
  s_bone_matrices.resize(n);

  for (uint32_t i = 0; i < n; i++) {
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 4; c++) {
        s_bone_matrices[i].tmat[r][c] = src[i].tmat[r][c];
      }
    }
    for (int r = 0; r < 3; r++) {
      for (int c = 0; c < 3; c++) {
        s_bone_matrices[i].nmat[r][c] = src[i].nmat[r][c];
      }
    }
  }

  s_bones_valid = true;
  return 0;
}

/* -------------------------------------------------------------------------- */
/*  Camera injection                                                          */
/* -------------------------------------------------------------------------- */

/**
 * Write external camera rotation into *math-camera*'s inv-camera-rot and
 * inv-camera-rot-smooth matrices so Jak's stick input is relative to the
 * external camera's yaw angle.
 *
 * The GOAL engine uses (matrix-local->world #t #f) which returns
 * inv-camera-rot-smooth (offset 496).  We also write inv-camera-rot (432)
 * for anything that reads the non-smooth variant.
 *
 * A GOAL matrix is column-major, 4 vectors of 4 floats (64 bytes).
 * For a Y-rotation by angle θ (the camera's yaw):
 *   inv-camera-rot = Ry(-θ)  (inverse = negate the angle)
 *   col0 = ( cos θ,  0,  sin θ,  0)   right
 *   col1 = (     0,  1,      0,  0)   up
 *   col2 = (-sin θ,  0,  cos θ,  0)   forward
 *   col3 = (     0,  0,      0,  1)   translation (zero)
 */
static void inject_camera_rotation() {
  if (!g_ee_main_mem)
    return;

  // Read the camera look direction from pad state
  float cam_x, cam_z;
  {
    std::lock_guard<std::mutex> lock(s_pad_state.mutex);
    if (!s_pad_state.has_input)
      return;
    cam_x = s_pad_state.current_inputs.cam_x;
    cam_z = s_pad_state.current_inputs.cam_z;
  }

  // cam_x, cam_z is the camera look direction vector (target - eye).
  // Compute camera yaw angle from it.
  float len = sqrtf(cam_x * cam_x + cam_z * cam_z);
  if (len < 0.001f)
    return;
  cam_x /= len;
  cam_z /= len;

  // The inverse rotation matrix (what GOAL uses) has:
  //   cos θ = cam_z / len  (forward Z component = cos of yaw)
  //   sin θ = cam_x / len  (forward X component = sin of yaw)
  // But since this is the *inverse* (camera-space to world-space):
  float cos_y = cam_z;  // forward Z
  float sin_y = cam_x;  // forward X

  // Find *math-camera* symbol
  auto mc_sym = jak1::intern_from_c("*math-camera*");
  if (!mc_sym.offset || mc_sym->value == 0 || mc_sym->value == s7.offset)
    return;

  u32 mc_ptr = mc_sym->value;

  // Write to both inv-camera-rot (offset 432) and inv-camera-rot-smooth (offset 496)
  // Matrix is column-major: 4 columns of 4 floats
  for (int offset : {432, 496}) {
    float* mat = (float*)(g_ee_main_mem + mc_ptr + offset);
    // col 0 (right vector)
    mat[0] = cos_y;
    mat[1] = 0.0f;
    mat[2] = sin_y;
    mat[3] = 0.0f;
    // col 1 (up vector)
    mat[4] = 0.0f;
    mat[5] = 1.0f;
    mat[6] = 0.0f;
    mat[7] = 0.0f;
    // col 2 (forward vector)
    mat[8] = -sin_y;
    mat[9] = 0.0f;
    mat[10] = cos_y;
    mat[11] = 0.0f;
    // col 3 (translation — zero)
    mat[12] = 0.0f;
    mat[13] = 0.0f;
    mat[14] = 0.0f;
    mat[15] = 1.0f;
  }
}

/* -------------------------------------------------------------------------- */
/*  Bridge lifecycle                                                          */
/* -------------------------------------------------------------------------- */

void initialize_bridge() {
  lg::info("[libjakopengoal] Initializing bridge...");

  // Register C++ functions as GOAL callables
  jak1::make_function_symbol_from_c("lib-jak-fill-external-collide",
                                     (void*)goal_fill_external_collide);
  jak1::make_function_symbol_from_c("lib-jak-notify-target-ready",
                                     (void*)goal_notify_target_ready);
  jak1::make_function_symbol_from_c("lib-jak-provide-bone-data",
                                     (void*)goal_provide_bone_data);

  // Initialize GOAL-side symbols for communication.
  // *lib-jak-command* and *lib-jak-command-type* are integer symbols used for command passing.
  // They are set to 0 initially. GOAL code will allocate the data structures.
  jak1::intern_from_c("*lib-jak-command*")->value = 0;
  jak1::intern_from_c("*lib-jak-command-type*")->value = 0;

  // Enable the bridge flag that GOAL code checks
  // #t in GOAL is represented as s7 + FIX_SYM_TRUE
  jak1::intern_from_c("*lib-jak-enabled*")->value = (s7 + jak1_symbols::FIX_SYM_TRUE).offset;

  lg::info("[libjakopengoal] Bridge initialized.");

  // Auto-set runtime ready since GOAL scripts are loaded and bridge is active.
  // The GOAL side can also call lib-jak-notify-target-ready explicitly if needed.
  set_runtime_ready(true);
  lg::info("[libjakopengoal] Runtime marked as ready.");
}

void bridge_tick() {
  process_commands();
  inject_camera_rotation();
  extract_jak_state();
  extract_jak_mesh();
}

}  // namespace jak_bridge
