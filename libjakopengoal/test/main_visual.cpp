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

static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    printf("\n[CRASH] Unhandled Exception! Code=0x%08lX Address=%p\n", code, addr);
    fflush(stdout);
    FILE* f = fopen("C:\\temp\\mj_crash.txt", "w");
    if (f) {
        fprintf(f, "Exception Code=0x%08lX Address=%p\n", code, addr);
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>


// SDL3 (from jak-project third-party)
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

// OpenGL loader (glad, from jak-project third-party)
#include "glad/glad.h"

// libsm64 (header has its own extern "C" guards)
#include "libsm64.h"

// libjakopengoal
#include "libjakopengoal.h"
#include "libjakopengoal/src/jak_bridge.h"  // for BoneDebugData

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

/* Textured shader — used for Jak's mesh with texture atlas */
static const char* TEX_VERT_SRC = R"(
#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec3 aColor;
layout(location = 3) in vec2 aUV;

uniform mat4 uMVP;
out vec3 vColor;
out vec3 vNormal;
out vec2 vUV;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vColor = aColor;
    vNormal = aNormal;
    vUV = aUV;
}
)";

static const char* TEX_FRAG_SRC = R"(
#version 330 core
in vec3 vColor;
in vec3 vNormal;
in vec2 vUV;
out vec4 FragColor;

uniform sampler2D uTexture;

void main() {
    vec3 light = normalize(vec3(1.0, 1.0, 0.5));
    float diff = 0.4 + 0.6 * max(dot(normalize(vNormal), light), 0.0);
    vec4 texColor = texture(uTexture, vUV);
    // Multiply texture by vertex color and lighting (like merc2 shader)
    FragColor = vec4(vColor * texColor.rgb * diff * 2.0, texColor.a);
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
    GLuint vao, vbo_pos, vbo_nrm, vbo_col, vbo_uv;
    int num_verts;
    bool has_uv;
};

static void char_mesh_init(CharMesh* cm, int max_tris, bool with_uv = false) {
    int max_verts = max_tris * 3;
    cm->has_uv = with_uv;
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

    cm->vbo_uv = 0;
    if (with_uv) {
        glGenBuffers(1, &cm->vbo_uv);
        glBindBuffer(GL_ARRAY_BUFFER, cm->vbo_uv);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 2 * max_verts, NULL, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    }

    glBindVertexArray(0);
    cm->num_verts = 0;
}

static void char_mesh_update(CharMesh* cm, float* pos, float* nrm, float* col, int num_tris, float* uv = NULL) {
    cm->num_verts = num_tris * 3;
    glBindBuffer(GL_ARRAY_BUFFER, cm->vbo_pos);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float) * 3 * cm->num_verts, pos);
    glBindBuffer(GL_ARRAY_BUFFER, cm->vbo_nrm);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float) * 3 * cm->num_verts, nrm);
    glBindBuffer(GL_ARRAY_BUFFER, cm->vbo_col);
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float) * 3 * cm->num_verts, col);
    if (cm->has_uv && uv) {
        glBindBuffer(GL_ARRAY_BUFFER, cm->vbo_uv);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float) * 2 * cm->num_verts, uv);
    }
}

static void char_mesh_draw(CharMesh* cm) {
    glBindVertexArray(cm->vao);
    glDrawArrays(GL_TRIANGLES, 0, cm->num_verts);
    glBindVertexArray(0);
}

/* -------------------------------------------------------------------------- */
/*  Skeleton line mesh (for bone debug visualization)                          */
/* -------------------------------------------------------------------------- */

struct SkelMesh {
    GLuint vao, vbo_pos, vbo_col;
    int num_verts;  // 2 verts per line segment
};

static void skel_mesh_init(SkelMesh* sm, int max_bones) {
    int max_verts = max_bones * 8;  // 2 verts per bone line + 6 verts for cross marker
    glGenVertexArrays(1, &sm->vao);
    glBindVertexArray(sm->vao);

    glGenBuffers(1, &sm->vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, sm->vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 3 * max_verts, NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, NULL);

    glGenBuffers(1, &sm->vbo_col);
    glBindBuffer(GL_ARRAY_BUFFER, sm->vbo_col);
    glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 3 * max_verts, NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, 0, NULL);

    /* Normals not really needed for lines but shader expects them — use attrib 1 from col VBO */
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, NULL);

    glBindVertexArray(0);
    sm->num_verts = 0;
}

static void skel_mesh_update(SkelMesh* sm, const jak_bridge::BoneDebugData& bones) {
    /* Build line segments: bone -> parent, plus small cross markers at each joint */
    float positions[256 * 8 * 3];  // bone->parent lines + 3 cross lines per bone
    float colors[256 * 8 * 3];
    int vi = 0;

    float marker_size = 12.0f;

    for (int i = 0; i < bones.num_bones && i < 256; i++) {
        float bx = bones.positions[i][0];
        float by = bones.positions[i][1];
        float bz = bones.positions[i][2];

        /* Skip bones at origin (likely unused) */
        if (bx == 0.0f && by == 0.0f && bz == 0.0f) continue;

        int pi = bones.parent_indices[i];
        /* Color: gradient from green (root) to cyan (leaf) */
        float t = (float)i / (float)(bones.num_bones > 1 ? bones.num_bones - 1 : 1);
        float cr = 0.0f, cg = 1.0f, cb = t;

        if (pi >= 0 && pi < bones.num_bones) {
            float px = bones.positions[pi][0];
            float py = bones.positions[pi][1];
            float pz = bones.positions[pi][2];

            /* Line from this bone to parent */
            positions[vi*3+0]=bx; positions[vi*3+1]=by; positions[vi*3+2]=bz;
            colors[vi*3+0]=cr; colors[vi*3+1]=cg; colors[vi*3+2]=cb; vi++;
            positions[vi*3+0]=px; positions[vi*3+1]=py; positions[vi*3+2]=pz;
            colors[vi*3+0]=cr; colors[vi*3+1]=cg; colors[vi*3+2]=cb; vi++;
        }

        /* Cross-hair marker at joint (3 lines = 6 verts) */
        float mc_r = 1.0f, mc_g = 0.3f, mc_b = 0.3f;  /* red markers */
        /* X axis */
        positions[vi*3+0]=bx-marker_size; positions[vi*3+1]=by; positions[vi*3+2]=bz;
        colors[vi*3+0]=mc_r; colors[vi*3+1]=mc_g; colors[vi*3+2]=mc_b; vi++;
        positions[vi*3+0]=bx+marker_size; positions[vi*3+1]=by; positions[vi*3+2]=bz;
        colors[vi*3+0]=mc_r; colors[vi*3+1]=mc_g; colors[vi*3+2]=mc_b; vi++;
        /* Y axis */
        positions[vi*3+0]=bx; positions[vi*3+1]=by-marker_size; positions[vi*3+2]=bz;
        colors[vi*3+0]=mc_r; colors[vi*3+1]=mc_g; colors[vi*3+2]=mc_b; vi++;
        positions[vi*3+0]=bx; positions[vi*3+1]=by+marker_size; positions[vi*3+2]=bz;
        colors[vi*3+0]=mc_r; colors[vi*3+1]=mc_g; colors[vi*3+2]=mc_b; vi++;
        /* Z axis */
        positions[vi*3+0]=bx; positions[vi*3+1]=by; positions[vi*3+2]=bz-marker_size;
        colors[vi*3+0]=mc_r; colors[vi*3+1]=mc_g; colors[vi*3+2]=mc_b; vi++;
        positions[vi*3+0]=bx; positions[vi*3+1]=by; positions[vi*3+2]=bz+marker_size;
        colors[vi*3+0]=mc_r; colors[vi*3+1]=mc_g; colors[vi*3+2]=mc_b; vi++;
    }

    /* Upload */
    sm->num_verts = vi;
    if (vi > 0) {
        glBindBuffer(GL_ARRAY_BUFFER, sm->vbo_pos);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float) * 3 * vi, positions);
        glBindBuffer(GL_ARRAY_BUFFER, sm->vbo_col);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(float) * 3 * vi, colors);
    }
}

