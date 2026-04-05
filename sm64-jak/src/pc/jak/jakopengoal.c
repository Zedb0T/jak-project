/**
 * jakopengoal.c - Jak and Daxter integration for SM64EX.
 *
 * Uses the full jakopengoal.dll (in-process GOAL runtime, same as mario_jak_test).
 * No subprocess, no IPC — the GOAL VM runs directly inside the SM64EX process.
 *
 * Dynamically loaded via LoadLibrary/GetProcAddress to avoid link-time deps
 * (SM64EX is MinGW-built; jakopengoal.dll is MSVC-built, C ABI compatible).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>

/* OpenGL for debug rendering */
#ifdef __MINGW32__
# define GLEW_STATIC
#endif
#include <GL/glew.h>
#include <GL/glu.h>
#include <SDL2/SDL.h>

#include "jakopengoal.h"

/* SM64 headers */
#include "sm64.h"
#include "types.h"
#include "game/level_update.h"
#include "game/game_init.h"
#include "game/camera.h"
#include "game/area.h"
#include "game/print.h"
#include "engine/math_util.h"
#include "engine/surface_collision.h"
#include "engine/surface_load.h"
#include "game/object_list_processor.h"
#include "engine/graph_node.h"
#include "game/save_file.h"
#include "game/interaction.h"
#include "object_fields.h"
#include "object_constants.h"
#include "surface_terrains.h"
#include "level_table.h"

/* Platform-specific dynamic loading */
#if defined(_WIN32)
#  include <windows.h>
#  define JAK_DLOPEN(lib)       LoadLibraryA(lib)
#  define JAK_DLSYM(h, func)   (void*)GetProcAddress((HMODULE)(h), func)
#  define JAK_DLCLOSE(h)        FreeLibrary((HMODULE)(h))
#  define JAK_DLERROR()         "LoadLibrary failed"
#  define JAK_DLL_NAME          "jakopengoal.dll"
#else
#  include <dlfcn.h>
#  define JAK_DLOPEN(lib)       dlopen(lib, RTLD_LAZY)
#  define JAK_DLSYM(h, func)   dlsym(h, func)
#  define JAK_DLCLOSE(h)        dlclose(h)
#  define JAK_DLERROR()         dlerror()
#  define JAK_DLL_NAME          "libjakopengoal.so"
#endif

/* Global held-object pointer — checked by cur_obj_disable_rendering() */
struct Object *g_jak_held_obj = NULL;

/* Punch frame counter for grab window */
static int s_punch_frame_count = 0;

/* Debug logging to file (Windows GUI apps don't have stdout) */
static FILE *s_log_file = NULL;
static void jak_log_init(void) {
    if (!s_log_file) {
        s_log_file = fopen("C:\\Users\\ZedBo\\jak_debug.log", "w");
    }
}
#define JAK_LOG(...) do { jak_log_init(); if(s_log_file) { fprintf(s_log_file, "[jak-sm64] "); fprintf(s_log_file, __VA_ARGS__); fprintf(s_log_file, "\n"); fflush(s_log_file); } } while(0)
#define JAK_ERR(...) do { jak_log_init(); if(s_log_file) { fprintf(s_log_file, "[jak-sm64 ERR] "); fprintf(s_log_file, __VA_ARGS__); fprintf(s_log_file, "\n"); fflush(s_log_file); } } while(0)

/* ---- libjakopengoal API structures (matching libjakopengoal.h) ---- */

struct JakSurface {
    int16_t type;   /* 0=default/stone, 1=ice, 2=quicksand, ... (maps to GOAL pat-material) */
    int16_t flags;
    float vertices[3][3];
    float normal[3];
};

/* GOAL pat-material values (must match libjakopengoal.h / pat-h.gc) */
#define JAK_PAT_STONE     0
#define JAK_PAT_ICE       1

struct JakInputs {
    float cam_x;
    float cam_z;
    float stick_x;
    float stick_y;
    float r_stick_x;
    float r_stick_y;
    uint16_t buttons;
};

struct JakState {
    float position[3];
    float velocity[3];
    float face_angle;
    float forward_velocity;
    float hp;
    int32_t eco_type;
    uint32_t action;
    int32_t anim_id;
    int16_t anim_frame;
    uint32_t flags;
    int32_t orb_count;
    int32_t buzz_count;
    int32_t cell_count;
    bool on_ground;
    bool in_water;
};

#define JAK_GEO_MAX_TRIANGLES 4096

struct JakGeometryBuffers {
    float *position;
    float *normal;
    float *color;
    float *uv;
    uint16_t num_triangles_used;
};

#define JAK_MAX_BONES 256

struct JakBoneData {
    float positions[JAK_MAX_BONES][3];
    int   parent_indices[JAK_MAX_BONES];
    int   num_bones;
};

struct JakTextureInfo {
    uint32_t width;
    uint32_t height;
    uint32_t num_textures;
};

/* Button flag constants */
#define JAK_BUTTON_X         (1 << 14)
#define JAK_BUTTON_SQUARE    (1 << 15)
#define JAK_BUTTON_CIRCLE    (1 << 13)
#define JAK_BUTTON_TRIANGLE  (1 << 12)
#define JAK_BUTTON_L1        (1 << 10)
#define JAK_BUTTON_R1        (1 << 11)

#define JAK_TEXTURE_WIDTH  1024
#define JAK_TEXTURE_HEIGHT 512

/* SM64 button constants */
#define SM64_A_BUTTON   0x8000
#define SM64_B_BUTTON   0x4000
#define SM64_Z_TRIG     0x2000
#define SM64_L_TRIG     0x0020
#define SM64_R_TRIG     0x0010

/* ---- Function pointer typedefs ---- */

typedef int32_t (*pfn_jak_global_init)(const char*, uint8_t*);
typedef void    (*pfn_jak_global_terminate)(void);
typedef bool    (*pfn_jak_is_ready)(void);
typedef void    (*pfn_jak_static_surfaces_load)(const struct JakSurface*, uint32_t);
typedef void    (*pfn_jak_static_surfaces_clear)(void);
typedef int32_t (*pfn_jak_create)(float, float, float);
typedef void    (*pfn_jak_delete)(int32_t);
typedef void    (*pfn_jak_tick)(int32_t, const struct JakInputs*, struct JakState*, struct JakGeometryBuffers*);
typedef void    (*pfn_jak_set_position)(int32_t, float, float, float);
typedef bool    (*pfn_jak_get_bone_data)(struct JakBoneData*);
typedef struct JakTextureInfo (*pfn_jak_get_texture_info)(void);

