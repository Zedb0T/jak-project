/**
 * jak_bridge.cpp - Bridge between the C API and the GOAL runtime.
 */

#define _USE_MATH_DEFINES
#include <cmath>

#include "jak_bridge.h"

#include <algorithm>
#include <cstring>
#include <set>

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
#include "game/sce/iop.h"
#include "game/sound/sndshim.h"

namespace jak_bridge {

/* -------------------------------------------------------------------------- */
/*  Singleton state accessors                                                 */
/* -------------------------------------------------------------------------- */

// State singletons are defined in jak_bridge_state.cpp (separate TU to keep
// this heavy .obj from being pulled in just for state accessors).
extern CollisionState s_collision_state;
extern PadState s_pad_state;
extern JakInternalState s_jak_state;
extern MeshState s_mesh_state;
extern CommandQueue s_command_queue;
extern std::atomic<bool> s_runtime_ready;
extern BoneDebugData s_bone_debug;
extern TextureAtlasInfo s_texture_atlas_info;
extern uint8_t* s_texture_output;

// Bone matrix storage for CPU skinning
struct BoneMatrix {
  float tmat[4][4];  // 4x4 transform matrix
  float nmat[3][3];  // 3x3 normal matrix
};
static std::mutex s_bone_mutex;
static std::vector<BoneMatrix> s_bone_matrices;
static bool s_bones_valid = false;

// Joint bind-pose matrices (read once from art-joint-geo)
// These transform model-space vertices into bone-local space.
// Skinning matrix = bone_world_transform * bind_pose
static std::vector<BoneMatrix> s_bind_pose_matrices;
static bool s_bind_poses_loaded = false;

// Atlas sub-texture rectangle (in pixel coordinates within the atlas)
struct AtlasRect {
  u32 x, y, w, h;
};

// FR3 model data for Jak's mesh (loaded once at init)
struct Fr3ModelData {
  std::vector<tfrag3::MercVertex> vertices;  // base-pose vertices (indexed by indices)
  std::vector<u32> indices;                  // triangle indices
  // Per-effect draw ranges so we can iterate all triangles
  struct DrawRange {
    u32 first_index;
    u32 index_count;
    bool no_strip;  // true = triangle list, false = triangle strip with UINT32_MAX restart
    s32 tree_tex_id;  // index into level.textures for this draw's texture
    u8 eye_id;        // 0xff = not eye, otherwise (slot<<1)|is_right
  };
  std::vector<DrawRange> draws;
  float xyz_scale = 1.0f;
  u32 max_bones = 0;
  bool loaded = false;

  // Texture atlas data
  std::vector<AtlasRect> atlas_rects;  // per-texture atlas placement (indexed by tree_tex_id)
  u32 atlas_width = 0;
  u32 atlas_height = 0;
  bool textures_loaded = false;

  // Eye texture: index into atlas_rects for the iris texture used for eye draws
  s32 eye_tex_id = -1;  // -1 = no eye texture found
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
      s_fr3_model.draws.push_back({draw.first_index, draw.index_count, draw.no_strip, draw.tree_tex_id, draw.eye_id});
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

  // Build texture atlas from FR3 level textures
  // Find the iris texture for eye rendering
  s32 iris_tex_id = -1;
  for (u32 ti = 0; ti < level.textures.size(); ti++) {
    if (level.textures[ti].debug_name == "bam-iris-16x16" &&
        level.textures[ti].debug_tpage_name == "eichar") {
      iris_tex_id = (s32)ti;
      break;
    }
  }
  if (iris_tex_id < 0) {
    // Fallback: try any iris texture
    for (u32 ti = 0; ti < level.textures.size(); ti++) {
      if (level.textures[ti].debug_name.find("iris") != std::string::npos) {
        iris_tex_id = (s32)ti;
        break;
      }
    }
  }
  s_fr3_model.eye_tex_id = iris_tex_id;
  if (iris_tex_id >= 0) {
    lg::info("[libjakopengoal] Using texture [{}] '{}' {}x{} for static eye rendering",
             iris_tex_id, level.textures[iris_tex_id].debug_name,
             level.textures[iris_tex_id].w, level.textures[iris_tex_id].h);
  }

  // Collect unique texture IDs used by draws
  std::set<s32> used_tex_ids;
  for (const auto& d : s_fr3_model.draws) {
    if (d.tree_tex_id >= 0 && (u32)d.tree_tex_id < level.textures.size()) {
      used_tex_ids.insert(d.tree_tex_id);
    }
  }
  // Also include the iris texture in the atlas
  if (iris_tex_id >= 0) {
    used_tex_ids.insert(iris_tex_id);
  }

  lg::info("[libjakopengoal] {} unique textures referenced by draws, {} total in level",
           used_tex_ids.size(), level.textures.size());

  // Pack textures into the atlas using a simple shelf-packing algorithm
  u32 atlas_w = JAK_TEXTURE_WIDTH;
  u32 atlas_h = JAK_TEXTURE_HEIGHT;
  s_fr3_model.atlas_width = atlas_w;
  s_fr3_model.atlas_height = atlas_h;

  // Allocate atlas rects for all textures (even unused ones get empty rects)
  s_fr3_model.atlas_rects.resize(level.textures.size());

  // Shelf packing: place textures left-to-right, top-to-bottom
  u32 shelf_x = 0, shelf_y = 0, shelf_h = 0;
  std::vector<u32> atlas_pixels(atlas_w * atlas_h, 0);  // RGBA8

  for (s32 tex_id : used_tex_ids) {
    const auto& tex = level.textures[tex_id];
    if (tex.w == 0 || tex.h == 0 || tex.data.empty()) {
      lg::warn("[libjakopengoal] Texture {} ({}) has no data ({}x{})",
               tex_id, tex.debug_name, tex.w, tex.h);
      continue;
    }

    // Check if texture fits on current shelf
    if (shelf_x + tex.w > atlas_w) {
      // Move to next shelf
      shelf_y += shelf_h;
      shelf_x = 0;
      shelf_h = 0;
    }

    if (shelf_y + tex.h > atlas_h) {
      lg::warn("[libjakopengoal] Atlas overflow! Cannot fit texture {} ({}x{}) at y={}",
               tex_id, tex.w, tex.h, shelf_y);
      continue;
    }

    // Place texture
    s_fr3_model.atlas_rects[tex_id] = {shelf_x, shelf_y, tex.w, tex.h};

    // Copy pixel data into atlas
    for (u32 row = 0; row < tex.h; row++) {
      for (u32 col = 0; col < tex.w; col++) {
        u32 src_idx = row * tex.w + col;
        u32 dst_idx = (shelf_y + row) * atlas_w + (shelf_x + col);
        if (src_idx < tex.data.size() && dst_idx < atlas_pixels.size()) {
          atlas_pixels[dst_idx] = tex.data[src_idx];
        }
      }
    }

    lg::info("[libjakopengoal]   tex[{}] '{}' {}x{} -> atlas ({},{})-({}x{})",
             tex_id, tex.debug_name, tex.w, tex.h,
             shelf_x, shelf_y, tex.w, tex.h);

    shelf_x += tex.w;
    if (tex.h > shelf_h) shelf_h = tex.h;
  }

  // Write atlas to the output texture buffer if provided
  if (s_texture_output) {
    memcpy(s_texture_output, atlas_pixels.data(), atlas_w * atlas_h * 4);
    lg::info("[libjakopengoal] Wrote {}x{} texture atlas to output buffer ({} bytes)",
             atlas_w, atlas_h, atlas_w * atlas_h * 4);
  }