static void skel_mesh_draw(SkelMesh* sm) {
    if (sm->num_verts <= 0) return;
    glBindVertexArray(sm->vao);
    glDrawArrays(GL_LINES, 0, sm->num_verts);
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
#define GROUND_Y 42

/* Raised platform — a box the characters can walk onto */
#define PLAT_CX 500
#define PLAT_CZ 500
#define PLAT_W  1500      /* half-width */
#define PLAT_D  1500      /* half-depth */
#define PLAT_Y  200       /* top surface height */
#define PLAT_BOT GROUND_Y /* bottom = ground level */

/* All static collision surfaces: ground (2 tris) + platform top (2 tris) + 4 ramp/side faces (8 tris) */
static struct SM64Surface sm64_surfaces[] = {
    /* Ground plane (2 tris) */
    {0, 0, 0, {{GROUND_CX - GROUND_SIZE, GROUND_Y, GROUND_CZ + GROUND_SIZE},
               {GROUND_CX + GROUND_SIZE, GROUND_Y, GROUND_CZ - GROUND_SIZE},
               {GROUND_CX - GROUND_SIZE, GROUND_Y, GROUND_CZ - GROUND_SIZE}}},
    {0, 0, 0, {{GROUND_CX - GROUND_SIZE, GROUND_Y, GROUND_CZ + GROUND_SIZE},
               {GROUND_CX + GROUND_SIZE, GROUND_Y, GROUND_CZ + GROUND_SIZE},
               {GROUND_CX + GROUND_SIZE, GROUND_Y, GROUND_CZ - GROUND_SIZE}}},

    /* Platform top (2 tris) */
    {0, 0, 0, {{PLAT_CX - PLAT_W, PLAT_Y, PLAT_CZ + PLAT_D},
               {PLAT_CX + PLAT_W, PLAT_Y, PLAT_CZ - PLAT_D},
               {PLAT_CX - PLAT_W, PLAT_Y, PLAT_CZ - PLAT_D}}},
    {0, 0, 0, {{PLAT_CX - PLAT_W, PLAT_Y, PLAT_CZ + PLAT_D},
               {PLAT_CX + PLAT_W, PLAT_Y, PLAT_CZ + PLAT_D},
               {PLAT_CX + PLAT_W, PLAT_Y, PLAT_CZ - PLAT_D}}},

    /* Platform sides — walls so characters don't clip through from below */
    /* Front face (Z+) */
    {0, 0, 0, {{PLAT_CX - PLAT_W, PLAT_BOT, PLAT_CZ + PLAT_D},
               {PLAT_CX + PLAT_W, PLAT_Y,   PLAT_CZ + PLAT_D},
               {PLAT_CX - PLAT_W, PLAT_Y,   PLAT_CZ + PLAT_D}}},
    {0, 0, 0, {{PLAT_CX - PLAT_W, PLAT_BOT, PLAT_CZ + PLAT_D},
               {PLAT_CX + PLAT_W, PLAT_BOT, PLAT_CZ + PLAT_D},
               {PLAT_CX + PLAT_W, PLAT_Y,   PLAT_CZ + PLAT_D}}},
    /* Back face (Z-) */
    {0, 0, 0, {{PLAT_CX + PLAT_W, PLAT_BOT, PLAT_CZ - PLAT_D},
               {PLAT_CX - PLAT_W, PLAT_Y,   PLAT_CZ - PLAT_D},
               {PLAT_CX + PLAT_W, PLAT_Y,   PLAT_CZ - PLAT_D}}},
    {0, 0, 0, {{PLAT_CX + PLAT_W, PLAT_BOT, PLAT_CZ - PLAT_D},
               {PLAT_CX - PLAT_W, PLAT_BOT, PLAT_CZ - PLAT_D},
               {PLAT_CX - PLAT_W, PLAT_Y,   PLAT_CZ - PLAT_D}}},
    /* Left face (X-) */
    {0, 0, 0, {{PLAT_CX - PLAT_W, PLAT_BOT, PLAT_CZ - PLAT_D},
               {PLAT_CX - PLAT_W, PLAT_Y,   PLAT_CZ + PLAT_D},
               {PLAT_CX - PLAT_W, PLAT_Y,   PLAT_CZ - PLAT_D}}},
    {0, 0, 0, {{PLAT_CX - PLAT_W, PLAT_BOT, PLAT_CZ - PLAT_D},
               {PLAT_CX - PLAT_W, PLAT_BOT, PLAT_CZ + PLAT_D},
               {PLAT_CX - PLAT_W, PLAT_Y,   PLAT_CZ + PLAT_D}}},
    /* Right face (X+) */
    {0, 0, 0, {{PLAT_CX + PLAT_W, PLAT_BOT, PLAT_CZ + PLAT_D},
               {PLAT_CX + PLAT_W, PLAT_Y,   PLAT_CZ - PLAT_D},
               {PLAT_CX + PLAT_W, PLAT_Y,   PLAT_CZ + PLAT_D}}},
    {0, 0, 0, {{PLAT_CX + PLAT_W, PLAT_BOT, PLAT_CZ + PLAT_D},
               {PLAT_CX + PLAT_W, PLAT_BOT, PLAT_CZ - PLAT_D},
               {PLAT_CX + PLAT_W, PLAT_Y,   PLAT_CZ - PLAT_D}}},
};
static const int sm64_surface_count = 12;  /* 2 ground + 2 top + 8 sides */

/* Ground mesh for rendering (just the flat ground plane) */
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

/* Platform mesh for rendering (top + 4 sides = 10 tris = 30 verts) */
static float platform_positions[] = {
    /* Top face */
    PLAT_CX-PLAT_W, PLAT_Y, PLAT_CZ-PLAT_D,  PLAT_CX+PLAT_W, PLAT_Y, PLAT_CZ-PLAT_D,  PLAT_CX-PLAT_W, PLAT_Y, PLAT_CZ+PLAT_D,
    PLAT_CX+PLAT_W, PLAT_Y, PLAT_CZ-PLAT_D,  PLAT_CX+PLAT_W, PLAT_Y, PLAT_CZ+PLAT_D,  PLAT_CX-PLAT_W, PLAT_Y, PLAT_CZ+PLAT_D,
    /* Front face (Z+) */
    PLAT_CX-PLAT_W, PLAT_BOT, PLAT_CZ+PLAT_D,  PLAT_CX+PLAT_W, PLAT_Y, PLAT_CZ+PLAT_D,  PLAT_CX-PLAT_W, PLAT_Y, PLAT_CZ+PLAT_D,
    PLAT_CX-PLAT_W, PLAT_BOT, PLAT_CZ+PLAT_D,  PLAT_CX+PLAT_W, PLAT_BOT, PLAT_CZ+PLAT_D,  PLAT_CX+PLAT_W, PLAT_Y, PLAT_CZ+PLAT_D,
    /* Back face (Z-) */
    PLAT_CX+PLAT_W, PLAT_BOT, PLAT_CZ-PLAT_D,  PLAT_CX-PLAT_W, PLAT_Y, PLAT_CZ-PLAT_D,  PLAT_CX+PLAT_W, PLAT_Y, PLAT_CZ-PLAT_D,
    PLAT_CX+PLAT_W, PLAT_BOT, PLAT_CZ-PLAT_D,  PLAT_CX-PLAT_W, PLAT_BOT, PLAT_CZ-PLAT_D,  PLAT_CX-PLAT_W, PLAT_Y, PLAT_CZ-PLAT_D,
    /* Left face (X-) */
    PLAT_CX-PLAT_W, PLAT_BOT, PLAT_CZ-PLAT_D,  PLAT_CX-PLAT_W, PLAT_Y, PLAT_CZ+PLAT_D,  PLAT_CX-PLAT_W, PLAT_Y, PLAT_CZ-PLAT_D,
    PLAT_CX-PLAT_W, PLAT_BOT, PLAT_CZ-PLAT_D,  PLAT_CX-PLAT_W, PLAT_BOT, PLAT_CZ+PLAT_D,  PLAT_CX-PLAT_W, PLAT_Y, PLAT_CZ+PLAT_D,
    /* Right face (X+) */
    PLAT_CX+PLAT_W, PLAT_BOT, PLAT_CZ+PLAT_D,  PLAT_CX+PLAT_W, PLAT_Y, PLAT_CZ-PLAT_D,  PLAT_CX+PLAT_W, PLAT_Y, PLAT_CZ+PLAT_D,
    PLAT_CX+PLAT_W, PLAT_BOT, PLAT_CZ+PLAT_D,  PLAT_CX+PLAT_W, PLAT_BOT, PLAT_CZ-PLAT_D,  PLAT_CX+PLAT_W, PLAT_Y, PLAT_CZ-PLAT_D,
};
static float platform_normals[] = {
    /* Top */     0,1,0,  0,1,0,  0,1,0,   0,1,0,  0,1,0,  0,1,0,
    /* Front */   0,0,1,  0,0,1,  0,0,1,   0,0,1,  0,0,1,  0,0,1,
    /* Back */    0,0,-1, 0,0,-1, 0,0,-1,  0,0,-1, 0,0,-1, 0,0,-1,
    /* Left */   -1,0,0, -1,0,0, -1,0,0,  -1,0,0, -1,0,0, -1,0,0,
    /* Right */   1,0,0,  1,0,0,  1,0,0,   1,0,0,  1,0,0,  1,0,0,
};
static float platform_colors[] = {
    /* Top — tan/brown */
    0.6f,0.5f,0.3f, 0.6f,0.5f,0.3f, 0.6f,0.5f,0.3f,  0.6f,0.5f,0.3f, 0.6f,0.5f,0.3f, 0.6f,0.5f,0.3f,
    /* Front — slightly darker */
    0.5f,0.4f,0.25f, 0.5f,0.4f,0.25f, 0.5f,0.4f,0.25f,  0.5f,0.4f,0.25f, 0.5f,0.4f,0.25f, 0.5f,0.4f,0.25f,
    /* Back */
    0.45f,0.35f,0.2f, 0.45f,0.35f,0.2f, 0.45f,0.35f,0.2f,  0.45f,0.35f,0.2f, 0.45f,0.35f,0.2f, 0.45f,0.35f,0.2f,
    /* Left */
    0.5f,0.4f,0.25f, 0.5f,0.4f,0.25f, 0.5f,0.4f,0.25f,  0.5f,0.4f,0.25f, 0.5f,0.4f,0.25f, 0.5f,0.4f,0.25f,
    /* Right */
    0.55f,0.45f,0.28f, 0.55f,0.45f,0.28f, 0.55f,0.45f,0.28f,  0.55f,0.45f,0.28f, 0.55f,0.45f,0.28f, 0.55f,0.45f,0.28f,
};
static const int PLATFORM_TRI_COUNT = 10;

/* Spawned platform — appears after 4 jumps, positioned to the left of spawn */
#define SPAWN_CX  -800
#define SPAWN_CZ  500
#define SPAWN_W   600       /* half-width */
#define SPAWN_D   600       /* half-depth */
#define SPAWN_Y   350       /* top surface height (higher than existing platform) */
#define SPAWN_BOT GROUND_Y  /* bottom = ground level */

/* SM64 surfaces for the spawned platform (2 top + 8 sides = 10 tris) */
static struct SM64Surface spawn_sm64_surfaces[] = {
    /* Top (2 tris) */
    {0, 0, 0, {{SPAWN_CX - SPAWN_W, SPAWN_Y, SPAWN_CZ + SPAWN_D},
               {SPAWN_CX + SPAWN_W, SPAWN_Y, SPAWN_CZ - SPAWN_D},
               {SPAWN_CX - SPAWN_W, SPAWN_Y, SPAWN_CZ - SPAWN_D}}},
    {0, 0, 0, {{SPAWN_CX - SPAWN_W, SPAWN_Y, SPAWN_CZ + SPAWN_D},
               {SPAWN_CX + SPAWN_W, SPAWN_Y, SPAWN_CZ + SPAWN_D},
               {SPAWN_CX + SPAWN_W, SPAWN_Y, SPAWN_CZ - SPAWN_D}}},
    /* Front (Z+) */
    {0, 0, 0, {{SPAWN_CX - SPAWN_W, SPAWN_BOT, SPAWN_CZ + SPAWN_D},
               {SPAWN_CX + SPAWN_W, SPAWN_Y,   SPAWN_CZ + SPAWN_D},
               {SPAWN_CX - SPAWN_W, SPAWN_Y,   SPAWN_CZ + SPAWN_D}}},
    {0, 0, 0, {{SPAWN_CX - SPAWN_W, SPAWN_BOT, SPAWN_CZ + SPAWN_D},
               {SPAWN_CX + SPAWN_W, SPAWN_BOT, SPAWN_CZ + SPAWN_D},
               {SPAWN_CX + SPAWN_W, SPAWN_Y,   SPAWN_CZ + SPAWN_D}}},
    /* Back (Z-) */
    {0, 0, 0, {{SPAWN_CX + SPAWN_W, SPAWN_BOT, SPAWN_CZ - SPAWN_D},
               {SPAWN_CX - SPAWN_W, SPAWN_Y,   SPAWN_CZ - SPAWN_D},
               {SPAWN_CX + SPAWN_W, SPAWN_Y,   SPAWN_CZ - SPAWN_D}}},
    {0, 0, 0, {{SPAWN_CX + SPAWN_W, SPAWN_BOT, SPAWN_CZ - SPAWN_D},
               {SPAWN_CX - SPAWN_W, SPAWN_BOT, SPAWN_CZ - SPAWN_D},
               {SPAWN_CX - SPAWN_W, SPAWN_Y,   SPAWN_CZ - SPAWN_D}}},
    /* Left (X-) */
    {0, 0, 0, {{SPAWN_CX - SPAWN_W, SPAWN_BOT, SPAWN_CZ - SPAWN_D},
               {SPAWN_CX - SPAWN_W, SPAWN_Y,   SPAWN_CZ + SPAWN_D},
               {SPAWN_CX - SPAWN_W, SPAWN_Y,   SPAWN_CZ - SPAWN_D}}},
    {0, 0, 0, {{SPAWN_CX - SPAWN_W, SPAWN_BOT, SPAWN_CZ - SPAWN_D},
               {SPAWN_CX - SPAWN_W, SPAWN_BOT, SPAWN_CZ + SPAWN_D},
               {SPAWN_CX - SPAWN_W, SPAWN_Y,   SPAWN_CZ + SPAWN_D}}},
    /* Right (X+) */
    {0, 0, 0, {{SPAWN_CX + SPAWN_W, SPAWN_BOT, SPAWN_CZ + SPAWN_D},
               {SPAWN_CX + SPAWN_W, SPAWN_Y,   SPAWN_CZ - SPAWN_D},
               {SPAWN_CX + SPAWN_W, SPAWN_Y,   SPAWN_CZ + SPAWN_D}}},
    {0, 0, 0, {{SPAWN_CX + SPAWN_W, SPAWN_BOT, SPAWN_CZ + SPAWN_D},
               {SPAWN_CX + SPAWN_W, SPAWN_BOT, SPAWN_CZ - SPAWN_D},
               {SPAWN_CX + SPAWN_W, SPAWN_Y,   SPAWN_CZ - SPAWN_D}}},
};
static const int SPAWN_SURFACE_COUNT = 10;

/* Render mesh for spawned platform (same layout as platform mesh) */
static float spawn_positions[] = {
    /* Top face */
    SPAWN_CX-SPAWN_W, SPAWN_Y, SPAWN_CZ-SPAWN_D,  SPAWN_CX+SPAWN_W, SPAWN_Y, SPAWN_CZ-SPAWN_D,  SPAWN_CX-SPAWN_W, SPAWN_Y, SPAWN_CZ+SPAWN_D,
    SPAWN_CX+SPAWN_W, SPAWN_Y, SPAWN_CZ-SPAWN_D,  SPAWN_CX+SPAWN_W, SPAWN_Y, SPAWN_CZ+SPAWN_D,  SPAWN_CX-SPAWN_W, SPAWN_Y, SPAWN_CZ+SPAWN_D,
    /* Front (Z+) */
    SPAWN_CX-SPAWN_W, SPAWN_BOT, SPAWN_CZ+SPAWN_D,  SPAWN_CX+SPAWN_W, SPAWN_Y, SPAWN_CZ+SPAWN_D,  SPAWN_CX-SPAWN_W, SPAWN_Y, SPAWN_CZ+SPAWN_D,
    SPAWN_CX-SPAWN_W, SPAWN_BOT, SPAWN_CZ+SPAWN_D,  SPAWN_CX+SPAWN_W, SPAWN_BOT, SPAWN_CZ+SPAWN_D,  SPAWN_CX+SPAWN_W, SPAWN_Y, SPAWN_CZ+SPAWN_D,
    /* Back (Z-) */
    SPAWN_CX+SPAWN_W, SPAWN_BOT, SPAWN_CZ-SPAWN_D,  SPAWN_CX-SPAWN_W, SPAWN_Y, SPAWN_CZ-SPAWN_D,  SPAWN_CX+SPAWN_W, SPAWN_Y, SPAWN_CZ-SPAWN_D,
    SPAWN_CX+SPAWN_W, SPAWN_BOT, SPAWN_CZ-SPAWN_D,  SPAWN_CX-SPAWN_W, SPAWN_BOT, SPAWN_CZ-SPAWN_D,  SPAWN_CX-SPAWN_W, SPAWN_Y, SPAWN_CZ-SPAWN_D,
    /* Left (X-) */
    SPAWN_CX-SPAWN_W, SPAWN_BOT, SPAWN_CZ-SPAWN_D,  SPAWN_CX-SPAWN_W, SPAWN_Y, SPAWN_CZ+SPAWN_D,  SPAWN_CX-SPAWN_W, SPAWN_Y, SPAWN_CZ-SPAWN_D,
    SPAWN_CX-SPAWN_W, SPAWN_BOT, SPAWN_CZ-SPAWN_D,  SPAWN_CX-SPAWN_W, SPAWN_BOT, SPAWN_CZ+SPAWN_D,  SPAWN_CX-SPAWN_W, SPAWN_Y, SPAWN_CZ+SPAWN_D,
    /* Right (X+) */
    SPAWN_CX+SPAWN_W, SPAWN_BOT, SPAWN_CZ+SPAWN_D,  SPAWN_CX+SPAWN_W, SPAWN_Y, SPAWN_CZ-SPAWN_D,  SPAWN_CX+SPAWN_W, SPAWN_Y, SPAWN_CZ+SPAWN_D,
    SPAWN_CX+SPAWN_W, SPAWN_BOT, SPAWN_CZ+SPAWN_D,  SPAWN_CX+SPAWN_W, SPAWN_BOT, SPAWN_CZ-SPAWN_D,  SPAWN_CX+SPAWN_W, SPAWN_Y, SPAWN_CZ-SPAWN_D,
};
static float spawn_normals[] = {
    /* Top */     0,1,0,  0,1,0,  0,1,0,   0,1,0,  0,1,0,  0,1,0,
    /* Front */   0,0,1,  0,0,1,  0,0,1,   0,0,1,  0,0,1,  0,0,1,
    /* Back */    0,0,-1, 0,0,-1, 0,0,-1,  0,0,-1, 0,0,-1, 0,0,-1,
    /* Left */   -1,0,0, -1,0,0, -1,0,0,  -1,0,0, -1,0,0, -1,0,0,
    /* Right */   1,0,0,  1,0,0,  1,0,0,   1,0,0,  1,0,0,  1,0,0,
};
static float spawn_colors[] = {
    /* Top — bright blue/purple */
    0.3f,0.3f,0.9f, 0.3f,0.3f,0.9f, 0.3f,0.3f,0.9f,  0.3f,0.3f,0.9f, 0.3f,0.3f,0.9f, 0.3f,0.3f,0.9f,
    /* Front */
    0.25f,0.25f,0.75f, 0.25f,0.25f,0.75f, 0.25f,0.25f,0.75f,  0.25f,0.25f,0.75f, 0.25f,0.25f,0.75f, 0.25f,0.25f,0.75f,
    /* Back */
    0.2f,0.2f,0.65f, 0.2f,0.2f,0.65f, 0.2f,0.2f,0.65f,  0.2f,0.2f,0.65f, 0.2f,0.2f,0.65f, 0.2f,0.2f,0.65f,
    /* Left */
    0.25f,0.25f,0.75f, 0.25f,0.25f,0.75f, 0.25f,0.25f,0.75f,  0.25f,0.25f,0.75f, 0.25f,0.25f,0.75f, 0.25f,0.25f,0.75f,
    /* Right */
    0.28f,0.28f,0.8f, 0.28f,0.28f,0.8f, 0.28f,0.28f,0.8f,  0.28f,0.28f,0.8f, 0.28f,0.28f,0.8f, 0.28f,0.28f,0.8f,
};
static const int SPAWN_TRI_COUNT = 10;

/* -------------------------------------------------------------------------- */
/*  Moving platform — SM64 surface object with oscillation + rotation          */
/* -------------------------------------------------------------------------- */

/* Platform slab dimensions (local space, centered at origin) */
#define MP_W  200       /* half-width (X) */
#define MP_D  200       /* half-depth (Z) */
#define MP_H  20        /* half-height (Y thickness) */

/* Animation parameters */
#define MP_CENTER_X  0.0f
#define MP_CENTER_Y  350.0f   /* above static platform (PLAT_Y=200) */
#define MP_CENTER_Z  500.0f
#define MP_AMPLITUDE 400.0f   /* oscillation distance in X */
#define MP_PERIOD    4.0f     /* seconds per full oscillation */
#define MP_ROT_SPEED 30.0f    /* degrees per second (Y rotation) — SM64 uses degrees */

/* Local-space SM64 surfaces for the moving platform slab (top + bottom + 4 sides = 12 tris) */
static struct SM64Surface mp_sm64_surfaces[] = {
    /* Top face (2 tris) — normal up */
    {0, 0, 0, {{ -MP_W, MP_H,  MP_D},
               {  MP_W, MP_H, -MP_D},
               { -MP_W, MP_H, -MP_D}}},
    {0, 0, 0, {{ -MP_W, MP_H,  MP_D},
               {  MP_W, MP_H,  MP_D},
               {  MP_W, MP_H, -MP_D}}},
    /* Bottom face (2 tris) — normal down */
    {0, 0, 0, {{ -MP_W, -MP_H, -MP_D},
               {  MP_W, -MP_H,  MP_D},
               { -MP_W, -MP_H,  MP_D}}},
    {0, 0, 0, {{ -MP_W, -MP_H, -MP_D},
               {  MP_W, -MP_H, -MP_D},
               {  MP_W, -MP_H,  MP_D}}},
    /* Front face Z+ (2 tris) */
    {0, 0, 0, {{ -MP_W, -MP_H,  MP_D},
               {  MP_W,  MP_H,  MP_D},
               { -MP_W,  MP_H,  MP_D}}},
    {0, 0, 0, {{ -MP_W, -MP_H,  MP_D},
               {  MP_W, -MP_H,  MP_D},
               {  MP_W,  MP_H,  MP_D}}},
    /* Back face Z- (2 tris) */
    {0, 0, 0, {{  MP_W, -MP_H, -MP_D},
               { -MP_W,  MP_H, -MP_D},
               {  MP_W,  MP_H, -MP_D}}},
    {0, 0, 0, {{  MP_W, -MP_H, -MP_D},
               { -MP_W, -MP_H, -MP_D},
               { -MP_W,  MP_H, -MP_D}}},
    /* Left face X- (2 tris) */
    {0, 0, 0, {{ -MP_W, -MP_H, -MP_D},
               { -MP_W,  MP_H,  MP_D},
               { -MP_W,  MP_H, -MP_D}}},
    {0, 0, 0, {{ -MP_W, -MP_H, -MP_D},
               { -MP_W, -MP_H,  MP_D},
               { -MP_W,  MP_H,  MP_D}}},
    /* Right face X+ (2 tris) */
    {0, 0, 0, {{  MP_W, -MP_H,  MP_D},
               {  MP_W,  MP_H, -MP_D},
               {  MP_W,  MP_H,  MP_D}}},
    {0, 0, 0, {{  MP_W, -MP_H,  MP_D},
               {  MP_W, -MP_H, -MP_D},
               {  MP_W,  MP_H, -MP_D}}},
};
static const int MP_SURFACE_COUNT = 12;
static const int MP_TRI_COUNT = 12;  /* for rendering */

/**
 * Build the render mesh for the moving platform, transformed by position + Y rotation.
 * Writes into caller-provided arrays (must hold MP_TRI_COUNT*3 verts = 36 verts).
 */
static void mp_build_render_mesh(float px, float py, float pz, float yaw,
                                  float* out_pos, float* out_nrm, float* out_col) {
    float cy = cosf(yaw), sy = sinf(yaw);

    /* Local-space vertices matching the SM64 surfaces above */
    struct { float x, y, z; } local_verts[MP_TRI_COUNT * 3];
    struct { float nx, ny, nz; } local_norms[MP_TRI_COUNT * 3];

    /* Build from the surface array */
    for (int t = 0; t < MP_TRI_COUNT; t++) {
        for (int v = 0; v < 3; v++) {
            int idx = t * 3 + v;
            local_verts[idx].x = (float)mp_sm64_surfaces[t].vertices[v][0];
            local_verts[idx].y = (float)mp_sm64_surfaces[t].vertices[v][1];
            local_verts[idx].z = (float)mp_sm64_surfaces[t].vertices[v][2];
        }
        /* Compute face normal from cross product */
        float ax = local_verts[t*3+1].x - local_verts[t*3].x;
        float ay = local_verts[t*3+1].y - local_verts[t*3].y;
        float az = local_verts[t*3+1].z - local_verts[t*3].z;
        float bx = local_verts[t*3+2].x - local_verts[t*3].x;
        float by = local_verts[t*3+2].y - local_verts[t*3].y;
        float bz = local_verts[t*3+2].z - local_verts[t*3].z;
        float nx = ay*bz - az*by, ny = az*bx - ax*bz, nz = ax*by - ay*bx;
        float len = sqrtf(nx*nx + ny*ny + nz*nz);
        if (len > 0.0001f) { nx /= len; ny /= len; nz /= len; }
        for (int v = 0; v < 3; v++) {
            local_norms[t*3+v].nx = nx;
            local_norms[t*3+v].ny = ny;
            local_norms[t*3+v].nz = nz;
        }
    }

    /* Transform to world space (Y-axis rotation + translation) */
    for (int i = 0; i < MP_TRI_COUNT * 3; i++) {
        float lx = local_verts[i].x, ly = local_verts[i].y, lz = local_verts[i].z;
        out_pos[i*3+0] = px + lx * cy + lz * sy;
        out_pos[i*3+1] = py + ly;
        out_pos[i*3+2] = pz - lx * sy + lz * cy;

        float lnx = local_norms[i].nx, lny = local_norms[i].ny, lnz = local_norms[i].nz;
        out_nrm[i*3+0] = lnx * cy + lnz * sy;
        out_nrm[i*3+1] = lny;
        out_nrm[i*3+2] = -lnx * sy + lnz * cy;

        /* Orange/red color for moving platform */
        out_col[i*3+0] = 0.9f;
        out_col[i*3+1] = 0.4f;
        out_col[i*3+2] = 0.1f;
    }
}

/* -------------------------------------------------------------------------- */
/*  Callbacks                                                                  */
/* -------------------------------------------------------------------------- */

static void sm64_debug(const char* msg) { printf("[SM64] %s\n", msg); fflush(stdout); }

/* -------------------------------------------------------------------------- */
/*  SM64 worker thread — SM64 must init and tick on the same thread           */
/* -------------------------------------------------------------------------- */

struct SM64Worker {
    // Shared state (protected by mutex)
    std::mutex mtx;
    std::condition_variable cv_request;  // main -> worker: "tick please"
    std::condition_variable cv_done;     // worker -> main: "tick done"

    // Init
    std::atomic<bool> init_done{false};
    int32_t mario_id = -1;

    // Per-frame tick
    bool tick_requested = false;
    bool tick_done = false;
    bool running = true;

    // Input/output buffers (double-buffered: main writes input, worker reads input/writes output)
    SM64MarioInputs inputs = {};
    SM64MarioState  state  = {};
    SM64MarioGeometryBuffers geo = {};

    // Pending surface object to create/delete on the worker thread (spawned platform)
    SM64SurfaceObject* pending_surface_obj = nullptr;
    int32_t pending_surface_delete = -1;  // object ID to delete, or -1
    uint32_t last_surface_obj_id = 0;     // ID returned by sm64_surface_object_create

    // Moving platform — separate tracking from spawned platform
    SM64SurfaceObject* pending_mp_create = nullptr;
    uint32_t mp_surface_id = 0;
    bool mp_created = false;
    SM64ObjectTransform* pending_mp_move = nullptr;  // queued transform update

    void thread_func(uint8_t* rom, uint8_t* tex,
                     SM64Surface* ground, int ground_count,
                     int cx, int cy, int cz) {
        sm64_global_terminate();
        sm64_global_init(rom, tex);
        sm64_static_surfaces_load(ground, ground_count);
        mario_id = sm64_mario_create(cx, cy + 500, cz);
        if (mario_id < 0)
            mario_id = sm64_mario_create(cx, cy, cz);
        free(rom);
        init_done.store(true);
        printf("[SM64] Init complete on worker thread. Mario id=%d\n", mario_id);
        fflush(stdout);

        // Tick loop — wait for requests from main thread
        while (true) {
            std::unique_lock<std::mutex> lock(mtx);
            cv_request.wait(lock, [this] { return tick_requested || !running; });
            if (!running) break;
            tick_requested = false;

            // Do the tick (with lock released so main thread can continue)
            SM64MarioInputs local_inputs = inputs;
            SM64SurfaceObject* surf_to_create = pending_surface_obj;
            pending_surface_obj = nullptr;
            int32_t surf_to_delete = pending_surface_delete;
            pending_surface_delete = -1;
            SM64SurfaceObject* mp_to_create = pending_mp_create;
            pending_mp_create = nullptr;
            SM64ObjectTransform* mp_to_move = pending_mp_move;
            pending_mp_move = nullptr;
            lock.unlock();

            // Create/delete surface objects on this thread (SM64 not thread-safe)
            if (surf_to_create) {
                last_surface_obj_id = sm64_surface_object_create(surf_to_create);
            }
            if (surf_to_delete >= 0) {
                sm64_surface_object_delete((uint32_t)surf_to_delete);
            }

            // Moving platform create/move
            if (mp_to_create) {
                mp_surface_id = sm64_surface_object_create(mp_to_create);
                mp_created = true;
                printf("[MP] Created SM64 surface object id=%u\n", mp_surface_id);
                fflush(stdout);
            }
            if (mp_to_move && mp_created) {
                sm64_surface_object_move(mp_surface_id, mp_to_move);
            }

            if (mario_id >= 0) {
                sm64_mario_tick(mario_id, &local_inputs, &state, &geo);
            }

            lock.lock();
            tick_done = true;
            lock.unlock();
            cv_done.notify_one();
        }
    }

    // Called from main thread — request a tick and wait for result
    void request_tick(const SM64MarioInputs& in) {
        {
            std::lock_guard<std::mutex> lock(mtx);
            inputs = in;
            tick_requested = true;
            tick_done = false;
        }
        cv_request.notify_one();

        // Wait for completion
        std::unique_lock<std::mutex> lock(mtx);
        cv_done.wait(lock, [this] { return tick_done; });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            running = false;
        }
        cv_request.notify_one();
    }
};
static void jak_debug(const char* msg) { printf("[JAK] %s\n", msg); fflush(stdout); }

