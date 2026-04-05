/**
 * jak_bridge_state.cpp - Lightweight state accessors for jak_bridge.
 *
 * These are in a separate translation unit from jak_bridge.cpp so that
 * referencing them doesn't pull in the heavy GOAL kernel dependencies.
 * This is critical for DLL loading in Blender — only this small .obj
 * gets linked when libjakopengoal.cpp references state accessors.
 */

#include "jak_bridge.h"

namespace jak_bridge {

/* Singleton state objects (extern'd in jak_bridge.cpp) */
CollisionState s_collision_state;
PadState s_pad_state;
JakInternalState s_jak_state;
MeshState s_mesh_state;
CommandQueue s_command_queue;
std::atomic<bool> s_runtime_ready{false};
BoneDebugData s_bone_debug;
TextureAtlasInfo s_texture_atlas_info;
uint8_t* s_texture_output = nullptr;
WaterState s_water_state;

/* Accessors */
CollisionState& get_collision_state() { return s_collision_state; }
PadState& get_pad_state() { return s_pad_state; }
JakInternalState& get_jak_state() { return s_jak_state; }
MeshState& get_mesh_state() { return s_mesh_state; }
CommandQueue& get_command_queue() { return s_command_queue; }
BoneDebugData& get_bone_debug_data() { return s_bone_debug; }
TextureAtlasInfo& get_texture_atlas_info() { return s_texture_atlas_info; }

WaterState& get_water_state() { return s_water_state; }

void set_water_level(float height) { s_water_state.height.store(height); }

bool is_runtime_ready() { return s_runtime_ready.load(); }
void set_runtime_ready(bool ready) { s_runtime_ready.store(ready); }

void set_texture_output(uint8_t* ptr) { s_texture_output = ptr; }
uint8_t* get_texture_output() { return s_texture_output; }

}  // namespace jak_bridge
