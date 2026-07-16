/**
 * jakopengoal.h - Jak and Daxter integration for SM64EX via libjakopengoal.
 *
 * Dynamically loads jakopengoal_thin.dll and launches jak_server.exe to run
 * Jak alongside Mario in the SM64 world. Jak uses SM64's collision surfaces,
 * controller inputs, and camera data.
 *
 * Follows the Discord RPC integration pattern (LoadLibrary + GetProcAddress).
 */

#ifndef JAKOPENGOAL_H
#define JAKOPENGOAL_H

#include <stdbool.h>
#include <stdint.h>

/* ---- State query ---- */
bool jak_is_loaded(void);       /* DLL loaded and runtime booted? */
bool jak_is_active(void);       /* Jak spawned and ticking? */

/* ---- Lifecycle ---- */
void jak_sm64_init(void);       /* Load DLL, boot runtime, load collision */
void jak_sm64_shutdown(void);   /* Kill Jak, unload DLL */

/* ---- Per-frame ---- */
void jak_sm64_pre_update(void); /* Sync Mario pos before object updates */
void jak_sm64_update(void);     /* Tick Jak with SM64 inputs & collision */
void jak_sm64_render(void);     /* Render debug sphere chain via OpenGL */
void jak_render_hud(void);      /* Draw debug text on HUD (positions, dist) */

/* ---- Toggle ---- */
void jak_sm64_toggle(void);     /* Spawn or despawn Jak */

/* ---- Mario fallback ---- */
bool jak_mario_should_fallback(void);  /* True when Mario handles the current action */

/* ---- gk-focus input mode ---- */
bool jak_gk_focus_active(void);     /* True while W is held (controller -> Jak world) */
void jak_sm64_filter_input(void);   /* Zero SM64 controller state while gk-focus held */

/* ---- Held object hack ---- */
#include "types.h"
extern struct Object *g_jak_held_obj;

/* ---- gk world picture-in-picture ----
 * g_jak_world_view: boot-time switch — must be true before the DLL boots
 * for the gk renderer to start. g_jak_world_view_visible: runtime overlay
 * toggle (ImGui menu on Left Alt / debug menu), defaults to hidden. */
extern bool g_jak_world_view;
extern bool g_jak_world_view_visible;
extern bool g_jak_display_swapped;  /* Tab: gk fullscreen + SM64 PiP; forces gk-focus */

/* ---- Jak shadow (persisted in jak_settings.txt next to the exe) ---- */
extern bool g_jak_shadow;
void jak_settings_save(void);

/* ---- Debug fly (hold R2 while enabled) ---- */
extern bool g_jak_debug_fly;

/* ---- Controller list/swap (ImGui Controllers section) ---- */
int  jak_controller_count(void);
const char *jak_controller_name(int idx);
bool jak_controller_is_gamepad(int idx);
int  jak_controller_active(void);        /* device index in use, -1 if none */
void jak_controller_select(int idx);     /* swap both Jak + SM64 input to idx */
int  jak_controller_override_index(void);

/* ---- ImGui overlay (jak_imgui.cpp) ---- */
void jak_imgui_frame(void);  /* Call once per frame before buffer swap */

#endif /* JAKOPENGOAL_H */
