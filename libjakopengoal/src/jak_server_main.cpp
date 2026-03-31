/**
 * jak_server_main.cpp - Standalone server process for Blender integration.
 *
 * This process owns the GOAL runtime. It:
 *   1. Creates shared memory and sync events
 *   2. Boots the GOAL runtime on a background thread
 *   3. Runs a server loop that:
 *      - Waits for frame requests from the client (thin DLL in Blender)
 *      - Reads inputs from shared memory
 *      - Processes queued commands
 *      - Runs one tick of the bridge
 *      - Writes state + geometry to shared memory
 *      - Signals frame done
 *   4. Shuts down when the shutdown event is signaled
 *
 * Usage: jak_server.exe <pid> <game_data_path>
 *   <pid>             The client PID used for shared memory naming
 *   <game_data_path>  Path to jak-project data directory
 */

#include "jak_ipc_shm.h"
#include "jak_bridge.h"

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static std::atomic<bool> s_shutdown{false};
static std::atomic<bool> s_runtime_thread_exited{false};

/* -------------------------------------------------------------------------- */
/*  Runtime thread                                                            */
/* -------------------------------------------------------------------------- */

static void runtime_thread_func(const std::string& game_data_path) {
  fprintf(stderr, "[jak_server] Runtime thread starting...\n");
  int status = jak_bridge::launch_runtime_headless(game_data_path);
  fprintf(stderr, "[jak_server] Runtime exited with status %d\n", status);
  s_runtime_thread_exited.store(true);
}

/* -------------------------------------------------------------------------- */
/*  Command type enum (matches thin DLL)                                      */
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
/*  Process IPC commands → jak_bridge command queue                           */
/* -------------------------------------------------------------------------- */

static void process_ipc_commands(JakIpcHandle* h) {
  JakIpcCommand cmd;
  while (jak_ipc_pop_command(h, &cmd)) {
    auto& cq = jak_bridge::get_command_queue();

    switch ((IpcCmdType)cmd.type) {
      case IPC_CMD_CREATE: {
        jak_bridge::Command c;
        c.type = jak_bridge::CommandType::SPAWN;
        c.f[0] = cmd.f[0];  // x
        c.f[1] = cmd.f[1];  // y
        c.f[2] = cmd.f[2];  // z
        c.jak_id = 0;
        std::lock_guard<std::mutex> lock(cq.mutex);
        cq.pending.push_back(c);
        h->shm->jak_id = 0;  // Only one Jak instance supported
        break;
      }
      case IPC_CMD_DELETE: {
        jak_bridge::Command c;
        c.type = jak_bridge::CommandType::DESTROY;
        c.jak_id = cmd.jak_id;
        std::lock_guard<std::mutex> lock(cq.mutex);
        cq.pending.push_back(c);
        h->shm->jak_id = -1;
        break;
      }
      case IPC_CMD_SET_POSITION: {
        jak_bridge::Command c;
        c.type = jak_bridge::CommandType::SET_POSITION;
        c.jak_id = cmd.jak_id;
        c.f[0] = cmd.f[0];
        c.f[1] = cmd.f[1];
        c.f[2] = cmd.f[2];
        std::lock_guard<std::mutex> lock(cq.mutex);
        cq.pending.push_back(c);
        break;
      }
      case IPC_CMD_SET_VELOCITY: {
        jak_bridge::Command c;
        c.type = jak_bridge::CommandType::SET_VELOCITY;
        c.jak_id = cmd.jak_id;
        c.f[0] = cmd.f[0];
        c.f[1] = cmd.f[1];
        c.f[2] = cmd.f[2];
        std::lock_guard<std::mutex> lock(cq.mutex);
        cq.pending.push_back(c);
        break;
      }
      case IPC_CMD_SET_ANGLE: {
        jak_bridge::Command c;
        c.type = jak_bridge::CommandType::SET_ANGLE;
        c.jak_id = cmd.jak_id;
        c.f[0] = cmd.f[0];
        std::lock_guard<std::mutex> lock(cq.mutex);
        cq.pending.push_back(c);
        break;
      }
      case IPC_CMD_SET_ACTION: {
        jak_bridge::Command c;
        c.type = jak_bridge::CommandType::SET_ACTION;
        c.jak_id = cmd.jak_id;
        c.i[0] = cmd.i[0];
        std::lock_guard<std::mutex> lock(cq.mutex);
        cq.pending.push_back(c);
        break;
      }
      case IPC_CMD_SET_HEALTH: {
        jak_bridge::Command c;
        c.type = jak_bridge::CommandType::SET_HEALTH;
        c.jak_id = cmd.jak_id;
        c.f[0] = cmd.f[0];
        std::lock_guard<std::mutex> lock(cq.mutex);
        cq.pending.push_back(c);
        break;
      }
      case IPC_CMD_TAKE_DAMAGE: {
        jak_bridge::Command c;
        c.type = jak_bridge::CommandType::TAKE_DAMAGE;
        c.jak_id = cmd.jak_id;
        c.f[0] = cmd.f[0];
        c.f[1] = cmd.f[1];
        c.f[2] = cmd.f[2];
        std::lock_guard<std::mutex> lock(cq.mutex);
        cq.pending.push_back(c);
        break;
      }
      case IPC_CMD_HEAL: {
        jak_bridge::Command c;
        c.type = jak_bridge::CommandType::HEAL;
        c.jak_id = cmd.jak_id;
        c.f[0] = cmd.f[0];
        std::lock_guard<std::mutex> lock(cq.mutex);
        cq.pending.push_back(c);
        break;
      }
      case IPC_CMD_KILL: {
        jak_bridge::Command c;
        c.type = jak_bridge::CommandType::KILL;
        c.jak_id = cmd.jak_id;
        std::lock_guard<std::mutex> lock(cq.mutex);
        cq.pending.push_back(c);
        break;
      }
      case IPC_CMD_GIVE_ECO: {
        jak_bridge::Command c;
        c.type = jak_bridge::CommandType::GIVE_ECO;
        c.jak_id = cmd.jak_id;
        c.i[0] = cmd.i[0];
        std::lock_guard<std::mutex> lock(cq.mutex);
        cq.pending.push_back(c);
        break;
      }
      case IPC_CMD_LOAD_SURFACES: {
        // Surfaces are in the surface_data area of shared memory
        auto& cs = jak_bridge::get_collision_state();
        std::lock_guard<std::mutex> lock(cs.mutex);
        cs.static_surfaces.clear();
        uint32_t count = h->shm->surface_upload_count;
        if (count > JAK_IPC_MAX_SURFACES) count = JAK_IPC_MAX_SURFACES;
        for (uint32_t i = 0; i < count; i++) {
          const JakSurface& s = h->shm->surface_data[i];
          jak_bridge::ExternalSurface es;
          for (int v = 0; v < 3; v++) {
            es.vertices[v][0] = s.vertices[v][0];
            es.vertices[v][1] = s.vertices[v][1];
            es.vertices[v][2] = s.vertices[v][2];
          }
          es.normal[0] = s.normal[0];
          es.normal[1] = s.normal[1];
          es.normal[2] = s.normal[2];
          es.type = s.type;
          es.flags = s.flags;
          cs.static_surfaces.push_back(es);
        }
        h->shm->surface_upload_ready = 0;
        break;
      }
      case IPC_CMD_CLEAR_SURFACES: {
        auto& cs = jak_bridge::get_collision_state();
        std::lock_guard<std::mutex> lock(cs.mutex);
        cs.static_surfaces.clear();
        break;
      }
      default:
        break;
    }
  }
}

