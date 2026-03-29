/**
 * libjakopengoal test program
 *
 * This demonstrates how to use the libjakopengoal API to embed Jak into
 * another application, following the same pattern as the libsm64 test program.
 *
 * The test creates a simple flat ground surface, spawns Jak, and runs
 * a simulation loop that prints Jak's state each frame.
 *
 * In a real integration (e.g., alongside libsm64), you would:
 *   1. Create collision surfaces from your game's level geometry
 *   2. Feed controller inputs each frame
 *   3. Read Jak's position/geometry and render it in your engine
 *   4. Use dynamic surface objects for moving platforms
 *
 * Example integration with libsm64 (pseudocode):
 *
 *   // Both libraries initialized
 *   sm64_global_init(rom, sm64_texture);
 *   jak_global_init(jak_data_path, jak_texture);
 *
 *   // Share the same collision surfaces
 *   sm64_static_surfaces_load(surfaces, count);
 *   jak_static_surfaces_load(jak_surfaces, count);  // Same geometry, different format
 *
 *   // Spawn both characters
 *   int mario = sm64_mario_create(0, 1000, 0);
 *   int jak = jak_create(500, 1000, 0);
 *
 *   // Main loop
 *   while (running) {
 *     sm64_mario_tick(mario, &mario_inputs, &mario_state, &mario_geo);
 *     jak_tick(jak, &jak_inputs, &jak_state, &jak_geo);
 *
 *     // Render both characters in your engine
 *     render_mesh(mario_geo);
 *     render_mesh(jak_geo);
 *   }
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

/* If building alongside libsm64, include it too */
#ifdef WITH_LIBSM64
#include "libsm64.h"
#endif

/* -------------------------------------------------------------------------- */
/*  Debug callback                                                            */
/* -------------------------------------------------------------------------- */

static void debug_print(const char* msg) {
  printf("[JAK DEBUG] %s\n", msg);
}

/* -------------------------------------------------------------------------- */
/*  Build test collision geometry (a simple flat platform)                    */
/* -------------------------------------------------------------------------- */

#define PLATFORM_SIZE 5000.0f
#define PLATFORM_Y 0.0f

static struct JakSurface* build_test_surfaces(uint32_t* out_count) {
  /* Create a flat square platform made of 2 triangles */
  struct JakSurface* surfaces = (struct JakSurface*)calloc(2, sizeof(struct JakSurface));

  /* Triangle 1: top-left half of the square */
  surfaces[0].type = JAK_SURFACE_STONE;
  surfaces[0].flags = 0;
  /* Vertex 0 */
  surfaces[0].vertices[0][0] = -PLATFORM_SIZE;
  surfaces[0].vertices[0][1] = PLATFORM_Y;
  surfaces[0].vertices[0][2] = -PLATFORM_SIZE;
  /* Vertex 1 */
  surfaces[0].vertices[1][0] = PLATFORM_SIZE;
  surfaces[0].vertices[1][1] = PLATFORM_Y;
  surfaces[0].vertices[1][2] = -PLATFORM_SIZE;
  /* Vertex 2 */
  surfaces[0].vertices[2][0] = -PLATFORM_SIZE;
  surfaces[0].vertices[2][1] = PLATFORM_Y;
  surfaces[0].vertices[2][2] = PLATFORM_SIZE;
  /* Normal will be auto-computed (pointing up) */

  /* Triangle 2: bottom-right half of the square */
  surfaces[1].type = JAK_SURFACE_STONE;
  surfaces[1].flags = 0;
  /* Vertex 0 */
  surfaces[1].vertices[0][0] = PLATFORM_SIZE;
  surfaces[1].vertices[0][1] = PLATFORM_Y;
  surfaces[1].vertices[0][2] = -PLATFORM_SIZE;
  /* Vertex 1 */
  surfaces[1].vertices[1][0] = PLATFORM_SIZE;
  surfaces[1].vertices[1][1] = PLATFORM_Y;
  surfaces[1].vertices[1][2] = PLATFORM_SIZE;
  /* Vertex 2 */
  surfaces[1].vertices[2][0] = -PLATFORM_SIZE;
  surfaces[1].vertices[2][1] = PLATFORM_Y;
  surfaces[1].vertices[2][2] = PLATFORM_SIZE;

  *out_count = 2;
  return surfaces;
}

/* -------------------------------------------------------------------------- */
/*  Build a simple ramp (dynamic surface object example)                     */
/* -------------------------------------------------------------------------- */

static struct JakSurfaceObject build_test_ramp(void) {
  struct JakSurfaceObject ramp;
  memset(&ramp, 0, sizeof(ramp));

  ramp.transform.position[0] = 200.0f;
  ramp.transform.position[1] = 0.0f;
  ramp.transform.position[2] = 0.0f;

