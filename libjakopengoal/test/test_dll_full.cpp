// Test DLL that references the same runtime symbols as jakopengoal.dll
// to isolate which linked object causes DllMain failure in Blender.

#ifdef _WIN32
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { return TRUE; }
#endif

#include <thread>
#include <atomic>
#include <string>
#include <mutex>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/versions/versions.h"
#include "game/common/game_common_types.h"
#include "game/kernel/common/kboot.h"
#include "game/runtime.h"

// Same static objects as libjakopengoal.cpp
static std::atomic<bool> s_test_init{false};
static std::thread s_test_thread;
static std::string s_test_path;

extern "C" __declspec(dllexport) int test_ping() { return 42; }

// Reference exec_runtime to force linking the same objects
extern "C" __declspec(dllexport) void* get_exec_runtime_ptr() {
    return (void*)&exec_runtime;
}

// Reference MasterExit
extern "C" __declspec(dllexport) int get_master_exit() {
    return (int)MasterExit;
}
