/**
 * libjakopengoal - A library for embedding Jak (from Jak and Daxter) into other engines.
 *
 * Inspired by libsm64, this library wraps the OpenGOAL runtime to expose Jak's
 * gameplay logic, collision, animation, and rendering data through a simple C API.
 *
 * Jak continues to run in the GOAL VM internally, but external code can:
 *   - Send static/dynamic collision geometry for Jak to interact with
 *   - Read Jak's state (position, velocity, health, animation)
 *   - Retrieve Jak's mesh data (skinned vertices) for rendering in another engine
 *   - Send inputs, damage, teleport commands, etc.
 *
 * Usage pattern (mirrors libsm64):
 *   1. jak_global_init(game_data_path)
 *   2. jak_static_surfaces_load(surfaces, count)
 *   3. jak_create(x, y, z) -> returns jak_id
 *   4. Each frame: jak_tick(jak_id, &inputs, &state, &geometry)
 *   5. jak_delete(jak_id)
 *   6. jak_global_terminate()
 */

#ifndef LIBJAKOPENGOAL_H
#define LIBJAKOPENGOAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#ifdef JAK_LIB_EXPORT
#define JAK_LIB_FN __declspec(dllexport)
#else
#define JAK_LIB_FN __declspec(dllimport)
#endif
#elif defined(__GNUC__) && __GNUC__ >= 4
#define JAK_LIB_FN __attribute__((visibility("default")))
#else
#define JAK_LIB_FN
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*  Constants                                                                 */
/* -------------------------------------------------------------------------- */

#define JAK_TEXTURE_WIDTH 1024
#define JAK_TEXTURE_HEIGHT 512
#define JAK_GEO_MAX_TRIANGLES 4096

/* Surface types (mirrors pat-surface material flags from GOAL) */
#define JAK_SURFACE_DEFAULT 0
#define JAK_SURFACE_ICE 1
#define JAK_SURFACE_STONE 2
#define JAK_SURFACE_WOOD 3
#define JAK_SURFACE_GRASS 4
#define JAK_SURFACE_SAND 5
#define JAK_SURFACE_METAL 6
#define JAK_SURFACE_DIRT 7

/* Jak action/state IDs (maps to GOAL target states) */
#define JAK_ACTION_IDLE 0
#define JAK_ACTION_WALK 1
#define JAK_ACTION_RUN 2
#define JAK_ACTION_JUMP 3
#define JAK_ACTION_DOUBLE_JUMP 4
#define JAK_ACTION_SPIN_ATTACK 5
#define JAK_ACTION_PUNCH 6
#define JAK_ACTION_DIVE 7
#define JAK_ACTION_ROLL 8
#define JAK_ACTION_CROUCH 9
#define JAK_ACTION_FALL 10
#define JAK_ACTION_HIT 11
#define JAK_ACTION_DEATH 12
#define JAK_ACTION_SWIM 13
#define JAK_ACTION_GROUND_POUND 14

/* Jak button flags (mirrors pad-buttons from GOAL) */
#define JAK_BUTTON_SELECT (1 << 0)
#define JAK_BUTTON_L3 (1 << 1)
#define JAK_BUTTON_R3 (1 << 2)
#define JAK_BUTTON_START (1 << 3)
#define JAK_BUTTON_DPAD_UP (1 << 4)
#define JAK_BUTTON_DPAD_RIGHT (1 << 5)
#define JAK_BUTTON_DPAD_DOWN (1 << 6)
#define JAK_BUTTON_DPAD_LEFT (1 << 7)
#define JAK_BUTTON_L2 (1 << 8)
#define JAK_BUTTON_R2 (1 << 9)
#define JAK_BUTTON_L1 (1 << 10)
#define JAK_BUTTON_R1 (1 << 11)
#define JAK_BUTTON_TRIANGLE (1 << 12)
#define JAK_BUTTON_CIRCLE (1 << 13)
#define JAK_BUTTON_X (1 << 14)
#define JAK_BUTTON_SQUARE (1 << 15)

/* -------------------------------------------------------------------------- */
/*  Callback types                                                            */
/* -------------------------------------------------------------------------- */

typedef void (*JakDebugPrintFunction)(const char* msg);
typedef void (*JakPlaySoundFunction)(uint32_t sound_id, float* pos);

/* -------------------------------------------------------------------------- */
/*  Data structures                                                           */
/* -------------------------------------------------------------------------- */

/**
 * A single collision triangle surface. Coordinates are in world space.
 * This mirrors SM64Surface but uses floats and Jak's surface type system.
 */
struct JakSurface {
  int16_t type;               /* Surface type (JAK_SURFACE_*) */
  int16_t flags;              /* Additional surface flags */
  float vertices[3][3];       /* 3 vertices, each with (x, y, z) */
  float normal[3];            /* Pre-computed face normal (set to 0 to auto-compute) */
};

