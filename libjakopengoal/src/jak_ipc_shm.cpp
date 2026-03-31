/**
 * jak_ipc_shm.cpp - Shared memory and event implementation.
 *
 * Windows: Uses CreateFileMapping/MapViewOfFile + CreateEvent.
 * Linux:   Uses shm_open/mmap + sem_open (future).
 */

#include "jak_ipc_shm.h"

#include <stdio.h>
#include <string.h>

#ifdef _WIN32

/* Build a name like "JakIPC_Shm_1234" */
static void build_name(char* buf, size_t bufsz, const char* prefix, uint32_t pid) {
  snprintf(buf, bufsz, "%s%u", prefix, pid);
}

struct JakIpcHandle jak_ipc_create(uint32_t pid) {
  struct JakIpcHandle h;
  memset(&h, 0, sizeof(h));
  h.is_owner = true;

  char name[128];

  /* Create shared memory */
  build_name(name, sizeof(name), JAK_IPC_SHM_PREFIX, pid);
  h.hMapFile = CreateFileMappingA(
      INVALID_HANDLE_VALUE,
      NULL,
      PAGE_READWRITE,
      0,
      (DWORD)sizeof(struct JakSharedMemory),
      name);
  if (!h.hMapFile) {
    fprintf(stderr, "[jak_ipc] CreateFileMapping failed: %lu\n", GetLastError());
    return h;
  }

  h.shm = (struct JakSharedMemory*)MapViewOfFile(
      h.hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(struct JakSharedMemory));
  if (!h.shm) {
    fprintf(stderr, "[jak_ipc] MapViewOfFile failed: %lu\n", GetLastError());
    CloseHandle(h.hMapFile);
    h.hMapFile = NULL;
    return h;
  }

  /* Zero-init the shared memory */
  memset(h.shm, 0, sizeof(struct JakSharedMemory));
  h.shm->magic = JAK_IPC_MAGIC;
  h.shm->version = JAK_IPC_VERSION;
  h.shm->server_state = JAK_IPC_STATE_UNINITIALIZED;
  h.shm->jak_id = -1;

  /* Create named events */
  build_name(name, sizeof(name), JAK_IPC_EVT_FRAME_REQ, pid);
  h.hEvtFrameReq = CreateEventA(NULL, FALSE, FALSE, name);  /* auto-reset */

  build_name(name, sizeof(name), JAK_IPC_EVT_FRAME_DONE, pid);
  h.hEvtFrameDone = CreateEventA(NULL, FALSE, FALSE, name);

  build_name(name, sizeof(name), JAK_IPC_EVT_SHUTDOWN, pid);
  h.hEvtShutdown = CreateEventA(NULL, TRUE, FALSE, name);  /* manual-reset */

  if (!h.hEvtFrameReq || !h.hEvtFrameDone || !h.hEvtShutdown) {
    fprintf(stderr, "[jak_ipc] CreateEvent failed: %lu\n", GetLastError());
    jak_ipc_close(&h);
    memset(&h, 0, sizeof(h));
    return h;
  }

  return h;
}

struct JakIpcHandle jak_ipc_open(uint32_t pid) {
  struct JakIpcHandle h;
  memset(&h, 0, sizeof(h));
  h.is_owner = false;

  char name[128];

  /* Open shared memory */
  build_name(name, sizeof(name), JAK_IPC_SHM_PREFIX, pid);
  h.hMapFile = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
  if (!h.hMapFile) {
    fprintf(stderr, "[jak_ipc] OpenFileMapping failed: %lu\n", GetLastError());
    return h;
  }

  h.shm = (struct JakSharedMemory*)MapViewOfFile(
      h.hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(struct JakSharedMemory));
  if (!h.shm) {
    fprintf(stderr, "[jak_ipc] MapViewOfFile (open) failed: %lu\n", GetLastError());
    CloseHandle(h.hMapFile);
    h.hMapFile = NULL;
    return h;
  }

  /* Open named events */
  build_name(name, sizeof(name), JAK_IPC_EVT_FRAME_REQ, pid);
  h.hEvtFrameReq = OpenEventA(EVENT_ALL_ACCESS, FALSE, name);

