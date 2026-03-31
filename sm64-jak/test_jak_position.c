/**
 * test_jak_position.c - Automated test for jak_set_position
 *
 * Tests whether jak_set_position actually teleports Jak by:
 *   1. Spawning Jak at a known position
 *   2. Ticking and reading back position
 *   3. Calling jak_set_position to a different location
 *   4. Ticking and reading back position
 *   5. Comparing results
 *
 * Also tests different coordinate scales to diagnose SM64 integration issues.
 *
 * Build (MinGW):
 *   gcc -O2 -o test_jak_position.exe test_jak_position.c -I../libjakopengoal/include
 * Run (from build/us_pc):
 *   set JAK_DATA_PATH=C:\path\to\jak-project
 *   test_jak_position.exe %JAK_DATA_PATH%
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define SLEEP_MS(ms) Sleep(ms)
#else
#include <unistd.h>
#define SLEEP_MS(ms) usleep((ms) * 1000)
#endif

/* ---- DLL loading ---- */

typedef struct {
    int16_t type;
    int16_t flags;
    float vertices[3][3];
    float normal[3];
} JakSurface;

typedef struct {
    float cam_x, cam_z;
    float stick_x, stick_y;
    float r_stick_x, r_stick_y;
    uint16_t buttons;
} JakInputs;

typedef struct {
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
} JakState;

typedef int32_t  (*pfn_jak_global_init)(const char*, uint8_t*);
typedef void     (*pfn_jak_global_terminate)(void);
typedef bool     (*pfn_jak_is_ready)(void);
typedef void     (*pfn_jak_static_surfaces_load)(const JakSurface*, uint32_t);
typedef int32_t  (*pfn_jak_create)(float, float, float);
typedef void     (*pfn_jak_delete)(int32_t);
typedef void     (*pfn_jak_tick)(int32_t, const JakInputs*, JakState*, void*);
typedef void     (*pfn_jak_set_position)(int32_t, float, float, float);
typedef float    (*pfn_jak_find_floor_height)(float, float, float);

#define JAK_MAX_BONES 256
typedef struct {
    float positions[JAK_MAX_BONES][3];
    int   parent_indices[JAK_MAX_BONES];
    int   num_bones;
} JakBoneData;
typedef bool     (*pfn_jak_get_bone_data)(JakBoneData*);

static pfn_jak_global_init         fn_init;
static pfn_jak_global_terminate    fn_terminate;
static pfn_jak_is_ready            fn_is_ready;
static pfn_jak_static_surfaces_load fn_load_surfaces;
static pfn_jak_create              fn_create;
static pfn_jak_delete              fn_delete;
static pfn_jak_tick                fn_tick;
static pfn_jak_set_position        fn_set_position;
static pfn_jak_find_floor_height   fn_find_floor;
static pfn_jak_get_bone_data       fn_get_bones;

static HMODULE s_dll;

static bool load_dll(void) {
    s_dll = LoadLibraryA("jakopengoal.dll");
    if (!s_dll) {
        printf("[FAIL] Could not load jakopengoal.dll (error %lu)\n", GetLastError());
        return false;
    }

    #define LOAD(name) do { \
        fn_##name = (pfn_jak_##name)GetProcAddress(s_dll, "jak_" #name); \
        if (!fn_##name) { printf("[FAIL] Missing: jak_%s\n", #name); return false; } \
    } while(0)

    /* Map short names to function pointers */
    fn_init = (pfn_jak_global_init)GetProcAddress(s_dll, "jak_global_init");
    fn_terminate = (pfn_jak_global_terminate)GetProcAddress(s_dll, "jak_global_terminate");
    fn_is_ready = (pfn_jak_is_ready)GetProcAddress(s_dll, "jak_is_ready");
    fn_load_surfaces = (pfn_jak_static_surfaces_load)GetProcAddress(s_dll, "jak_static_surfaces_load");
    fn_create = (pfn_jak_create)GetProcAddress(s_dll, "jak_create");
    fn_delete = (pfn_jak_delete)GetProcAddress(s_dll, "jak_delete");
    fn_tick = (pfn_jak_tick)GetProcAddress(s_dll, "jak_tick");
    fn_set_position = (pfn_jak_set_position)GetProcAddress(s_dll, "jak_set_position");
    fn_find_floor = (pfn_jak_find_floor_height)GetProcAddress(s_dll, "jak_find_floor_height");
    fn_get_bones = (pfn_jak_get_bone_data)GetProcAddress(s_dll, "jak_get_bone_data");

    if (!fn_init || !fn_terminate || !fn_is_ready || !fn_load_surfaces ||
        !fn_create || !fn_delete || !fn_tick || !fn_set_position) {
        printf("[FAIL] Missing required function pointers\n");
        return false;
    }

    printf("[OK] DLL loaded, all functions resolved\n");
    printf("[INFO] jak_get_bone_data: %s\n", fn_get_bones ? "available" : "not available");
    return true;

    #undef LOAD
}

