/**
 * Combined libsm64 + libjakopengoal visual test
 *
 * Renders Mario and Jak side by side on the same collision surface.
 * Uses SDL3 + OpenGL (glad) from the jak-project.
 * Links against pre-built libsm64 and libjakopengoal.
 */

#define _CRT_SECURE_NO_WARNINGS 1

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

// SDL3 (from jak-project third-party)
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

// OpenGL loader (glad, from jak-project third-party)
#include "glad/glad.h"

// libsm64 (header has its own extern "C" guards)
#include "libsm64.h"

// libjakopengoal
#include "libjakopengoal.h"

/* -------------------------------------------------------------------------- */
/*  Config                                                                     */
/* -------------------------------------------------------------------------- */

#define WINDOW_W 1024
#define WINDOW_H 768

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* -------------------------------------------------------------------------- */
/*  Simple shader                                                              */
/* -------------------------------------------------------------------------- */

static const char* VERT_SRC = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;

uniform mat4 uMVP;
out vec3 vColor;
out vec3 vNormal;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vColor = aColor;
    vNormal = aNormal;
}
)";

static const char* FRAG_SRC = R"(
#version 330 core
in vec3 vColor;
in vec3 vNormal;
out vec4 FragColor;

void main() {
    vec3 light = normalize(vec3(1.0, 1.0, 0.5));
    float diff = 0.4 + 0.6 * max(dot(normalize(vNormal), light), 0.0);
    FragColor = vec4(vColor * diff, 1.0);
}
)";

/* -------------------------------------------------------------------------- */
/*  Math helpers (minimal 4x4 matrix ops)                                      */
/* -------------------------------------------------------------------------- */

typedef float mat4[16];

static void mat4_identity(mat4 m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mat4_perspective(mat4 m, float fovDeg, float aspect, float znear, float zfar) {
    memset(m, 0, 16 * sizeof(float));
    float f = 1.0f / tanf(fovDeg * (float)M_PI / 360.0f);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (zfar + znear) / (znear - zfar);
    m[11] = -1.0f;
    m[14] = (2.0f * zfar * znear) / (znear - zfar);
}

static void mat4_lookat(mat4 m, const float eye[3], const float center[3], const float up[3]) {
    float f[3] = {center[0]-eye[0], center[1]-eye[1], center[2]-eye[2]};
    float len = sqrtf(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]);
    f[0]/=len; f[1]/=len; f[2]/=len;

    float s[3] = {f[1]*up[2]-f[2]*up[1], f[2]*up[0]-f[0]*up[2], f[0]*up[1]-f[1]*up[0]};
    len = sqrtf(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]);
    s[0]/=len; s[1]/=len; s[2]/=len;

    float u[3] = {s[1]*f[2]-s[2]*f[1], s[2]*f[0]-s[0]*f[2], s[0]*f[1]-s[1]*f[0]};

    mat4_identity(m);
    m[0]=s[0]; m[4]=s[1]; m[8]=s[2];
    m[1]=u[0]; m[5]=u[1]; m[9]=u[2];
    m[2]=-f[0]; m[6]=-f[1]; m[10]=-f[2];
    m[12]=-(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]);
    m[13]=-(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]);
    m[14]=(f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);
}

static void mat4_mul(mat4 out, const mat4 a, const mat4 b) {
    mat4 r;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            r[j*4+i] = 0;
            for (int k = 0; k < 4; k++)
                r[j*4+i] += a[k*4+i] * b[j*4+k];
        }
    memcpy(out, r, sizeof(mat4));
}

static void mat4_translate(mat4 m, float x, float y, float z) {
    mat4_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
}

/* -------------------------------------------------------------------------- */
/*  Sphere mesh generator                                                      */
/* -------------------------------------------------------------------------- */

#define SPHERE_STACKS 12
#define SPHERE_SLICES 16
#define SPHERE_TRI_COUNT (SPHERE_STACKS * SPHERE_SLICES * 2)
#define SPHERE_VERT_COUNT (SPHERE_TRI_COUNT * 3)

struct SphereMesh {
    GLuint vao, vbo_pos, vbo_nrm, vbo_col;
};

