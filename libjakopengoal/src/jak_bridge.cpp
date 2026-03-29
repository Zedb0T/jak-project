/**
 * jak_bridge.cpp - Bridge between the C API and the GOAL runtime.
 */

#include "jak_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/custom_data/Tfrag3Data.h"
#include "common/log/log.h"
#include "common/symbols.h"
#include "common/util/FileUtil.h"
#include "common/util/Serializer.h"
#include "common/util/compress.h"
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
static BoneDebugData s_bone_debug;

// Bone matrix storage for CPU skinning
struct BoneMatrix {
  float tmat[4][4];  // 4x4 transform matrix
  float nmat[3][3];  // 3x3 normal matrix
};
static std::mutex s_bone_mutex;
static std::vector<BoneMatrix> s_bone_matrices;
static bool s_bones_valid = false;

// FR3 model data for Jak's mesh (loaded once at init)
struct Fr3ModelData {
  std::vector<tfrag3::MercVertex> vertices;  // base-pose vertices (indexed by indices)
  std::vector<u32> indices;                  // triangle indices
  // Per-effect draw ranges so we can iterate all triangles
  struct DrawRange {
    u32 first_index;
    u32 index_count;
  };
  std::vector<DrawRange> draws;
  float xyz_scale = 1.0f;
  u32 max_bones = 0;
  bool loaded = false;
};
static Fr3ModelData s_fr3_model;
static std::mutex s_fr3_mutex;

/**
 * Load Jak's merc model from an FR3 file.
 * Searches for "jak" in the given FR3, extracts vertices/indices.
 */