/* ---- Test surfaces: flat 10000x10000 platform at Y=0 ---- */

static JakSurface* build_platform(uint32_t *out_count) {
    JakSurface *s = (JakSurface*)calloc(2, sizeof(JakSurface));
    float sz = 5000.0f;

    /* Triangle 1 */
    s[0].type = 0; s[0].flags = 0;
    s[0].vertices[0][0] = -sz; s[0].vertices[0][1] = 0; s[0].vertices[0][2] = -sz;
    s[0].vertices[1][0] =  sz; s[0].vertices[1][1] = 0; s[0].vertices[1][2] = -sz;
    s[0].vertices[2][0] = -sz; s[0].vertices[2][1] = 0; s[0].vertices[2][2] =  sz;

    /* Triangle 2 */
    s[1].type = 0; s[1].flags = 0;
    s[1].vertices[0][0] =  sz; s[1].vertices[0][1] = 0; s[1].vertices[0][2] = -sz;
    s[1].vertices[1][0] =  sz; s[1].vertices[1][1] = 0; s[1].vertices[1][2] =  sz;
    s[1].vertices[2][0] = -sz; s[1].vertices[2][1] = 0; s[1].vertices[2][2] =  sz;

    *out_count = 2;
    return s;
}

/* ---- Helpers ---- */

