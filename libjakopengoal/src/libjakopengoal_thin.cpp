/**
 * libjakopengoal_thin.cpp - Thin DLL for Blender integration.
 *
 * This DLL contains ONLY the C API surface. It does NOT link the GOAL runtime.
 * Instead, it launches jak_server.exe as a subprocess and communicates via
 * shared memory (CreateFileMapping/MapViewOfFile) and named events.
 *
 * The thin DLL is ~30KB and loads safely in Blender's process because it has
 * no static constructors from graphics libraries.
 */

#include "libjakopengoal/include/libjakopengoal.h"
#include "jak_ipc_shm.h"

#include <cstring>
#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* -------------------------------------------------------------------------- */
/*  IPC command type enum (must match jak_server_main.cpp)                    */
/* -------------------------------------------------------------------------- */

enum IpcCmdType : uint8_t {
  IPC_CMD_NONE = 0,
  IPC_CMD_CREATE = 1,
  IPC_CMD_DELETE = 2,
  IPC_CMD_SET_POSITION = 3,
  IPC_CMD_SET_VELOCITY = 4,
  IPC_CMD_SET_ANGLE = 5,
  IPC_CMD_SET_ACTION = 6,
  IPC_CMD_SET_HEALTH = 7,
  IPC_CMD_TAKE_DAMAGE = 8,
  IPC_CMD_HEAL = 9,
  IPC_CMD_KILL = 10,
  IPC_CMD_GIVE_ECO = 11,
  IPC_CMD_LOAD_SURFACES = 12,
  IPC_CMD_CLEAR_SURFACES = 13,
  IPC_CMD_FIND_FLOOR = 14,
};

/* -------------------------------------------------------------------------- */
/*  Static state                                                              */
/* -------------------------------------------------------------------------- */

static JakIpcHandle s_ipc;
static bool s_initialized = false;

#ifdef _WIN32
static HANDLE s_server_process = NULL;
static DWORD s_server_pid = 0;
#endif

/* Timeout for frame request/response (ms) */
#define JAK_FRAME_TIMEOUT_MS 2000
#define JAK_BOOT_TIMEOUT_MS 60000

/* -------------------------------------------------------------------------- */
/*  Helper: send a command                                                    */
/* -------------------------------------------------------------------------- */

static bool send_cmd(uint8_t type, int32_t jak_id = 0,
                     float f0 = 0, float f1 = 0, float f2 = 0, float f3 = 0,
                     int32_t i0 = 0, int32_t i1 = 0) {
  if (!s_initialized || !s_ipc.shm) return false;
  JakIpcCommand cmd;
  memset(&cmd, 0, sizeof(cmd));
  cmd.type = type;
  cmd.jak_id = jak_id;
  cmd.f[0] = f0; cmd.f[1] = f1; cmd.f[2] = f2; cmd.f[3] = f3;
  cmd.i[0] = i0; cmd.i[1] = i1;
  return jak_ipc_push_command(&s_ipc, &cmd);
}

/* -------------------------------------------------------------------------- */
/*  Helper: request one frame from server                                     */
/* -------------------------------------------------------------------------- */

static bool request_frame(void) {
#ifdef _WIN32
  if (!s_initialized || !s_ipc.shm) return false;
  SetEvent(s_ipc.hEvtFrameReq);
  DWORD result = WaitForSingleObject(s_ipc.hEvtFrameDone, JAK_FRAME_TIMEOUT_MS);
  return (result == WAIT_OBJECT_0);
#else
  return false;
#endif
}

/* -------------------------------------------------------------------------- */
/*  DllMain                                                                   */
/* -------------------------------------------------------------------------- */

#ifdef _WIN32
BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved) {
  (void)hModule;
  (void)lpReserved;
  switch (reason) {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
      break;
  }
  return TRUE;
}
#endif

/* -------------------------------------------------------------------------- */
/*  C API Implementation                                                      */
/* -------------------------------------------------------------------------- */

