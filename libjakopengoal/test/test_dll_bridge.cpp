// Test: just reference jak_bridge symbols (same as jakopengoal)
#ifdef _WIN32
#include <windows.h>
BOOL WINAPI DllMain(HINSTANCE h, DWORD r, LPVOID p) { return TRUE; }
#endif

#include "libjakopengoal/src/jak_bridge.h"

extern "C" __declspec(dllexport) int test_ping() { return 42; }
extern "C" __declspec(dllexport) void* get_coll() {
    return &jak_bridge::get_collision_state();
}
extern "C" __declspec(dllexport) bool get_ready() {
    return jak_bridge::is_runtime_ready();
}