/**
 * Transform for a dynamic surface object.
 */
struct JakObjectTransform {
  float position[3];          /* World position (x, y, z) */
  float rotation[3];          /* Euler angles in degrees (pitch, yaw, roll) — matches SM64 convention */
};

/**
 * A dynamic surface object (moving platform, etc).
 */
struct JakSurfaceObject {
  struct JakObjectTransform transform;
  uint32_t surface_count;
  struct JakSurface* surfaces;
};

/**
 * Per-frame inputs sent to Jak. Analog sticks are normalized [-1, 1].
 * Camera angles are used for movement-relative-to-camera (like SM64).
 */
struct JakInputs {
  float cam_x;                /* Camera look direction X (world space) */
  float cam_z;                /* Camera look direction Z (world space) */
  float stick_x;              /* Left analog stick X [-1.0, 1.0] */
  float stick_y;              /* Left analog stick Y [-1.0, 1.0] */
  float r_stick_x;            /* Right analog stick X [-1.0, 1.0] */
  float r_stick_y;            /* Right analog stick Y [-1.0, 1.0] */
  uint16_t buttons;           /* Button bitmask (JAK_BUTTON_*) */
};

/**
 * Output state of Jak, populated each tick.
 */
struct JakState {
  float position[3];          /* World position */
  float velocity[3];          /* World velocity (meters per tick) */
  float face_angle;           /* Y-axis rotation in radians */
  float forward_velocity;     /* Scalar speed along facing direction */
  float hp;                   /* Health (0.0 = dead, 1.0 = full) */
  int32_t eco_type;           /* Current eco power (-1 = none, 0=blue, 1=red, etc) */
  uint32_t action;            /* Current action ID (JAK_ACTION_*) */
  int32_t anim_id;            /* Current animation resource ID */
  int16_t anim_frame;         /* Current frame within animation */
  uint32_t flags;             /* Misc state flags */
  int32_t orb_count;          /* Collected precursor orbs */
  int32_t buzz_count;         /* Collected scout flies (per level) */
  int32_t cell_count;         /* Collected power cells */
  bool on_ground;             /* True if Jak is standing on ground */
  bool in_water;              /* True if Jak is in water */
};

/**
 * Geometry output buffers for rendering Jak. Caller allocates all buffers.
 * Vertex data is pre-skinned (bone transforms already applied).
 *
 * Buffer sizes must accommodate JAK_GEO_MAX_TRIANGLES * 3 vertices:
 *   position: float[JAK_GEO_MAX_TRIANGLES * 3 * 3]  (x,y,z per vertex)
 *   normal:   float[JAK_GEO_MAX_TRIANGLES * 3 * 3]  (nx,ny,nz per vertex)
 *   color:    float[JAK_GEO_MAX_TRIANGLES * 3 * 4]  (r,g,b,a per vertex)
 *   uv:       float[JAK_GEO_MAX_TRIANGLES * 3 * 2]  (u,v per vertex)
 */
struct JakGeometryBuffers {
  float* position;            /* Vertex positions (x,y,z) */
  float* normal;              /* Vertex normals (nx,ny,nz) */
  float* color;               /* Vertex colors (r,g,b,a) */
  float* uv;                  /* Texture coordinates (u,v) */
  uint16_t num_triangles_used;/* Number of triangles written this frame */
};

/**
 * Texture atlas info returned during initialization.
 * Contains Jak's textures packed into an atlas for the host renderer.
 */
struct JakTextureInfo {
  uint32_t width;
  uint32_t height;
  uint32_t num_textures;      /* Number of sub-textures in atlas */
};

/* -------------------------------------------------------------------------- */
/*  Lifecycle                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * Initialize the library. Boots the GOAL runtime headlessly.
 *
 * @param game_data_path  Path to the jak-project data directory (containing
 *                        iso_data/ and out/ with compiled game data).
 * @param out_texture     Output buffer for Jak's texture atlas (RGBA8).
 *                        Must be at least JAK_TEXTURE_WIDTH * JAK_TEXTURE_HEIGHT * 4 bytes.
 *                        May be NULL if you don't need textures yet.
 * @return                0 on success, negative on error.
 */
JAK_LIB_FN int32_t jak_global_init(const char* game_data_path, uint8_t* out_texture);

/**
 * Shut down the library and the GOAL runtime.
 */
JAK_LIB_FN void jak_global_terminate(void);

/**
 * Check if the runtime has finished booting and is ready.
 * After jak_global_init returns, the GOAL VM boots asynchronously.
 * Poll this until it returns true before calling other functions.
 */
JAK_LIB_FN bool jak_is_ready(void);

/* -------------------------------------------------------------------------- */
/*  Callbacks                                                                 */
/* -------------------------------------------------------------------------- */

JAK_LIB_FN void jak_register_debug_print(JakDebugPrintFunction fn);
JAK_LIB_FN void jak_register_play_sound(JakPlaySoundFunction fn);