/* ---- Module state ---- */

static void *s_dll_handle = NULL;
static int32_t s_jak_id = -1;
static bool s_runtime_ready = false;
static bool s_active = false;
static int s_boot_wait_frames = 0;
static bool s_init_attempted = false;
static bool s_init_failed = false;
static int s_deferred_wait = 0;
static bool s_auto_warped = false;

/* Function pointers */
static pfn_jak_global_init         fn_jak_global_init = NULL;
static pfn_jak_global_terminate    fn_jak_global_terminate = NULL;
static pfn_jak_is_ready            fn_jak_is_ready = NULL;
static pfn_jak_static_surfaces_load fn_jak_static_surfaces_load = NULL;
static pfn_jak_static_surfaces_clear fn_jak_static_surfaces_clear = NULL;
static pfn_jak_create              fn_jak_create = NULL;
static pfn_jak_delete              fn_jak_delete = NULL;
static pfn_jak_tick                fn_jak_tick = NULL;
static pfn_jak_set_position        fn_jak_set_position = NULL;
static pfn_jak_get_bone_data       fn_jak_get_bone_data = NULL;
static pfn_jak_get_texture_info    fn_jak_get_texture_info = NULL;

/* Geometry buffers (heap-allocated) */
static float *s_geo_position = NULL;
static float *s_geo_normal = NULL;
static float *s_geo_color = NULL;
static float *s_geo_uv = NULL;

/* Texture atlas */
static uint8_t *s_texture_atlas = NULL;
static GLuint s_jak_texture_id = 0;
static bool s_texture_uploaded = false;
static bool s_draw_wireframe_overlay = false; /* toggle bone overlay on top of mesh */
static bool s_draw_debug_hud = false;        /* toggle on-screen position/debug text */

static struct JakState s_jak_state;
static struct JakGeometryBuffers s_jak_geo;

/* SM64 collision conversion */
#define MAX_JAK_SURFACES 8192
static struct JakSurface *s_jak_surfaces = NULL;
static int s_jak_surface_count = 0;
static bool s_surfaces_loaded = false;
static s32 s_last_level_area = -1;  /* gCurrLevelNum * 16 + gCurrAreaIndex */

/* ---- Helpers ---- */

static bool load_dll(void) {
    const char *paths[] = {
        JAK_DLL_NAME,
        "lib/jakopengoal/" JAK_DLL_NAME,
        "lib/" JAK_DLL_NAME,
        NULL
    };

    for (int i = 0; paths[i]; i++) {
        JAK_LOG("Trying to load: %s", paths[i]);
        s_dll_handle = JAK_DLOPEN(paths[i]);
        if (s_dll_handle) {
            JAK_LOG("Loaded %s from: %s", JAK_DLL_NAME, paths[i]);
            return true;
        }
#if defined(_WIN32)
        DWORD err = GetLastError();
        JAK_ERR("  LoadLibrary('%s') failed, GetLastError=%lu (0x%lx)", paths[i], err, err);
#endif
    }

    JAK_ERR("Failed to load %s: %s", JAK_DLL_NAME, JAK_DLERROR());
    return false;
}

static bool resolve_functions(void) {
    #define RESOLVE(name) do { \
        fn_##name = (pfn_##name)JAK_DLSYM(s_dll_handle, #name); \
        if (!fn_##name) { \
            JAK_ERR("Missing symbol: %s", #name); \
            return false; \
        } \
    } while(0)

    RESOLVE(jak_global_init);
    RESOLVE(jak_global_terminate);
    RESOLVE(jak_is_ready);
    RESOLVE(jak_static_surfaces_load);
    RESOLVE(jak_static_surfaces_clear);
    RESOLVE(jak_create);
    RESOLVE(jak_delete);
    RESOLVE(jak_tick);
    RESOLVE(jak_set_position);

    #undef RESOLVE

    /* Optional symbols — won't fail if missing */
    fn_jak_get_bone_data = (pfn_jak_get_bone_data)JAK_DLSYM(s_dll_handle, "jak_get_bone_data");
    if (!fn_jak_get_bone_data) JAK_LOG("jak_get_bone_data not available (optional)");
    fn_jak_get_texture_info = (pfn_jak_get_texture_info)JAK_DLSYM(s_dll_handle, "jak_get_texture_info");
    if (!fn_jak_get_texture_info) JAK_LOG("jak_get_texture_info not available (optional)");
    JAK_LOG("All function pointers resolved OK");
    return true;
}

static void alloc_buffers(void) {
    if (s_geo_position) return;
    s_geo_position = (float*)calloc(JAK_GEO_MAX_TRIANGLES * 3 * 3, sizeof(float));
    s_geo_normal   = (float*)calloc(JAK_GEO_MAX_TRIANGLES * 3 * 3, sizeof(float));
    s_geo_color    = (float*)calloc(JAK_GEO_MAX_TRIANGLES * 3 * 4, sizeof(float));
    s_geo_uv       = (float*)calloc(JAK_GEO_MAX_TRIANGLES * 3 * 2, sizeof(float));
    s_jak_surfaces = (struct JakSurface*)calloc(MAX_JAK_SURFACES, sizeof(struct JakSurface));
    s_texture_atlas = (uint8_t*)calloc(JAK_TEXTURE_WIDTH * JAK_TEXTURE_HEIGHT * 4, 1);
}

static void free_buffers(void) {
    free(s_geo_position); s_geo_position = NULL;
    free(s_geo_normal);   s_geo_normal = NULL;
    free(s_geo_color);    s_geo_color = NULL;
    free(s_geo_uv);       s_geo_uv = NULL;
    free(s_jak_surfaces); s_jak_surfaces = NULL;
    free(s_texture_atlas); s_texture_atlas = NULL;
    if (s_jak_texture_id) {
        glDeleteTextures(1, &s_jak_texture_id);
        s_jak_texture_id = 0;
    }
    s_texture_uploaded = false;
}

/**
 * Convert SM64's loaded collision surfaces to JakSurface format.
 */