  ramp.surface_count = 2;
  ramp.surfaces = (struct JakSurface*)calloc(2, sizeof(struct JakSurface));

  /* Ramp surface (angled up) - triangle 1 */
  ramp.surfaces[0].type = JAK_SURFACE_STONE;
  ramp.surfaces[0].vertices[0][0] = -100.0f;
  ramp.surfaces[0].vertices[0][1] = 0.0f;
  ramp.surfaces[0].vertices[0][2] = -100.0f;

  ramp.surfaces[0].vertices[1][0] = 100.0f;
  ramp.surfaces[0].vertices[1][1] = 0.0f;
  ramp.surfaces[0].vertices[1][2] = -100.0f;

  ramp.surfaces[0].vertices[2][0] = -100.0f;
  ramp.surfaces[0].vertices[2][1] = 200.0f;
  ramp.surfaces[0].vertices[2][2] = 100.0f;

  /* Ramp surface - triangle 2 */
  ramp.surfaces[1].type = JAK_SURFACE_STONE;
  ramp.surfaces[1].vertices[0][0] = 100.0f;
  ramp.surfaces[1].vertices[0][1] = 0.0f;
  ramp.surfaces[1].vertices[0][2] = -100.0f;

  ramp.surfaces[1].vertices[1][0] = 100.0f;
  ramp.surfaces[1].vertices[1][1] = 200.0f;
  ramp.surfaces[1].vertices[1][2] = 100.0f;

  ramp.surfaces[1].vertices[2][0] = -100.0f;
  ramp.surfaces[1].vertices[2][1] = 200.0f;
  ramp.surfaces[1].vertices[2][2] = 100.0f;

  return ramp;
}

/* -------------------------------------------------------------------------- */
/*  Main test loop                                                            */
/* -------------------------------------------------------------------------- */