static float dist3d(float *a, float *b) {
    float dx = a[0]-b[0], dy = a[1]-b[1], dz = a[2]-b[2];
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

static void tick_n(int32_t id, int n, JakState *state) {
    JakInputs inputs;
    memset(&inputs, 0, sizeof(inputs));
    for (int i = 0; i < n; i++) {
        fn_tick(id, &inputs, state, NULL);
        SLEEP_MS(17); /* ~60fps, give GOAL VM thread time to process */
    }
}

/** Tick with specific inputs (stick + camera direction). */
static void tick_with_input(int32_t id, int n, JakState *state,
                            float stick_x, float stick_y,
                            float cam_x, float cam_z) {
    JakInputs inputs;
    memset(&inputs, 0, sizeof(inputs));
    inputs.stick_x = stick_x;
    inputs.stick_y = stick_y;
    inputs.cam_x = cam_x;
    inputs.cam_z = cam_z;
    for (int i = 0; i < n; i++) {
        fn_tick(id, &inputs, state, NULL);
        SLEEP_MS(17);
    }
}

static void print_pos(const char *label, JakState *s) {
    printf("  %-20s pos=(%.1f, %.1f, %.1f) vel=(%.1f, %.1f, %.1f) ground=%s\n",
           label, s->position[0], s->position[1], s->position[2],
           s->velocity[0], s->velocity[1], s->velocity[2],
           s->on_ground ? "yes" : "no");
}

/* ---- Tests ---- */

static int test_count = 0;
static int pass_count = 0;
static int fail_count = 0;

static void check(const char *name, bool condition) {
    test_count++;
    if (condition) {
        pass_count++;
        printf("[PASS] %s\n", name);
    } else {
        fail_count++;
        printf("[FAIL] %s\n", name);
    }
}

int main(int argc, char **argv) {
    const char *data_path = NULL;

    if (argc > 1) {
        data_path = argv[1];
    } else {
        data_path = getenv("JAK_DATA_PATH");
    }

    if (!data_path || !data_path[0]) {
        printf("Usage: %s <jak-project-path>\n", argv[0]);
        printf("  Or set JAK_DATA_PATH environment variable.\n");
        return 1;
    }

    printf("=== jak_set_position test ===\n");
    printf("Data path: %s\n\n", data_path);

    /* ---- Load DLL ---- */
    if (!load_dll()) return 1;

    /* ---- Init runtime ---- */
    printf("[....] Booting GOAL runtime...\n");
    int32_t ret = fn_init(data_path, NULL);
    if (ret < 0) {
        printf("[FAIL] jak_global_init returned %d\n", ret);
        return 1;
    }

    /* Wait for GOAL VM */
    int wait = 0;
    while (!fn_is_ready()) {
        SLEEP_MS(100);
        wait++;
        if (wait > 300) {
            printf("[FAIL] Timeout waiting for GOAL runtime (30s)\n");
            fn_terminate();
            return 1;
        }
    }
    printf("[OK] GOAL runtime ready after %.1fs\n\n", wait * 0.1f);

    /* ---- Load surfaces ---- */
    uint32_t surf_count;
    JakSurface *surfaces = build_platform(&surf_count);
    fn_load_surfaces(surfaces, surf_count);
    printf("[OK] Loaded %u collision surfaces\n", surf_count);

    /* Query floor height to verify surfaces work */
    if (fn_find_floor) {
        float floor = fn_find_floor(0.0f, 100.0f, 0.0f);
        printf("[INFO] Floor height at (0,100,0) = %.1f\n", floor);
    }

    /* ============================================================ */
    /*  TEST 0: Floor height queries (collision system check)       */
    /* ============================================================ */
    printf("\n--- TEST 0: Floor height queries ---\n");
    if (fn_find_floor) {
        /* Query points that should be ON the platform */
        float fh1 = fn_find_floor(0.0f, 100.0f, 0.0f);
        printf("  Floor at (0,100,0) = %.1f\n", fh1);
        float fh2 = fn_find_floor(500.0f, 200.0f, 300.0f);
        printf("  Floor at (500,200,300) = %.1f\n", fh2);
        float fh3 = fn_find_floor(-1000.0f, 500.0f, -1000.0f);
        printf("  Floor at (-1000,500,-1000) = %.1f\n", fh3);
        /* Query point way outside the platform */
        float fh4 = fn_find_floor(99999.0f, 100.0f, 99999.0f);
        printf("  Floor at (99999,100,99999) = %.1f  (off-platform)\n", fh4);

        /* The floor height should be 0 (platform Y) for points on the platform,
           or a large negative for off-platform */
        check("Floor height at origin ~= 0", fh1 > -100.0f && fh1 < 100.0f);
        check("Floor height at (500,200,300) ~= 0", fh2 > -100.0f && fh2 < 100.0f);
    } else {
        printf("  jak_find_floor_height not available, skipping\n");
    }

    /* ============================================================ */
    /*  TEST 1: Basic spawn position                                */
    /* ============================================================ */
    printf("\n--- TEST 1: Spawn position ---\n");

    float spawn_x = 0.0f, spawn_y = 100.0f, spawn_z = 0.0f;
    int32_t jak_id = fn_create(spawn_x, spawn_y, spawn_z);
    check("jak_create succeeds", jak_id >= 0);
    if (jak_id < 0) goto cleanup;

    JakState state;
    memset(&state, 0, sizeof(state));

    /* Tick a few frames to get initial state */
    tick_n(jak_id, 5, &state);
    print_pos("After 5 ticks:", &state);

    /* Tick more to let Jak settle on ground */
    tick_n(jak_id, 60, &state);
    print_pos("After 65 ticks:", &state);

    /* Even more to be sure */
    tick_n(jak_id, 60, &state);
    print_pos("After 125 ticks:", &state);

    /* ============================================================ */
    /*  TEST 2: set_position to small offset                        */
    /* ============================================================ */
    printf("\n--- TEST 2: set_position small offset (500, 100, 300) ---\n");

    float target_x = 500.0f, target_y = 100.0f, target_z = 300.0f;
    float pre_pos[3] = { state.position[0], state.position[1], state.position[2] };
    printf("  Before:  (%.1f, %.1f, %.1f)\n", pre_pos[0], pre_pos[1], pre_pos[2]);

    fn_set_position(jak_id, target_x, target_y, target_z);

    /* Tick 1 frame */
    tick_n(jak_id, 1, &state);
    print_pos("After 1 tick:", &state);

    /* Tick 10 more frames */
    tick_n(jak_id, 10, &state);
    print_pos("After 11 ticks:", &state);

    /* Tick 30 more frames */
    tick_n(jak_id, 30, &state);
    print_pos("After 41 ticks:", &state);

    float post_pos[3] = { state.position[0], state.position[1], state.position[2] };
    float d_from_pre = dist3d(post_pos, pre_pos);
    float d_from_target = dist3d(post_pos, (float[]){target_x, target_y, target_z});

    printf("  Distance from pre-teleport pos: %.1f\n", d_from_pre);
    printf("  Distance from target pos:       %.1f\n", d_from_target);

    check("Position changed after set_position", d_from_pre > 50.0f);
    check("Position near target (within 200 units)", d_from_target < 200.0f);

    /* ============================================================ */
    /*  TEST 3: set_position to SM64-scale coords                   */
    /* ============================================================ */
    printf("\n--- TEST 3: set_position SM64-scale (-1328, 260, 4664) ---\n");

    float sm64_x = -1328.0f, sm64_y = 260.0f, sm64_z = 4664.0f;
    pre_pos[0] = state.position[0];
    pre_pos[1] = state.position[1];
    pre_pos[2] = state.position[2];

    fn_set_position(jak_id, sm64_x, sm64_y, sm64_z);
    tick_n(jak_id, 1, &state);
    print_pos("After 1 tick:", &state);
    tick_n(jak_id, 30, &state);
    print_pos("After 31 ticks:", &state);

    post_pos[0] = state.position[0]; post_pos[1] = state.position[1]; post_pos[2] = state.position[2];
    d_from_pre = dist3d(post_pos, pre_pos);
    d_from_target = dist3d(post_pos, (float[]){sm64_x, sm64_y, sm64_z});

    printf("  Distance from pre-teleport pos: %.1f\n", d_from_pre);
    printf("  Distance from target pos:       %.1f\n", d_from_target);

    check("Position changed for SM64-scale coords", d_from_pre > 50.0f);
    /* Y can drift significantly due to gravity when no collision floor at target Y.
       Check XZ separately since those are the reliable indicators of teleport success. */
    float d_xz_from_target = sqrtf(
        (post_pos[0] - sm64_x) * (post_pos[0] - sm64_x) +
        (post_pos[2] - sm64_z) * (post_pos[2] - sm64_z));
    printf("  XZ distance from target: %.1f\n", d_xz_from_target);
    check("SM64 XZ pos near target (within 50 units)", d_xz_from_target < 50.0f);

    /* ============================================================ */
    /*  TEST 4: Repeated set_position (does it work more than once?)*/
    /* ============================================================ */
    printf("\n--- TEST 4: Repeated set_position ---\n");

    float positions[][3] = {
        {  100.0f,  50.0f,  100.0f },
        { -200.0f,  50.0f, -200.0f },
        { 1000.0f,  50.0f, 1000.0f },
    };

    for (int i = 0; i < 3; i++) {
        fn_set_position(jak_id, positions[i][0], positions[i][1], positions[i][2]);
        tick_n(jak_id, 15, &state);
        float d = dist3d(state.position, positions[i]);
        printf("  Teleport #%d to (%.0f,%.0f,%.0f) -> actual (%.1f,%.1f,%.1f) dist=%.1f\n",
               i+1, positions[i][0], positions[i][1], positions[i][2],
               state.position[0], state.position[1], state.position[2], d);
        /* Check XZ distance since Y drifts with gravity */
        float d_xz = sqrtf(
            (state.position[0] - positions[i][0]) * (state.position[0] - positions[i][0]) +
            (state.position[2] - positions[i][2]) * (state.position[2] - positions[i][2]));
        printf("    XZ dist=%.1f\n", d_xz);
        check("Repeated teleport XZ near target", d_xz < 50.0f);
    }

    /* ============================================================ */
    /*  TEST 5: set_position vs jak_create position comparison      */
    /* ============================================================ */
    printf("\n--- TEST 5: Compare create vs set_position at same coords ---\n");

    /* Delete and re-create at (500, 100, 300) */
    fn_delete(jak_id);
    jak_id = fn_create(500.0f, 100.0f, 300.0f);
    tick_n(jak_id, 30, &state);
    float create_pos[3] = { state.position[0], state.position[1], state.position[2] };
    printf("  jak_create(500,100,300) -> (%.1f, %.1f, %.1f)\n",
           create_pos[0], create_pos[1], create_pos[2]);

    /* Now teleport to (0,100,0) and back to (500,100,300) */
    fn_set_position(jak_id, 0.0f, 100.0f, 0.0f);
    tick_n(jak_id, 15, &state);
    fn_set_position(jak_id, 500.0f, 100.0f, 300.0f);
    tick_n(jak_id, 30, &state);
    float setpos_pos[3] = { state.position[0], state.position[1], state.position[2] };
    printf("  set_position(500,100,300) -> (%.1f, %.1f, %.1f)\n",
           setpos_pos[0], setpos_pos[1], setpos_pos[2]);

    float create_vs_setpos = dist3d(create_pos, setpos_pos);
    printf("  Distance between create and set_position results: %.1f\n", create_vs_setpos);

    check("create and set_position produce similar results", create_vs_setpos < 200.0f);

    /* ============================================================ */
    /*  TEST 6: Ground snapping — Jak should land on floor Y=0      */
    /* ============================================================ */
    printf("\n--- TEST 6: Ground snapping ---\n");
    /* Teleport to floor-level and verify Y stays at 0 */
    fn_set_position(jak_id, 0.0f, 100.0f, 0.0f);
    tick_n(jak_id, 60, &state);
    printf("  After teleport to (0,100,0) + 60 ticks: Y=%.1f\n", state.position[1]);
    check("Jak at floor height (Y ~= 0)", state.position[1] > -5.0f && state.position[1] < 5.0f);

    /* Teleport to large Y and verify still snaps to floor */
    fn_set_position(jak_id, 200.0f, 500.0f, 200.0f);
    tick_n(jak_id, 120, &state);
    printf("  After teleport to (200,500,200) + 120 ticks: Y=%.1f\n", state.position[1]);
    check("Jak falls to floor from Y=500 (Y ~= 0)", state.position[1] > -5.0f && state.position[1] < 5.0f);

    /* ============================================================ */
    /*  TEST 7: Bone data retrieval                                 */
    /* ============================================================ */
    printf("\n--- TEST 7: Bone data ---\n");
    if (fn_get_bones) {
        /* Make sure Jak is on ground at a known position */
        fn_set_position(jak_id, 0.0f, 100.0f, 0.0f);
        tick_n(jak_id, 60, &state);

        JakBoneData bones;
        memset(&bones, 0, sizeof(bones));
        bool got_bones = fn_get_bones(&bones);
        printf("  jak_get_bone_data returned: %s\n", got_bones ? "true" : "false");
        printf("  num_bones: %d\n", bones.num_bones);
        check("Bone data available", got_bones);
        check("Has bones (>0)", bones.num_bones > 0);
        check("Has typical Jak bone count (>20)", bones.num_bones > 20);

        if (bones.num_bones > 0) {
            /* Count bones with parent links */
            int with_parent = 0;
            int at_origin = 0;
            for (int i = 0; i < bones.num_bones; i++) {
                if (bones.parent_indices[i] >= 0) with_parent++;
                if (bones.positions[i][0] == 0.0f &&
                    bones.positions[i][1] == 0.0f &&
                    bones.positions[i][2] == 0.0f) at_origin++;
            }
            printf("  Bones with parent: %d / %d\n", with_parent, bones.num_bones);
            printf("  Bones at origin (invalid): %d / %d\n", at_origin, bones.num_bones);
            check("Most bones have parent links", with_parent > bones.num_bones / 2);
            check("Most bones NOT at origin", at_origin < bones.num_bones / 2);

            /* Print first 10 bones for inspection */
            printf("  First 10 bones:\n");
            for (int i = 0; i < 10 && i < bones.num_bones; i++) {
                printf("    bone[%2d] parent=%2d pos=(%.1f, %.1f, %.1f)\n",
                       i, bones.parent_indices[i],
                       bones.positions[i][0], bones.positions[i][1], bones.positions[i][2]);
            }

            /* Verify bone positions are near Jak's position (within ~500 units) */
            float jak_cx = state.position[0];
            float jak_cy = state.position[1];
            float jak_cz = state.position[2];
            int near_jak = 0;
            float max_dist = 0;
            for (int i = 0; i < bones.num_bones; i++) {
                if (bones.positions[i][0] == 0.0f &&
                    bones.positions[i][1] == 0.0f &&
                    bones.positions[i][2] == 0.0f) continue;
                float bdx = bones.positions[i][0] - jak_cx;
                float bdy = bones.positions[i][1] - jak_cy;
                float bdz = bones.positions[i][2] - jak_cz;
                float bdist = sqrtf(bdx*bdx + bdy*bdy + bdz*bdz);
                if (bdist < 500.0f) near_jak++;
                if (bdist > max_dist) max_dist = bdist;
            }
            printf("  Bones within 500 units of Jak: %d / %d\n", near_jak, bones.num_bones - at_origin);
            printf("  Max bone distance from Jak: %.1f\n", max_dist);
            check("Bones are near Jak position", near_jak > 0);

            /* Check that bones actually form a hierarchy (at least some parent links make lines) */
            int valid_lines = 0;
            for (int i = 0; i < bones.num_bones; i++) {
                int p = bones.parent_indices[i];
                if (p >= 0 && p < bones.num_bones) {
                    float dx = bones.positions[i][0] - bones.positions[p][0];
                    float dy = bones.positions[i][1] - bones.positions[p][1];
                    float dz = bones.positions[i][2] - bones.positions[p][2];
                    float len = sqrtf(dx*dx + dy*dy + dz*dz);
                    if (len > 0.1f && len < 300.0f) valid_lines++;
                }
            }
            printf("  Valid bone lines (reasonable length): %d\n", valid_lines);
            check("Has valid skeleton lines (>5)", valid_lines > 5);
        }
    } else {
        printf("  jak_get_bone_data not available, skipping\n");
    }

    /* ============================================================ */
    /*  TEST 8: Does position stick without surfaces?               */
    /* ============================================================ */
    printf("\n--- TEST 8: set_position outside collision bounds ---\n");
    fn_set_position(jak_id, 9999.0f, 9999.0f, 9999.0f);
    tick_n(jak_id, 30, &state);
    print_pos("After teleport to (9999,9999,9999):", &state);
    printf("  (Jak may fall/respawn if outside collision)\n");

    /* ============================================================ */
    /*  TEST 9: Camera-relative movement                            */
    /* ============================================================ */
    printf("\n--- TEST 9: Camera-relative movement ---\n");
    {
        /* Place Jak at origin on the floor and let him settle */
        fn_set_position(jak_id, 0.0f, 100.0f, 0.0f);
        tick_n(jak_id, 60, &state);
        float start_x = state.position[0];
        float start_z = state.position[2];
        printf("  Start pos: (%.1f, %.1f, %.1f)\n", state.position[0], state.position[1], state.position[2]);

        /* --- Sub-test A: Camera facing +Z, push stick forward ---
         * Camera direction = (0, 0, 1)  (looking towards +Z)
         * Stick forward = stick_y = +1.0
         * Expected: Jak should move in +Z direction
         */
        printf("\n  [A] Camera=(0,0,+1), stick forward (stick_y=+1)\n");
        fn_set_position(jak_id, 0.0f, 10.0f, 0.0f);
        tick_n(jak_id, 30, &state);
        start_x = state.position[0];
        start_z = state.position[2];
        printf("    Before: (%.1f, %.1f, %.1f)\n", state.position[0], state.position[1], state.position[2]);

        tick_with_input(jak_id, 90, &state, 0.0f, 1.0f, 0.0f, 1.0f);
        float dz_a = state.position[2] - start_z;
        float dx_a = state.position[0] - start_x;
        printf("    After 90 ticks: (%.1f, %.1f, %.1f)\n", state.position[0], state.position[1], state.position[2]);
        printf("    Delta: dx=%.1f dz=%.1f\n", dx_a, dz_a);
        check("Stick forward + cam +Z => moved in +Z", dz_a > 30.0f);
        check("Stick forward + cam +Z => minimal X drift", fabsf(dx_a) < fabsf(dz_a) * 0.5f);

        /* --- Sub-test B: Camera facing +X, push stick forward ---
         * Camera direction = (1, 0, 0)  (looking towards +X)
         * Stick forward = stick_y = +1.0
         * Expected: Jak should move in +X direction
         */
        printf("\n  [B] Camera=(+1,0,0), stick forward (stick_y=+1)\n");
        fn_set_position(jak_id, 0.0f, 10.0f, 0.0f);
        tick_n(jak_id, 30, &state);
        start_x = state.position[0];
        start_z = state.position[2];
        printf("    Before: (%.1f, %.1f, %.1f)\n", state.position[0], state.position[1], state.position[2]);

        tick_with_input(jak_id, 90, &state, 0.0f, 1.0f, 1.0f, 0.0f);
        float dx_b = state.position[0] - start_x;
        float dz_b = state.position[2] - start_z;
        printf("    After 90 ticks: (%.1f, %.1f, %.1f)\n", state.position[0], state.position[1], state.position[2]);
        printf("    Delta: dx=%.1f dz=%.1f\n", dx_b, dz_b);
        check("Stick forward + cam +X => moved in +X", dx_b > 30.0f);
        check("Stick forward + cam +X => minimal Z drift", fabsf(dz_b) < fabsf(dx_b) * 0.5f);

        /* --- Sub-test C: Camera facing -Z, push stick forward ---
         * Camera direction = (0, 0, -1)  (looking towards -Z)
         * Stick forward = stick_y = +1.0
         * Expected: Jak should move in -Z direction
         */
        printf("\n  [C] Camera=(0,0,-1), stick forward (stick_y=+1)\n");
        fn_set_position(jak_id, 0.0f, 10.0f, 0.0f);
        tick_n(jak_id, 30, &state);
        start_x = state.position[0];
        start_z = state.position[2];
        printf("    Before: (%.1f, %.1f, %.1f)\n", state.position[0], state.position[1], state.position[2]);

        tick_with_input(jak_id, 90, &state, 0.0f, 1.0f, 0.0f, -1.0f);
        float dz_c = state.position[2] - start_z;
        float dx_c = state.position[0] - start_x;
        printf("    After 90 ticks: (%.1f, %.1f, %.1f)\n", state.position[0], state.position[1], state.position[2]);
        printf("    Delta: dx=%.1f dz=%.1f\n", dx_c, dz_c);
        check("Stick forward + cam -Z => moved in -Z", dz_c < -30.0f);

        /* --- Sub-test D: Camera facing +Z, push stick LEFT ---
         * Camera direction = (0, 0, 1)
         * Stick left = stick_x = -1.0
         * Expected: Jak moves in -X direction (left relative to +Z forward)
         */
        printf("\n  [D] Camera=(0,0,+1), stick left (stick_x=-1)\n");
        fn_set_position(jak_id, 0.0f, 10.0f, 0.0f);
        tick_n(jak_id, 30, &state);
        start_x = state.position[0];
        start_z = state.position[2];
        printf("    Before: (%.1f, %.1f, %.1f)\n", state.position[0], state.position[1], state.position[2]);

        tick_with_input(jak_id, 90, &state, -1.0f, 0.0f, 0.0f, 1.0f);
        float dx_d = state.position[0] - start_x;
        float dz_d = state.position[2] - start_z;
        printf("    After 90 ticks: (%.1f, %.1f, %.1f)\n", state.position[0], state.position[1], state.position[2]);
        printf("    Delta: dx=%.1f dz=%.1f\n", dx_d, dz_d);
        check("Stick left + cam +Z => moved in -X", dx_d < -30.0f);

        /* --- Sub-test E: Camera facing +Z, push stick backward ---
         * stick_y = -1.0
         * Expected: Jak moves in -Z direction
         */
        printf("\n  [E] Camera=(0,0,+1), stick backward (stick_y=-1)\n");
        fn_set_position(jak_id, 0.0f, 10.0f, 0.0f);
        tick_n(jak_id, 30, &state);
        start_x = state.position[0];
        start_z = state.position[2];
        printf("    Before: (%.1f, %.1f, %.1f)\n", state.position[0], state.position[1], state.position[2]);

        tick_with_input(jak_id, 90, &state, 0.0f, -1.0f, 0.0f, 1.0f);
        float dz_e = state.position[2] - start_z;
        printf("    After 90 ticks: (%.1f, %.1f, %.1f)\n", state.position[0], state.position[1], state.position[2]);
        printf("    Delta: dx=%.1f dz=%.1f\n", state.position[0] - start_x, dz_e);
        check("Stick backward + cam +Z => moved in -Z", dz_e < -30.0f);
    }

    /* ============================================================ */
    /*  Summary                                                     */
    /* ============================================================ */
cleanup:
    printf("\n=== RESULTS: %d/%d passed, %d failed ===\n",
           pass_count, test_count, fail_count);

    if (jak_id >= 0) fn_delete(jak_id);
    fn_terminate();
    FreeLibrary(s_dll);
    free(surfaces);

    return fail_count > 0 ? 1 : 0;
}