static void convert_sm64_surfaces(void) {
    extern struct Surface *sSurfacePool;
    extern s32 gSurfacesAllocated;

    s_jak_surface_count = 0;

    if (!sSurfacePool || gSurfacesAllocated <= 0) {
        return;
    }

    uint32_t count = (uint32_t)gSurfacesAllocated;
    if (count > MAX_JAK_SURFACES) count = MAX_JAK_SURFACES;

    for (uint32_t i = 0; i < count; i++) {
        struct Surface *s = &sSurfacePool[i];

        /* Skip degenerate surfaces (zero normal = uninitialized or degenerate tri) */
        if (s->normal.x == 0.0f && s->normal.y == 0.0f && s->normal.z == 0.0f) continue;

        struct JakSurface *js = &s_jak_surfaces[s_jak_surface_count];

        js->type = JAK_PAT_STONE;

        /* Determine SM64 steep threshold for this surface type.
         * Uses mario_floor_is_steep() thresholds — the point where
         * Mario loses control and slides uncontrollably. */
        float slope_thresh;
        switch (s->type) {
            case SURFACE_VERY_SLIPPERY:
            case SURFACE_ICE:
            case SURFACE_HARD_VERY_SLIPPERY:
            case SURFACE_NOISE_VERY_SLIPPERY_73:
            case SURFACE_NOISE_VERY_SLIPPERY_74:
            case SURFACE_NOISE_VERY_SLIPPERY:
            case SURFACE_NO_CAM_COL_VERY_SLIPPERY:
                slope_thresh = 0.9659258f;  /* cos(15°) */
                break;
            case SURFACE_SLIPPERY:
            case SURFACE_NOISE_SLIPPERY:
            case SURFACE_HARD_SLIPPERY:
            case SURFACE_NO_CAM_COL_SLIPPERY:
                slope_thresh = 0.9396926f;  /* cos(20°) */
                break;
            default:
                slope_thresh = 0.8660254f;  /* cos(30°) */
                break;
        }

        /* Slide terrain override (e.g. slide courses) */
        if (gCurrentArea && (gCurrentArea->terrainType & TERRAIN_MASK) == TERRAIN_SLIDE) {
            slope_thresh = 0.9998477f;  /* cos(1°) — almost any tilt slides */
        }

        /* flags field: 0 = ground mode, 1 = wall mode (in GOAL pat-surface) */
        if (s->normal.y <= slope_thresh && s->normal.y > 0.05f) {
            js->flags = 1;  /* wall mode — Jak slides off */
        } else {
            js->flags = 0;  /* ground mode — Jak can walk */
        }

        js->vertices[0][0] = (float)s->vertex1[0];
        js->vertices[0][1] = (float)s->vertex1[1];
        js->vertices[0][2] = (float)s->vertex1[2];

        js->vertices[1][0] = (float)s->vertex2[0];
        js->vertices[1][1] = (float)s->vertex2[1];
        js->vertices[1][2] = (float)s->vertex2[2];

        js->vertices[2][0] = (float)s->vertex3[0];
        js->vertices[2][1] = (float)s->vertex3[1];
        js->vertices[2][2] = (float)s->vertex3[2];

        js->normal[0] = s->normal.x;
        js->normal[1] = s->normal.y;
        js->normal[2] = s->normal.z;

        s_jak_surface_count++;
    }
}

/**
 * SDL gamepad — read circle/triangle directly since SM64's N64-style
 * button system doesn't map them.
 */
static SDL_GameController *s_sdl_pad = NULL;

static SDL_GameController *get_sdl_gamepad(void) {
    if (s_sdl_pad && SDL_GameControllerGetAttached(s_sdl_pad)) return s_sdl_pad;
    s_sdl_pad = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            s_sdl_pad = SDL_GameControllerOpen(i);
            if (s_sdl_pad) break;
        }
    }
    return s_sdl_pad;
}

/**
 * Build JakInputs from SM64's controller state.
 */
static void build_jak_inputs(struct JakInputs *inputs) {
    memset(inputs, 0, sizeof(*inputs));

    if (!gPlayer1Controller) return;
    struct Controller *ctrl = gPlayer1Controller;

    inputs->stick_x = -(ctrl->stickX / 64.0f);
    inputs->stick_y = ctrl->stickY / 64.0f;

    float dx = gLakituState.curFocus[0] - gLakituState.curPos[0];
    float dz = gLakituState.curFocus[2] - gLakituState.curPos[2];
    float len = sqrtf(dx * dx + dz * dz);
    if (len > 0.01f) {
        inputs->cam_x = dx / len;
        inputs->cam_z = dz / len;
    } else {
        inputs->cam_x = 0.0f;
        inputs->cam_z = -1.0f;
    }

    uint16_t jak_buttons = 0;
    if (ctrl->buttonDown & SM64_A_BUTTON) jak_buttons |= JAK_BUTTON_X;
    if (ctrl->buttonDown & SM64_B_BUTTON) jak_buttons |= JAK_BUTTON_SQUARE;
    if (ctrl->buttonDown & SM64_L_TRIG)   jak_buttons |= JAK_BUTTON_L1;
    if (ctrl->buttonDown & SM64_R_TRIG)   jak_buttons |= JAK_BUTTON_R1;

    /* Read circle and triangle directly from SDL — they have no N64 mapping */
    SDL_GameController *pad = get_sdl_gamepad();
    if (pad) {
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B))
            jak_buttons |= JAK_BUTTON_CIRCLE;
        if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y))
            jak_buttons |= JAK_BUTTON_TRIANGLE;
    }

    inputs->buttons = jak_buttons;
}

/* ---- Deferred init ---- */

static void deferred_init(void) {
    if (s_init_attempted) return;
    s_init_attempted = true;

    JAK_LOG("=== Deferred init starting (direct DLL, no server) ===");

    alloc_buffers();
    if (!s_geo_position || !s_jak_surfaces) {
        JAK_ERR("Failed to allocate buffers");
        s_init_failed = true;
        return;
    }

    if (!load_dll()) {
        s_init_failed = true;
        return;
    }

    if (!resolve_functions()) {
        JAK_DLCLOSE(s_dll_handle);
        s_dll_handle = NULL;
        s_init_failed = true;
        return;
    }

    const char *jak_path = getenv("JAK_DATA_PATH");
    if (!jak_path || !jak_path[0]) {
        JAK_ERR("JAK_DATA_PATH not set! Set it to your jak-project directory.");
        JAK_DLCLOSE(s_dll_handle);
        s_dll_handle = NULL;
        s_init_failed = true;
        return;
    }

    JAK_LOG("Booting GOAL runtime (in-process) with data: %s", jak_path);

    int32_t ret = fn_jak_global_init(jak_path, s_texture_atlas);
    if (ret < 0) {
        JAK_ERR("jak_global_init failed (error %d)", ret);
        JAK_DLCLOSE(s_dll_handle);
        s_dll_handle = NULL;
        s_init_failed = true;
        return;
    }

    s_boot_wait_frames = 0;
    s_runtime_ready = false;

    s_jak_geo.position = s_geo_position;
    s_jak_geo.normal = s_geo_normal;
    s_jak_geo.color = s_geo_color;
    s_jak_geo.uv = s_geo_uv;
    s_jak_geo.num_triangles_used = 0;

    JAK_LOG("=== Init complete, waiting for GOAL VM to boot ===");
}

