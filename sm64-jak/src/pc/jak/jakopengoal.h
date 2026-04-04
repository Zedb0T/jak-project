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
void jak_sm64_update(void);     /* Tick Jak with SM64 inputs & collision */
void jak_sm64_render(void);     /* Render debug sphere chain via OpenGL */
void jak_render_hud(void);      /* Draw debug text on HUD (positions, dist) */

/* ---- Toggle ---- */
void jak_sm64_toggle(void);     /* Spawn or despawn Jak */

/* ---- Held object hack ---- */
#include "types.h"
extern struct Object *g_jak_held_obj;

#endif /* JAKOPENGOAL_H */