static bool load_jak_model_from_fr3(const std::string& fr3_name) {
  auto fr3_path =
      file_util::get_jak_project_dir() / "out" / "jak1" / "fr3" / fmt::format("{}.fr3", fr3_name);

  lg::info("[libjakopengoal] Loading FR3: {}", fr3_path.string());

  std::vector<u8> data;
  try {
    data = file_util::read_binary_file(fr3_path);
  } catch (const std::exception& e) {
    lg::error("[libjakopengoal] Failed to read FR3 file: {}", e.what());
    return false;
  }

  if (data.empty()) {
    lg::error("[libjakopengoal] FR3 file is empty");
    return false;
  }

  auto decomp_data = compression::decompress_zstd(data.data(), data.size());
  tfrag3::Level level;
  Serializer ser(decomp_data.data(), decomp_data.size());
  level.serialize(ser);

  // Find Jak's merc model (internal codename "eichar")
  const tfrag3::MercModel* jak_model = nullptr;
  for (const auto& model : level.merc_data.models) {
    if (model.name == "eichar-lod0") {
      jak_model = &model;
      break;
    }
  }

  if (!jak_model) {
    lg::warn("[libjakopengoal] eichar-lod0 not found in {}.fr3", fr3_name);
    return false;
  }

  lg::info("[libjakopengoal] Found eichar-lod0: {} effects, {} max_bones, xyz_scale={}",
           jak_model->effects.size(), jak_model->max_bones, jak_model->xyz_scale);

  // Collect all draw ranges and figure out which vertices we need
  std::lock_guard<std::mutex> lock(s_fr3_mutex);
  s_fr3_model.draws.clear();
  s_fr3_model.xyz_scale = jak_model->xyz_scale;
  s_fr3_model.max_bones = jak_model->max_bones;

  for (const auto& effect : jak_model->effects) {
    for (const auto& draw : effect.all_draws) {
      s_fr3_model.draws.push_back({draw.first_index, draw.index_count});
    }
  }

  // Copy the shared vertex and index buffers
  // (We copy all vertices/indices from the level — the draws reference specific index ranges)
  s_fr3_model.vertices = level.merc_data.vertices;
  s_fr3_model.indices = level.merc_data.indices;
  s_fr3_model.loaded = true;

  u32 total_tris = 0;
  for (const auto& d : s_fr3_model.draws) {
    total_tris += d.index_count / 3;
  }
  lg::info("[libjakopengoal] Loaded eichar-lod0: {} vertices, {} indices, {} draws, ~{} triangles",
           s_fr3_model.vertices.size(), s_fr3_model.indices.size(), s_fr3_model.draws.size(),
           total_tris);

  return true;
}

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
BoneDebugData& get_bone_debug_data() {
  return s_bone_debug;
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
  // Pad injection is handled by the patched scePadRead in game/sce/libpad.cpp.
  // It reads from s_pad_state.current_inputs when lib_jak_bridge mode is active.
  // No additional work needed here.
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

  // target is a process-drawable.  We try two data sources:
  // 1. *lib-jak-state-data* — a GOAL-allocated struct with rich state (if GOAL bridge code exists)
  // 2. Direct memory read — root (offset 108) -> trans (offset 12), using known struct offsets

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
  // process-drawable.root is a GOAL pointer at offset 108.
  // root (trsqv).trans is a vector at offset 12 within root.
  // Each component is a float in GOAL internal units (4096 = 1 meter).
  constexpr float METERS_TO_UNITS = 50.0f / 4096.0f;
  u32 root_ptr = *(u32*)(g_ee_main_mem + target_ptr + 108);

  if (root_ptr == 0 || root_ptr == s7.offset) {
    return;
  }

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

/**
 * Read bone transforms from *target*'s draw->skeleton->bones[] in GOAL memory.
 *
 * GOAL POINTER CONVENTION:
 *   In OpenGOAL, GOAL pointers to basic types point PAST the 4-byte type tag.
 *   So to access a field at all-types offset X, use: goal_ptr + (X - 4).
 *   Structures (non-basic) do NOT have this adjustment.
 *
 * Memory layout with -4 adjustment for basic types:
 *   process-drawable (basic):
 *     all-types: root=112, node-list=116, draw=120, skel=124
 *     actual:    root@+108, node-list@+112, draw@+116, skel@+120
 *
 *   draw-control (basic):
 *     all-types: skeleton=24
 *     actual:    skeleton@+20
 *
 *   skeleton (inline-array-class, basic):
 *     all-types: length=4, data=16
 *     actual:    length@+0, data@+12
 *
 *   bone (structure, NO -4):
 *     transform (matrix 4x4) at +0 (64 bytes)
 *     scale (vector) at +64 (16 bytes)
 *     cache at +80 (16 bytes)
 *     Total: 96 bytes per bone
 */
static void read_bones_from_goal() {
  if (!g_ee_main_mem)
    return;

  auto target_sym = jak1::intern_from_c("*target*");
  if (!target_sym.offset || target_sym->value == 0 || target_sym->value == s7.offset)
    return;

  u32 target_ptr = target_sym->value;

  // draw-control at target+116 (all-types offset 120, -4 for basic)
  u32 draw_ptr = *(u32*)(g_ee_main_mem + target_ptr + 116);
  if (draw_ptr == 0 || draw_ptr == s7.offset || draw_ptr >= 0x8000000)
    return;

  // skeleton at draw+20 (all-types offset 24, -4 for basic)
  u32 skel_ptr = *(u32*)(g_ee_main_mem + draw_ptr + 20);
  if (skel_ptr == 0 || skel_ptr == s7.offset || skel_ptr >= 0x8000000)
    return;

  // skeleton length at skel+0 (all-types offset 4, -4 for basic)
  u32 num_bones = *(u32*)(g_ee_main_mem + skel_ptr + 0);
  if (num_bones == 0 || num_bones > 256) {
    static bool warned = false;
    if (!warned) {
      lg::warn("[libjakopengoal] bad bone count {} from skel 0x{:x}", num_bones, skel_ptr);
      warned = true;
    }
    return;
  }

  // One-time diagnostic dump
  {
    static bool probed = false;
    if (!probed) {
      lg::info("[libjakopengoal] draw=0x{:x} (from target+116), skel=0x{:x} (from draw+20), "
               "num_bones={}",
               draw_ptr, skel_ptr, num_bones);
      probed = true;
    }
  }

  // Cap to FR3 model's max_bones
  {
    std::lock_guard<std::mutex> fr3_lock(s_fr3_mutex);
    if (s_fr3_model.loaded && s_fr3_model.max_bones > 0 && num_bones > s_fr3_model.max_bones) {
      num_bones = s_fr3_model.max_bones;
    }
  }

  std::lock_guard<std::mutex> lock(s_bone_mutex);
  s_bone_matrices.resize(num_bones);

  // Read bone transforms from skeleton's inline array
  // Bones start at skel+12 (all-types data offset 16, -4 for basic)
  constexpr u32 BONE_SIZE = 96;  // transform(64) + scale(16) + cache(16)
  constexpr u32 BONES_DATA_OFFSET = 12;  // all-types 16 - 4

  u32 valid_bones = 0;
  for (u32 i = 0; i < num_bones; i++) {
    u32 bone_addr = skel_ptr + BONES_DATA_OFFSET + i * BONE_SIZE;

    // bone.transform is a 4x4 matrix at offset 0 within bone (64 bytes)
    float* src_mat = (float*)(g_ee_main_mem + bone_addr);

    // Copy the 4x4 transform matrix
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 4; c++) {
        s_bone_matrices[i].tmat[r][c] = src_mat[r * 4 + c];
      }
    }

    // Compute normal matrix as inverse-transpose of upper 3x3
    float* m = &s_bone_matrices[i].tmat[0][0];
    float det = m[0] * (m[5] * m[10] - m[6] * m[9]) -
                m[1] * (m[4] * m[10] - m[6] * m[8]) +
                m[2] * (m[4] * m[9] - m[5] * m[8]);

    if (std::fabs(det) > 0.0001f) {
      float inv_det = 1.0f / det;
      s_bone_matrices[i].nmat[0][0] = (m[5] * m[10] - m[6] * m[9]) * inv_det;
      s_bone_matrices[i].nmat[0][1] = (m[2] * m[9] - m[1] * m[10]) * inv_det;
      s_bone_matrices[i].nmat[0][2] = (m[1] * m[6] - m[2] * m[5]) * inv_det;
      s_bone_matrices[i].nmat[1][0] = (m[6] * m[8] - m[4] * m[10]) * inv_det;
      s_bone_matrices[i].nmat[1][1] = (m[0] * m[10] - m[2] * m[8]) * inv_det;
      s_bone_matrices[i].nmat[1][2] = (m[2] * m[4] - m[0] * m[6]) * inv_det;
      s_bone_matrices[i].nmat[2][0] = (m[4] * m[9] - m[5] * m[8]) * inv_det;
      s_bone_matrices[i].nmat[2][1] = (m[1] * m[8] - m[0] * m[9]) * inv_det;
      s_bone_matrices[i].nmat[2][2] = (m[0] * m[5] - m[1] * m[4]) * inv_det;
    } else {
      // Fallback: identity normal matrix
      s_bone_matrices[i].nmat[0][0] = 1; s_bone_matrices[i].nmat[0][1] = 0; s_bone_matrices[i].nmat[0][2] = 0;
      s_bone_matrices[i].nmat[1][0] = 0; s_bone_matrices[i].nmat[1][1] = 1; s_bone_matrices[i].nmat[1][2] = 0;
      s_bone_matrices[i].nmat[2][0] = 0; s_bone_matrices[i].nmat[2][1] = 0; s_bone_matrices[i].nmat[2][2] = 1;
    }
    valid_bones++;
  }

  {
    static int bone_log_count = 0;
    bone_log_count++;
    // Log first 5 reads, then every 60th
    if (bone_log_count <= 5 || bone_log_count % 60 == 0) {
      lg::info("[libjakopengoal] Bone read #{}: skel=0x{:x}, {} bones, {} valid", bone_log_count,
               skel_ptr, num_bones, valid_bones);
      // Print bone[0] and bone[3] transforms (bone[3] is often the main body bone)
      for (u32 bi : {0u, 1u, 3u}) {
        if (bi >= num_bones)
          break;
        lg::info("[libjakopengoal]   bone[{}] row0=({:.3f}, {:.3f}, {:.3f}, {:.3f})", bi,
                 s_bone_matrices[bi].tmat[0][0], s_bone_matrices[bi].tmat[0][1],
                 s_bone_matrices[bi].tmat[0][2], s_bone_matrices[bi].tmat[0][3]);
        lg::info("[libjakopengoal]   bone[{}] row3(trans)=({:.3f}, {:.3f}, {:.3f}, {:.3f})", bi,
                 s_bone_matrices[bi].tmat[3][0], s_bone_matrices[bi].tmat[3][1],
                 s_bone_matrices[bi].tmat[3][2], s_bone_matrices[bi].tmat[3][3]);
      }
    }
  }
  // Only mark bones valid once we have non-zero transforms
  // (skeleton is allocated with zeros, gets filled by do-joint-math!)
  if (!s_bones_valid && num_bones >= 4) {
    // Check bone[3] (main body bone) — if it has a non-zero transform, bones are ready
    float check = std::fabs(s_bone_matrices[3].tmat[0][0]) +
                  std::fabs(s_bone_matrices[3].tmat[1][1]) +
                  std::fabs(s_bone_matrices[3].tmat[2][2]);
    if (check > 0.01f) {
      s_bones_valid = true;
      lg::info("[libjakopengoal] Bones now valid! bone[3] diag=({:.3f}, {:.3f}, {:.3f})",
               s_bone_matrices[3].tmat[0][0], s_bone_matrices[3].tmat[1][1],
               s_bone_matrices[3].tmat[2][2]);
    }
  }

  // Populate bone debug data for skeleton visualization
  if (s_bones_valid) {
    constexpr float METERS_TO_UNITS = 50.0f / 4096.0f;
    std::lock_guard<std::mutex> dbg_lock(s_bone_debug.mutex);
    s_bone_debug.num_bones = (int)num_bones;

    for (u32 i = 0; i < num_bones && i < 256; i++) {
      // Position is the translation row of the bone matrix (row 3)
      s_bone_debug.positions[i][0] = s_bone_matrices[i].tmat[3][0] * METERS_TO_UNITS;
      s_bone_debug.positions[i][1] = s_bone_matrices[i].tmat[3][1] * METERS_TO_UNITS;
      s_bone_debug.positions[i][2] = s_bone_matrices[i].tmat[3][2] * METERS_TO_UNITS;
      s_bone_debug.parent_indices[i] = -1;  // will be filled from cspace below
    }

    // Read parent indices from cspace-array (node-list)
    // process-drawable: node-list at target+112 (all-types 116, -4 for basic)
    u32 nodelist_ptr = *(u32*)(g_ee_main_mem + target_ptr + 112);
    if (nodelist_ptr != 0 && nodelist_ptr != s7.offset && nodelist_ptr < 0x8000000) {
      // cspace-array (inline-array-class, basic):
      //   length at +0 (all-types 4, -4)
      //   data starts at +12 (all-types 16, -4)
      //   each cspace is 32 bytes
      u32 cs_count = *(u32*)(g_ee_main_mem + nodelist_ptr + 0);
      constexpr u32 CSPACE_SIZE = 32;
      constexpr u32 CSPACE_DATA_OFFSET = 12;

      if (cs_count > 0 && cs_count <= 256) {
        // Build a map: cspace address -> index
        for (u32 i = 0; i < cs_count && i < num_bones; i++) {
          u32 cs_addr = nodelist_ptr + CSPACE_DATA_OFFSET + i * CSPACE_SIZE;
          // parent is at offset 0 of cspace (it's a structure, no -4 adjustment)
          u32 parent_ptr = *(u32*)(g_ee_main_mem + cs_addr + 0);
          if (parent_ptr != 0 && parent_ptr != s7.offset && parent_ptr < 0x8000000) {
            // Find parent index by computing offset from data start
            if (parent_ptr >= nodelist_ptr + CSPACE_DATA_OFFSET) {
              u32 parent_idx = (parent_ptr - (nodelist_ptr + CSPACE_DATA_OFFSET)) / CSPACE_SIZE;
              if (parent_idx < num_bones) {
                s_bone_debug.parent_indices[i] = (int)parent_idx;
              }
            }
          }
        }
      }
    }

    if (!s_bone_debug.valid) {
      // One-time log of skeleton hierarchy
      int lines_with_parent = 0;
      for (int i = 0; i < (int)num_bones && i < 256; i++) {
        if (s_bone_debug.parent_indices[i] >= 0) lines_with_parent++;
      }
      lg::info("[libjakopengoal] Skeleton debug: {} bones, {} with parents", num_bones, lines_with_parent);
      // Print first 20 bone parent mappings
      for (int i = 0; i < (int)num_bones && i < 20; i++) {
        lg::info("[libjakopengoal]   bone[{}]: parent={} pos=({:.1f}, {:.1f}, {:.1f})",
                 i, s_bone_debug.parent_indices[i],
                 s_bone_debug.positions[i][0], s_bone_debug.positions[i][1], s_bone_debug.positions[i][2]);
      }
    }
    s_bone_debug.valid = true;
  }
}