/* ---- Public API ---- */

bool jak_is_loaded(void) {
    return s_dll_handle != NULL && s_runtime_ready;
}

bool jak_is_active(void) {
    return s_active && s_jak_id >= 0;
}

void jak_sm64_init(void) {
    JAK_LOG("Init registered (deferred until game is running)");
}

void jak_sm64_shutdown(void) {
    JAK_LOG("Shutting down...");

    if (s_jak_id >= 0 && fn_jak_delete) {
        fn_jak_delete(s_jak_id);
        s_jak_id = -1;
    }

    if (fn_jak_global_terminate) {
        fn_jak_global_terminate();
    }

    if (s_dll_handle) {
        JAK_DLCLOSE(s_dll_handle);
        s_dll_handle = NULL;
    }

    free_buffers();

    s_runtime_ready = false;
    s_active = false;
    s_surfaces_loaded = false;
    s_init_attempted = false;
    s_init_failed = false;

    JAK_LOG("Shutdown complete");
}

void jak_sm64_toggle(void) {
    if (!s_dll_handle) {
        deferred_init();
        return;
    }

    if (s_active && s_jak_id >= 0) {
        fn_jak_delete(s_jak_id);
        s_jak_id = -1;
        s_active = false;
        JAK_LOG("Jak removed");
    } else if (s_runtime_ready) {
        struct MarioState *m = &gMarioStates[0];
        s_jak_id = fn_jak_create(m->pos[0], m->pos[1], m->pos[2]);
        if (s_jak_id >= 0) {
            s_active = true;
            JAK_LOG("Jak spawned at (%.0f, %.0f, %.0f)", m->pos[0], m->pos[1], m->pos[2]);
        }
    }
}

