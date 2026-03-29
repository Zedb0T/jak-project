/**
 * Combined libjakopengoal + libsm64 test program
 *
 * This demonstrates running both Mario (via libsm64) and Jak (via libjakopengoal)
 * in the same application, sharing collision geometry and rendering both characters.
 *
 * Build with -DWITH_LIBSM64 and link against both libraries.
 *
 * Without libsm64, this just runs the Jak portion as a standalone test.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "libjakopengoal.h"

#ifdef WITH_LIBSM64
#include "libsm64.h"
#endif

/* -------------------------------------------------------------------------- */
/*  Shared collision geometry                                                 */
/* -------------------------------------------------------------------------- */

#define GROUND_SIZE 5000.0f
#define GROUND_Y 0.0f

/* Build a flat ground + some platforms */
static struct JakSurface jak_surfaces[8];
static int num_jak_surfaces = 0;

#ifdef WITH_LIBSM64
static struct SM64Surface sm64_surfaces[8];
static int num_sm64_surfaces = 0;
#endif

static void build_shared_collision(void) {
  /* Ground triangle 1 */
  jak_surfaces[0].type = JAK_SURFACE_STONE;
  jak_surfaces[0].flags = 0;
  jak_surfaces[0].vertices[0][0] = -GROUND_SIZE;
  jak_surfaces[0].vertices[0][1] = GROUND_Y;
  jak_surfaces[0].vertices[0][2] = -GROUND_SIZE;
  jak_surfaces[0].vertices[1][0] = GROUND_SIZE;
  jak_surfaces[0].vertices[1][1] = GROUND_Y;
  jak_surfaces[0].vertices[1][2] = -GROUND_SIZE;
  jak_surfaces[0].vertices[2][0] = -GROUND_SIZE;
  jak_surfaces[0].vertices[2][1] = GROUND_Y;
  jak_surfaces[0].vertices[2][2] = GROUND_SIZE;
  memset(jak_surfaces[0].normal, 0, sizeof(float) * 3);

  /* Ground triangle 2 */
  jak_surfaces[1].type = JAK_SURFACE_STONE;
  jak_surfaces[1].flags = 0;
  jak_surfaces[1].vertices[0][0] = GROUND_SIZE;
  jak_surfaces[1].vertices[0][1] = GROUND_Y;
  jak_surfaces[1].vertices[0][2] = -GROUND_SIZE;
  jak_surfaces[1].vertices[1][0] = GROUND_SIZE;
  jak_surfaces[1].vertices[1][1] = GROUND_Y;
  jak_surfaces[1].vertices[1][2] = GROUND_SIZE;
  jak_surfaces[1].vertices[2][0] = -GROUND_SIZE;
  jak_surfaces[1].vertices[2][1] = GROUND_Y;
  jak_surfaces[1].vertices[2][2] = GROUND_SIZE;
  memset(jak_surfaces[1].normal, 0, sizeof(float) * 3);

  /* Elevated platform triangle 1 */
  jak_surfaces[2].type = JAK_SURFACE_STONE;
  jak_surfaces[2].flags = 0;
  jak_surfaces[2].vertices[0][0] = 300.0f;
  jak_surfaces[2].vertices[0][1] = 200.0f;
  jak_surfaces[2].vertices[0][2] = -200.0f;
  jak_surfaces[2].vertices[1][0] = 700.0f;
  jak_surfaces[2].vertices[1][1] = 200.0f;
  jak_surfaces[2].vertices[1][2] = -200.0f;
  jak_surfaces[2].vertices[2][0] = 300.0f;
  jak_surfaces[2].vertices[2][1] = 200.0f;
  jak_surfaces[2].vertices[2][2] = 200.0f;
  memset(jak_surfaces[2].normal, 0, sizeof(float) * 3);

  /* Elevated platform triangle 2 */
  jak_surfaces[3].type = JAK_SURFACE_STONE;
  jak_surfaces[3].flags = 0;
  jak_surfaces[3].vertices[0][0] = 700.0f;
  jak_surfaces[3].vertices[0][1] = 200.0f;
  jak_surfaces[3].vertices[0][2] = -200.0f;
  jak_surfaces[3].vertices[1][0] = 700.0f;
  jak_surfaces[3].vertices[1][1] = 200.0f;
  jak_surfaces[3].vertices[1][2] = 200.0f;
  jak_surfaces[3].vertices[2][0] = 300.0f;
  jak_surfaces[3].vertices[2][1] = 200.0f;
  jak_surfaces[3].vertices[2][2] = 200.0f;
  memset(jak_surfaces[3].normal, 0, sizeof(float) * 3);

  num_jak_surfaces = 4;

#ifdef WITH_LIBSM64
  /* Convert the same geometry to SM64 format */
  for (int i = 0; i < num_jak_surfaces; i++) {
    sm64_surfaces[i].type = 0;    /* SURFACE_DEFAULT */
    sm64_surfaces[i].force = 0;
    sm64_surfaces[i].terrain = 0;
    for (int v = 0; v < 3; v++) {
      for (int c = 0; c < 3; c++) {
        sm64_surfaces[i].vertices[v][c] = (int32_t)jak_surfaces[i].vertices[v][c];
      }
    }
  }
  num_sm64_surfaces = num_jak_surfaces;
#endif
}

