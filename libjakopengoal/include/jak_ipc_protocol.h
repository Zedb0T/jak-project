/**
 * jak_ipc_protocol.h - Shared memory IPC protocol between jakopengoal_thin.dll and jak_server.exe.
 *
 * This header defines the memory layout for the shared memory region used to
 * communicate between the Blender addon (thin DLL) and the GOAL runtime server.
 * Pure C header, no external dependencies.
 */

#pragma once

#include <stdint.h>

#include "libjakopengoal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Magic and version */
#define JAK_IPC_MAGIC   0x4A414B53  /* "JAKS" */
#define JAK_IPC_VERSION 1

/* Server state enum */
#define JAK_IPC_STATE_UNINITIALIZED 0
#define JAK_IPC_STATE_BOOTING       1
#define JAK_IPC_STATE_READY         2
#define JAK_IPC_STATE_ERROR         3
#define JAK_IPC_STATE_EXITING       4

/* Command ring size (must be power of 2) */
#define JAK_IPC_CMD_RING_SIZE 64

/* Max static surfaces for upload */
#define JAK_IPC_MAX_SURFACES 8192

/* IPC command slot */
#pragma pack(push, 1)
struct JakIpcCommand {
  uint8_t  type;       /* CommandType enum value */
  uint8_t  pad[3];
  int32_t  jak_id;
  float    f[4];       /* generic float args */
  int32_t  i[2];       /* generic int args */
};

/* Double-buffered geometry output */
struct JakIpcGeoBuf {
  uint32_t num_triangles;
  uint32_t valid;
  float    position[JAK_GEO_MAX_TRIANGLES * 3 * 3];  /* xyz per vertex */
  float    normal[JAK_GEO_MAX_TRIANGLES * 3 * 3];
  float    color[JAK_GEO_MAX_TRIANGLES * 3 * 4];     /* rgba per vertex */
  float    uv[JAK_GEO_MAX_TRIANGLES * 3 * 2];        /* uv per vertex */
};

/* Full shared memory layout */
struct JakSharedMemory {
  /* Header */
  uint32_t magic;
  uint32_t version;
  uint32_t server_state;
  uint32_t client_sequence;
  uint32_t server_sequence;
  uint32_t active_buffer_index;
  uint32_t error_code;
  uint32_t jak_id;  /* current jak instance id (-1 if none) */

  /* Command ring (host -> server) */
  uint32_t cmd_write_head;
  uint32_t cmd_read_head;
  struct JakIpcCommand cmd_ring[JAK_IPC_CMD_RING_SIZE];

  /* Input buffer (host -> server, per frame) */
  struct JakInputs inputs;
  uint32_t input_valid;

  /* State output (server -> host, per frame) */
  struct JakState state;
  uint32_t state_valid;
  uint32_t _pad_state;

  /* Static surface upload (host -> server, one-shot) */
  uint32_t surface_upload_count;
  uint32_t surface_upload_ready;
  struct JakSurface surface_data[JAK_IPC_MAX_SURFACES];

  /* Double-buffered geometry (server -> host) */
  struct JakIpcGeoBuf geo_buffers[2];

  /* Texture atlas (server -> host, one-shot) */
  uint32_t tex_width;
  uint32_t tex_height;
  uint32_t tex_num_textures;
  uint32_t tex_data_ready;
  uint8_t  tex_data[JAK_TEXTURE_WIDTH * JAK_TEXTURE_HEIGHT * 4];
};
#pragma pack(pop)

/* Event name helpers (append PID for uniqueness) */
#define JAK_IPC_SHM_PREFIX       "JakIPC_Shm_"
#define JAK_IPC_EVT_FRAME_REQ    "JakIPC_FrameReq_"
#define JAK_IPC_EVT_FRAME_DONE   "JakIPC_FrameDone_"
#define JAK_IPC_EVT_SHUTDOWN     "JakIPC_Shutdown_"

#ifdef __cplusplus
}
#endif