static void sphere_mesh_init(SphereMesh* sm, float r, float cr, float cg, float cb) {
    float positions[SPHERE_VERT_COUNT * 3];
    float normals[SPHERE_VERT_COUNT * 3];
    float colors[SPHERE_VERT_COUNT * 3];
    int vi = 0;

    auto push_vert = [&](float nx, float ny, float nz) {
        positions[vi*3+0] = nx * r;
        positions[vi*3+1] = ny * r;
        positions[vi*3+2] = nz * r;
        normals[vi*3+0] = nx;
        normals[vi*3+1] = ny;
        normals[vi*3+2] = nz;
        colors[vi*3+0] = cr;
        colors[vi*3+1] = cg;
        colors[vi*3+2] = cb;
        vi++;
    };

    for (int i = 0; i < SPHERE_STACKS; i++) {
        float t0 = (float)i / SPHERE_STACKS;
        float t1 = (float)(i + 1) / SPHERE_STACKS;
        float phi0 = t0 * (float)M_PI;
        float phi1 = t1 * (float)M_PI;

        for (int j = 0; j < SPHERE_SLICES; j++) {
            float s0 = (float)j / SPHERE_SLICES;
            float s1 = (float)(j + 1) / SPHERE_SLICES;
            float theta0 = s0 * 2.0f * (float)M_PI;
            float theta1 = s1 * 2.0f * (float)M_PI;

            float x00 = sinf(phi0)*cosf(theta0), y00 = cosf(phi0), z00 = sinf(phi0)*sinf(theta0);
            float x01 = sinf(phi0)*cosf(theta1), y01 = cosf(phi0), z01 = sinf(phi0)*sinf(theta1);
            float x10 = sinf(phi1)*cosf(theta0), y10 = cosf(phi1), z10 = sinf(phi1)*sinf(theta0);
            float x11 = sinf(phi1)*cosf(theta1), y11 = cosf(phi1), z11 = sinf(phi1)*sinf(theta1);

            push_vert(x00,y00,z00); push_vert(x10,y10,z10); push_vert(x01,y01,z01);
            push_vert(x01,y01,z01); push_vert(x10,y10,z10); push_vert(x11,y11,z11);
        }
    }

    glGenVertexArrays(1, &sm->vao);
    glBindVertexArray(sm->vao);

    glGenBuffers(1, &sm->vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, sm->vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, sizeof(positions), positions, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, NULL);

    glGenBuffers(1, &sm->vbo_nrm);
    glBindBuffer(GL_ARRAY_BUFFER, sm->vbo_nrm);
    glBufferData(GL_ARRAY_BUFFER, sizeof(normals), normals, GL_STATIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, NULL);

    glGenBuffers(1, &sm->vbo_col);
    glBindBuffer(GL_ARRAY_BUFFER, sm->vbo_col);
    glBufferData(GL_ARRAY_BUFFER, sizeof(colors), colors, GL_STATIC_DRAW);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, NULL);

    glBindVertexArray(0);
}

static void sphere_mesh_draw(SphereMesh* sm) {
    glBindVertexArray(sm->vao);
    glDrawArrays(GL_TRIANGLES, 0, SPHERE_VERT_COUNT);
    glBindVertexArray(0);
}

/* -------------------------------------------------------------------------- */
/*  GL helpers                                                                 */
/* -------------------------------------------------------------------------- */

static GLuint compile_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(s, 512, NULL, log);
        printf("Shader error: %s\n", log);
    }
    return s;
}

static GLuint create_program(const char* vs, const char* fs) {
    GLuint v = compile_shader(GL_VERTEX_SHADER, vs);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs);
    GLuint p = glCreateProgram();
    glAttachShader(p, v); glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v); glDeleteShader(f);
    return p;
}

/* -------------------------------------------------------------------------- */
/*  Character mesh (shared between Mario and Jak)                              */
/* -------------------------------------------------------------------------- */

struct CharMesh {
    GLuint vao, vbo_pos, vbo_nrm, vbo_col;
    int num_verts;
};

static void char_mesh_init(CharMesh* cm, int max_tris) {
    int max_verts = max_tris * 3;
    glGenVertexArrays(1, &cm->vao);
    glBindVertexArray(cm->vao);

    glGenBuffers(1, &cm->vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, cm->vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 3 * max_verts, NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, NULL);

    glGenBuffers(1, &cm->vbo_nrm);
    glBindBuffer(GL_ARRAY_BUFFER, cm->vbo_nrm);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 3 * max_verts, NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, NULL);

    glGenBuffers(1, &cm->vbo_col);
    glBindBuffer(GL_ARRAY_BUFFER, cm->vbo_col);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 3 * max_verts, NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, NULL);

    glBindVertexArray(0);
    cm->num_verts = 0;
}