/* -------------------------------------------------------------------------- */
/*  Static collision (level geometry)                                         */
/* -------------------------------------------------------------------------- */

/**
 * Load static collision surfaces. Call after jak_is_ready() returns true.
 * Replaces any previously loaded static surfaces.
 */
JAK_LIB_FN void jak_static_surfaces_load(const struct JakSurface* surfaces, uint32_t count);

/**
 * Clear all static collision surfaces.
 */
JAK_LIB_FN void jak_static_surfaces_clear(void);

/* -------------------------------------------------------------------------- */
/*  Dynamic surface objects                                                   */
/* -------------------------------------------------------------------------- */

JAK_LIB_FN uint32_t jak_surface_object_create(const struct JakSurfaceObject* obj);
JAK_LIB_FN void jak_surface_object_move(uint32_t obj_id, const struct JakObjectTransform* t);
JAK_LIB_FN void jak_surface_object_delete(uint32_t obj_id);

/* -------------------------------------------------------------------------- */
/*  Jak instance management                                                   */
/* -------------------------------------------------------------------------- */

/**
 * Spawn Jak at the given world position.
 * @return  A handle (>= 0) on success, negative on error.
 */
JAK_LIB_FN int32_t jak_create(float x, float y, float z);

/**
 * Remove Jak from the world.
 */
JAK_LIB_FN void jak_delete(int32_t jak_id);

/* -------------------------------------------------------------------------- */
/*  Per-frame tick                                                            */
/* -------------------------------------------------------------------------- */

/**
 * Advance Jak's simulation by one tick (1/60s).
 * Reads inputs, runs GOAL game logic, and outputs state + geometry.
 *
 * @param jak_id    Handle returned by jak_create.
 * @param inputs    Controller inputs for this tick. May be NULL for no input.
 * @param state     Output: Jak's state after this tick. May be NULL.
 * @param geometry  Output: Jak's mesh geometry for rendering. May be NULL.
 */
JAK_LIB_FN void jak_tick(int32_t jak_id,
                          const struct JakInputs* inputs,
                          struct JakState* state,
                          struct JakGeometryBuffers* geometry);

/* -------------------------------------------------------------------------- */
/*  State manipulation (setters)                                              */
/* -------------------------------------------------------------------------- */

JAK_LIB_FN void jak_set_position(int32_t jak_id, float x, float y, float z);
JAK_LIB_FN void jak_set_velocity(int32_t jak_id, float vx, float vy, float vz);
JAK_LIB_FN void jak_set_angle(int32_t jak_id, float yaw);
JAK_LIB_FN void jak_set_action(int32_t jak_id, uint32_t action);
JAK_LIB_FN void jak_set_health(int32_t jak_id, float hp);

JAK_LIB_FN void jak_take_damage(int32_t jak_id, float from_x, float from_y, float from_z);
JAK_LIB_FN void jak_heal(int32_t jak_id, float amount);
JAK_LIB_FN void jak_kill(int32_t jak_id);
JAK_LIB_FN void jak_give_eco(int32_t jak_id, int32_t eco_type);

/**
 * Set the water surface height at Jak's position (SM64/render units).
 * Set to -11000 (or lower) to indicate no water.
 */
JAK_LIB_FN void jak_set_water_level(float height);

/* -------------------------------------------------------------------------- */
/*  Collision queries (for host engine use)                                   */
/* -------------------------------------------------------------------------- */

/**
 * Find the ground height at (x, z) by querying Jak's collision system.
 * Returns the Y coordinate of the ground, or a very negative number if none.
 */
JAK_LIB_FN float jak_find_floor_height(float x, float y, float z);

/* -------------------------------------------------------------------------- */
/*  Skeleton / Bone queries                                                   */
/* -------------------------------------------------------------------------- */

/** Maximum bones returned by jak_get_bone_data. */
#define JAK_MAX_BONES 256

/**
 * Bone data for skeleton debug rendering.
 * Positions are in render/SM64 units (same coordinate space as JakState.position).
 */
struct JakBoneData {
  float positions[JAK_MAX_BONES][3];    /**< World-space bone positions [x,y,z] */
  int   parent_indices[JAK_MAX_BONES];  /**< Parent bone index (-1 = root/no parent) */
  int   num_bones;                      /**< Number of valid bones */
};

/**
 * Get the current skeleton bone data.
 * Returns true if bone data is valid (Jak is spawned and animated).
 */
JAK_LIB_FN bool jak_get_bone_data(struct JakBoneData* out);

/* -------------------------------------------------------------------------- */
/*  Utility                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * Get the texture info for Jak's character atlas.
 */
JAK_LIB_FN struct JakTextureInfo jak_get_texture_info(void);

#ifdef __cplusplus
}
#endif

#endif /* LIBJAKOPENGOAL_H */