  build_name(name, sizeof(name), JAK_IPC_EVT_FRAME_DONE, pid);
  h.hEvtFrameDone = OpenEventA(EVENT_ALL_ACCESS, FALSE, name);

  build_name(name, sizeof(name), JAK_IPC_EVT_SHUTDOWN, pid);
  h.hEvtShutdown = OpenEventA(EVENT_ALL_ACCESS, FALSE, name);

  if (!h.hEvtFrameReq || !h.hEvtFrameDone || !h.hEvtShutdown) {
    fprintf(stderr, "[jak_ipc] OpenEvent failed: %lu\n", GetLastError());
    jak_ipc_close(&h);
    memset(&h, 0, sizeof(h));
    return h;
  }

  /* Verify magic */
  if (h.shm->magic != JAK_IPC_MAGIC) {
    fprintf(stderr, "[jak_ipc] Bad magic: 0x%08X (expected 0x%08X)\n",
            h.shm->magic, JAK_IPC_MAGIC);
    jak_ipc_close(&h);
    memset(&h, 0, sizeof(h));
    return h;
  }

  return h;
}

void jak_ipc_close(struct JakIpcHandle* h) {
  if (!h) return;
  if (h->shm) {
    UnmapViewOfFile(h->shm);
    h->shm = NULL;
  }
  if (h->hMapFile) { CloseHandle(h->hMapFile); h->hMapFile = NULL; }
  if (h->hEvtFrameReq) { CloseHandle(h->hEvtFrameReq); h->hEvtFrameReq = NULL; }
  if (h->hEvtFrameDone) { CloseHandle(h->hEvtFrameDone); h->hEvtFrameDone = NULL; }
  if (h->hEvtShutdown) { CloseHandle(h->hEvtShutdown); h->hEvtShutdown = NULL; }
}

#else
/* Linux/macOS stubs — future implementation with shm_open/mmap/sem_open */

struct JakIpcHandle jak_ipc_create(uint32_t pid) {
  struct JakIpcHandle h;
  memset(&h, 0, sizeof(h));
  fprintf(stderr, "[jak_ipc] POSIX shared memory not yet implemented\n");
  return h;
}

struct JakIpcHandle jak_ipc_open(uint32_t pid) {
  struct JakIpcHandle h;
  memset(&h, 0, sizeof(h));
  fprintf(stderr, "[jak_ipc] POSIX shared memory not yet implemented\n");
  return h;
}

void jak_ipc_close(struct JakIpcHandle* h) {
  (void)h;
}

#endif  /* _WIN32 */

/* -------------------------------------------------------------------------- */
/*  Command ring buffer (lock-free SPSC)                                      */
/* -------------------------------------------------------------------------- */

bool jak_ipc_push_command(struct JakIpcHandle* h, const struct JakIpcCommand* cmd) {
  if (!h || !h->shm || !cmd) return false;

  volatile struct JakSharedMemory* shm = h->shm;
  uint32_t write = shm->cmd_write_head;
  uint32_t read = shm->cmd_read_head;
  uint32_t next = (write + 1) & (JAK_IPC_CMD_RING_SIZE - 1);

  if (next == read) {
    return false;  /* ring full */
  }

  memcpy((void*)&shm->cmd_ring[write], cmd, sizeof(struct JakIpcCommand));

  /* Memory barrier before publishing write head */
#ifdef _WIN32
  MemoryBarrier();
#else
  __sync_synchronize();
#endif

  shm->cmd_write_head = next;
  return true;
}

bool jak_ipc_pop_command(struct JakIpcHandle* h, struct JakIpcCommand* out) {
  if (!h || !h->shm || !out) return false;

  volatile struct JakSharedMemory* shm = h->shm;
  uint32_t write = shm->cmd_write_head;
  uint32_t read = shm->cmd_read_head;

  if (read == write) {
    return false;  /* ring empty */
  }

  /* Memory barrier before reading data */
#ifdef _WIN32
  MemoryBarrier();
#else
  __sync_synchronize();
#endif

  memcpy(out, (const void*)&shm->cmd_ring[read], sizeof(struct JakIpcCommand));
  shm->cmd_read_head = (read + 1) & (JAK_IPC_CMD_RING_SIZE - 1);
  return true;
}