void jak_sm64_update(void) {
    /* Deferred init: wait until a level is loaded */
    if (!s_init_attempted && !s_init_failed) {
        s_deferred_wait++;
        if (s_deferred_wait >= 120 && gCurrentArea != NULL) {
            deferred_init();
        }
        return;
    }

    if (s_init_failed || !s_dll_handle) return;

    /* Wait for GOAL VM to boot */
    if (!s_runtime_ready) {
        s_boot_wait_frames++;
        if (fn_jak_is_ready && fn_jak_is_ready()) {
            s_runtime_ready = true;
            JAK_LOG("GOAL runtime ready after %d frames!", s_boot_wait_frames);
            /* Don't spawn yet — wait until we have collision surfaces (a real level) */
        } else if (s_boot_wait_frames % 120 == 0) {
            JAK_LOG("Still waiting for GOAL VM... (%d frames)", s_boot_wait_frames);
        }
        return;
    }

    extern s32 gSurfacesAllocated;

    /* Detect level/area change — destroy Jak & reload surfaces */
    if (gCurrentArea != NULL) {
        s32 cur_la = (s32)gCurrLevelNum * 16 + (s32)gCurrAreaIndex;
        if (s_last_level_area != cur_la) {
            JAK_LOG("Level/area changed (%d -> %d)", s_last_level_area, cur_la);

            /* Destroy Jak so he stops ticking with stale collision */
            if (s_active && s_jak_id >= 0) {
                fn_jak_delete(s_jak_id);
                s_jak_id = -1;
                s_active = false;
                JAK_LOG("  Destroyed Jak for area transition");
            }

            /* Clear old surfaces */
            if (s_surfaces_loaded) {
                s_surfaces_loaded = false;
                if (fn_jak_static_surfaces_clear) fn_jak_static_surfaces_clear();
                JAK_LOG("  Cleared old collision surfaces");
            }

            s_last_level_area = cur_la;
        }
    }

    /* Re-sync collision surfaces every frame from SM64's live surface pool */
    {
        extern struct Surface *sSurfacePool;
        if (gCurrentArea != NULL && sSurfacePool != NULL && gSurfacesAllocated > 0) {
            if (fn_jak_static_surfaces_clear) fn_jak_static_surfaces_clear();
            convert_sm64_surfaces();
            if (s_jak_surface_count > 0) {
                fn_jak_static_surfaces_load(s_jak_surfaces, s_jak_surface_count);
                s_surfaces_loaded = true;
            } else {
                s_surfaces_loaded = false;
            }
        } else if (s_surfaces_loaded) {
            if (fn_jak_static_surfaces_clear) fn_jak_static_surfaces_clear();
            s_surfaces_loaded = false;
        }
    }

    /* Per-frame diagnostic: log collision & Jak state every 30 frames */
    {
        static int diag_frame = 0;
        diag_frame++;
        if (diag_frame <= 10 || diag_frame % 30 == 0) {
            JAK_LOG("DIAG #%d: level=%d area=%d gSurfAlloc=%d jakSurf=%d loaded=%d active=%d id=%d",
                    diag_frame, (int)gCurrLevelNum, (int)gCurrAreaIndex,
                    (int)gSurfacesAllocated, s_jak_surface_count,
                    s_surfaces_loaded, s_active, s_jak_id);
            if (s_active && s_jak_id >= 0) {
                JAK_LOG("  jak_pos=(%.1f,%.1f,%.1f) on_ground=%d action=%u",
                        s_jak_state.position[0], s_jak_state.position[1], s_jak_state.position[2],
                        s_jak_state.on_ground, s_jak_state.action);
            }
            struct MarioState *m = &gMarioStates[0];
            JAK_LOG("  mario_pos=(%.1f,%.1f,%.1f) gCurrentArea=%p",
                    m->pos[0], m->pos[1], m->pos[2], (void*)gCurrentArea);
        }
    }

    /* Auto-spawn/re-spawn Jak once surfaces are loaded and we're in a level */
    if (!s_active && s_surfaces_loaded && gCurrentArea != NULL) {
        struct MarioState *m = &gMarioStates[0];
        /* Skip if Mario is at origin (likely still loading) */
        if (m->pos[0] == 0.0f && m->pos[1] == 0.0f && m->pos[2] == 0.0f) return;

        JAK_LOG("Spawning Jak at Mario pos (%.0f, %.0f, %.0f)",
                m->pos[0], m->pos[1], m->pos[2]);
        s_jak_id = fn_jak_create(m->pos[0], m->pos[1], m->pos[2]);
        if (s_jak_id >= 0) {
            s_active = true;
            JAK_LOG("Jak spawned (id=%d)", s_jak_id);
        } else {
            JAK_ERR("Failed to spawn Jak");
        }
        return;
    }

    /* Don't tick Jak if surfaces aren't loaded yet (prevents falling through void) */
    if (!s_surfaces_loaded) return;
    if (!s_active || s_jak_id < 0) return;

    /* Freeze Jak entirely during warp transitions (ACT_DISAPPEARED).
     * Jak must not tick or move while SM64 is unloading/loading areas —
     * otherwise he runs through the painting into void geometry and GOAL crashes.
     * Pin his position in GOAL each frame so the VM thread doesn't let him fall. */
    {
        struct MarioState *m = &gMarioStates[0];
        if (m->action == ACT_DISAPPEARED) {
            if (fn_jak_set_position) {
                fn_jak_set_position(s_jak_id,
                    s_jak_state.position[0],
                    s_jak_state.position[1],
                    s_jak_state.position[2]);
            }
            return;
        }
    }

    /* Build inputs from SM64 controller */
    struct JakInputs inputs;
    build_jak_inputs(&inputs);

    /* Tick Jak */
    s_jak_geo.num_triangles_used = 0;
    fn_jak_tick(s_jak_id, &inputs, &s_jak_state, &s_jak_geo);

    /* --- Keep Mario at Jak's position every frame ---
     * This serves two purposes:
     *   1) SM64 camera follows gMarioStates[0].pos, so it will track Jak.
     *   2) SM64 load triggers, enemies, etc. react to Mario's position.
     */
    {
        struct MarioState *m = &gMarioStates[0];
        m->pos[0] = s_jak_state.position[0];
        m->pos[1] = s_jak_state.position[1];
        m->pos[2] = s_jak_state.position[2];

        /* Hide Mario's model — Jak is the player character.
         * Also sync marioObj position so enemies/interactions detect us. */
        if (gMarioObject) {
            gMarioObject->header.gfx.node.flags |= GRAPH_RENDER_INVISIBLE;
            gMarioObject->oPosX = s_jak_state.position[0];
            gMarioObject->oPosY = s_jak_state.position[1];
            gMarioObject->oPosZ = s_jak_state.position[2];
            gMarioObject->header.gfx.pos[0] = s_jak_state.position[0];
            gMarioObject->header.gfx.pos[1] = s_jak_state.position[1];
            gMarioObject->header.gfx.pos[2] = s_jak_state.position[2];
        }
    }

    /* --- Punch-to-grab: let Jak grab SM64 objects during first 0.25s of punch --- */
    if (gCurrentArea == NULL) return;  /* level not loaded — skip all interaction code */

    {
        static uint32_t prev_jak_action = 0;
        struct MarioState *m = &gMarioStates[0];

        /* Detect punch start */
        if (s_jak_state.action == 6 /* JAK_ACTION_PUNCH */ && prev_jak_action != 6) {
            s_punch_frame_count = 0;
            JAK_LOG("PUNCH: Jak started punching");
        }

        if (s_jak_state.action == 6 /* JAK_ACTION_PUNCH */) {
            s_punch_frame_count++;

            /* First 15 frames (~0.25s at 60fps) — enable grab window */
            if (s_punch_frame_count <= 15) {
                /* Don't override if Mario is already grabbing/holding */
                if (m->action != ACT_PICKING_UP && m->heldObj == NULL) {
                    m->action = ACT_PUNCHING;
                    m->actionArg = 0;
                    m->actionTimer = 0;
                    m->flags |= MARIO_PUNCHING;
                    /* Sync face angle from Jak (radians -> SM64 s16 angle) */
                    m->faceAngle[1] = (s16)(s_jak_state.face_angle * 32768.0f / 3.14159265f);

                    if (s_punch_frame_count == 1) {
                        JAK_LOG("PUNCH: Set Mario to ACT_PUNCHING faceAngle=%d", (int)m->faceAngle[1]);
                    }
                }
            }
        } else {
            if (prev_jak_action == 6 && s_punch_frame_count > 0) {
                JAK_LOG("PUNCH: Jak stopped punching after %d frames", s_punch_frame_count);
            }
            s_punch_frame_count = 0;
        }

        prev_jak_action = s_jak_state.action;
    }

    /* --- Spin attack: damage nearby SM64 objects when Jak is spinning --- */
    {
        static int s_spin_frame_count = 0;
        static uint32_t prev_spin_action = 0;

        if (s_jak_state.action == 5 /* JAK_ACTION_SPIN */ && prev_spin_action != 5) {
            s_spin_frame_count = 0;
            JAK_LOG("SPIN: Jak started spin attack");
        }

        if (s_jak_state.action == 5 /* JAK_ACTION_SPIN */) {
            s_spin_frame_count++;

            /* 18-frame attack window (0.3s at 60fps, matches GOAL spin duration) */
            if (s_spin_frame_count <= 18 && gObjectLists != NULL) {
                struct MarioState *m = &gMarioStates[0];
                float jak_x = m->pos[0];
                float jak_y = m->pos[1];
                float jak_z = m->pos[2];

                /* Scan object lists that contain attackable things:
                 *   DESTRUCTIVE(2), GENACTOR(4), PUSHABLE(5), SURFACE(9) */
                int lists_to_scan[] = { OBJ_LIST_DESTRUCTIVE, OBJ_LIST_GENACTOR,
                                        OBJ_LIST_PUSHABLE, OBJ_LIST_SURFACE };
                int hit_count = 0;

                for (int li = 0; li < 4; li++) {
                    struct ObjectNode *list = &gObjectLists[lists_to_scan[li]];
                    struct ObjectNode *node = list->next;

                    while (node != list) {
                        struct Object *obj = (struct Object *)node;
                        node = node->next;

                        if (obj->activeFlags == ACTIVE_FLAG_DEACTIVATED) continue;
                        if (obj->oInteractType == 0) continue;

                        /* Distance check: Jak spin radius = 2.2 GOAL meters = 110 SM64 units */
                        float dx = obj->oPosX - jak_x;
                        float dy = obj->oPosY - (jak_y + 80.0f); /* 1.6m Y offset for spin center */
                        float dz = obj->oPosZ - jak_z;
                        float dist_sq = dx*dx + dy*dy + dz*dz;

                        if (dist_sq < 110.0f * 110.0f) {
                            obj->oInteractStatus = INT_STATUS_INTERACTED | INT_STATUS_WAS_ATTACKED
                                                 | ATTACK_KICK_OR_TRIP;
                            hit_count++;
                            if (s_spin_frame_count == 1) {
                                JAK_LOG("SPIN: Hit object at (%.0f,%.0f,%.0f) type=0x%x dist=%.0f",
                                        obj->oPosX, obj->oPosY, obj->oPosZ,
                                        obj->oInteractType, sqrtf(dist_sq));
                            }
                        }
                    }
                }

                if (hit_count > 0 && s_spin_frame_count == 1) {
                    JAK_LOG("SPIN: Hit %d objects on first frame", hit_count);
                }
            }
        } else {
            if (prev_spin_action == 5 && s_spin_frame_count > 0) {
                JAK_LOG("SPIN: Spin ended after %d frames", s_spin_frame_count);
            }
            s_spin_frame_count = 0;
        }

        prev_spin_action = s_jak_state.action;
    }

    /* --- Ground pound: set Mario to ACT_GROUND_POUND_LAND so bosses detect it --- */
    {
        struct MarioState *m = &gMarioStates[0];

        if (s_jak_state.action == 14 /* JAK_ACTION_GROUND_POUND */) {
            m->action = ACT_GROUND_POUND_LAND;
            m->actionState = 0;
            m->actionTimer = 0;
            m->vel[1] = -50.0f;  /* downward velocity for impact feel */
            m->faceAngle[1] = (s16)(s_jak_state.face_angle * 32768.0f / 3.14159265f);
        }
    }

    /* --- Carry held objects above Jak, throw on next punch --- */
    {
        struct MarioState *m = &gMarioStates[0];

        if (m->heldObj != NULL) {
            g_jak_held_obj = m->heldObj;

            /* Position held object above Jak. These values are consumed by
             * the NEXT frame's behavior/display-list build (1-frame lag, invisible at 60fps). */
            m->heldObj->oPosX = m->pos[0];
            m->heldObj->oPosY = m->pos[1] + 200.0f;
            m->heldObj->oPosZ = m->pos[2];
            m->heldObj->header.gfx.pos[0] = m->pos[0];
            m->heldObj->header.gfx.pos[1] = m->pos[1] + 200.0f;
            m->heldObj->header.gfx.pos[2] = m->pos[2];

            /* Keep HOLP updated for throw positioning */
            m->marioBodyState->heldObjLastPosition[0] = m->pos[0];
            m->marioBodyState->heldObjLastPosition[1] = m->pos[1] + 200.0f;
            m->marioBodyState->heldObjLastPosition[2] = m->pos[2];

            /* Re-enable rendering — behavior tries to hide it every frame */
            m->heldObj->header.gfx.node.flags |= GRAPH_RENDER_ACTIVE;

            /* Throw on next punch while holding */
            if (s_jak_state.action == 6 /* JAK_ACTION_PUNCH */ && s_punch_frame_count == 1) {
                JAK_LOG("GRAB: Throwing held object %p!", (void*)m->heldObj);
                mario_throw_held_object(m);
                g_jak_held_obj = NULL;
                m->action = ACT_IDLE;
            }

            static int hold_log = 0;
            if (hold_log++ % 60 == 0) {
                JAK_LOG("GRAB: Carrying %p at (%.0f,%.0f,%.0f)",
                        (void*)m->heldObj, m->pos[0], m->pos[1] + 200.0f, m->pos[2]);
            }
        } else {
            g_jak_held_obj = NULL;
        }
    }

    /* Autosave every 5 minutes (18000 ticks at 60fps) */
    static int tick_count = 0;
    tick_count++;
    if (tick_count % 18000 == 0) {
        save_file_do_save(gCurrSaveFileNum - 1);
        JAK_LOG("Autosave triggered at tick %d", tick_count);
    }

    /* Log state and inputs periodically */
    if (tick_count <= 5 || tick_count % 300 == 0) {
        struct MarioState *m = &gMarioStates[0];
        JAK_LOG("Tick #%d: jak_pos=(%.0f,%.0f,%.0f) mario_pos=(%.0f,%.0f,%.0f)",
                tick_count,
                s_jak_state.position[0], s_jak_state.position[1], s_jak_state.position[2],
                m->pos[0], m->pos[1], m->pos[2]);
        JAK_LOG("  vel=(%.1f,%.1f,%.1f) fwd_vel=%.1f action=%d ground=%d",
                s_jak_state.velocity[0], s_jak_state.velocity[1], s_jak_state.velocity[2],
                s_jak_state.forward_velocity, s_jak_state.action, s_jak_state.on_ground);
        JAK_LOG("  inputs: stick=(%.2f,%.2f) cam_dir=(%.3f,%.3f) buttons=0x%04x",
                inputs.stick_x, inputs.stick_y, inputs.cam_x, inputs.cam_z, inputs.buttons);
        JAK_LOG("  sm64_raw: stickX=%.1f stickY=%.1f",
                gPlayer1Controller ? gPlayer1Controller->stickX : 0.0f,
                gPlayer1Controller ? gPlayer1Controller->stickY : 0.0f);
        JAK_LOG("  lakitu: pos=(%.0f,%.0f,%.0f) focus=(%.0f,%.0f,%.0f)",
                gLakituState.curPos[0], gLakituState.curPos[1], gLakituState.curPos[2],
                gLakituState.curFocus[0], gLakituState.curFocus[1], gLakituState.curFocus[2]);
    }
}