/* -------------------------------------------------------------------------- */
/*  Copy bridge state → shared memory                                        */
/* -------------------------------------------------------------------------- */

static void copy_state_to_shm(JakIpcHandle* h) {
  auto& js = jak_bridge::get_jak_state();
  if (js.valid) {
    memcpy(&h->shm->state, &js.state, sizeof(JakState));
    h->shm->state_valid = 1;
  } else {
    h->shm->state_valid = 0;
  }
}

static void copy_mesh_to_shm(JakIpcHandle* h) {
  auto& ms = jak_bridge::get_mesh_state();
  if (!ms.valid || ms.num_triangles == 0) return;

  uint32_t active = h->shm->active_buffer_index;
  uint32_t write_buf = 1 - active;  // Write to inactive buffer

  JakIpcGeoBuf* geo = &h->shm->geo_buffers[write_buf];
  uint32_t ntris = ms.num_triangles;
  if (ntris > JAK_GEO_MAX_TRIANGLES) ntris = JAK_GEO_MAX_TRIANGLES;

  uint32_t nverts = ntris * 3;
  memcpy((void*)geo->position, ms.positions.data(), nverts * 3 * sizeof(float));
  memcpy((void*)geo->normal, ms.normals.data(), nverts * 3 * sizeof(float));
  memcpy((void*)geo->color, ms.colors.data(), nverts * 4 * sizeof(float));
  memcpy((void*)geo->uv, ms.uvs.data(), nverts * 2 * sizeof(float));
  geo->num_triangles = ntris;

#ifdef _WIN32
  MemoryBarrier();
#else
  __sync_synchronize();
#endif

  geo->valid = 1;

  // Flip active buffer
  h->shm->active_buffer_index = write_buf;
}

static void copy_texture_to_shm(JakIpcHandle* h) {
  auto& tai = jak_bridge::get_texture_atlas_info();
  if (!tai.valid) return;
  if (h->shm->tex_data_ready) return;  // Already copied

  uint8_t* tex_ptr = jak_bridge::get_texture_output();
  if (!tex_ptr) return;

  h->shm->tex_width = tai.atlas_width;
  h->shm->tex_height = tai.atlas_height;
  h->shm->tex_num_textures = tai.num_textures;
  memcpy(h->shm->tex_data, tex_ptr, tai.atlas_width * tai.atlas_height * 4);

#ifdef _WIN32
  MemoryBarrier();
#else
  __sync_synchronize();
#endif

  h->shm->tex_data_ready = 1;
}