/* -------------------------------------------------------------------------- */
/*  Debug callbacks                                                           */
/* -------------------------------------------------------------------------- */

static void jak_debug_print(const char* msg) {
  printf("[JAK] %s\n", msg);
}

#ifdef WITH_LIBSM64
static void sm64_debug_print(const char* msg) {
  printf("[SM64] %s\n", msg);
}
#endif

/* -------------------------------------------------------------------------- */
/*  Main                                                                      */
/* -------------------------------------------------------------------------- */

int main(int argc, char** argv) {
  const char* jak_data_path = ".";
#ifdef WITH_LIBSM64
  const char* sm64_rom_path = NULL;
#endif

  /* Parse args */
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--jak-data") == 0 && i + 1 < argc) {
      jak_data_path = argv[++i];
    }
#ifdef WITH_LIBSM64
    if (strcmp(argv[i], "--sm64-rom") == 0 && i + 1 < argc) {
      sm64_rom_path = argv[++i];
    }
#endif
  }

  printf("=== Combined libjakopengoal");
#ifdef WITH_LIBSM64
  printf(" + libsm64");
#endif
  printf(" test ===\n\n");

  /* Build shared collision */
  build_shared_collision();

  /* ---- Initialize libjakopengoal ---- */
  jak_register_debug_print(jak_debug_print);

  uint8_t jak_texture[JAK_TEXTURE_WIDTH * JAK_TEXTURE_HEIGHT * 4];
  if (jak_global_init(jak_data_path, jak_texture) < 0) {
    printf("ERROR: Failed to init libjakopengoal\n");
    return 1;
  }

#ifdef WITH_LIBSM64
  /* ---- Initialize libsm64 ---- */
  if (sm64_rom_path) {
    sm64_register_debug_print_function(sm64_debug_print);

    FILE* rom_file = fopen(sm64_rom_path, "rb");
    if (!rom_file) {
      printf("ERROR: Cannot open SM64 ROM: %s\n", sm64_rom_path);
      jak_global_terminate();
      return 1;
    }

    fseek(rom_file, 0, SEEK_END);
    size_t rom_size = ftell(rom_file);
    fseek(rom_file, 0, SEEK_SET);
    uint8_t* rom = (uint8_t*)malloc(rom_size);
    fread(rom, 1, rom_size, rom_file);
    fclose(rom_file);

    uint8_t sm64_texture[4 * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT];
    sm64_global_init(rom, sm64_texture);
    free(rom);

    /* Load collision for Mario */
    sm64_static_surfaces_load(sm64_surfaces, num_sm64_surfaces);
  }