static void char_mesh_update(CharMesh* cm, float* pos, float* nrm, float* col, int num_tris) {
    cm->num_verts = num_tris * 3;
    glBindBuffer(GL_ARRAY_BUFFER, cm->vbo_pos);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float) * 3 * cm->num_verts, pos);
    glBindBuffer(GL_ARRAY_BUFFER, cm->vbo_nrm);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float) * 3 * cm->num_verts, nrm);
    glBindBuffer(GL_ARRAY_BUFFER, cm->vbo_col);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float) * 3 * cm->num_verts, col);
}

static void char_mesh_draw(CharMesh* cm) {
    glBindVertexArray(cm->vao);
    glDrawArrays(GL_TRIANGLES, 0, cm->num_verts);
    glBindVertexArray(0);
}

/* -------------------------------------------------------------------------- */
/*  Collision geometry (flat ground)                                            */
/* -------------------------------------------------------------------------- */

/* Ground plane centered under Jak's test-zone spawn position.
 * Jak spawns at (0, meters 10, meters 10) = (0, 500, 500) in render units. */
#define GROUND_SIZE 5000
#define GROUND_CX 0
#define GROUND_CZ 500
#define GROUND_Y 490

static struct SM64Surface sm64_ground[] = {
    {0, 0, 0, {{GROUND_CX - GROUND_SIZE, GROUND_Y, GROUND_CZ + GROUND_SIZE},
               {GROUND_CX + GROUND_SIZE, GROUND_Y, GROUND_CZ - GROUND_SIZE},
               {GROUND_CX - GROUND_SIZE, GROUND_Y, GROUND_CZ - GROUND_SIZE}}},
    {0, 0, 0, {{GROUND_CX - GROUND_SIZE, GROUND_Y, GROUND_CZ + GROUND_SIZE},
               {GROUND_CX + GROUND_SIZE, GROUND_Y, GROUND_CZ + GROUND_SIZE},
               {GROUND_CX + GROUND_SIZE, GROUND_Y, GROUND_CZ - GROUND_SIZE}}},
};
static const int sm64_ground_count = 2;

/* Ground mesh for rendering */
static float ground_positions[] = {
    GROUND_CX - GROUND_SIZE, GROUND_Y, GROUND_CZ - GROUND_SIZE,
    GROUND_CX + GROUND_SIZE, GROUND_Y, GROUND_CZ - GROUND_SIZE,
    GROUND_CX - GROUND_SIZE, GROUND_Y, GROUND_CZ + GROUND_SIZE,
    GROUND_CX + GROUND_SIZE, GROUND_Y, GROUND_CZ - GROUND_SIZE,
    GROUND_CX + GROUND_SIZE, GROUND_Y, GROUND_CZ + GROUND_SIZE,
    GROUND_CX - GROUND_SIZE, GROUND_Y, GROUND_CZ + GROUND_SIZE,
};
static float ground_normals[] = {
    0,1,0, 0,1,0, 0,1,0,
    0,1,0, 0,1,0, 0,1,0,
};
static float ground_colors[] = {
    0.3f,0.5f,0.3f, 0.3f,0.5f,0.3f, 0.3f,0.5f,0.3f,
    0.3f,0.5f,0.3f, 0.3f,0.5f,0.3f, 0.3f,0.5f,0.3f,
};

/* -------------------------------------------------------------------------- */
/*  Callbacks                                                                  */
/* -------------------------------------------------------------------------- */

static void sm64_debug(const char* msg) { printf("[SM64] %s\n", msg); fflush(stdout); }
static void jak_debug(const char* msg) { printf("[JAK] %s\n", msg); fflush(stdout); }

/* -------------------------------------------------------------------------- */
/*  Main                                                                       */
/* -------------------------------------------------------------------------- */