void extract_jak_mesh() {
  // Use FR3 model data + bone matrices for CPU skinning
  std::lock_guard<std::mutex> fr3_lock(s_fr3_mutex);
  if (!s_fr3_model.loaded) {
    return;
  }

  std::lock_guard<std::mutex> bone_lock(s_bone_mutex);
  // If real bones aren't available yet, use identity matrices (bind/T-pose)
  bool using_identity_bones = false;
  if (!s_bones_valid || s_bone_matrices.empty()) {
    if (s_fr3_model.max_bones == 0)
      return;
    s_bone_matrices.resize(s_fr3_model.max_bones);
    for (u32 i = 0; i < s_fr3_model.max_bones; i++) {
      // Identity 4x4 transform
      for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
          s_bone_matrices[i].tmat[r][c] = (r == c) ? 1.0f : 0.0f;
      // Identity 3x3 normal matrix
      for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
          s_bone_matrices[i].nmat[r][c] = (r == c) ? 1.0f : 0.0f;
    }
    using_identity_bones = true;
  }

  // Count total output vertices from all draws
  uint32_t total_verts = 0;
  for (const auto& draw : s_fr3_model.draws) {
    total_verts += draw.index_count;
  }
  if (total_verts == 0 || total_verts > JAK_GEO_MAX_TRIANGLES * 3) {
    return;
  }

  // The FR3 vertex positions are pre-scaled by xyz_scale and need to be
  // transformed by bone matrices (which are in GOAL internal units).
  // After skinning, convert from GOAL units to render units.
  constexpr float METERS_TO_UNITS = 50.0f / 4096.0f;

  std::lock_guard<std::mutex> lock(s_mesh_state.mutex);
  s_mesh_state.positions.resize(total_verts * 3);
  s_mesh_state.normals.resize(total_verts * 3);
  s_mesh_state.colors.resize(total_verts * 3);  // RGB only (matches rendering VBO layout)
  s_mesh_state.uvs.resize(total_verts * 2);

  uint32_t out_idx = 0;
  for (const auto& draw : s_fr3_model.draws) {
    for (uint32_t j = 0; j < draw.index_count; j++) {
      u32 vi = s_fr3_model.indices[draw.first_index + j];
      if (vi >= s_fr3_model.vertices.size()) {
        memset(&s_mesh_state.positions[out_idx * 3], 0, 3 * sizeof(float));
        memset(&s_mesh_state.normals[out_idx * 3], 0, 3 * sizeof(float));
        memset(&s_mesh_state.colors[out_idx * 3], 0, 3 * sizeof(float));
        memset(&s_mesh_state.uvs[out_idx * 2], 0, 2 * sizeof(float));
        out_idx++;
        continue;
      }

      const auto& v = s_fr3_model.vertices[vi];

      // The vertex position in the FR3 is in bone-local space, pre-multiplied by xyz_scale.
      // We need to undo the scale, apply bone transform, then scale to render units.
      float local_pos[3] = {v.pos[0] * s_fr3_model.xyz_scale,
                            v.pos[1] * s_fr3_model.xyz_scale,
                            v.pos[2] * s_fr3_model.xyz_scale};

      float skinned_pos[3], skinned_nrm[3];
      skin_vertex(local_pos, v.normal, v.mats, v.weights, s_bone_matrices, skinned_pos,
                  skinned_nrm);

      // Convert from GOAL internal units to render units
      s_mesh_state.positions[out_idx * 3 + 0] = skinned_pos[0] * METERS_TO_UNITS;
      s_mesh_state.positions[out_idx * 3 + 1] = skinned_pos[1] * METERS_TO_UNITS;
      s_mesh_state.positions[out_idx * 3 + 2] = skinned_pos[2] * METERS_TO_UNITS;

      s_mesh_state.normals[out_idx * 3 + 0] = skinned_nrm[0];
      s_mesh_state.normals[out_idx * 3 + 1] = skinned_nrm[1];
      s_mesh_state.normals[out_idx * 3 + 2] = skinned_nrm[2];

      s_mesh_state.colors[out_idx * 3 + 0] = v.rgba[0] / 255.0f;
      s_mesh_state.colors[out_idx * 3 + 1] = v.rgba[1] / 255.0f;
      s_mesh_state.colors[out_idx * 3 + 2] = v.rgba[2] / 255.0f;

      s_mesh_state.uvs[out_idx * 2 + 0] = v.st[0];
      s_mesh_state.uvs[out_idx * 2 + 1] = v.st[1];

      out_idx++;
    }
  }

  s_mesh_state.num_triangles = out_idx / 3;
  if (!s_mesh_state.valid && s_mesh_state.num_triangles > 0) {
    lg::info("[libjakopengoal] First mesh extraction: {} triangles, {} bones ({})",
             s_mesh_state.num_triangles, s_bone_matrices.size(),
             using_identity_bones ? "IDENTITY/T-pose" : "from GOAL");
  }
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
 * Write external camera yaw into *math-camera*'s inv-camera-rot (offset 432)
 * and inv-camera-rot-smooth (offset 496) so stick input is relative to the
 * external camera.  GOAL's read-pad calls (matrix-local->world #t #f) which
 * returns inv-camera-rot-smooth to rotate the stick vector into world space.
 *
 * Matrix is column-major Ry(yaw):
 *   col0 = ( cos, 0, sin, 0)   col1 = (0, 1, 0, 0)
 *   col2 = (-sin, 0, cos, 0)   col3 = (0, 0, 0, 1)
 */
static void inject_camera_rotation() {
  if (!g_ee_main_mem)
    return;

  float cam_x, cam_z;
  {
    std::lock_guard<std::mutex> lock(s_pad_state.mutex);
    if (!s_pad_state.has_input)
      return;
    cam_x = s_pad_state.current_inputs.cam_x;
    cam_z = s_pad_state.current_inputs.cam_z;
  }

  // Normalize the look direction to get cos/sin of camera yaw
  float len = sqrtf(cam_x * cam_x + cam_z * cam_z);
  if (len < 0.001f)
    return;
  float cos_y = cam_z / len;
  float sin_y = cam_x / len;

  auto mc_sym = jak1::intern_from_c("*math-camera*");
  if (!mc_sym.offset || mc_sym->value == 0 || mc_sym->value == s7.offset)
    return;

  u32 mc_ptr = mc_sym->value;

  // Write Y-rotation matrix to both inv-camera-rot and inv-camera-rot-smooth
  for (int offset : {432, 496}) {
    float* m = (float*)(g_ee_main_mem + mc_ptr + offset);
    m[0] = cos_y;   m[1] = 0.0f;  m[2] = sin_y;   m[3] = 0.0f;   // col 0 (right)
    m[4] = 0.0f;    m[5] = 1.0f;  m[6] = 0.0f;     m[7] = 0.0f;   // col 1 (up)
    m[8] = -sin_y;  m[9] = 0.0f;  m[10] = cos_y;   m[11] = 0.0f;  // col 2 (forward)
    m[12] = 0.0f;   m[13] = 0.0f; m[14] = 0.0f;    m[15] = 1.0f;  // col 3 (translation)
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

  // Load Jak's merc model from FR3 data for CPU-side mesh skinning.
  // Try GAME.fr3 first (common data), then village1 as fallback.
  if (!load_jak_model_from_fr3("GAME")) {
    if (!load_jak_model_from_fr3("village1")) {
      lg::warn("[libjakopengoal] Could not load eichar-lod0 model from any FR3 file");
    }
  }

  lg::info("[libjakopengoal] Bridge initialized.");

  // Auto-set runtime ready since GOAL scripts are loaded and bridge is active.
  // The GOAL side can also call lib-jak-notify-target-ready explicitly if needed.
  set_runtime_ready(true);
  lg::info("[libjakopengoal] Runtime marked as ready.");
}

/**
 * Force GOAL settings that conflict with library mode every frame.
 * - border-mode must be #f or Jak gets killed for leaving the level boundary.
 *
 * *setting-control* is a setting-control (basic) with:
 *   default (setting-data :inline :offset 432)
 *     border-mode (symbol :offset 0 within setting-data)
 * So default.border-mode is at offset 432 from the setting-control object.
 * #f in GOAL = s7.offset (the false symbol).
 */
static void force_settings() {
  if (!g_ee_main_mem)
    return;

  auto sc_sym = jak1::intern_from_c("*setting-control*");
  if (!sc_sym.offset || sc_sym->value == 0 || sc_sym->value == s7.offset)
    return;

  u32 sc_ptr = sc_sym->value;

  // default.border-mode at offset 432 — force to #f
  u32* border_mode = (u32*)(g_ee_main_mem + sc_ptr + 432);
  *border_mode = s7.offset;
}

void bridge_tick() {
  process_commands();
  force_settings();
  inject_camera_rotation();
  extract_jak_state();
  read_bones_from_goal();
  extract_jak_mesh();
}

}  // namespace jak_bridge
