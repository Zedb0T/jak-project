/**
 * jak_model_exporter - Export Jak's FR3 mesh to SM64 DynOS-compatible C source files.
 *
 * Reads eichar-lod0 from GAME.fr3, maps Jak's 83 bones to Mario's 20-bone
 * skeleton, and outputs:
 *   - model.inc.c  : per-bone vertex arrays + display lists
 *   - geo.inc.c    : GeoLayout matching Mario's bone hierarchy
 *
 * The exported model uses Mario's skeleton structure so Mario's animations
 * drive Jak's movement in sm64coopdx character-select.
 *
 * Build: cmake --build build --target jak_model_exporter --config Release
 * Run:   ./build/bin/Release/jak_model_exporter.exe
 * Output: 64-jak-mp-mod/mods/jak-opengoal/actors/jak/
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/Serializer.h"
#include "common/util/compress.h"

// SM64 vertex format
struct SM64Vtx {
  int16_t x, y, z;
  int16_t flag;
  int16_t u, v;
  uint8_t r, g, b, a;
};

// Triangle with indices into a local vertex array
struct Triangle {
  uint32_t v[3];
};

// Per-bone mesh data for output
struct BoneMeshData {
  std::vector<SM64Vtx> verts;
  std::vector<Triangle> tris;
};

// Mario's 20-bone skeleton
enum MarioBone {
  MARIO_ROOT = 0,
  MARIO_BUTT,
  MARIO_TORSO,
  MARIO_HEAD,
  MARIO_L_SHOULDER,
  MARIO_L_UPPER_ARM,
  MARIO_L_FOREARM,
  MARIO_L_HAND,
  MARIO_R_SHOULDER,
  MARIO_R_UPPER_ARM,
  MARIO_R_FOREARM,
  MARIO_R_HAND,
  MARIO_L_HIP,
  MARIO_L_THIGH,
  MARIO_L_LEG,
  MARIO_L_FOOT,
  MARIO_R_HIP,
  MARIO_R_THIGH,
  MARIO_R_LEG,
  MARIO_R_FOOT,
  MARIO_BONE_COUNT
};

static const char* mario_bone_names[MARIO_BONE_COUNT] = {
    "root",        "butt",        "torso",     "head",       "l_shoulder", "l_upper_arm",
    "l_forearm",   "l_hand",      "r_shoulder", "r_upper_arm", "r_forearm",  "r_hand",
    "l_hip",       "l_thigh",     "l_leg",     "l_foot",     "r_hip",      "r_thigh",
    "r_leg",       "r_foot",
};

// Parent index for each bone (-1 = root)
static const int mario_bone_parent[MARIO_BONE_COUNT] = {
    -1,  // root
    0,   // butt -> root
    1,   // torso -> butt
    2,   // head -> torso
    2,   // l_shoulder -> torso
    4,   // l_upper_arm -> l_shoulder
    5,   // l_forearm -> l_upper_arm
    6,   // l_hand -> l_forearm
    2,   // r_shoulder -> torso
    8,   // r_upper_arm -> r_shoulder
    9,   // r_forearm -> r_upper_arm
    10,  // r_hand -> r_forearm
    1,   // l_hip -> butt
    12,  // l_thigh -> l_hip
    13,  // l_leg -> l_thigh
    14,  // l_foot -> l_leg
    1,   // r_hip -> butt
    16,  // r_thigh -> r_hip
    17,  // r_leg -> r_thigh
    18,  // r_foot -> r_leg
};

// Map Jak FR3 mats[] value (1-indexed bone index) to a Mario bone.
// mats[i]=0 means unweighted, mats[i]=N means Jak bone N (1-indexed).
static int jak_mats_to_mario(uint8_t mats_val) {
  switch (mats_val) {
    // Unweighted / structural
    case 0:
    case 1:   // align
    case 2:   // prejoint
    case 3:   // main
      return MARIO_ROOT;

    // Hips / pelvis
    case 26:  // hips
    case 81:  // belt
    case 35:  // shirtLthigh
    case 36:  // shirtRthigh
      return MARIO_BUTT;

    // Torso / chest
    case 4:   // upper_body
    case 5:   // chest
    case 64:  // LshoulderPad
    case 65:  // collarL
    case 66:  // collarR
    case 67:  // packStrapTop
    case 68:  // packStrapMid
      return MARIO_TORSO;

    // Head / neck / face / ears / hair
    case 6:   // neckA
    case 7:   // neckB
    case 8:   // MhairA
    case 9:   // MhairB
    case 10:  // Learbase
    case 11:  // Learmid
    case 12:  // Rearbase
    case 13:  // Rearmid
    case 37:  // mouth
    case 38:  // browL
    case 39:  // browR
    case 82:  // eyeL
    case 83:  // eyeR
      return MARIO_HEAD;

    // Left arm
    case 14:  // Lshould
    case 15:  // Larm
      return MARIO_L_UPPER_ARM;
    case 16:  // Lforarm
    case 18:  // handLStrapTopTop
    case 19:  // handLStrapTopMid
      return MARIO_L_FOREARM;
    case 17:  // sk_lhand
    case 40: case 41:  // lthumA/B
    case 42: case 43:  // lindA/B
    case 44: case 45:  // lmidA/B
    case 46: case 47:  // lringA/B
    case 48: case 49:  // lpinkA/B
    case 50: case 51:  // handLStrapBotTop/Mid
      return MARIO_L_HAND;

    // Right arm
    case 20:  // Rshould
    case 21:  // Rarm
      return MARIO_R_UPPER_ARM;
    case 22:  // Rforarm
    case 24:  // handRStrapTopTop
    case 25:  // handRStrapTopMid
      return MARIO_R_FOREARM;
    case 23:  // sk_rhand
    case 52: case 53:  // rthumA/B
    case 54: case 55:  // rindA/B
    case 56: case 57:  // rmidA/B
    case 58: case 59:  // rringA/B
    case 60: case 61:  // rpinkA/B
    case 62: case 63:  // handRStrapBotTop/Mid
      return MARIO_R_HAND;

    // Left leg
    case 27:  // Lthigh
    case 79:  // pantsLthigh
      return MARIO_L_THIGH;
    case 28:  // Lknee
    case 80:  // pantsLknee
      return MARIO_L_LEG;
    case 29:  // Lankle
    case 30:  // ankleLStrap
    case 69:  // Lball
    case 70:  // LbigToe
    case 71:  // Ltoes
    case 72:  // footLStrap
      return MARIO_L_FOOT;

    // Right leg
    case 31:  // Rthigh
    case 77:  // pantsRthigh
      return MARIO_R_THIGH;
    case 32:  // Rknee
    case 78:  // pantsRknee
      return MARIO_R_LEG;
    case 33:  // Rankle
    case 34:  // ankleRStrap
    case 73:  // Rball
    case 74:  // RbigToe
    case 75:  // Rtoes
    case 76:  // footRStrap
      return MARIO_R_FOOT;

    default:
      return MARIO_BUTT;  // fallback: assign unknown bones to pelvis
  }
}

// Get the primary (highest weight) bone for a vertex, mapped to Mario bone
static int get_mario_bone(const tfrag3::MercVertex& v) {
  uint8_t primary = v.mats[0];
  float max_w = v.weights[0];
  for (int j = 1; j < 3; j++) {
    if (v.weights[j] > max_w) {
      max_w = v.weights[j];
      primary = v.mats[j];
    }
  }
  return jak_mats_to_mario(primary);
}

int main(int argc, char** argv) {
  lg::initialize();
  if (!file_util::setup_project_path(std::nullopt)) {
    printf("ERROR: Could not find jak-project directory.\n");
    return 1;
  }

  auto project_dir = file_util::get_jak_project_dir();
  auto fr3_path = project_dir / "out" / "jak1" / "fr3" / "GAME.fr3";

  printf("=== Jak Skeleton Model Exporter ===\n");
  printf("FR3 path: %s\n", fr3_path.string().c_str());

  if (!fs::exists(fr3_path)) {
    printf("ERROR: GAME.fr3 not found. Decompile the game first.\n");
    return 1;
  }

  // --------------------------------------------------------------------------
  // Load FR3
  // --------------------------------------------------------------------------
  printf("Loading FR3...\n");
  auto data = file_util::read_binary_file(fr3_path);
  auto decomp = compression::decompress_zstd(data.data(), data.size());
  tfrag3::Level level;
  Serializer ser(decomp.data(), decomp.size());
  level.serialize(ser);

  const tfrag3::MercModel* jak_model = nullptr;
  for (const auto& model : level.merc_data.models) {
    if (model.name == "eichar-lod0") {
      jak_model = &model;
      break;
    }
  }
  if (!jak_model) {
    printf("ERROR: eichar-lod0 not found in GAME.fr3\n");
    return 1;
  }

  printf("Found eichar-lod0: %zu effects, max_bones=%d, xyz_scale=%.3f\n",
         jak_model->effects.size(), jak_model->max_bones, jak_model->xyz_scale);

  // --------------------------------------------------------------------------
  // Collect draw ranges (skip eye draws)
  // --------------------------------------------------------------------------
  struct DrawRange {
    uint32_t first_index, index_count;
    bool no_strip;
    int32_t tree_tex_id;
    uint8_t eye_id;
  };
  std::vector<DrawRange> draws;
  for (const auto& effect : jak_model->effects) {
    for (const auto& draw : effect.all_draws) {
      draws.push_back(
          {draw.first_index, draw.index_count, draw.no_strip, draw.tree_tex_id, draw.eye_id});
    }
  }

  auto& all_verts = level.merc_data.vertices;
  auto& all_indices = level.merc_data.indices;

  // --------------------------------------------------------------------------
  // Expand triangle strips/lists, skip degenerate and eye draws
  // --------------------------------------------------------------------------
  std::vector<Triangle> all_triangles;
  for (const auto& draw : draws) {
    if (draw.eye_id != 0xFF)
      continue;

    std::vector<uint32_t> expanded;
    if (draw.no_strip) {
      for (uint32_t i = 0; i < draw.index_count; i++)
        expanded.push_back(all_indices[draw.first_index + i]);
    } else {
      std::vector<uint32_t> strip;
      for (uint32_t i = 0; i < draw.index_count; i++) {
        uint32_t idx = all_indices[draw.first_index + i];
        if (idx == UINT32_MAX) {
          strip.clear();
          continue;
        }
        strip.push_back(idx);
        if (strip.size() >= 3) {
          size_t n = strip.size();
          if ((n - 3) % 2 == 0) {
            expanded.push_back(strip[n - 3]);
            expanded.push_back(strip[n - 2]);
            expanded.push_back(strip[n - 1]);
          } else {
            expanded.push_back(strip[n - 2]);
            expanded.push_back(strip[n - 3]);
            expanded.push_back(strip[n - 1]);
          }
        }
      }
    }

    for (size_t i = 0; i + 2 < expanded.size(); i += 3) {
      uint32_t a = expanded[i], b = expanded[i + 1], c = expanded[i + 2];
      if (a == b || b == c || a == c)
        continue;
      all_triangles.push_back({a, b, c});
    }
  }

  printf("Total triangles: %zu\n", all_triangles.size());

  // --------------------------------------------------------------------------
  // Scale factor: FR3 → SM64 units
  // --------------------------------------------------------------------------
  float fr3_to_sm64 = 160.0f / (12288.0f * jak_model->xyz_scale);
  printf("Scale: fr3_to_sm64 = %.6f\n", fr3_to_sm64);

  // Helper: convert FR3 vertex position to SM64 world-space float
  auto fr3_pos_to_sm64 = [&](const tfrag3::MercVertex& sv, float out[3]) {
    out[0] = sv.pos[0] * jak_model->xyz_scale * fr3_to_sm64;
    out[1] = sv.pos[1] * jak_model->xyz_scale * fr3_to_sm64;
    out[2] = sv.pos[2] * jak_model->xyz_scale * fr3_to_sm64;
  };

  // --------------------------------------------------------------------------
  // Assign each triangle to a Mario bone (based on vertex 0's primary bone)
  // --------------------------------------------------------------------------
  std::vector<int> tri_bone(all_triangles.size());
  std::map<int, std::vector<size_t>> mario_bone_tris;  // mario bone → triangle indices

  for (size_t ti = 0; ti < all_triangles.size(); ti++) {
    int mb = get_mario_bone(all_verts[all_triangles[ti].v[0]]);
    tri_bone[ti] = mb;
    mario_bone_tris[mb].push_back(ti);
  }

  // --------------------------------------------------------------------------
  // Compute centroid (world-space SM64 units) per Mario bone
  // --------------------------------------------------------------------------
  float bone_centroid[MARIO_BONE_COUNT][3] = {};
  int bone_vert_count[MARIO_BONE_COUNT] = {};

  for (size_t ti = 0; ti < all_triangles.size(); ti++) {
    int mb = tri_bone[ti];
    for (int k = 0; k < 3; k++) {
      float pos[3];
      fr3_pos_to_sm64(all_verts[all_triangles[ti].v[k]], pos);
      bone_centroid[mb][0] += pos[0];
      bone_centroid[mb][1] += pos[1];
      bone_centroid[mb][2] += pos[2];
      bone_vert_count[mb]++;
    }
  }

  for (int b = 0; b < MARIO_BONE_COUNT; b++) {
    if (bone_vert_count[b] > 0) {
      bone_centroid[b][0] /= bone_vert_count[b];
      bone_centroid[b][1] /= bone_vert_count[b];
      bone_centroid[b][2] /= bone_vert_count[b];
    }
  }

  // For bones with no vertices (joints only), estimate position
  // Use child centroid or midpoint between parent and child
  auto estimate_empty_bone = [&](int bone, int child) {
    if (bone_vert_count[bone] == 0) {
      int parent = mario_bone_parent[bone];
      if (parent >= 0 && bone_vert_count[parent] > 0 && bone_vert_count[child] > 0) {
        // Midpoint between parent and child
        for (int i = 0; i < 3; i++)
          bone_centroid[bone][i] = (bone_centroid[parent][i] + bone_centroid[child][i]) * 0.5f;
      } else if (bone_vert_count[child] > 0) {
        for (int i = 0; i < 3; i++)
          bone_centroid[bone][i] = bone_centroid[child][i];
      }
    }
  };

  // Root: use butt position
  if (bone_vert_count[MARIO_ROOT] == 0) {
    for (int i = 0; i < 3; i++)
      bone_centroid[MARIO_ROOT][i] = bone_centroid[MARIO_BUTT][i];
  }

  estimate_empty_bone(MARIO_L_SHOULDER, MARIO_L_UPPER_ARM);
  estimate_empty_bone(MARIO_R_SHOULDER, MARIO_R_UPPER_ARM);
  estimate_empty_bone(MARIO_L_HIP, MARIO_L_THIGH);
  estimate_empty_bone(MARIO_R_HIP, MARIO_R_THIGH);

  // --------------------------------------------------------------------------
  // Compute bone world positions and parent-relative translations
  // The world position is the centroid. The translation is relative to parent.
  // --------------------------------------------------------------------------
  float bone_world[MARIO_BONE_COUNT][3];
  int16_t bone_trans[MARIO_BONE_COUNT][3];

  for (int b = 0; b < MARIO_BONE_COUNT; b++) {
    for (int i = 0; i < 3; i++)
      bone_world[b][i] = bone_centroid[b][i];

    int parent = mario_bone_parent[b];
    if (parent < 0) {
      // Root: no translation (model is centered at origin)
      bone_trans[b][0] = 0;
      bone_trans[b][1] = 0;
      bone_trans[b][2] = 0;
    } else {
      bone_trans[b][0] = (int16_t)std::round(bone_world[b][0] - bone_world[parent][0]);
      bone_trans[b][1] = (int16_t)std::round(bone_world[b][1] - bone_world[parent][1]);
      bone_trans[b][2] = (int16_t)std::round(bone_world[b][2] - bone_world[parent][2]);
    }
  }

  printf("\nBone positions (SM64 units):\n");
  for (int b = 0; b < MARIO_BONE_COUNT; b++) {
    printf("  %2d %-14s: world=(%.1f, %.1f, %.1f) trans=(%d, %d, %d) verts=%d\n", b,
           mario_bone_names[b], bone_world[b][0], bone_world[b][1], bone_world[b][2],
           bone_trans[b][0], bone_trans[b][1], bone_trans[b][2], bone_vert_count[b]);
  }

  // --------------------------------------------------------------------------
  // Build per-bone mesh: remap vertices to bone-local coordinates
  // --------------------------------------------------------------------------
  BoneMeshData bone_mesh[MARIO_BONE_COUNT];

  for (int b = 0; b < MARIO_BONE_COUNT; b++) {
    auto it = mario_bone_tris.find(b);
    if (it == mario_bone_tris.end())
      continue;

    std::map<uint32_t, uint32_t> global_to_local;

    for (size_t ti_idx : it->second) {
      Triangle lt;
      for (int k = 0; k < 3; k++) {
        uint32_t gi = all_triangles[ti_idx].v[k];
        auto found = global_to_local.find(gi);
        if (found == global_to_local.end()) {
          uint32_t li = (uint32_t)bone_mesh[b].verts.size();
          global_to_local[gi] = li;

          auto& sv = all_verts[gi];
          float wpos[3];
          fr3_pos_to_sm64(sv, wpos);

          SM64Vtx vtx;
          // Bone-local position: subtract bone's world position
          vtx.x = (int16_t)std::clamp(wpos[0] - bone_world[b][0], -32768.0f, 32767.0f);
          vtx.y = (int16_t)std::clamp(wpos[1] - bone_world[b][1], -32768.0f, 32767.0f);
          vtx.z = (int16_t)std::clamp(wpos[2] - bone_world[b][2], -32768.0f, 32767.0f);
          vtx.flag = 0;
          vtx.u = (int16_t)(sv.st[0] * 32.0f * 32.0f);
          vtx.v = (int16_t)(sv.st[1] * 32.0f * 32.0f);
          vtx.r = sv.rgba[0];
          vtx.g = sv.rgba[1];
          vtx.b = sv.rgba[2];
          vtx.a = 0xFF;

          bone_mesh[b].verts.push_back(vtx);
          lt.v[k] = li;
        } else {
          lt.v[k] = found->second;
        }
      }
      bone_mesh[b].tris.push_back(lt);
    }
  }

  // --------------------------------------------------------------------------
  // Output directory
  // --------------------------------------------------------------------------
  auto out_dir = project_dir / "64-jak-mp-mod" / "mods" / "jak-opengoal" / "actors" / "jak";
  fs::create_directories(out_dir);

  // --------------------------------------------------------------------------
  // Write model.inc.c — per-bone vertex arrays + display lists
  // --------------------------------------------------------------------------
  {
    auto model_path = out_dir / "model.inc.c";
    FILE* f = fopen(model_path.string().c_str(), "w");
    if (!f) {
      printf("ERROR: Can't open %s\n", model_path.string().c_str());
      return 1;
    }

    fprintf(f, "// Auto-generated by jak_model_exporter (skeleton mode)\n");
    fprintf(f, "// Jak (eichar-lod0) mapped to Mario's 20-bone skeleton\n\n");

    const int MAX_VTX_BATCH = 32;

    // For each bone that has geometry, write vertex arrays + display list
    for (int b = 0; b < MARIO_BONE_COUNT; b++) {
      if (bone_mesh[b].tris.empty())
        continue;

      auto& bm = bone_mesh[b];

      // Batch triangles into groups of <=32 unique verts
      struct Batch {
        std::vector<uint32_t> tri_indices;
        std::vector<uint32_t> vert_list;
        std::map<uint32_t, int> vert_map;
      };
      std::vector<Batch> batches;

      {
        Batch cur;
        for (size_t ti = 0; ti < bm.tris.size(); ti++) {
          auto& tri = bm.tris[ti];
          int new_count = 0;
          for (int k = 0; k < 3; k++) {
            if (cur.vert_map.find(tri.v[k]) == cur.vert_map.end())
              new_count++;
          }
          if ((int)cur.vert_list.size() + new_count > MAX_VTX_BATCH) {
            if (!cur.tri_indices.empty()) {
              batches.push_back(std::move(cur));
              cur = Batch{};
            }
          }
          cur.tri_indices.push_back((uint32_t)ti);
          for (int k = 0; k < 3; k++) {
            if (cur.vert_map.find(tri.v[k]) == cur.vert_map.end()) {
              int bi = (int)cur.vert_list.size();
              cur.vert_list.push_back(tri.v[k]);
              cur.vert_map[tri.v[k]] = bi;
            }
          }
        }
        if (!cur.tri_indices.empty())
          batches.push_back(std::move(cur));
      }

      // Write vertex arrays
      for (size_t bi = 0; bi < batches.size(); bi++) {
        fprintf(f, "static const Vtx jak_%s_vtx_%zu[] = {\n", mario_bone_names[b], bi);
        for (auto vi : batches[bi].vert_list) {
          auto& v = bm.verts[vi];
          fprintf(f,
                  "    {{{%6d, %6d, %6d}, 0, {%6d, %6d}, {0x%02x, 0x%02x, 0x%02x, 0x%02x}}},\n",
                  v.x, v.y, v.z, v.u, v.v, v.r, v.g, v.b, v.a);
        }
        fprintf(f, "};\n\n");
      }

      // Write display list for this bone
      fprintf(f, "const Gfx jak_%s_dl[] = {\n", mario_bone_names[b]);
      fprintf(f, "    gsDPPipeSync(),\n");
      fprintf(f, "    gsSPClearGeometryMode(G_LIGHTING | G_TEXTURE_GEN | G_TEXTURE_GEN_LINEAR),\n");
      fprintf(f, "    gsSPSetGeometryMode(G_SHADE | G_SHADING_SMOOTH),\n");
      fprintf(f, "    gsDPSetCombineMode(G_CC_SHADE, G_CC_SHADE),\n");

      for (size_t bi = 0; bi < batches.size(); bi++) {
        auto& batch = batches[bi];
        fprintf(f, "    gsSPVertex(jak_%s_vtx_%zu, %d, 0),\n", mario_bone_names[b], bi,
                (int)batch.vert_list.size());

        for (size_t i = 0; i < batch.tri_indices.size(); i += 2) {
          auto& t0 = bm.tris[batch.tri_indices[i]];
          int a0 = batch.vert_map[t0.v[0]];
          int b0 = batch.vert_map[t0.v[1]];
          int c0 = batch.vert_map[t0.v[2]];

          if (i + 1 < batch.tri_indices.size()) {
            auto& t1 = bm.tris[batch.tri_indices[i + 1]];
            int a1 = batch.vert_map[t1.v[0]];
            int b1 = batch.vert_map[t1.v[1]];
            int c1 = batch.vert_map[t1.v[2]];
            fprintf(f, "    gsSP2Triangles(%2d, %2d, %2d, 0x0, %2d, %2d, %2d, 0x0),\n", a0, b0,
                    c0, a1, b1, c1);
          } else {
            fprintf(f, "    gsSP1Triangle(%2d, %2d, %2d, 0x0),\n", a0, b0, c0);
          }
        }
      }

      fprintf(f, "    gsSPEndDisplayList(),\n");
      fprintf(f, "};\n\n");

      printf("  Bone %2d %-14s: %zu verts, %zu tris, %zu batches\n", b, mario_bone_names[b],
             bm.verts.size(), bm.tris.size(), batches.size());
    }

    fclose(f);
    printf("Wrote model.inc.c\n");
  }

  // --------------------------------------------------------------------------
  // Write geo.inc.c — GeoLayout matching Mario's bone hierarchy
  // --------------------------------------------------------------------------
  {
    auto geo_path = out_dir / "geo.inc.c";
    FILE* f = fopen(geo_path.string().c_str(), "w");
    if (!f) {
      printf("ERROR: Can't open %s\n", geo_path.string().c_str());
      return 1;
    }

    fprintf(f, "// Auto-generated by jak_model_exporter (skeleton mode)\n");
    fprintf(f, "// GeoLayout with Mario-compatible 20-bone skeleton\n");
    fprintf(f, "// Allows Mario's animations to drive Jak's movement\n\n");

    // Helper: get display list name or NULL for a bone
    auto dl_name = [&](int bone) -> std::string {
      if (bone_mesh[bone].tris.empty())
        return "NULL";
      return std::string("jak_") + mario_bone_names[bone] + "_dl";
    };

    // Write the GeoLayout matching Mario's body structure exactly.
    // Tree structure (indentation = nesting):
    //   root
    //     butt
    //       torso
    //         head
    //         l_shoulder
    //           l_upper_arm
    //             l_forearm
    //               l_hand
    //         r_shoulder
    //           r_upper_arm
    //             r_forearm
    //               r_hand
    //       l_hip
    //         l_thigh
    //           l_leg
    //             l_foot
    //       r_hip
    //         r_thigh
    //           r_leg
    //             r_foot

    fprintf(f, "const GeoLayout jak_geo[] = {\n");
    fprintf(f, "    GEO_NODE_START(),\n");
    fprintf(f, "    GEO_OPEN_NODE(),\n");
    fprintf(f, "        GEO_SCALE(0x00, 65536),\n");  // 1.0x scale (65536 = 1.0)
    fprintf(f, "        GEO_OPEN_NODE(),\n");

    // Bone 0: Root
    fprintf(f, "            GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[0][0], bone_trans[0][1], bone_trans[0][2], dl_name(0).c_str());
    fprintf(f, "            GEO_OPEN_NODE(),\n");

    // Bone 1: Butt
    fprintf(f, "                GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[1][0], bone_trans[1][1], bone_trans[1][2], dl_name(1).c_str());
    fprintf(f, "                GEO_OPEN_NODE(),\n");

    // Bone 2: Torso
    fprintf(f, "                    GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[2][0], bone_trans[2][1], bone_trans[2][2], dl_name(2).c_str());
    fprintf(f, "                    GEO_OPEN_NODE(),\n");

    // Bone 3: Head
    fprintf(f, "                        GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[3][0], bone_trans[3][1], bone_trans[3][2], dl_name(3).c_str());

    // Bone 4: L Shoulder
    fprintf(f, "                        GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[4][0], bone_trans[4][1], bone_trans[4][2], dl_name(4).c_str());
    fprintf(f, "                        GEO_OPEN_NODE(),\n");

    // Bone 5: L Upper Arm
    fprintf(f, "                            GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[5][0], bone_trans[5][1], bone_trans[5][2], dl_name(5).c_str());
    fprintf(f, "                            GEO_OPEN_NODE(),\n");

    // Bone 6: L Forearm
    fprintf(f,
            "                                GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[6][0], bone_trans[6][1], bone_trans[6][2], dl_name(6).c_str());
    fprintf(f, "                                GEO_OPEN_NODE(),\n");

    // Bone 7: L Hand
    fprintf(f,
            "                                    GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, "
            "%s),\n",
            bone_trans[7][0], bone_trans[7][1], bone_trans[7][2], dl_name(7).c_str());

    fprintf(f, "                                GEO_CLOSE_NODE(),\n");  // close l_forearm
    fprintf(f, "                            GEO_CLOSE_NODE(),\n");      // close l_upper_arm
    fprintf(f, "                        GEO_CLOSE_NODE(),\n");          // close l_shoulder

    // Bone 8: R Shoulder
    fprintf(f, "                        GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[8][0], bone_trans[8][1], bone_trans[8][2], dl_name(8).c_str());
    fprintf(f, "                        GEO_OPEN_NODE(),\n");

    // Bone 9: R Upper Arm
    fprintf(f, "                            GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[9][0], bone_trans[9][1], bone_trans[9][2], dl_name(9).c_str());
    fprintf(f, "                            GEO_OPEN_NODE(),\n");

    // Bone 10: R Forearm
    fprintf(f,
            "                                GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[10][0], bone_trans[10][1], bone_trans[10][2], dl_name(10).c_str());
    fprintf(f, "                                GEO_OPEN_NODE(),\n");

    // Bone 11: R Hand
    fprintf(f,
            "                                    GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, "
            "%s),\n",
            bone_trans[11][0], bone_trans[11][1], bone_trans[11][2], dl_name(11).c_str());

    fprintf(f, "                                GEO_CLOSE_NODE(),\n");  // close r_forearm
    fprintf(f, "                            GEO_CLOSE_NODE(),\n");      // close r_upper_arm
    fprintf(f, "                        GEO_CLOSE_NODE(),\n");          // close r_shoulder

    fprintf(f, "                    GEO_CLOSE_NODE(),\n");  // close torso

    // Bone 12: L Hip
    fprintf(f, "                    GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[12][0], bone_trans[12][1], bone_trans[12][2], dl_name(12).c_str());
    fprintf(f, "                    GEO_OPEN_NODE(),\n");

    // Bone 13: L Thigh
    fprintf(f, "                        GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[13][0], bone_trans[13][1], bone_trans[13][2], dl_name(13).c_str());
    fprintf(f, "                        GEO_OPEN_NODE(),\n");

    // Bone 14: L Leg
    fprintf(f, "                            GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[14][0], bone_trans[14][1], bone_trans[14][2], dl_name(14).c_str());
    fprintf(f, "                            GEO_OPEN_NODE(),\n");

    // Bone 15: L Foot
    fprintf(f,
            "                                GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[15][0], bone_trans[15][1], bone_trans[15][2], dl_name(15).c_str());

    fprintf(f, "                            GEO_CLOSE_NODE(),\n");  // close l_leg
    fprintf(f, "                        GEO_CLOSE_NODE(),\n");      // close l_thigh
    fprintf(f, "                    GEO_CLOSE_NODE(),\n");          // close l_hip

    // Bone 16: R Hip
    fprintf(f, "                    GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[16][0], bone_trans[16][1], bone_trans[16][2], dl_name(16).c_str());
    fprintf(f, "                    GEO_OPEN_NODE(),\n");

    // Bone 17: R Thigh
    fprintf(f, "                        GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[17][0], bone_trans[17][1], bone_trans[17][2], dl_name(17).c_str());
    fprintf(f, "                        GEO_OPEN_NODE(),\n");

    // Bone 18: R Leg
    fprintf(f, "                            GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[18][0], bone_trans[18][1], bone_trans[18][2], dl_name(18).c_str());
    fprintf(f, "                            GEO_OPEN_NODE(),\n");

    // Bone 19: R Foot
    fprintf(f,
            "                                GEO_ANIMATED_PART(LAYER_OPAQUE, %d, %d, %d, %s),\n",
            bone_trans[19][0], bone_trans[19][1], bone_trans[19][2], dl_name(19).c_str());

    fprintf(f, "                            GEO_CLOSE_NODE(),\n");  // close r_leg
    fprintf(f, "                        GEO_CLOSE_NODE(),\n");      // close r_thigh
    fprintf(f, "                    GEO_CLOSE_NODE(),\n");          // close r_hip

    fprintf(f, "                GEO_CLOSE_NODE(),\n");  // close butt
    fprintf(f, "            GEO_CLOSE_NODE(),\n");      // close root

    fprintf(f, "        GEO_CLOSE_NODE(),\n");  // close scale
    fprintf(f, "    GEO_CLOSE_NODE(),\n");      // close node_start
    fprintf(f, "    GEO_END(),\n");
    fprintf(f, "};\n\n");

    // Cap geo (simple version)
    fprintf(f, "const GeoLayout jak_cap_geo[] = {\n");
    fprintf(f, "    GEO_NODE_START(),\n");
    fprintf(f, "    GEO_OPEN_NODE(),\n");
    fprintf(f, "        GEO_DISPLAY_LIST(LAYER_OPAQUE, jak_head_dl),\n");
    fprintf(f, "    GEO_CLOSE_NODE(),\n");
    fprintf(f, "    GEO_END(),\n");
    fprintf(f, "};\n");

    fclose(f);
    printf("Wrote geo.inc.c\n");
  }

  printf("\n=== Skeleton export complete! ===\n");
  printf("Output: %s\n", out_dir.string().c_str());
  printf("Bones with geometry:\n");
  int total_verts = 0, total_tris = 0;
  for (int b = 0; b < MARIO_BONE_COUNT; b++) {
    if (!bone_mesh[b].tris.empty()) {
      printf("  %-14s: %zu verts, %zu tris\n", mario_bone_names[b], bone_mesh[b].verts.size(),
             bone_mesh[b].tris.size());
      total_verts += bone_mesh[b].verts.size();
      total_tris += bone_mesh[b].tris.size();
    }
  }
  printf("Total: %d verts, %d tris across %d bones\n", total_verts, total_tris, MARIO_BONE_COUNT);

  return 0;
}