int main(int argc, char** argv) {
    const char* rom_path = "baserom.us.z64";
    const char* jak_data = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--rom") == 0 && i+1 < argc) rom_path = argv[++i];
        if (strcmp(argv[i], "--jak-data") == 0 && i+1 < argc) jak_data = argv[++i];
    }

    if (!jak_data) {
        // Try env var, then default to project root
        jak_data = getenv("JAK_DATA_PATH");
        if (!jak_data) jak_data = "../../..";
    }

    printf("=== Mario + Jak Combined Test ===\n");
    printf("ROM: %s\n", rom_path);
    printf("Jak data: %s\n\n", jak_data);

    /* ---- Load SM64 ROM ---- */
    FILE* f = fopen(rom_path, "rb");
    if (!f) {
        printf("ERROR: Cannot open ROM '%s'\n", rom_path);
        printf("Usage: %s --rom <path-to-baserom.us.z64> --jak-data <path-to-jak-project>\n", argv[0]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size_t rom_size = ftell(f);
    rewind(f);
    uint8_t* rom = (uint8_t*)malloc(rom_size);
    fread(rom, 1, rom_size, f);
    fclose(f);

    /* ---- Init SDL3 + OpenGL ---- */
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_Window* window = SDL_CreateWindow("Mario + Jak", WINDOW_W, WINDOW_H,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1);

    if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        printf("Failed to load OpenGL\n");
        return 1;
    }
    printf("OpenGL %s\n\n", glGetString(GL_VERSION));
    fflush(stdout);

    /* ---- Open first available gamepad ---- */
    SDL_Gamepad* gamepad = NULL;
    {
        int count = 0;
        SDL_JoystickID* joysticks = SDL_GetGamepads(&count);
        if (joysticks && count > 0) {
            gamepad = SDL_OpenGamepad(joysticks[0]);
            if (gamepad) {
                printf("[INIT] Gamepad: %s\n", SDL_GetGamepadName(gamepad));
            }
        }
        SDL_free(joysticks);
        if (!gamepad) printf("[INIT] No gamepad found, using keyboard only\n");
        fflush(stdout);
    }

    /* ---- Init libsm64 ---- */
    printf("[INIT] Registering SM64 debug print callback...\n");
    fflush(stdout);
    sm64_register_debug_print_function(sm64_debug);

    printf("[INIT] Allocating SM64 texture (%d x %d)...\n", SM64_TEXTURE_WIDTH, SM64_TEXTURE_HEIGHT);
    fflush(stdout);
    uint8_t* sm64_tex = (uint8_t*)malloc(4 * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT);
    if (!sm64_tex) { printf("ERROR: Failed to allocate SM64 texture\n"); return 1; }
    memset(sm64_tex, 0, 4 * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT);

    printf("[INIT] Calling sm64_global_terminate (pre-init reset)...\n");
    fflush(stdout);
    sm64_global_terminate();
    printf("[INIT] Calling sm64_global_init (ROM size: %zu bytes)...\n", rom_size);
    printf("[INIT] ROM first 4 bytes: %02X %02X %02X %02X\n", rom[0], rom[1], rom[2], rom[3]);
    printf("[INIT] sm64_tex buffer at %p\n", (void*)sm64_tex);
    fflush(stdout);
    sm64_global_init(rom, sm64_tex);
    printf("[INIT] sm64_global_init done\n");
    fflush(stdout);

    printf("[INIT] Loading %d collision surfaces...\n", sm64_ground_count);
    fflush(stdout);
    sm64_static_surfaces_load(sm64_ground, sm64_ground_count);
    printf("[INIT] Surfaces loaded\n");
    fflush(stdout);

    printf("[INIT] Creating Mario near village1 (%d, %d, %d)...\n", GROUND_CX, GROUND_Y + 500, GROUND_CZ);
    fflush(stdout);
    int32_t marioId = sm64_mario_create(GROUND_CX, GROUND_Y + 500, GROUND_CZ);
    printf("[INIT] Mario spawned (id=%d)\n", marioId);
    fflush(stdout);
    if (marioId < 0) {
        printf("WARNING: Mario creation failed. Retrying at ground center...\n");
        fflush(stdout);
        marioId = sm64_mario_create(GROUND_CX, GROUND_Y, GROUND_CZ);
        printf("[INIT] Mario retry (id=%d)\n", marioId);
    }
    free(rom);

    /* ---- Init libjakopengoal ---- */
    printf("[INIT] Registering Jak debug callback...\n");
    fflush(stdout);
    jak_register_debug_print(jak_debug);

    printf("[INIT] Allocating Jak texture (%d x %d)...\n", JAK_TEXTURE_WIDTH, JAK_TEXTURE_HEIGHT);
    fflush(stdout);
    uint8_t* jak_tex = (uint8_t*)malloc(JAK_TEXTURE_WIDTH * JAK_TEXTURE_HEIGHT * 4);
    if (!jak_tex) { printf("ERROR: Failed to allocate Jak texture\n"); return 1; }

    printf("[INIT] Calling jak_global_init (data: %s)...\n", jak_data);
    fflush(stdout);
    int32_t jak_result = jak_global_init(jak_data, jak_tex);
    printf("[INIT] jak_global_init returned %d\n", jak_result);
    fflush(stdout);
    if (jak_result < 0) {
        printf("WARNING: Jak init failed (%d). Will run Mario only.\n", jak_result);
    }

    /* ---- GL setup ---- */
    printf("[INIT] Compiling shaders...\n"); fflush(stdout);
    GLuint prog = create_program(VERT_SRC, FRAG_SRC);
    GLint uMVP = glGetUniformLocation(prog, "uMVP");
    printf("[INIT] Shaders compiled (program=%u, uMVP=%d)\n", prog, uMVP); fflush(stdout);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glClearColor(0.4f, 0.6f, 0.9f, 1.0f);

    /* Marker spheres (radius=50 units) */
    SphereMesh jak_sphere, mario_sphere;
    sphere_mesh_init(&jak_sphere, 50.0f, 0.0f, 1.0f, 0.2f);   /* green = Jak */
    sphere_mesh_init(&mario_sphere, 50.0f, 1.0f, 0.2f, 0.2f);  /* red = Mario */

    /* Ground mesh */
    CharMesh ground_mesh;
    char_mesh_init(&ground_mesh, 2);
    char_mesh_update(&ground_mesh, ground_positions, ground_normals, ground_colors, 2);

    /* Mario mesh */
    CharMesh mario_mesh;
    char_mesh_init(&mario_mesh, SM64_GEO_MAX_TRIANGLES);

    SM64MarioGeometryBuffers mario_geo;
    mario_geo.position = (float*)malloc(sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES);
    mario_geo.normal   = (float*)malloc(sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES);
    mario_geo.color    = (float*)malloc(sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES);
    mario_geo.uv       = (float*)malloc(sizeof(float) * 6 * SM64_GEO_MAX_TRIANGLES);
    mario_geo.numTrianglesUsed = 0;

    /* Jak mesh */
    CharMesh jak_mesh;
    char_mesh_init(&jak_mesh, JAK_GEO_MAX_TRIANGLES);

    JakGeometryBuffers jak_geo;
    jak_geo.position = (float*)calloc(JAK_GEO_MAX_TRIANGLES * 3 * 3, sizeof(float));
    jak_geo.normal   = (float*)calloc(JAK_GEO_MAX_TRIANGLES * 3 * 3, sizeof(float));
    jak_geo.color    = (float*)calloc(JAK_GEO_MAX_TRIANGLES * 3 * 4, sizeof(float));
    jak_geo.uv       = (float*)calloc(JAK_GEO_MAX_TRIANGLES * 3 * 2, sizeof(float));
    jak_geo.num_triangles_used = 0;

    SM64MarioInputs mario_inputs = {};
    SM64MarioState  mario_state  = {};
    JakInputs       jak_inputs   = {};
    JakState        jak_state    = {};

    /* ---- Jak state ---- */
    bool jak_ready = false;
    int32_t jak_id = -1;

    /* ---- Camera ---- */
    float cam_rot = 0.0f;
    float cam_pos[3] = {0, 500, 1500};
    bool cam_follow_jak = false;  /* Tab toggles between Mario/Jak */

    /* ---- Main loop ---- */
    printf("[INIT] Setup complete. Entering main loop...\n");
    printf("Controls: Arrow keys = move, X = jump, C = attack, Shift = rotate camera, Tab = toggle Mario/Jak camera\n");
    printf("Waiting for Jak runtime to boot (Mario is playable immediately)...\n\n");
    fflush(stdout);
    float tick_accum = 0.0f;
    uint64_t last_time = SDL_GetTicks();
    bool running = true;

    while (running) {
        /* Events */
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) running = false;
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_TAB) {
                cam_follow_jak = !cam_follow_jak;
                printf("[CAM] Now following %s\n", cam_follow_jak ? "Jak" : "Mario");
                fflush(stdout);
            }
        }

        /* Timing */
        uint64_t now = SDL_GetTicks();
        float dt = (now - last_time) / 1000.0f;
        last_time = now;
        tick_accum += dt;

        /* Input — gamepad takes priority, falls back to keyboard */
        const bool* kb = SDL_GetKeyboardState(NULL);
        float ix = 0, iy = 0;
        bool btn_a = false, btn_b = false, btn_z = false;

        if (gamepad && SDL_GamepadConnected(gamepad)) {
            float lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
            float ly = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
            /* deadzone */
            if (lx * lx + ly * ly > 0.04f) { ix = lx; iy = ly; }
            float rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
            if (fabsf(rx) > 0.2f) cam_rot += rx * dt * 3.0f;
            btn_a = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH);
            btn_b = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST);
            btn_z = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        } else {
            if (kb[SDL_SCANCODE_UP])    iy = -1;
            if (kb[SDL_SCANCODE_DOWN])  iy =  1;
            if (kb[SDL_SCANCODE_LEFT])  ix = -1;
            if (kb[SDL_SCANCODE_RIGHT]) ix =  1;
            btn_a = kb[SDL_SCANCODE_X];
            btn_b = kb[SDL_SCANCODE_C];
            btn_z = kb[SDL_SCANCODE_Z];
        }
        if (kb[SDL_SCANCODE_LSHIFT]) cam_rot += dt * 2.0f;
        if (kb[SDL_SCANCODE_RSHIFT]) cam_rot -= dt * 2.0f;

        /* Camera — follows Mario or Jak depending on toggle */
        float target[3];
        if (cam_follow_jak && jak_ready) {
            float jx = jak_state.position[0];
            float jy = jak_state.position[1];
            float jz = jak_state.position[2];
            if (jx == 0.0f && jy == 0.0f && jz == 0.0f) {
                jx = 500.0f; jy = 0.0f; jz = 0.0f;
            }
            target[0] = jx;
            target[1] = jy + 100.0f;
            target[2] = jz;
        } else {
            target[0] = mario_state.position[0];
            target[1] = mario_state.position[1] + 100.0f;
            target[2] = mario_state.position[2];
        }
        cam_pos[0] = target[0] + 1500.0f * cosf(cam_rot);
        cam_pos[1] = target[1] + 500.0f;
        cam_pos[2] = target[2] + 1500.0f * sinf(cam_rot);

        /* Mario inputs */
        mario_inputs.camLookX = target[0] - cam_pos[0];
        mario_inputs.camLookZ = target[2] - cam_pos[2];
        mario_inputs.stickX = ix;
        mario_inputs.stickY = iy;
        mario_inputs.buttonA = btn_a ? 1 : 0;
        mario_inputs.buttonB = btn_b ? 1 : 0;
        mario_inputs.buttonZ = btn_z ? 1 : 0;

        /* Jak inputs (mirror Mario) */
        jak_inputs.cam_x = mario_inputs.camLookX;
        jak_inputs.cam_z = mario_inputs.camLookZ;
        jak_inputs.stick_x = ix;
        jak_inputs.stick_y = -iy;
        jak_inputs.buttons = 0;
        if (btn_a) jak_inputs.buttons |= JAK_BUTTON_X;
        if (btn_b) jak_inputs.buttons |= JAK_BUTTON_CIRCLE;

        /* Physics ticks at 30fps */
        while (tick_accum >= 1.0f/30.0f) {
            tick_accum -= 1.0f/30.0f;

            if (marioId >= 0)
                sm64_mario_tick(marioId, &mario_inputs, &mario_state, &mario_geo);

            /* Check if Jak is ready */
            if (!jak_ready && jak_result >= 0 && jak_is_ready()) {
                jak_ready = true;
                printf("Jak runtime ready!\n");

                /* Share collision with Jak */
                JakSurface jak_surfs[2];
                for (int i = 0; i < 2; i++) {
                    jak_surfs[i].type = JAK_SURFACE_STONE;
                    jak_surfs[i].flags = 0;
                    for (int v = 0; v < 3; v++)
                        for (int c = 0; c < 3; c++)
                            jak_surfs[i].vertices[v][c] = (float)sm64_ground[i].vertices[v][c];
                    memset(jak_surfs[i].normal, 0, sizeof(float)*3);
                }
                jak_static_surfaces_load(jak_surfs, 2);

                /* Spawn Jak near Mario on the village1 ground */
                jak_id = jak_create(GROUND_CX + 300.0f, GROUND_Y + 100.0f, GROUND_CZ);
                printf("Jak spawned (id=%d) near test-zone\n\n", jak_id);
            }

            /* Tick Jak (once per frame — engine runs at its own rate internally) */
            if (jak_ready && jak_id >= 0) {
                jak_tick(jak_id, &jak_inputs, &jak_state, &jak_geo);
            }
        }

        /* Update meshes */
        if (mario_geo.numTrianglesUsed > 0) {
            char_mesh_update(&mario_mesh, mario_geo.position, mario_geo.normal,
                           mario_geo.color, mario_geo.numTrianglesUsed);
        }
        if (jak_geo.num_triangles_used > 0) {
            char_mesh_update(&jak_mesh, jak_geo.position, jak_geo.normal,
                           jak_geo.color, jak_geo.num_triangles_used);
        }

        /* ---- Render ---- */
        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        glUseProgram(prog);

        mat4 proj, view, mvp;
        mat4_perspective(proj, 45.0f, (float)w / (float)h, 10.0f, 50000.0f);
        float up[3] = {0, 1, 0};
        mat4_lookat(view, cam_pos, target, up);
        mat4_mul(mvp, proj, view);
        glUniformMatrix4fv(uMVP, 1, GL_FALSE, mvp);

        /* Draw ground */
        glDisable(GL_CULL_FACE);
        char_mesh_draw(&ground_mesh);
        glEnable(GL_CULL_FACE);

        /* Draw Mario */
        char_mesh_draw(&mario_mesh);

        /* Draw Jak */
        if (jak_geo.num_triangles_used > 0) {
            char_mesh_draw(&jak_mesh);
        }

        /* Draw debug marker spheres at character positions */
        {
            mat4 model, vp, sphere_mvp;
            mat4_mul(vp, proj, view);

            /* Mario marker (red sphere at Mario's position + 100 up) */
            if (marioId >= 0) {
                mat4_translate(model, mario_state.position[0],
                               mario_state.position[1] + 100.0f,
                               mario_state.position[2]);
                mat4_mul(sphere_mvp, vp, model);
                glUniformMatrix4fv(uMVP, 1, GL_FALSE, sphere_mvp);
                sphere_mesh_draw(&mario_sphere);
            }

            /* Jak marker (green sphere at Jak's position + 100 up) */
            /* If position is zero, show at spawn point instead */
            if (jak_ready) {
                float jx = jak_state.position[0];
                float jy = jak_state.position[1];
                float jz = jak_state.position[2];
                if (jx == 0.0f && jy == 0.0f && jz == 0.0f) {
                    jx = GROUND_CX + 300.0f; jy = (float)GROUND_Y; jz = (float)GROUND_CZ;
                }
                mat4_translate(model, jx, jy + 100.0f, jz);
                mat4_mul(sphere_mvp, vp, model);
                glUniformMatrix4fv(uMVP, 1, GL_FALSE, sphere_mvp);
                sphere_mesh_draw(&jak_sphere);
            }

            /* Reset MVP for next frame */
            glUniformMatrix4fv(uMVP, 1, GL_FALSE, mvp);
        }

        /* Print positions periodically */
        {
            static int frame_counter = 0;
            if (++frame_counter % 60 == 0) {
                printf("[POS] Mario=(%.1f, %.1f, %.1f)  Jak=(%.1f, %.1f, %.1f) ready=%d id=%d\n",
                       mario_state.position[0], mario_state.position[1], mario_state.position[2],
                       jak_state.position[0], jak_state.position[1], jak_state.position[2],
                       jak_ready ? 1 : 0, jak_id);
                fflush(stdout);
            }
        }

        SDL_GL_SwapWindow(window);
    }

    /* ---- Cleanup ---- */
    printf("Cleaning up...\n");
    if (marioId >= 0) sm64_mario_delete(marioId);
    sm64_global_terminate();

    if (jak_id >= 0) jak_delete(jak_id);
    if (jak_result >= 0) jak_global_terminate();

    free(mario_geo.position); free(mario_geo.normal);
    free(mario_geo.color); free(mario_geo.uv);
    free(jak_geo.position); free(jak_geo.normal);
    free(jak_geo.color); free(jak_geo.uv);

    if (gamepad) SDL_CloseGamepad(gamepad);
    SDL_GL_DestroyContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    printf("Done!\n");
    return 0;
}