/* ---- Skeleton bone rendering ---- */

#define BONE_JOINT_RADIUS 8.0f

/**
 * Draw a small octahedron at a world position (bone joint marker).
 */
static void draw_octahedron(float cx, float cy, float cz, float r,
                            float red, float grn, float blu, float alpha) {
    float top[3] = { cx,     cy + r, cz     };
    float bot[3] = { cx,     cy - r, cz     };
    float frt[3] = { cx,     cy,     cz + r };
    float bck[3] = { cx,     cy,     cz - r };
    float lft[3] = { cx - r, cy,     cz     };
    float rgt[3] = { cx + r, cy,     cz     };

    glColor4f(red, grn, blu, alpha);
    glBegin(GL_TRIANGLES);
    glVertex3fv(top); glVertex3fv(frt); glVertex3fv(rgt);
    glVertex3fv(top); glVertex3fv(rgt); glVertex3fv(bck);
    glVertex3fv(top); glVertex3fv(bck); glVertex3fv(lft);
    glVertex3fv(top); glVertex3fv(lft); glVertex3fv(frt);
    glVertex3fv(bot); glVertex3fv(rgt); glVertex3fv(frt);
    glVertex3fv(bot); glVertex3fv(bck); glVertex3fv(rgt);
    glVertex3fv(bot); glVertex3fv(lft); glVertex3fv(bck);
    glVertex3fv(bot); glVertex3fv(frt); glVertex3fv(lft);
    glEnd();
}