  s_fr3_model.textures_loaded = !used_tex_ids.empty();

  // Update texture atlas info
  {
    auto& tai = get_texture_atlas_info();
    tai.atlas_width = atlas_w;
    tai.atlas_height = atlas_h;
    tai.num_textures = (u32)used_tex_ids.size();
    tai.valid = s_fr3_model.textures_loaded;
  }

  return true;
}

// State accessors are defined in jak_bridge_state.cpp

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

/**
 * Multiply two 4x4 matrices stored as float[4][4] in GOAL column-major order.
 * result = A * B, where tmat[col][row] is the convention.
 */
static void mat4_mul_goal(float out[4][4], const float a[4][4], const float b[4][4]) {
  // out_col[j] = A * b_col[j]
  // out[j][r] = sum_c(a[c][r] * b[j][c]) for the 3x3 rotation part
  for (int j = 0; j < 4; j++) {
    for (int r = 0; r < 4; r++) {
      float v = 0.0f;
      for (int c = 0; c < 4; c++) {
        v += a[c][r] * b[j][c];
      }
      out[j][r] = v;
    }
  }
}

static void skin_vertex(const float* pos_in,
                         const float* nrm_in,
                         const uint8_t* mats,
                         const float* weights,
                         const std::vector<BoneMatrix>& skinning_matrices,
                         float* pos_out,
                         float* nrm_out) {
  pos_out[0] = pos_out[1] = pos_out[2] = 0.0f;
  nrm_out[0] = nrm_out[1] = nrm_out[2] = 0.0f;

  for (int b = 0; b < 3; b++) {
    float w = weights[b];
    if (w <= 0.0f)
      continue;

    uint8_t bone_idx = mats[b];
    if (bone_idx >= skinning_matrices.size())
      continue;

    const auto& skin = skinning_matrices[bone_idx];

    // Transform position: result[r] = sum_c(tmat[c][r] * pos[c]) + tmat[3][r]
    // tmat is stored as tmat[col][row] in GOAL column-major convention.
    for (int r = 0; r < 3; r++) {
      float v = skin.tmat[3][r];  // translation column
      for (int c = 0; c < 3; c++) {
        v += skin.tmat[c][r] * pos_in[c];
      }
      pos_out[r] += w * v;
    }

    // Transform normal: result[r] = sum_c(nmat[c][r] * n[c])
    for (int r = 0; r < 3; r++) {
      float v = 0.0f;
      for (int c = 0; c < 3; c++) {
        v += skin.nmat[c][r] * nrm_in[c];
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

  // Cap to FR3 model's max_bones (+1 because max_bones is the highest index used)
  {
    std::lock_guard<std::mutex> fr3_lock(s_fr3_mutex);
    if (s_fr3_model.loaded && s_fr3_model.max_bones > 0 &&
        num_bones > s_fr3_model.max_bones + 1) {
      num_bones = s_fr3_model.max_bones + 1;
    }
  }

  // Read joint bind-pose matrices once from art-joint-geo
  // These are needed for proper skinning: skinning_mat = bone_world * bind_pose
  if (!s_bind_poses_loaded) {
    // draw-control.jgeo (art-joint-geo) at all-types offset 12, -4 for basic = 8
    u32 jgeo_ptr = *(u32*)(g_ee_main_mem + draw_ptr + 8);
    if (jgeo_ptr != 0 && jgeo_ptr != s7.offset && jgeo_ptr < 0x8000000) {
      // art.length at all-types offset 12, -4 for basic = 8
      u32 num_joints = *(u32*)(g_ee_main_mem + jgeo_ptr + 8);
      // art-joint-geo.data[0] at all-types offset 32, -4 for basic = 28
      // data is a dynamic array of joint pointers; data[0] = first joint pointer
      u32 first_joint_ptr = *(u32*)(g_ee_main_mem + jgeo_ptr + 28);

      if (num_joints > 0 && num_joints <= 256 && first_joint_ptr != 0 &&
          first_joint_ptr != s7.offset && first_joint_ptr < 0x8000000) {
        // Joints are stored as an inline array starting at first_joint_ptr.
        // Each joint is 0x50 (80) bytes (basic type).
        // bind-pose (matrix 4x4) is at all-types offset 16, -4 for basic = 12.
        constexpr u32 JOINT_SIZE = 0x50;   // 80 bytes per joint
        constexpr u32 BIND_POSE_OFFSET = 12;  // all-types 16 - 4

        s_bind_pose_matrices.resize(num_joints);
        for (u32 i = 0; i < num_joints; i++) {
          u32 joint_addr = first_joint_ptr + i * JOINT_SIZE;
          float* bp_mat = (float*)(g_ee_main_mem + joint_addr + BIND_POSE_OFFSET);
          // Copy bind-pose 4x4 matrix (same layout as bone transform)
          for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
              s_bind_pose_matrices[i].tmat[r][c] = bp_mat[r * 4 + c];
            }
          }
          // Normal matrix not needed for bind pose (computed from combined skinning matrix)
        }

        s_bind_poses_loaded = true;
        lg::info("[libjakopengoal] Loaded {} joint bind-pose matrices from jgeo=0x{:x}",
                 num_joints, jgeo_ptr);

        // Log first bind-pose for diagnostics
        if (num_joints > 0) {
          lg::info("[libjakopengoal]   bind_pose[0] col0=({:.3f}, {:.3f}, {:.3f}, {:.3f})",
                   s_bind_pose_matrices[0].tmat[0][0], s_bind_pose_matrices[0].tmat[0][1],
                   s_bind_pose_matrices[0].tmat[0][2], s_bind_pose_matrices[0].tmat[0][3]);
          lg::info("[libjakopengoal]   bind_pose[0] col3=({:.3f}, {:.3f}, {:.3f}, {:.3f})",
                   s_bind_pose_matrices[0].tmat[3][0], s_bind_pose_matrices[0].tmat[3][1],
                   s_bind_pose_matrices[0].tmat[3][2], s_bind_pose_matrices[0].tmat[3][3]);
        }
      }
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
    s_bone_matrices.resize(s_fr3_model.max_bones + 1);
    for (u32 i = 0; i <= s_fr3_model.max_bones; i++) {
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

  // Compute skinning matrices: skinning[b] = bone_transform[b] * bind_pose[b-1]
  // This matches GOAL's new-bones-mtx-calc-asm:
  //   output[i+1] = bone[i+1].transform * joints[i].bind_pose
  // So for bone index b: skinning[b] = bone[b] * bind_pose[b-1]
  u32 num_skinning = (u32)s_bone_matrices.size();
  std::vector<BoneMatrix> skinning_matrices(num_skinning);

  if (s_bind_poses_loaded && !using_identity_bones) {
    // Bone 0: use raw bone transform (no bind pose for bone 0)
    skinning_matrices[0] = s_bone_matrices[0];
    // Compute combined skinning for bones 1+
    for (u32 b = 1; b < num_skinning; b++) {
      u32 joint_idx = b - 1;  // bone[b] pairs with joint[b-1]
      if (joint_idx < s_bind_pose_matrices.size()) {
        // skinning = bone_world * bind_pose (column-major multiply)
        mat4_mul_goal(skinning_matrices[b].tmat, s_bone_matrices[b].tmat,
                      s_bind_pose_matrices[joint_idx].tmat);
      } else {
        skinning_matrices[b] = s_bone_matrices[b];
      }

      // Compute normal matrix as inverse-transpose of upper 3x3
      float* m = &skinning_matrices[b].tmat[0][0];
      float det = m[0] * (m[5] * m[10] - m[6] * m[9]) -
                  m[1] * (m[4] * m[10] - m[6] * m[8]) +
                  m[2] * (m[4] * m[9] - m[5] * m[8]);
      if (std::fabs(det) > 0.0001f) {
        float inv_det = 1.0f / det;
        skinning_matrices[b].nmat[0][0] = (m[5] * m[10] - m[6] * m[9]) * inv_det;
        skinning_matrices[b].nmat[0][1] = (m[2] * m[9] - m[1] * m[10]) * inv_det;
        skinning_matrices[b].nmat[0][2] = (m[1] * m[6] - m[2] * m[5]) * inv_det;
        skinning_matrices[b].nmat[1][0] = (m[6] * m[8] - m[4] * m[10]) * inv_det;
        skinning_matrices[b].nmat[1][1] = (m[0] * m[10] - m[2] * m[8]) * inv_det;
        skinning_matrices[b].nmat[1][2] = (m[2] * m[4] - m[0] * m[6]) * inv_det;
        skinning_matrices[b].nmat[2][0] = (m[4] * m[9] - m[5] * m[8]) * inv_det;
        skinning_matrices[b].nmat[2][1] = (m[1] * m[8] - m[0] * m[9]) * inv_det;
        skinning_matrices[b].nmat[2][2] = (m[0] * m[5] - m[1] * m[4]) * inv_det;
      } else {
        for (int r = 0; r < 3; r++)
          for (int c = 0; c < 3; c++)
            skinning_matrices[b].nmat[r][c] = (r == c) ? 1.0f : 0.0f;
      }
    }
    // Also compute normal matrix for bone 0
    {
      float* m = &skinning_matrices[0].tmat[0][0];
      float det = m[0] * (m[5] * m[10] - m[6] * m[9]) -
                  m[1] * (m[4] * m[10] - m[6] * m[8]) +
                  m[2] * (m[4] * m[9] - m[5] * m[8]);
      if (std::fabs(det) > 0.0001f) {
        float inv_det = 1.0f / det;
        skinning_matrices[0].nmat[0][0] = (m[5] * m[10] - m[6] * m[9]) * inv_det;
        skinning_matrices[0].nmat[0][1] = (m[2] * m[9] - m[1] * m[10]) * inv_det;
        skinning_matrices[0].nmat[0][2] = (m[1] * m[6] - m[2] * m[5]) * inv_det;
        skinning_matrices[0].nmat[1][0] = (m[6] * m[8] - m[4] * m[10]) * inv_det;
        skinning_matrices[0].nmat[1][1] = (m[0] * m[10] - m[2] * m[8]) * inv_det;
        skinning_matrices[0].nmat[1][2] = (m[2] * m[4] - m[0] * m[6]) * inv_det;
        skinning_matrices[0].nmat[2][0] = (m[4] * m[9] - m[5] * m[8]) * inv_det;
        skinning_matrices[0].nmat[2][1] = (m[1] * m[8] - m[0] * m[9]) * inv_det;
        skinning_matrices[0].nmat[2][2] = (m[0] * m[5] - m[1] * m[4]) * inv_det;
      } else {
        for (int r = 0; r < 3; r++)
          for (int c = 0; c < 3; c++)
            skinning_matrices[0].nmat[r][c] = (r == c) ? 1.0f : 0.0f;
      }
    }
  } else {
    // No bind poses: use raw bone matrices as before
    skinning_matrices.assign(s_bone_matrices.begin(), s_bone_matrices.end());
  }

  // Convert triangle strips to triangle lists.
  // FR3 index buffers use UINT32_MAX as primitive restart markers for strips.
  // We need to expand strips into individual triangles.
  // Also track which draw each triangle came from (for UV atlas remapping).
  std::vector<u32> triangle_indices;
  std::vector<u32> triangle_draw_ids;  // one per triangle (not per vertex)
  triangle_indices.reserve(JAK_GEO_MAX_TRIANGLES * 3);
  triangle_draw_ids.reserve(JAK_GEO_MAX_TRIANGLES);

  for (u32 draw_idx = 0; draw_idx < s_fr3_model.draws.size(); draw_idx++) {
    const auto& draw = s_fr3_model.draws[draw_idx];
    if (draw.no_strip) {
      // Triangle list: indices are already grouped in triples, skip UINT32_MAX
      for (uint32_t j = 0; j + 2 < draw.index_count; j += 3) {
        u32 i0 = s_fr3_model.indices[draw.first_index + j + 0];
        u32 i1 = s_fr3_model.indices[draw.first_index + j + 1];
        u32 i2 = s_fr3_model.indices[draw.first_index + j + 2];
        if (i0 == UINT32_MAX || i1 == UINT32_MAX || i2 == UINT32_MAX) continue;
        triangle_indices.push_back(i0);
        triangle_indices.push_back(i1);
        triangle_indices.push_back(i2);
        triangle_draw_ids.push_back(draw_idx);
      }
    } else {
      // Triangle strip with primitive restart (UINT32_MAX)
      // Walk through indices, building strips and emitting triangles
      uint32_t strip_start = 0;
      for (uint32_t j = 0; j <= draw.index_count; j++) {
        bool is_restart = (j == draw.index_count) ||
                          (s_fr3_model.indices[draw.first_index + j] == UINT32_MAX);
        if (is_restart) {
          // Process strip from strip_start to j-1
          uint32_t strip_len = j - strip_start;
          if (strip_len >= 3) {
            for (uint32_t k = 0; k + 2 < strip_len; k++) {
              u32 idx0 = s_fr3_model.indices[draw.first_index + strip_start + k + 0];
              u32 idx1 = s_fr3_model.indices[draw.first_index + strip_start + k + 1];
              u32 idx2 = s_fr3_model.indices[draw.first_index + strip_start + k + 2];
              // Skip degenerate triangles
              if (idx0 == idx1 || idx1 == idx2 || idx0 == idx2) continue;
              // Flip winding for even/odd triangles in the strip
              if (k % 2 == 0) {
                triangle_indices.push_back(idx0);
                triangle_indices.push_back(idx1);
                triangle_indices.push_back(idx2);
              } else {
                triangle_indices.push_back(idx0);
                triangle_indices.push_back(idx2);
                triangle_indices.push_back(idx1);
              }
              triangle_draw_ids.push_back(draw_idx);
            }
          }
          strip_start = j + 1;
        }
      }
    }
  }

  uint32_t total_verts = (uint32_t)triangle_indices.size();
  if (total_verts == 0 || total_verts > JAK_GEO_MAX_TRIANGLES * 3) {
    return;
  }

  // One-time diagnostic: log bone usage and skinning matrix translations
  {
    static bool logged_bones = false;
    if (!logged_bones && !using_identity_bones && s_bind_poses_loaded) {
      logged_bones = true;
      // Count vertices per bone index
      u32 bone_usage[256] = {};
      for (u32 ti = 0; ti < total_verts; ti++) {
        u32 vi = triangle_indices[ti];
        if (vi < s_fr3_model.vertices.size()) {
          const auto& vtx = s_fr3_model.vertices[vi];
          for (int b = 0; b < 3; b++) {
            if (vtx.weights[b] > 0.0f) {
              bone_usage[vtx.mats[b]]++;
            }
          }
        }
      }
      lg::info("[libjakopengoal] Bone index usage in FR3 model:");
      for (u32 bi = 0; bi < 256; bi++) {
        if (bone_usage[bi] == 0)
          continue;
        if (bi < skinning_matrices.size()) {
          const auto& sm = skinning_matrices[bi];
          float tx = sm.tmat[3][0] * (50.0f / 4096.0f);
          float ty = sm.tmat[3][1] * (50.0f / 4096.0f);
          float tz = sm.tmat[3][2] * (50.0f / 4096.0f);
          // Check rotation column magnitudes
          float col0_len = std::sqrt(sm.tmat[0][0]*sm.tmat[0][0] + sm.tmat[0][1]*sm.tmat[0][1] + sm.tmat[0][2]*sm.tmat[0][2]);
          float col1_len = std::sqrt(sm.tmat[1][0]*sm.tmat[1][0] + sm.tmat[1][1]*sm.tmat[1][1] + sm.tmat[1][2]*sm.tmat[1][2]);
          float col2_len = std::sqrt(sm.tmat[2][0]*sm.tmat[2][0] + sm.tmat[2][1]*sm.tmat[2][1] + sm.tmat[2][2]*sm.tmat[2][2]);
          lg::info("  bone[{}]: {} verts, trans=({:.1f},{:.1f},{:.1f}) col_lens=({:.2f},{:.2f},{:.2f})",
                   bi, bone_usage[bi], tx, ty, tz, col0_len, col1_len, col2_len);
        } else {
          lg::info("  bone[{}]: {} verts, OUT OF RANGE (max={})", bi, bone_usage[bi],
                   skinning_matrices.size());
        }
      }
      // Log a few sample skinned vertex positions
      lg::info("[libjakopengoal] Sample skinned vertex positions:");
      int samples = 0;
      for (const auto& draw : s_fr3_model.draws) {
        for (uint32_t j = 0; j < draw.index_count && samples < 10; j += draw.index_count / 3) {
          u32 vi = s_fr3_model.indices[draw.first_index + j];
          if (vi >= s_fr3_model.vertices.size()) continue;
          const auto& vtx = s_fr3_model.vertices[vi];
          float pos[3] = {vtx.pos[0], vtx.pos[1], vtx.pos[2]};
          float spos[3] = {0,0,0}, snrm[3] = {0,0,0};
          skin_vertex(pos, vtx.normal, vtx.mats, vtx.weights, skinning_matrices, spos, snrm);
          float scale = 50.0f / 4096.0f;
          lg::info("  vtx[{}] local=({:.1f},{:.1f},{:.1f}) bone={} w={:.2f} -> skinned=({:.1f},{:.1f},{:.1f})",
                   vi, pos[0], pos[1], pos[2], vtx.mats[0], vtx.weights[0],
                   spos[0]*scale, spos[1]*scale, spos[2]*scale);
          samples++;
        }
      }
    }
  }

  // After skinning, convert from GOAL units to render units.
  constexpr float METERS_TO_UNITS = 50.0f / 4096.0f;

  std::lock_guard<std::mutex> lock(s_mesh_state.mutex);
  s_mesh_state.positions.resize(total_verts * 3);
  s_mesh_state.normals.resize(total_verts * 3);
  s_mesh_state.colors.resize(total_verts * 4);  // RGBA (matches JakGeometryBuffers layout)
  s_mesh_state.uvs.resize(total_verts * 2);

  uint32_t out_idx = 0;
  for (uint32_t ti = 0; ti < total_verts; ti++) {
    u32 vi = triangle_indices[ti];
    if (vi >= s_fr3_model.vertices.size()) {
      memset(&s_mesh_state.positions[out_idx * 3], 0, 3 * sizeof(float));
      memset(&s_mesh_state.normals[out_idx * 3], 0, 3 * sizeof(float));
      memset(&s_mesh_state.colors[out_idx * 4], 0, 4 * sizeof(float));
      memset(&s_mesh_state.uvs[out_idx * 2], 0, 2 * sizeof(float));
      out_idx++;
      continue;
    }

    const auto& v = s_fr3_model.vertices[vi];

    // FR3 vertex positions are already scaled by xyz_scale during extraction.
    // They are in model/bind-pose space, ready for skinning matrix transform.
    float local_pos[3] = {v.pos[0], v.pos[1], v.pos[2]};

    float skinned_pos[3], skinned_nrm[3];
    skin_vertex(local_pos, v.normal, v.mats, v.weights, skinning_matrices, skinned_pos,
                skinned_nrm);

    // Convert from GOAL internal units to render units
    s_mesh_state.positions[out_idx * 3 + 0] = skinned_pos[0] * METERS_TO_UNITS;
    s_mesh_state.positions[out_idx * 3 + 1] = skinned_pos[1] * METERS_TO_UNITS;
    s_mesh_state.positions[out_idx * 3 + 2] = skinned_pos[2] * METERS_TO_UNITS;

    s_mesh_state.normals[out_idx * 3 + 0] = skinned_nrm[0];
    s_mesh_state.normals[out_idx * 3 + 1] = skinned_nrm[1];
    s_mesh_state.normals[out_idx * 3 + 2] = skinned_nrm[2];

    s_mesh_state.colors[out_idx * 4 + 0] = v.rgba[0] / 255.0f;
    s_mesh_state.colors[out_idx * 4 + 1] = v.rgba[1] / 255.0f;
    s_mesh_state.colors[out_idx * 4 + 2] = v.rgba[2] / 255.0f;
    s_mesh_state.colors[out_idx * 4 + 3] = v.rgba[3] / 255.0f;

    // Remap UVs from per-texture space to atlas space
    {
      u32 tri_idx = ti / 3;
      float u = v.st[0];
      float v_coord = v.st[1];

      if (s_fr3_model.textures_loaded && tri_idx < triangle_draw_ids.size()) {
        u32 didx = triangle_draw_ids[tri_idx];
        if (didx < s_fr3_model.draws.size()) {
          const auto& draw = s_fr3_model.draws[didx];
          // For eye draws, use the iris texture instead of the placeholder
          s32 tex_id = draw.tree_tex_id;
          if (draw.eye_id != 0xff && s_fr3_model.eye_tex_id >= 0) {
            tex_id = s_fr3_model.eye_tex_id;
          }
          if (tex_id >= 0 && (u32)tex_id < s_fr3_model.atlas_rects.size()) {
            const auto& rect = s_fr3_model.atlas_rects[tex_id];
            if (rect.w > 0 && rect.h > 0) {
              // Wrap UVs to [0,1] range first (textures repeat)
              u = u - std::floor(u);
              v_coord = v_coord - std::floor(v_coord);
              // Remap to atlas sub-rect in normalized [0,1] atlas space
              float atlas_w = (float)s_fr3_model.atlas_width;
              float atlas_h = (float)s_fr3_model.atlas_height;
              u = ((float)rect.x + u * (float)rect.w) / atlas_w;
              v_coord = ((float)rect.y + v_coord * (float)rect.h) / atlas_h;
            }
          }
        }
      }

      s_mesh_state.uvs[out_idx * 2 + 0] = u;
      s_mesh_state.uvs[out_idx * 2 + 1] = v_coord;
    }

    out_idx++;
  }

  s_mesh_state.num_triangles = out_idx / 3;
  if (!s_mesh_state.valid && s_mesh_state.num_triangles > 0) {
    lg::info("[libjakopengoal] First mesh extraction: {} triangles, {} bones ({})",
             s_mesh_state.num_triangles, s_bone_matrices.size(),
             using_identity_bones ? "IDENTITY/T-pose" : "from GOAL");
    // Log eye draw vertex info
    if (s_fr3_model.eye_tex_id >= 0) {
      u32 eye_verts = 0;
      for (u32 i = 0; i < (u32)triangle_draw_ids.size(); i++) {
        u32 didx = triangle_draw_ids[i];
        if (didx < s_fr3_model.draws.size() && s_fr3_model.draws[didx].eye_id != 0xff) {
          eye_verts += 3;
        }
      }
      lg::info("[libjakopengoal] Eye triangles: {} ({} vertices), using iris tex [{}]",
               eye_verts / 3, eye_verts, s_fr3_model.eye_tex_id);
    }
    // Log sample UV values
    if (s_fr3_model.textures_loaded) {
      lg::info("[libjakopengoal] UV sample (first 10 verts, atlas-remapped):");
      for (u32 i = 0; i < 10 && i < out_idx; i++) {
        lg::info("  v[{}]: uv=({:.4f}, {:.4f})", i, s_mesh_state.uvs[i*2], s_mesh_state.uvs[i*2+1]);
      }
      // Count vertices with UVs in valid range [0,1]
      u32 in_range = 0;
      for (u32 i = 0; i < out_idx; i++) {
        float u = s_mesh_state.uvs[i*2], v = s_mesh_state.uvs[i*2+1];
        if (u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f) in_range++;
      }
      lg::info("[libjakopengoal] UV range: {}/{} verts in [0,1]", in_range, out_idx);
    }
  }
  s_mesh_state.valid = true;

  // One-time triangle count log after strip conversion
  {
    static bool logged_strip_stats = false;
    if (!logged_strip_stats && !using_identity_bones) {
      logged_strip_stats = true;
      lg::info("[libjakopengoal] Strip conversion: {} raw indices -> {} triangle verts ({} tris)",
               s_fr3_model.indices.size(), total_verts, total_verts / 3);
    }
  }
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

  // Direct memory approach: write to *target* root trans directly from C++.
  // The GOAL command pipeline (lib-jak-process-commands) is never called from the
  // game loop, so we bypass it entirely and poke GOAL memory the same way
  // extract_jak_state() reads it, but in reverse.
  //
  // *target* (process-drawable) -> root (offset 108, GOAL pointer to trsqv)
  //   -> trans (offset 12 within trsqv, vector of 4 floats: x y z w)
  //   -> transv (offset 28 within trsqv, velocity vector)

  auto target_sym = jak1::intern_from_c("*target*");
  if (!target_sym.offset || target_sym->value == 0 || target_sym->value == s7.offset) {
    lg::warn("[libjakopengoal] process_commands: *target* not available (val={})", target_sym->value);
    return;
  }

  u32 target_ptr = target_sym->value;
  u32 root_ptr = *(u32*)(g_ee_main_mem + target_ptr + 108);
  if (root_ptr == 0 || root_ptr == s7.offset) {
    lg::warn("[libjakopengoal] process_commands: root pointer invalid (root={})", root_ptr);
    return;
  }

  // trans is at root + 12, transv (velocity) is at root + 28
  float* trans = (float*)(g_ee_main_mem + root_ptr + 12);
  float* transv = (float*)(g_ee_main_mem + root_ptr + 28);

  // Render/SM64 units -> GOAL internal units conversion
  // GOAL uses 4096 units = 1 meter.  The host passes positions in "render units"
  // which are 50 units = 1 meter.  Scale factor = 4096 / 50.
  constexpr float UNITS_TO_GOAL = 4096.0f / 50.0f;

  for (auto& cmd : cmds) {
    lg::info("[libjakopengoal] Processing command type={} f=({:.1f},{:.1f},{:.1f})",
             static_cast<int>(cmd.type), cmd.f[0], cmd.f[1], cmd.f[2]);

    switch (cmd.type) {
      case CommandType::SPAWN:
      case CommandType::SET_POSITION: {
        float gx = cmd.f[0] * UNITS_TO_GOAL;
        float gy = cmd.f[1] * UNITS_TO_GOAL;
        float gz = cmd.f[2] * UNITS_TO_GOAL;
        trans[0] = gx;
        trans[1] = gy;
        trans[2] = gz;
        // Zero velocity on teleport so Jak doesn't slide
        transv[0] = 0.0f;
        transv[1] = 0.0f;
        transv[2] = 0.0f;
        lg::info("[libjakopengoal]   SET_POSITION: wrote trans=({:.1f},{:.1f},{:.1f}) GOAL units",
                 gx, gy, gz);
        break;
      }

      case CommandType::SET_VELOCITY: {
        transv[0] = cmd.f[0] * UNITS_TO_GOAL;
        transv[1] = cmd.f[1] * UNITS_TO_GOAL;
        transv[2] = cmd.f[2] * UNITS_TO_GOAL;
        lg::info("[libjakopengoal]   SET_VELOCITY: wrote transv=({:.1f},{:.1f},{:.1f})",
                 transv[0], transv[1], transv[2]);
        break;
      }

      default:
        lg::info("[libjakopengoal]   command type {} not handled by direct-write path",
                 static_cast<int>(cmd.type));
        break;
    }
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
  // IMPORTANT: In GOAL, a basic pointer points PAST the type tag.
  // So cache_addr + 0 is the FIRST user field (num-tris), not the type tag.
  // Layout (offsets from the GOAL pointer, i.e. from cache_addr):
  //   offset 0:   num-tris (int32)
  //   offset 4:   num-prims (int32)
  //   offset 8:   ignore-mask (pat-surface)
  //   offset 12:  proc (process-drawable)
  //   offset 28:  collide-box (bounding-box :inline, 32 bytes, 16-byte aligned)
  //   offset 60:  collide-box4w (bounding-box4w :inline, 32 bytes)
  //   offset 92:  collide-with (collide-kind, 8 bytes)
  //   offset 108: prims array (100 * 48 bytes = 4800 bytes)  [mips2c verified]
  //   offset 4908: tris array (461 * 64 bytes = 29504 bytes) [mips2c verified]

  // We write collision triangles directly into the cache's tri array
  // and set up a single background prim that references them.

  uint8_t* cache = g_ee_main_mem + (uint32_t)cache_addr;

  int32_t* num_tris_ptr = (int32_t*)(cache + 0);  // num-tris is at offset 0 from GOAL pointer
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

  // NOTE: mips2c code (method 30/puyp-mesh) uses offset 4908 from the GOAL object pointer.
  // Since the GOAL pointer for a basic is past the type tag, the offset from cache_addr is:
  // prims=104, 100*48=4800, 104+4800=4904.
  // But mips2c code also operates on the GOAL pointer (past type tag), so 4908 is correct
  // as an offset from cache_addr (which IS the GOAL pointer).
  const int TRI_OFFSET = 4908;
  const int MAX_TRIS = 461;

  // Surface vertices are stored in render/SM64 units.
  // GOAL's internal coordinate system uses meters where:
  //   METERS_TO_UNITS = 50.0 / 4096.0 (GOAL meters -> render units)
  // So we need the inverse: render units -> GOAL meters
  constexpr float UNITS_TO_METERS = 4096.0f / 50.0f;

  int tris_added = 0;
  auto& surfaces = s_collision_state.static_surfaces;

  // Read the collide-box bounding box (offset 28) to spatially filter surfaces.
  // bounding-box is min (vector 16 bytes) then max (vector 16 bytes) = 32 bytes total
  // These are in GOAL meters (already converted coordinate system).
  float* bbox_min = (float*)(cache + 28);
  float* bbox_max = (float*)(cache + 44);

  static int fill_log_counter = 0;
  bool should_log_fill = (++fill_log_counter % 120 == 1);
  if (should_log_fill) {
    lg::info("[collide-fill] goal_fill_external_collide called: {} static surfs, existing_tris={}, bbox=({:.0f},{:.0f},{:.0f})-({:.0f},{:.0f},{:.0f})",
             surfaces.size(), existing_tris,
             bbox_min[0], bbox_min[1], bbox_min[2],
             bbox_max[0], bbox_max[1], bbox_max[2]);
  }

  int skipped = 0;
  for (size_t s = 0; s < surfaces.size() && (existing_tris + tris_added) < MAX_TRIS; s++) {
    auto& surf = surfaces[s];

    // Spatial filter: check if any vertex of this triangle (in GOAL meters)
    // falls within or near the collide-box query region.
    // Convert vertices to GOAL meters for comparison with the bbox.
    float v0x = surf.vertices[0][0] * UNITS_TO_METERS;
    float v0y = surf.vertices[0][1] * UNITS_TO_METERS;
    float v0z = surf.vertices[0][2] * UNITS_TO_METERS;
    float v1x = surf.vertices[1][0] * UNITS_TO_METERS;
    float v1y = surf.vertices[1][1] * UNITS_TO_METERS;
    float v1z = surf.vertices[1][2] * UNITS_TO_METERS;
    float v2x = surf.vertices[2][0] * UNITS_TO_METERS;
    float v2y = surf.vertices[2][1] * UNITS_TO_METERS;
    float v2z = surf.vertices[2][2] * UNITS_TO_METERS;

    // Triangle AABB
    float tri_min_x = std::min({v0x, v1x, v2x});
    float tri_min_y = std::min({v0y, v1y, v2y});
    float tri_min_z = std::min({v0z, v1z, v2z});
    float tri_max_x = std::max({v0x, v1x, v2x});
    float tri_max_y = std::max({v0y, v1y, v2y});
    float tri_max_z = std::max({v0z, v1z, v2z});

    // AABB overlap test with the collide-box
    if (tri_max_x < bbox_min[0] || tri_min_x > bbox_max[0] ||
        tri_max_y < bbox_min[1] || tri_min_y > bbox_max[1] ||
        tri_max_z < bbox_min[2] || tri_min_z > bbox_max[2]) {
      skipped++;
      continue;  // Triangle is outside the query region
    }
    int tri_idx = existing_tris + tris_added;
    uint8_t* tri_base = cache + TRI_OFFSET + tri_idx * 64;

    // Write vertex 0 (converted from render units to GOAL meters)
    float* v0 = (float*)(tri_base + 0);
    v0[0] = surf.vertices[0][0] * UNITS_TO_METERS;
    v0[1] = surf.vertices[0][1] * UNITS_TO_METERS;
    v0[2] = surf.vertices[0][2] * UNITS_TO_METERS;
    v0[3] = 1.0f;

    // Write vertex 1
    float* v1 = (float*)(tri_base + 16);
    v1[0] = surf.vertices[1][0] * UNITS_TO_METERS;
    v1[1] = surf.vertices[1][1] * UNITS_TO_METERS;
    v1[2] = surf.vertices[1][2] * UNITS_TO_METERS;
    v1[3] = 1.0f;

    // Write vertex 2
    float* v2 = (float*)(tri_base + 32);
    v2[0] = surf.vertices[2][0] * UNITS_TO_METERS;
    v2[1] = surf.vertices[2][1] * UNITS_TO_METERS;
    v2[2] = surf.vertices[2][2] * UNITS_TO_METERS;
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

    if (should_log_fill && s < 4) {
      lg::info("[collide-fill] static tri[{}] v0=({:.1f},{:.1f},{:.1f}) v1=({:.1f},{:.1f},{:.1f}) v2=({:.1f},{:.1f},{:.1f}) pat=0x{:x}",
               tri_idx,
               v0[0], v0[1], v0[2],
               v1[0], v1[1], v1[2],
               v2[0], v2[1], v2[2],
               *pat);
    }

    tris_added++;
  }

  if (should_log_fill) {
    lg::info("[collide-fill] added {} tris (skipped {} outside bbox), total now {}",
             tris_added, skipped, existing_tris + tris_added);
  }

  // Also add dynamic object surfaces
  static int dyn_log_counter = 0;
  bool should_log_dyn = (++dyn_log_counter % 120 == 1);
  int dyn_obj_count = (int)s_collision_state.dynamic_objects.size();
  if (should_log_dyn && dyn_obj_count > 0) {
    lg::info("[collide-fill] {} dynamic objects, {} static tris already", dyn_obj_count, tris_added);
  }
  for (auto& obj : s_collision_state.dynamic_objects) {
    if (should_log_dyn) {
      lg::info("[collide-fill] obj id={} pos=({:.1f},{:.1f},{:.1f}) rot=({:.1f},{:.1f},{:.1f}) surfs={}",
               obj.id, obj.transform.position[0], obj.transform.position[1], obj.transform.position[2],
               obj.transform.rotation[0], obj.transform.rotation[1], obj.transform.rotation[2],
               obj.surfaces.size());
    }
    for (auto& surf : obj.surfaces) {
      if ((existing_tris + tris_added) >= MAX_TRIS)
        break;

      int tri_idx = existing_tris + tris_added;
      uint8_t* tri_base = cache + TRI_OFFSET + tri_idx * 64;

      // Apply object transform to vertices (rotation is in degrees, convert to radians)
      float yaw_rad = obj.transform.rotation[1] * (float)M_PI / 180.0f;
      float cos_y = std::cos(yaw_rad);
      float sin_y = std::sin(yaw_rad);

      for (int vi = 0; vi < 3; vi++) {
        float lx = surf.vertices[vi][0];
        float ly = surf.vertices[vi][1];
        float lz = surf.vertices[vi][2];

        // Simple Y-axis rotation + translation
        float wx = cos_y * lx + sin_y * lz + obj.transform.position[0];
        float wy = ly + obj.transform.position[1];
        float wz = -sin_y * lx + cos_y * lz + obj.transform.position[2];

        float* v = (float*)(tri_base + vi * 16);
        v[0] = wx * UNITS_TO_METERS;
        v[1] = wy * UNITS_TO_METERS;
        v[2] = wz * UNITS_TO_METERS;
        v[3] = 1.0f;
      }

      uint32_t* pat = (uint32_t*)(tri_base + 48);
      *pat = (uint32_t)(surf.type & 0x7);
      uint16_t* prim_idx = (uint16_t*)(tri_base + 52);
      *prim_idx = 0;

      /* Log first dynamic tri vertex in GOAL meters (should be ~28672 for Y=350) */
      if (should_log_dyn && &surf == &obj.surfaces[0]) {
        float* v0 = (float*)(tri_base);
        lg::info("[collide-fill] dyn tri v0 GOAL=({:.1f},{:.1f},{:.1f}) render=({:.1f},{:.1f},{:.1f})",
                 v0[0], v0[1], v0[2],
                 v0[0] / UNITS_TO_METERS, v0[1] / UNITS_TO_METERS, v0[2] / UNITS_TO_METERS);
      }

      tris_added++;
    }
  }

  // Update the tri count — GOAL will handle prim setup after this function returns
  // (we cannot reliably set up the prim from C++ due to potential offset differences
  //  between what the GOAL compiler uses and what all-types.gc declares)
  *num_tris_ptr = existing_tris + tris_added;

  if (should_log_fill) {
    lg::info("[collide-fill] returning tris_added={} total_tris={}", tris_added, existing_tris + tris_added);
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

  // Normalize the look direction to build inv-camera-rot.
  // cam_x/cam_z point from camera toward target (the look direction).
  // GOAL's inv-camera-rot transforms camera-local to world (column-major):
  //   col0 = right   = (-cam_z, 0, cam_x) / len   (left-handed: up × forward)
  //   col1 = up      = (0, 1, 0)
  //   col2 = forward = (cam_x, 0, cam_z) / len     (look direction)
  // read-pad creates (0,0,1) for stick-up, multiplied by this matrix gives col2 = forward.
  float len = sqrtf(cam_x * cam_x + cam_z * cam_z);
  if (len < 0.001f)
    return;
  float fwd_x = cam_x / len;
  float fwd_z = cam_z / len;
  // GOAL's atan negates stick X (stick0-dir = atan(-(leftx-128), ...)),
  // so the right vector must be flipped to compensate.
  float right_x = -fwd_z;    // -cam_z / len
  float right_z = fwd_x;     //  cam_x / len

  auto mc_sym = jak1::intern_from_c("*math-camera*");
  if (!mc_sym.offset || mc_sym->value == 0 || mc_sym->value == s7.offset)
    return;

  u32 mc_ptr = mc_sym->value;

  // Write rotation matrix to both inv-camera-rot and inv-camera-rot-smooth
  // math-camera is a basic type: all-types offsets 432 and 496, -4 for basic = 428 and 492
  for (int offset : {428, 492}) {
    float* m = (float*)(g_ee_main_mem + mc_ptr + offset);
    m[0] = right_x;  m[1] = 0.0f;  m[2] = right_z;  m[3] = 0.0f;   // col 0 (right)
    m[4] = 0.0f;     m[5] = 1.0f;  m[6] = 0.0f;     m[7] = 0.0f;   // col 1 (up)
    m[8] = fwd_x;    m[9] = 0.0f;  m[10] = fwd_z;   m[11] = 0.0f;  // col 2 (forward)
    m[12] = 0.0f;    m[13] = 0.0f; m[14] = 0.0f;    m[15] = 1.0f;  // col 3 (translation)
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

  // Set a minimum master volume floor to prevent the GOAL settings system from
  // fading all audio to zero. In library mode, save data isn't loaded so the
  // game's volume defaults/targets can end up at 0.
  snd_SetMinMasterVolume(512);  // ~50% of max (1024)

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

  // GOAL basic object pointer is 4 bytes after the allocation start.
  // all-types offset-assert values are relative to allocation start,
  // so subtract 4 to get the offset from the object pointer.
  // default setting-data is at offset-assert 432 → pointer offset 428.
  // setting-data field offsets: border-mode(+0), sfx-volume(+4), music-volume(+8),
  //   dialog-volume(+12), ambient-volume(+140)
  // GOAL basic object pointer is 4 bytes after the allocation start.
  // all-types offset-assert values are relative to allocation start,
  // so subtract 4 to get the offset from the object pointer.
  // setting-data field offsets: border-mode(+0), sfx-volume(+4), music-volume(+8),
  //   dialog-volume(+12), ambient-volume(+140)
  constexpr int DEFAULT_BASE = 428;  // offset-assert 432 - 4
  constexpr int TARGET_BASE = 220;   // offset-assert 224 - 4

  // default.border-mode — force to #f so Jak doesn't get killed for leaving level boundary
  *(u32*)(g_ee_main_mem + sc_ptr + DEFAULT_BASE + 0) = s7.offset;

  // Force default AND target volume settings to stay at reasonable levels.
  // Default alone isn't enough because apply-settings copies default to target then
  // game engine overrides can set volumes to 0 (e.g. during transitions/loading).
  // By forcing both, we ensure the seek target is always our desired volume.
  constexpr float SFX_VOL = 100.0f;
  constexpr float MUSIC_VOL = 80.0f;
  constexpr float DIALOG_VOL = 100.0f;
  constexpr float AMBIENT_VOL = 50.0f;

  // Force default volumes
  *(float*)(g_ee_main_mem + sc_ptr + DEFAULT_BASE + 4) = SFX_VOL;
  *(float*)(g_ee_main_mem + sc_ptr + DEFAULT_BASE + 8) = MUSIC_VOL;
  *(float*)(g_ee_main_mem + sc_ptr + DEFAULT_BASE + 12) = DIALOG_VOL;
  *(float*)(g_ee_main_mem + sc_ptr + DEFAULT_BASE + 140) = AMBIENT_VOL;

  // Force target volumes (overrides engine setting modifications)
  *(float*)(g_ee_main_mem + sc_ptr + TARGET_BASE + 4) = SFX_VOL;
  *(float*)(g_ee_main_mem + sc_ptr + TARGET_BASE + 8) = MUSIC_VOL;
  *(float*)(g_ee_main_mem + sc_ptr + TARGET_BASE + 12) = DIALOG_VOL;
  *(float*)(g_ee_main_mem + sc_ptr + TARGET_BASE + 140) = AMBIENT_VOL;
}

/**
 * Apply platform rider displacement — if Jak is standing on a dynamic surface object,
 * move him along with it by adding the platform's velocity to his position in GOAL memory.
 */
static void apply_rider_displacement() {
  if (!g_ee_main_mem)
    return;

  auto target_sym = jak1::intern_from_c("*target*");
  if (!target_sym.offset || target_sym->value == 0 || target_sym->value == s7.offset)
    return;

  u32 target_ptr = target_sym->value;
  u32 root_ptr = *(u32*)(g_ee_main_mem + target_ptr + 108);
  if (root_ptr == 0 || root_ptr == s7.offset)
    return;

  float* trans = (float*)(g_ee_main_mem + root_ptr + 12);
  constexpr float METERS_TO_UNITS = 50.0f / 4096.0f;
  constexpr float UNITS_TO_METERS = 4096.0f / 50.0f;

  // Jak's position in render units
  float jak_x = trans[0] * METERS_TO_UNITS;
  float jak_y = trans[1] * METERS_TO_UNITS;
  float jak_z = trans[2] * METERS_TO_UNITS;

  auto& coll = s_collision_state;
  std::lock_guard<std::mutex> lock(coll.mutex);

  for (auto& obj : coll.dynamic_objects) {
    // Only apply displacement once per move call (host moves at 30fps, bridge ticks at 60fps)
    if (obj.velocity_applied)
      continue;

    // Use PREVIOUS transform to check if Jak WAS on this platform before it moved
    float prev_top = obj.prev_transform.position[1] + obj.half_height;
    float dy = jak_y - prev_top;

    // Check if Jak is standing on this platform (within tolerance)
    if (dy < -10.0f || dy > 30.0f)
      continue;

    // Check XZ overlap against PREVIOUS transform (where was the platform last frame?)
    float prev_yaw_rad = obj.prev_transform.rotation[1] * (float)M_PI / 180.0f;
    float prev_cos = cosf(prev_yaw_rad);
    float prev_sin = sinf(prev_yaw_rad);
    float old_rel_x = jak_x - obj.prev_transform.position[0];
    float old_rel_z = jak_z - obj.prev_transform.position[2];
    // Inverse rotation to get local-space position on the OLD platform
    float local_x =  prev_cos * old_rel_x + prev_sin * old_rel_z;
    float local_z = -prev_sin * old_rel_x + prev_cos * old_rel_z;

    if (fabsf(local_x) > obj.half_width + 20.0f || fabsf(local_z) > obj.half_depth + 20.0f)
      continue;

    obj.velocity_applied = true;

    // Compute Jak's new world position:
    // 1. Take Jak's offset from OLD platform center
    // 2. Rotate that offset by the yaw delta
    // 3. Place at NEW platform center + rotated offset
    float new_yaw_rad = obj.transform.rotation[1] * (float)M_PI / 180.0f;
    float dyaw = new_yaw_rad - prev_yaw_rad;
    float cd = cosf(dyaw);
    float sd = sinf(dyaw);
    float rotated_rel_x = cd * old_rel_x - sd * old_rel_z;
    float rotated_rel_z = sd * old_rel_x + cd * old_rel_z;

    float new_jak_x = obj.transform.position[0] + rotated_rel_x;
    float new_jak_z = obj.transform.position[2] + rotated_rel_z;
    float new_jak_y = jak_y + obj.velocity[1];  // vertical displacement

    float vx = new_jak_x - jak_x;
    float vy = new_jak_y - jak_y;
    float vz = new_jak_z - jak_z;

    if (fabsf(vx) > 0.001f || fabsf(vy) > 0.001f || fabsf(vz) > 0.001f) {
      trans[0] += vx * UNITS_TO_METERS;
      trans[1] += vy * UNITS_TO_METERS;
      trans[2] += vz * UNITS_TO_METERS;

      static int rider_log = 0;
      if (++rider_log % 60 == 1) {
        lg::info("[rider] Displacing Jak by ({:.2f},{:.2f},{:.2f}) on platform id={}",
                 vx, vy, vz, obj.id);
      }
    }
    break;  // only ride one platform at a time
  }
}

/**
 * Snap Jak's Y position to the floor height from external collision surfaces.
 *
 * The GOAL collision pipeline's collide-cache fill correctly adds our external
 * tris, but the mips2c collision resolve methods don't detect them for ground.
 * As a workaround, we do a C++ floor probe each frame against the stored
 * surfaces and directly write the floor Y into Jak's root trans.
 */
void snap_to_floor() {
  if (!g_ee_main_mem) return;

  auto target_sym = jak1::intern_from_c("*target*");
  if (!target_sym.offset || target_sym->value == 0 || target_sym->value == s7.offset)
    return;

  u32 target_ptr = target_sym->value;
  u32 root_ptr = *(u32*)(g_ee_main_mem + target_ptr + 108);
  if (root_ptr == 0 || root_ptr == s7.offset)
    return;

  float* trans = (float*)(g_ee_main_mem + root_ptr + 12);

  // Convert Jak's GOAL position to render units for the floor query
  constexpr float GOAL_TO_RENDER = 50.0f / 4096.0f;
  constexpr float RENDER_TO_GOAL = 4096.0f / 50.0f;
  float rx = trans[0] * GOAL_TO_RENDER;
  float ry = trans[1] * GOAL_TO_RENDER;
  float rz = trans[2] * GOAL_TO_RENDER;

  // Find floor height (in render units) using the C++ surface test
  auto& coll = s_collision_state;
  std::lock_guard<std::mutex> lock(coll.mutex);

  float best_y = -1000000.0f;
  for (const auto& surf : coll.static_surfaces) {
    const float* v0 = surf.vertices[0];
    const float* v1 = surf.vertices[1];
    const float* v2 = surf.vertices[2];

    // Barycentric test in XZ plane
    float d00 = (v1[0]-v0[0])*(v1[0]-v0[0]) + (v1[2]-v0[2])*(v1[2]-v0[2]);
    float d01 = (v1[0]-v0[0])*(v2[0]-v0[0]) + (v1[2]-v0[2])*(v2[2]-v0[2]);
    float d11 = (v2[0]-v0[0])*(v2[0]-v0[0]) + (v2[2]-v0[2])*(v2[2]-v0[2]);
    float d20 = (rx-v0[0])*(v1[0]-v0[0]) + (rz-v0[2])*(v1[2]-v0[2]);
    float d21 = (rx-v0[0])*(v2[0]-v0[0]) + (rz-v0[2])*(v2[2]-v0[2]);

    float denom = d00*d11 - d01*d01;
    if (std::abs(denom) < 0.0001f) continue;

    float u = (d11*d20 - d01*d21) / denom;
    float v = (d00*d21 - d01*d20) / denom;

    if (u >= 0.0f && v >= 0.0f && (u+v) <= 1.0f) {
      float tri_y = v0[1]*(1.0f-u-v) + v1[1]*u + v2[1]*v;
      // Find the CLOSEST floor in Y regardless of whether Jak is above or below it.
      // This handles the case where GOAL gravity has already pulled Jak below the floor.
      if (tri_y > best_y) {
        best_y = tri_y;
      }
    }
  }

  static int snap_log = 0;
  bool should_log = (++snap_log % 60 == 1);

  if (best_y > -999999.0f) {
    float floor_goal_y = best_y * RENDER_TO_GOAL;
    float dist_above = ry - best_y;   // positive = above floor, negative = below

    // Simulate a floor:
    // - If Jak is at or below the floor (dist_above <= small threshold), snap to floor
    //   and zero downward velocity.  This acts as ground detection.
    // - If Jak is above the floor (e.g. jumping), leave him alone.
    // The threshold is generous (30 render units ≈ 0.37 GOAL meters) to catch
    // GOAL gravity pulling Jak past the floor between ticks.
    // Jak's default standing height is ~52 render units above the floor due to
    // GOAL's collision capsule.  Set threshold above that to catch idle Jak,
    // but below typical jump height (~150 render units) so jumping still works.
    constexpr float FLOOR_THRESHOLD = 60.0f;  // render units above floor to still snap

    if (dist_above <= FLOOR_THRESHOLD) {
      trans[1] = floor_goal_y;

      // Only zero downward velocity; preserve upward velocity (jumping)
      float* transv = (float*)(g_ee_main_mem + root_ptr + 28);
      if (transv[1] < 0.0f) {
        transv[1] = 0.0f;
      }

      if (should_log) {
        lg::info("[snap] jak=({:.1f},{:.1f},{:.1f}) floor={:.1f} dist={:.1f} -> snapped",
                 rx, ry, rz, best_y, dist_above);
      }
    } else {
      if (should_log) {
        lg::info("[snap] jak=({:.1f},{:.1f},{:.1f}) floor={:.1f} dist={:.1f} -> AIRBORNE (no snap)",
                 rx, ry, rz, best_y, dist_above);
      }
    }
  } else {
    if (should_log) {
      lg::info("[snap] jak=({:.1f},{:.1f},{:.1f}) NO FLOOR", rx, ry, rz);
    }
  }
}

void bridge_tick() {
  process_commands();
  force_settings();
  inject_camera_rotation();
  apply_rider_displacement();
  // snap_to_floor();  // DISABLED — was stomping Jak's Y position each frame
  extract_jak_state();
  read_bones_from_goal();
  extract_jak_mesh();

  // Signal vblank to the IOP kernel — in normal game mode this is driven by
  // the display vsync callback, but in library mode display is disabled.
  // Without this, VBlank_Handler never fires and the sound system stalls
  // (gFrameNum stops incrementing, VAG clock stops, sound dies after ~1s).
  iop::LIBRARY_signal_vblank();

  // Throttle the GOAL runtime to ~60fps — without display/vsync it runs uncapped.
  // We measure wall-clock time since the last tick and sleep for the remainder of 16.67ms.
  {
    using clock = std::chrono::steady_clock;
    static auto last_tick = clock::now();
    constexpr auto FRAME_TIME = std::chrono::microseconds(16667);  // 60fps

    auto now = clock::now();
    auto elapsed = now - last_tick;
    if (elapsed < FRAME_TIME) {
      std::this_thread::sleep_for(FRAME_TIME - elapsed);
    }
    last_tick = clock::now();
  }
}

}  // namespace jak_bridge
