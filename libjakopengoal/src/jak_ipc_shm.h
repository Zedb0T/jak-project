/**
 * jak_ipc_shm.h - RAII wrapper for Windows shared memory and named events.
 *
 * Used by both jak_server.exe (creates) and jakopengoal_thin.dll (opens).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "libjakopengoal/include/jak_ipc_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Handle bundle for shared memory + sync events */
struct JakIpcHandle {
#ifdef _WIN32
  HANDLE hMapFile;
  HANDLE hEvtFrameReq;
  HANDLE hEvtFrameDone;
  HANDLE hEvtShutdown;
#else
  int fd;
#endif
  struct JakSharedMemory* shm;
  bool is_owner;  /* true = created (server), false = opened (client) */
};

/**
 * Create shared memory region and events (server side).
 * @param pid  PID to append to object names for uniqueness.
 * @return     Initialized handle, or handle with shm==NULL on failure.
 */
struct JakIpcHandle jak_ipc_create(uint32_t pid);

/**
 * Open existing shared memory region and events (client side).
 * @param pid  PID used when creating the objects.
 * @return     Initialized handle, or handle with shm==NULL on failure.
 */
struct JakIpcHandle jak_ipc_open(uint32_t pid);

/**
 * Close and clean up all handles.
 */
void jak_ipc_close(struct JakIpcHandle* h);

/**
 * Push a command into the ring buffer (client side).
 * @return true if command was enqueued, false if ring is full.
 */
bool jak_ipc_push_command(struct JakIpcHandle* h, const struct JakIpcCommand* cmd);

/**
 * Pop a command from the ring buffer (server side).
 * @return true if a command was dequeued, false if ring is empty.
 */
bool jak_ipc_pop_command(struct JakIpcHandle* h, struct JakIpcCommand* out);

#ifdef __cplusplus
}
#endif