/* Cached bone data — refreshed each frame */
static struct JakBoneData s_bone_data;
static bool s_bones_available = false;

/**
 * Upload the texture atlas to an OpenGL texture (once).
 * Called lazily the first time we have valid texture info.
 */
static void upload_texture_atlas(void) {
    if (s_texture_uploaded || !s_texture_atlas) return;
    if (!fn_jak_get_texture_info) return;

    struct JakTextureInfo ti = fn_jak_get_texture_info();
    if (ti.num_textures == 0) return;

    JAK_LOG("Uploading texture atlas: %ux%u, %u sub-textures",
            ti.width, ti.height, ti.num_textures);

    /* Check if atlas has any non-zero pixel data */
    int nonzero = 0;
    for (uint32_t i = 0; i < ti.width * ti.height * 4 && i < 1024; i++) {
        if (s_texture_atlas[i] != 0) { nonzero++; }
    }
    JAK_LOG("  Atlas data check: %d non-zero bytes in first 1024", nonzero);

    if (!s_jak_texture_id) {
        glGenTextures(1, &s_jak_texture_id);
    }
    glBindTexture(GL_TEXTURE_2D, s_jak_texture_id);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ti.width, ti.height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, s_texture_atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    s_texture_uploaded = true;
    JAK_LOG("  GL texture ID = %u", s_jak_texture_id);
}