int main(int argc, char** argv) {
  const char* data_path = ".";

  if (argc > 1) {
    data_path = argv[1];
  } else {
    printf("Usage: %s <jak-project-data-path>\n", argv[0]);
    printf("  The data path should contain iso_data/ and out/ directories.\n");
    printf("  Using current directory as default.\n\n");
  }

  printf("=== libjakopengoal test program ===\n\n");

  /* Register debug callback before init */
  jak_register_debug_print(debug_print);

  /* ---- Step 1: Initialize the library ---- */
  printf("[TEST] Initializing libjakopengoal with data path: %s\n", data_path);

  uint8_t texture[JAK_TEXTURE_WIDTH * JAK_TEXTURE_HEIGHT * 4];
  int32_t result = jak_global_init(data_path, texture);
  if (result < 0) {
    printf("[TEST] ERROR: Failed to initialize libjakopengoal (error %d)\n", result);
    return 1;
  }

  /* ---- Step 2: Wait for the GOAL runtime to boot ---- */
  printf("[TEST] Waiting for GOAL runtime to boot...\n");

  int wait_count = 0;
  while (!jak_is_ready()) {
    /* Sleep ~100ms */
#ifdef _WIN32
    Sleep(100);
#else
    usleep(100000);
#endif
    wait_count++;
    if (wait_count % 50 == 0) {
      printf("[TEST]   Still waiting... (%d seconds)\n", wait_count / 10);
    }
    if (wait_count > 600) { /* 60 second timeout */
      printf("[TEST] ERROR: Timeout waiting for GOAL runtime\n");
      jak_global_terminate();
      return 1;
    }
  }

  printf("[TEST] GOAL runtime is ready!\n\n");

  /* ---- Step 3: Load collision surfaces ---- */
  uint32_t surface_count = 0;
  struct JakSurface* surfaces = build_test_surfaces(&surface_count);
  jak_static_surfaces_load(surfaces, surface_count);
  printf("[TEST] Loaded %u static collision surfaces\n", surface_count);

  /* ---- Step 4: Create a dynamic surface object (ramp) ---- */
  struct JakSurfaceObject ramp = build_test_ramp();
  uint32_t ramp_id = jak_surface_object_create(&ramp);
  printf("[TEST] Created dynamic surface object (ramp) id=%u\n", ramp_id);

  /* ---- Step 5: Spawn Jak ---- */
  int32_t jak_id = jak_create(0.0f, 100.0f, 0.0f);
  if (jak_id < 0) {
    printf("[TEST] ERROR: Failed to spawn Jak (error %d)\n", jak_id);
    free(surfaces);
    free(ramp.surfaces);
    jak_global_terminate();
    return 1;
  }
  printf("[TEST] Spawned Jak (id=%d) at (0, 100, 0)\n\n", jak_id);

  /* ---- Step 6: Simulation loop ---- */
  printf("[TEST] Running simulation loop (300 ticks = 5 seconds at 60fps)...\n\n");

  /* Allocate geometry buffers */
  float* geo_pos = (float*)calloc(JAK_GEO_MAX_TRIANGLES * 3 * 3, sizeof(float));
  float* geo_nrm = (float*)calloc(JAK_GEO_MAX_TRIANGLES * 3 * 3, sizeof(float));
  float* geo_col = (float*)calloc(JAK_GEO_MAX_TRIANGLES * 3 * 4, sizeof(float));
  float* geo_uv = (float*)calloc(JAK_GEO_MAX_TRIANGLES * 3 * 2, sizeof(float));

  struct JakGeometryBuffers geometry;
  geometry.position = geo_pos;
  geometry.normal = geo_nrm;
  geometry.color = geo_col;
  geometry.uv = geo_uv;
  geometry.num_triangles_used = 0;

  struct JakInputs inputs;
  memset(&inputs, 0, sizeof(inputs));

  struct JakState state;

  for (int tick = 0; tick < 300; tick++) {
    /* Simulate some input: walk forward for 2 seconds, then jump */
    if (tick < 120) {
      /* Walk forward */
      inputs.stick_y = 0.8f;
      inputs.cam_x = 0.0f;
      inputs.cam_z = 1.0f;
      inputs.buttons = 0;
    } else if (tick == 120) {
      /* Jump */
      inputs.stick_y = 0.8f;
      inputs.buttons = JAK_BUTTON_X;
    } else if (tick < 180) {
      /* Continue walking */
      inputs.stick_y = 0.8f;
      inputs.buttons = 0;
    } else if (tick == 180) {
      /* Spin attack */
      inputs.buttons = JAK_BUTTON_CIRCLE;
    } else {
      inputs.buttons = 0;
      inputs.stick_y = 0.0f;
    }

    /* Animate the ramp (move it slowly) */
    if (tick % 30 == 0) {
      struct JakObjectTransform ramp_t;
      ramp_t.position[0] = 200.0f + (float)tick * 0.5f;
      ramp_t.position[1] = 0.0f;
      ramp_t.position[2] = 0.0f;
      ramp_t.rotation[0] = 0.0f;
      ramp_t.rotation[1] = 0.0f;
      ramp_t.rotation[2] = 0.0f;
      jak_surface_object_move(ramp_id, &ramp_t);
    }

    /* Tick the simulation */
    jak_tick(jak_id, &inputs, &state, &geometry);

    /* Print state every second (60 ticks) */
    if (tick % 60 == 0) {
      printf("[TEST] Tick %3d | Pos: (%8.1f, %8.1f, %8.1f) | "
             "Vel: (%6.1f, %6.1f, %6.1f) | HP: %.2f | "
             "Action: %u | Tris: %u | Ground: %s\n",
             tick, state.position[0], state.position[1], state.position[2], state.velocity[0],
             state.velocity[1], state.velocity[2], state.hp, state.action,
             geometry.num_triangles_used, state.on_ground ? "yes" : "no");
    }
  }

  /* ---- Step 7: Test state manipulation ---- */
  printf("\n[TEST] Testing state manipulation...\n");

  /* Teleport Jak */
  jak_set_position(jak_id, 500.0f, 200.0f, 300.0f);
  printf("[TEST] Teleported Jak to (500, 200, 300)\n");

  /* Tick a few times to let it take effect */
  for (int i = 0; i < 10; i++) {
    memset(&inputs, 0, sizeof(inputs));
    jak_tick(jak_id, &inputs, &state, NULL);
  }

  printf("[TEST] After teleport: Pos: (%.1f, %.1f, %.1f)\n", state.position[0], state.position[1],
         state.position[2]);

  /* Deal damage */
  jak_take_damage(jak_id, state.position[0] + 100.0f, state.position[1], state.position[2]);
  printf("[TEST] Dealt damage from the right\n");

  for (int i = 0; i < 10; i++) {
    jak_tick(jak_id, &inputs, &state, NULL);
  }
  printf("[TEST] After damage: HP: %.2f\n", state.hp);

  /* Test floor height query */
  float floor = jak_find_floor_height(0.0f, 100.0f, 0.0f);
  printf("[TEST] Floor height at (0, 100, 0): %.1f\n", floor);

  /* ---- Step 8: Cleanup ---- */
  printf("\n[TEST] Cleaning up...\n");

  jak_surface_object_delete(ramp_id);
  jak_delete(jak_id);

  free(geo_pos);
  free(geo_nrm);
  free(geo_col);
  free(geo_uv);
  free(surfaces);
  free(ramp.surfaces);

  jak_global_terminate();

  printf("[TEST] Done! All tests completed.\n");
  return 0;
}