/* -------------------------------------------------------------------------- */
/*  Main                                                                       */
/* -------------------------------------------------------------------------- */

static FILE* g_log = NULL;
#define LOG(...) do { printf(__VA_ARGS__); if (g_log) { fprintf(g_log, __VA_ARGS__); fflush(g_log); } } while(0)

int main(int argc, char** argv) {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_handler);
#endif
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    g_log = fopen("C:\\temp\\mj_log.txt", "w");
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
    uint8_t* rom = NULL;
    size_t rom_size = 0;
    FILE* f = fopen(rom_path, "rb");
    if (!f) {
        printf("WARNING: Cannot open ROM '%s' — Mario will be disabled\n", rom_path);
    } else {
        fseek(f, 0, SEEK_END);
        rom_size = ftell(f);
        rewind(f);
        rom = (uint8_t*)malloc(rom_size);
        fread(rom, 1, rom_size, f);
        fclose(f);
    }

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
    SDL_GL_SetSwapInterval(0);  /* No vsync — vsync blocks when window isn't visible */

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
    uint8_t* sm64_tex = NULL;
    SM64Worker sm64_worker;
    std::thread sm64_thread;
    int32_t marioId = -1;
    bool sm64_enabled = (rom != NULL);

    if (sm64_enabled) {
        LOG("[INIT] sm64_register_debug_print...\n");
        sm64_register_debug_print_function(sm64_debug);
        LOG("[INIT] Allocating SM64 texture...\n");
        sm64_tex = (uint8_t*)malloc(4 * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT);
        if (!sm64_tex) { LOG("ERROR: Failed to allocate SM64 texture\n"); return 1; }
        memset(sm64_tex, 0, 4 * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT);

        /* SM64 must init+tick on the SAME thread (it uses thread-local state internally) */
        LOG("[INIT] Starting SM64 worker thread...\n");
        sm64_worker.geo.position = (float*)malloc(sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES);
        sm64_worker.geo.normal   = (float*)malloc(sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES);
        sm64_worker.geo.color    = (float*)malloc(sizeof(float) * 9 * SM64_GEO_MAX_TRIANGLES);
        sm64_worker.geo.uv       = (float*)malloc(sizeof(float) * 6 * SM64_GEO_MAX_TRIANGLES);
        sm64_worker.geo.numTrianglesUsed = 0;

        sm64_thread = std::thread([&]() {
            sm64_worker.thread_func(rom, sm64_tex, sm64_surfaces, sm64_surface_count,
                                    GROUND_CX, GROUND_Y, GROUND_CZ);
        });

        /* Pump events while SM64 inits so Windows doesn't kill us */
        while (!sm64_worker.init_done.load()) {
            SDL_PumpEvents();
            SDL_Delay(16);
        }
        marioId = sm64_worker.mario_id;
        LOG("[INIT] SM64 init complete. Mario id=%d\n", marioId);

        /* Create the moving platform as an SM64 surface object */
        static SM64SurfaceObject mp_obj = {};
        mp_obj.transform.position[0] = MP_CENTER_X;
        mp_obj.transform.position[1] = MP_CENTER_Y;
        mp_obj.transform.position[2] = MP_CENTER_Z;
        mp_obj.transform.eulerRotation[0] = 0;
        mp_obj.transform.eulerRotation[1] = 0;
        mp_obj.transform.eulerRotation[2] = 0;
        mp_obj.surfaceCount = MP_SURFACE_COUNT;
        mp_obj.surfaces = mp_sm64_surfaces;
        {
            std::lock_guard<std::mutex> lock(sm64_worker.mtx);
            sm64_worker.pending_mp_create = &mp_obj;
        }
        LOG("[INIT] Queued moving platform creation at (%.0f, %.0f, %.0f)\n",
            MP_CENTER_X, MP_CENTER_Y, MP_CENTER_Z);
    } else {
        LOG("[INIT] SM64 disabled (no ROM)\n");
    }

    /* ---- Init libjakopengoal ---- */
    LOG("[INIT] jak_register_debug_print...\n");
    jak_register_debug_print(jak_debug);
    LOG("[INIT] Allocating Jak texture...\n");
    uint8_t* jak_tex = (uint8_t*)malloc(JAK_TEXTURE_WIDTH * JAK_TEXTURE_HEIGHT * 4);
    if (!jak_tex) { LOG("ERROR: Failed to allocate Jak texture\n"); return 1; }

    LOG("[INIT] jak_global_init...\n");
    int32_t jak_result = jak_global_init(jak_data, jak_tex);
    LOG("[INIT] Jak init: %s (code %d)\n", jak_result >= 0 ? "OK" : "FAILED", jak_result);

    /* ---- GL setup ---- */
    GLuint prog = create_program(VERT_SRC, FRAG_SRC);
    GLint uMVP = glGetUniformLocation(prog, "uMVP");

    /* Textured shader for Jak */
    GLuint tex_prog = create_program(TEX_VERT_SRC, TEX_FRAG_SRC);
    GLint tex_uMVP = glGetUniformLocation(tex_prog, "uMVP");
    GLint tex_uTexture = glGetUniformLocation(tex_prog, "uTexture");

    /* Jak texture atlas GL object — created later when atlas data is ready */
    GLuint jak_gl_texture = 0;
    bool jak_texture_created = false;

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);  /* Dark background to spot Jak */

    /* Marker spheres (radius=50 units) */
    SphereMesh jak_sphere, mario_sphere;
    sphere_mesh_init(&jak_sphere, 50.0f, 0.0f, 1.0f, 0.2f);   /* green = Jak */
    sphere_mesh_init(&mario_sphere, 50.0f, 1.0f, 0.2f, 0.2f);  /* red = Mario */

    /* Ground mesh */
    CharMesh ground_mesh;
    char_mesh_init(&ground_mesh, 2);
    char_mesh_update(&ground_mesh, ground_positions, ground_normals, ground_colors, 2);

    /* Platform mesh */
    CharMesh plat_mesh;
    char_mesh_init(&plat_mesh, PLATFORM_TRI_COUNT);
    char_mesh_update(&plat_mesh, platform_positions, platform_normals, platform_colors, PLATFORM_TRI_COUNT);

    /* Spawned platform mesh (created after 4 jumps) */
    CharMesh spawn_mesh;
    char_mesh_init(&spawn_mesh, SPAWN_TRI_COUNT);
    bool spawn_visible = false;
    int jump_count = 0;
    bool prev_btn_a = false;

    /* Moving platform mesh + animation state */
    CharMesh mp_mesh;
    char_mesh_init(&mp_mesh, MP_TRI_COUNT);
    float mp_time = 0.0f;
    static SM64ObjectTransform mp_transform = {};  /* persists for worker thread pointer */
    float mp_render_pos[MP_TRI_COUNT * 3 * 3];
    float mp_render_nrm[MP_TRI_COUNT * 3 * 3];
    float mp_render_col[MP_TRI_COUNT * 3 * 3];

    /* Mario mesh */
    CharMesh mario_mesh;
    char_mesh_init(&mario_mesh, SM64_GEO_MAX_TRIANGLES);

    /* mario_geo/state/inputs are in sm64_worker — use references for convenience */
    SM64MarioGeometryBuffers& mario_geo = sm64_worker.geo;
    SM64MarioState& mario_state = sm64_worker.state;
    SM64MarioInputs mario_inputs = {};

    /* Jak mesh (with UV support for textures) */
    CharMesh jak_mesh;
    char_mesh_init(&jak_mesh, JAK_GEO_MAX_TRIANGLES, true);

    /* Skeleton debug mesh */
    SkelMesh jak_skel;
    skel_mesh_init(&jak_skel, 256);

    JakGeometryBuffers jak_geo;
    jak_geo.position = (float*)calloc(JAK_GEO_MAX_TRIANGLES * 3 * 3, sizeof(float));
    jak_geo.normal   = (float*)calloc(JAK_GEO_MAX_TRIANGLES * 3 * 3, sizeof(float));
    jak_geo.color    = (float*)calloc(JAK_GEO_MAX_TRIANGLES * 3 * 4, sizeof(float));
    jak_geo.uv       = (float*)calloc(JAK_GEO_MAX_TRIANGLES * 3 * 2, sizeof(float));
    jak_geo.num_triangles_used = 0;

    JakInputs       jak_inputs   = {};
    JakState        jak_state    = {};

    /* ---- Jak state ---- */
    bool jak_ready = false;
    int32_t jak_id = -1;
    uint32_t jak_mp_id = 0;       /* Jak moving platform surface object ID */
    bool jak_mp_created = false;

    /* ---- Camera ---- */
    float cam_rot = 0.0f;
    float cam_pos[3] = {0, 500, 1500};
    bool cam_follow_jak = true;   /* Tab toggles between Mario/Jak */
    bool draw_debug_skeleton = false;  /* Toggle skeleton/sphere debug drawing */

    /* ---- Main loop ---- */
    LOG("\n[INIT] Setup complete. Ground at (%d, %d, %d)\n", GROUND_CX, GROUND_Y, GROUND_CZ);
    LOG("Controls: Stick/Arrows=move  A/X=jump  B/C=attack  RStick/Shift=camera  Tab=follow Jak\n\n");
    LOG("[DEBUG] Entering main loop...\n");
    float tick_accum = 0.0f;
    uint64_t last_time = SDL_GetTicks();
    bool running = true;
    int global_frame = 0;

    while (running) {
        global_frame++;

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
        bool btn_square = false, btn_circle = false, btn_l1 = false, btn_r1 = false;

        if (gamepad && SDL_GamepadConnected(gamepad)) {
            float lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
            float ly = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
            /* deadzone */
            if (lx * lx + ly * ly > 0.04f) { ix = lx; iy = ly; }
            float rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX) / 32767.0f;
            if (fabsf(rx) > 0.2f) cam_rot += rx * dt * 3.0f;
            btn_a = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH);       /* X / A */
            btn_b = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST);        /* Square / X */
            btn_z = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
            btn_square = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST);   /* Square */
            btn_circle = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_EAST);   /* Circle */
            btn_l1 = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
            btn_r1 = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        } else {
            if (kb[SDL_SCANCODE_UP])    iy = -1;
            if (kb[SDL_SCANCODE_DOWN])  iy =  1;
            if (kb[SDL_SCANCODE_LEFT])  ix = -1;
            if (kb[SDL_SCANCODE_RIGHT]) ix =  1;
            btn_a = kb[SDL_SCANCODE_X];          /* X button (jump) */
            btn_b = kb[SDL_SCANCODE_C];          /* Mario B */
            btn_z = kb[SDL_SCANCODE_Z];          /* Mario Z */
            btn_square = kb[SDL_SCANCODE_S];     /* Square (punch/spin) */
            btn_circle = kb[SDL_SCANCODE_C];     /* Circle (roll/attack) */
            btn_l1 = kb[SDL_SCANCODE_Q];         /* L1 */
            btn_r1 = kb[SDL_SCANCODE_E];         /* R1 */
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
        if (btn_a)      jak_inputs.buttons |= JAK_BUTTON_X;        /* jump */
        if (btn_square)  jak_inputs.buttons |= JAK_BUTTON_SQUARE;   /* punch/spin kick */
        if (btn_circle)  jak_inputs.buttons |= JAK_BUTTON_CIRCLE;   /* roll/yellow eco */
        if (btn_l1)      jak_inputs.buttons |= JAK_BUTTON_L1;
        if (btn_r1)      jak_inputs.buttons |= JAK_BUTTON_R1;

        /* Track jump presses — spawn platform after 4 jumps, despawn after 7 */
        if (btn_a && !prev_btn_a) {
            jump_count++;
            if (!spawn_visible && jump_count <= 4) {
                printf("[JUMP] Jump #%d/4\n", jump_count);
                fflush(stdout);
            }
            if (jump_count == 4 && !spawn_visible) {
                printf("[SPAWN] Spawning new platform!\n");
                fflush(stdout);

                /* Queue SM64 surface object creation on the worker thread */
                static SM64SurfaceObject spawn_obj = {};
                spawn_obj.transform.position[0] = 0;
                spawn_obj.transform.position[1] = 0;
                spawn_obj.transform.position[2] = 0;
                spawn_obj.surfaceCount = SPAWN_SURFACE_COUNT;
                spawn_obj.surfaces = spawn_sm64_surfaces;
                {
                    std::lock_guard<std::mutex> lock(sm64_worker.mtx);
                    sm64_worker.pending_surface_obj = &spawn_obj;
                }

                /* Reload Jak collision with original + spawned surfaces */
                if (jak_ready) {
                    int total = sm64_surface_count + SPAWN_SURFACE_COUNT;
                    JakSurface* all_surfs = new JakSurface[total];
                    for (int i = 0; i < sm64_surface_count; i++) {
                        all_surfs[i].type = JAK_SURFACE_STONE;
                        all_surfs[i].flags = 0;
                        for (int v = 0; v < 3; v++)
                            for (int c = 0; c < 3; c++)
                                all_surfs[i].vertices[v][c] = (float)sm64_surfaces[i].vertices[v][c];
                        memset(all_surfs[i].normal, 0, sizeof(float)*3);
                    }
                    for (int i = 0; i < SPAWN_SURFACE_COUNT; i++) {
                        int idx = sm64_surface_count + i;
                        all_surfs[idx].type = JAK_SURFACE_STONE;
                        all_surfs[idx].flags = 0;
                        for (int v = 0; v < 3; v++)
                            for (int c = 0; c < 3; c++)
                                all_surfs[idx].vertices[v][c] = (float)spawn_sm64_surfaces[i].vertices[v][c];
                        memset(all_surfs[idx].normal, 0, sizeof(float)*3);
                    }
                    jak_static_surfaces_load(all_surfs, total);
                    delete[] all_surfs;
                }

                /* Upload render mesh */
                char_mesh_update(&spawn_mesh, spawn_positions, spawn_normals, spawn_colors, SPAWN_TRI_COUNT);
                spawn_visible = true;
            } else if (jump_count == 7 && spawn_visible) {
                printf("[DESPAWN] Removing spawned platform!\n");
                fflush(stdout);

                /* Queue SM64 surface object deletion on the worker thread */
                {
                    std::lock_guard<std::mutex> lock(sm64_worker.mtx);
                    sm64_worker.pending_surface_delete = (int32_t)sm64_worker.last_surface_obj_id;
                }

                /* Reload Jak collision with only the original surfaces */
                if (jak_ready) {
                    JakSurface* orig_surfs = new JakSurface[sm64_surface_count];
                    for (int i = 0; i < sm64_surface_count; i++) {
                        orig_surfs[i].type = JAK_SURFACE_STONE;
                        orig_surfs[i].flags = 0;
                        for (int v = 0; v < 3; v++)
                            for (int c = 0; c < 3; c++)
                                orig_surfs[i].vertices[v][c] = (float)sm64_surfaces[i].vertices[v][c];
                        memset(orig_surfs[i].normal, 0, sizeof(float)*3);
                    }
                    jak_static_surfaces_load(orig_surfs, sm64_surface_count);
                    delete[] orig_surfs;
                }

                spawn_visible = false;
            }
        }
        prev_btn_a = btn_a;

        /* Physics ticks at 30fps */
        while (tick_accum >= 1.0f/30.0f) {
            tick_accum -= 1.0f/30.0f;

            /* Animate moving platform */
            {
                mp_time += 1.0f / 30.0f;
                float t = mp_time * 2.0f * (float)M_PI / MP_PERIOD;
                mp_transform.position[0] = MP_CENTER_X + MP_AMPLITUDE * sinf(t);
                mp_transform.position[1] = MP_CENTER_Y;
                mp_transform.position[2] = MP_CENTER_Z;
                mp_transform.eulerRotation[0] = 0;
                mp_transform.eulerRotation[1] = fmodf(mp_time * MP_ROT_SPEED, 360.0f);
                mp_transform.eulerRotation[2] = 0;

                /* Queue SM64 move on worker thread */
                {
                    std::lock_guard<std::mutex> lock(sm64_worker.mtx);
                    sm64_worker.pending_mp_move = &mp_transform;
                }
            }

            /* Move Jak's moving platform with the same transform */
            if (jak_mp_created) {
                JakObjectTransform jak_mp_t = {};
                jak_mp_t.position[0] = mp_transform.position[0];
                jak_mp_t.position[1] = mp_transform.position[1];
                jak_mp_t.position[2] = mp_transform.position[2];
                jak_mp_t.rotation[0] = mp_transform.eulerRotation[0];
                jak_mp_t.rotation[1] = mp_transform.eulerRotation[1];
                jak_mp_t.rotation[2] = mp_transform.eulerRotation[2];
                jak_surface_object_move(jak_mp_id, &jak_mp_t);
            }

            if (sm64_enabled && marioId >= 0) {
                sm64_worker.request_tick(mario_inputs);
            }

            /* Check if Jak is ready */
            if (!jak_ready && jak_result >= 0 && jak_is_ready()) {
                jak_ready = true;
                LOG("Jak runtime ready!\n");

                /* Share ALL collision surfaces with Jak (ground + platform) */
                JakSurface jak_surfs[12];
                for (int i = 0; i < sm64_surface_count; i++) {
                    jak_surfs[i].type = JAK_SURFACE_STONE;
                    jak_surfs[i].flags = 0;
                    for (int v = 0; v < 3; v++)
                        for (int c = 0; c < 3; c++)
                            jak_surfs[i].vertices[v][c] = (float)sm64_surfaces[i].vertices[v][c];
                    memset(jak_surfs[i].normal, 0, sizeof(float)*3);
                }
                jak_static_surfaces_load(jak_surfs, sm64_surface_count);

                jak_id = jak_create(GROUND_CX + 300.0f, GROUND_Y + 100.0f, GROUND_CZ);
                printf("[INIT] Jak spawned (id=%d)\n", jak_id);

                /* Create GL texture from the Jak texture atlas (populated during FR3 load) */
                JakTextureInfo tex_info = jak_get_texture_info();
                printf("[INIT] Jak texture atlas: %ux%u, %u textures\n",
                       tex_info.width, tex_info.height, tex_info.num_textures);
                if (tex_info.num_textures > 0 && jak_tex) {
                    glGenTextures(1, &jak_gl_texture);
                    glBindTexture(GL_TEXTURE_2D, jak_gl_texture);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                                 tex_info.width, tex_info.height, 0,
                                 GL_RGBA, GL_UNSIGNED_BYTE, jak_tex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    jak_texture_created = true;
                    printf("[INIT] Created GL texture for Jak atlas (%ux%u)\n",
                           tex_info.width, tex_info.height);
                }

                /* Create the Jak moving platform as a dynamic surface object */
                {
                    JakSurface jak_mp_surfs[MP_SURFACE_COUNT];
                    for (int i = 0; i < MP_SURFACE_COUNT; i++) {
                        jak_mp_surfs[i].type = JAK_SURFACE_STONE;
                        jak_mp_surfs[i].flags = 0;
                        for (int v = 0; v < 3; v++)
                            for (int c = 0; c < 3; c++)
                                jak_mp_surfs[i].vertices[v][c] = (float)mp_sm64_surfaces[i].vertices[v][c];
                        memset(jak_mp_surfs[i].normal, 0, sizeof(float)*3);
                    }
                    JakSurfaceObject jak_mp_obj = {};
                    jak_mp_obj.transform.position[0] = mp_transform.position[0];
                    jak_mp_obj.transform.position[1] = mp_transform.position[1];
                    jak_mp_obj.transform.position[2] = mp_transform.position[2];
                    jak_mp_obj.transform.rotation[0] = mp_transform.eulerRotation[0];
                    jak_mp_obj.transform.rotation[1] = mp_transform.eulerRotation[1];
                    jak_mp_obj.transform.rotation[2] = mp_transform.eulerRotation[2];
                    jak_mp_obj.surface_count = MP_SURFACE_COUNT;
                    jak_mp_obj.surfaces = jak_mp_surfs;
                    jak_mp_id = jak_surface_object_create(&jak_mp_obj);
                    jak_mp_created = true;
                    printf("[INIT] Jak moving platform created (id=%u)\n", jak_mp_id);
                }
            }

            /* Tick Jak (once per frame — engine runs at its own rate internally) */
            if (jak_ready && jak_id >= 0) {
                jak_tick(jak_id, &jak_inputs, &jak_state, &jak_geo);
                static bool logged_first_tick = false;
                if (!logged_first_tick) {
                    LOG("[DEBUG] jak_tick returned: num_triangles_used=%d\n", jak_geo.num_triangles_used);
                    logged_first_tick = true;
                }
            }
        }

        /* Update moving platform render mesh */
        if (sm64_enabled) {
            /* SM64 CONVERT_ANGLE negates: -(deg)/180*32768, so negate yaw for render to match */
            float yaw_rad = -mp_transform.eulerRotation[1] * (float)M_PI / 180.0f;
            mp_build_render_mesh(mp_transform.position[0], mp_transform.position[1],
                                 mp_transform.position[2], yaw_rad,
                                 mp_render_pos, mp_render_nrm, mp_render_col);
            char_mesh_update(&mp_mesh, mp_render_pos, mp_render_nrm, mp_render_col, MP_TRI_COUNT);
        }

        /* Update meshes */
        if (mario_geo.numTrianglesUsed > 0) {
            char_mesh_update(&mario_mesh, mario_geo.position, mario_geo.normal,
                           mario_geo.color, mario_geo.numTrianglesUsed);
        }
        if (jak_geo.num_triangles_used > 0) {
            static bool printed_tri = false;
            if (!printed_tri) {
                LOG("[JAK GEO] %d triangles, first tri:\n", jak_geo.num_triangles_used);
                for (int vi = 0; vi < 3 && vi * 3 + 2 < jak_geo.num_triangles_used * 9; vi++) {
                    LOG("  v%d: pos=(%.1f, %.1f, %.1f) col=(%.2f, %.2f, %.2f)\n",
                           vi,
                           jak_geo.position[vi*3+0], jak_geo.position[vi*3+1], jak_geo.position[vi*3+2],
                           jak_geo.color[vi*3+0], jak_geo.color[vi*3+1], jak_geo.color[vi*3+2]);
                }
                /* Also print min/max bounds of all positions */
                float minx=1e9,miny=1e9,minz=1e9,maxx=-1e9,maxy=-1e9,maxz=-1e9;
                for (int i = 0; i < jak_geo.num_triangles_used * 3; i++) {
                    float x=jak_geo.position[i*3], y=jak_geo.position[i*3+1], z=jak_geo.position[i*3+2];
                    if(x<minx)minx=x; if(y<miny)miny=y; if(z<minz)minz=z;
                    if(x>maxx)maxx=x; if(y>maxy)maxy=y; if(z>maxz)maxz=z;
                }
                LOG("[JAK GEO] bounds: min=(%.1f, %.1f, %.1f) max=(%.1f, %.1f, %.1f)\n",
                       minx, miny, minz, maxx, maxy, maxz);
                printed_tri = true;
            }
            char_mesh_update(&jak_mesh, jak_geo.position, jak_geo.normal,
                           jak_geo.color, jak_geo.num_triangles_used, jak_geo.uv);
        }

        /* ---- Render ---- */
        /* Re-acquire GL context in case GOAL runtime stole it */
        SDL_GL_MakeCurrent(window, gl_ctx);

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

        /* Draw platform */
        char_mesh_draw(&plat_mesh);

        /* Draw spawned platform (if visible) */
        if (spawn_visible)
            char_mesh_draw(&spawn_mesh);

        /* Draw moving platform */
        if (sm64_enabled)
            char_mesh_draw(&mp_mesh);
        glEnable(GL_CULL_FACE);

        /* Draw Mario */
        char_mesh_draw(&mario_mesh);

        /* Draw Jak mesh (disable culling — GOAL winding order differs) */
        glDisable(GL_CULL_FACE);
        if (jak_mesh.num_verts > 0) {
            if (jak_texture_created && jak_gl_texture) {
                /* Use textured shader */
                glUseProgram(tex_prog);
                glUniformMatrix4fv(tex_uMVP, 1, GL_FALSE, mvp);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, jak_gl_texture);
                glUniform1i(tex_uTexture, 0);
                char_mesh_draw(&jak_mesh);
                glBindTexture(GL_TEXTURE_2D, 0);
                /* Switch back to non-textured shader */
                glUseProgram(prog);
                glUniformMatrix4fv(uMVP, 1, GL_FALSE, mvp);
            } else {
                char_mesh_draw(&jak_mesh);
            }
        }
        glEnable(GL_CULL_FACE);

        /* Draw Jak skeleton & debug markers (toggle with draw_debug_skeleton) */
        if (draw_debug_skeleton) {
            {
                auto& bone_data = jak_bridge::get_bone_debug_data();
                std::lock_guard<std::mutex> block(bone_data.mutex);
                if (bone_data.valid && bone_data.num_bones > 0) {
                    skel_mesh_update(&jak_skel, bone_data);
                    glDisable(GL_DEPTH_TEST);
                    glLineWidth(3.0f);
                    skel_mesh_draw(&jak_skel);
                    glLineWidth(1.0f);
                    glEnable(GL_DEPTH_TEST);
                }
            }
            {
                static int jak_dbg = 0;
                if (++jak_dbg % 120 == 1) {
                    auto& bone_data = jak_bridge::get_bone_debug_data();
                    std::lock_guard<std::mutex> block(bone_data.mutex);
                    printf("[JAK DBG] bones=%d valid=%d ready=%d id=%d",
                           bone_data.num_bones, bone_data.valid?1:0, jak_ready?1:0, jak_id);
                    if (bone_data.valid && bone_data.num_bones > 3) {
                        printf(" bone3=(%.1f,%.1f,%.1f)",
                               bone_data.positions[3][0], bone_data.positions[3][1], bone_data.positions[3][2]);
                    }
                    printf("\n");
                    fflush(stdout);
                }
            }
            /* Debug marker spheres */
            {
                mat4 model, vp, sphere_mvp;
                mat4_mul(vp, proj, view);
                if (marioId >= 0) {
                    mat4_translate(model, mario_state.position[0],
                                   mario_state.position[1] + 100.0f,
                                   mario_state.position[2]);
                    mat4_mul(sphere_mvp, vp, model);
                    glUniformMatrix4fv(uMVP, 1, GL_FALSE, sphere_mvp);
                    sphere_mesh_draw(&mario_sphere);
                }
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
                glUniformMatrix4fv(uMVP, 1, GL_FALSE, mvp);
            }
        }

        /* Print positions periodically */
        {
            static int frame_counter = 0;
            if (++frame_counter % 60 == 0) {
                printf("[POS] Mario=(%.1f, %.1f, %.1f)  Jak=(%.1f, %.1f, %.1f) ready=%d id=%d\n",
                       mario_state.position[0], mario_state.position[1], mario_state.position[2],
                       jak_state.position[0], jak_state.position[1], jak_state.position[2],
                       jak_ready ? 1 : 0, jak_id);
                if (sm64_enabled) {
                    float mp_px = mp_transform.position[0];
                    float mp_py = mp_transform.position[1];
                    float mp_pz = mp_transform.position[2];
                    float mp_yaw = mp_transform.eulerRotation[1];
                    float mp_top = mp_py + MP_H;
                    float mario_dx = mario_state.position[0] - mp_px;
                    float mario_dy = mario_state.position[1] - mp_top;
                    float mario_dz = mario_state.position[2] - mp_pz;
                    bool mario_above = (mario_dy >= -5.0f && mario_dy < 50.0f);
                    bool mario_in_xz = (fabsf(mario_dx) < MP_W + 50 && fabsf(mario_dz) < MP_D + 50);
                    printf("[MP] pos=(%.1f, %.1f, %.1f) yaw=%.2f top=%.1f | Mario dist=(%.1f, %.1f, %.1f) on_plat=%s\n",
                           mp_px, mp_py, mp_pz, mp_yaw, mp_top,
                           mario_dx, mario_dy, mario_dz,
                           (mario_above && mario_in_xz) ? "YES" : "no");
                    /* Print first render tri to verify mesh transform */
                    printf("[MP MESH] v0=(%.1f,%.1f,%.1f) v1=(%.1f,%.1f,%.1f) v2=(%.1f,%.1f,%.1f)\n",
                           mp_render_pos[0], mp_render_pos[1], mp_render_pos[2],
                           mp_render_pos[3], mp_render_pos[4], mp_render_pos[5],
                           mp_render_pos[6], mp_render_pos[7], mp_render_pos[8]);
                }
                if (jak_mp_created && jak_ready) {
                    float jak_dx = jak_state.position[0] - mp_transform.position[0];
                    float jak_dy = jak_state.position[1] - (mp_transform.position[1] + MP_H);
                    float jak_dz = jak_state.position[2] - mp_transform.position[2];
                    printf("[JAK MP] Jak dist from mp=(%.1f, %.1f, %.1f) jak_mp_id=%u\n",
                           jak_dx, jak_dy, jak_dz, jak_mp_id);
                }
                fflush(stdout);
            }
        }

        SDL_GL_SwapWindow(window);

        /* Frame limiter — cap at ~60fps to avoid tight-looping and GL context contention */
        SDL_Delay(1);
    }

    /* ---- Cleanup ---- */
    printf("Cleaning up...\n");

    /* Stop SM64 worker thread (it will call sm64_mario_delete/terminate on its thread) */
    if (sm64_enabled) {
        sm64_worker.stop();
        sm64_thread.join();
        if (marioId >= 0) sm64_mario_delete(marioId);
        sm64_global_terminate();
    }

    if (jak_id >= 0) jak_delete(jak_id);
    if (jak_result >= 0) jak_global_terminate();

    if (sm64_enabled) {
        free(sm64_worker.geo.position); free(sm64_worker.geo.normal);
        free(sm64_worker.geo.color); free(sm64_worker.geo.uv);
    }
    free(jak_geo.position); free(jak_geo.normal);
    free(jak_geo.color); free(jak_geo.uv);

    if (gamepad) SDL_CloseGamepad(gamepad);
    SDL_GL_DestroyContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();

    printf("Done!\n");
    return 0;
}