void jak_sm64_render(void) {
    if (!s_active || s_jak_id < 0) return;

    /* Fetch bone data from DLL */
    s_bones_available = false;
    if (fn_jak_get_bone_data) {
        s_bones_available = fn_jak_get_bone_data(&s_bone_data);
    }

    /* Try to upload texture atlas if not done yet */
    if (!s_texture_uploaded) {
        upload_texture_atlas();
    }

    extern struct CameraFOVStatus sFOVState;

    /* ---- Save GL state ---- */
    GLint prev_program;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    GLboolean prev_depth_test = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prev_blend = glIsEnabled(GL_BLEND);
    GLboolean prev_cull = glIsEnabled(GL_CULL_FACE);
    GLint prev_depth_func;
    glGetIntegerv(GL_DEPTH_FUNC, &prev_depth_func);
    GLboolean prev_tex2d = glIsEnabled(GL_TEXTURE_2D);
    GLint prev_tex_binding;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex_binding);

    glUseProgram(0);

    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    float aspect = (float)viewport[2] / (float)viewport[3];
    float fov = sFOVState.fov > 0.0f ? sFOVState.fov : 45.0f;

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    gluPerspective((double)fov, (double)aspect, 100.0, 50000.0);

    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    gluLookAt(
        gLakituState.curPos[0], gLakituState.curPos[1], gLakituState.curPos[2],
        gLakituState.curFocus[0], gLakituState.curFocus[1], gLakituState.curFocus[2],
        0.0, 1.0, 0.0
    );

    /* ================================================================== */
    /*  PASS 1: Textured triangle mesh (depth tested, behind SM64 geo)    */
    /* ================================================================== */
    uint16_t num_tris = s_jak_geo.num_triangles_used;

    /* Diagnostic logging */
    {
        static int mesh_log_count = 0;
        mesh_log_count++;
        if (mesh_log_count <= 5 || mesh_log_count % 300 == 0) {
            JAK_LOG("MeshRender #%d: %u triangles, tex_uploaded=%d, tex_id=%u",
                    mesh_log_count, (unsigned)num_tris,
                    s_texture_uploaded, s_jak_texture_id);
            if (num_tris > 0 && s_geo_position) {
                JAK_LOG("  vtx0=(%.1f,%.1f,%.1f) vtx1=(%.1f,%.1f,%.1f) vtx2=(%.1f,%.1f,%.1f)",
                        s_geo_position[0], s_geo_position[1], s_geo_position[2],
                        s_geo_position[3], s_geo_position[4], s_geo_position[5],
                        s_geo_position[6], s_geo_position[7], s_geo_position[8]);
                if (s_geo_uv) {
                    JAK_LOG("  uv0=(%.4f,%.4f) uv1=(%.4f,%.4f) uv2=(%.4f,%.4f)",
                            s_geo_uv[0], s_geo_uv[1], s_geo_uv[2], s_geo_uv[3],
                            s_geo_uv[4], s_geo_uv[5]);
                }
                if (s_geo_color) {
                    JAK_LOG("  col0=(%.2f,%.2f,%.2f,%.2f) col1=(%.2f,%.2f,%.2f,%.2f)",
                            s_geo_color[0], s_geo_color[1], s_geo_color[2], s_geo_color[3],
                            s_geo_color[4], s_geo_color[5], s_geo_color[6], s_geo_color[7]);
                }
            }
        }
    }

    if (num_tris > 0 && s_geo_position) {
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDisable(GL_LIGHTING);
        glDisable(GL_ALPHA_TEST);

        /* Bind texture atlas if available */
        if (s_texture_uploaded && s_jak_texture_id) {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, s_jak_texture_id);
            /* Use GL_COMBINE: RGB = texture * vertex color, Alpha = vertex color (always 1.0)
               This ignores texture alpha entirely so hair/edges aren't transparent */
            glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
            /* RGB: modulate texture with vertex color */
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_RGB, GL_MODULATE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_RGB, GL_TEXTURE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SRC1_RGB, GL_PRIMARY_COLOR);
            /* Alpha: use vertex color alpha only (ignore texture alpha) */
            glTexEnvi(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
            glTexEnvi(GL_TEXTURE_ENV, GL_SRC0_ALPHA, GL_PRIMARY_COLOR);
        } else {
            glDisable(GL_TEXTURE_2D);
        }

        uint32_t num_verts = (uint32_t)num_tris * 3;
        glBegin(GL_TRIANGLES);
        for (uint32_t i = 0; i < num_verts; i++) {
            /* Vertex color (RGBA from DLL, 4 floats per vertex) */
            if (s_geo_color) {
                float r = s_geo_color[i * 4 + 0];
                float g = s_geo_color[i * 4 + 1];
                float b = s_geo_color[i * 4 + 2];
                /* Ignore vertex alpha — always fully opaque */
                glColor4f(r, g, b, 1.0f);
            } else {
                glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
            }

            /* Normal */
            if (s_geo_normal) {
                glNormal3f(s_geo_normal[i * 3 + 0],
                           s_geo_normal[i * 3 + 1],
                           s_geo_normal[i * 3 + 2]);
            }

            /* Texture coordinate */
            if (s_geo_uv && s_texture_uploaded) {
                glTexCoord2f(s_geo_uv[i * 2 + 0],
                             s_geo_uv[i * 2 + 1]);
            }

            /* Position */
            glVertex3f(s_geo_position[i * 3 + 0],
                       s_geo_position[i * 3 + 1],
                       s_geo_position[i * 3 + 2]);
        }
        glEnd();

        glDisable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    /* ================================================================== */
    /*  PASS 2: Bone wireframe overlay (optional, on top of everything)   */
    /* ================================================================== */
    if (s_draw_wireframe_overlay || num_tris == 0) {
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_CULL_FACE);
        glDisable(GL_LIGHTING);
        glDisable(GL_TEXTURE_2D);

        if (s_bones_available && s_bone_data.num_bones > 0) {
            static int render_count = 0;
            render_count++;
            if (render_count <= 3 || render_count % 300 == 0) {
                JAK_LOG("BoneOverlay #%d: %d bones, bone0=(%.1f,%.1f,%.1f)",
                        render_count, s_bone_data.num_bones,
                        s_bone_data.positions[0][0], s_bone_data.positions[0][1],
                        s_bone_data.positions[0][2]);
            }

            /* Draw bone lines (parent -> child) */
            glLineWidth(3.0f);
            glBegin(GL_LINES);
            for (int i = 0; i < s_bone_data.num_bones; i++) {
                int parent = s_bone_data.parent_indices[i];
                if (parent >= 0 && parent < s_bone_data.num_bones) {
                    glColor4f(1.0f, 0.8f, 0.2f, 0.9f);
                    glVertex3f(s_bone_data.positions[parent][0],
                               s_bone_data.positions[parent][1],
                               s_bone_data.positions[parent][2]);
                    glVertex3f(s_bone_data.positions[i][0],
                               s_bone_data.positions[i][1],
                               s_bone_data.positions[i][2]);
                }
            }
            glEnd();

            /* Draw joint markers */
            for (int i = 0; i < s_bone_data.num_bones; i++) {
                float bx = s_bone_data.positions[i][0];
                float by = s_bone_data.positions[i][1];
                float bz = s_bone_data.positions[i][2];
                if (bx == 0.0f && by == 0.0f && bz == 0.0f) continue;
                if (s_bone_data.parent_indices[i] < 0) {
                    draw_octahedron(bx, by, bz, BONE_JOINT_RADIUS * 1.5f,
                                    0.2f, 1.0f, 0.3f, 0.95f);
                } else {
                    draw_octahedron(bx, by, bz, BONE_JOINT_RADIUS,
                                    0.2f, 0.8f, 1.0f, 0.85f);
                }
            }
        } else if (num_tris == 0) {
            /* Fallback: draw a marker at Jak's reported position */
            float jak_x = s_jak_state.position[0];
            float jak_y = s_jak_state.position[1];
            float jak_z = s_jak_state.position[2];
            draw_octahedron(jak_x, jak_y + 80.0f, jak_z, 30.0f,
                            0.2f, 1.0f, 0.3f, 0.9f);
        }
    }

    /* ---- Restore GL state ---- */
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();

    if (prev_depth_test) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthFunc(prev_depth_func);
    if (prev_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    if (prev_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
    if (prev_tex2d) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, prev_tex_binding);

    glUseProgram(prev_program);
}

/* ---- HUD debug text (called from render_hud in hud.c) ---- */

void jak_render_hud(void) {
    if (!s_active || s_jak_id < 0) return;

    struct MarioState *m = &gMarioStates[0];

    float jak_x = s_jak_state.position[0];
    float jak_y = s_jak_state.position[1];
    float jak_z = s_jak_state.position[2];

    float dx = m->pos[0] - jak_x;
    float dy = m->pos[1] - jak_y;
    float dz = m->pos[2] - jak_z;
    s32 dist = (s32)sqrtf(dx * dx + dy * dy + dz * dz);

    /* Bottom-left of screen, 3 lines (toggle with s_draw_debug_hud) */
    if (s_draw_debug_hud) {
        print_text(20, 50, "MARIO");
        print_text_fmt_int(90, 50, "%d", (s32)m->pos[0]);
        print_text_fmt_int(160, 50, "%d", (s32)m->pos[1]);
        print_text_fmt_int(230, 50, "%d", (s32)m->pos[2]);

        print_text(20, 34, "JAK");
        print_text_fmt_int(90, 34, "%d", (s32)jak_x);
        print_text_fmt_int(160, 34, "%d", (s32)jak_y);
        print_text_fmt_int(230, 34, "%d", (s32)jak_z);

        print_text_fmt_int(20, 18, "DIST %d", dist);
        print_text_fmt_int(120, 18, "TRI %d", (s32)s_jak_geo.num_triangles_used);
        print_text_fmt_int(200, 18, "BONES %d", s_bones_available ? s_bone_data.num_bones : 0);
    }
}