#endif

  /* Wait for Jak runtime */
  printf("Waiting for GOAL runtime to boot...\n");
  int wait = 0;
  while (!jak_is_ready()) {
#ifdef _WIN32
    Sleep(100);
#else
    usleep(100000);
#endif
    if (++wait > 600) {
      printf("ERROR: Timeout\n");
      jak_global_terminate();
      return 1;
    }
  }
  printf("GOAL runtime ready!\n\n");

  /* Load collision for Jak */
  jak_static_surfaces_load(jak_surfaces, num_jak_surfaces);

  /* ---- Spawn characters ---- */
  int32_t jak_id = jak_create(0.0f, 100.0f, 0.0f);
  printf("Spawned Jak (id=%d)\n", jak_id);

#ifdef WITH_LIBSM64
  int32_t mario_id = -1;
  if (sm64_rom_path) {
    mario_id = sm64_mario_create(200, 100, 0);
    printf("Spawned Mario (id=%d)\n", mario_id);
  }
#endif

  /* ---- Main simulation loop ---- */
  printf("\nRunning combined simulation (300 ticks)...\n\n");

  struct JakInputs jak_inputs;
  struct JakState jak_state;
  memset(&jak_inputs, 0, sizeof(jak_inputs));

#ifdef WITH_LIBSM64
  struct SM64MarioInputs mario_inputs;
  struct SM64MarioState mario_state;
  struct SM64MarioGeometryBuffers mario_geo;
  float mario_pos_buf[SM64_GEO_MAX_TRIANGLES * 3 * 3];
  float mario_nrm_buf[SM64_GEO_MAX_TRIANGLES * 3 * 3];
  float mario_col_buf[SM64_GEO_MAX_TRIANGLES * 3 * 3];
  float mario_uv_buf[SM64_GEO_MAX_TRIANGLES * 3 * 2];
  mario_geo.position = mario_pos_buf;
  mario_geo.normal = mario_nrm_buf;
  mario_geo.color = mario_col_buf;
  mario_geo.uv = mario_uv_buf;
  memset(&mario_inputs, 0, sizeof(mario_inputs));
#endif

  for (int tick = 0; tick < 300; tick++) {
    /* Both characters walk forward */
    if (tick < 120) {
      jak_inputs.stick_y = 0.7f;
      jak_inputs.cam_z = 1.0f;
#ifdef WITH_LIBSM64
      mario_inputs.stickY = 0.7f;
      mario_inputs.camLookZ = 1.0f;
#endif
    } else {
      jak_inputs.stick_y = 0.0f;
#ifdef WITH_LIBSM64
      mario_inputs.stickY = 0.0f;
#endif
    }

    /* Tick both engines */
    jak_tick(jak_id, &jak_inputs, &jak_state, NULL);

#ifdef WITH_LIBSM64
    if (mario_id >= 0) {
      sm64_mario_tick(mario_id, &mario_inputs, &mario_state, &mario_geo);
    }
#endif

    /* Print state every 60 ticks */
    if (tick % 60 == 0) {
      printf("Tick %3d:\n", tick);
      printf("  Jak:   pos=(%7.1f, %7.1f, %7.1f) hp=%.2f ground=%s\n",
             jak_state.position[0], jak_state.position[1], jak_state.position[2],
             jak_state.hp, jak_state.on_ground ? "yes" : "no");
#ifdef WITH_LIBSM64
      if (mario_id >= 0) {
        printf("  Mario: pos=(%7.1f, %7.1f, %7.1f) hp=%d\n",
               mario_state.position[0], mario_state.position[1], mario_state.position[2],
               mario_state.health);
      }
#endif
      printf("\n");
    }
  }

  /* ---- Cleanup ---- */
  printf("Cleaning up...\n");
  jak_delete(jak_id);
  jak_global_terminate();

#ifdef WITH_LIBSM64
  if (mario_id >= 0) {
    sm64_mario_delete(mario_id);
    sm64_global_terminate();
  }
#endif

  printf("Done!\n");
  return 0;
}