extern "C" {

JAK_LIB_FN int32_t jak_global_init(const char* game_data_path, uint8_t* out_texture) {
  if (s_initialized) return 0;

#ifdef _WIN32
  /* Get our PID — used for unique shared memory names */
  DWORD my_pid = GetCurrentProcessId();

  /* Find jak_server.exe next to this DLL */
  char dll_path[MAX_PATH];
  HMODULE hSelf = NULL;
  GetModuleHandleExA(
      GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      (LPCSTR)jak_global_init, &hSelf);
  GetModuleFileNameA(hSelf, dll_path, MAX_PATH);

  /* Replace DLL filename with jak_server.exe */
  char* last_sep = strrchr(dll_path, '\\');
  if (!last_sep) last_sep = strrchr(dll_path, '/');
  if (last_sep) {
    *(last_sep + 1) = '\0';
  } else {
    dll_path[0] = '\0';
  }

  char server_path[MAX_PATH];
  snprintf(server_path, MAX_PATH, "%sjak_server.exe", dll_path);

  /* Build command line: jak_server.exe <pid> <game_data_path>
   * Strip trailing backslash from game_data_path to avoid the Windows C runtime
   * interpreting \" as an escaped quote instead of path-separator + closing-quote. */
  char clean_path[4096] = {0};
  if (game_data_path) {
    strncpy(clean_path, game_data_path, sizeof(clean_path) - 1);
    size_t len = strlen(clean_path);
    while (len > 0 && (clean_path[len - 1] == '\\' || clean_path[len - 1] == '/')) {
      clean_path[--len] = '\0';
    }
  }

  char cmdline[4096];
  snprintf(cmdline, sizeof(cmdline), "\"%s\" %u \"%s\"",
           server_path, (unsigned)my_pid, clean_path);

  fprintf(stderr, "[jakopengoal_thin] Launching server: %s\n", cmdline);

  /* Create a log file for server output (next to the DLL) */
  char log_path[MAX_PATH];
  snprintf(log_path, MAX_PATH, "%sjak_server.log", dll_path);

  SECURITY_ATTRIBUTES sa;
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = NULL;
  sa.bInheritHandle = TRUE;

  HANDLE hLogFile = CreateFileA(log_path, GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

  fprintf(stderr, "[jakopengoal_thin] Server log: %s\n", log_path);

  /* Launch subprocess with stderr/stdout redirected to log file */
  STARTUPINFOA si;
  PROCESS_INFORMATION pi;
  memset(&si, 0, sizeof(si));
  si.cb = sizeof(si);
  if (hLogFile != INVALID_HANDLE_VALUE) {
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hLogFile;
    si.hStdError = hLogFile;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  }
  memset(&pi, 0, sizeof(pi));

  if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                      CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
    fprintf(stderr, "[jakopengoal_thin] CreateProcess failed: %lu\n", GetLastError());
    if (hLogFile != INVALID_HANDLE_VALUE) CloseHandle(hLogFile);
    return -1;
  }

  s_server_process = pi.hProcess;
  s_server_pid = pi.dwProcessId;
  CloseHandle(pi.hThread);
  if (hLogFile != INVALID_HANDLE_VALUE) CloseHandle(hLogFile);

  /* Wait for server to create shared memory (poll with timeout) */
  DWORD start_tick = GetTickCount();
  while (true) {
    s_ipc = jak_ipc_open(my_pid);
    if (s_ipc.shm) break;

    if (GetTickCount() - start_tick > JAK_BOOT_TIMEOUT_MS) {
      fprintf(stderr, "[jakopengoal_thin] Timeout waiting for server shared memory\n");
      TerminateProcess(s_server_process, 1);
      CloseHandle(s_server_process);
      s_server_process = NULL;
      return -2;
    }

    /* Check if server process died */
    DWORD exit_code;
    if (GetExitCodeProcess(s_server_process, &exit_code) && exit_code != STILL_ACTIVE) {
      fprintf(stderr, "[jakopengoal_thin] Server process exited early (code %lu)\n", exit_code);
      CloseHandle(s_server_process);
      s_server_process = NULL;
      return -3;
    }

    Sleep(100);
  }

  fprintf(stderr, "[jakopengoal_thin] Connected to shared memory (magic=0x%08X)\n",
          s_ipc.shm->magic);

  s_initialized = true;

  /* If out_texture provided, we'll copy it once the server has it ready */
  /* (handled in jak_is_ready or jak_tick) */
  (void)out_texture;

  return 0;
#else
  fprintf(stderr, "[jakopengoal_thin] Only Windows is supported currently\n");
  return -1;
#endif
}

JAK_LIB_FN void jak_global_terminate(void) {
  if (!s_initialized) return;

#ifdef _WIN32
  /* Signal shutdown */
  if (s_ipc.hEvtShutdown) {
    SetEvent(s_ipc.hEvtShutdown);
  }

  /* Wait for server to exit (with timeout) */
  if (s_server_process) {
    DWORD result = WaitForSingleObject(s_server_process, 5000);
    if (result == WAIT_TIMEOUT) {
      fprintf(stderr, "[jakopengoal_thin] Server didn't exit in time, terminating\n");
      TerminateProcess(s_server_process, 1);
    }
    CloseHandle(s_server_process);
    s_server_process = NULL;
  }
#endif

  jak_ipc_close(&s_ipc);
  s_initialized = false;
  fprintf(stderr, "[jakopengoal_thin] Terminated\n");
}

JAK_LIB_FN bool jak_is_ready(void) {
  if (!s_initialized || !s_ipc.shm) return false;
  return s_ipc.shm->server_state == JAK_IPC_STATE_READY;
}

JAK_LIB_FN int32_t jak_create(float x, float y, float z) {
  if (!send_cmd(IPC_CMD_CREATE, 0, x, y, z)) return -1;
  return 0;  // Only one Jak instance (id=0)
}

JAK_LIB_FN void jak_delete(int32_t jak_id) {
  send_cmd(IPC_CMD_DELETE, jak_id);
}

JAK_LIB_FN void jak_tick(int32_t jak_id,
                          const struct JakInputs* inputs,
                          struct JakState* state,
                          struct JakGeometryBuffers* geometry) {
  if (!s_initialized || !s_ipc.shm) return;

  /* Write inputs to shared memory */
  if (inputs) {
    memcpy(&s_ipc.shm->inputs, inputs, sizeof(JakInputs));
    s_ipc.shm->input_valid = 1;
  } else {
    s_ipc.shm->input_valid = 0;
  }

  /* Request a frame from the server */
  if (!request_frame()) {
    fprintf(stderr, "[jakopengoal_thin] Frame request timed out!\n");
    return;
  }

  /* Read state from shared memory */
  if (state && s_ipc.shm->state_valid) {
    memcpy(state, &s_ipc.shm->state, sizeof(JakState));
  }

  /* Read geometry from active buffer */
  if (geometry) {
    uint32_t active = s_ipc.shm->active_buffer_index;
    const JakIpcGeoBuf* geo = &s_ipc.shm->geo_buffers[active];
    if (geo->valid && geo->num_triangles > 0) {
      uint32_t ntris = geo->num_triangles;
      if (ntris > JAK_GEO_MAX_TRIANGLES) ntris = JAK_GEO_MAX_TRIANGLES;
      uint32_t nverts = ntris * 3;

      if (geometry->position) memcpy(geometry->position, (const void*)geo->position, nverts * 3 * sizeof(float));
      if (geometry->normal) memcpy(geometry->normal, (const void*)geo->normal, nverts * 3 * sizeof(float));
      if (geometry->color) memcpy(geometry->color, (const void*)geo->color, nverts * 4 * sizeof(float));
      if (geometry->uv) memcpy(geometry->uv, (const void*)geo->uv, nverts * 2 * sizeof(float));
      geometry->num_triangles_used = (uint16_t)ntris;
    } else {
      geometry->num_triangles_used = 0;
    }
  }
}

JAK_LIB_FN void jak_static_surfaces_load(const struct JakSurface* surfaces, uint32_t count) {
  if (!s_initialized || !s_ipc.shm || !surfaces) return;
  if (count > JAK_IPC_MAX_SURFACES) count = JAK_IPC_MAX_SURFACES;

  /* Copy surface data to shared memory */
  memcpy(s_ipc.shm->surface_data, surfaces, count * sizeof(JakSurface));
  s_ipc.shm->surface_upload_count = count;
  s_ipc.shm->surface_upload_ready = 1;

  /* Send command to tell server to load them */
  send_cmd(IPC_CMD_LOAD_SURFACES);
}

JAK_LIB_FN void jak_static_surfaces_clear(void) {
  send_cmd(IPC_CMD_CLEAR_SURFACES);
}

JAK_LIB_FN uint32_t jak_surface_object_create(const struct JakSurfaceObject* obj) {
  /* Dynamic objects not yet implemented over IPC */
  (void)obj;
  return 0;
}

JAK_LIB_FN void jak_surface_object_move(uint32_t obj_id, const struct JakObjectTransform* t) {
  (void)obj_id;
  (void)t;
}

JAK_LIB_FN void jak_surface_object_delete(uint32_t obj_id) {
  (void)obj_id;
}

JAK_LIB_FN void jak_set_position(int32_t jak_id, float x, float y, float z) {
  send_cmd(IPC_CMD_SET_POSITION, jak_id, x, y, z);
}

JAK_LIB_FN void jak_set_velocity(int32_t jak_id, float vx, float vy, float vz) {
  send_cmd(IPC_CMD_SET_VELOCITY, jak_id, vx, vy, vz);
}

JAK_LIB_FN void jak_set_angle(int32_t jak_id, float yaw) {
  send_cmd(IPC_CMD_SET_ANGLE, jak_id, yaw);
}

JAK_LIB_FN void jak_set_action(int32_t jak_id, uint32_t action) {
  send_cmd(IPC_CMD_SET_ACTION, jak_id, 0, 0, 0, 0, (int32_t)action);
}

JAK_LIB_FN void jak_set_health(int32_t jak_id, float hp) {
  send_cmd(IPC_CMD_SET_HEALTH, jak_id, hp);
}

JAK_LIB_FN void jak_take_damage(int32_t jak_id, float from_x, float from_y, float from_z) {
  send_cmd(IPC_CMD_TAKE_DAMAGE, jak_id, from_x, from_y, from_z);
}

JAK_LIB_FN void jak_heal(int32_t jak_id, float amount) {
  send_cmd(IPC_CMD_HEAL, jak_id, amount);
}

JAK_LIB_FN void jak_kill(int32_t jak_id) {
  send_cmd(IPC_CMD_KILL, jak_id);
}

JAK_LIB_FN void jak_give_eco(int32_t jak_id, int32_t eco_type) {
  send_cmd(IPC_CMD_GIVE_ECO, jak_id, 0, 0, 0, 0, eco_type);
}

JAK_LIB_FN float jak_find_floor_height(float x, float y, float z) {
  /* Floor height queries require a synchronous round-trip.
     For now, send command and do a frame request to get the result. */
  // TODO: Implement synchronous floor query via shared memory
  (void)x; (void)y; (void)z;
  return -1000000.0f;
}

JAK_LIB_FN struct JakTextureInfo jak_get_texture_info(void) {
  JakTextureInfo info;
  if (s_initialized && s_ipc.shm && s_ipc.shm->tex_data_ready) {
    info.width = s_ipc.shm->tex_width;
    info.height = s_ipc.shm->tex_height;
    info.num_textures = s_ipc.shm->tex_num_textures;
  } else {
    info.width = JAK_TEXTURE_WIDTH;
    info.height = JAK_TEXTURE_HEIGHT;
    info.num_textures = 0;
  }
  return info;
}

JAK_LIB_FN void jak_register_debug_print(JakDebugPrintFunction fn) {
  /* Debug print callbacks can't cross process boundary — ignored in thin DLL */
  (void)fn;
}

JAK_LIB_FN void jak_register_play_sound(JakPlaySoundFunction fn) {
  /* Sound callbacks can't cross process boundary — ignored in thin DLL */
  (void)fn;
}

}  // extern "C"