/* -------------------------------------------------------------------------- */
/*  Copy shared memory inputs → bridge                                       */
/* -------------------------------------------------------------------------- */

static void copy_inputs_from_shm(JakIpcHandle* h) {
  if (!h->shm->input_valid) return;

  auto& ps = jak_bridge::get_pad_state();
  std::lock_guard<std::mutex> lock(ps.mutex);
  memcpy(&ps.current_inputs, &h->shm->inputs, sizeof(JakInputs));
  ps.has_input = true;
}

/* -------------------------------------------------------------------------- */
/*  Main server loop                                                          */
/* -------------------------------------------------------------------------- */

int main(int argc, char* argv[]) {
  if (argc < 3) {
    fprintf(stderr, "Usage: jak_server <client_pid> <game_data_path>\n");
    return 1;
  }

  uint32_t client_pid = (uint32_t)atoi(argv[1]);
  std::string game_data_path = argv[2];

  fprintf(stderr, "[jak_server] Starting for client PID %u, data=%s\n",
          client_pid, game_data_path.c_str());

  /* Create shared memory */
  JakIpcHandle ipc = jak_ipc_create(client_pid);
  if (!ipc.shm) {
    fprintf(stderr, "[jak_server] Failed to create shared memory!\n");
    return 1;
  }

  ipc.shm->server_state = JAK_IPC_STATE_BOOTING;

  /* Set up texture output buffer pointing into shared memory */
  jak_bridge::set_texture_output(ipc.shm->tex_data);

  /* Launch GOAL runtime on background thread */
  std::thread runtime_thread(runtime_thread_func, game_data_path);
  runtime_thread.detach();

  /* Wait for runtime to be ready (poll jak_bridge::is_runtime_ready) */
  fprintf(stderr, "[jak_server] Waiting for runtime to boot...\n");
  while (!jak_bridge::is_runtime_ready()) {
    if (s_runtime_thread_exited.load()) {
      fprintf(stderr, "[jak_server] Runtime thread exited before ready!\n");
      ipc.shm->server_state = JAK_IPC_STATE_ERROR;
      ipc.shm->error_code = 1;
      jak_ipc_close(&ipc);
      return 1;
    }
#ifdef _WIN32
    // Also check shutdown event while waiting
    DWORD wait = WaitForSingleObject(ipc.hEvtShutdown, 100);
    if (wait == WAIT_OBJECT_0) {
      fprintf(stderr, "[jak_server] Shutdown requested during boot\n");
      jak_bridge::set_runtime_ready(false);  // force exit
      jak_ipc_close(&ipc);
      return 0;
    }
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
#endif
  }

  fprintf(stderr, "[jak_server] Runtime ready! Entering server loop.\n");
  ipc.shm->server_state = JAK_IPC_STATE_READY;

#ifdef _WIN32
  /* Main server loop: wait for frame request or shutdown */
  HANDLE wait_handles[2] = {ipc.hEvtFrameReq, ipc.hEvtShutdown};

  while (!s_shutdown.load()) {
    DWORD result = WaitForMultipleObjects(2, wait_handles, 0, 1000);

    if (result == WAIT_OBJECT_0) {
      // Frame request from client
      ipc.shm->server_sequence++;

      // 1. Read inputs from shared memory
      copy_inputs_from_shm(&ipc);

      // 2. Process queued IPC commands
      process_ipc_commands(&ipc);

      // 3. Run one tick of the bridge (drives GOAL)
      jak_bridge::bridge_tick();

      // 4. Copy state + geometry to shared memory
      copy_state_to_shm(&ipc);
      copy_mesh_to_shm(&ipc);
      copy_texture_to_shm(&ipc);

      // 5. Signal frame done
      SetEvent(ipc.hEvtFrameDone);

    } else if (result == WAIT_OBJECT_0 + 1) {
      // Shutdown event
      fprintf(stderr, "[jak_server] Shutdown event received\n");
      s_shutdown.store(true);

    } else if (result == WAIT_TIMEOUT) {
      // Timeout — check if runtime died
      if (s_runtime_thread_exited.load()) {
        fprintf(stderr, "[jak_server] Runtime thread exited unexpectedly\n");
        s_shutdown.store(true);
      }
    } else {
      fprintf(stderr, "[jak_server] WaitForMultipleObjects error: %lu\n", GetLastError());
      s_shutdown.store(true);
    }
  }
#else
  // Linux: simple poll loop (future: use semaphores)
  while (!s_shutdown.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    copy_inputs_from_shm(&ipc);
    process_ipc_commands(&ipc);
    jak_bridge::bridge_tick();
    copy_state_to_shm(&ipc);
    copy_mesh_to_shm(&ipc);
    copy_texture_to_shm(&ipc);
  }
#endif

  fprintf(stderr, "[jak_server] Shutting down...\n");
  ipc.shm->server_state = JAK_IPC_STATE_EXITING;

  // Signal runtime to exit
  jak_bridge::set_runtime_ready(false);

  // Give runtime a moment to wind down
  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  jak_ipc_close(&ipc);
  fprintf(stderr, "[jak_server] Clean exit.\n");
  return 0;
}
